#include "config.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>

CFPropertyListRef plist_read(const char *path, bool secure) {
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size <= 0 ||
        st.st_size > 65536 || (secure && (st.st_uid != 0 || (st.st_mode & 022)))) {
        close(fd);
        return NULL;
    }
    unsigned char bytes[65536];
    size_t used = 0, length = (size_t)st.st_size;
    while (used < length) {
        ssize_t n = read(fd, bytes + used, length - used);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        used += (size_t)n;
    }
    close(fd);
    if (used != length) return NULL;
    CFDataRef data = CFDataCreate(NULL, bytes, (CFIndex)used);
    if (!data) return NULL;
    CFPropertyListRef plist = CFPropertyListCreateWithData(NULL, data,
                                     kCFPropertyListImmutable, NULL, NULL);
    CFRelease(data);
    return plist;
}

bool plist_write(const char *path, CFPropertyListRef value, unsigned permissions) {
    CFDataRef data = CFPropertyListCreateData(NULL, value, kCFPropertyListBinaryFormat_v1_0, 0, NULL);
    if (!data) return false;
    char temporary[1024];
    int count = snprintf(temporary, sizeof(temporary), "%s.XXXXXX", path);
    if (count < 0 || (size_t)count >= sizeof(temporary)) { CFRelease(data); return false; }
    int fd = mkstemp(temporary);
    bool ok = fd >= 0;
    if (ok) ok = fchmod(fd, (mode_t)permissions) == 0;
    const UInt8 *bytes = CFDataGetBytePtr(data);
    size_t length = (size_t)CFDataGetLength(data), used = 0;
    while (ok && used < length) {
        ssize_t n = write(fd, bytes + used, length - used);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { ok = false; break; }
        used += (size_t)n;
    }
    if (ok) ok = fsync(fd) == 0;
    if (fd >= 0 && close(fd)) ok = false;
    if (ok) ok = rename(temporary, path) == 0;
    if (!ok) unlink(temporary);
    CFRelease(data);
    return ok;
}

static bool number(CFDictionaryRef dict, CFStringRef key, double *target) {
    CFTypeRef item = CFDictionaryGetValue(dict, key);
    return item && CFGetTypeID(item) == CFNumberGetTypeID() &&
           CFNumberGetValue(item, kCFNumberDoubleType, target);
}

bool config_load(const char *path, Config *out, bool secure) {
    Config config;
    config_defaults(&config);
    CFPropertyListRef plist = plist_read(path, secure);
    if (!plist) return false;
    bool ok = CFGetTypeID(plist) == CFDictionaryGetTypeID();
    if (!ok) { CFRelease(plist); return false; }
    CFDictionaryRef dict = (CFDictionaryRef)plist;
    struct { CFStringRef key; double *value; } fields[] = {
        {CFSTR("Interval"), &config.interval}, {CFSTR("IdleInterval"), &config.idle_interval},
        {CFSTR("PerformanceLoad"), &config.performance_load},
        {CFSTR("ExtremeLoad"), &config.extreme_load},
        {CFSTR("GPUActivationTemperature"), &config.gpu_activation_temperature},
        {CFSTR("PerformanceTemperature"), &config.performance_temperature},
        {CFSTR("ExtremeTemperature"), &config.extreme_temperature},
        {CFSTR("EmergencyTemperature"), &config.emergency_temperature},
        {CFSTR("DieEmergencyTemperature"), &config.die_emergency_temperature},
        {CFSTR("UpgradeSeconds"), &config.upgrade_seconds},
        {CFSTR("DowngradeSeconds"), &config.downgrade_seconds},
        {CFSTR("Hysteresis"), &config.hysteresis},
        {CFSTR("RampUpPerSecond"), &config.ramp_up},
        {CFSTR("RampDownPerSecond"), &config.ramp_down}
    };
    CFIndex count = CFDictionaryGetCount(dict);
    const void *keys[64];
    if (count > 64) ok = false;
    if (ok) {
        CFDictionaryGetKeysAndValues(dict, keys, NULL);
        for (CFIndex i = 0; i < count; i++) {
            bool known = CFEqual(keys[i], CFSTR("Curves"));
            for (size_t k = 0; k < sizeof(fields) / sizeof(*fields); k++)
                known = known || CFEqual(keys[i], fields[k].key);
            if (!known) ok = false;
        }
    }
    for (size_t i = 0; ok && i < sizeof(fields) / sizeof(*fields); i++)
        if (CFDictionaryContainsKey(dict, fields[i].key))
            ok = number(dict, fields[i].key, fields[i].value);
    CFTypeRef curves = CFDictionaryGetValue(dict, CFSTR("Curves"));
    if (ok && curves) {
        ok = CFGetTypeID(curves) == CFArrayGetTypeID() && CFArrayGetCount(curves) == 3;
        for (CFIndex k = 0; ok && k < 3; k++) {
            CFTypeRef points = CFArrayGetValueAtIndex(curves, k);
            ok = CFGetTypeID(points) == CFArrayGetTypeID();
            if (!ok) break;
            CFIndex n = CFArrayGetCount(points);
            ok = n >= 2 && n <= MAX_POINTS;
            config.curves[k].count = (size_t)n;
            for (CFIndex i = 0; ok && i < n; i++) {
                CFTypeRef point = CFArrayGetValueAtIndex(points, i);
                ok = CFGetTypeID(point) == CFDictionaryGetTypeID();
                if (ok) ok = CFDictionaryGetCount(point) == 2 &&
                    number(point, CFSTR("Temperature"), &config.curves[k].points[i].temperature) &&
                    number(point, CFSTR("Fraction"), &config.curves[k].points[i].fraction);
            }
        }
    }
    CFRelease(plist);
    if (!ok || !config_valid(&config)) return false;
    *out = config;
    return true;
}
