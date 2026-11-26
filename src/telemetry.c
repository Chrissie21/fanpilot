#include "telemetry.h"
#include <CoreFoundation/CoreFoundation.h>
#include <libproc.h>
#include <mach/host_info.h>
#include <mach/mach_host.h>
#include <mach/mach_time.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/sysctl.h>
#include <time.h>
#include <unistd.h>

double monotonic_seconds(void) {
    static mach_timebase_info_data_t scale;
    if (!scale.denom) mach_timebase_info(&scale);
    return (double)mach_continuous_time() * scale.numer / scale.denom / 1e9;
}

static bool discover_temperature(Smc *smc, const char *const *names, size_t count,
                                 SmcKey *key) {
    for (size_t i = 0; i < count; i++) {
        double value;
        if (smc_key(smc, names[i], key) && smc_read(smc, key, &value) &&
            value >= 5 && value <= 125) return true;
    }
    memset(key, 0, sizeof(*key));
    return false;
}

bool hardware_open(Hardware *h) {
    memset(h, 0, sizeof(*h));
    size_t size = sizeof(h->model);
    if (sysctlbyname("hw.model", h->model, &size, NULL, 0)) return false;
    if (!smc_open(&h->smc)) return false;
    SmcKey count_key;
    double count;
    if (!smc_key(&h->smc, "FNum", &count_key) ||
        !smc_read(&h->smc, &count_key, &count) || count < 1 ||
        count > MAX_FANS || count != floor(count)) goto fail;
    h->fan_count = (size_t)count;
    for (size_t i = 0; i < h->fan_count; i++) {
        Fan *f = &h->fans[i];
        char key[5] = "F0Mn";
        key[1] = (char)('0' + i);
        if (!smc_key(&h->smc, key, &f->minimum_key) ||
            !(f->minimum_key.attributes & 0x40) ||
            !smc_read(&h->smc, &f->minimum_key, &f->minimum)) goto fail;
        memcpy(key + 2, "Mx", 2);
        if (!smc_key(&h->smc, key, &f->maximum_key) ||
            !smc_read(&h->smc, &f->maximum_key, &f->maximum)) goto fail;
        memcpy(key + 2, "Ac", 2);
        if (!smc_key(&h->smc, key, &f->actual_key) ||
            !smc_read(&h->smc, &f->actual_key, &f->actual)) goto fail;
        if (f->minimum < 1000 || f->minimum > f->maximum || f->maximum > 12000 ||
            (f->minimum_key.type != smc_fourcc("fpe2") &&
             f->minimum_key.type != smc_fourcc("flt "))) goto fail;
        f->applied = NAN;
    }
    const char *const cpu[] = {"TC0P", "TCXC", "TC0F", "TC0E", "TC0D"};
    const char *const die[] = {"TCXC", "TC0F", "TC0E", "TC0D", "TC0P"};
    const char *const gpu[] = {"TG0P", "TG0D", "TG0F", "TG0T"};
    const char *const gpu_die[] = {"TG0D", "TG0F", "TG0T", "TG0P"};
    if (!discover_temperature(&h->smc, cpu, 5, &h->cpu_key) ||
        !discover_temperature(&h->smc, die, 5, &h->die_key)) goto fail;
    h->has_gpu = discover_temperature(&h->smc, gpu, 4, &h->gpu_key);
    h->has_gpu_die = discover_temperature(&h->smc, gpu_die, 4, &h->gpu_die_key);
    h->has_forced = smc_key(&h->smc, "FS! ", &h->forced_key);
    if (!h->has_forced) goto fail;
    h->host = mach_host_self();
    return true;
fail:
    hardware_close(h);
    return false;
}

void hardware_close(Hardware *h) {
    for (size_t i = 0; i < h->gpu_count; i++) IOObjectRelease(h->gpus[i]);
    if (h->host) mach_port_deallocate(mach_task_self(), h->host);
    smc_close(&h->smc);
    h->gpu_count = 0;
    h->host = 0;
}

static double cpu_usage(Hardware *h) {
    host_cpu_load_info_data_t info;
    mach_msg_type_number_t count = HOST_CPU_LOAD_INFO_COUNT;
    if (host_statistics(h->host, HOST_CPU_LOAD_INFO, (host_info_t)&info, &count) != KERN_SUCCESS)
        return NAN;
    uint64_t total = 0, idle = 0;
    for (unsigned i = 0; i < 4; i++) {
        uint32_t difference = info.cpu_ticks[i] - h->ticks[i];
        total += difference;
        if (i == CPU_STATE_IDLE) idle = difference;
        h->ticks[i] = info.cpu_ticks[i];
    }
    if (!h->ticks_valid) { h->ticks_valid = true; return NAN; }
    return total ? (double)(total - idle) / (double)total : 0;
}

static void refresh_gpus(Hardware *h, double now) {
    for (size_t i = 0; i < h->gpu_count; i++) IOObjectRelease(h->gpus[i]);
    h->gpu_count = 0;
    io_iterator_t iterator = 0;
    if (IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching("IOAccelerator"),
                                    &iterator) == KERN_SUCCESS) {
        io_service_t service;
        while ((service = IOIteratorNext(iterator))) {
            if (h->gpu_count < MAX_GPUS) h->gpus[h->gpu_count++] = service;
            else IOObjectRelease(service);
        }
        IOObjectRelease(iterator);
    }
    h->gpu_refresh = now + 60;
}

static double gpu_usage(Hardware *h, double now) {
    if (now >= h->gpu_refresh) refresh_gpus(h, now);
    double highest = NAN;
    const CFStringRef keys[] = {CFSTR("Device Utilization %"),
        CFSTR("Device Utilization % at cur p-state"), CFSTR("GPU Activity(%)")};
    for (size_t i = 0; i < h->gpu_count; i++) {
        CFTypeRef stats = IORegistryEntryCreateCFProperty(h->gpus[i],
                                CFSTR("PerformanceStatistics"), NULL, 0);
        if (!stats) continue;
        if (CFGetTypeID(stats) == CFDictionaryGetTypeID()) {
            for (size_t k = 0; k < sizeof(keys) / sizeof(*keys); k++) {
                CFTypeRef item = CFDictionaryGetValue(stats, keys[k]);
                double value;
                if (item && CFGetTypeID(item) == CFNumberGetTypeID() &&
                    CFNumberGetValue(item, kCFNumberDoubleType, &value) &&
                    isfinite(value) && value >= 0 && value <= 100)
                    highest = isnan(highest) ? value / 100 : fmax(highest, value / 100);
            }
        }
        CFRelease(stats);
    }
    return highest;
}

static bool temperature(Hardware *h, SmcKey *key, double *value) {
    return smc_read(&h->smc, key, value) && *value >= 5 && *value <= 125;
}

bool hardware_sample(Hardware *h, Sample *s, double now) {
    double cpu = NAN, gpu = NAN, die = NAN, gpu_die = NAN;
    bool ok = temperature(h, &h->cpu_key, &cpu) && temperature(h, &h->die_key, &die);
    if (h->has_gpu) ok = temperature(h, &h->gpu_key, &gpu) && ok;
    if (h->has_gpu_die) ok = temperature(h, &h->gpu_die_key, &gpu_die) && ok;
    s->temperature = fmax(cpu, gpu);
    s->die_temperature = fmax(die, gpu_die);
    s->sensors_valid = ok;
    s->cpu = cpu_usage(h);
    s->gpu = gpu_usage(h, now);
    return ok;
}

bool hardware_automatic(Hardware *h) {
    double forced;
    return h->has_forced && smc_read(&h->smc, &h->forced_key, &forced) && forced == 0;
}

bool hardware_apply(Hardware *h, const double *targets, bool force) {
    for (size_t i = 0; i < h->fan_count; i++)
        if (!isfinite(targets[i]) || targets[i] < h->fans[i].minimum ||
            targets[i] > h->fans[i].maximum) return false;
    for (size_t i = 0; i < h->fan_count; i++) {
        Fan *f = &h->fans[i];
        double requested = targets[i], existing;
        if (!smc_read(&h->smc, &f->minimum_key, &existing)) return false;
        if (force || fabs(existing - requested) >= 40 ||
            (requested == f->maximum && fabs(existing - requested) > .5) ||
            (requested == f->minimum && fabs(existing - requested) > .5)) {
            if (!smc_write(&h->smc, &f->minimum_key, requested) ||
                !smc_read(&h->smc, &f->minimum_key, &existing) ||
                fabs(existing - requested) > 1) return false;
        }
        f->applied = existing;
    }
    return true;
}

bool hardware_restore(Hardware *h) {
    bool ok = true;
    for (size_t i = 0; i < h->fan_count; i++) {
        Fan *f = &h->fans[i];
        double value;
        if (!smc_write(&h->smc, &f->minimum_key, f->minimum) ||
            !smc_read(&h->smc, &f->minimum_key, &value) ||
            fabs(value - f->minimum) > 1) ok = false;
    }
    return ok;
}

bool controller_conflict(char *name, size_t size) {
    pid_t pids[4096];
    int bytes = proc_listpids(PROC_ALL_PIDS, 0, pids, sizeof(pids));
    if (bytes <= 0 || (size_t)bytes >= sizeof(pids)) {
        snprintf(name, size, "process inventory unavailable");
        return true;
    }
    const char *const names[] = {"FanBar", "Macs Fan Control", "smcFanControl", "TG Pro"};
    for (size_t i = 0; i < (size_t)bytes / sizeof(pid_t); i++) {
        if (pids[i] <= 0 || pids[i] == getpid()) continue;
        char path[PROC_PIDPATHINFO_MAXSIZE];
        if (proc_pidpath(pids[i], path, sizeof(path)) <= 0) continue;
        const char *base = strrchr(path, '/');
        base = base ? base + 1 : path;
        for (size_t k = 0; k < sizeof(names) / sizeof(*names); k++)
            if (!strcmp(base, names[k])) { snprintf(name, size, "%s", base); return true; }
    }
    return false;
}
