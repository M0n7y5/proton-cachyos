/*
 * State the frame builder keeps between frames, and its entry point.  Separate
 * from render.hpp because none of this is lifted: render.hpp carries the
 * MangoHud draw path, this is ours.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef WINEPIPEWIRE_HUD_FRAME_HPP
#define WINEPIPEWIRE_HUD_FRAME_HPP

#include <cstdint>

#include "snapshot.hpp"

/* One publish per period, so 128 samples is 1.28 s of history at a 10 ms period
 * and proportionally more at a longer one.  A plot wider than the panel would be
 * resampled away by PlotLines, which draws at most one segment per pixel. */
#define HUD_HISTORY 128

/* pw_xruns, drv_underruns, drv_overruns, drv_bad_buffers, drv_ring_resyncs, in
 * that order.  A counter answers "is this happening now" with a recency, not with
 * a plot: the value is a total, and a plot of a monotonic total is a staircase
 * nobody can read at a glance.  None of these totals is monotonic, either: the
 * driver sums each over its live streams, so a released stream lowers them. */
#define HUD_COUNTERS 5

/* How long a level meter's hold marker stays at a maximum before it follows the
 * level back down. */
#define HUD_HOLD_NS 1500000000ull

/* A counter that moved this recently is called out as moving now rather than as
 * having moved once. */
#define HUD_RECENT_NS 3000000000ull

struct hud_frame_state
{
    /* Advanced on a section A publish, never on a present.  hud_build_frame runs
     * once per present, which is the application's frame rate, while the driver
     * publishes at the period rate; sampling per present would duplicate samples
     * and scroll every plot at the frame rate instead of at the publish rate.
     * Owned by the presenting thread along with the rest of swapchain_data, so
     * none of this needs a lock. */
    uint64_t first_clock_ns;
    uint64_t last_clock_ns;
    uint32_t publishes;
    /* Consecutive publishes carrying no output meter, so the display can say
     * whether that is this tick or every tick since the stream started. */
    uint32_t out_absent_run;

    float phase_us[HUD_HISTORY];
    uint32_t phase_count;
    uint32_t phase_next;

    uint32_t counter[HUD_COUNTERS];
    uint64_t counter_ns[HUD_COUNTERS];

    /* Section B publishes at its own rate under its own seqlock, so its holds
     * are gated on its own sequence rather than on section A's clock. */
    uint32_t last_seq_sp;
    float bed_hold[PWHUD_BED_MAX];
    uint64_t bed_hold_ns[PWHUD_BED_MAX];
    /* The clip total as of the last section B publish, and when it last grew.
     * A cumulative total cannot say whether truncation is happening now, which
     * is the only question a listener has, so the row needs the edge and not
     * just the value.  Section B's own sequence gates it, as the holds above. */
    uint64_t clip_last;
    uint64_t clip_ns;
    float out_hold[PWHUD_OUT_MAX];
    uint64_t out_hold_ns[PWHUD_OUT_MAX];
    uint32_t str_hold_id[PWHUD_STR_MAX];
    float str_hold[PWHUD_STR_MAX][PWHUD_OUT_MAX];
    uint64_t str_hold_ns[PWHUD_STR_MAX][PWHUD_OUT_MAX];

    /* Reported by the once-a-second log line, so the worst case is measured
     * rather than estimated.  The layer has no vertex offset support, which caps
     * one draw list at 65535 vertices. */
    uint32_t last_vtx;
    uint32_t last_idx;
};

/* Builds the ImGui frame for one swapchain, in frame.cpp. */
void hud_build_frame(struct swapchain_data *swapchain_data);

#endif /* WINEPIPEWIRE_HUD_FRAME_HPP */
