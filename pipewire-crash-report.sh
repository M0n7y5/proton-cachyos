#!/bin/bash
# Turn a crash into a text report, locally, without sending anyone a core dump.
#
# A core dump of a game process contains whatever was in that process's memory:
# account credentials, session tokens, chat, anything the game or the launcher
# touched. We do not want it and you should not upload it anywhere. This script
# reads the core on your own machine and writes a single text file containing
# only backtraces and a fixed list of driver counters, which is what we
# actually need to attribute a crash.
#
# Usage:
#   ./pipewire-crash-report.sh                        # newest matching core
#   ./pipewire-crash-report.sh --pid 12345
#   ./pipewire-crash-report.sh --core /path/to/core
#   ./pipewire-crash-report.sh --debug ~/Downloads/driver-debug
#
# Options:
#   --core PATH     read this core file instead of asking coredumpctl
#   --pid PID       pick the coredumpctl entry with this PID
#   --match NAME    coredumpctl match (default: the wine loader in this tree)
#   --driver PATH   winepipewire.so to take symbols from
#   --debug DIR     directory holding winepipewire.so.debug from the CI artifact
#   --log PATH      PROTON_LOG file to summarise
#   -o FILE         output path (default ./pipewire-crash-report-<stamp>.txt)
set -uo pipefail

here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
core=; pid=; match=; driver=; debugdir=; logfile=; out=; sel_used=
stamp=$(date -u +%Y%m%dT%H%M%SZ)

die() { printf '\nERROR: %s\n' "$*" >&2; exit 1; }

while [ $# -gt 0 ]; do
  case "$1" in
    --core)   core=${2:-}; shift 2 ;;
    --pid)    pid=${2:-}; shift 2 ;;
    --match)  match=${2:-}; shift 2 ;;
    --driver) driver=${2:-}; shift 2 ;;
    --debug)  debugdir=${2:-}; shift 2 ;;
    --log)    logfile=${2:-}; shift 2 ;;
    -o)       out=${2:-}; shift 2 ;;
    -h|--help) sed -n '2,27p' "$0"; exit 0 ;;
    *) die "unknown option: $1" ;;
  esac
done
out=${out:-./pipewire-crash-report-${stamp}.txt}

command -v gdb >/dev/null 2>&1 || die "gdb is not installed. Install it and run this again (Arch: pacman -S gdb, Debian/Ubuntu: apt install gdb, Fedora: dnf install gdb)."

# ---------------------------------------------------------------- driver .so
if [ -z "$driver" ]; then
  for c in "$here/files/lib/wine/x86_64-unix/winepipewire.so" \
           "$here/../files/lib/wine/x86_64-unix/winepipewire.so"; do
    [ -f "$c" ] && { driver=$c; break; }
  done
fi
[ -n "$driver" ] && [ -f "$driver" ] \
  || die "could not find winepipewire.so. Pass --driver /path/to/files/lib/wine/x86_64-unix/winepipewire.so"
driver=$(readlink -f "$driver")

build_id=$(readelf -n "$driver" 2>/dev/null | grep -oE 'Build ID: [0-9a-f]+' | head -1 | awk '{print $3}')
[ -n "$build_id" ] || die "no build ID in $driver, this does not look like our driver"

# ------------------------------------------------------- Proton tree + loader
# Anchor on the DRIVER path rather than on $here. gdb has to pair the core with
# an executable from the same tree the symbols come from, and --driver can
# point somewhere other than this script. $here is only the fallback, for a
# driver handed over as a loose file outside a redist layout.
proot=
case "$driver" in
  */files/lib/wine/*-unix/winepipewire.so)
      proot=${driver%/files/lib/wine/*-unix/winepipewire.so} ;;
esac
if [ -z "$proot" ]; then
  for c in "$here" "$here/.."; do
    [ -d "$c/files/lib/wine" ] && { proot=$(cd -- "$c" && pwd); break; }
  done
fi

# Probe for what the tree actually contains instead of hardcoding a name. The
# 64 and 32 bit loaders differ, this tree keeps the preloaders under
# files/lib/wine/<arch>-unix and not files/bin, and a wow64 build may ship no
# preloader at all.
loaders=()
if [ -n "$proot" ]; then
  for cand in \
    "$proot/files/lib/wine/x86_64-unix/wine64-preloader" \
    "$proot/files/lib/wine/x86_64-unix/wine-preloader" \
    "$proot/files/lib/wine/i386-unix/wine-preloader" \
    "$proot/files/lib/wine/i386-unix/wine64-preloader" \
    "$proot/files/lib/wine/x86_64-unix/wine64" \
    "$proot/files/lib/wine/x86_64-unix/wine" \
    "$proot/files/bin/wine" ; do
    [ -f "$cand" ] && loaders+=("$cand")
  done
fi
if [ ${#loaders[@]} -eq 0 ]; then
  die "could not find a Wine loader to identify your crash with.

  Proton tree resolved to: ${proot:-none}
  Looked for, under that tree:
    files/lib/wine/x86_64-unix/{wine64-preloader,wine-preloader,wine64,wine}
    files/lib/wine/i386-unix/{wine-preloader,wine64-preloader}
    files/bin/wine

  Without one this script will not guess which core is yours. Re-run with
  --pid <PID from 'coredumpctl list'> or --core /path/to/core."
fi

# ------------------------------------------------------------- debug info
# Either the .so still carries DWARF, or we point gdb at the split .debug from
# the CI artifact through a build-id tree, which works even when the Proton
# directory is read only.
symroot=$(mktemp -d)
trap 'rm -rf "$symroot"' EXIT
have_debug=0
if readelf -S "$driver" 2>/dev/null | grep -q '\.debug_info'; then
  have_debug=1
else
  cands=()
  [ -n "$debugdir" ] && cands+=("$debugdir")
  cands+=("$(dirname "$driver")" "$here" "$here/driver-debug" "$PWD" \
          "$HOME/Downloads" "$HOME/Downloads/driver-debug")
  found=
  for d in "${cands[@]}"; do
    [ -d "$d" ] || continue
    while IFS= read -r f; do
      id=$(readelf -n "$f" 2>/dev/null | grep -oE 'Build ID: [0-9a-f]+' | head -1 | awk '{print $3}')
      [ "$id" = "$build_id" ] && { found=$f; break; }
    done < <(find "$d" -maxdepth 3 -name 'winepipewire.so.debug' 2>/dev/null)
    [ -n "$found" ] && break
  done
  if [ -n "$found" ]; then
    mkdir -p "$symroot/.build-id/${build_id:0:2}"
    cp "$found" "$symroot/.build-id/${build_id:0:2}/${build_id:2}.debug"
    have_debug=1
    debug_used=$found
  fi
fi

if [ "$have_debug" != 1 ]; then
  cat >&2 <<EOF

ERROR: no debug information for this driver, so the report would be missing
       everything it exists to collect. Nothing has been written.

  driver:   $driver
  build ID: $build_id

  Download the "driver-debug" artifact from the SAME CI run that produced this
  Proton build (the run URL is in build-info.txt next to this script, under
  workflow_run), unzip it, and re-run with:

      $0 --debug /path/to/unzipped/driver-debug

EOF
  exit 1
fi

# ------------------------------------------------------------------- core
exe=
if [ -z "$core" ]; then
  command -v coredumpctl >/dev/null 2>&1 \
    || die "coredumpctl not available. Pass --core /path/to/core instead."
  core=$symroot/core
  if [ -n "$pid$match" ]; then
    sel=${pid:-$match}
    if ! coredumpctl dump "$sel" --output="$core" >/dev/null 2>&1; then
      echo "Recent core dumps on this machine:" >&2
      coredumpctl list --no-pager 2>/dev/null | tail -10 >&2
      die "coredumpctl has no stored dump matching '$sel'."
    fi
    sel_used=$sel
    exe=$(coredumpctl info "$sel" 2>/dev/null | awk '/^ *Executable:/{print $2; exit}')
    [ -n "$exe" ] && [ -f "$exe" ] || exe=
  else
    # Try each loader this tree actually has, in turn. There is deliberately no
    # unfiltered fallback: `coredumpctl dump` with no match returns the newest
    # core on the machine whatever produced it, so a tester following these
    # instructions would get a confident report about an unrelated process.
    for cand in "${loaders[@]}"; do
      if coredumpctl dump "$cand" --output="$core" >/dev/null 2>&1; then
        exe=$cand
        sel_used=$cand
        break
      fi
    done
    if [ -z "$exe" ]; then
      echo "Recent core dumps on this machine:" >&2
      coredumpctl list --no-pager 2>/dev/null | tail -10 >&2
      die "no stored core dump was produced by a Wine loader from
  $proot

  Nothing has been written. This script will not fall back to the newest core
  on the machine, because that is usually some unrelated program. If your
  crash is in the list above, re-run with --pid <PID>, or point at the file
  directly with --core /path/to/core."
    fi
  fi
else
  # A core the user pointed at: work out which loader it names, so gdb gets a
  # matching executable rather than none.
  for cand in "${loaders[@]}"; do
    if grep -aqF -- "$cand" "$core" 2>/dev/null; then exe=$cand; break; fi
  done
  if [ -z "$exe" ]; then
    die "the core at
  $core
  does not reference any Wine loader from
  $proot

  It is probably from a different process, or from a different Proton build
  than the one this script shipped in. Nothing has been written."
  fi
fi
[ -s "$core" ] || die "core file is empty: $core"

# --------------------------------------------------------------- preflight
# Refuse rather than emit half a report.
pre=$(gdb -batch -nx -iex 'set debuginfod enabled off' -iex 'set sysroot /' \
        -iex "set debug-file-directory $symroot" \
        -c "$core" ${exe:+"$exe"} \
        -ex 'list pipewire.c:200' -ex 'ptype struct pipewire_stream' 2>&1)
if ! printf '%s' "$pre" | grep -q 'cb_mark'; then
  cat >&2 <<EOF

ERROR: gdb loaded the core but could not read the driver's types, so the
       report would be empty of driver state. Nothing has been written.

  core:     $core
  driver:   $driver
  build ID: $build_id
  ${debug_used:+debug:    $debug_used}

  The usual cause is a debug file from a different build than the Proton that
  crashed. Download the "driver-debug" artifact from the SAME run as this
  Proton build and pass it with --debug.

EOF
  exit 1
fi

# ---------------------------------------------------------------- gdb script
cmds=$symroot/report.gdb
cat > "$cmds" <<'GDBEOF'
set pagination off
set print frame-arguments none
set print address on
echo \n==== THREADS ====\n
info threads
echo \n==== BACKTRACES (function names and source lines only) ====\n
thread apply all bt
echo \n==== DRIVER STATE ====\n
list pipewire.c:200
printf "rt_render (REQUESTED) = %d\n", rt_render
echo \n
echo   rt_render is the WINEPIPEWIRE_RT environment request, parsed once at\n
echo   process attach. It is NOT where the process callback actually ran:\n
echo   node.loop.class from PIPEWIRE_PROPS or a client.conf stream.rules entry\n
echo   can override it independently. The effective mode is the EFFECTIVE MODE\n
echo   verdict near the top of this file.\n
echo \n
echo   cb_mark is published with relaxed atomic stores, so the phase below is a\n
echo   hint about how far the callback had got, not a happens-before witness.\n
echo   Read it as "roughly here". The callback count is the reliable part.\n
set $h = (struct list *)&g_streams
set $p = g_streams.next
set $n = 0
while $p != $h && $n < 32
  set $s = (struct pipewire_stream *)((char *)$p - (char *)&((struct pipewire_stream *)0)->entry)
  printf "\nstream[%d]\n", $n
  if $s->dataflow == 0
    printf "  dataflow           = render\n"
  else
    printf "  dataflow           = capture\n"
  end
  printf "  started            = %d\n", $s->started
  printf "  cb_mark            = %u (raw)\n", $s->cb_mark
  printf "  callbacks entered  = %u\n", $s->cb_mark >> 2
  if ($s->cb_mark & 3) == 0
    printf "  phase              = 0 (the process callback never ran for this stream)\n"
  end
  if ($s->cb_mark & 3) == 1
    printf "  phase              = 1 CB_ENTER (in the callback, buffer not yet validated)\n"
  end
  if ($s->cb_mark & 3) == 2
    printf "  phase              = 2 CB_BODY (buffer validated, moving audio)\n"
  end
  if ($s->cb_mark & 3) == 3
    printf "  phase              = 3 CB_DONE (buffer queued back, callback returning)\n"
  end
  printf "  underrun_count     = %u\n", $s->underrun_count
  printf "  overrun_count      = %u\n", $s->overrun_count
  printf "  bad_buffer_count   = %u\n", $s->bad_buffer_count
  printf "  bufsize_frames     = %lu\n", $s->bufsize_frames
  printf "  real_bufsize_bytes = %lu\n", $s->real_bufsize_bytes
  printf "  period_bytes       = %lu\n", $s->period_bytes
  printf "  capture_ring_size  = %lu\n", $s->capture_ring_size
  printf "  cap_n_slots        = %u\n", $s->cap_n_slots
  set $p = $p->next
  set $n = $n + 1
end
printf "\nstream_count = %d\n", $n
GDBEOF

tmp=$symroot/body.txt
gdb -batch -nx -iex 'set debuginfod enabled off' -iex 'set sysroot /' \
    -iex "set debug-file-directory $symroot" \
    -c "$core" ${exe:+"$exe"} -x "$cmds" > "$tmp" 2>&1

# ------------------------------------------------- effective dispatch verdict
# rt_render is only what the environment asked for. node.loop.class from
# PIPEWIRE_PROPS or a client.conf stream.rules entry decides where the
# callback ran, so the driver's certification line is the only statement of
# the effective mode.
#
# Pairing that line to THIS core is the other half. dispatch_token is a
# per-process value, readable from the core by name and printed at the end of
# every dispatch line as session=<16 lowercase hex>. A match is evidence
# rather than assertion, and makes the log selection method irrelevant.
# Builds without it fall back to the weaker rules below.
log_explicit=1
log_source="supplied with --log"
if [ -z "$logfile" ]; then
  log_explicit=0
  log_source="auto-selected, NOT confirmed by you"
  for c in "$HOME"/steam-*.log "$PWD"/steam-*.log; do
    [ -f "$c" ] || continue
    if [ -z "$logfile" ] || [ "$c" -nt "$logfile" ]; then logfile=$c; fi
  done
fi

# Read the token out of the core. Separate gdb run so a missing symbol on an
# older build cannot disturb the main report. 0 is the unset sentinel: the
# live value is forced non-zero, so 0 means the core was taken before process
# attach finished and nothing may be certified from it.
core_token=$(gdb -batch -nx -iex 'set debuginfod enabled off' -iex 'set sysroot /' \
    -iex "set debug-file-directory $symroot" \
    -c "$core" ${exe:+"$exe"} \
    -ex 'list pipewire.c:200' \
    -ex 'printf "CORETOKEN=%016llx\n", (unsigned long long)dispatch_token' 2>/dev/null \
  | sed -n 's/^CORETOKEN=\([0-9a-f]\{16\}\)$/\1/p' | head -1)

# Select on the whole family, not just the certification variant: all five
# carry the token and the prose ones are worth quoting on a mismatch.
all_dispatch=
if [ -n "$logfile" ] && [ -f "$logfile" ]; then
  all_dispatch=$(grep -a 'audio dispatch: ' "$logfile" | sed 's/^.*audio dispatch: /audio dispatch: /' | sort -u)
fi
log_tokens=$(printf '%s\n' "$all_dispatch" | grep -oE 'session=[0-9a-f]{16}$' | sed 's/^session=//' | sort -u)

token_state=absent          # absent | matched | mismatched | unset-core
dispatch_lines=$all_dispatch
if [ -n "$log_tokens" ]; then
  if [ -z "$core_token" ]; then
    token_state=absent      # log has tokens, core symbol does not exist
  elif [ "$core_token" = 0000000000000000 ]; then
    token_state=unset-core
  elif printf '%s\n' "$log_tokens" | grep -qx "$core_token"; then
    token_state=matched
    # Per-line: one file can hold more than one run.
    dispatch_lines=$(printf '%s\n' "$all_dispatch" | grep -F "session=${core_token}")
  else
    token_state=mismatched
  fi
fi

render_line=$(printf '%s\n' "$dispatch_lines" | grep -m1 -E '^audio dispatch: render requested ' || true)

verdict=UNVERIFIED
if printf '%s\n' "$dispatch_lines" | grep -q 'MISMATCH'; then
  verdict=MISMATCH
elif [ -n "$render_line" ]; then
  case "$(printf '%s' "$render_line" | sed -n 's/.*effective \([^,]*\).*/\1/p')" in
    data-thread) verdict=CERTIFIED-DATA-THREAD ;;
    driver-loop) verdict=CERTIFIED-DRIVER-LOOP ;;
  esac
fi

mismatched_pair=
guessed_log=
core_rt=$(grep -m1 'rt_render (REQUESTED) = ' "$tmp" 2>/dev/null | grep -oE '[0-9]+$' || true)
log_req=$(printf '%s' "$render_line" | sed -n 's/.*requested \([^,]*\).*/\1/p')

case "$token_state" in
  matched)
    : # Proven pairing, so none of the weaker heuristics apply.
    ;;
  mismatched|unset-core)
    verdict=UNVERIFIED
    ;;
  absent)
    # No token on one side or the other, so fall back to the old rules.
    # rt_render from the core against the requested mode from the log catches
    # a log from the other arm, and it is only one bit, so a guessed log still
    # cannot certify.
    if [ -n "$core_rt" ] && [ -n "$log_req" ]; then
      if { [ "$core_rt" = 1 ] && [ "$log_req" != data-thread ]; } ||
         { [ "$core_rt" = 0 ] && [ "$log_req" != driver-loop ]; }; then
        mismatched_pair="core rt_render=${core_rt} but the log says requested ${log_req}"
        verdict=UNVERIFIED
      fi
    fi
    if [ "$log_explicit" != 1 ] && [ -n "$dispatch_lines" ]; then
      case "$verdict" in
        CERTIFIED-*) guessed_log=1; verdict=UNVERIFIED ;;
      esac
    fi
    ;;
esac

cert_basis() {
  if [ "$token_state" = matched ]; then
    echo "  Pairing is PROVEN, not assumed: the driver's per-process token in the"
    echo "  core and in the log line are the same value, session=${core_token}."
    echo "  Only lines carrying that token were used, so another run in the same"
    echo "  file could not contribute. How the log was chosen does not matter."
  else
    echo "  Pairing is ASSERTED, not proven: this build does not carry the"
    echo "  per-process session token, so the script is relying on you having"
    echo "  named the log from the run that crashed."
  fi
}

# ------------------------------------------------------------------ assemble
{
  echo "PipeWire driver crash report"
  echo "generated: $stamp (UTC)"
  echo "generated by: pipewire-crash-report.sh"
  echo
  echo "==== BUILD ===="
  for bi in "${proot:+$proot/build-info.txt}" "$here/build-info.txt"; do
    [ -f "$bi" ] && { cat "$bi"; break; }
  done
  echo "driver_build_id=$build_id"
  [ -n "${debug_used:-}" ] && echo "debug_file=$(basename "$debug_used")"
  echo
  echo "==== CRASH ===="
  # Query the SAME selector the dump came from. A bare `coredumpctl info`
  # reports the newest entry on the machine, which is how a report about one
  # process ends up wearing another process's signal and timestamp.
  if [ -n "$sel_used" ]; then
    # tail -2: `info` prints every matching entry oldest first, two lines
    # each after this filter, and `dump` took the newest one.
    coredumpctl info "$sel_used" 2>/dev/null \
      | grep -E '^\s+(Signal|Timestamp):' | sed 's/^ *//' | tail -2 || true
  else
    grep -m1 '^Program terminated with signal' "$tmp" 2>/dev/null || true
    echo "core file: $(basename "$core")"
  fi
  echo "executable: $(basename "${exe:-unknown}")"
  echo
  echo "==== EFFECTIVE MODE ===="
  case "$verdict" in
    CERTIFIED-DATA-THREAD)
      echo "VERDICT: CERTIFIED data-thread. The realtime render dispatch was in"
      echo "         force for the run this log came from."
      echo "  $render_line"
      echo
      cert_basis ;;
    CERTIFIED-DRIVER-LOOP)
      echo "VERDICT: CERTIFIED driver-loop. The baseline dispatch was in force"
      echo "         for the run this log came from."
      echo "  $render_line"
      echo
      cert_basis ;;
    MISMATCH)
      echo "VERDICT: MISMATCH. The requested dispatch mode was NOT in force."
      printf '%s\n' "$dispatch_lines" | sed 's/^/  /'
      echo
      echo "  This run exercised neither mode cleanly. Do not count it as"
      echo "  evidence for or against the realtime mode." ;;
    *)
      echo "VERDICT: UNVERIFIED. The effective dispatch mode is NOT known for"
      echo "         this crash. Do NOT count this as a realtime-mode crash,"
      echo "         whatever rt_render says further down: that value is only"
      echo "         the environment request, and the graph can override it."
      echo
      if [ "$token_state" = mismatched ]; then
        echo "  The log is from a DIFFERENT process run. The driver stamps every"
        echo "  dispatch line with a per-process token and stores the same value"
        echo "  in the core, and the two do not agree:"
        echo "    core  session=${core_token}"
        printf '%s\n' "$log_tokens" | sed 's/^/    log   session=/'
        echo "  Nothing from that log has been used. Find the log from the"
        echo "  session that actually crashed and pass it with --log."
      elif [ "$token_state" = unset-core ]; then
        echo "  The core's session token reads all zeroes, which is the unset"
        echo "  value. The dump was taken before the driver finished attaching,"
        echo "  so no log line can be paired with it and nothing is certified."
      elif [ -n "$guessed_log" ]; then
        echo "  A log WAS found and it does contain a certification line, but the"
        echo "  script guessed that log rather than being told which one to use,"
        echo "  and it cannot be shown to belong to this core. An unrelated run of"
        echo "  the same arm would look exactly like this. The line is shown here"
        echo "  as context only and has NOT been used to certify anything:"
        printf '%s\n' "$dispatch_lines" | sed 's/^/    /'
        echo
        echo "  Re-run with --log pointing at the log from the session that"
        echo "  actually crashed to turn this into a verdict."
      elif [ -n "$mismatched_pair" ]; then
        echo "  The log does not belong to this core: ${mismatched_pair}."
        echo "  Its certification line is therefore about a different run and"
        echo "  has been discarded:"
        echo "    $render_line"
      elif [ -n "$logfile" ] && [ -f "$logfile" ]; then
        echo "  A log was read ($(basename "$logfile")) but it has no"
        echo "  'audio dispatch:' line, so the driver never certified a mode."
      else
        echo "  No PROTON_LOG was supplied or found next to this script or in"
        echo "  \$HOME."
      fi
      if [ -z "$guessed_log" ]; then
        echo "  To get a verdict, the run needs this launch option:"
        echo "    PROTON_LOG=warn+pipewire,warn+mmdevapi %command%"
        echo "  with WINEPIPEWIRE_RT=1 in front of it for the realtime arm, and"
        echo "  this script re-run with --log \$HOME/steam-<appid>.log"
      fi ;;
  esac
  echo
  if [ -n "$logfile" ] && [ -f "$logfile" ]; then
    echo "  log file: $(basename "$logfile")"
    echo "  last written: $(date -u -r "$logfile" +%Y-%m-%dT%H:%M:%SZ 2>/dev/null)"
    echo "  how it was chosen: ${log_source}"
    echo "  The script cannot prove this log came from the same run as the core."
    if [ "$log_explicit" != 1 ]; then
      echo "  It was picked as the newest steam-*.log in \$HOME, which may well be"
      echo "  a different game or a different session, so it can never certify."
      echo "  Re-run with --log if the timestamp above matches the crash."
    fi
  fi
  echo
  # Scrub and de-noise. "Core was generated by" carries the full command
  # line, which can hold a game path or a user name, and we promised not to
  # include it. The rest is gdb chatter about files it could not open.
  sed -e "s#$HOME#\$HOME#g" "$tmp" \
    | grep -vaE '^Core was generated by|No such file or directory'"|"'^warning: Can.t open file|^\[New LWP|^\[Thread debugging|^Using host libthread_db|^\[Current thread is'
  echo
  echo "==== DRIVER LOG ===="
  if [ -n "$logfile" ] && [ -f "$logfile" ]; then
    echo "source: $(basename "$logfile")"
    echo "(only lines on the pipewire and mmdevapi channels are copied)"
    grep -aE ':(warn|err|fixme|trace):(pipewire|mmdevapi):|audio dispatch:' "$logfile" \
      | sed -e "s#$HOME#\$HOME#g" | tail -400
  else
    echo "no PROTON_LOG found. If you have one, re-run with --log /path/to/steam-<appid>.log"
  fi
  echo
  echo "==== WHAT THIS FILE CONTAINS ===="
  echo "INCLUDED:"
  echo "  - the list of threads and a backtrace for each"
  echo "  - function names, source file names and line numbers"
  echo "  - library paths that were mapped into the process"
  echo "  - the effective dispatch verdict, and the driver certification line"
  echo "    it was read from"
  echo "  - rt_render, the environment request"
  echo "  - for each audio stream: dataflow, started, cb_mark with its decoded"
  echo "    callback count and phase, underrun_count, overrun_count,"
  echo "    bad_buffer_count, bufsize_frames, real_bufsize_bytes, period_bytes,"
  echo "    capture_ring_size, cap_n_slots"
  echo "  - driver log lines on the pipewire and mmdevapi channels only"
  echo "  - the build identity from build-info.txt"
  echo
  echo "NOT INCLUDED:"
  echo "  - the core dump itself, which stays on your machine"
  echo "  - function arguments and local variables (gdb was run with"
  echo "    'set print frame-arguments none')"
  echo "  - any audio sample data, any heap or stack contents, any registers"
  echo "  - the process command line, environment variables, and any other"
  echo "    log channel"
  echo "  - your home directory path, which is written as \$HOME above"
  echo
  echo "The core dump this was read from is NOT attached and does not need to be"
  echo "sent. Please read through this file before posting it."
} > "$out"

printf '\nWrote %s (%s)\n' "$out" "$(du -h "$out" | cut -f1)"
printf 'Streams found: %s\n' "$(grep -c '^stream\[' "$out" 2>/dev/null || echo 0)"
printf 'Effective dispatch mode: %s\n' "$verdict"
if [ "$verdict" = UNVERIFIED ]; then
  printf '\n  WARNING: this report does NOT establish which dispatch mode was\n'
  printf '  running, so it must not be counted as a realtime-mode crash.\n'
  printf '  See the EFFECTIVE MODE section of the report for why, and for\n'
  printf '  what to run next time.\n'
fi
printf '\nSend that TEXT FILE. Do not send the core dump.\n'
printf 'Read it first; the last section lists exactly what is in it.\n\n'
