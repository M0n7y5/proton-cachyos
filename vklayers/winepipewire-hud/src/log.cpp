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
    int view_level;
    pid_t pid;

    hud_config()
    {
        const char *level = getenv(HUD_ENV_LOG);
        const char *view = getenv(HUD_ENV_VIEW);

        enabled = env_on(HUD_ENV_ENABLE);
        log_level = level ? (int)strtol(level, nullptr, 10) : (enabled ? HUD_LOG_LIFECYCLE : 0);
        /* Out of range clamps rather than falls back to the default: a typo that
         * asked for more should not silently give less. */
        view_level = view && *view ? (int)strtol(view, nullptr, 10) : HUD_VIEW_COMPACT;
        if (view_level < HUD_VIEW_OFF)
            view_level = HUD_VIEW_OFF;
        else if (view_level > HUD_VIEW_VERBOSE)
            view_level = HUD_VIEW_VERBOSE;
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

int hud_view_level(void)
{
    return config().view_level;
}

void hud_logf(const char *fmt, ...)
{
    char line[512];
    char out[576];
    va_list args;
    ssize_t written;
    int len;

    va_start(args, fmt);
    len = vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    if (len < 0)
        return;

    /* One write() per line, not fprintf: Wine's own debug channels share this
     * fd from other threads, and stderr is unbuffered, so a multi-segment
     * fprintf can emit a line in several syscalls and another thread's output
     * lands mid-line.  Level 2 is the form users paste back, so a torn line
     * costs a counter nobody can re-derive. */
    len = snprintf(out, sizeof(out), "winepipewire-hud[%d]: %s\n", (int)config().pid, line);
    if (len <= 0)
        return;
    if (len > (int)sizeof(out) - 1)
        len = (int)sizeof(out) - 1;
    written = write(STDERR_FILENO, out, (size_t)len);
    (void)written;
}

uint64_t hud_mono_ns(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
