#include "win32/pdh.h"
#include "win32/toolhelp32.h"

#include <windows.h>
#include <cstdio>

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

    for (int tick = 1; tick <= 10; tick++) {
        Sleep(1000);

        if (!pdh_collect()) {
            printf("pdh_collect failed on tick %d\n", tick);
            break;
        }

        PdhValues v;
        if (!pdh_read(&v)) {
            printf("pdh_read failed on tick %d\n", tick);
            break;
        }

        TopProcess top;
        bool have_top = th32_top_process(&top);

        printf("tick %2d  cpu %5.1f%%  free %7.0f MB  disk %5.1f%%  net %10.0f B/s  ",
               tick, v.cpu_percent, v.available_memory_mb, v.disk_percent, v.network_bytes_per_sec);

        if (have_top) {
            printf("top: %ls (pid %lu) %.1f%%\n", top.name, top.pid, top.cpu_percent);
        } else {
            printf("top: (none yet)\n");
        }
    }

    pdh_shutdown();
    return 0;
}
