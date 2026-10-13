#include "toolhelp32.h"

#include <tlhelp32.h>
#include <unordered_map>

static std::unordered_map<DWORD, ULONGLONG> g_prev_times;
static ULONGLONG g_prev_tick_time = 0;
static DWORD g_logical_cores = 1;

static ULONGLONG file_time_to_u64(const FILETIME& ft) {
    ULARGE_INTEGER uli;
    uli.LowPart = ft.dwLowDateTime;
    uli.HighPart = ft.dwHighDateTime;
    return uli.QuadPart;
}

static ULONGLONG current_time_100ns() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return file_time_to_u64(ft);
}

static bool get_process_cpu_time(DWORD pid, ULONGLONG* out_time) {
    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (proc == nullptr) {
        return false;
    }
    FILETIME creation, exit_time, kernel, user;
    bool ok = GetProcessTimes(proc, &creation, &exit_time, &kernel, &user) != 0;
    CloseHandle(proc);
    if (!ok) {
        return false;
    }
    *out_time = file_time_to_u64(kernel) + file_time_to_u64(user);
    return true;
}

bool th32_init() {
    SYSTEM_INFO sys_info;
    GetSystemInfo(&sys_info);
    g_logical_cores = sys_info.dwNumberOfProcessors;
    if (g_logical_cores == 0) {
        g_logical_cores = 1;
    }

    g_prev_times.clear();

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return false;
    }

    PROCESSENTRY32W entry;
    entry.dwSize = sizeof(PROCESSENTRY32W);

    if (Process32FirstW(snapshot, &entry)) {
        do {
            ULONGLONG cpu_time;
            if (get_process_cpu_time(entry.th32ProcessID, &cpu_time)) {
                g_prev_times[entry.th32ProcessID] = cpu_time;
            }
        } while (Process32NextW(snapshot, &entry));
    }

    CloseHandle(snapshot);
    g_prev_tick_time = current_time_100ns();
    return true;
}

bool th32_top_process(TopProcess* out) {
    if (out == nullptr) {
        return false;
    }

    ULONGLONG now = current_time_100ns();
    ULONGLONG elapsed = now - g_prev_tick_time;
    if (elapsed == 0) {
        elapsed = 1;
    }

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return false;
    }

    PROCESSENTRY32W entry;
    entry.dwSize = sizeof(PROCESSENTRY32W);

    std::unordered_map<DWORD, ULONGLONG> new_times;
    bool found_best = false;
    double best_percent = -1.0;
    DWORD best_pid = 0;
    wchar_t best_name[MAX_PATH] = L"";

    if (Process32FirstW(snapshot, &entry)) {
        do {
            ULONGLONG cpu_time;
            if (!get_process_cpu_time(entry.th32ProcessID, &cpu_time)) {
                continue;
            }
            new_times[entry.th32ProcessID] = cpu_time;

            auto prev = g_prev_times.find(entry.th32ProcessID);
            if (prev == g_prev_times.end()) {
                continue;
            }
            ULONGLONG delta = cpu_time - prev->second;
            double percent = (static_cast<double>(delta) / (static_cast<double>(elapsed) * g_logical_cores)) * 100.0;

            if (percent > best_percent) {
                best_percent = percent;
                best_pid = entry.th32ProcessID;
                wcsncpy_s(best_name, entry.szExeFile, _TRUNCATE);
                found_best = true;
            }
        } while (Process32NextW(snapshot, &entry));
    }

    CloseHandle(snapshot);

    g_prev_times = std::move(new_times);
    g_prev_tick_time = now;

    if (!found_best) {
        return false;
    }

    out->pid = best_pid;
    out->cpu_percent = best_percent;
    wcsncpy_s(out->name, best_name, _TRUNCATE);
    return true;
}
