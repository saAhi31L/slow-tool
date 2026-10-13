const std = @import("std");
const Sample = @import("sample.zig").Sample;
const ProcessEvent = @import("process_event.zig").ProcessEvent;
const DiskEvent = @import("disk_event.zig").DiskEvent;

// Spike log (one row per metric that crossed its threshold).
const LOG_PATH_W = std.unicode.utf8ToUtf16LeStringLiteral("slowtool_log.csv");
const CSV_HEADER = "timestamp,metric,current_value,baseline_value,top_pid,top_process,top_process_cpu_percent\n";

// Process start/exit log - kept in a separate file on purpose (see project
// notes): different shape of data, and mixing it into the spike log would
// make both harder to read and easier to misinterpret.
const PROCESS_LOG_PATH_W = std.unicode.utf8ToUtf16LeStringLiteral("slowtool_process_events.csv");
const PROCESS_CSV_HEADER = "timestamp,event,pid,process_name\n";

// Disk I/O event log - third output file, same reasoning as the process
// event log: a disk event is a discrete occurrence with its own row shape,
// not a periodic measurement with a baseline, so it stays separate.
const DISK_LOG_PATH_W = std.unicode.utf8ToUtf16LeStringLiteral("slowtool_disk_events.csv");
const DISK_CSV_HEADER = "timestamp,direction,disk_number,byte_offset,transfer_size_bytes\n";

const GENERIC_READ: u32 = 0x80000000;
const GENERIC_WRITE: u32 = 0x40000000;
const FILE_SHARE_READ: u32 = 0x00000001;
const OPEN_ALWAYS: u32 = 4;
const FILE_ATTRIBUTE_NORMAL: u32 = 0x80;
const FILE_END: u32 = 2;
const INVALID_HANDLE: ?*anyopaque = @ptrFromInt(std.math.maxInt(usize));

extern "kernel32" fn CreateFileW(
    lpFileName: [*:0]const u16,
    dwDesiredAccess: u32,
    dwShareMode: u32,
    lpSecurityAttributes: ?*anyopaque,
    dwCreationDisposition: u32,
    dwFlagsAndAttributes: u32,
    hTemplateFile: ?*anyopaque,
) callconv(.c) ?*anyopaque;

extern "kernel32" fn WriteFile(
    hFile: ?*anyopaque,
    lpBuffer: [*]const u8,
    nNumberOfBytesToWrite: u32,
    lpNumberOfBytesWritten: ?*u32,
    lpOverlapped: ?*anyopaque,
) callconv(.c) i32;

extern "kernel32" fn CloseHandle(hObject: ?*anyopaque) callconv(.c) i32;

extern "kernel32" fn SetFilePointerEx(
    hFile: ?*anyopaque,
    liDistanceToMove: i64,
    lpNewFilePointer: ?*i64,
    dwMoveMethod: u32,
) callconv(.c) i32;

extern "kernel32" fn GetFileSizeEx(hFile: ?*anyopaque, lpFileSize: *i64) callconv(.c) i32;

const STD_ERROR_HANDLE: u32 = @bitCast(@as(i32, -12));
extern "kernel32" fn GetStdHandle(nStdHandle: u32) callconv(.c) ?*anyopaque;

fn debug_print(msg: []const u8) void {
    const handle = GetStdHandle(STD_ERROR_HANDLE);
    _ = write_bytes(handle, msg);
}

// Converts a fixed-size UTF-16 code-unit array (how both Sample and
// ProcessEvent store process names) into a plain ASCII string for the CSV.
// Shared by both log functions below instead of duplicating this per type.
fn code_units_to_ascii(code_units: []const u16, buf: []u8) []const u8 {
    var len: usize = 0;
    for (code_units) |code_unit| {
        if (code_unit == 0) break;
        if (len >= buf.len) break;
        buf[len] = @truncate(code_unit);
        len += 1;
    }
    return buf[0..len];
}

fn write_bytes(handle: ?*anyopaque, data: []const u8) bool {
    var written: u32 = 0;
    return WriteFile(handle, data.ptr, @intCast(data.len), &written, null) != 0;
}

pub fn write(timestamp: u64, metric: []const u8, current_value: f64, baseline_value: f64, unit: []const u8, sample: *const Sample) void {
    const handle = CreateFileW(
        LOG_PATH_W,
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ,
        null,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        null,
    );
    if (handle == INVALID_HANDLE) {
        debug_print("log: CreateFileW failed\n");
        return;
    }
    defer _ = CloseHandle(handle);

    var size: i64 = 0;
    if (GetFileSizeEx(handle, &size) == 0) {
        debug_print("log: GetFileSizeEx failed\n");
        return;
    }

    if (SetFilePointerEx(handle, 0, null, FILE_END) == 0) {
        debug_print("log: SetFilePointerEx failed\n");
        return;
    }

    if (size == 0) {
        if (!write_bytes(handle, CSV_HEADER)) {
            debug_print("log: header write failed\n");
            return;
        }
    }

    var name_buf: [32]u8 = undefined;
    const name = code_units_to_ascii(&sample.top_process_name, &name_buf);

    var line_buf: [256]u8 = undefined;
    const line = std.fmt.bufPrint(&line_buf, "{d},{s},{d:.1}{s},{d:.1}{s},{d},{s},{d:.1}\n", .{
        timestamp, metric, current_value, unit, baseline_value, unit, sample.top_pid, name, sample.top_cpu_percent,
    }) catch {
        debug_print("log: bufPrint failed\n");
        return;
    };

    if (!write_bytes(handle, line)) {
        debug_print("log: row write failed\n");
    }
}

// Writes one row to slowtool_process_events.csv for a single process
// start or exit event. Same open/check-size/write-header-if-new/append
// pattern as write() above, just against a different file and shape.
//
// This is a direct C-facing export (unlike ring_push, no orchestrator is
// needed in correlate.zig) because there's no baseline or threshold check
// involved - a process event is just logged the moment it happens.
pub export fn process_event_log(event: *const ProcessEvent) callconv(.c) void {
    const handle = CreateFileW(
        PROCESS_LOG_PATH_W,
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ,
        null,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        null,
    );
    if (handle == INVALID_HANDLE) {
        debug_print("log: process event CreateFileW failed\n");
        return;
    }
    defer _ = CloseHandle(handle);

    var size: i64 = 0;
    if (GetFileSizeEx(handle, &size) == 0) {
        debug_print("log: process event GetFileSizeEx failed\n");
        return;
    }

    if (SetFilePointerEx(handle, 0, null, FILE_END) == 0) {
        debug_print("log: process event SetFilePointerEx failed\n");
        return;
    }

    if (size == 0) {
        if (!write_bytes(handle, PROCESS_CSV_HEADER)) {
            debug_print("log: process event header write failed\n");
            return;
        }
    }

    var name_buf: [32]u8 = undefined;
    const name = code_units_to_ascii(&event.process_name, &name_buf);

    // is_start is a bool internally, but "start"/"exit" reads far better
    // in a spreadsheet than a bare 0/1 would.
    const event_label: []const u8 = if (event.is_start) "start" else "exit";

    var line_buf: [128]u8 = undefined;
    const line = std.fmt.bufPrint(&line_buf, "{d},{s},{d},{s}\n", .{
        event.timestamp, event_label, event.pid, name,
    }) catch {
        debug_print("log: process event bufPrint failed\n");
        return;
    };

    if (!write_bytes(handle, line)) {
        debug_print("log: process event row write failed\n");
    }
}

// Writes one row to slowtool_disk_events.csv for a single disk I/O
// completion. Same open/check-size/write-header-if-new/append pattern as
// the other two log functions above, just a different file and shape -
// no PID/process attribution here (deferred, see disk_event.h), so this is
// disk activity on its own: direction, which physical disk, where, and how
// much.
pub export fn disk_event_log(event: *const DiskEvent) callconv(.c) void {
    const handle = CreateFileW(
        DISK_LOG_PATH_W,
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ,
        null,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        null,
    );
    if (handle == INVALID_HANDLE) {
        debug_print("log: disk event CreateFileW failed\n");
        return;
    }
    defer _ = CloseHandle(handle);

    var size: i64 = 0;
    if (GetFileSizeEx(handle, &size) == 0) {
        debug_print("log: disk event GetFileSizeEx failed\n");
        return;
    }

    if (SetFilePointerEx(handle, 0, null, FILE_END) == 0) {
        debug_print("log: disk event SetFilePointerEx failed\n");
        return;
    }

    if (size == 0) {
        if (!write_bytes(handle, DISK_CSV_HEADER)) {
            debug_print("log: disk event header write failed\n");
            return;
        }
    }

    // direction is 0/1/2 internally (see disk_event.h), but a word reads
    // far better in a spreadsheet than a bare number would - same reasoning
    // as event_label above for process events.
    const direction_label: []const u8 = switch (event.direction) {
        0 => "read",
        1 => "write",
        else => "other",
    };

    var line_buf: [128]u8 = undefined;
    const line = std.fmt.bufPrint(&line_buf, "{d},{s},{d},{d},{d}\n", .{
        event.timestamp, direction_label, event.disk_number, event.byte_offset, event.transfer_size,
    }) catch {
        debug_print("log: disk event bufPrint failed\n");
        return;
    };

    if (!write_bytes(handle, line)) {
        debug_print("log: disk event row write failed\n");
    }
}
