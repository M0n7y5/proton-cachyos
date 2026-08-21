/*
 * The ImGui frame: the driver's published snapshot as a layout.
 *
 * Two rules run through all of it.  The first is that a value the driver did not
 * measure must not be able to look like one it did: absence draws no meter at
 * all and says why, a measured floor draws a real track with a real number, and
 * the two are told apart by whether there is a track rather than by reading the
 * label.  The second is that a cell's position depends on which channel it is,
 * never on how many channels precede it, so a bed that changes width changes
 * which rows exist and never where a channel sits.
 *
 * SPDX-License-Identifier: MIT
 */
#include <cmath>
#include <cstdarg>
#include <cstdio>

#include "layer.hpp"

/* Display floor, deliberately not PWHUD_DB_FLOOR.  That constant is the
 * producer's value for silence and for an unused slot, and the header documents
 * it as something a meter plots rather than as a display range; a -120 dB span
 * spends two thirds of a track on content that never occurs.  Bar length is
 * linear in dB from here to 0 dBFS, which puts quiet menu content low on the
 * track.  That is the honest picture: a scale that made -44 dBFS look
 * two thirds of full scale would be a different lie, and an auto-ranged one
 * would change the meaning of a length between frames. */
#define HUD_FLOOR_DB (-60.0f)

/* Drawn 1 px wide inside the track so a length can be read as a level rather
 * than only compared with its neighbours. */
static const float hud_ticks_db[] = { -40.0f, -20.0f };

/* Every value on screen is in one of these states, and the state decides the
 * colour.  Unavailable is grey and never an alarm colour: a red or amber
 * nothing reads as a measurement. */
enum hud_state
{
    HUD_STATE_CONFIG,  /* changes at stream setup, so one step down */
    HUD_STATE_LIVE,    /* measured, this publish */
    HUD_STATE_NA,      /* no measurement exists */
    HUD_STATE_WATCH,   /* a fault counter that is nonzero but not moving */
    HUD_STATE_FAULT,   /* moving now, or an input that cannot be believed */
};

/* Set for a frame whose snapshot has aged out.  The numbers are still the last
 * good ones, so they stay on screen and the whole panel loses alpha instead:
 * frozen values that look live are the defect this avoids. */
static bool hud_frame_stale;

static ImVec4 hud_state_colour(enum hud_state state)
{
    ImVec4 col;

    switch (state)
    {
    case HUD_STATE_CONFIG: col = ImVec4(0.62f, 0.62f, 0.62f, 1.00f); break;
    case HUD_STATE_NA:     col = ImVec4(0.50f, 0.50f, 0.50f, 1.00f); break;
    case HUD_STATE_WATCH:  col = ImVec4(1.00f, 0.72f, 0.25f, 1.00f); break;
    case HUD_STATE_FAULT:  col = ImVec4(1.00f, 0.45f, 0.40f, 1.00f); break;
    default:               col = ImVec4(0.90f, 0.90f, 0.90f, 1.00f); break;
    }
    if (hud_frame_stale)
        col.w *= 0.55f;
    return col;
}

static void hud_text(enum hud_state state, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

static void hud_text(enum hud_state state, const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    ImGui::PushStyleColor(ImGuiCol_Text, hud_state_colour(state));
    ImGui::TextV(fmt, args);
    ImGui::PopStyleColor();
    va_end(args);
}

/* The driver's own words, verbatim, so a phrase read off the overlay is a phrase
 * that greps the driver's log.  pipewire.c:2908 sets the field from the same
 * condition its report_dispatch_mode names "data-thread" or "driver-loop" at
 * :2917, and this used to render the second of those as "main loop", which in
 * PipeWire's model is a different thread from the driver loop and sent a reader
 * chasing a scheduling problem to the wrong place.  UNKNOWN is the field's initial
 * value, from before report_dispatch_mode has latched anything, so it is reported
 * as not latched rather than as a third mode: the driver has no word for it
 * because it never logs one. */
static const char *dispatch_name(uint32_t dispatch)
{
    switch (dispatch)
    {
    case PWHUD_DISPATCH_DATA: return "data-thread";
    case PWHUD_DISPATCH_LOOP: return "driver-loop";
    default:                  return "dispatch not latched";
    }
}

/* AudioObjectType bit order, FrontLeft 0x2 through BackCenter 0x20000, which is
 * the order the spatial publisher indexes sp_bed_db by.  Slot 17 exists only so
 * the struct lands on a multiple of 8 and sits above the real ceiling, so it has
 * no name: a bed that sets it is drawn as "?" rather than dropped. */
static const char *bed_name(unsigned i)
{
    static const char *const names[] = { "FL", "FR", "FC", "LFE", "SL", "SR", "BL", "BR",
                                         "TFL", "TFR", "TBL", "TBR", "BFL", "BFR", "BBL",
                                         "BBR", "BC" };

    return i < sizeof(names) / sizeof(*names) ? names[i] : "?";
}

/* Four reachable combinations of the engine and the request, each named for what
 * is actually rendering the bed.  sp_hrtf is the engine, sp_bed_virtualized is the
 * request, and the interesting one is a request with no engine: that is a stream
 * that asked for HRTF, could not load it, and is panning instead.  Those are two
 * different facts, and printing them side by side left the reader to combine
 * them; a configuration that did not get what it asked for should be legible
 * without knowing this codebase, and nothing else on screen says so.
 * Phrased to follow the row's own "bed" label without repeating it. */
static const char *bed_backend(uint32_t hrtf, uint32_t virtualized)
{
    if (virtualized)
        return hrtf ? "HRTF" : "panned, no HRTF engine";
    return hrtf ? "direct, objects on HRTF" : "direct";
}

/* Speaker rows in canonical order, left and right slot per row.  A row is drawn
 * only when the bed mask names one of its channels, so a stereo bed is one row
 * and the 16-channel bed a title in the test set ships is eight, while FL stays
 * in the same place in both. */
struct hud_bed_row
{
    signed char left;
    signed char right;
};

static const struct hud_bed_row hud_bed_grid[] = {
    {  0,  1 },  /* FL  FR  */
    {  2,  3 },  /* FC  LFE */
    {  4,  5 },  /* SL  SR  */
    {  6,  7 },  /* BL  BR  */
    { 16, -1 },  /* BC      */
    {  8,  9 },  /* TFL TFR */
    { 10, 11 },  /* TBL TBR */
    { 12, 13 },  /* BFL BFR */
    { 14, 15 },  /* BBL BBR */
    { 17, -1 },  /* the unnamed spare slot, so it cannot vanish silently */
};

/* Geometry in character advances rather than pixels, so the whole layout follows
 * the baked font size and a scaled atlas needs no second set of constants. */
struct hud_layout
{
    float em;
    float line;
    float label_w;
    float track_w;
    float track_h;
    float value_w;
    float cell_w;
    bool verbose;
    ImU32 col_label;
    ImU32 col_value;
    ImU32 col_track;
    ImU32 col_fill;
    ImU32 col_tick;
    ImU32 col_hold;
    ImU32 col_fault;
};

static void hud_layout_init(struct hud_layout *l, bool verbose)
{
    l->em = ImGui::CalcTextSize("M").x;
    l->line = ImGui::GetFontSize();
    l->verbose = verbose;
    l->label_w = 4.5f * l->em;
    l->track_w = (verbose ? 14.0f : 12.0f) * l->em;
    l->track_h = l->line > 8.0f ? l->line - 4.0f : 4.0f;
    l->value_w = 7.5f * l->em;
    l->cell_w = l->label_w + l->track_w + l->value_w;
    l->col_label = ImGui::GetColorU32(hud_state_colour(HUD_STATE_CONFIG));
    l->col_value = ImGui::GetColorU32(hud_state_colour(HUD_STATE_LIVE));
    l->col_track = ImGui::GetColorU32(ImVec4(0.30f, 0.30f, 0.30f,
                                             hud_frame_stale ? 0.55f : 1.00f));
    l->col_fill = ImGui::GetColorU32(ImVec4(0.90f, 0.70f, 0.00f,
                                            hud_frame_stale ? 0.55f : 1.00f));
    l->col_tick = ImGui::GetColorU32(ImVec4(0.55f, 0.55f, 0.55f,
                                            hud_frame_stale ? 0.55f : 1.00f));
    l->col_hold = ImGui::GetColorU32(ImVec4(0.85f, 0.85f, 0.85f,
                                            hud_frame_stale ? 0.55f : 1.00f));
    l->col_fault = ImGui::GetColorU32(hud_state_colour(HUD_STATE_FAULT));
}

static float hud_meter_fraction(float db)
{
    float t = (db - HUD_FLOOR_DB) / (0.0f - HUD_FLOOR_DB);

    return t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
}

/* One meter cell, drawn at a fixed offset from the row origin.  The bar is built
 * from AddRectFilled rather than from PlotHistogram: that widget draws each bar
 * between the value and the data value 0.0, and for a dBFS range ending at 0 the
 * baseline lands at the top of the frame, so a -60 to 0 range renders headroom
 * instead of level and puts the longest bar on the quietest channel. */
static void hud_meter(const struct hud_layout *l, ImVec2 origin, int column, const char *label,
                      float db, float hold, bool have_hold)
{
    ImDrawList *dl = ImGui::GetWindowDrawList();
    float x = origin.x + (float)column * l->cell_w;
    float tx0 = x + l->label_w;
    float tx1 = tx0 + l->track_w;
    float ty0 = origin.y + (l->line - l->track_h) * 0.5f;
    float ty1 = ty0 + l->track_h;
    ImVec2 label_size = ImGui::CalcTextSize(label);
    char value[16];
    float fill_x;

    /* Right-aligned against the track so FL and TFL share an edge. */
    dl->AddText(ImVec2(tx0 - 0.5f * l->em - label_size.x, origin.y), l->col_label, label);

    /* The track itself is what says "this channel was measured".  An absent
     * channel draws nothing here at all, which is the whole distinction. */
    dl->AddRectFilled(ImVec2(tx0, ty0), ImVec2(tx1, ty1), l->col_track);

    if (!std::isfinite(db))
    {
        /* Section B computes its levels from an RMS accumulation that returns
         * 20*log10 of a NaN sum unchanged, so a title feeding NaN audio publishes
         * a NaN level.  A bar cannot express that, and the NaN would reach
         * AddRectFilled as a coordinate, so it gets its own mark. */
        dl->AddLine(ImVec2(tx0, ty0), ImVec2(tx1, ty1), l->col_fault, 1.0f);
        dl->AddLine(ImVec2(tx0, ty1), ImVec2(tx1, ty0), l->col_fault, 1.0f);
        dl->AddText(ImVec2(tx1 + l->em, origin.y), l->col_fault, "   bad");
        return;
    }

    fill_x = tx0 + hud_meter_fraction(db) * l->track_w;
    if (fill_x > tx0)
        dl->AddRectFilled(ImVec2(tx0, ty0), ImVec2(fill_x, ty1), l->col_fill);

    for (unsigned i = 0; i < sizeof(hud_ticks_db) / sizeof(*hud_ticks_db); i++)
    {
        float tick_x = tx0 + hud_meter_fraction(hud_ticks_db[i]) * l->track_w;

        if (tick_x > fill_x)
            dl->AddRectFilled(ImVec2(tick_x, ty0), ImVec2(tick_x + 1.0f, ty1), l->col_tick);
    }

    if (have_hold && std::isfinite(hold))
    {
        float hold_x = tx0 + hud_meter_fraction(hold) * l->track_w;

        if (hold_x > tx1 - 1.0f)
            hold_x = tx1 - 1.0f;
        dl->AddRectFilled(ImVec2(hold_x, ty0), ImVec2(hold_x + 1.0f, ty1), l->col_hold);
    }

    snprintf(value, sizeof(value), "%6.1f", (double)db);
    dl->AddText(ImVec2(tx1 + l->em, origin.y), l->col_value, value);
}

/* Signed, centred, and full scale at plus or minus half a period because that is
 * exactly where the driver clamps the adjustment.  A bar that has reached an end
 * therefore says the clamp fired, with no separate indicator to keep in step. */
static void hud_phase_bar(const struct hud_layout *l, int64_t phase_us, uint32_t period_usec)
{
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 origin = ImGui::GetCursorScreenPos();
    float tx0 = origin.x;
    float tx1 = tx0 + l->track_w;
    float ty0 = origin.y + (l->line - l->track_h) * 0.5f;
    float ty1 = ty0 + l->track_h;
    float centre = tx0 + l->track_w * 0.5f;
    float half = (float)(period_usec / 2);
    float t = half > 0.0f ? (float)phase_us / half : 0.0f;

    if (t < -1.0f)
        t = -1.0f;
    else if (t > 1.0f)
        t = 1.0f;

    dl->AddRectFilled(ImVec2(tx0, ty0), ImVec2(tx1, ty1), l->col_track);
    if (t < 0.0f)
        dl->AddRectFilled(ImVec2(centre + t * l->track_w * 0.5f, ty0), ImVec2(centre, ty1),
                          l->col_fill);
    else if (t > 0.0f)
        dl->AddRectFilled(ImVec2(centre, ty0), ImVec2(centre + t * l->track_w * 0.5f, ty1),
                          l->col_fill);
    dl->AddRectFilled(ImVec2(centre, ty0), ImVec2(centre + 1.0f, ty1), l->col_tick);
    ImGui::Dummy(ImVec2(l->track_w, l->line));
}

/* A total plus how long ago it last moved.  A total alone cannot distinguish
 * something happening now from something that happened once at startup, and a
 * plot of a monotonic total is unreadable at this size. */
static void hud_counter(const struct hud_layout *l, const struct hud_frame_state *st,
                        unsigned index, const char *name, uint32_t value, uint64_t now)
{
    enum hud_state state = HUD_STATE_LIVE;
    uint64_t age_ns = 0;

    (void)l;
    if (value)
    {
        state = HUD_STATE_WATCH;
        if (st->counter_ns[index])
        {
            age_ns = now > st->counter_ns[index] ? now - st->counter_ns[index] : 0;
            if (age_ns < HUD_RECENT_NS)
                state = HUD_STATE_FAULT;
        }
    }

    /* No age when the counter has not moved since this overlay started looking:
     * claiming an age we never observed would be an invention. */
    if (!value || !st->counter_ns[index])
        hud_text(state, "%s %u", name, value);
    else if (age_ns < 90000000000ull)
        hud_text(state, "%s %u %.0f s", name, value, (double)age_ns / 1e9);
    else
        hud_text(state, "%s %u %.0f m", name, value, (double)age_ns / 6e10);
}

/* Advanced only when section A's clock moves, so the history is per publish and
 * not per present. */
static void hud_history_update(struct hud_frame_state *st, const struct hud_snapshot_view *view)
{
    const struct pwhud_snapshot *a = &view->a;
    const struct pwhud_snapshot *b = &view->b;
    uint32_t counters[HUD_COUNTERS];
    unsigned i;

    if (view->have_b && b->seq_sp && b->seq_sp != st->last_seq_sp)
    {
        st->last_seq_sp = b->seq_sp;
        for (i = 0; i < PWHUD_BED_MAX; i++)
        {
            float v = b->sp_bed_db[i];

            if (!std::isfinite(v))
                continue;
            if (!st->bed_hold_ns[i] || v >= st->bed_hold[i] ||
                hud_mono_ns() - st->bed_hold_ns[i] > HUD_HOLD_NS)
            {
                st->bed_hold[i] = v;
                st->bed_hold_ns[i] = hud_mono_ns();
            }
        }

        /* Only growth is truncation happening now.  A new elected stream
         * restarts its own totals, so the value can fall; that clears the
         * timestamp rather than setting it, exactly as the section A counters
         * treat a released stream, and for the same reason: after the drop we
         * no longer know when the remaining total was earned. */
        if (hud_snapshot_have_clip_stats(view))
        {
            if (st->clip_last && b->sp_clip_samples > st->clip_last)
                st->clip_ns = hud_mono_ns();
            else if (b->sp_clip_samples < st->clip_last)
                st->clip_ns = 0;
            st->clip_last = b->sp_clip_samples;
        }
    }

    if (!view->have_a || !a->clock_ns || a->clock_ns == st->last_clock_ns)
        return;

    if (!st->publishes)
        st->first_clock_ns = a->clock_ns;
    st->last_clock_ns = a->clock_ns;
    st->publishes++;

    st->out_absent_run = a->out_channels ? 0 : st->out_absent_run + 1;

    st->phase_us[st->phase_next] = (float)a->drv_phase_adjust_us;
    st->phase_next = (st->phase_next + 1) % HUD_HISTORY;
    if (st->phase_count < HUD_HISTORY)
        st->phase_count++;

    counters[0] = a->pw_xruns;
    counters[1] = a->drv_underruns;
    counters[2] = a->drv_overruns;
    counters[3] = a->drv_bad_buffers;
    /* Claimed from the old _pad_a, so an older driver leaves it zero.  That is
     * indistinguishable from a genuine zero and deliberately not surfaced: the
     * field reused declared padding, so sizeof stayed 240 and PWHUD_VERSION stayed
     * 1, and there is nothing in the header a reader could discriminate on even if
     * it wanted to.  The tool ships driver and layer together. */
    counters[4] = a->drv_ring_resyncs;
    for (i = 0; i < HUD_COUNTERS; i++)
    {
        /* The first publish only records where the counters already stood: it
         * says nothing about when they got there.
         *
         * Only an increase is a fault happening now.  These totals are summed
         * over the driver's live streams (pipewire.c:3768-3774), so a released
         * stream takes its history out of the total and the number goes down: a
         * real session stepped 9 to 5 across an orderly teardown.  Stamping on
         * any change painted that decrease in the fault colour for three seconds
         * and told the reader underruns were happening at the one moment they
         * demonstrably were not.
         *
         * A decrease clears the timestamp instead of setting it.  That is not a
         * second mechanism: it is the same "nonzero but not seen to move" state
         * the row already uses before any change is observed, and it is the
         * truthful one, because after a stream leaves we no longer know when the
         * remaining streams' faults happened. */
        if (st->publishes > 1)
        {
            if (counters[i] > st->counter[i])
                st->counter_ns[i] = a->clock_ns;
            else if (counters[i] < st->counter[i])
                st->counter_ns[i] = 0;
        }
        st->counter[i] = counters[i];
    }

    for (i = 0; i < PWHUD_OUT_MAX; i++)
    {
        float v = a->out_peak_db[i];

        if (i >= a->out_channels || !std::isfinite(v))
            continue;
        if (!st->out_hold_ns[i] || v >= st->out_hold[i] ||
            a->clock_ns - st->out_hold_ns[i] > HUD_HOLD_NS)
        {
            st->out_hold[i] = v;
            st->out_hold_ns[i] = a->clock_ns;
        }
    }

    if (hud_snapshot_have_stream_meters(view))
    {
        unsigned s, n = a->drv_str_count < PWHUD_STR_MAX ? a->drv_str_count : PWHUD_STR_MAX;

        for (s = 0; s < n; s++)
        {
            if (st->str_hold_id[s] != a->drv_str[s].id)
            {
                st->str_hold_id[s] = a->drv_str[s].id;
                for (i = 0; i < PWHUD_OUT_MAX; i++)
                {
                    st->str_hold[s][i] = 0.0f;
                    st->str_hold_ns[s][i] = 0;
                }
            }
            for (i = 0; i < a->drv_str[s].channels && i < PWHUD_OUT_MAX; i++)
            {
                float v = a->drv_str[s].peak_db[i];

                if (!std::isfinite(v))
                    continue;
                if (!st->str_hold_ns[s][i] || v >= st->str_hold[s][i] ||
                    a->clock_ns - st->str_hold_ns[s][i] > HUD_HOLD_NS)
                {
                    st->str_hold[s][i] = v;
                    st->str_hold_ns[s][i] = a->clock_ns;
                }
            }
        }
    }
}

/* Configuration: what the stream is, which changes at setup and then not at all.
 * Separated from the live block so a glance lands on the part that moves. */
static void hud_config_rows(const struct hud_layout *l, const struct hud_snapshot_view *view)
{
    const struct pwhud_snapshot *a = &view->a;
    uint32_t flags = hud_snapshot_flags_a(view);
    char rate[16], quantum[16], scope[32];

    if (a->pw_rate)
        snprintf(rate, sizeof(rate), "%u Hz", a->pw_rate);
    else
        snprintf(rate, sizeof(rate), "-- Hz");
    if (a->pw_quantum)
        snprintf(quantum, sizeof(quantum), "q %u", a->pw_quantum);
    else
        snprintf(quantum, sizeof(quantum), "q --");

    /* Two scopes on one row, kept terse because this row is width-constrained
     * and the long form pushed the overlay past the bounds the present smoke
     * test asserts.  "4 str  #7/2" is: four streams in this process, and the ring,
     * quantum and xrun numbers describe stream 7, one of the two started render
     * streams in its period group.  The out meters are the maximum over those two,
     * not stream 7's own, so a quiet elected stream cannot hide a loud sibling.
     * The id is a monotonic counter and not an
     * index into that two, hence the #, so it can be the larger number.  The
     * verbose legend below spells this out; the row itself cannot afford to.
     * Unavailable rather than 0 where there is no id, because 0 is not a
     * stream. */
    if (!hud_snapshot_have_stream_scope(view))
        snprintf(scope, sizeof(scope), "--");
    else if (!a->drv_stream_id)
        snprintf(scope, sizeof(scope), "none");
    else
        snprintf(scope, sizeof(scope), "#%u/%u", a->drv_stream_id, a->drv_group_streams);

    hud_text(HUD_STATE_CONFIG, "%s %s  %s  %.2f ms  %s  %u str  %s",
             flags & PWHUD_F_CAPTURE ? "capture" : "render", rate, quantum,
             (double)a->drv_period_usec / 1000.0, dispatch_name(a->drv_dispatch),
             a->pw_stream_count, scope);
    ImGui::SameLine();
    hud_text(flags & PWHUD_F_GRID_VALID ? HUD_STATE_CONFIG : HUD_STATE_FAULT, "%s",
             flags & PWHUD_F_GRID_VALID ? "grid" : "no grid");

    /* Ring geometry, not occupancy.  Occupancy is the held bytes at the publish
     * point, which for a client that hands over one period at a time is zero
     * every tick, so it belongs in the verbose block with its meaning attached
     * rather than in a headline percentage that reads as an error. */
    if (a->drv_ring_bytes && a->drv_period_bytes)
        hud_text(HUD_STATE_CONFIG, "ring %llu B = %llu x %llu B",
                 (unsigned long long)a->drv_ring_bytes,
                 (unsigned long long)(a->drv_ring_bytes / a->drv_period_bytes),
                 (unsigned long long)a->drv_period_bytes);
    else
        hud_text(HUD_STATE_NA, "ring not sized yet");

    if (l->verbose)
    {
        hud_text(HUD_STATE_CONFIG, "held at publish %llu B",
                 (unsigned long long)a->drv_held_bytes);
        ImGui::SameLine();
        if (hud_snapshot_dsp_load_valid(view))
            hud_text(HUD_STATE_LIVE, "dsp %.1f%%", 100.0 * (double)a->pw_dsp_load);
        else
            /* Not measured rather than zero: the driver sets the flag on every
             * publish because it cannot bind PipeWire's profiler at all. */
            hud_text(HUD_STATE_NA, "dsp --");
    }
}

static void hud_live_rows(const struct hud_layout *l, const struct hud_snapshot_view *view,
                          const struct hud_frame_state *st, uint64_t now)
{
    const struct pwhud_snapshot *a = &view->a;
    uint32_t flags = hud_snapshot_flags_a(view);

    /* Zero has two causes here: a phase that really is on the grid, and a grid
     * the driver lost, which forces the adjustment to exactly zero.  Without the
     * grid there is no measurement, so there is no bar to draw. */
    if (!(flags & PWHUD_F_GRID_VALID))
        hud_text(HUD_STATE_NA, "phase  --      no grid");
    else
    {
        hud_text(HUD_STATE_LIVE, "phase %+5lld us", (long long)a->drv_phase_adjust_us);
        ImGui::SameLine();
        hud_phase_bar(l, a->drv_phase_adjust_us, a->drv_period_usec);
        if (l->verbose && st->phase_count > 1)
        {
            float lo = st->phase_us[0];
            float hi = st->phase_us[0];
            uint32_t i;

            for (i = 1; i < st->phase_count; i++)
            {
                if (st->phase_us[i] < lo)
                    lo = st->phase_us[i];
                if (st->phase_us[i] > hi)
                    hi = st->phase_us[i];
            }
            if (hi - lo < 1.0f)
                hi = lo + 1.0f;
            /* The bar above is scaled to the clamp, so a bar at an end means the
             * clamp fired.  The trend is scaled to what it actually contains,
             * because at plus or minus half a period every real history is a flat
             * line through the middle and answers nothing.  Its range is printed
             * beside it, so no length here can be read as a magnitude it is not. */
            ImGui::SameLine();
            ImGui::PlotLines("##phase", st->phase_us, (int)st->phase_count,
                             st->phase_count == HUD_HISTORY ? (int)st->phase_next : 0, nullptr,
                             lo, hi, ImVec2(l->track_w, l->line * 2.0f));
            ImGui::SameLine();
            hud_text(HUD_STATE_LIVE, "%.0f..%.0f us over %u", (double)lo, (double)hi,
                     st->phase_count);
        }
    }

    hud_text(HUD_STATE_CONFIG, "fault");
    ImGui::SameLine();
    hud_counter(l, st, 0, "xrun", a->pw_xruns, now);
    ImGui::SameLine();
    hud_counter(l, st, 1, "under", a->drv_underruns, now);
    ImGui::SameLine();
    hud_counter(l, st, 2, "over", a->drv_overruns, now);
    ImGui::SameLine();
    hud_counter(l, st, 3, "bad", a->drv_bad_buffers, now);
    ImGui::SameLine();
    /* On this row and not its own, because it is the same kind of thing as the
     * other four: a total summed over live streams, so it needs the identical
     * non-monotonic recency treatment, and it is a rare event on a row whose whole
     * purpose is to be scanned for a non-zero.  A dedicated line would spend a row
     * of the compact view on a number that is almost always 0, in the view whose
     * density is the constraint.  "resync" is the driver's own word, from
     * do_resync_ring and drv_ring_resyncs, so it greps both sides. */
    hud_counter(l, st, 4, "resync", a->drv_ring_resyncs, now);
}

static bool hud_str_contains_id(const struct pwhud_snapshot *a, uint32_t id)
{
    unsigned i, n = a->drv_str_count < PWHUD_STR_MAX ? a->drv_str_count : PWHUD_STR_MAX;

    if (!id)
        return false;
    for (i = 0; i < n; i++)
        if (a->drv_str[i].id == id)
            return true;
    return false;
}

static void hud_str_rows(const struct hud_layout *l, const struct hud_snapshot_view *view,
                         const struct hud_frame_state *st)
{
    const struct pwhud_snapshot *a = &view->a;
    uint32_t flags = hud_snapshot_flags_a(view);
    unsigned s, n = a->drv_str_count < PWHUD_STR_MAX ? a->drv_str_count : PWHUD_STR_MAX;

    for (s = 0; s < n; s++)
    {
        const struct pwhud_str *str = &a->drv_str[s];
        bool elected = str->id && str->id == a->drv_stream_id;
        bool all_floor = true;
        unsigned i, drawn = 0;

        if (!str->channels)
        {
            hud_text(HUD_STATE_NA, "str %s%u  NO DATA  nothing to scan",
                     elected ? "#" : "", str->id);
            continue;
        }
        for (i = 0; i < str->channels && i < PWHUD_OUT_MAX; i++)
            if (str->peak_db[i] > PWHUD_DB_FLOOR || !std::isfinite(str->peak_db[i]))
                all_floor = false;
        hud_text(HUD_STATE_CONFIG, "str %s%u  peak, %u ch%s%s",
                 elected ? "#" : "", str->id, str->channels,
                 flags & PWHUD_F_OUT_TRUNCATED && elected ? ", +more" : "",
                 all_floor ? ", all at floor" : "");
        while (drawn < str->channels && drawn < PWHUD_OUT_MAX)
        {
            ImVec2 origin = ImGui::GetCursorScreenPos();
            int column = 0;

            while (column < 2 && drawn < str->channels && drawn < PWHUD_OUT_MAX)
            {
                char label[8];

                snprintf(label, sizeof(label), "ch%u", drawn + 1);
                hud_meter(l, origin, column, label, str->peak_db[drawn],
                          st->str_hold[s][drawn], l->verbose);
                column++;
                drawn++;
            }
            ImGui::Dummy(ImVec2(l->cell_w * 2.0f, l->line));
        }
    }
    if (flags & PWHUD_F_STR_TRUNCATED)
        hud_text(HUD_STATE_WATCH, "str   +more");
}

/* The output meter, in whichever of its three states the snapshot is actually
 * in.  Absence draws no track, because the metered channel count is itself zero
 * in that state: eight empty tracks would be a channel count the overlay
 * invented.  When the writer carries drv_str[] and the elected stream is one of
 * those rows, that list replaces this block so the elected stream is not drawn
 * twice. */
static void hud_out_rows(const struct hud_layout *l, const struct hud_snapshot_view *view,
                         const struct hud_frame_state *st)
{
    const struct pwhud_snapshot *a = &view->a;
    uint32_t flags = hud_snapshot_flags_a(view);
    unsigned i, drawn = 0;
    bool all_floor = true;

    if (hud_snapshot_have_stream_meters(view) && a->drv_str_count)
    {
        hud_str_rows(l, view, st);
        if (hud_str_contains_id(a, a->drv_stream_id))
            return;
    }

    if (!a->out_channels)
    {
        char reason[96];

        if (flags & PWHUD_F_CAPTURE)
            snprintf(reason, sizeof(reason), "no meter on a capture stream");
        else if (flags & PWHUD_F_OUT_NO_METER)
            snprintf(reason, sizeof(reason), "the negotiated format carries no meter");
        else if (!a->drv_ring_bytes)
            snprintf(reason, sizeof(reason), "stream not ready");
        else if (!a->drv_held_bytes && st->out_absent_run >= st->publishes && st->publishes > 1)
            /* Every publish since this overlay started looking, which is a
             * different statement from one empty period and the one that points
             * at the driver rather than at the title. */
            snprintf(reason, sizeof(reason), "nothing held, every one of %u publishes",
                     st->publishes);
        else if (!a->drv_held_bytes)
            snprintf(reason, sizeof(reason), "nothing held, %u of %u publishes",
                     st->out_absent_run, st->publishes);
        else
            snprintf(reason, sizeof(reason), "unavailable");
        hud_text(HUD_STATE_NA, "out   NO DATA  %s", reason);
        return;
    }

    for (i = 0; i < a->out_channels && i < PWHUD_OUT_MAX; i++)
        if (a->out_peak_db[i] > PWHUD_DB_FLOOR || !std::isfinite(a->out_peak_db[i]))
            all_floor = false;

    hud_text(HUD_STATE_CONFIG, "out   peak, %u ch%s%s", a->out_channels,
             flags & PWHUD_F_OUT_TRUNCATED ? ", +more" : "", all_floor ? ", all at floor" : "");

    while (drawn < a->out_channels && drawn < PWHUD_OUT_MAX)
    {
        ImVec2 origin = ImGui::GetCursorScreenPos();
        int column = 0;

        while (column < 2 && drawn < a->out_channels && drawn < PWHUD_OUT_MAX)
        {
            char label[8];

            snprintf(label, sizeof(label), "ch%u", drawn + 1);
            hud_meter(l, origin, column, label, a->out_peak_db[drawn], st->out_hold[drawn],
                      l->verbose);
            column++;
            drawn++;
        }
        ImGui::Dummy(ImVec2(l->cell_w * 2.0f, l->line));
    }
}

static void hud_bed_rows(const struct hud_layout *l, const struct hud_snapshot_view *view,
                         const struct hud_frame_state *st)
{
    const struct pwhud_snapshot *b = &view->b;
    enum hud_spatial_state spatial = hud_snapshot_spatial_state(view);
    uint32_t mask;
    unsigned row;
    bool bed_fallback;

    if (spatial == HUD_SPATIAL_ABSENT)
    {
        /* Benign, and by far the most common: a title whose audio never goes
         * through ISpatialAudioClient stays here for its whole life.  This row
         * said "never published", which reads as a broken spatial path, and it
         * was reported as one against a process doing nothing wrong.  Worded as
         * a state the process is in rather than as something missing, and kept
         * short because the row is width-constrained. */
        hud_text(HUD_STATE_NA, "bed   no spatial stream");
        return;
    }
    if (spatial == HUD_SPATIAL_NO_MIX)
    {
        /* The fault the old wording hid.  Activation stamps sp_clients on its own, so a
         * stream can be counted and never have mixed a sample, and the bed
         * fields then carry nothing to draw: no meters follow this row. */
        hud_text(HUD_STATE_FAULT, "bed   NO MIX (%u seen)", b->sp_clients);
        return;
    }

    mask = b->sp_bed_mask & ((1u << PWHUD_BED_MAX) - 1u);
    bed_fallback = b->sp_bed_virtualized && !b->sp_hrtf;

    /* rms, because these are an RMS accumulation over the update block while the
     * output meter above is a peak, and equal bar lengths in the two blocks
     * therefore do not mean equal loudness.  pre-gain, because SetVolume is
     * applied later during mixing and never to the buffer these are read from,
     * so a title at a quarter volume shows an unchanged bed. */
    /* One resolved phrase, not two independent words.  The snapshot carries the
     * engine and the request separately, sp_hrtf from stream->engine != 0 and
     * sp_bed_virtualized from the request, and printing them side by side gave
     * "stereo pan, virtualized", which is true twice over and still needs the
     * reader to know the codebase to see that a requested HRTF path fell back to
     * panning.  The driver's own log has the identical defect in a worse form: it
     * announces "HRTF bed virtualization" before the engine is created, so it says
     * that in the fallback too.  Naming the fallback outright is the whole point,
     * and it is coloured as something to notice rather than as configuration. */
    hud_text(bed_fallback ? HUD_STATE_WATCH : HUD_STATE_CONFIG,
             "bed   %s, rms, pre-gain, %u of %u ch%s",
             bed_backend(b->sp_hrtf, b->sp_bed_virtualized),
             (unsigned)__builtin_popcount(mask), PWHUD_BED_MAX,
             hud_snapshot_flags_b(view) & PWHUD_F_BED_TRUNCATED ? ", +more" : "");
    ImGui::SameLine();
    /* dyn_max is the budget the title asked for, so 0/0 means it asked for none
     * rather than that none of none is in use. */
    if (b->sp_dyn_max)
        hud_text(HUD_STATE_LIVE, "obj %u/%u", b->sp_dyn_live, b->sp_dyn_max);
    else
        hud_text(HUD_STATE_NA, "obj none");

    /* The number that decides whether the rows below are loud, drawn on the header
     * rather than as an extra row because it is a property of the set.  Twelve rows
     * at -18 dBFS look like headroom and are already within 7.2 dB of full scale
     * once summed, further still if correlated; reading them as quiet is a mistake
     * this overlay has already caused.  WATCH from -15.5, the point where the sum
     * started agreeing with real clipping. */
    if (mask)
    {
        float sum = hud_snapshot_bed_power_db(view);

        ImGui::SameLine();
        hud_text(sum >= -15.5f ? HUD_STATE_WATCH : HUD_STATE_LIVE, "sum %.1f", sum);
    }

    if (l->verbose && hud_snapshot_have_spatial_counts(view))
    {
        ImGui::SameLine();
        /* clients counts activations, stamped by every activating stream;
         * publishes counts snapshot publishes by the one elected to write the
         * bed, which is one per eleven mix passes, not one per mix.  The pair
         * separates a spatial stream that merely exists from a mixer that is
         * running. */
        hud_text(HUD_STATE_LIVE, "%u client(s), %u publishes", b->sp_clients, b->sp_publishes);
    }

    /* The clip is the only stage in our mixer that changes samples
     * irreversibly, so it gets its own row rather than a corner of the bed
     * line.  Drawn whenever the clip has run, bed or no bed: a title with a
     * dynamic-object budget and no bed at all can still truncate.
     *
     * FAULT only while it is still growing.  A cumulative percentage that
     * stopped moving ten minutes ago is history, and painting history in the
     * fault colour is how a reader learns to ignore the colour. */
    if (hud_snapshot_have_clip_stats(view))
    {
        double pct = b->sp_clip_total
                         ? 100.0 * (double)b->sp_clip_samples / (double)b->sp_clip_total
                         : 0.0;
        uint64_t now = hud_mono_ns();
        bool moving = st->clip_ns && now > st->clip_ns && now - st->clip_ns < HUD_RECENT_NS;
        enum hud_state state = !b->sp_clip_samples ? HUD_STATE_LIVE
                               : moving            ? HUD_STATE_FAULT
                                                   : HUD_STATE_WATCH;

        if (!b->sp_clip_samples)
            hud_text(state, "clip  none, %u pass(es) on the bus", b->sp_bus_passes);
        else if (l->verbose)
            /* Both ratios, because they answer different questions: the sample
             * share is how much of the signal was altered, the pass share is
             * how often it happened, and a title can be high in one and low in
             * the other.  Peak is dB above full scale, so it sizes the gain a
             * limiter would have needed. */
            hud_text(state, "clip  %.3f%% smp, %.1f%% pass, %u eng, peak +%.1f dB", pct,
                     100.0 * (double)b->sp_clip_passes / (double)b->sp_bus_passes,
                     b->sp_clip_engagements, b->sp_clip_peak_db);
        else
            hud_text(state, "clip  %.2f%%, peak +%.1f dB", pct, b->sp_clip_peak_db);

        /* Never folded into the clip count: a NaN is a different fault with a
         * different cause, and it is always worth saying out loud. */
        if (b->sp_clip_nonfinite)
        {
            ImGui::SameLine();
            hud_text(HUD_STATE_FAULT, "NONFINITE %u", b->sp_clip_nonfinite);
        }
    }

    if (!mask)
    {
        hud_text(HUD_STATE_NA, "      no channels");
        return;
    }

    for (row = 0; row < sizeof(hud_bed_grid) / sizeof(*hud_bed_grid); row++)
    {
        const struct hud_bed_row *r = &hud_bed_grid[row];
        int slot[2] = { r->left, r->right };
        ImVec2 origin;
        int column;

        if (!((r->left >= 0 && (mask & (1u << r->left))) ||
              (r->right >= 0 && (mask & (1u << r->right)))))
            continue;

        origin = ImGui::GetCursorScreenPos();
        for (column = 0; column < 2; column++)
        {
            int i = slot[column];

            /* An absent channel leaves its cell empty rather than closing the
             * gap, which is what keeps a channel's position independent of how
             * many others there are. */
            if (i < 0 || !(mask & (1u << i)))
                continue;
            hud_meter(l, origin, column, bed_name((unsigned)i), b->sp_bed_db[i],
                      st->bed_hold[i], l->verbose);
        }
        ImGui::Dummy(ImVec2(l->cell_w * 2.0f, l->line));
    }
}

static void hud_verbose_header(const struct hud_snapshot_view *view,
                               const struct hud_frame_state *st, uint64_t now)
{
    const struct pwhud_snapshot *a = &view->a;
    double age_ms = view->have_a && a->clock_ns && now > a->clock_ns
                        ? (double)(now - a->clock_ns) / 1e6
                        : 0.0;

    hud_text(HUD_STATE_CONFIG, "v%u %u B pid %u  age %.0f ms  samples %llu", a->version, a->size,
             a->writer_pid, age_ms, (unsigned long long)view->samples);
    ImGui::SameLine();
    /* A torn read leaves the previous copy in place, so the numbers on screen are
     * one publish old rather than blank.  That is worth saying out loud. */
    if (view->torn_a_total || view->torn_b_total)
        hud_text(HUD_STATE_WATCH, "torn %llu/%llu", (unsigned long long)view->torn_a_total,
                 (unsigned long long)view->torn_b_total);
    else
        hud_text(HUD_STATE_CONFIG, "torn 0/0");

    /* Publishes this overlay saw, not publishes the driver made.  It samples once
     * per present, so this rate is min(publish rate, frame rate) and saying "Hz"
     * unqualified would be a measurement of the wrong thing.  It is still worth
     * having: below the frame rate it is the driver's timer thread falling behind
     * its own period. */
    if (st->publishes > 1 && st->last_clock_ns > st->first_clock_ns)
        hud_text(HUD_STATE_LIVE, "seen %u publishes at %.1f/s, capped by presents", st->publishes,
                 (double)(st->publishes - 1) * 1e9 /
                     (double)(st->last_clock_ns - st->first_clock_ns));
    else
        hud_text(HUD_STATE_NA, "no publish rate yet");
}

void hud_build_frame(struct swapchain_data *swapchain_data)
{
    const struct hud_snapshot_view *view = &swapchain_data->snapshot;
    struct hud_frame_state *st = &swapchain_data->frame;
    uint64_t now = hud_mono_ns();
    int level = hud_view_level();
    struct hud_layout l;
    const ImDrawData *draw_data;

    ImGui::NewFrame();

    if (level == HUD_VIEW_OFF)
    {
        /* An empty frame, not a skipped one: the context still owes NewFrame a
         * Render, and a draw data with no vertices makes the present path pass
         * the application's own frame straight through. */
        ImGui::EndFrame();
        ImGui::Render();
        return;
    }

    hud_history_update(st, view);
    hud_frame_stale = hud_snapshot_idle(view, now);
    hud_layout_init(&l, level >= HUD_VIEW_VERBOSE);

    ImGui::SetNextWindowPos(ImVec2(8.0f, 8.0f), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.65f);
    ImGui::Begin("winepipewire", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                     ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing |
                     ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings);

    if (!view->samples)
        hud_text(HUD_STATE_NA, "winepipewire: no snapshot for this process");
    else if (!view->have_a || !view->a.clock_ns)
        hud_text(HUD_STATE_NA, "winepipewire: driver has not published yet");
    else
    {
        if (hud_frame_stale)
            hud_text(HUD_STATE_FAULT, "IDLE  last publish %.1f s ago, values frozen",
                     (double)(now - view->a.clock_ns) / 1e9);
        if (l.verbose)
            hud_verbose_header(view, st, now);
        hud_config_rows(&l, view);
        ImGui::Separator();
        hud_live_rows(&l, view, st, now);
        hud_out_rows(&l, view, st);
        ImGui::Separator();
        hud_bed_rows(&l, view, st);
        if (l.verbose)
        {
            hud_text(HUD_STATE_CONFIG, "meters: bar is the level, %.0f dBFS left to 0 right",
                     (double)HUD_FLOOR_DB);
            hud_text(HUD_STATE_CONFIG, "ticks %.0f and %.0f, pip is a %.1f s hold",
                     (double)hud_ticks_db[0], (double)hud_ticks_db[1],
                     (double)HUD_HOLD_NS / 1e9);
            /* Two lines, not one: the single long form pushed the drawn region
             * past three quarters of the window and the present smoke test
             * caught it. */
            hud_text(HUD_STATE_CONFIG, "str counts this process,");
            hud_text(HUD_STATE_CONFIG, "#id/n is the metered stream of its group");
        }
        /* A courtesy credit, not a licence obligation: Steam Audio ships no NOTICE
         * file, upstream or in the SDK, so Apache-2.0 section 4(d) is not engaged,
         * and the tool already carries the licence and a verbatim copy of the
         * upstream third-party notices at its root.
         *
         * Gated on the engine actually running, not on the feature existing or the
         * bed asking for it: in the panning fallback no sample goes through Steam
         * Audio, and a credit there would be a lying display of the same kind this
         * component keeps removing.  No version number, because the layer cannot
         * observe which libphonon loaded and a hardcoded one would be a claim it
         * cannot make.
         *
         * In both views, not just the verbose one.  Compact is dense because every
         * row there has to earn its place against gameplay, but that argument is
         * about telemetry competing for space and an attribution line is not
         * competing: it is the last row, it costs one line height, it appears only
         * when the engine is genuinely running, and a credit only visible in a view
         * nobody runs during play does not discharge the intent of asking for one. */
        if (hud_snapshot_spatial_published(view) && view->b.sp_hrtf)
            hud_text(HUD_STATE_CONFIG, "HRTF powered by Steam Audio");
    }

    ImGui::End();
    ImGui::EndFrame();
    ImGui::Render();

    if ((draw_data = ImGui::GetDrawData()))
    {
        st->last_vtx = (uint32_t)draw_data->TotalVtxCount;
        st->last_idx = (uint32_t)draw_data->TotalIdxCount;
    }
}
