#include "etw.h"
#include "../process_event.h"
#include "../disk_event.h"

#include <windows.h>
#include <evntrace.h>
#include <evntcons.h>
#include <tdh.h>
#include <thread>
#include <cstdio>
#include <ctime>
#include <vector>
#include <cwchar>
#include <unordered_map>
#include <string>

#pragma comment(lib, "tdh.lib")
#pragma comment(lib, "advapi32.lib")

// --- Session identity ---
// This is a "modern" manifest-based ETW session (not the legacy, single-
// instance NT Kernel Logger), so we're free to pick any unique name.
static const wchar_t* SESSION_NAME = L"SlowToolETW";

// The Microsoft-Windows-Kernel-Process provider's GUID. This is a fixed,
// Windows-defined identifier for "the kernel component that reports process
// lifecycle events" - not something we invent, it has to match exactly.
static const GUID KERNEL_PROCESS_PROVIDER = {
    0x22fb2cd6, 0x0e7b, 0x422b, { 0xa0, 0xc7, 0x2f, 0xad, 0x1f, 0xd0, 0xe7, 0x16 }
};

// This provider covers more than just process start/stop (threads, image
// loads, and other kernel housekeeping are bundled into it too). This
// keyword bit asks specifically for the process-lifecycle category, instead
// of receiving everything and having to filter it ourselves after the fact.
static const ULONGLONG KEYWORD_PROCESS = 0x10;

// The Microsoft-Windows-Kernel-Disk provider's GUID. Confirmed directly from
// this machine via `logman query providers "Microsoft-Windows-Kernel-Disk"`
// rather than assumed - a kernel provider GUID is exactly the kind of fact
// that fails silently instead of loudly if guessed wrong.
static const GUID KERNEL_DISK_PROVIDER = {
    0xc7bde69a, 0xe1e0, 0x4177, { 0xb6, 0xef, 0x28, 0x3a, 0xd1, 0x52, 0x52, 0x71 }
};

// Unlike Kernel-Process, this provider only exposes one keyword bit, covering
// everything it emits (reads, writes, flushes, init events, and more). There
// is no finer-grained keyword to pre-filter with at EnableTraceEx2 time, so
// narrowing down to just read/write events has to happen on opcode, inside
// the callback - once we actually know what Kernel-Disk's opcode values are.
static const ULONGLONG KEYWORD_DISK = 0x8000000000000000ULL;

// --- Persistent state, same pattern as the other win32/*.cpp files ---
static TRACEHANDLE g_session_handle = 0;
static TRACEHANDLE g_trace_handle = 0;
static std::thread g_consumer_thread;

// PID -> name, filled in on a process's start event and consumed (looked up
// then erased) on its matching exit event, since the exit event's schema
// carries no name field of its own. Same keyed-lookup pattern as
// toolhelp32.cpp's g_prev_times. Safe without a lock - ETW's real-time
// consumer calls this callback for one event at a time, never concurrently.
static std::unordered_map<DWORD, std::wstring> g_process_names;

// Extracts one string property (e.g. the process image name) from an ETW
// event's payload by name, using TDH (Trace Data Helper). This is the
// documented, robust way to decode manifest-based event fields - the
// alternative (manually computing byte offsets into the raw payload) is far
// more fragile and not something to hand-roll.
// Returns false (and leaves *out untouched) if the field can't be read -
// the caller is expected to fall back to a placeholder, not treat this as
// fatal, since PID and start/exit already come from the event header
// directly and don't depend on this succeeding.
static bool get_string_property(PEVENT_RECORD event_record, const wchar_t* property_name, wchar_t* out, size_t out_capacity_chars) {
    PROPERTY_DATA_DESCRIPTOR descriptor = {};
    descriptor.PropertyName = reinterpret_cast<ULONGLONG>(property_name);
    descriptor.ArrayIndex = ULONG_MAX;

    ULONG property_size = 0;
    if (TdhGetPropertySize(event_record, 0, nullptr, 1, &descriptor, &property_size) != ERROR_SUCCESS) {
        return false;
    }
    if (property_size == 0 || property_size > out_capacity_chars * sizeof(wchar_t)) {
        return false;
    }

    return TdhGetProperty(event_record, 0, nullptr, 1, &descriptor, property_size, reinterpret_cast<PBYTE>(out)) == ERROR_SUCCESS;
}

// Same idea as get_string_property, but for a 4-byte numeric field - used
// for the event payload's own "ProcessID" property. EventHeader.ProcessId
// is NOT reliable for this: it's whichever process reported the event to
// ETW, not necessarily the process the event is actually about (confirmed
// by real data - many distinct process instances sharing one header PID).
static bool get_uint32_property(PEVENT_RECORD event_record, const wchar_t* property_name, uint32_t* out) {
    PROPERTY_DATA_DESCRIPTOR descriptor = {};
    descriptor.PropertyName = reinterpret_cast<ULONGLONG>(property_name);
    descriptor.ArrayIndex = ULONG_MAX;

    ULONG property_size = 0;
    if (TdhGetPropertySize(event_record, 0, nullptr, 1, &descriptor, &property_size) != ERROR_SUCCESS) {
        return false;
    }
    if (property_size != sizeof(uint32_t)) {
        return false;
    }

    return TdhGetProperty(event_record, 0, nullptr, 1, &descriptor, property_size, reinterpret_cast<PBYTE>(out)) == ERROR_SUCCESS;
}

// Same idea as get_uint32_property, but for an 8-byte numeric field - used
// for the disk event payload's "ByteOffset" property.
static bool get_uint64_property(PEVENT_RECORD event_record, const wchar_t* property_name, uint64_t* out) {
    PROPERTY_DATA_DESCRIPTOR descriptor = {};
    descriptor.PropertyName = reinterpret_cast<ULONGLONG>(property_name);
    descriptor.ArrayIndex = ULONG_MAX;

    ULONG property_size = 0;
    if (TdhGetPropertySize(event_record, 0, nullptr, 1, &descriptor, &property_size) != ERROR_SUCCESS) {
        return false;
    }
    if (property_size != sizeof(uint64_t)) {
        return false;
    }

    return TdhGetProperty(event_record, 0, nullptr, 1, &descriptor, property_size, reinterpret_cast<PBYTE>(out)) == ERROR_SUCCESS;
}

// --- TEMPORARY diagnostic: print this event's real property names ---
// TdhGetEventInformation returns a TRACE_EVENT_INFO describing the event's
// actual schema. Each property's name is a null-terminated wide string
// stored elsewhere in the same buffer, pointed to by NameOffset (a byte
// offset into that buffer, not a separate allocation) - this is how we find
// out the CORRECT field name instead of guessing again.
static void dump_property_names(PEVENT_RECORD event_record) {
    DWORD buffer_size = 0;
    if (TdhGetEventInformation(event_record, 0, nullptr, nullptr, &buffer_size) != ERROR_INSUFFICIENT_BUFFER) {
        printf("etw: [diagnostic] TdhGetEventInformation size query failed\n");
        return;
    }

    std::vector<BYTE> info_buffer(buffer_size);
    TRACE_EVENT_INFO* event_info = reinterpret_cast<TRACE_EVENT_INFO*>(info_buffer.data());
    if (TdhGetEventInformation(event_record, 0, nullptr, event_info, &buffer_size) != ERROR_SUCCESS) {
        printf("etw: [diagnostic] TdhGetEventInformation fetch failed\n");
        return;
    }

    printf("etw: [diagnostic] event has %lu properties:\n", event_info->TopLevelPropertyCount);
    for (ULONG i = 0; i < event_info->TopLevelPropertyCount; i++) {
        const wchar_t* name = reinterpret_cast<const wchar_t*>(
            info_buffer.data() + event_info->EventPropertyInfoArray[i].NameOffset);
        wprintf(L"etw: [diagnostic]   [%lu] %s\n", i, name);
    }
}

// Caps how many times the diagnostic above runs, so the console doesn't
// flood - we only need to see this schema information a handful of times.
static int g_diagnostic_dumps_remaining = 5;


// Confirmed from the real schema dump: ImageName arrives as a full path
// (e.g. C:\...\chrome.exe), not a bare filename. This keeps only the part
// after the last backslash - the actually useful, identifying piece - so
// it fits meaningfully inside process_name's small fixed size.
static const wchar_t* filename_from_path(const wchar_t* path) {
    const wchar_t* last_slash = wcsrchr(path, L'\\');
    return last_slash ? (last_slash + 1) : path;
}

// Phase B callback: builds a real ProcessEvent from each start/exit event
// and logs it across the boundary into Zig.
static void WINAPI etw_event_callback(PEVENT_RECORD event_record) {
    // Both Kernel-Process and Kernel-Disk events arrive through this same
    // callback now that both providers are enabled on the same session.
    // Opcode numbers are only meaningful relative to a specific provider
    // (opcode 1 means "start" for Kernel-Process, but could mean anything -
    // or nothing - for Kernel-Disk), so provider identity must be checked
    // BEFORE opcode is interpreted at all.
    if (IsEqualGUID(event_record->EventHeader.ProviderId, KERNEL_DISK_PROVIDER)) {
        // Confirmed from real captured data (60-sample diagnostic run
        // during a large file copy): EventHeader.ProcessId is always 4 (the
        // System process) on disk events - not usable for per-process
        // attribution, unlike Kernel-Process's header PID. True attribution
        // would need a separate Kernel-File correlation subsystem (matching
        // DiskIo completions to FileIo events by shared pointer) - out of
        // scope for now, deferred.
        //
        // Also confirmed: opcode and id don't cleanly distinguish read from
        // write (both appeared across events of either kind). The real
        // signal is the IrpFlags bitmask: IRP_READ_OPERATION (0x100) and
        // IRP_WRITE_OPERATION (0x200) are standard Windows kernel flag bits,
        // but most captured events (very likely cached/paging I/O, e.g.
        // write-behind flushes) carry NEITHER bit - "other" is an honest
        // bucket for those, not a guess forced into read or write.
        uint32_t irp_flags = 0;
        uint32_t transfer_size = 0;
        uint32_t disk_number = 0;
        uint64_t byte_offset = 0;
        get_uint32_property(event_record, L"IrpFlags", &irp_flags);
        get_uint32_property(event_record, L"TransferSize", &transfer_size);
        get_uint32_property(event_record, L"DiskNumber", &disk_number);
        get_uint64_property(event_record, L"ByteOffset", &byte_offset);

        const uint32_t IRP_READ_OPERATION = 0x100;
        const uint32_t IRP_WRITE_OPERATION = 0x200;

        DiskEvent disk_event{};
        disk_event.timestamp = static_cast<uint64_t>(std::time(nullptr));
        disk_event.byte_offset = byte_offset;
        disk_event.transfer_size = transfer_size;
        disk_event.disk_number = disk_number;
        if (irp_flags & IRP_READ_OPERATION) {
            disk_event.direction = 0;
        } else if (irp_flags & IRP_WRITE_OPERATION) {
            disk_event.direction = 1;
        } else {
            disk_event.direction = 2;
        }

        disk_event_log(&disk_event);
        return; // disk events never fall through to the process-event logic below
    }

    // Everything below this point assumes a Kernel-Process event.
    // Confirmed against real output during Phase A: this provider's
    // process-lifecycle category reports opcode 1 for a process starting
    // and opcode 2 for a process exiting.
    const UCHAR opcode = event_record->EventHeader.EventDescriptor.Opcode;
    if (opcode != 1 && opcode != 2) {
        return; // shouldn't happen given our KEYWORD_PROCESS filter, but be defensive
    }

    if (g_diagnostic_dumps_remaining > 0) {
        g_diagnostic_dumps_remaining--;
        dump_property_names(event_record);
    }

    ProcessEvent process_event{};
    process_event.timestamp = static_cast<uint64_t>(std::time(nullptr));
    process_event.is_start = (opcode == 1);

    // Confirmed from real data: EventHeader.ProcessId is NOT the started/
    // exited process's own PID - it's whichever process reported the event.
    // The payload's own "ProcessID" field is the correct source; fall back
    // to the header value only if that lookup somehow fails.
    uint32_t payload_pid = 0;
    process_event.pid = get_uint32_property(event_record, L"ProcessID", &payload_pid)
        ? payload_pid
        : event_record->EventHeader.ProcessId;

    // The exit event's ImageName field exists in the schema, but decodes
    // to garbage ('?' characters) with the same logic that works fine for
    // start events - almost certainly a different underlying text encoding
    // (ANSI vs Unicode) despite the shared field name. Rather than handle
    // both encodings, only trust the direct TDH lookup on start events, and
    // always use the PID->name map (filled in by that same start event) for
    // exit events - simpler, and already proven correct.
    if (process_event.is_start) {
        wchar_t image_path[MAX_PATH] = {};
        if (get_string_property(event_record, L"ImageName", image_path, MAX_PATH)) {
            const wchar_t* filename = filename_from_path(image_path);
            wcsncpy_s(process_event.process_name, filename, _TRUNCATE);
            g_process_names[process_event.pid] = filename;
        } else {
            wcsncpy_s(process_event.process_name, L"(unknown)", _TRUNCATE);
        }
    } else {
        auto it = g_process_names.find(process_event.pid);
        if (it != g_process_names.end()) {
            wcsncpy_s(process_event.process_name, it->second.c_str(), _TRUNCATE);
        } else {
            // Most likely this process started before slowtool.exe did, so
            // we never saw its start event to learn its name from.
            wcsncpy_s(process_event.process_name, L"(unknown)", _TRUNCATE);
        }
    }

    // Clean up on exit regardless of which path supplied the name - this
    // PID is gone now, and Windows can reuse the number for an unrelated
    // process later, so a stale entry must not be left behind.
    if (!process_event.is_start) {
        g_process_names.erase(process_event.pid);
    }

    wprintf(L"etw: process %s, pid=%lu, name=%s\n",
            process_event.is_start ? L"start" : L"exit",
            process_event.pid, process_event.process_name);

    process_event_log(&process_event);
}

// Fills in the fixed fields every EVENT_TRACE_PROPERTIES call needs.
// Used twice in etw_init() (once to stop any stale leftover session, once
// for the real StartTraceW call), so it's pulled out to avoid repeating it.
static void init_properties(EVENT_TRACE_PROPERTIES* properties, size_t buffer_size) {
    ZeroMemory(properties, buffer_size);
    properties->Wnode.BufferSize = static_cast<ULONG>(buffer_size);
    properties->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
    properties->Wnode.ClientContext = 1; // use QueryPerformanceCounter-based timestamps
    properties->LogFileMode = EVENT_TRACE_REAL_TIME_MODE;
    properties->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
}

bool etw_init() {
    // EVENT_TRACE_PROPERTIES is a fixed-size header, but ETW also needs the
    // session's name stored in memory immediately after it. One buffer big
    // enough for both, with LoggerNameOffset pointing at where the name
    // starts, is how ETW expects this to be laid out.
    static BYTE properties_buffer[sizeof(EVENT_TRACE_PROPERTIES) + 256];
    EVENT_TRACE_PROPERTIES* properties = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(properties_buffer);

    // If a previous run didn't shut down cleanly (e.g. the program was
    // killed instead of stopped via Ctrl+C reaching etw_shutdown), a stale
    // session named SlowToolETW may still be registered with Windows. Stop
    // it first so the real StartTraceW below doesn't fail every time that
    // happens. Failure here is expected and fine if no stale session exists.
    init_properties(properties, sizeof(properties_buffer));
    ControlTraceW(0, SESSION_NAME, properties, EVENT_TRACE_CONTROL_STOP);

    // Now start the real session.
    init_properties(properties, sizeof(properties_buffer));
    ULONG status = StartTraceW(&g_session_handle, SESSION_NAME, properties);
    if (status != ERROR_SUCCESS) {
        printf("etw_init: StartTraceW failed (%lu) - is this running as Administrator?\n", status);
        return false;
    }

    // Turn on the Kernel-Process provider for this session specifically -
    // without this, the session exists but reports nothing.
    status = EnableTraceEx2(
        g_session_handle,
        &KERNEL_PROCESS_PROVIDER,
        EVENT_CONTROL_CODE_ENABLE_PROVIDER,
        TRACE_LEVEL_INFORMATION,
        KEYWORD_PROCESS, 0, 0, nullptr
    );
    if (status != ERROR_SUCCESS) {
        printf("etw_init: EnableTraceEx2 failed (%lu)\n", status);
        return false;
    }

    // Turn on the Kernel-Disk provider too - same session, same consumer
    // thread, same callback. A session can have multiple providers enabled
    // on it; it's just the buffer/thread plumbing, not a per-provider thing.
    status = EnableTraceEx2(
        g_session_handle,
        &KERNEL_DISK_PROVIDER,
        EVENT_CONTROL_CODE_ENABLE_PROVIDER,
        TRACE_LEVEL_INFORMATION,
        KEYWORD_DISK, 0, 0, nullptr
    );
    if (status != ERROR_SUCCESS) {
        printf("etw_init: EnableTraceEx2 (disk) failed (%lu)\n", status);
        return false;
    }

    // Open a handle to consume events from this session in real time,
    // registering our callback to be invoked once per event.
    EVENT_TRACE_LOGFILEW logfile = {};
    logfile.LoggerName = const_cast<wchar_t*>(SESSION_NAME);
    logfile.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD;
    logfile.EventRecordCallback = etw_event_callback;

    g_trace_handle = OpenTraceW(&logfile);
    if (g_trace_handle == INVALID_PROCESSTRACE_HANDLE) {
        printf("etw_init: OpenTraceW failed\n");
        return false;
    }

    // ProcessTrace blocks forever, pumping events to the callback above,
    // until the session is stopped elsewhere (see etw_shutdown). It has to
    // run on its own thread so it doesn't freeze the main capture loop.
    g_consumer_thread = std::thread([]() {
        ProcessTrace(&g_trace_handle, 1, nullptr, nullptr);
    });

    return true;
}

void etw_shutdown() {
    static BYTE properties_buffer[sizeof(EVENT_TRACE_PROPERTIES) + 256];
    EVENT_TRACE_PROPERTIES* properties = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(properties_buffer);
    init_properties(properties, sizeof(properties_buffer));

    // Stopping the session is what causes the blocked ProcessTrace call on
    // the consumer thread to finally return.
    ControlTraceW(g_session_handle, SESSION_NAME, properties, EVENT_TRACE_CONTROL_STOP);

    if (g_consumer_thread.joinable()) {
        g_consumer_thread.join();
    }
}
