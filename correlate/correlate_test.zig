const std = @import("std");
const Sample = @import("sample.zig").Sample;
const ring = @import("ring.zig");
const correlate = @import("correlate.zig");

fn make_sample(cpu: f64, mem: f64, disk: f64, net: f64) Sample {
    return Sample{
        .timestamp = 0,
        .cpu_percent = cpu,
        .available_memory_mb = mem,
        .disk_percent = disk,
        .network_bytes_per_sec = net,
        .top_cpu_percent = 0,
        .top_pid = 0,
        .top_process_name = [_]u16{0} ** 32,
    };
}

fn make_named_sample(cpu: f64, mem: f64, disk: f64, net: f64, pid: u32, top_cpu: f64, name: []const u8) Sample {
    var name_buf = [_]u16{0} ** 32;
    var i: usize = 0;
    while (i < name.len and i < 32) : (i += 1) {
        name_buf[i] = name[i];
    }
    return Sample{
        .timestamp = 1758700000,
        .cpu_percent = cpu,
        .available_memory_mb = mem,
        .disk_percent = disk,
        .network_bytes_per_sec = net,
        .top_cpu_percent = top_cpu,
        .top_pid = pid,
        .top_process_name = name_buf,
    };
}

fn run_case(label: []const u8, sample: Sample) void {
    const baseline = ring.average();
    ring.push(&sample);
    const result = correlate.check(&sample, baseline);
    std.debug.print("{s}: cpu={} disk={} network={} memory={}\n", .{
        label, result.cpu, result.disk, result.network, result.memory,
    });
}

pub fn main() void {
    var tick: usize = 1;
    while (tick <= ring.RING_SIZE) : (tick += 1) {
        const sample = make_sample(10.0, 8000.0, 5.0, 1000.0);
        ring.push(&sample);
    }
    std.debug.print("warmed up: {d}/{d} samples in ring\n\n", .{ ring.sample_count(), ring.RING_SIZE });

    run_case("normal tick        ", make_sample(11.0, 7900.0, 5.5, 1050.0));
    run_case("cpu spike          ", make_sample(50.0, 8000.0, 5.0, 1000.0));
    run_case("memory drop        ", make_sample(10.0, 3000.0, 5.0, 1000.0));
    run_case("disk spike         ", make_sample(10.0, 8000.0, 20.0, 1000.0));
    run_case("network spike      ", make_sample(10.0, 8000.0, 5.0, 5000.0));

    std.debug.print("\n--- writing a real log entry via ring_push ---\n\n", .{});
    const spike = make_named_sample(80.0, 8000.0, 5.0, 1000.0, 8408, 62.3, "Code.exe");
    correlate.ring_push(&spike);
    std.debug.print("check slowtool_log.csv in this folder for a new row\n", .{});
}
