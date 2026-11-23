/*
Copyright: Boaz Segev, 2018-2019
License: MIT
*/
#ifndef H_FIO_TMPFILE_H
/** a simple helper to create temporary files and file names */
#define H_FIO_TMPFILE_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdio.h>
#include <stdlib.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* Creates an anonymous temporary file and returns its descriptor (-1 on
 * error). The name is unlinked as soon as the file exists: every caller keeps
 * only the fd, so the kernel reclaims the storage when the fd is closed.
 * Without the unlink, every request body spilled to disk stayed in the temp
 * directory for good. */
static inline int fio_tmpfile(void) {
  // create a temporary file to contain the data.
  int fd = 0;
#ifdef P_tmpdir
  if (P_tmpdir[sizeof(P_tmpdir) - 1] == '/') {
    char name_template[] = P_tmpdir "facil_io_tmpfile_XXXXXXXX";
    fd = mkstemp(name_template);
    if (fd != -1)
      unlink(name_template);
  } else {
    char name_template[] = P_tmpdir "/facil_io_tmpfile_XXXXXXXX";
    fd = mkstemp(name_template);
    if (fd != -1)
      unlink(name_template);
  }
#else
  char name_template[] = "/tmp/facil_io_tmpfile_XXXXXXXX";
  fd = mkstemp(name_template);
  if (fd != -1)
    unlink(name_template);
#endif
  return fd;
}
#endif
