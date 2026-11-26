#ifndef FANPILOT_TELEMETRY_H
#define FANPILOT_TELEMETRY_H

#include "smc.h"
#include "policy.h"
#include <mach/mach.h>

#define MAX_FANS 4
#define MAX_GPUS 8

typedef struct {
    SmcKey minimum_key, maximum_key, actual_key;
    double minimum, maximum, actual, applied;
} Fan;
typedef struct {
    Smc smc;
    Fan fans[MAX_FANS];
    size_t fan_count;
    SmcKey cpu_key, gpu_key, die_key, gpu_die_key, forced_key;
    bool has_gpu, has_gpu_die, has_forced;
    mach_port_t host;
    uint32_t ticks[4];
    bool ticks_valid;
    io_service_t gpus[MAX_GPUS];
    size_t gpu_count;
    double gpu_refresh;
    char model[64];
} Hardware;

bool hardware_open(Hardware *hardware);
void hardware_close(Hardware *hardware);
bool hardware_sample(Hardware *hardware, Sample *sample, double now);
bool hardware_automatic(Hardware *hardware);
bool hardware_apply(Hardware *hardware, const double *targets, bool force);
bool hardware_restore(Hardware *hardware);
bool controller_conflict(char *name, size_t size);
double monotonic_seconds(void);

#endif
