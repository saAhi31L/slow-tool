# **SlowTool**

For weeks, my laptop had been acting up — slowing to a crawl, the fan spinning up for no apparent reason. The strange part was that Task Manager never showed anything wrong. CPU, memory, disk, al[...]

Slow Tool is a real-time Windows system monitor: it watches CPU, memory, disk, and network usage every second, and the moment something spikes, it logs exactly which process was the top CPU consume[...]

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

- Sample: (120 bytes) — one row of PDH counters + the top CPU-consuming process that tick, pushed once a second via `ring_push()`.

- ProcessEvent (80 bytes) — one process start or exit, pushed the instant ETW reports it.
- **`DiskEvent`** (32 bytes) — one physical disk I/O completion, pushed the instant ETW reports it.




Capture layer (C++)

- PDH(Performance Data Helper) reads four system-wide counters every second: CPU (`% Processor Time`), available memory (`Available MBytes`), disk (`% Disk Time`), and network (summed across all i[...]
- Toolhelp32 snapshots running processes each tick and computes per-process CPU deltas (via `GetProcessTimes` + a PID-keyed map, rebuilt every tick so it self-prunes exited processes) to find the [...]
- ETW (Event Tracing for Windows) runs a real-time session with two kernel providers on a dedicated consumer thread: `Microsoft-Windows-Kernel-Process` (process lifecycle) and `Microsoft-Windows-Kerne[...]

Correlation layer (Zig)
- A fixed 60-slot ring buffer holds the last 60 one-second samples, allocation-free, gated so no comparison happens until it's completely full (60 real seconds of history, not a partial average).
- A rolling baseline (the ring's average) is compared against the current sample each tick; a metric "spikes" if it exceeds 1.5× baseline (or, for memory, drops below baseline ÷ 1.5 — directio[...]
- All file output goes through raw `kernel32.dll` calls (`CreateFileW`/`WriteFile`/etc.), bypassing Zig's standard I/O entirely — a deliberate choice after Zig 0.16's standard-library file/debug[...]
What it logs

Three separate CSVs, each a different shape of data on purpose — a periodic measurement, a discrete process event, and a discrete disk event don't share a row shape, so mixing them would make al[...]

| File | What | Columns |
|---|---|---|
| `slowtool_log.csv` | A spike event (CPU/disk/network/memory crossed its threshold) | timestamp, metric, current/baseline value, top PID/process at that tick |
| `slowtool_process_events.csv` | A process starting or exiting (via ETW, not polling) | timestamp, start/exit, PID, process name |
| `slowtool_disk_events.csv` | A physical disk I/O completion (via ETW) | timestamp, read/write/other, disk number, byte offset, transfer size |

## Real results

**v1 (PDH + Toolhelp32 + ring buffer + correlation) ran unattended for 29.3 hours** with zero crashes and 58,824 logged spike rows. Real findings from that run: `WebView2` processes were a persist[...]

**ETW process-events caught real activity polling misses.** Of 53 matched process start/exit pairs in one session, 11 lasted 0 full seconds — meaning the entire process lifetime fit inside a sin[...]

Bugs found and fixed along the way

A few of the more interesting ones, since the debugging is as much the point of this project as the final code:

- **ETW event header PID is not the subject's PID.** `EventHeader.ProcessId` reports whichever process *relayed* the event to ETW, not the process the event is actually about — 91 distinct proce[...]
- **The same field name decodes differently across event schemas.** `ImageName` is valid UTF-16 on a process-start event but decodes to garbage on the matching exit event (same provider, same fiel[...]
- **Disk I/O events have no usable process attribution at all.** Unlike process events, a `DiskIo` completion's header PID is always `4` (the System process — it reflects where the I/O *complete[...]
- **Zig 0.16's standard file I/O doesn't link cleanly into an MSVC executable.** `std.fs`/`std.Io`/`std.debug.print` all pull in NT-native runtime machinery in debug builds, producing 60 unresolve[...]

Status

- ✅ Core monitoring (PDH + Toolhelp32 + ring buffer + correlation + CSV logging) — verified, 29.3-hour run
- ✅ ETW process start/exit tracking — verified
- ✅ ETW disk I/O event tracking (no process attribution yet) — built
- ⏳ Per-process disk attribution via `Kernel-File` correlation — deferred
- ⏳ GPU tracking, historical storage — deferred
- ⏳ **Local AI summarizer (planned)** — running **Qwen2.5-7B-Instruct** locally to turn a session's raw CSVs into a plain-English summary ("here's what was actually slowing the machine down, a[...]

Building

Windows-only (PDH/Toolhelp32/ETW are Win32 APIs). Requires MSVC (Visual Studio Build Tools, "Desktop development with C++") and the [Zig 0.16 compiler](https://ziglang.org/). ETW requires running as A[...]

```
# From an elevated Developer Command Prompt:
cd correlate
zig build-obj correlate.zig -O ReleaseFast

cd ../capture
cl /EHsc /std:c++17 /c capture_loop.cpp win32\pdh.cpp win32\toolhelp32.cpp win32\etw.cpp
cl /EHsc /std:c++17 capture_loop.obj pdh.obj toolhelp32.obj etw.obj ../correlate/correlate.obj /Fe:slowtool.exe

slowtool.exe
```


