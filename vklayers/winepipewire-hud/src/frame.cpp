/*
 * The ImGui frame: one window showing the driver's published snapshot.  First
 * pixels only, so this is a plain list of values, not a layout.
 *
 * SPDX-License-Identifier: MIT
 */
#include <cstdio>

#include "layer.hpp"

static const char *dispatch_name(uint32_t dispatch)
{
    switch (dispatch)
    {
    case PWHUD_DISPATCH_DATA: return "RT data loop";
    case PWHUD_DISPATCH_LOOP: return "main loop";
    default:                  return "unknown";
    }
}

static const char *bed_name(unsigned i)
{
    static const char *const names[] = { "FL", "FR", "FC", "LFE", "SL", "SR", "BL", "BR",
                                         "TFL", "TFR", "TBL", "TBR", "BFL", "BFR", "BBL",
                                         "BBR", "BC" };

    return i < sizeof(names) / sizeof(*names) ? names[i] : "?";
}

static void section_a_rows(const struct hud_snapshot_view *view, uint64_t now)
{
    const struct pwhud_snapshot *a = &view->a;
    uint32_t flags = hud_snapshot_flags_a(view);

    if (!view->have_a || !a->clock_ns)
    {
        ImGui::TextUnformatted("driver: no publish yet");
        return;
    }

    ImGui::Text("%s %u Hz, quantum %u, period %u us%s",
                flags & PWHUD_F_CAPTURE ? "capture" : "render", a->pw_rate, a->pw_quantum,
                a->drv_period_usec, flags & PWHUD_F_GRID_VALID ? "" : ", grid lost");
    ImGui::Text("dispatch %s, %u stream%s, phase %+lld us", dispatch_name(a->drv_dispatch),
                a->pw_stream_count, a->pw_stream_count == 1 ? "" : "s",
                (long long)a->drv_phase_adjust_us);
    /* Two failures that must never share a row.  pw_xruns belongs to the graph
     * driver node, the device the whole graph is clocked by and which every other
     * client shares, so it says the sink glitched.  drv_underruns is our own ring
     * starving.  One induced fault produced 2 of the former against 249 of the
     * latter, so a single "xruns" row would have pointed at the wrong subsystem. */
    if (hud_snapshot_dsp_load_valid(view))
        ImGui::Text("graph sink xruns %u, dsp %.1f%%", a->pw_xruns,
                    100.0 * (double)a->pw_dsp_load);
    else
        ImGui::Text("graph sink xruns %u, dsp not measured", a->pw_xruns);
    ImGui::Text("ring %.1f%% of %llu bytes, period %llu",
                a->drv_ring_bytes ? 100.0 * (double)a->drv_held_bytes / (double)a->drv_ring_bytes
                                  : 0.0,
                (unsigned long long)a->drv_ring_bytes, (unsigned long long)a->drv_period_bytes);
    ImGui::Text("ring under %u, over %u, bad %u", a->drv_underruns, a->drv_overruns,
                a->drv_bad_buffers);

    if (a->out_channels)
    {
        char peaks[128] = "peak";
        int len = 4;

        for (uint32_t i = 0; i < a->out_channels && i < PWHUD_OUT_MAX && len < (int)sizeof(peaks);
             i++)
            len += snprintf(peaks + len, sizeof(peaks) - len, " %.1f", a->out_peak_db[i]);
        ImGui::Text("%s dB%s", peaks, flags & PWHUD_F_OUT_TRUNCATED ? " +more" : "");
    }
    else
    {
        /* Nothing here is a measurement, so nothing here may claim one.
         *
         * Genuine digital silence never reaches this branch: a scan that runs
         * and finds only zeroes publishes PWHUD_DB_FLOOR on every channel and
         * a non-zero out_channels, which is the row above.  The old "silent"
         * was therefore unreachable for its stated meaning and is gone rather
         * than reworded, and the old count of three was wrong: four unflagged
         * conditions reach here, the driver's empty ring plus three that mean
         * the stream is not carrying audio at all.
         *
         * An empty ring is worth separating because it is the only one that
         * happens in normal running, and only for a tick: the driver takes the
         * peak before it retires the period, so a zero here means that period
         * really had nothing in it.  The rest share one honest word; telling
         * them apart needs a flag bit the driver does not spend. */
        ImGui::Text("peak %s", flags & PWHUD_F_CAPTURE        ? "n/a on capture"
                               : flags & PWHUD_F_OUT_NO_METER ? "unmetered"
                               : !a->drv_held_bytes           ? "no data this tick"
                                                              : "unavailable");
    }

    if (hud_snapshot_idle(view, now))
        ImGui::Text("IDLE, last publish %.1f s ago",
                    (double)(now - a->clock_ns) / 1e9);
}

static void section_b_rows(const struct hud_snapshot_view *view)
{
    const struct pwhud_snapshot *b = &view->b;
    char beds[192];
    int len = 0;

    if (!hud_snapshot_spatial_published(view))
    {
        /* A process whose audio never goes through ISpatialAudioClient stays
         * here for its whole life, which is not a bed of zeroes. */
        ImGui::TextUnformatted("spatial: never published");
        return;
    }

    ImGui::Text("spatial %s, bed %s, objects %u/%u", b->sp_hrtf ? "HRTF" : "stereo pan",
                b->sp_bed_virtualized ? "virtualized" : "direct", b->sp_dyn_live, b->sp_dyn_max);

    if (!b->sp_bed_mask)
    {
        ImGui::TextUnformatted("bed: no channels");
        return;
    }
    for (unsigned i = 0; i < PWHUD_BED_MAX && len < (int)sizeof(beds); i++)
        if (b->sp_bed_mask & (1u << i))
            len += snprintf(beds + len, sizeof(beds) - len, "%s%s %.1f", len ? "  " : "",
                            bed_name(i), b->sp_bed_db[i]);
    ImGui::Text("bed %s%s", beds,
                hud_snapshot_flags_b(view) & PWHUD_F_BED_TRUNCATED ? "  +more" : "");
}

void hud_build_frame(struct swapchain_data *swapchain_data)
{
    const struct hud_snapshot_view *view = &swapchain_data->snapshot;
    uint64_t now = hud_mono_ns();

    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(8.0f, 8.0f), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.65f);
    ImGui::Begin("winepipewire", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                     ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing |
                     ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings);

    if (!view->samples)
        ImGui::TextUnformatted("winepipewire: no snapshot for this process");
    else
    {
        section_a_rows(view, now);
        ImGui::Separator();
        section_b_rows(view);
    }

    ImGui::End();
    ImGui::EndFrame();
    ImGui::Render();
}
