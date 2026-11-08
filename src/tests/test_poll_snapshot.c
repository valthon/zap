/* Exercise production poll review after the kernel has copied an event batch.
 * Interpose only the wait syscall to force a real descriptor-reuse interleaving. */
#define FIO_MAX_SOCK_CAPACITY 1024
#ifndef FIO_ENGINE_POLL
#define FIO_ENGINE_POLL 0
#endif
#if FIO_ENGINE_POLL
#define poll snapshot_poll
#elif defined(__linux__)
#define epoll_wait snapshot_epoll_wait
#else
#define kevent snapshot_kevent
#endif
#include "../../facil.io/lib/facil/fio.c"
#undef poll
#undef epoll_wait
#undef kevent
#include <assert.h>
#include <fcntl.h>

enum scenario { RESET, DOCUMENT, READ_EVENT, WRITE_EVENT, EXTERNAL_FD };
static enum scenario scenario;
static intptr_t retired_uuid, replacement_uuid;
static int retired_fd, replacement[2], inject;
static void replace_after_snapshot(void);

#if FIO_ENGINE_EPOLL
extern int epoll_wait(int, struct epoll_event *, int, int);
int snapshot_epoll_wait(int epfd, struct epoll_event *events, int maximum, int timeout) {
  int count = epoll_wait(epfd, events, maximum, timeout);
  if (inject && count > 0 && epfd == evio_fd[scenario == WRITE_EVENT ? 2 : 1])
    replace_after_snapshot();
  return count;
}
#elif FIO_ENGINE_KQUEUE
extern int kevent(int, const struct snapshot_kevent *, int,
                  struct snapshot_kevent *, int, const struct timespec *);
int snapshot_kevent(int queue, const struct snapshot_kevent *changes, int count,
                    struct snapshot_kevent *events, int maximum,
                    const struct timespec *timeout) {
  int ready = kevent(queue, changes, count, events, maximum, timeout);
  if (inject && ready > 0 && maximum > 0)
    replace_after_snapshot();
  return ready;
}
#else
extern int poll(struct pollfd *, nfds_t, int);
int snapshot_poll(struct pollfd *events, nfds_t count, int timeout) {
  int ready = poll(events, count, timeout);
  if (inject && ready > 0)
    replace_after_snapshot();
  return ready;
}
#endif

static const char document[] = "<!doctype html><input data-test=email>";
static void replace_after_snapshot(void) {
  inject = 0; /* close/registration may themselves call the wait API */
  fio_force_close(retired_uuid);
  assert(socketpair(AF_UNIX, SOCK_STREAM, 0, replacement) == 0);
  assert(replacement[0] == retired_fd);
  replacement_uuid = scenario == EXTERNAL_FD ? -1 : fio_fd2uuid(replacement[0]);
  if (replacement_uuid >= 0) {
    assert(replacement_uuid != retired_uuid);
    fio_poll_add_read(replacement[0]);
  }
  const char request[] = "POST /api/collections/members/auth/two-factor/enroll-complete HTTP/1.1\r\nHost: localhost\r\nContent-Length: 2\r\n\r\n{}";
  assert(write(replacement[1], request, sizeof(request) - 1) == sizeof(request) - 1);
  if (scenario == DOCUMENT) {
    char buffer[sizeof(request)];
    size_t used = 0;
    while (used < sizeof(request) - 1) {
      ssize_t count = read(replacement[0], buffer + used, sizeof(request) - 1 - used);
      assert(count > 0);
      used += (size_t)count;
    }
    int length = snprintf(buffer, sizeof(buffer),
        "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: %zu\r\n\r\n", sizeof(document) - 1);
    assert(length > 0 && (size_t)length < sizeof(buffer));
    assert(write(replacement[0], buffer, (size_t)length) == length);
  }
}

static int run_scenario(enum scenario kind) {
  scenario = kind;
  fio_defer_clear_queue();
  int retired[2];
  assert(socketpair(AF_UNIX, SOCK_STREAM, 0, retired) == 0);
  retired_fd = retired[0];
  retired_uuid = fio_fd2uuid(retired_fd);
  if (kind == WRITE_EVENT) {
    fio_poll_add_write(retired_fd);
  } else {
    fio_poll_add_read(retired_fd);
    if (kind == READ_EVENT)
      assert(write(retired[1], "x", 1) == 1);
    else
      assert(shutdown(retired[1], SHUT_RDWR) == 0);
  }
  inject = 1;
  fio_poll();
  assert(!inject);
  int failed = fcntl(replacement[0], F_GETFD) < 0;
  if (replacement_uuid >= 0)
    failed |= fio_is_closed(replacement_uuid);
  failed |= fio_defer_has_queue(); /* stale data/ready callbacks must not target the new connection */
  if (failed) {
    char buffer[256];
    errno = 0;
    ssize_t count = recv(replacement[1], buffer, sizeof(buffer) - 1, MSG_DONTWAIT);
    if (count > 0) buffer[count] = 0;
    fprintf(stderr, "scenario=%d old_uuid=%ld new_uuid=%ld: closed or stale callback; client=%ld errno=%d%s%s\n",
            kind, (long)retired_uuid, (long)replacement_uuid, (long)count, errno,
            count > 0 ? " response=" : "", count > 0 ? buffer : "");
  } else if (kind == DOCUMENT) {
    assert(write(replacement[0], document, sizeof(document) - 1) == sizeof(document) - 1);
    char buffer[512];
    ssize_t count = recv(replacement[1], buffer, sizeof(buffer) - 1, 0);
    assert(count > 0); buffer[count] = 0;
    assert(strstr(buffer, document) != NULL);
  }
  fio_defer_clear_queue();
  if (fcntl(replacement[0], F_GETFD) >= 0) {
    if (replacement_uuid >= 0) fio_force_close(replacement_uuid);
    else close(replacement[0]);
  }
  close(replacement[1]); close(retired[1]);
  return failed;
}

static void valid_event_controls(void) {
  int sockets[2];
  assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
  intptr_t uuid = fio_fd2uuid(sockets[0]);
  fio_defer_clear_queue();
  fio_poll_add_read(sockets[0]);
  assert(write(sockets[1], "x", 1) == 1);
  fio_poll();
  assert(!fio_is_closed(uuid) && fio_defer_has_queue());
  fio_defer_clear_queue();
  assert(shutdown(sockets[1], SHUT_RDWR) == 0);
  fio_poll_add_read(sockets[0]);
  fio_poll();
  assert(fio_is_closed(uuid));
  fio_defer_clear_queue();
  close(sockets[1]);
}

enum queued_kind { QUEUED_READY, QUEUED_READY_USER, QUEUED_DATA,
                   QUEUED_SHUTDOWN, QUEUED_PING, QUEUED_IO };
static int queued_callbacks, queued_fallbacks;
static void queued_callback(intptr_t uuid, fio_protocol_s *protocol) {
  (void)uuid; (void)protocol;
  ++queued_callbacks;
}
static uint8_t queued_shutdown(intptr_t uuid, fio_protocol_s *protocol) {
  queued_callback(uuid, protocol);
  return 255;
}
static void queued_io(intptr_t uuid, fio_protocol_s *protocol, void *udata) {
  (void)udata;
  queued_callback(uuid, protocol);
}
static void queued_fallback(intptr_t uuid, void *udata) {
  (void)uuid; (void)udata;
  ++queued_fallbacks;
}
static fio_protocol_s queued_protocol(void) {
  return (fio_protocol_s){ .on_data = queued_callback, .on_ready = queued_callback,
                          .on_shutdown = queued_shutdown, .ping = queued_callback };
}
static void queue_captured_event(enum queued_kind kind, intptr_t uuid) {
  switch (kind) {
  case QUEUED_READY: fio_force_event(uuid, FIO_EVENT_ON_READY); break;
  case QUEUED_READY_USER:
    fio_defer_push_task(deferred_on_ready_usr, (void *)uuid, NULL); break;
  case QUEUED_DATA: fio_force_event(uuid, FIO_EVENT_ON_DATA); break;
  case QUEUED_SHUTDOWN:
    fio_defer_push_task(deferred_on_shutdown, (void *)uuid, NULL); break;
  case QUEUED_PING: fio_force_event(uuid, FIO_EVENT_ON_TIMEOUT); break;
  case QUEUED_IO:
    fio_defer_io_task(uuid, .task = queued_io, .fallback = queued_fallback); break;
  }
}

/* A valid event can pass poll review, then wait on the task queue while its fd
 * is recycled. Draining must not look up the replacement protocol by raw fd. */
static int queued_after_reuse(enum queued_kind kind) {
  int retired[2], next[2];
  assert(socketpair(AF_UNIX, SOCK_STREAM, 0, retired) == 0);
  intptr_t retired_id = fio_fd2uuid(retired[0]);
  fio_protocol_s previous = queued_protocol(), replacement = queued_protocol();
  fio_attach(retired_id, &previous);
  fio_defer_clear_queue();
  queue_captured_event(kind, retired_id);
  fio_force_close(retired_id);
  assert(socketpair(AF_UNIX, SOCK_STREAM, 0, next) == 0);
  assert(next[0] == retired[0]);
  intptr_t next_id = fio_fd2uuid(next[0]);
  assert(next_id != retired_id);
  fio_attach(next_id, &replacement);
  queued_callbacks = queued_fallbacks = 0;
  fio_defer_perform();
  int failed = queued_callbacks != 0 ||
      queued_fallbacks != (kind == QUEUED_IO) || fio_is_closed(next_id);
  if (failed)
    fprintf(stderr, "queued=%d old=%ld replacement=%ld callbacks=%d fallbacks=%d\n",
            kind, (long)retired_id, (long)next_id, queued_callbacks, queued_fallbacks);
  fio_force_close(next_id);
  fio_defer_perform();
  close(next[1]); close(retired[1]);
  return failed;
}

static void valid_queued_controls(void) {
  for (int kind = QUEUED_READY; kind <= QUEUED_IO; ++kind) {
    int sockets[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    intptr_t uuid = fio_fd2uuid(sockets[0]);
    fio_protocol_s protocol = queued_protocol();
    fio_attach(uuid, &protocol);
    fio_defer_clear_queue();
    queued_callbacks = queued_fallbacks = 0;
    queue_captured_event((enum queued_kind)kind, uuid);
    fio_defer_perform();
    assert(queued_callbacks == 1 && queued_fallbacks == 0);
    assert(!fio_is_closed(uuid));
    fio_force_close(uuid);
    fio_defer_perform();
    close(sockets[1]);
  }
}

int main(void) {
  int failed = 0;
  for (int kind = RESET; kind <= EXTERNAL_FD; ++kind)
    failed |= run_scenario((enum scenario)kind);
  valid_event_controls();
  for (int kind = QUEUED_READY; kind <= QUEUED_IO; ++kind)
    failed |= queued_after_reuse((enum queued_kind)kind);
  valid_queued_controls();
  fprintf(stderr, "%s poll snapshot regression: %s\n", fio_engine(), failed ? "FAIL" : "PASS");
  return failed;
}
