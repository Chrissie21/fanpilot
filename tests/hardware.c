#include "telemetry.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static double minima[MAX_FANS];
static unsigned writes;
static int failed_fan = -1;
static bool bad_readback;
static double forced;
static unsigned checks;

static void check(bool condition) {
    checks++;
    if (!condition) { fprintf(stderr, "Hardware check %u failed.\n", checks); abort(); }
}

uint32_t smc_fourcc(const char *name) {
    return (uint32_t)(unsigned char)name[0] << 24 |
           (uint32_t)(unsigned char)name[1] << 16 |
           (uint32_t)(unsigned char)name[2] << 8 | (unsigned char)name[3];
}

bool smc_open(Smc *smc) { (void)smc; return false; }
void smc_close(Smc *smc) { (void)smc; }
bool smc_key(Smc *smc, const char *name, SmcKey *key) {
    (void)smc; (void)name; (void)key;
    return false;
}

bool smc_read(Smc *smc, const SmcKey *key, double *value) {
    (void)smc;
    if (!strcmp(key->name, "FS! ")) { *value = forced; return true; }
    unsigned index = (unsigned)(key->name[1] - '0');
    if (index >= MAX_FANS) return false;
    *value = minima[index] + (bad_readback && writes ? 100 : 0);
    return true;
}

bool smc_write(Smc *smc, const SmcKey *key, double value) {
    (void)smc;
    check(key->name[0] == 'F' && key->name[2] == 'M' && key->name[3] == 'n');
    int index = key->name[1] - '0';
    if (index == failed_fan) return false;
    check(index >= 0 && index < MAX_FANS && isfinite(value) && value >= 2000 && value <= 6000);
    minima[index] = value;
    writes++;
    return true;
}

static Hardware fixture(void) {
    writes = 0;
    failed_fan = -1;
    bad_readback = false;
    forced = 0;
    Hardware h = {.fan_count = 2, .has_forced = true};
    memcpy(h.forced_key.name, "FS! ", 5);
    for (size_t i = 0; i < h.fan_count; i++) {
        minima[i] = 2000;
        h.fans[i].minimum = 2000;
        h.fans[i].maximum = 6000;
        snprintf(h.fans[i].minimum_key.name, 5, "F%zuMn", i);
    }
    return h;
}

int main(void) {
    Hardware h = fixture();
    double targets[] = {3000, 4000};
    check(hardware_automatic(&h));
    forced = 3;
    check(!hardware_automatic(&h));
    forced = 0;
    check(hardware_apply(&h, targets, false));
    check(minima[0] == 3000 && minima[1] == 4000 && writes == 2);
    check(hardware_apply(&h, targets, false) && writes == 2);
    targets[0] += 20;
    check(hardware_apply(&h, targets, false) && writes == 2);
    check(hardware_apply(&h, targets, true) && writes == 4);
    check(hardware_restore(&h) && minima[0] == 2000 && minima[1] == 2000);
    h = fixture();
    double invalid[] = {3000, NAN};
    check(!hardware_apply(&h, invalid, false) && writes == 0);
    invalid[1] = 1000;
    check(!hardware_apply(&h, invalid, false) && writes == 0);
    invalid[1] = 6001;
    check(!hardware_apply(&h, invalid, false) && writes == 0);
    failed_fan = 1;
    check(!hardware_apply(&h, targets, true) && minima[0] == 3020 && minima[1] == 2000);
    failed_fan = -1;
    check(hardware_restore(&h) && minima[0] == 2000 && minima[1] == 2000);
    h = fixture();
    bad_readback = true;
    check(!hardware_apply(&h, targets, true));
    bad_readback = false;
    check(hardware_restore(&h));
    failed_fan = 0;
    check(!hardware_restore(&h));
    h = fixture();
    targets[0] = targets[1] = 6000;
    check(hardware_apply(&h, targets, false) && minima[0] == 6000 && minima[1] == 6000);
    targets[0] = targets[1] = 2000;
    check(hardware_apply(&h, targets, false) && minima[0] == 2000 && minima[1] == 2000);
    printf("%u hardware checks passed.\n", checks);
    return 0;
}
