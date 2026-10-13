pub const Sample = extern struct {
    timestamp: u64,
    cpu_percent: f64,
    available_memory_mb: f64,
    disk_percent: f64,
    network_bytes_per_sec: f64,
    top_cpu_percent: f64,
    top_pid: u32,
    top_process_name: [32]u16,
};

comptime {
    if (@sizeOf(Sample) != 120) {
        @compileError("Sample size mismatch - must match sample.h exactly");
    }
}
