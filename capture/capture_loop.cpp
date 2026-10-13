#include "win32/pdh.h"
#include "win32/toolhelp32.h"
#include "win32/etw.h"
#include "sample.h"

#include <windows.h>
#include <cstdio>
#include <ctime>

int main() {
    if (!pdh_init()) {
        printf("pdh_init failed\n");
        return 1;
    }
    if (!th32_init()) {
        printf("th32_init failed\n");
        pdh_shutdown();
        return 1;
    }

    // Unlike pdh_init/th32_init, a failed etw_init is not treated as fatal -
    // ETW is an additive feature (catches short-lived processes polling
    // misses), not required for the core PDH/Toolhelp32 monitoring to work.
    // The most likely failure cause is not running as Administrator.
    if (!etw_init()) {
        printf("etw_init failed - continuing without it (try running as Administrator)\n");
    }

    printf("SlowTool capture loop running. Press Ctrl+C to stop.\n");

    while (true) {
        Sleep(1000);

        if (!pdh_collect()) {
            printf("pdh_collect failed\n");
            continue;
        }

        PdhValues v;
        if (!pdh_read(&v)) {
            printf("pdh_read failed\n");
            continue;
        }

        TopProcess top;
        bool have_top = th32_top_process(&top);

        Sample sample{};
        sample.timestamp = static_cast<uint64_t>(std::time(nullptr));
        sample.cpu_percent = v.cpu_percent;
        sample.available_memory_mb = v.available_memory_mb;
        sample.disk_percent = v.disk_percent;
        sample.network_bytes_per_sec = v.network_bytes_per_sec;

        if (have_top) {
            sample.top_pid = top.pid;
            sample.top_cpu_percent = top.cpu_percent;
            wcsncpy_s(sample.top_process_name, top.name, _TRUNCATE);
        } else {
            sample.top_pid = 0;
            sample.top_cpu_percent = 0;
            sample.top_process_name[0] = L'\0';
        }

        ring_push(&sample);
    }

    etw_shutdown();
    pdh_shutdown();
    return 0;
}
