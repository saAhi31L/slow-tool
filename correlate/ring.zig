const Sample = @import("sample.zig").Sample;

pub const RING_SIZE: usize = 60;

var buffer: [RING_SIZE]Sample = undefined;
var head: usize = 0;
var count: usize = 0;

pub fn push(sample: *const Sample) void {
    buffer[head] = sample.*;
    head = (head + 1) % RING_SIZE;
    if (count < RING_SIZE) {
        count += 1;
    }
}

pub fn sample_count() usize {
    return count;
}

pub const Baseline = struct {
    cpu_percent: f64,
    available_memory_mb: f64,
    disk_percent: f64,
    network_bytes_per_sec: f64,
};

pub fn average() Baseline {
    var sum = Baseline{
        .cpu_percent = 0,
        .available_memory_mb = 0,
        .disk_percent = 0,
        .network_bytes_per_sec = 0,
    };

    if (count == 0) {
        return sum;
    }

    var i: usize = 0;
    while (i < count) : (i += 1) {
        sum.cpu_percent += buffer[i].cpu_percent;
        sum.available_memory_mb += buffer[i].available_memory_mb;
        sum.disk_percent += buffer[i].disk_percent;
        sum.network_bytes_per_sec += buffer[i].network_bytes_per_sec;
    }

    const n: f64 = @floatFromInt(count);
    sum.cpu_percent /= n;
    sum.available_memory_mb /= n;
    sum.disk_percent /= n;
    sum.network_bytes_per_sec /= n;
    return sum;
}
