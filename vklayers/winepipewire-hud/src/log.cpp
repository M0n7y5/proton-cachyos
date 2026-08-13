/*
 * SPDX-License-Identifier: MIT
 */
#include "log.hpp"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include <unistd.h>

namespace {

bool env_on(const char *name)
{
    const char *value = getenv(name);

    return value && *value && strcmp(value, "0");
}

struct hud_config
{
    bool enabled;
    int log_level;
    pid_t pid;

    hud_config()
    {
        const char *level = getenv(HUD_ENV_LOG);

        enabled = env_on(HUD_ENV_ENABLE);
        log_level = level ? (int)strtol(level, nullptr, 10) : (enabled ? HUD_LOG_LIFECYCLE : 0);
        pid = getpid();
    }
};

const struct hud_config &config()
{
    static const struct hud_config cfg;

    return cfg;
}

} /* namespace */

bool hud_enabled(void)
{
    return config().enabled;
}

int hud_log_level(void)
{
    return config().log_level;
}

void hud_logf(const char *fmt, ...)
{
    char line[512];
    va_list args;

    va_start(args, fmt);
    if (vsnprintf(line, sizeof(line), fmt, args) >= 0)
        fprintf(stderr, "winepipewire-hud[%d]: %s\n", (int)config().pid, line);
    va_end(args);
}

uint64_t hud_mono_ns(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
