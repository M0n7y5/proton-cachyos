/*
 * Exercises the snapshot reader against a controlled publisher, including the
 * three degraded cases the renderer must survive: a torn section keeps the
 * previous copy, a stale clock reads as idle rather than as frozen numbers, and
 * a section B that never published is not a bed of zeroes.
 *
 * Exit 0 all cases pass, 1 a case failed, 77 skipped.
 *
 * SPDX-License-Identifier: MIT
 */
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "../src/log.hpp"
#include "../src/snapshot.hpp"
#include "hud_publisher.hpp"

static int failures;

static void check(bool ok, const char *what)
{
    printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok)
        failures++;
}

/* Mirrors the driver's own section A publish: everything inside seqlock A, and
 * only PWHUD_F_MASK_A touched in the shared flags word. */
static void publish_a(struct hud_publisher &pub, uint64_t held, int64_t phase, uint32_t bits,
                      uint64_t clock_ns)
{
    struct pwhud_snapshot *snap = pub.snap;

    pub.a_begin();
    snap->clock_ns = clock_ns;
    pwhud_flags_publish(snap, PWHUD_F_MASK_A, bits);
    snap->pw_quantum = 1024;
    snap->pw_rate = 48000;
    /* The graph driver node's xruns, distinct from drv_underruns below. */
    snap->pw_xruns = 2;
    snap->pw_dsp_load = 0.0f;
    snap->pw_stream_count = 2;
    snap->drv_dispatch = PWHUD_DISPATCH_LOOP;
    snap->drv_underruns = 249;
    snap->drv_overruns = 1;
    snap->drv_bad_buffers = 0;
    snap->drv_period_usec = 21333;
    snap->drv_held_bytes = held;
    snap->drv_ring_bytes = 24576;
    snap->drv_period_bytes = 4096;
    snap->drv_phase_adjust_us = phase;
    snap->out_peak_db[0] = -6.0f;
    snap->out_peak_db[1] = -12.5f;
    for (unsigned i = 2; i < PWHUD_OUT_MAX; i++)
        snap->out_peak_db[i] = PWHUD_DB_FLOOR;
    snap->out_channels = 2;
    pub.a_end();
}

/* Mirrors the PE spatial publisher: seqlock B, and only PWHUD_F_MASK_B. */
static void publish_b(struct hud_publisher &pub, uint32_t bed_mask, uint32_t bits)
{
    struct pwhud_snapshot *snap = pub.snap;

    pub.b_begin();
    snap->sp_hrtf = 1;
    snap->sp_bed_virtualized = 1;
    snap->sp_bed_mask = bed_mask;
    snap->sp_dyn_live = 3;
    snap->sp_dyn_max = 128;
    for (unsigned i = 0; i < PWHUD_BED_MAX; i++)
        snap->sp_bed_db[i] = bed_mask & (1u << i) ? -15.0f - (float)i : PWHUD_DB_FLOOR;
    pwhud_flags_publish(snap, PWHUD_F_MASK_B, bits);
    pub.b_end();
}

int main(void)
{
    struct hud_snapshot_view view = {};
    struct hud_publisher pub;
    const struct pwhud_snapshot *snap;
    uint64_t now;

    if (!getenv("HOME"))
    {
        printf("snapshot_probe: HOME unset, skipping\n");
        return 77;
    }
    if (!pub.open())
        return 1;
    printf("snapshot_probe: publishing into %s\n", pub.path);

    now = hud_mono_ns();
    publish_a(pub, 12288, -12, PWHUD_F_GRID_VALID, now);
    publish_b(pub, (1u << 0) | (1u << 1) | (1u << 4), PWHUD_F_BED_TRUNCATED);

    /* The production discovery path: own pid, no override, same builder the
     * publisher used. */
    if (!(snap = hud_snapshot_map()))
    {
        printf("FAIL hud_snapshot_map found nothing at %s\n", pub.path);
        pub.close_and_unlink();
        return 1;
    }
    check(true, "mapped through the production own-pid discovery path");
    check(snap->size >= sizeof(*snap), "size is validated with >= and not ==, so a longer "
                                       "producer struct is still readable");

    hud_snapshot_sample(&view, snap);
    check(view.have_a && !view.torn_a, "section A read torn-free");
    check(view.have_b && !view.torn_b, "section B read torn-free");
    check(view.a.pw_quantum == 1024 && view.a.pw_rate == 48000 && view.a.drv_held_bytes == 12288 &&
              view.a.drv_phase_adjust_us == -12 && view.a.out_channels == 2,
          "section A values match what was published");
    check(view.b.sp_hrtf == 1 && view.b.sp_dyn_live == 3 && view.b.sp_dyn_max == 128 &&
              view.b.sp_bed_mask == ((1u << 0) | (1u << 1) | (1u << 4)),
          "section B values match what was published");
    check(view.a.pw_xruns == 2 && view.a.drv_underruns == 249,
          "the graph driver node's xruns and our ring underruns are separate counters");
    check(hud_snapshot_dsp_load_valid(&view),
          "a clear PWHUD_F_NO_DSP_LOAD means pw_dsp_load may be shown");

    /* The partitioned flags word: each side owns its mask and neither publisher
     * may erase the other's bits. */
    check(hud_snapshot_flags_a(&view) == PWHUD_F_GRID_VALID,
          "section A view decodes only PWHUD_F_MASK_A");
    check((hud_snapshot_flags_a(&view) & PWHUD_F_BED_TRUNCATED) == 0,
          "section A view does not claim section B's bit");
    check(hud_snapshot_flags_b(&view) == PWHUD_F_BED_TRUNCATED,
          "section B view decodes only PWHUD_F_MASK_B");
    check(hud_snapshot_spatial_published(&view), "section B reads as published once seq_sp moved");
    check(!hud_snapshot_idle(&view, hud_mono_ns()), "a fresh clock_ns does not read as idle");
    hud_snapshot_log(&view);

    /* An unmeasured DSP load, which is what the driver publishes today: the
     * value is 0.0 and rendering it as 0% would be a measurement nobody took. */
    publish_a(pub, 12288, -12, PWHUD_F_GRID_VALID | PWHUD_F_NO_DSP_LOAD, hud_mono_ns());
    hud_snapshot_sample(&view, snap);
    check(!hud_snapshot_dsp_load_valid(&view),
          "PWHUD_F_NO_DSP_LOAD makes pw_dsp_load unavailable rather than zero");
    check(view.a.pw_dsp_load == 0.0f, "the unavailable load is in fact published as zero");
    check((hud_snapshot_flags_b(&view) & PWHUD_F_NO_DSP_LOAD) == 0,
          "the new bit belongs to section A and does not appear in section B's view");
    hud_snapshot_log(&view);

    /* Degraded case 1: a writer caught mid-update.  The reader must report the
     * tear and keep the previous copy rather than showing the half-written one
     * or blanking the display. */
    pub.a_begin();
    pub.snap->drv_held_bytes = 999;
    pub.snap->pw_quantum = 4096;
    hud_snapshot_sample(&view, snap);
    check(view.torn_a, "a publish in flight is reported as torn");
    check(view.a.drv_held_bytes == 12288 && view.a.pw_quantum == 1024,
          "a torn section A keeps the previous copy, not the half-written values");
    check(!view.torn_b, "a torn section A does not disturb section B");
    check(view.torn_a_total == 1, "the torn read is counted");
    pub.a_end();
    hud_snapshot_sample(&view, snap);
    check(!view.torn_a && view.a.drv_held_bytes == 999 && view.a.pw_quantum == 4096,
          "the next torn-free read picks up the completed publish");
    hud_snapshot_log(&view);

    /* Degraded case 2: the producer stopped publishing.  Idle, not frozen. */
    publish_a(pub, 12288, -12, PWHUD_F_GRID_VALID, hud_mono_ns() - 2 * HUD_SNAPSHOT_STALE_NS);
    hud_snapshot_sample(&view, snap);
    check(hud_snapshot_idle(&view, hud_mono_ns()), "a clock_ns two seconds old reads as idle");
    hud_snapshot_log(&view);

    /* Also idle before the first publish ever lands, where clock_ns is 0 and
     * every other field is a zero that must not be read as a measurement. */
    {
        struct hud_snapshot_view fresh = {};

        check(hud_snapshot_idle(&fresh, hud_mono_ns()), "a view with no read yet is idle");
        check(!hud_snapshot_spatial_published(&fresh), "a view with no read yet is not published");
    }

    /* Degraded case 3: section B never published.  seq_sp 0 is unambiguous
     * because the producer's first write takes it to 2. */
    {
        struct hud_snapshot_view unpublished = {};

        pub.b_begin();
        memset(&pub.snap->sp_hrtf, 0, sizeof(struct pwhud_snapshot) -
                                          offsetof(struct pwhud_snapshot, sp_hrtf));
        pwhud_flags_publish(pub.snap, PWHUD_F_MASK_B, 0);
        pub.b_end();
        __atomic_store_n(&pub.snap->seq_sp, 0u, __ATOMIC_RELEASE);

        hud_snapshot_sample(&unpublished, snap);
        check(unpublished.have_b && !unpublished.torn_b,
              "a zero seq_sp is an even sequence and reads torn-free");
        check(!hud_snapshot_spatial_published(&unpublished),
              "seq_sp 0 reads as never published, not as a bed of zeroes");
        check(hud_snapshot_flags_b(&unpublished) == 0,
              "section B's own bit is clear once the publisher cleared it");
        check(unpublished.have_a, "section A still reads while section B is unpublished");
        hud_snapshot_log(&unpublished);
    }

    /* The state between the two: section B has published, and the bed is empty.
     * Distinct from never published and from a bed sitting at the floor, and none
     * of the three may be rendered as the others. */
    {
        struct hud_snapshot_view empty_bed = {};

        publish_b(pub, 0, 0);
        hud_snapshot_sample(&empty_bed, snap);
        check(hud_snapshot_spatial_published(&empty_bed),
              "a published section B with no bed channels still reads as published");
        check(empty_bed.b.sp_bed_mask == 0, "and its bed mask is empty");
        check(empty_bed.b.sp_bed_db[0] == PWHUD_DB_FLOOR,
              "its unused bed slots hold the floor, which is why the mask and not the "
              "level decides what is shown");
        hud_snapshot_log(&empty_bed);
    }

    /* A file that is not a snapshot is refused rather than misparsed. */
    {
        uint32_t magic = pub.snap->magic;
        char short_path[4096];
        int fd;

        __atomic_store_n(&pub.snap->magic, 0u, __ATOMIC_RELEASE);
        check(hud_snapshot_open_path(pub.path) == nullptr, "a bad magic is refused");
        __atomic_store_n(&pub.snap->magic, magic, __ATOMIC_RELEASE);

        snprintf(short_path, sizeof(short_path), "%s.short", pub.path);
        if ((fd = open(short_path, O_CREAT | O_RDWR | O_CLOEXEC, 0600)) >= 0)
        {
            if (ftruncate(fd, 64))
                printf("snapshot_probe: ftruncate failed, skipping the short-file case\n");
            else
                check(hud_snapshot_open_path(short_path) == nullptr,
                      "a file shorter than the struct is refused");
            close(fd);
            unlink(short_path);
        }
    }

    pub.close_and_unlink();
    printf("snapshot_probe: %d failures\n", failures);
    return failures ? 1 : 0;
}
