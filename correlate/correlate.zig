const Sample = @import("sample.zig").Sample;
const ring = @import("ring.zig");
const log = @import("log.zig");

const SPIKE_MULTIPLIER: f64 = 1.5;

pub const SpikeResult = struct {
    cpu: bool,
    disk: bool,
    network: bool,
    memory: bool,

    pub fn any(self: SpikeResult) bool {
        return self.cpu or self.disk or self.network or self.memory;
    }
};

pub fn check(sample: *const Sample, baseline: ring.Baseline) SpikeResult {
    return SpikeResult{
        .cpu = sample.cpu_percent > baseline.cpu_percent * SPIKE_MULTIPLIER,
        .disk = sample.disk_percent > baseline.disk_percent * SPIKE_MULTIPLIER,
        .network = sample.network_bytes_per_sec > baseline.network_bytes_per_sec * SPIKE_MULTIPLIER,
        .memory = sample.available_memory_mb < baseline.available_memory_mb / SPIKE_MULTIPLIER,
    };
}

pub export fn ring_push(sample: *const Sample) callconv(.c) void {
    const baseline = ring.average();
    const warmed_up = ring.sample_count() == ring.RING_SIZE;

    ring.push(sample);

    if (!warmed_up) {
        return;
    }

    const result = check(sample, baseline);

    if (result.cpu) {
        log.write(sample.timestamp, "cpu", sample.cpu_percent, baseline.cpu_percent, "%", sample);
    }
    if (result.disk) {
        log.write(sample.timestamp, "disk", sample.disk_percent, baseline.disk_percent, "%", sample);
    }
    if (result.network) {
        log.write(sample.timestamp, "network", sample.network_bytes_per_sec, baseline.network_bytes_per_sec, "B/s", sample);
    }
    if (result.memory) {
        log.write(sample.timestamp, "memory", sample.available_memory_mb, baseline.available_memory_mb, "MB", sample);
    }
}
