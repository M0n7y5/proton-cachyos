/*
 * Consumer of the winepipewire.drv shared-memory diagnostic snapshot.
 *
 * The struct, its flag masks and its constants come from the driver's own
 * header, included by path and never retyped: the driver tree owns that ABI.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef WINEPIPEWIRE_HUD_SNAPSHOT_HPP
#define WINEPIPEWIRE_HUD_SNAPSHOT_HPP

#include <cstddef>
#include <cstdint>

/* The driver header falls back to C11 _Static_assert, which C++ does not have,
 * and defers to a C_ASSERT defined by its consumer. */
#ifndef C_ASSERT
#define C_ASSERT(e) static_assert(e, #e)
#endif

#include "winepipewire_hud.h"

/* Older than this and the producer has stopped publishing: show idle rather
 * than numbers that look live but are frozen. */
#define HUD_SNAPSHOT_STALE_NS 1000000000ull

/* End offset of one snapshot field, so a size check names the field it protects
 * instead of a number that has to be kept in step with the header.  The driver
 * header's PWHUD_SIZE_V1_BASE covers every field a version 1 writer has always
 * published; this is for the ones appended after it, which an older writer leaves
 * at the zero the page was created with. */
#define HUD_SNAPSHOT_THROUGH(field) \
    (offsetof(struct pwhud_snapshot, field) + sizeof(((struct pwhud_snapshot *)0)->field))

/* One per consumer, holding the last torn-free copy of each section.  The two
 * sections have one writer each on opposite sides of the PE/unix boundary, so a
 * successful read of one says nothing about the other and they are tracked
 * separately. */
struct hud_snapshot_view
{
    struct pwhud_snapshot a;
    struct pwhud_snapshot b;
    bool have_a;
    bool have_b;
    /* The most recent sample tore, so the copies above are the previous ones. */
    bool torn_a;
    bool torn_b;
    uint64_t samples;
    uint64_t torn_a_total;
    uint64_t torn_b_total;
};

/* Path this process reads, which is its own pid unless HUD_ENV_PID overrides it.
 * Returns false if it does not fit or HOME is unset. */
bool hud_snapshot_path(char *buf, size_t len);

/* Map and validate one snapshot file.  Returns NULL and logs why on failure. */
const struct pwhud_snapshot *hud_snapshot_open_path(const char *path);

/* The mapping for this process, opened on first use and retried at most once a
 * second afterwards, because audio can start long after the first frame. */
const struct pwhud_snapshot *hud_snapshot_map(void);

/* Refresh both sections through their own seqlocks.  A torn section leaves the
 * previous copy in place rather than blanking it. */
void hud_snapshot_sample(struct hud_snapshot_view *view, const struct pwhud_snapshot *snap);

/* flags is one word with two publishers, partitioned by owner, so each section's
 * bits are only meaningful when taken from that section's own copy. */
uint32_t hud_snapshot_flags_a(const struct hud_snapshot_view *view);
uint32_t hud_snapshot_flags_b(const struct hud_snapshot_view *view);

bool hud_snapshot_idle(const struct hud_snapshot_view *view, uint64_t now_ns);
/* True when the writer carries the fields appended after PWHUD_SIZE_V1_BASE:
 * section A's stream identity, section B's client and mix counts.  Presence is
 * decided by size and never by a value, because an older writer leaves all four
 * at the zero the page was created with and zero means something in three of
 * them.  One predicate per section, each read from that section's own copy, the
 * same rule the flag masks follow. */
bool hud_snapshot_have_stream_scope(const struct hud_snapshot_view *view);
bool hud_snapshot_have_spatial_counts(const struct hud_snapshot_view *view);
/* True when the writer carries the section B clip counters and the clip has
 * actually run.  Both halves matter: size alone would draw an empty ratio. */
bool hud_snapshot_have_clip_stats(const struct hud_snapshot_view *view);
/* True when the writer carries drv_str[].  Presence is decided by size and
 * never by drv_str_count, because an older writer leaves the count at the zero
 * the page was created with and zero is also a live empty group. */
bool hud_snapshot_have_stream_meters(const struct hud_snapshot_view *view);
C_ASSERT(HUD_SNAPSHOT_THROUGH(drv_str) == PWHUD_SIZE_V1_STR);

/* Section B's three reachable states.  All three used to render as the one string
 * "never published", and the first is the ordinary case for a title that never
 * asks for spatial audio: read as a fault, it produced a bug report against a
 * process that was behaving correctly. */
enum hud_spatial_state
{
    HUD_SPATIAL_ABSENT,  /* sp_clients 0, so no spatial stream has ever existed here */
    HUD_SPATIAL_NO_MIX,  /* activated, and not one mix published since */
    HUD_SPATIAL_LIVE,    /* the bed levels came from a mix */
};

enum hud_spatial_state hud_snapshot_spatial_state(const struct hud_snapshot_view *view);

/* False until the first mix publish lands.  Not the announce: the announce path
 * increments sp_clients and returns before seqlock B is entered (spatial.c:535-539),
 * so an activation leaves seq_sp at 0 and only the elected stream's mix moves it.
 * On any writer the driver can produce that makes this exactly
 * hud_snapshot_spatial_state() == HUD_SPATIAL_LIVE, because a mix is unreachable
 * without a prior announce: spatial_hud_claim refuses once hud_off has latched
 * (spatialaudio.c:849) and the announce fires at activation whenever it has not.
 * Prefer hud_snapshot_spatial_state, which also names HUD_SPATIAL_NO_MIX, the
 * activated-but-never-mixed stream this predicate folds into false.  A process
 * whose audio never goes through ISpatialAudioClient stays here forever, which is
 * not the same statement as a bed of zeroes. */
bool hud_snapshot_spatial_published(const struct hud_snapshot_view *view);
/* False when the driver cannot measure the graph's DSP load, which it cannot
 * without binding PipeWire's Profiler from a second connection.  pw_dsp_load is
 * then 0.0 and rendering that as 0% would be a measurement that never happened. */
bool hud_snapshot_dsp_load_valid(const struct hud_snapshot_view *view);

void hud_snapshot_log(const struct hud_snapshot_view *view);

#endif /* WINEPIPEWIRE_HUD_SNAPSHOT_HPP */
