#pragma once

#include <cstdint>

extern "C" {

struct Sample {
    uint64_t timestamp;
    double cpu_percent;
    double available_memory_mb;
    double disk_percent;
    double network_bytes_per_sec;
    double top_cpu_percent;
    uint32_t top_pid;
    wchar_t top_process_name[32];
};

void ring_push(const Sample* sample);

}

static_assert(sizeof(Sample) == 120, "Sample layout drifted from sample.zig - fix both sides together");
