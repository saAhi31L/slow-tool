#pragma once

struct PdhValues {
    double cpu_percent;
    double available_memory_mb;
    double disk_percent;
    double network_bytes_per_sec;
};

bool pdh_init();
bool pdh_collect();
bool pdh_read(PdhValues* out);
void pdh_shutdown();
