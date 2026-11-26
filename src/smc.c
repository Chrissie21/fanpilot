#include "smc.h"
#include <math.h>
#include <stddef.h>
#include <string.h>

typedef struct {
    uint32_t key;
    uint8_t version[6];
    uint8_t version_padding[2];
    uint16_t power_version;
    uint16_t power_length;
    uint32_t cpu_limit;
    uint32_t gpu_limit;
    uint32_t memory_limit;
    uint32_t size;
    uint32_t type;
    uint8_t attributes;
    uint8_t metadata_padding[3];
    uint8_t result;
    uint8_t status;
    uint8_t command;
    uint32_t index;
    uint8_t bytes[32];
} SmcPacket;

_Static_assert(sizeof(SmcPacket) == 80, "SMC packet size");
_Static_assert(offsetof(SmcPacket, size) == 28, "SMC metadata offset");
_Static_assert(offsetof(SmcPacket, command) == 42, "SMC command offset");

uint32_t smc_fourcc(const char *name) {
    return (uint32_t)(uint8_t)name[0] << 24 |
           (uint32_t)(uint8_t)name[1] << 16 |
           (uint32_t)(uint8_t)name[2] << 8 | (uint8_t)name[3];
}

static bool exchange(Smc *smc, const SmcPacket *request, SmcPacket *reply) {
    memset(reply, 0, sizeof(*reply));
    size_t size = sizeof(*reply);
    smc->error = IOConnectCallStructMethod(smc->connection, 2, request,
                                         sizeof(*request), reply, &size);
    smc->result = reply->result;
    return smc->error == KERN_SUCCESS && size == sizeof(*reply) && reply->result == 0;
}

bool smc_open(Smc *smc) {
    memset(smc, 0, sizeof(*smc));
    io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault,
                                                      IOServiceMatching("AppleSMC"));
    if (!service) return false;
    smc->error = IOServiceOpen(service, mach_task_self(), 0, &smc->connection);
    IOObjectRelease(service);
    return smc->error == KERN_SUCCESS;
}

void smc_close(Smc *smc) {
    if (smc->connection) IOServiceClose(smc->connection);
    smc->connection = 0;
}

bool smc_key(Smc *smc, const char *name, SmcKey *key) {
    if (strlen(name) != 4) return false;
    SmcPacket request = {.key = smc_fourcc(name), .command = 9}, reply;
    if (!exchange(smc, &request, &reply) || !reply.size || reply.size > 32) return false;
    *key = (SmcKey){.key = request.key, .type = reply.type,
                    .size = reply.size, .attributes = reply.attributes};
    memcpy(key->name, name, 5);
    return true;
}

double smc_decode(uint32_t type, uint32_t size, const uint8_t bytes[32]) {
    uint32_t number = 0;
    if (size > 4 || !size) return NAN;
    for (uint32_t i = 0; i < size; i++) number = number << 8 | bytes[i];
    if (type == smc_fourcc("sp78") && size == 2) return (int16_t)number / 256.0;
    if (type == smc_fourcc("fpe2") && size == 2) return number / 4.0;
    if ((type == smc_fourcc("ui8 ") && size == 1) ||
        (type == smc_fourcc("ui16") && size == 2) ||
        (type == smc_fourcc("ui32") && size == 4)) return number;
    if (type == smc_fourcc("flt ") && size == 4) {
        float value;
        memcpy(&value, bytes, sizeof(value));
        return value;
    }
    return NAN;
}

bool smc_encode(uint32_t type, uint32_t size, double value, uint8_t bytes[32]) {
    if (!isfinite(value) || value < 0) return false;
    memset(bytes, 0, 32);
    if (type == smc_fourcc("fpe2") && size == 2 && value <= 16383.75) {
        uint16_t encoded = (uint16_t)lround(value * 4);
        bytes[0] = (uint8_t)(encoded >> 8);
        bytes[1] = (uint8_t)encoded;
        return true;
    }
    if (type == smc_fourcc("flt ") && size == 4 && value <= 20000) {
        float encoded = (float)value;
        memcpy(bytes, &encoded, sizeof(encoded));
        return true;
    }
    return false;
}

bool smc_read(Smc *smc, const SmcKey *key, double *value) {
    SmcPacket request = {.key = key->key, .size = key->size, .command = 5}, reply;
    if (!exchange(smc, &request, &reply)) return false;
    *value = smc_decode(key->type, key->size, reply.bytes);
    return isfinite(*value);
}

bool smc_write(Smc *smc, const SmcKey *key, double value) {
    SmcPacket request = {.key = key->key, .size = key->size, .command = 6}, reply;
    if (!smc_encode(key->type, key->size, value, request.bytes)) return false;
    return exchange(smc, &request, &reply);
}
