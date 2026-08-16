/*
 * SPDX-License-Identifier: MIT
 */
#include "snapshot.hpp"

#include "log.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

/* Retries cost one open() and are on the present path, so they are rate
 * limited: a process with no audio at all would otherwise pay per frame. */
#define HUD_SNAPSHOT_RETRY_NS 1000000000ull

/* Same tokens as the driver's report_dispatch_mode at pipewire.c:2917, so one grep
 * finds the mode in the driver's log and in this one.  These were "data" and
 * "loop", which matched neither. */
const char *dispatch_name(uint32_t dispatch)
{
    switch (dispatch)
    {
    case PWHUD_DISPATCH_DATA: return "data-thread";
    case PWHUD_DISPATCH_LOOP: return "driver-loop";
    default:                  return "not-latched";
    }
}

const char *bed_name(unsigned i)
{
    static const char *const names[] = { "FL", "FR", "FC", "LFE", "SL", "SR", "BL", "BR",
                                         "TFL", "TFR", "TBL", "TBR", "BFL", "BFR", "BBL",
                                         "BBR", "BC" };

    return i < sizeof(names) / sizeof(*names) ? names[i] : "?";
}

/* R or C for the dataflow, then the section A bits that change how the rest of
 * the line reads.  PWHUD_F_BED_TRUNCATED is deliberately absent: it belongs to
 * section B and is validated by seq_sp, not by seq_drv. */
void flags_a_str(uint32_t flags, char *buf, size_t len)
{
    snprintf(buf, len, "%s%s%s%s%s%s", flags & PWHUD_F_CAPTURE ? "C" : "R",
             flags & PWHUD_F_GRID_VALID ? ",grid" : ",nogrid",
             flags & PWHUD_F_OUT_TRUNCATED ? ",trunc" : "",
             flags & PWHUD_F_OUT_NO_METER ? ",nometer" : "",
             flags & PWHUD_F_NO_DSP_LOAD ? ",nodsp" : "",
             flags & PWHUD_F_STR_TRUNCATED ? ",strtrunc" : "");
}

/* Seqlock A.  An odd sequence means a publish is in flight, and a sequence that
 * moved during the copy means one landed inside it; either way the payload is
 * discarded rather than mixed.
 *
 * The copy is the whole struct, so a field appended to either section rides along
 * with no change here.  What that does not decide is which copy a field may be
 * read from: only section A's fields are validated by this sequence, so only they
 * may be taken from this copy. */
bool read_section_a(const struct pwhud_snapshot *snap, struct pwhud_snapshot *out)
{
    for (int attempt = 0; attempt < 4; attempt++)
    {
        uint32_t seq = __atomic_load_n(&snap->seq_drv, __ATOMIC_ACQUIRE);

        if (seq & 1u)
            continue;
        memcpy(out, snap, sizeof(*out));
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (__atomic_load_n(&snap->seq_drv, __ATOMIC_RELAXED) == seq)
            return true;
    }
    return false;
}

/* Seqlock B, and a separate function rather than a parameter on the first: the
 * two sections have one writer each on opposite sides of the PE/unix boundary,
 * and a good read of one says nothing about the other. */
bool read_section_b(const struct pwhud_snapshot *snap, struct pwhud_snapshot *out)
{
    for (int attempt = 0; attempt < 4; attempt++)
    {
        uint32_t seq = __atomic_load_n(&snap->seq_sp, __ATOMIC_ACQUIRE);

        if (seq & 1u)
            continue;
        memcpy(out, snap, sizeof(*out));
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (__atomic_load_n(&snap->seq_sp, __ATOMIC_RELAXED) == seq)
            return true;
    }
    return false;
}

} /* namespace */

bool hud_snapshot_path(char *buf, size_t len)
{
    const char *override_pid = getenv(HUD_ENV_PID);
    const char *home = getenv("HOME");
    long pid = override_pid && *override_pid ? strtol(override_pid, nullptr, 10) : 0;

    if (!home || !*home)
        return false;
    if (pid <= 0)
        pid = (long)getpid();
    return snprintf(buf, len, "%s%s/%s%ld", home, PWHUD_DIR_SUFFIX, PWHUD_FILE_PREFIX, pid) <
           (int)len;
}

const struct pwhud_snapshot *hud_snapshot_open_path(const char *path)
{
    const struct pwhud_snapshot *snap;
    struct stat st;
    int fd;

    if ((fd = open(path, O_RDONLY | O_CLOEXEC)) < 0)
        return nullptr;
    if (fstat(fd, &st) || (size_t)st.st_size < sizeof(*snap))
    {
        HUD_LOG(HUD_LOG_LIFECYCLE, "%s is %lld bytes, too short for a snapshot", path,
                (long long)st.st_size);
        close(fd);
        return nullptr;
    }
    snap = (const struct pwhud_snapshot *)mmap(nullptr, PWHUD_BYTES, PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    if (snap == MAP_FAILED)
    {
        HUD_LOG(HUD_LOG_LIFECYCLE, "cannot map %s: %s", path, strerror(errno));
        return nullptr;
    }
    /* The creator stamps magic last, so a valid magic means the header landed.
     * Validated against the version 1 baseline and not against this reader's own
     * sizeof, in both directions: a newer writer appends fields this build does
     * not know, and an older writer is shorter than this build's struct while
     * still carrying every field the baseline promises.  Testing sizeof would
     * reject the second case, which is the one the append rule exists for. */
    if (__atomic_load_n(&snap->magic, __ATOMIC_ACQUIRE) != PWHUD_MAGIC ||
        snap->version > PWHUD_VERSION || snap->size < PWHUD_SIZE_V1_BASE)
    {
        HUD_LOG(HUD_LOG_LIFECYCLE, "%s is not a usable snapshot (magic %#x version %u size %u)",
                path, snap->magic, snap->version, snap->size);
        munmap((void *)snap, PWHUD_BYTES);
        return nullptr;
    }
    return snap;
}

const struct pwhud_snapshot *hud_snapshot_map(void)
{
    static std::mutex lock;
    static const struct pwhud_snapshot *mapped;
    static uint64_t next_attempt_ns;
    std::lock_guard<std::mutex> guard(lock);
    uint64_t now = hud_mono_ns();
    char path[4096];

    if (mapped)
        return mapped;
    if (next_attempt_ns && now < next_attempt_ns)
        return nullptr;
    next_attempt_ns = now + HUD_SNAPSHOT_RETRY_NS;

    if (!hud_snapshot_path(path, sizeof(path)))
    {
        HUD_LOG(HUD_LOG_LIFECYCLE, "cannot build a snapshot path, is HOME set");
        return nullptr;
    }
    if (!(mapped = hud_snapshot_open_path(path)))
        return nullptr;

    HUD_LOG(HUD_LOG_LIFECYCLE, "reading %s, writer pid %u, version %u, size %u%s%s", path,
            mapped->writer_pid, mapped->version, mapped->size,
            mapped->size < sizeof(*mapped) ? " (older writer, trailing fields absent)" : "",
            getenv(HUD_ENV_PID) ? " (pid overridden, not this process)" : "");
    return mapped;
}

void hud_snapshot_sample(struct hud_snapshot_view *view, const struct pwhud_snapshot *snap)
{
    struct pwhud_snapshot copy;

    view->samples++;

    /* A torn section keeps the copy from the last good read.  The consumer is on
     * the present path: it must never block, never spin and never blank. */
    if ((view->torn_a = !read_section_a(snap, &copy)))
        view->torn_a_total++;
    else
    {
        view->a = copy;
        view->have_a = true;
    }

    if ((view->torn_b = !read_section_b(snap, &copy)))
        view->torn_b_total++;
    else
    {
        view->b = copy;
        view->have_b = true;
    }
}

/* Each publisher compare-exchanges only its own mask, and each writes the word
 * inside its own seqlock, so the bits are trustworthy exactly when taken from
 * that section's copy and masked to that section's owner. */
uint32_t hud_snapshot_flags_a(const struct hud_snapshot_view *view)
{
    return view->a.flags & PWHUD_F_MASK_A;
}

uint32_t hud_snapshot_flags_b(const struct hud_snapshot_view *view)
{
    return view->b.flags & PWHUD_F_MASK_B;
}

bool hud_snapshot_idle(const struct hud_snapshot_view *view, uint64_t now_ns)
{
    if (!view->have_a || !view->a.clock_ns)
        return true;
    return now_ns - view->a.clock_ns > HUD_SNAPSHOT_STALE_NS;
}

bool hud_snapshot_spatial_published(const struct hud_snapshot_view *view)
{
    return view->have_b && view->b.seq_sp != 0;
}

bool hud_snapshot_have_stream_scope(const struct hud_snapshot_view *view)
{
    return view->have_a && view->a.size >= HUD_SNAPSHOT_THROUGH(drv_group_streams);
}

bool hud_snapshot_have_spatial_counts(const struct hud_snapshot_view *view)
{
    return view->have_b && view->b.size >= HUD_SNAPSHOT_THROUGH(sp_publishes);
}

bool hud_snapshot_have_stream_meters(const struct hud_snapshot_view *view)
{
    return view->have_a && view->a.size >= HUD_SNAPSHOT_THROUGH(drv_str);
}

enum hud_spatial_state hud_snapshot_spatial_state(const struct hud_snapshot_view *view)
{
    /* An older writer has neither counter, so fall back to the rule that held
     * before they existed: only a mix publish moved seq_sp, so a moved
     * sequence does mean live and a still one means nothing ever published. */
    if (!hud_snapshot_have_spatial_counts(view))
        return hud_snapshot_spatial_published(view) ? HUD_SPATIAL_LIVE : HUD_SPATIAL_ABSENT;

    /* sp_clients is stamped at activation and is deliberately outside seqlock
     * B, because every activating stream writes it while only the elected one
     * writes the bed.  It is therefore the authority on whether a spatial
     * client exists, and seq_sp is not: an activation no longer moves the
     * sequence at all. */
    if (!view->b.sp_clients)
        return HUD_SPATIAL_ABSENT;
    return view->b.sp_publishes ? HUD_SPATIAL_LIVE : HUD_SPATIAL_NO_MIX;
}

bool hud_snapshot_dsp_load_valid(const struct hud_snapshot_view *view)
{
    return view->have_a && !(hud_snapshot_flags_a(view) & PWHUD_F_NO_DSP_LOAD);
}

void hud_snapshot_log(const struct hud_snapshot_view *view)
{
    const struct pwhud_snapshot *a = &view->a;
    const struct pwhud_snapshot *b = &view->b;
    enum hud_spatial_state spatial = hud_snapshot_spatial_state(view);
    uint64_t now = hud_mono_ns();
    char flags[48];
    char counts[40];
    char line[512];
    int len;
    unsigned i;

    if (!view->have_a)
    {
        hud_logf("drv: no torn-free read yet after %llu samples",
                 (unsigned long long)view->samples);
    }
    else if (!a->clock_ns)
    {
        hud_logf("drv: mapped, no publish yet");
    }
    else
    {
        char dsp[16];
        char scope[96];

        flags_a_str(hud_snapshot_flags_a(view), flags, sizeof(flags));
        if (hud_snapshot_dsp_load_valid(view))
            snprintf(dsp, sizeof(dsp), "%.1f%%", 100.0 * (double)a->pw_dsp_load);
        else
            snprintf(dsp, sizeof(dsp), "n/a");
        /* Which stream section A is about.  Every field on this line except
         * pw_stream_count and sinkxrun describes one stream, and nothing here used
         * to say which one, so one stream's peaks read as the whole process's
         * output.  streams is the process-wide count, showing is the elected
         * stream inside its period group: the two are different scopes and the
         * ratio only makes sense against the group. */
        if (!hud_snapshot_have_stream_scope(view))
            snprintf(scope, sizeof(scope), "unavailable, writer predates the field");
        else if (!a->drv_stream_id)
            snprintf(scope, sizeof(scope), "no stream elected");
        else
            snprintf(scope, sizeof(scope), "stream id %u of the %u started in its period group",
                     a->drv_stream_id, a->drv_group_streams);
        /* sinkxrun is the graph driver node's, shared with every client on that
         * device; under is our own ring starving.  Different failures. */
        len = snprintf(line, sizeof(line),
                       "drv seq %u age %.1fms %s quantum %u rate %u period %uus "
                       "streams %u in process showing %s "
                       "dispatch %s dsp %s sinkxrun %u ring %.1f%% (%llu/%llu) jitter %+lldus "
                       "under %u over %u bad %u resync %u",
                       a->seq_drv / 2, (double)(now - a->clock_ns) / 1e6, flags, a->pw_quantum,
                       a->pw_rate, a->drv_period_usec, a->pw_stream_count, scope,
                       dispatch_name(a->drv_dispatch), dsp, a->pw_xruns,
                       a->drv_ring_bytes ? 100.0 * (double)a->drv_held_bytes /
                                               (double)a->drv_ring_bytes
                                         : 0.0,
                       (unsigned long long)a->drv_held_bytes,
                       (unsigned long long)a->drv_ring_bytes,
                       (long long)a->drv_phase_adjust_us, a->drv_underruns, a->drv_overruns,
                       a->drv_bad_buffers, a->drv_ring_resyncs);

        if (a->out_channels)
            for (i = 0; i < a->out_channels && i < PWHUD_OUT_MAX && len > 0 &&
                        len < (int)sizeof(line);
                 i++)
                len += snprintf(line + len, sizeof(line) - len, "%s%.1f", i ? " " : " peak ",
                                a->out_peak_db[i]);
        else if (len > 0 && len < (int)sizeof(line))
        {
            /* Mirrors the overlay row: none of these is a measurement, and a
             * render stream with nothing queued is not "genuinely silent", it
             * is unmeasured.  Real silence arrives as PWHUD_DB_FLOOR on a
             * non-zero out_channels and is printed by the branch above. */
            uint32_t flags_a = hud_snapshot_flags_a(view);

            len += snprintf(line + len, sizeof(line) - len, "%s",
                            flags_a & PWHUD_F_CAPTURE        ? " peak n/a on capture"
                            : flags_a & PWHUD_F_OUT_NO_METER ? " peak unmetered"
                            : !a->drv_held_bytes             ? " peak no data this tick"
                                                             : " peak unavailable");
        }
        if (hud_snapshot_flags_a(view) & PWHUD_F_OUT_TRUNCATED && len > 0 &&
            len < (int)sizeof(line))
            len += snprintf(line + len, sizeof(line) - len, " +more channels");

        hud_logf("%s%s%s", line, hud_snapshot_idle(view, now) ? " IDLE" : "",
                 view->torn_a ? " TORN, previous copy" : "");

        if (hud_snapshot_have_stream_meters(view))
        {
            unsigned s, n = a->drv_str_count < PWHUD_STR_MAX ? a->drv_str_count : PWHUD_STR_MAX;

            for (s = 0; s < n; s++)
            {
                const struct pwhud_str *str = &a->drv_str[s];
                char peaks[128];
                unsigned c, plen = 0;

                if (!str->channels)
                {
                    hud_logf("str %u%s: no data", str->id,
                             str->id && str->id == a->drv_stream_id ? " elected" : "");
                    continue;
                }
                peaks[0] = 0;
                for (c = 0; c < str->channels && c < PWHUD_OUT_MAX && plen < sizeof(peaks); c++)
                    plen += snprintf(peaks + plen, sizeof(peaks) - plen, "%s%.1f",
                                     c ? " " : "", str->peak_db[c]);
                hud_logf("str %u%s: ch %u %s", str->id,
                         str->id && str->id == a->drv_stream_id ? " elected" : "",
                         str->channels, peaks);
            }
            if (hud_snapshot_flags_a(view) & PWHUD_F_STR_TRUNCATED)
                hud_logf("str: +more");
        }
    }

    if (spatial == HUD_SPATIAL_ABSENT)
    {
        /* Benign, and the common case: a process whose audio never goes through
         * ISpatialAudioClient stays here for its whole life.  This said "never
         * published", which reads as a fault, and a report of a broken spatial
         * path turned out to be exactly this state and nothing else. */
        hud_logf("spatial: inactive, no spatial stream in this process%s",
                 view->torn_b ? " (last read torn)" : "");
        return;
    }
    if (spatial == HUD_SPATIAL_NO_MIX)
    {
        /* The fault the old wording hid.  An activation stamps sp_clients and
         * returns without entering seqlock B, so this state still has seq_sp 0 and
         * sp_publishes 0: nothing has ever been written into the bed fields, and
         * there is nothing to draw rather than something stale to draw. */
        hud_logf("spatial: %u client(s) activated but no mix published%s", b->sp_clients,
                 view->torn_b ? " (last read torn)" : "");
        return;
    }

    /* Both printed, and they are expected to be equal: only the elected stream's
     * mix moves seq_sp, and sp_publishes is incremented inside that same protected
     * region, so seq_sp/2 == sp_publishes on every torn-free copy.  That is the
     * point of printing both rather than a reason to drop one.  Deciding whether
     * this copy was torn is the whole job of the reader above, and these are two
     * fields from inside one seqlock that must agree, so a line where they differ
     * says the reader accepted a copy it should have rejected.  It is the cheapest
     * assertion available that the seqlock discipline is working, and it sits in
     * the log where such a bug would first show.  Do not delete one as redundant. */
    if (hud_snapshot_have_spatial_counts(view))
        snprintf(counts, sizeof(counts), "clients %u mixes %u", b->sp_clients, b->sp_publishes);
    else
        snprintf(counts, sizeof(counts), "clients n/a mixes n/a");

    len = snprintf(line, sizeof(line),
                   "spatial seq %u %s hrtf %u bedvirt %u dyn %u/%u mask 0x%04x%s",
                   b->seq_sp / 2, counts, b->sp_hrtf, b->sp_bed_virtualized, b->sp_dyn_live,
                   b->sp_dyn_max, b->sp_bed_mask,
                   hud_snapshot_flags_b(view) & PWHUD_F_BED_TRUNCATED ? ",bedtrunc" : "");

    if (!b->sp_bed_mask)
        len += snprintf(line + len, sizeof(line) - len, " no bed channels");
    else
    {
        unsigned printed = 0;

        for (i = 0; i < PWHUD_BED_MAX && len > 0 && len < (int)sizeof(line); i++)
            if (b->sp_bed_mask & (1u << i))
                len += snprintf(line + len, sizeof(line) - len, "%s%s %.1f",
                                printed++ ? " " : " bed ", bed_name(i), b->sp_bed_db[i]);
    }

    hud_logf("%s%s", line, view->torn_b ? " TORN, previous copy" : "");
}
