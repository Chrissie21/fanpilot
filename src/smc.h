#ifndef FANPILOT_SMC_H
#define FANPILOT_SMC_H

#include <IOKit/IOKitLib.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t key;
    uint32_t type;
    uint32_t size;
    uint8_t attributes;
    char name[5];
} SmcKey;

typedef struct {
    io_connect_t connection;
    kern_return_t error;
    uint8_t result;
} Smc;

bool smc_open(Smc *smc);
void smc_close(Smc *smc);
bool smc_key(Smc *smc, const char *name, SmcKey *key);
bool smc_read(Smc *smc, const SmcKey *key, double *value);
bool smc_write(Smc *smc, const SmcKey *key, double value);
double smc_decode(uint32_t type, uint32_t size, const uint8_t bytes[32]);
bool smc_encode(uint32_t type, uint32_t size, double value, uint8_t bytes[32]);
uint32_t smc_fourcc(const char *name);

#endif
