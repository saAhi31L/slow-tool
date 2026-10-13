// DiskEvent mirrors capture/disk_event.h exactly - field order, types, and
// size must match on both sides of the C++ -> Zig boundary, same rule as
// Sample and ProcessEvent.
pub const DiskEvent = extern struct {
    timestamp: u64,
    byte_offset: u64,
    transfer_size: u32,
    disk_number: u32,
    direction: u8, // 0 = read, 1 = write, 2 = other
};

comptime {
    if (@sizeOf(DiskEvent) != 32) {
        @compileError("DiskEvent layout drifted from capture/disk_event.h - fix both sides together");
    }
}
