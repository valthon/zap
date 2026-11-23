//! A request body larger than facil.io's HTTP_MAX_HEADER_LENGTH (8 KiB) is not
//! buffered in memory: facil.io spills it to a temporary file
//! (`fio_tmpfile()`, `facil_io_tmpfile_*` in the temp directory) and hands the
//! handler a body backed by that file's descriptor.
//!
//! This test sends such a body and checks, while the request is being handled,
//! that (a) the body was spilled to a temp file and reads back intact, and (b)
//! that temp file no longer has a name in the file system. A temp file that
//! keeps its name outlives the request: the descriptor is closed afterwards but
//! nothing removes the file, so every large request leaves its body behind.
//!
//! The check reads the server's own descriptors from /proc/self/fd, so it only
//! looks at files this process opened and runs on Linux only.
const std = @import("std");
const builtin = @import("builtin");
const zap = @import("zap");

const PORT = 3050;
const BODY_LEN = 256 * 1024; // well above HTTP_MAX_HEADER_LENGTH
const TMPFILE_PREFIX = "facil_io_tmpfile_";
const DELETED_SUFFIX = " (deleted)";

var test_error: ?anyerror = null;
var spilled_fds: usize = 0;
var named_tmpfile: [std.Io.Dir.max_path_bytes]u8 = undefined;
var named_tmpfile_len: usize = 0;

fn bodyByte(i: usize) u8 {
    return @intCast('a' + i % 26);
}

fn makeRequest(allocator: std.mem.Allocator, url: []const u8) !void {
    defer zap.stop();

    var threaded: std.Io.Threaded = .init(allocator, .{});
    defer threaded.deinit();
    const io = threaded.io();
    var http_client: std.http.Client = .{ .allocator = allocator, .io = io };
    defer http_client.deinit();

    const payload = try allocator.alloc(u8, BODY_LEN);
    defer allocator.free(payload);
    for (payload, 0..) |*c, i| c.* = bodyByte(i);

    _ = try http_client.fetch(.{
        .method = .POST,
        .location = .{ .url = url },
        .headers = .{ .content_type = .{ .override = "application/octet-stream" } },
        .payload = payload,
    });
}

/// Counts this process's descriptors that point at a facil.io temp file, and
/// records the first one whose link target is still a live path (no
/// " (deleted)" suffix).
fn scanDescriptors() void {
    var path_buf: [64]u8 = undefined;
    var link_buf: [std.Io.Dir.max_path_bytes]u8 = undefined;
    var fd: usize = 0;
    while (fd < 1024) : (fd += 1) {
        const path = std.fmt.bufPrintSentinel(&path_buf, "/proc/self/fd/{d}", .{fd}, 0) catch unreachable;
        const rc = std.os.linux.readlink(path.ptr, &link_buf, link_buf.len);
        if (@as(isize, @bitCast(rc)) < 0) continue; // no such descriptor
        const target = link_buf[0..rc];
        if (std.mem.indexOf(u8, target, TMPFILE_PREFIX) == null) continue;
        spilled_fds += 1;
        if (!std.mem.endsWith(u8, target, DELETED_SUFFIX) and named_tmpfile_len == 0) {
            @memcpy(named_tmpfile[0..target.len], target);
            named_tmpfile_len = target.len;
        }
    }
}

fn on_request(r: zap.Request) !void {
    on_request_inner(r) catch |err| {
        test_error = err;
    };
}

fn on_request_inner(r: zap.Request) !void {
    scanDescriptors();

    const body = r.body orelse return error.MissingBody;
    try std.testing.expectEqual(@as(usize, BODY_LEN), body.len);
    for (body, 0..) |c, i| {
        if (c != bodyByte(i)) return error.BodyMismatch;
    }
    try r.sendBody("ok");
}

test "large request body is spilled to an unnamed temp file" {
    if (builtin.os.tag != .linux) return error.SkipZigTest;

    const allocator = std.testing.allocator;

    var listener = zap.HttpListener.init(.{
        .port = PORT,
        .on_request = on_request,
        .log = false,
        .max_clients = 10,
        .max_body_size = 4 * BODY_LEN,
    });
    try listener.listen();

    const t1 = try std.Thread.spawn(.{}, makeRequest, .{ allocator, std.fmt.comptimePrint("http://127.0.0.1:{d}", .{PORT}) });
    defer t1.join();

    zap.start(.{
        .threads = 1,
        .workers = 1,
    });

    if (test_error) |err| return err;

    // The body must actually have gone through a temp file, or the check
    // below proves nothing.
    std.testing.expect(spilled_fds > 0) catch |err| {
        std.debug.print("expected the {d}-byte body to be spilled to a {s}* file; none was open\n", .{ BODY_LEN, TMPFILE_PREFIX });
        return err;
    };
    std.testing.expect(named_tmpfile_len == 0) catch |err| {
        std.debug.print("temp file still has a name and will outlive the request: {s}\n", .{named_tmpfile[0..named_tmpfile_len]});
        return err;
    };
}
