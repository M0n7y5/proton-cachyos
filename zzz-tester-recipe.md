# ZZZ tester recipe: PipeWire audio driver A/B

What this is for: finding out whether the realtime render mode
(`WINEPIPEWIRE_RT=1`) makes the game **crash** more, less, or the same.

**Read this first: this is a crash test, not a listening test.**
Do not try to hear a difference between the two arms. There is no measurement
saying you should be able to. Our CI can show that the flag changes the way
PipeWire schedules our audio node, but on the test setup it uses, the reported
latency was identical with the flag on and off, and we have no end to end
measurement on real hardware at all. If you listen for a difference you will
probably convince yourself of one, and "sounds the same" or "sounds snappier"
would both get mistaken for a result. Judge this on crashes and on the log
line in step 3 only.

You do not need to be a developer to follow this. Copy and paste the commands.

---

## 0. What you need

- The Proton build we sent you, installed in `compatibilitytools.d`.
- About 2 GB of free disk for crash dumps.
- `gdb` installed:
  - Arch or CachyOS: `sudo pacman -S gdb`
  - Ubuntu or Debian: `sudo apt install gdb`
  - Fedora: `sudo dnf install gdb`

---

## 1. Launch options

In Steam, right click the game, Properties, General, Launch Options.

**Arm A, the baseline. Use this first.**

```
PROTON_LOG=warn+pipewire,warn+mmdevapi %command%
```

**Arm B, realtime render mode.**

```
WINEPIPEWIRE_RT=1 PROTON_LOG=warn+pipewire,warn+mmdevapi %command%
```

Only ever run one arm at a time, and note which one you were in. The log lands
in your home directory as `steam-<appid>.log`. It is overwritten every launch,
so copy it somewhere if a run is interesting.

---

## 2. Prove crash dumps work BEFORE you play

Do not skip this. If dumps are not being saved you will reproduce a crash and
have nothing to show for it.

```bash
# 1. allow big dumps for this session
ulimit -c unlimited
ulimit -c                      # must print: unlimited

# 2. confirm something is catching dumps
cat /proc/sys/kernel/core_pattern
```

That last command should print something starting with `|` and mentioning
`systemd-coredump`. If it prints just `core` or `|/bin/false`, dumps are not
being collected; tell us and stop here.

```bash
# 3. deliberately crash a harmless process
sleep 600 &
kill -SEGV %1
sleep 2

# 4. check it was actually stored
coredumpctl list | tail -3
```

You should see a `sleep` line with signal `SIGSEGV` and, importantly, the
Storage column saying `present`. If it says `none` or `inaccessible`, dumps are
not usable yet; tell us what it says.

`ulimit -c unlimited` only applies to the terminal you typed it in.
Steam needs it too, so the reliable way is to close Steam completely, then
start it from that same terminal:

```bash
ulimit -c unlimited
steam
```

---

## 3. The certification step, every run

After the game reaches audio, look at the log:

```bash
grep "audio dispatch" ~/steam-*.log
```

You should get exactly one line per stream direction, like this:

```
audio dispatch: render requested data-thread, effective data-thread, node.async=unset, loop "data-loop.0"
```

Read the two words after `requested` and after `effective`.

- **They match**, as above: good, the run is valid, carry on.
- **The line says `-- MISMATCH, the requested mode is NOT in force`**: stop.
  The run did not test what we think it tested. Send us that line.
- **There is no `audio dispatch` line at all**: stop. Either audio never
  started or the driver did not load. Send us the whole log.

In arm A you should see `requested driver-loop, effective driver-loop`.
In arm B you should see `requested data-thread, effective data-thread`.
If arm B still says `driver-loop`, the flag did not take effect. Stop and
tell us.

---

## 4. If the game crashes

**We do not want your core dump.** A core dump is a copy of everything the
game had in memory at that moment. That can include your account credentials,
session tokens, chat messages, and anything else the game or the launcher had
loaded. Please do not upload one to an issue tracker, a paste site or a file
host, for us or for anyone else.

Instead, run the script that comes with the Proton build. It reads the dump on
your own machine and writes one text file.

```bash
# 1. get the debug information for this exact build, once
#    (we will send you the link; it is the "driver-debug" artifact)
mkdir -p ~/Downloads/driver-debug
cd ~/Downloads/driver-debug
unzip ~/Downloads/*driver-debug*.zip

# 2. run the extractor, and ALWAYS pass --log
cd ~/.steam/root/compatibilitytools.d/Proton-CachyOS-PipeWire*/
./pipewire-crash-report.sh --debug ~/Downloads/driver-debug --log ~/steam-<appid>.log
```

**Pass `--log` whenever you can.** Replace `<appid>` with the number in your
log's filename; `ls ~/steam-*.log` will show it.

Newer builds stamp every driver log line with a per-run `session=` value and
store the same value inside the crash dump. When both carry it, the script
proves the log and the dump came from the same run, and it will say
`Pairing is PROVEN`. On those builds it does not matter how the log was
found, so leaving `--log` off is safe.

On older builds there is no `session=` stamp and the script has to fall back
to trusting you. There, a guessed log can never certify anything, on purpose:
an unrelated session that happened to use the same arm would look identical
to the right one, and we would file your crash under the wrong mode. So pass
`--log` and you are covered either way.

When you run it, the last line it prints is the verdict:

- `CERTIFIED-DATA-THREAD` or `CERTIFIED-DRIVER-LOOP`: good, the report says
  which mode was really in force.
- `MISMATCH`: the run did not cleanly test either mode. Still send it, and say
  so.
- `UNVERIFIED`: the report is still useful for the backtrace, but it does not
  establish which mode was running. The report says which of these applied:
  the log came from a different run (the `session=` values disagree), no log
  was passed on an older build, or the dump was taken before the driver
  finished starting. If you still have the right log, re-run with `--log`
  pointing at it.

It prints where it wrote the report, something like
`pipewire-crash-report-20260726T193110Z.txt`.

If it cannot find the crash by itself:

```bash
coredumpctl list | tail -5          # find the PID of the crashed game
./pipewire-crash-report.sh --pid <PID> --debug ~/Downloads/driver-debug
```

If it refuses because it has no debug information, it will say so and write
nothing. That is deliberate; a half report is worse than none. Follow the
instructions it prints.

**Send us the text file.** The last section of it lists exactly what it
contains and what it leaves out, so you can check before sending. Read it.

If we ever genuinely need the core itself we will say so and arrange a private
transfer directly with you. We will not ask you to post one publicly.

Tell us which arm you were in when it crashed.

---

## 5. What to report when it does NOT crash

A clean run is a real result and we want it, but it is only meaningful if we
know what "clean" is for you. Please tell us:

- Which arm, and how long you played, in minutes.
- What you were doing. Combat, cutscenes, city hub, menus, alt tabbing.
- **How often the game normally crashes for you, before any of this.** For
  example "about once every two hours", or "twice a week", or "it has never
  crashed". Without that we cannot tell a fixed bug from a lucky session.
- Whether you changed audio device, plugged in headphones, or opened Discord
  or a browser playing audio during the run. Those change the graph and are
  worth knowing about.
- The `audio dispatch` line from step 3.

Roughly matched time in both arms is much more useful than a long run in one
of them. Two hours in each beats six hours in one.

---

## 6. Quick checklist

1. `gdb` installed, `ulimit -c unlimited`, `core_pattern` uses systemd-coredump.
2. Test dump with `sleep` and `kill -SEGV` shows `present` in `coredumpctl list`.
3. Steam started from a terminal that had `ulimit -c unlimited`.
4. Launch option set, arm noted.
5. Play, then check the `audio dispatch` line.
6. Crash: run `pipewire-crash-report.sh` **with `--log`**, check the verdict
   it prints, send the TEXT file, never the core.
7. No crash: report time played, activity, and your normal crash rate.

Thank you. The boring negative runs are as useful to us as the crashes, as
long as we know how long they were.
