#include "pdh.h"

#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <cstdlib>

#pragma comment(lib, "pdh.lib")

static PDH_HQUERY g_query = nullptr;
static PDH_HCOUNTER g_cpu_counter = nullptr;
static PDH_HCOUNTER g_mem_counter = nullptr;
static PDH_HCOUNTER g_disk_counter = nullptr;
static PDH_HCOUNTER g_net_counter = nullptr;
static PDH_FMT_COUNTERVALUE_ITEM_W* g_net_buffer = nullptr;
static DWORD g_net_buffer_bytes = 0;

bool pdh_init() {
    if (PdhOpenQuery(nullptr, 0, &g_query) != ERROR_SUCCESS) {
        return false;
    }
    if (PdhAddEnglishCounterW(g_query, L"\\Processor(_Total)\\% Processor Time", 0, &g_cpu_counter) != ERROR_SUCCESS) {
        pdh_shutdown();
        return false;
    }
    if (PdhAddEnglishCounterW(g_query, L"\\Memory\\Available MBytes", 0, &g_mem_counter) != ERROR_SUCCESS) {
        pdh_shutdown();
        return false;
    }
    if (PdhAddEnglishCounterW(g_query, L"\\PhysicalDisk(_Total)\\% Disk Time", 0, &g_disk_counter) != ERROR_SUCCESS) {
        pdh_shutdown();
        return false;
    }
    if (PdhAddEnglishCounterW(g_query, L"\\Network Interface(*)\\Bytes Total/sec", 0, &g_net_counter) != ERROR_SUCCESS) {
        pdh_shutdown();
        return false;
    }
    if (PdhCollectQueryData(g_query) != ERROR_SUCCESS) {
        pdh_shutdown();
        return false;
    }
    return true;
}

bool pdh_collect() {
    if (g_query == nullptr) {
        return false;
    }
    return PdhCollectQueryData(g_query) == ERROR_SUCCESS;
}

static bool read_network_total(double* total) {
    DWORD bytes = g_net_buffer_bytes;
    DWORD count = 0;
    PDH_STATUS status = PdhGetFormattedCounterArrayW(g_net_counter, PDH_FMT_DOUBLE, &bytes, &count, g_net_buffer);
    if (status == PDH_MORE_DATA) {
        void* grown = realloc(g_net_buffer, bytes);
        if (grown == nullptr) {
            return false;
        }
        g_net_buffer = static_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(grown);
        g_net_buffer_bytes = bytes;
        status = PdhGetFormattedCounterArrayW(g_net_counter, PDH_FMT_DOUBLE, &bytes, &count, g_net_buffer);
    }
    if (status != ERROR_SUCCESS) {
        return false;
    }
    double sum = 0.0;
    for (DWORD i = 0; i < count; i++) {
        DWORD item_status = g_net_buffer[i].FmtValue.CStatus;
        if (item_status == PDH_CSTATUS_VALID_DATA || item_status == PDH_CSTATUS_NEW_DATA) {
            sum += g_net_buffer[i].FmtValue.doubleValue;
        }
    }
    *total = sum;
    return true;
}

bool pdh_read(PdhValues* out) {
    if (g_query == nullptr || out == nullptr) {
        return false;
    }
    *out = PdhValues{};
    PDH_FMT_COUNTERVALUE value;
    if (PdhGetFormattedCounterValue(g_cpu_counter, PDH_FMT_DOUBLE, nullptr, &value) != ERROR_SUCCESS) {
        return false;
    }
    out->cpu_percent = value.doubleValue;
    if (PdhGetFormattedCounterValue(g_mem_counter, PDH_FMT_DOUBLE, nullptr, &value) != ERROR_SUCCESS) {
        return false;
    }
    out->available_memory_mb = value.doubleValue;
    if (PdhGetFormattedCounterValue(g_disk_counter, PDH_FMT_DOUBLE, nullptr, &value) != ERROR_SUCCESS) {
        return false;
    }
    out->disk_percent = value.doubleValue;
    return read_network_total(&out->network_bytes_per_sec);
}

void pdh_shutdown() {
    if (g_query != nullptr) {
        PdhCloseQuery(g_query);
        g_query = nullptr;
        g_cpu_counter = nullptr;
        g_mem_counter = nullptr;
        g_disk_counter = nullptr;
        g_net_counter = nullptr;
        free(g_net_buffer);
        g_net_buffer = nullptr;
        g_net_buffer_bytes = 0;
    }
}
