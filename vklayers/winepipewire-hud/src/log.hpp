/*
 * Environment gating, logging and the monotonic clock, shared by the layer and
 * the snapshot reader.  Kept free of Vulkan so the reader links without it.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef WINEPIPEWIRE_HUD_LOG_HPP
#define WINEPIPEWIRE_HUD_LOG_HPP

#include <cstdint>

/* Opt-in, and off unless asked for: an installed manifest changes nothing until
 * this is set.  The loader also honours it through the manifest's
 * enable_environment for the implicit-layer case, but an explicitly enabled
 * layer bypasses that, so the gate is enforced in the layer too. */
#define HUD_ENV_ENABLE "WINEPIPEWIRE_HUD_OVERLAY"
#define HUD_ENV_LOG    "WINEPIPEWIRE_HUD_OVERLAY_LOG"
/* Development only: read another process's snapshot instead of this process's.
 * In a game the driver and the layer share one process, so the default needs no
 * coordination at all; a non-Wine Vulkan application has no snapshot of its own
 * and can be pointed at one with this. */
#define HUD_ENV_PID    "WINEPIPEWIRE_HUD_OVERLAY_PID"

/* Levels: 1 lifecycle, 2 one line per swapchain per second, 3 every present. */
#define HUD_LOG_LIFECYCLE 1
#define HUD_LOG_RATE      2
#define HUD_LOG_PRESENT   3

bool hud_enabled(void);
int hud_log_level(void);
void hud_logf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
uint64_t hud_mono_ns(void);

#define HUD_LOG(level, ...)                     \
    do {                                        \
        if (hud_log_level() >= (level))         \
            hud_logf(__VA_ARGS__);              \
    } while (0)

#endif /* WINEPIPEWIRE_HUD_LOG_HPP */
