#pragma once

#include <windows.h>

struct TopProcess {
    DWORD pid;
    wchar_t name[MAX_PATH];
    double cpu_percent;
};

bool th32_init();
bool th32_top_process(TopProcess* out);
