A known-good session, measured
==============================

Every number here has its denominator and its window beside it, because the
previous baseline this component had was "4 underruns against 6311 callbacks",
quoted without saying over what period or how many streams, and it turned out to
be hard to compare anything against.

What was measured
-----------------

Shadow of the Tomb Raider, appid 750920, on `Proton-CachyOS-PipeWire-HUD-20260815`,
captured to `~/steam-750920.log`, 321.3 MB, 2581701 lines. Session clock 63051.9 s
to 63247.4 s, so **195.5 s** of gameplay. 2560x1440, median 223 presents per second.
Two render streams over the session, the first activating at 3.9 s and released at
47.1 s, the second activating at 49.1 s and still running at the end.

The owner had confirmed by ear that the bass defect was fixed in this build, so
this is a session that a human judged good before anyone counted anything.

The build, because a baseline from a different build is not comparable
---------------------------------------------------------------------

Confirmed by string counts over the whole file, not a prefix of it:

| string | count |
| --- | ---: |
| `bed virtualization requested, backend chosen at activation` | 2 |
| `HRTF bed virtualization` | 0 |
| `bass low-shelf off` | 2 |
| `bass low-shelf on` | 0 |
| `Using the Steam Audio HRTF engine` | 2 |
| `HRTF engine unavailable` | 0 |
| `rendering through the Steam Audio HRTF engine` | 2 |
| `rendering through stereo panning` | 0 |

A log whose configuration line says `HRTF bed virtualization` predates the fix that
stopped the driver naming a backend before it was created, and one whose shelf line
says `bass low-shelf on` predates the bass default changing. Either way its numbers
are from different code and should not be compared with the table below.

The numbers
-----------

The right-hand column is the coverage, and it is the point of the table.

| quantity | value | over what |
| --- | --- | --- |
| `err:` on `pipewire`, `mmdevapi`, `spatial` | 0 | 2581701 lines, 195.5 s |
| `fixme:` on the same three | 0 | same |
| `warn:` on the same three | 6 lines total | same, and all six are enumerated below |
| graph sink xruns | 0 | 186 of 186 once-a-second overlay reports |
| overruns | 0 | 186 of 186 |
| bad buffers | 0 | 186 of 186 |
| underruns, stream 1 | 9 over 8075 callbacks, 1.1 per 1000 | all within the first ~5 s of a 43 s stream life, then unchanged for 42 s |
| underruns, stream 2 | reached 8 | all within ~1 s of activation, then unchanged for the remaining ~140 s |
| render ring resync repairs | 1 in the session | counted from the driver's own `PipeWire buffer overflow` warning at log line 114078, **not** from `drv_ring_resyncs`, which did not exist in this build. The overlay reported nothing for it, which is the gap this session exposed. A later run's `resync` reading is comparable with this 1 only because the event was counted by hand here |
| period grid valid | 186 of 186 reports, never lost | flags read `R,grid,nodsp` throughout |
| phase adjustment | median -57 us, range -340 us to -8 us | 186 reports, against the driver's `+-period_usec/2` clamp of +-5000 us, so 6.8 percent of clamp at worst |
| ring occupancy | 0.0 percent in 186 of 186 | structural, see below |
| output peak meter | live, e.g. -22.6 and -21.2 dBFS | 2 channels, 186 reports |
| torn snapshot reads | 1 section A, 0 section B | 41457 samples |
| overlay geometry | 2276 to 2488 vertices | 186 reports, against a 65535 ceiling, so 3.8 percent at worst |
| snapshot staleness | one spike to 2005.8 ms | 186 reports; it lands exactly between the stream release and the next activation, so the overlay correctly showed its IDLE banner for those two seconds |

The six audio warnings, in full, because "6 warnings" without them is not a baseline:

```
warn mmdevapi MMDevice_GetPropValue Reading L"{1DA5D803-...},3" returned 2
warn pipewire report_dispatch_mode audio dispatch: render requested driver-loop,
     effective driver-loop, node.async=true, loop "winepipewire", rttime=unlimited
warn pipewire pipewire_period_timer_loop stream 0x... first underrun (count 1).
warn pipewire pipewire_release_render_buffer 0x... PipeWire buffer overflow.
warn pipewire pipewire_release_stream stream 0x... underran 9 times, overran 0
     times, bad buffers 0, cb_seq 8075 last phase 3 last_error (null).
warn pipewire pipewire_period_timer_loop stream 0x... first underrun (count 1).
```

Zero occurrences, each of which would be a regression
-----------------------------------------------------

Checked explicitly and absent from all 2581701 lines: `No HRTF effect slot`, which
is the object-slot leak; `No more objects` and `No dynamic object slots`, which are
budget exhaustion; `SPTLAUDCLNT`, which is any spatial HRESULT surfacing in a
message; `invalidated`; `GetBuffer failed`. Also absent: any ring repair *failure*,
which is a different thing from the one repair that succeeded and is recorded in the
table above, and the stream release line records `last_error (null)`.

Things that read as zero and are not faults
-------------------------------------------

- **Ring occupancy is 0.0 percent in every report.** `drv_held_bytes` is published
  after the period timer drains the ring, so for a client that hands over one
  period at a time it is zero at every publish by construction. It is not a
  measure of ring health. This is why the overlay demoted it to the verbose view
  with the words "held at publish" attached rather than showing a headline
  percentage that reads as an error.
- **`dsp` is unavailable, not zero.** The driver sets `PWHUD_F_NO_DSP_LOAD` on
  every publish because it cannot bind PipeWire's profiler, so the overlay shows
  `dsp --`. A build that starts showing a percentage there has gained a feature,
  not drifted.
- **A `PipeWire buffer overflow` warning is still not an overrun or a bad buffer,
  so `over 0 bad 0` beside one of these warnings is consistent, not
  contradictory.** In this session it incremented nothing at all and the overlay
  could not show it; the driver now counts the successful repair in
  `drv_ring_resyncs` and the overlay renders it as `resync` on the fault row, so on
  a current build the same event reads `over 0 bad 0 resync 1`. The failure path is
  still not counted there, because it reports through `ring_op_failed` and returns
  an error to the application instead.

Noise that is not ours
----------------------

`vkd3d-proton` emitted 2571368 warnings, 99.6 percent of every line in the file,
dominated by `d3d12_command_allocator_allocate_meta_index: Meta descriptor
pressure! Falling back to global heap (potentially slow)` at 42128 occurrences,
216 per second, sustained from 3.4 s to the end. Any future scan of a log from this
game should expect to filter that before concluding anything about audio.

How to take the same measurement again
--------------------------------------

Launch with logging enabled so `~/steam-<appid>.log` is written, and with
`WINEPIPEWIRE_HUD_OVERLAY=1` plus `WINEPIPEWIRE_HUD_OVERLAY_LOG=2` so the overlay
writes one line per swapchain per second into the same log. That is what makes the
driver's traces and the overlay's own reading of the snapshot comparable: they end
up interleaved in one file, which is how the two dispatch-word and counter-recency
defects in this component were found. Do not read a 300 MB log linearly; scan it
once programmatically and report rates with their denominators.
