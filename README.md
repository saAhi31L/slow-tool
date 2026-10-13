# Slow Tool

A real-time Windows system monitor built to learn **C++/Zig interoperability** by building something real instead of a toy example. C++ handles all Win32 data capture (performance counters, process enumeration, kernel event tracing); Zig owns correlation, state, and logging. No GUI, no database, no cloud reporting, no LLM calls anywhere in the pipeline — correctness means the numbers are real.

## Why two languages

C++ has first-class access to the Win32 APIs this needs (PDH, Toolhelp32, ETW) with decades of documentation and examples. Zig was chosen for the analysis layer specifically to work through a real FFI boundary: fixed-size `extern` structs shared byte-for-byte between both languages, manual memory layout (alignment-ordered fields, explicit padding), and zero heap allocation after startup. Every struct crossing the boundary is `extern "C"` on the C++ side and `extern struct` on the Zig side, with a `static_assert`/`comptime` size check on both ends so the two can never silently drift apart.

## Architecture

```
capture/ (C++)                         correlate/ (Zig)
├── win32/pdh.cpp        ─┐
├── win32/toolhelp32.cpp  ┼─ Sample ──► ring.zig (60-sample ring buffer, no heap)
├── win32/etw.cpp        ─┘             │
│                                       ▼
│                              correlate.zig (rolling-baseline spike detection)
│                                       │
└── capture_loop.cpp                   ▼
    (1-second tick)              log.zig (raw kernel32 file I/O → CSV)
```

Three typed structs cross the boundary, one per tick/event:
- **`Sample`** (120 bytes) — one row of PDH counters + the top CPU-consuming process that tick, pushed once a second via `ring_push()`.
- **`ProcessEvent`** (80 bytes) — one process start or exit, pushed the instant ETW reports it.
- **`DiskEvent`** (32 bytes) — one physical disk I/O completion, pushed the instant ETW reports it.

### Capture layer (C++)
- **PDH** (Performance Data Helper) reads four system-wide counters every second: CPU (`% Processor Time`), available memory (`Available MBytes`), disk (`% Disk Time`), and network (summed across all interfaces, to catch a WSL2/Hyper-V virtual adapter a single-adapter view would miss).
- **Toolhelp32** snapshots running processes each tick and computes per-process CPU deltas (via `GetProcessTimes` + a PID-keyed map, rebuilt every tick so it self-prunes exited processes) to find the top CPU consumer.
- **ETW** (Event Tracing for Windows) runs a real-time session with two kernel providers on a dedicated consumer thread: `Microsoft-Windows-Kernel-Process` (process lifecycle) and `Microsoft-Windows-Kernel-Disk` (disk I/O completions), decoded via TDH (Trace Data Helper) rather than fragile manual byte-offset parsing.

### Correlation layer (Zig)
- A fixed 60-slot ring buffer holds the last 60 one-second samples, allocation-free, gated so no comparison happens until it's completely full (60 real seconds of history, not a partial average).
- A rolling baseline (the ring's average) is compared against the current sample each tick; a metric "spikes" if it exceeds 1.5× baseline (or, for memory, drops below baseline ÷ 1.5 — direction-aware, since falling memory is the "bad" direction).
- All file output goes through raw `kernel32.dll` calls (`CreateFileW`/`WriteFile`/etc.), bypassing Zig's standard I/O entirely — a deliberate choice after Zig 0.16's standard-library file/debug machinery pulled in NT-native runtime code that wouldn't link cleanly against MSVC in release builds.

## What it logs

Three separate CSVs, each a different shape of data on purpose — a periodic measurement, a discrete process event, and a discrete disk event don't share a row shape, so mixing them would make all three harder to read:

| File | What | Columns |
|---|---|---|
| `slowtool_log.csv` | A spike event (CPU/disk/network/memory crossed its threshold) | timestamp, metric, current/baseline value, top PID/process at that tick |
| `slowtool_process_events.csv` | A process starting or exiting (via ETW, not polling) | timestamp, start/exit, PID, process name |
| `slowtool_disk_events.csv` | A physical disk I/O completion (via ETW) | timestamp, read/write/other, disk number, byte offset, transfer size |

## Real results

**v1 (PDH + Toolhelp32 + ring buffer + correlation) ran unattended for 29.3 hours** with zero crashes and 58,824 logged spike rows. Real findings from that run: `WebView2` processes were a persistent recurring CPU consumer (1,412 occurrences), and — unprompted, found by the data rather than searched for — `Grammarly.Desktop.exe` was the #2 most frequent top-CPU process (14,471 occurrences, ~25% of all spikes), which led to disabling its startup launch.

**ETW process-events caught real activity polling misses.** Of 53 matched process start/exit pairs in one session, 11 lasted 0 full seconds — meaning the entire process lifetime fit inside a single tick of Toolhelp32's 1-second polling, including real applications (`chrome.exe`, `FreedomBlocker.exe`), not just background indexing. That's not a hypothetical: it's the original justification for adding ETW, confirmed with data from this machine.

## Bugs found and fixed along the way

A few of the more interesting ones, since the debugging is as much the point of this project as the final code:

- **ETW event header PID is not the subject's PID.** `EventHeader.ProcessId` reports whichever process *relayed* the event to ETW, not the process the event is actually about — 91 distinct process-start events were initially showing the same PID before this was caught and fixed by reading the event payload's own `ProcessID` field via TDH instead.
- **The same field name decodes differently across event schemas.** `ImageName` is valid UTF-16 on a process-start event but decodes to garbage on the matching exit event (same provider, same field name, different underlying encoding) — fixed by trusting the direct decode only on start events and recovering the name from a PID→name map on exit.
- **Disk I/O events have no usable process attribution at all.** Unlike process events, a `DiskIo` completion's header PID is always `4` (the System process — it reflects where the I/O *completed*, not who requested it), and the payload carries no PID field either. True per-process disk attribution would require a second provider (`Kernel-File`) correlated by a shared pointer — scoped out for now and documented as a known limitation rather than faked.
- **Zig 0.16's standard file I/O doesn't link cleanly into an MSVC executable.** `std.fs`/`std.Io`/`std.debug.print` all pull in NT-native runtime machinery in debug builds, producing 60 unresolved `Nt*`/`Rtl*` link errors. Fixed by building with `zig build-obj -O ReleaseFast` (not `build-lib`) and routing all Zig-side file I/O through raw `kernel32` externs instead of the standard library.

## Status

- ✅ Core monitoring (PDH + Toolhelp32 + ring buffer + correlation + CSV logging) — verified, 29.3-hour run
- ✅ ETW process start/exit tracking — verified
- ✅ ETW disk I/O event tracking (no process attribution yet) — built
- ⏳ Per-process disk attribution via `Kernel-File` correlation — deferred
- ⏳ GPU tracking, historical storage, local AI summarizer — deferred

## Building

Windows-only (PDH/Toolhelp32/ETW are Win32 APIs). Requires MSVC (Visual Studio Build Tools, "Desktop development with C++") and the [Zig 0.16 compiler](https://ziglang.org/). ETW requires running as Administrator.

```
# From an elevated Developer Command Prompt:
cd correlate
zig build-obj correlate.zig -O ReleaseFast

cd ../capture
cl /EHsc /std:c++17 /c capture_loop.cpp win32\pdh.cpp win32\toolhelp32.cpp win32\etw.cpp
cl /EHsc /std:c++17 capture_loop.obj pdh.obj toolhelp32.obj etw.obj ../correlate/correlate.obj /Fe:slowtool.exe

slowtool.exe
```
