#ifndef FANPILOT_POLICY_H
#define FANPILOT_POLICY_H

#include <stdbool.h>
#include <stddef.h>

#define MAX_POINTS 16

typedef enum { BALANCED, PERFORMANCE, EXTREME } Mode;
typedef struct { double temperature, fraction; } CurvePoint;
typedef struct { size_t count; CurvePoint points[MAX_POINTS]; } Curve;
typedef struct {
    Curve curves[3];
    double interval, idle_interval, performance_load, extreme_load;
    double gpu_activation_temperature;
    double performance_temperature, extreme_temperature, emergency_temperature;
    double die_emergency_temperature, upgrade_seconds, downgrade_seconds;
    double hysteresis, ramp_up, ramp_down;
} Config;
typedef struct {
    Mode mode;
    Mode candidate;
    bool initialized, emergency, gpu_active;
    double candidate_seconds, downgrade_seconds, recovery_seconds;
    double load, temperature, last_temperature, fraction;
} Policy;
typedef struct {
    double temperature, die_temperature, cpu, gpu, elapsed;
    bool sensors_valid;
} Sample;

void config_defaults(Config *config);
bool config_valid(const Config *config);
double curve_value(const Curve *curve, double temperature);
double policy_step(Policy *policy, const Config *config, Sample sample);
const char *mode_name(Mode mode);
double fan_target(double fraction, double minimum, double maximum);

#endif
