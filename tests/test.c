#include "config.h"
#include "smc.h"
#include <assert.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static unsigned checks;

static void check(bool condition) {
    checks++;
    if (!condition) { fprintf(stderr, "Check %u failed.\n", checks); abort(); }
}

static Sample sample(double temperature, double load) {
    return (Sample){.temperature = temperature, .die_temperature = temperature + 10,
                    .cpu = load, .gpu = 0, .elapsed = 2, .sensors_valid = true};
}

static void curves(void) {
    Config c;
    config_defaults(&c);
    check(config_valid(&c));
    for (unsigned k = 0; k < 3; k++) {
        Curve *curve = &c.curves[k];
        for (size_t i = 0; i < curve->count; i++)
            check(fabs(curve_value(curve, curve->points[i].temperature) -
                       curve->points[i].fraction) < 1e-12);
        double previous = 0;
        for (double t = 0; t <= 130; t += .05) {
            double value = curve_value(curve, t);
            check(value >= 0 && value <= 1 && value >= previous - 1e-12);
            previous = value;
        }
        check(curve_value(curve, NAN) == 1);
    }
    check(fan_target(0, 2160, 5927) == 2160);
    check(fan_target(1, 2160, 5927) == 5927);
    check(fan_target(.75, 2000, 5489) == 4117);
    c.curves[0].points[1].temperature = 41;
    check(!config_valid(&c));
    config_defaults(&c);
    c.curves[2].points[4].fraction = .8;
    check(!config_valid(&c));
    config_defaults(&c);
    c.emergency_temperature = NAN;
    check(!config_valid(&c));
}

static void transitions(void) {
    Config c;
    config_defaults(&c);
    Policy p = {0};
    for (unsigned i = 0; i < 30; i++) policy_step(&p, &c, sample(45, .1));
    check(p.mode == BALANCED && !p.emergency);
    policy_step(&p, &c, sample(45, .6));
    check(p.mode == BALANCED);
    for (unsigned i = 0; i < 20; i++) policy_step(&p, &c, sample(45, .6));
    check(p.mode == PERFORMANCE);
    for (unsigned i = 0; i < 20; i++) policy_step(&p, &c, sample(55, .95));
    check(p.mode == EXTREME);
    for (unsigned i = 0; i < 100; i++) policy_step(&p, &c, sample(74, .05));
    check(p.mode == EXTREME);
    for (unsigned i = 0; i < 20; i++) policy_step(&p, &c, sample(50, .05));
    check(p.mode == EXTREME);
    for (unsigned i = 0; i < 100; i++) policy_step(&p, &c, sample(50, .05));
    check(p.mode == BALANCED);
    p = (Policy){0};
    Sample gpu = sample(45, .05);
    gpu.gpu = .95;
    for (unsigned i = 0; i < 15; i++) policy_step(&p, &c, gpu);
    check(p.mode == BALANCED);
    p = (Policy){0};
    gpu.temperature = 56;
    for (unsigned i = 0; i < 5; i++) policy_step(&p, &c, gpu);
    check(p.mode == EXTREME);
    check(p.gpu_active);
    gpu.temperature = 54;
    policy_step(&p, &c, gpu);
    check(p.gpu_active);
    gpu.temperature = 52;
    policy_step(&p, &c, gpu);
    check(!p.gpu_active);
    p = (Policy){0};
    policy_step(&p, &c, sample(66, .05));
    check(p.mode == PERFORMANCE);
    policy_step(&p, &c, sample(78, .05));
    check(p.mode == EXTREME);
    double held = p.fraction;
    policy_step(&p, &c, sample(77.5, .05));
    check(p.fraction >= held - c.ramp_down * 2 - 1e-12);
    Sample wake = sample(45, .01);
    wake.elapsed = 3600;
    policy_step(&p, &c, wake);
    check(p.mode == EXTREME && p.downgrade_seconds <= c.interval);
}

static void emergencies(void) {
    Config c;
    config_defaults(&c);
    Policy p = {0};
    check(policy_step(&p, &c, sample(85, .01)) == 1 && p.emergency);
    for (unsigned i = 0; i < 10; i++) policy_step(&p, &c, sample(65, .01));
    check(p.emergency && p.fraction == 1);
    policy_step(&p, &c, sample(84, .01));
    check(p.emergency && p.recovery_seconds == 0);
    for (unsigned i = 0; i < 15; i++) policy_step(&p, &c, sample(65, .01));
    check(!p.emergency && p.mode == EXTREME);
    Sample fault = sample(50, .01);
    fault.sensors_valid = false;
    check(policy_step(&p, &c, fault) == 1 && p.emergency);
    p = (Policy){0};
    fault = sample(50, .01);
    fault.die_temperature = 95;
    check(policy_step(&p, &c, fault) == 1 && p.emergency);
    fault = sample(NAN, .01);
    check(policy_step(&p, &c, fault) == 1);
    fault = sample(0, .01);
    check(policy_step(&p, &c, fault) == 1);
    p = (Policy){0};
    for (unsigned i = 0; i < 100; i++) {
        double before = p.fraction;
        double after = policy_step(&p, &c, sample(60, .6));
        check(after - before <= c.ramp_up * 2 + 1e-12);
        check(before - after <= c.ramp_down * 2 + 1e-12);
    }
}

static void encoding(void) {
    uint8_t bytes[32];
    check(smc_fourcc("F0Mn") == 0x46304d6e);
    for (double rpm = 0; rpm < 12000; rpm += .25) {
        check(smc_encode(smc_fourcc("fpe2"), 2, rpm, bytes));
        check(smc_decode(smc_fourcc("fpe2"), 2, bytes) == rpm);
    }
    check(!smc_encode(smc_fourcc("fpe2"), 2, NAN, bytes));
    check(!smc_encode(smc_fourcc("fpe2"), 2, 16384, bytes));
    check(!smc_encode(smc_fourcc("fpe2"), 2, -1, bytes));
    check(!smc_encode(smc_fourcc("ui16"), 2, 1, bytes));
    check(smc_encode(smc_fourcc("flt "), 4, 5927, bytes));
    check(smc_decode(smc_fourcc("flt "), 4, bytes) == 5927);
    bytes[0] = 0xff; bytes[1] = 0x80;
    check(smc_decode(smc_fourcc("sp78"), 2, bytes) == -.5);
    check(isnan(smc_decode(smc_fourcc("xxxx"), 2, bytes)));
    check(isnan(smc_decode(smc_fourcc("ui32"), 32, bytes)));
}

static void configurations(void) {
    Config c;
    check(config_load("config/fanpilot.plist", &c, false));
    check(c.curves[0].points[0].temperature == 41);
    char path[] = "/tmp/fanpilot-tests.XXXXXX";
    int fd = mkstemp(path);
    check(fd >= 0);
    close(fd);
    const char *invalid[] = {
        "<plist><array/></plist>",
        "<plist><dict><key>Typo</key><real>2</real></dict></plist>",
        "<plist><dict><key>Interval</key><real>0</real></dict></plist>",
        "<plist><dict><key>Interval</key><string>2</string></dict></plist>",
        "<plist><dict><key>Curves</key><array/></dict></plist>",
        "<plist><dict><key>Curves</key><array><string>x</string><array/><array/></array></dict></plist>",
        "not a plist"
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(*invalid); i++) {
        FILE *file = fopen(path, "w");
        check(file != NULL);
        fputs(invalid[i], file);
        fclose(file);
        check(!config_load(path, &c, false));
    }
    CFMutableDictionaryRef dict = CFDictionaryCreateMutable(NULL, 0,
                        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    check(plist_write(path, dict, 0600));
    check(config_load(path, &c, false));
    struct stat st;
    check(stat(path, &st) == 0 && (st.st_mode & 0777) == 0600);
    chmod(path, 0666);
    check(!config_load(path, &c, true));
    unlink(path);
    check(symlink("config/fanpilot.plist", path) == 0);
    check(!config_load(path, &c, false));
    unlink(path);
    CFRelease(dict);
}

int main(void) {
    curves();
    transitions();
    emergencies();
    encoding();
    configurations();
    printf("%u checks passed.\n", checks);
    return 0;
}
