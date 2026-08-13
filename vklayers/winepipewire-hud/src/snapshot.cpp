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

const char *dispatch_name(uint32_t dispatch)
{
    switch (dispatch)
    {
    case PWHUD_DISPATCH_DATA: return "data";
    case PWHUD_DISPATCH_LOOP: return "loop";
    default:                  return "unknown";
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
    snprintf(buf, len, "%s%s%s%s%s", flags & PWHUD_F_CAPTURE ? "C" : "R",
             flags & PWHUD_F_GRID_VALID ? ",grid" : ",nogrid",
             flags & PWHUD_F_OUT_TRUNCATED ? ",trunc" : "",
             flags & PWHUD_F_OUT_NO_METER ? ",nometer" : "",
             flags & PWHUD_F_NO_DSP_LOAD ? ",nodsp" : "");
}

/* Seqlock A.  An odd sequence means a publish is in flight, and a sequence that
 * moved during the copy means one landed inside it; either way the payload is
 * discarded rather than mixed. */
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
     * A newer producer may have appended fields, hence >= and not ==. */
    if (__atomic_load_n(&snap->magic, __ATOMIC_ACQUIRE) != PWHUD_MAGIC ||
        snap->version > PWHUD_VERSION || snap->size < sizeof(*snap))
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

    HUD_LOG(HUD_LOG_LIFECYCLE, "reading %s, writer pid %u, version %u, size %u%s", path,
            mapped->writer_pid, mapped->version, mapped->size,
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

bool hud_snapshot_dsp_load_valid(const struct hud_snapshot_view *view)
{
    return view->have_a && !(hud_snapshot_flags_a(view) & PWHUD_F_NO_DSP_LOAD);
}

void hud_snapshot_log(const struct hud_snapshot_view *view)
{
    const struct pwhud_snapshot *a = &view->a;
    const struct pwhud_snapshot *b = &view->b;
    uint64_t now = hud_mono_ns();
    char flags[32];
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

        flags_a_str(hud_snapshot_flags_a(view), flags, sizeof(flags));
        if (hud_snapshot_dsp_load_valid(view))
            snprintf(dsp, sizeof(dsp), "%.1f%%", 100.0 * (double)a->pw_dsp_load);
        else
            snprintf(dsp, sizeof(dsp), "n/a");
        /* sinkxrun is the graph driver node's, shared with every client on that
         * device; under is our own ring starving.  Different failures. */
        len = snprintf(line, sizeof(line),
                       "drv seq %u age %.1fms %s quantum %u rate %u period %uus streams %u "
                       "dispatch %s dsp %s sinkxrun %u ring %.1f%% (%llu/%llu) jitter %+lldus "
                       "under %u over %u bad %u",
                       a->seq_drv / 2, (double)(now - a->clock_ns) / 1e6, flags, a->pw_quantum,
                       a->pw_rate, a->drv_period_usec, a->pw_stream_count,
                       dispatch_name(a->drv_dispatch), dsp, a->pw_xruns,
                       a->drv_ring_bytes ? 100.0 * (double)a->drv_held_bytes /
                                               (double)a->drv_ring_bytes
                                         : 0.0,
                       (unsigned long long)a->drv_held_bytes,
                       (unsigned long long)a->drv_ring_bytes,
                       (long long)a->drv_phase_adjust_us, a->drv_underruns, a->drv_overruns,
                       a->drv_bad_buffers);

        if (a->out_channels)
            for (i = 0; i < a->out_channels && i < PWHUD_OUT_MAX && len > 0 &&
                        len < (int)sizeof(line);
                 i++)
                len += snprintf(line + len, sizeof(line) - len, "%s%.1f", i ? " " : " peak ",
                                a->out_peak_db[i]);
        else if (len > 0 && len < (int)sizeof(line))
        {
            /* out_channels 0 has three causes and they are different diagnoses:
             * the driver only meters render streams, the negotiated format may
             * carry no meter at all, and a render stream with nothing queued is
             * genuinely silent. */
            uint32_t flags_a = hud_snapshot_flags_a(view);

            len += snprintf(line + len, sizeof(line) - len, "%s",
                            flags_a & PWHUD_F_CAPTURE     ? " peak n/a on capture"
                            : flags_a & PWHUD_F_OUT_NO_METER ? " peak unmetered"
                                                             : " peak silent");
        }
        if (hud_snapshot_flags_a(view) & PWHUD_F_OUT_TRUNCATED && len > 0 &&
            len < (int)sizeof(line))
            len += snprintf(line + len, sizeof(line) - len, " +more channels");

        hud_logf("%s%s%s", line, hud_snapshot_idle(view, now) ? " IDLE" : "",
                 view->torn_a ? " TORN, previous copy" : "");
    }

    if (!hud_snapshot_spatial_published(view))
    {
        /* Never published is not a bed of zeroes and not a bed at the floor: a
         * process whose audio never goes through ISpatialAudioClient stays here
         * for its whole life, and rendering it as levels would be a lie. */
        hud_logf("spatial: never published%s", view->torn_b ? " (last read torn)" : "");
        return;
    }

    len = snprintf(line, sizeof(line), "spatial seq %u hrtf %u bedvirt %u dyn %u/%u mask 0x%04x%s",
                   b->seq_sp / 2, b->sp_hrtf, b->sp_bed_virtualized, b->sp_dyn_live, b->sp_dyn_max,
                   b->sp_bed_mask,
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
