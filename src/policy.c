#include "policy.h"
#include <math.h>
#include <string.h>

static double clamp(double x, double lo, double hi) { return fmin(hi, fmax(lo, x)); }

void config_defaults(Config *c) {
    *c = (Config){
        .curves = {
            {5, {{41, 0}, {50, .30}, {65, .50}, {78, .70}, {88, .85}}},
            {5, {{40, 0}, {50, .40}, {62, .60}, {75, .80}, {88, .95}}},
            {5, {{38, 0}, {48, .45}, {60, .70}, {72, .90}, {82, 1}}}
        },
        .interval = 2, .idle_interval = 5, .performance_load = .35,
        .extreme_load = .75, .performance_temperature = 65,
        .gpu_activation_temperature = 55,
        .extreme_temperature = 76, .emergency_temperature = 85,
        .die_emergency_temperature = 95, .upgrade_seconds = 8,
        .downgrade_seconds = 90, .hysteresis = 2,
        .ramp_up = .10, .ramp_down = .025
    };
}

bool config_valid(const Config *c) {
    const double values[] = {c->interval, c->idle_interval, c->performance_load,
        c->extreme_load, c->performance_temperature, c->extreme_temperature,
        c->emergency_temperature, c->die_emergency_temperature, c->upgrade_seconds,
        c->downgrade_seconds, c->hysteresis, c->ramp_up, c->ramp_down,
        c->gpu_activation_temperature};
    for (size_t i = 0; i < sizeof(values) / sizeof(*values); i++)
        if (!isfinite(values[i])) return false;
    if (c->interval < 1 || c->interval > 5 || c->idle_interval < c->interval ||
        c->idle_interval > 10 || c->performance_load < .05 ||
        c->performance_load >= c->extreme_load || c->extreme_load > 1 ||
        c->gpu_activation_temperature < 40 || c->gpu_activation_temperature > 70 ||
        c->performance_temperature < 40 ||
        c->performance_temperature >= c->extreme_temperature ||
        c->extreme_temperature >= c->emergency_temperature ||
        c->emergency_temperature > 90 || c->die_emergency_temperature < 85 ||
        c->die_emergency_temperature > 100 || c->upgrade_seconds < 2 ||
        c->upgrade_seconds > 60 || c->downgrade_seconds < 15 ||
        c->downgrade_seconds > 600 || c->hysteresis < 1 || c->hysteresis > 5 ||
        c->ramp_up < .02 || c->ramp_up > 1 || c->ramp_down < .005 ||
        c->ramp_down > c->ramp_up) return false;
    for (size_t k = 0; k < 3; k++) {
        const Curve *curve = &c->curves[k];
        if (curve->count < 2 || curve->count > MAX_POINTS) return false;
        for (size_t i = 0; i < curve->count; i++) {
            CurvePoint p = curve->points[i];
            if (!isfinite(p.temperature) || !isfinite(p.fraction) ||
                p.temperature < 20 || p.temperature > 100 ||
                p.fraction < 0 || p.fraction > 1) return false;
            if (i && (p.temperature <= curve->points[i - 1].temperature ||
                      p.fraction < curve->points[i - 1].fraction)) return false;
        }
    }
    return true;
}

static double secant(const Curve *c, size_t i) {
    return (c->points[i + 1].fraction - c->points[i].fraction) /
           (c->points[i + 1].temperature - c->points[i].temperature);
}

static double tangent(const Curve *c, size_t i) {
    if (!i || i == c->count - 1) return 0;
    double left = secant(c, i - 1), right = secant(c, i);
    if (left <= 0 || right <= 0) return 0;
    double a = c->points[i].temperature - c->points[i - 1].temperature;
    double b = c->points[i + 1].temperature - c->points[i].temperature;
    return 3 * (a + b) / ((a + 2 * b) / left + (2 * a + b) / right);
}

double curve_value(const Curve *c, double temperature) {
    if (!isfinite(temperature)) return 1;
    if (temperature <= c->points[0].temperature) return c->points[0].fraction;
    if (temperature >= c->points[c->count - 1].temperature)
        return c->points[c->count - 1].fraction;
    size_t i = 0;
    while (temperature > c->points[i + 1].temperature) i++;
    double width = c->points[i + 1].temperature - c->points[i].temperature;
    double t = (temperature - c->points[i].temperature) / width;
    double t2 = t * t, t3 = t2 * t;
    double value = (2 * t3 - 3 * t2 + 1) * c->points[i].fraction +
                   (t3 - 2 * t2 + t) * width * tangent(c, i) +
                   (-2 * t3 + 3 * t2) * c->points[i + 1].fraction +
                   (t3 - t2) * width * tangent(c, i + 1);
    return clamp(value, c->points[i].fraction, c->points[i + 1].fraction);
}

double policy_step(Policy *p, const Config *c, Sample s) {
    bool valid = s.sensors_valid && isfinite(s.temperature) &&
                 s.temperature >= 5 && s.temperature <= 125 &&
                 isfinite(s.die_temperature) && s.die_temperature >= 5 &&
                 s.die_temperature <= 125;
    bool gap = !isfinite(s.elapsed) || s.elapsed > c->idle_interval * 3;
    double dt = gap ? c->interval : clamp(s.elapsed, 0, c->idle_interval);
    if (valid && s.temperature >= c->gpu_activation_temperature) p->gpu_active = true;
    if (valid && s.temperature < c->gpu_activation_temperature - c->hysteresis)
        p->gpu_active = false;
    double load = fmax(isfinite(s.cpu) ? clamp(s.cpu, 0, 1) : 0,
                       p->gpu_active && isfinite(s.gpu) ? clamp(s.gpu, 0, 1) : 0);
    bool first = !p->initialized;
    if (gap) {
        p->candidate_seconds = p->downgrade_seconds = p->recovery_seconds = 0;
        p->load = load;
    }
    if (first) {
        p->load = load;
        p->temperature = p->last_temperature = valid ? s.temperature : 85;
        p->initialized = true;
    }
    p->load += (load - p->load) * (1 - exp(-dt / 6));
    if (!valid || s.temperature >= c->emergency_temperature ||
        s.die_temperature >= c->die_emergency_temperature) {
        p->emergency = true;
        p->recovery_seconds = 0;
    }
    if (p->emergency) {
        if (valid && s.temperature < c->emergency_temperature - 5 &&
            s.die_temperature < c->die_emergency_temperature - 5)
            p->recovery_seconds += dt;
        else p->recovery_seconds = 0;
        p->mode = EXTREME;
        p->downgrade_seconds = p->candidate_seconds = 0;
        p->fraction = 1;
        if (p->recovery_seconds < 30) return 1;
        p->emergency = false;
        p->temperature = p->last_temperature = s.temperature;
    }
    double rate = gap || first ? 0 : (s.temperature - p->last_temperature) / fmax(dt, .1);
    p->last_temperature = s.temperature;
    if (s.temperature > p->temperature) p->temperature = s.temperature;
    else if (s.temperature < p->temperature - c->hysteresis)
        p->temperature += (s.temperature - p->temperature) * (1 - exp(-dt / 12));
    double predicted = s.temperature + clamp(rate * 4, 0, 6);
    Mode thermal = predicted >= c->extreme_temperature ? EXTREME :
                   predicted >= c->performance_temperature ? PERFORMANCE : BALANCED;
    Mode requested = p->load >= c->extreme_load ? EXTREME :
                     p->load >= c->performance_load ? PERFORMANCE : BALANCED;
    if (requested != p->candidate) {
        p->candidate = requested;
        p->candidate_seconds = 0;
    }
    p->candidate_seconds += dt;
    Mode next = p->mode;
    if (thermal > next) next = thermal;
    if (requested > next && p->candidate_seconds >= c->upgrade_seconds) next = requested;
    if (next > p->mode) {
        p->mode = next;
        p->downgrade_seconds = 0;
    } else {
        double threshold = p->mode == EXTREME ? c->extreme_temperature : c->performance_temperature;
        double load_threshold = p->mode == EXTREME ? c->extreme_load : c->performance_load;
        if (p->mode > BALANCED && s.temperature < threshold - 5 &&
            p->load < load_threshold - .1 && rate < .2)
            p->downgrade_seconds += dt;
        else p->downgrade_seconds = 0;
        if (p->downgrade_seconds >= c->downgrade_seconds) {
            p->mode--;
            p->downgrade_seconds = 0;
        }
    }
    double desired = curve_value(&c->curves[p->mode], p->temperature);
    p->fraction += clamp(desired - p->fraction, -c->ramp_down * dt, c->ramp_up * dt);
    return clamp(p->fraction, 0, 1);
}

const char *mode_name(Mode mode) {
    static const char *names[] = {"balanced", "performance", "extreme"};
    return mode >= BALANCED && mode <= EXTREME ? names[mode] : "unknown";
}

double fan_target(double fraction, double minimum, double maximum) {
    return clamp(round(clamp(fraction, 0, 1) * maximum), minimum, maximum);
}
