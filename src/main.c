#include "config.h"
#include "telemetry.h"
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#define CONFIG_PATH "/Library/Application Support/Fanpilot/config.plist"
#define BASELINE_PATH "/var/db/fanpilot/baseline.plist"
#define STATUS_PATH "/var/run/fanpilot/status.json"
#define LOCK_PATH "/var/run/fanpilot/lock"

static volatile sig_atomic_t stopping;
static volatile sig_atomic_t reloading;

static void signal_received(int signal) {
    if (signal == SIGHUP) reloading = 1;
    else stopping = 1;
}

static bool private_directory(const char *path, mode_t mode) {
    if (mkdir(path, mode) && errno != EEXIST) return false;
    struct stat st;
    return lstat(path, &st) == 0 && S_ISDIR(st.st_mode) &&
           st.st_uid == 0 && !(st.st_mode & 022);
}

static bool baseline(Hardware *h, bool create) {
    CFPropertyListRef plist = plist_read(BASELINE_PATH, true);
    if (plist) {
        bool ok = CFGetTypeID(plist) == CFDictionaryGetTypeID();
        CFStringRef model = CFStringCreateWithCString(NULL, h->model, kCFStringEncodingUTF8);
        if (ok) {
            CFTypeRef saved_model = CFDictionaryGetValue(plist, CFSTR("Model"));
            CFTypeRef minima = CFDictionaryGetValue(plist, CFSTR("Minima"));
            CFTypeRef maxima = CFDictionaryGetValue(plist, CFSTR("Maxima"));
            ok = saved_model && model && CFEqual(saved_model, model) && minima && maxima &&
                 CFGetTypeID(minima) == CFArrayGetTypeID() &&
                 CFGetTypeID(maxima) == CFArrayGetTypeID() &&
                 CFArrayGetCount(minima) == (CFIndex)h->fan_count &&
                 CFArrayGetCount(maxima) == (CFIndex)h->fan_count;
            double restored[MAX_FANS];
            for (size_t i = 0; ok && i < h->fan_count; i++) {
                CFTypeRef low = CFArrayGetValueAtIndex(minima, (CFIndex)i);
                CFTypeRef high = CFArrayGetValueAtIndex(maxima, (CFIndex)i);
                double maximum;
                ok = CFGetTypeID(low) == CFNumberGetTypeID() &&
                     CFGetTypeID(high) == CFNumberGetTypeID() &&
                     CFNumberGetValue(low, kCFNumberDoubleType, &restored[i]) &&
                     CFNumberGetValue(high, kCFNumberDoubleType, &maximum) &&
                     isfinite(restored[i]) && isfinite(maximum) &&
                     fabs(maximum - h->fans[i].maximum) <= 1 &&
                     restored[i] >= 1000 && restored[i] <= maximum;
            }
            if (ok) for (size_t i = 0; i < h->fan_count; i++) h->fans[i].minimum = restored[i];
        }
        if (model) CFRelease(model);
        CFRelease(plist);
        return ok;
    }
    if (!create || access(BASELINE_PATH, F_OK) == 0 || errno != ENOENT) return false;
    CFMutableDictionaryRef dict = CFDictionaryCreateMutable(NULL, 0,
                            &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFMutableArrayRef minima = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
    CFMutableArrayRef maxima = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
    CFStringRef model = CFStringCreateWithCString(NULL, h->model, kCFStringEncodingUTF8);
    for (size_t i = 0; i < h->fan_count; i++) {
        CFNumberRef low = CFNumberCreate(NULL, kCFNumberDoubleType, &h->fans[i].minimum);
        CFNumberRef high = CFNumberCreate(NULL, kCFNumberDoubleType, &h->fans[i].maximum);
        CFArrayAppendValue(minima, low);
        CFArrayAppendValue(maxima, high);
        CFRelease(low);
        CFRelease(high);
    }
    CFDictionarySetValue(dict, CFSTR("Model"), model);
    CFDictionarySetValue(dict, CFSTR("Minima"), minima);
    CFDictionarySetValue(dict, CFSTR("Maxima"), maxima);
    bool ok = plist_write(BASELINE_PATH, dict, 0600);
    CFRelease(model);
    CFRelease(minima);
    CFRelease(maxima);
    CFRelease(dict);
    if (ok) {
        int fd = open("/var/db/fanpilot", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        ok = fd >= 0 && fsync(fd) == 0;
        if (fd >= 0) close(fd);
    }
    return ok;
}

static void json_number(FILE *file, double number) {
    if (isfinite(number)) fprintf(file, "%.3f", number);
    else fputs("null", file);
}

static void write_status(FILE *file, Hardware *h, const Sample *s, const Policy *p,
                         const double *targets, unsigned long samples, double elapsed,
                         bool controlling) {
    struct rusage usage;
    getrusage(RUSAGE_SELF, &usage);
    double cpu = usage.ru_utime.tv_sec + usage.ru_utime.tv_usec / 1e6 +
                 usage.ru_stime.tv_sec + usage.ru_stime.tv_usec / 1e6;
    fprintf(file, "{\"pid\":%d,\"timestamp\":%lld,\"controlling\":%s,\"mode\":\"%s\","
            "\"emergency\":%s,\"sensors_valid\":%s,\"temperature_c\":", getpid(),
            (long long)time(NULL), controlling ? "true" : "false", mode_name(p->mode),
            p->emergency ? "true" : "false", s->sensors_valid ? "true" : "false");
    json_number(file, s->temperature);
    fputs(",\"die_temperature_c\":", file);
    json_number(file, s->die_temperature);
    fputs(",\"cpu_load\":", file);
    json_number(file, s->cpu);
    fputs(",\"gpu_load\":", file);
    json_number(file, s->gpu);
    fprintf(file, ",\"fraction\":%.4f,\"samples\":%lu,\"uptime_seconds\":%.1f,"
            "\"process_cpu_seconds\":%.6f,\"peak_rss_bytes\":%ld,\"fans\":[",
            p->fraction, samples, elapsed, cpu, usage.ru_maxrss);
    for (size_t i = 0; i < h->fan_count; i++) {
        Fan *fan = &h->fans[i];
        double actual = NAN;
        smc_read(&h->smc, &fan->actual_key, &actual);
        fprintf(file, "%s{\"id\":%zu,\"minimum_rpm\":%.0f,\"maximum_rpm\":%.0f,"
                "\"requested_minimum_rpm\":%.0f,\"actual_rpm\":", i ? "," : "", i,
                fan->minimum, fan->maximum, targets[i]);
        json_number(file, actual);
        fputc('}', file);
    }
    fputs("]}\n", file);
}

static bool publish_status(Hardware *h, Sample *s, Policy *p, double *targets,
                           unsigned long samples, double elapsed) {
    char path[] = STATUS_PATH ".XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) return false;
    if (fchmod(fd, 0644)) { close(fd); unlink(path); return false; }
    FILE *file = fdopen(fd, "w");
    if (!file) { close(fd); unlink(path); return false; }
    write_status(file, h, s, p, targets, samples, elapsed, true);
    bool ok = !ferror(file);
    if (fclose(file)) ok = false;
    if (ok) ok = rename(path, STATUS_PATH) == 0;
    if (!ok) unlink(path);
    return ok;
}

static int status_command(void) {
    int fd = open(STATUS_PATH, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) || !S_ISREG(st.st_mode) ||
        st.st_uid != 0 || st.st_size > 8192 || (st.st_mode & 022)) {
        if (fd >= 0) close(fd);
        fputs("Fanpilot status is unavailable.\n", stderr);
        return 1;
    }
    char data[8193];
    ssize_t n = read(fd, data, 8192);
    close(fd);
    if (n <= 0) return 1;
    data[n] = 0;
    int pid = 0;
    bool alive = sscanf(data, "{\"pid\":%d,", &pid) == 1 && pid > 1;
    if (alive && kill((pid_t)pid, 0) && errno != EPERM) alive = false;
    if (!alive || time(NULL) - st.st_mtime > 30) {
        fputs("Status is stale; check launchctl print system/local.fanpilot.\n", stderr);
        return 1;
    }
    fputs(data, stdout);
    return 0;
}

static int probe(Hardware *h) {
    char conflict[128] = "none";
    controller_conflict(conflict, sizeof(conflict));
    printf("model=%s automatic=%s conflict=%s\ncpu_sensor=%s gpu_sensor=%s die_sensor=%s\n",
           h->model, hardware_automatic(h) ? "yes" : "no", conflict,
           h->cpu_key.name, h->has_gpu ? h->gpu_key.name : "unavailable", h->die_key.name);
    for (size_t i = 0; i < h->fan_count; i++)
        printf("fan=%zu minimum=%.0f maximum=%.0f actual=%.0f\n", i,
               h->fans[i].minimum, h->fans[i].maximum, h->fans[i].actual);
    Sample sample = {0};
    hardware_sample(h, &sample, monotonic_seconds());
    printf("sensors_valid=%s temperature=%.2f die=%.2f gpu_load=%.3f\n",
           sample.sensors_valid ? "yes" : "no", sample.temperature,
           sample.die_temperature, sample.gpu);
    return sample.sensors_valid ? 0 : 1;
}

static int loop(Hardware *h, const char *path, Config config, bool control, double duration) {
    int lock = -1;
    if (control) {
        if (!private_directory("/var/db/fanpilot", 0700) ||
            !private_directory("/var/run/fanpilot", 0755)) {
            fputs("Cannot create secure runtime directories.\n", stderr);
            return 1;
        }
        lock = open(LOCK_PATH, O_CREAT | O_RDWR | O_NOFOLLOW | O_CLOEXEC, 0600);
        struct stat st;
        if (lock < 0 || fstat(lock, &st) || !S_ISREG(st.st_mode) || st.st_uid != 0 ||
            (st.st_mode & 022) || flock(lock, LOCK_EX | LOCK_NB)) {
            fputs("Cannot acquire exclusive controller lock.\n", stderr);
            if (lock >= 0) close(lock);
            return 1;
        }
        char conflict[128];
        if (controller_conflict(conflict, sizeof(conflict)) || !hardware_automatic(h)) {
            fprintf(stderr, "Fan control is occupied. Quit other fan controllers and restore automatic mode.\n");
            close(lock);
            return 1;
        }
        if (!baseline(h, true)) {
            fputs("Cannot save or validate original fan minimums.\n", stderr);
            close(lock);
            return 1;
        }
    }
    struct sigaction action = {.sa_handler = signal_received};
    sigemptyset(&action.sa_mask);
    sigaction(SIGTERM, &action, NULL);
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGHUP, &action, NULL);
    setvbuf(stdout, NULL, _IOLBF, 0);
    openlog("fanpilot", LOG_PID | LOG_NDELAY, LOG_DAEMON);
    Policy policy = {0};
    unsigned long samples = 0;
    double start = monotonic_seconds(), previous = start;
    double report_at = 0, conflict_at = start + 15, attest_at = 0;
    bool changed_hardware = false;
    int result = 0;
    while (!stopping) {
        double now = monotonic_seconds();
        if (!control && samples && now - start >= duration) break;
        if (reloading) {
            reloading = 0;
            Config replacement;
            if (config_load(path, &replacement, control)) {
                config = replacement;
                policy.candidate_seconds = policy.downgrade_seconds = 0;
                syslog(LOG_NOTICE, "configuration reloaded");
            } else syslog(LOG_ERR, "configuration rejected; retaining current configuration");
        }
        Sample sample = {.elapsed = samples ? now - previous : config.interval};
        hardware_sample(h, &sample, now);
        previous = now;
        Mode old_mode = policy.mode;
        bool old_emergency = policy.emergency;
        double fraction = policy_step(&policy, &config, sample);
        double targets[MAX_FANS];
        for (size_t i = 0; i < h->fan_count; i++)
            targets[i] = fan_target(fraction, h->fans[i].minimum, h->fans[i].maximum);
        bool transition = !samples || old_mode != policy.mode || old_emergency != policy.emergency;
        if (control) {
            if (!hardware_automatic(h)) {
                syslog(LOG_ERR, "automatic fan mode lost; relinquishing control");
                result = 1;
                break;
            }
            if (now >= conflict_at) {
                char conflict[128];
                if (controller_conflict(conflict, sizeof(conflict))) {
                    syslog(LOG_ERR, "competing controller: %s; relinquishing control", conflict);
                    result = 1;
                    break;
                }
                conflict_at = now + 15;
            }
            changed_hardware = true;
            if (!hardware_apply(h, targets, now >= attest_at)) {
                syslog(LOG_ERR, "fan minimum write or readback failed: kernel=%x result=%u",
                       h->smc.error, h->smc.result);
                result = 1;
                break;
            }
            if (now >= attest_at) attest_at = now + 60;
        }
        samples++;
        if (transition && control)
            syslog(LOG_NOTICE, "mode=%s emergency=%d temperature=%.1f die=%.1f fraction=%.2f",
                   mode_name(policy.mode), policy.emergency, sample.temperature,
                   sample.die_temperature, fraction);
        if (now >= report_at || transition) {
            if (control) {
                if (!publish_status(h, &sample, &policy, targets, samples, now - start))
                    syslog(LOG_ERR, "status publish failed");
            } else write_status(stdout, h, &sample, &policy, targets, samples, now - start, false);
            report_at = now + 15;
        }
        double interval = policy.mode == BALANCED && !policy.emergency &&
                          policy.load < .15 && sample.temperature < 55 ?
                          config.idle_interval : config.interval;
        if (!control) interval = fmin(interval, duration - (monotonic_seconds() - start));
        double delay = fmax(0, interval - (monotonic_seconds() - now));
        struct timespec wait = {.tv_sec = (time_t)delay,
                               .tv_nsec = (long)((delay - floor(delay)) * 1e9)};
        if (delay > 0) nanosleep(&wait, NULL);
    }
    if (control && changed_hardware) {
        if (!hardware_restore(h)) {
            syslog(LOG_ERR, "original fan minimum restoration failed");
            fputs("Original fan minimum restoration failed.\n", stderr);
            result = 1;
        } else syslog(LOG_NOTICE, "original fan minimums restored");
    }
    if (control) { unlink(STATUS_PATH); close(lock); }
    else {
        struct rusage usage;
        getrusage(RUSAGE_SELF, &usage);
        double cpu = usage.ru_utime.tv_sec + usage.ru_utime.tv_usec / 1e6 +
                     usage.ru_stime.tv_sec + usage.ru_stime.tv_usec / 1e6;
        double elapsed = monotonic_seconds() - start;
        fprintf(stderr, "samples=%lu elapsed=%.2fs cpu=%.6fs average_cpu=%.4f%% peak_rss=%.2fMiB\n",
                samples, elapsed, cpu, elapsed > 0 ? cpu / elapsed * 100 : 0,
                usage.ru_maxrss / 1048576.0);
    }
    closelog();
    return result;
}

static void usage(void) {
    fputs("fanpilot probe\nfanpilot validate [config.plist]\n"
          "fanpilot monitor [seconds] [config.plist]\nfanpilot run [config.plist]\n"
          "fanpilot verify\nfanpilot restore\nfanpilot status\nfanpilot version\n", stderr);
}

int main(int argc, char **argv) {
    if (argc < 2) { usage(); return 64; }
    const char *command = argv[1];
    if (!strcmp(command, "version") && argc == 2) { puts("fanpilot 0.1.0"); return 0; }
    if (!strcmp(command, "status") && argc == 2) return status_command();
    bool monitor = !strcmp(command, "monitor"), run = !strcmp(command, "run");
    bool validate = !strcmp(command, "validate"), verify = !strcmp(command, "verify");
    bool restore = !strcmp(command, "restore"), probing = !strcmp(command, "probe");
    if (!(monitor || run || validate || verify || restore || probing) ||
        argc > (monitor ? 4 : (run || validate) ? 3 : 2)) { usage(); return 64; }
    if ((run || verify || restore) && geteuid() != 0) {
        fputs("This command requires root.\n", stderr);
        return 77;
    }
    double duration = 60;
    if (monitor && argc >= 3) {
        char *end;
        duration = strtod(argv[2], &end);
        if (!*argv[2] || *end || !isfinite(duration) || duration < 2 || duration > 3600) {
            fputs("Monitor duration must be between 2 and 3600 seconds.\n", stderr);
            return 64;
        }
    }
    const char *path = monitor && argc == 4 ? argv[3] :
                       (run || validate) && argc == 3 ? argv[2] : CONFIG_PATH;
    Config config;
    config_defaults(&config);
    if (run || validate || (monitor && argc == 4)) {
        if (!config_load(path, &config, run)) {
            fprintf(stderr, "Configuration is invalid, unreadable, or insecure: %s\n", path);
            return 78;
        }
    }
    if (validate) { puts("Configuration valid."); return 0; }
    Hardware hardware;
    if (!hardware_open(&hardware)) {
        fprintf(stderr, "Unsupported or inaccessible AppleSMC: kernel=%x result=%u\n",
                hardware.smc.error, hardware.smc.result);
        return 69;
    }
    int result;
    if (probing) result = probe(&hardware);
    else if (verify) {
        char conflict[128];
        if (controller_conflict(conflict, sizeof(conflict)) || !hardware_automatic(&hardware)) {
            fputs("Quit other fan controllers and restore automatic mode before verification.\n", stderr);
            result = 1;
        } else {
            double targets[MAX_FANS];
            for (size_t i = 0; i < hardware.fan_count; i++) targets[i] = hardware.fans[i].minimum;
            result = hardware_apply(&hardware, targets, true) ? 0 : 1;
            if (!result) puts("Fan minimum writes and readback verified; automatic mode retained.");
            else fprintf(stderr, "Fan minimum verification failed: kernel=%x result=%u\n",
                         hardware.smc.error, hardware.smc.result);
        }
    } else if (restore) {
        int fd = open(LOCK_PATH, O_RDWR | O_NOFOLLOW | O_CLOEXEC);
        bool locked = fd >= 0 && flock(fd, LOCK_EX | LOCK_NB) == 0;
        result = locked && baseline(&hardware, false) && hardware_restore(&hardware) ? 0 : 1;
        if (fd >= 0) close(fd);
        if (result) fputs("Restoration failed or daemon is still running.\n", stderr);
        else puts("Original fan minimums restored.");
    } else result = loop(&hardware, path, config, run, duration);
    hardware_close(&hardware);
    return result;
}
