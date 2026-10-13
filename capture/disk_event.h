#pragma once

#include <cstdint>

// DiskEvent is the third type (after Sample and ProcessEvent) that crosses
// the C++ -> Zig boundary. One row = one physical disk I/O completion,
// captured via the Kernel-Disk ETW provider - not polling, same idea as
// ProcessEvent.
extern "C" {

struct DiskEvent {
    uint64_t timestamp;      // when this happened, Unix epoch seconds
    uint64_t byte_offset;    // where on the disk this I/O landed
    uint32_t transfer_size;  // bytes transferred in this operation
    uint32_t disk_number;    // which physical disk (0, 1, ...)

    // 0 = read, 1 = write, 2 = other. Derived from the IRP_READ_OPERATION /
    // IRP_WRITE_OPERATION flag bits (0x100 / 0x200) inside the event's
    // IrpFlags field. Confirmed against real captured data that MOST disk
    // events (cache/paging I/O - e.g. write-behind flushes) carry NEITHER
    // bit, so "other" is an honest bucket, not a bug - see
    // slowtool_disk_events_legend.txt.
    uint8_t direction;
};

// Implemented in Zig (correlate/log.zig). Logs one disk I/O event directly
// to slowtool_disk_events.csv - no correlation step involved, same pattern
// as process_event_log.
void disk_event_log(const DiskEvent* event);

}

// Field order is alignment-first (8 -> 4 -> 1 bytes), same rule used for
// Sample and ProcessEvent. 8 (timestamp) + 8 (byte_offset) + 4
// (transfer_size) + 4 (disk_number) + 1 (direction) = 25 raw bytes, rounded
// up to 32 (a multiple of 8, timestamp/byte_offset's alignment).
static_assert(sizeof(DiskEvent) == 32, "DiskEvent layout drifted from disk_event.zig - fix both sides together");
