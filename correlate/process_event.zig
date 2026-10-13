// Zig-side mirror of capture/process_event.h's ProcessEvent struct.
// Field order and types must match exactly - see process_event.h for the
// byte-by-byte layout reasoning (alignment-first ordering, 80 bytes total).
pub const ProcessEvent = extern struct {
    timestamp: u64,
    pid: u32,
    process_name: [32]u16,
    is_start: bool,
};

// Build-time safety net: if this struct ever drifts out of sync with
// process_event.h, fail the build immediately instead of silently
// corrupting data at runtime (same reasoning as sample.zig's check).
comptime {
    if (@sizeOf(ProcessEvent) != 80) {
        @compileError("ProcessEvent size mismatch - must match process_event.h exactly");
    }
}
