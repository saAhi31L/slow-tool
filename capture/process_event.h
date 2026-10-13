#pragma once

#include <cstdint>

// ProcessEvent is the second type (after Sample) that crosses the C++ -> Zig
// boundary. It represents one discrete occurrence - a process starting or
// exiting - rather than a periodic measurement, which is why it lives in its
// own file instead of being folded into Sample.
extern "C" {

struct ProcessEvent {
    uint64_t timestamp;        // when this happened, Unix epoch seconds
    uint32_t pid;               // which process
    wchar_t process_name[32];   // executable name, truncated/zero-padded to fit
    bool is_start;               // true = process started, false = process exited
};

// Implemented in Zig (correlate/log.zig). Logs one process start/exit event
// directly to slowtool_process_events.csv - no correlation step involved.
void process_event_log(const ProcessEvent* event);

}

// Field order is alignment-first (8 -> 4 -> 2 -> 1 bytes), same rule used for
// Sample, so the struct needs no internal padding - only 3 bytes of trailing
// padding to round the total up to a multiple of 8 (timestamp's alignment).
// 8 (timestamp) + 4 (pid) + 64 (process_name) + 1 (is_start) = 77 raw bytes,
// rounded up to 80.
static_assert(sizeof(ProcessEvent) == 80, "ProcessEvent layout drifted from process_event.zig - fix both sides together");
