#!/usr/bin/env bash
# probe.sh -- build and run every probe in this repo, print a machine header,
# and emit one machine-readable record for aggregation.
#
# The same script runs locally and in CI, so that a result pasted into a bug
# report and a result from the matrix are produced by identical code.
#
# Usage: ./probe.sh
# Env:
#   THREADS, TRIALS   size of the first stage             (default 20, 200)
#   RESULT_TSV        append a one-line tab-separated record to this file
#   RESULT_LABEL      name for that record                (default: hostname)
#   RACE_DIR          parent directory for the temp dirs  (default $TMPDIR)
#   SKIP_GO=1         do not run the Go probe
#
# Exit: 0 every probe interpretable (whether it reproduced or was clean)
#       1 build or setup failure
#       2 a control row failed somewhere -- suspect the harness or the machine
#       3 the race was not fail-closed; that is a new finding, read the output
#
# Written for bash 3.2, which is what macOS ships.
set -u

threads=${THREADS:-20}
trials=${TRIALS:-200}
label=${RESULT_LABEL:-$(hostname -s 2>/dev/null || hostname 2>/dev/null || echo unknown)}
bin=$(mktemp -d "${TMPDIR:-/tmp}/openatrace-bin.XXXXXX")
trap 'rm -rf "$bin"' EXIT
here=$(cd "$(dirname "$0")" && pwd)

# ---------------------------------------------------------------- machine ---

uname_s=$(uname -s)
arch=$(uname -m)
os_name=$uname_s
os_version="?"
os_build="-"
cpu="?"
cores="?"

case "$uname_s" in
Darwin)
    os_name="macOS"
    os_version=$(sw_vers -productVersion 2>/dev/null || echo "?")
    os_build=$(sw_vers -buildVersion 2>/dev/null || echo "?")
    cpu=$(sysctl -n machdep.cpu.brand_string 2>/dev/null || echo "?")
    cores=$(sysctl -n hw.ncpu 2>/dev/null || echo "?")
    ;;
Linux)
    os_name=$(sed -n 's/^PRETTY_NAME="\(.*\)"$/\1/p' /etc/os-release 2>/dev/null)
    [ -n "$os_name" ] || os_name="Linux"
    os_version=$(uname -r)
    cpu=$(sed -n 's/^model name[[:space:]]*:[[:space:]]*//p' /proc/cpuinfo 2>/dev/null | head -1)
    [ -n "$cpu" ] || cpu=$(sed -n 's/^Model[[:space:]]*:[[:space:]]*//p' /proc/cpuinfo 2>/dev/null | head -1)
    [ -n "$cpu" ] || cpu="?"
    cores=$(nproc 2>/dev/null || echo "?")
    ;;
esac

racedir=${RACE_DIR:-${TMPDIR:-/tmp}}
fstype="?"
if [ "$uname_s" = "Linux" ]; then
    fstype=$(stat -f -c %T "$racedir" 2>/dev/null || echo "?")
else
    mountpoint=$(df -k "$racedir" 2>/dev/null | awk 'NR==2 {print $NF}')
    if [ -n "${mountpoint:-}" ]; then
        fstype=$(mount 2>/dev/null | sed -n "s|.* on ${mountpoint} (\([^,)]*\).*|\1|p" | head -1)
    fi
    [ -n "$fstype" ] || fstype="?"
fi

echo "=========================================================================="
echo "label      : $label"
echo "os         : $os_name $os_version (build $os_build)"
echo "arch       : $arch   cores: $cores"
echo "cpu        : $cpu"
echo "temp dirs  : $racedir  (filesystem: $fstype)"
echo "compiler   : $(cc --version 2>/dev/null | head -1 || echo '?')"
echo "=========================================================================="
echo

# ------------------------------------------------------------------ build ---

for src in openat_race other_syscalls; do
    if ! cc -O2 -pthread -std=gnu11 -Wall -Wextra -o "$bin/$src" "$here/c/$src.c"; then
        echo "FATAL: failed to build c/$src.c" >&2
        exit 1
    fi
done

worst=0
note_rc() { [ "$1" -gt "$worst" ] && worst=$1; return 0; }

# --------------------------------------------------------- the C ladder ----
#
# The rate depends heavily on how much true parallelism the machine has, so a
# small or busy machine can legitimately score zero at a size where a laptop
# scores 75%. A zero is only worth reporting after the size has been raised, so
# the ladder does that automatically and the record says which stage answered.

sum_get() { printf '%s\n' "$1" | tr ' ' '\n' | sed -n "s/^$2=//p" | head -1; }

c_log=$bin/c.log
c_threads=0 c_trials=0 c_calls=0 c_suspect=0 c_control=0 c_verdict="?" c_rc=0
stage=0
for spec in "$threads $trials all" "$threads $((trials * 5)) key" "$((threads * 2)) $((trials * 5)) key"; do
    stage=$((stage + 1))
    set -- $spec
    echo "--- C probe, stage $stage: $1 threads x $2 trials ($3 rows) ---"
    "$bin/openat_race" "$1" "$2" "$3" 2>&1 | tee "$c_log"
    c_rc=${PIPESTATUS[0]}
    echo
    sum=$(grep '^SUMMARY ' "$c_log" | tail -1)
    c_threads=$1
    c_trials=$2
    c_calls=$(sum_get "$sum" suspect_calls)
    c_suspect=$(sum_get "$sum" suspect_enoent)
    c_control=$(sum_get "$sum" control_enoent)
    c_verdict=$(sum_get "$sum" verdict)
    [ "$c_verdict" = "CLEAN" ] || break
    if [ "$stage" -lt 3 ]; then
        echo "    clean at this size -- raising it before calling the machine clean"
        echo
    fi
done
note_rc "$c_rc"

echo "--- C probe, other name-creating syscalls ---"
"$bin/other_syscalls" "$threads" "$trials" 2>&1
note_rc $?
echo

# ------------------------------------------------------------- Go probe ----

go_version="-" go_calls=0 go_suspect=0 go_verdict="skipped"
if [ "${SKIP_GO:-0}" != "1" ] && command -v go >/dev/null 2>&1; then
    echo "--- Go probe (os.Root) ---"
    go_log=$bin/go.log
    ( cd "$here/go" && go run . -threads "$threads" -trials "$((trials / 4 > 0 ? trials / 4 : 1))" ) 2>&1 | tee "$go_log"
    go_rc=${PIPESTATUS[0]}
    note_rc "$go_rc"
    echo
    gsum=$(grep '^SUMMARY ' "$go_log" | tail -1)
    go_version=$(sum_get "$gsum" go)
    go_calls=$(sum_get "$gsum" suspect_calls)
    go_suspect=$(sum_get "$gsum" suspect_enoent)
    go_verdict=$(sum_get "$gsum" verdict)
    [ -n "$go_verdict" ] || go_verdict="?"
else
    echo "--- Go probe skipped (no go toolchain, or SKIP_GO=1) ---"
    echo
fi

# ---------------------------------------------------------------- record ----

record=$(printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s' \
    "$label" "$os_name" "$os_version" "$os_build" "$arch" "$cores" "$cpu" "$fstype" \
    "$stage" "$c_threads" "$c_trials" "$c_calls" "$c_suspect" "$c_control" "$c_verdict" \
    "$go_version" "$go_verdict:$go_suspect/$go_calls")

echo "RECORD	$record"
if [ -n "${RESULT_TSV:-}" ]; then
    printf '%s\n' "$record" >>"$RESULT_TSV"
fi

echo
case "$worst" in
0) echo "OVERALL: every probe interpretable (C verdict: $c_verdict, Go verdict: $go_verdict)" ;;
2) echo "OVERALL: ANOMALOUS -- a control row failed. Suspect the harness or this machine." ;;
3) echo "OVERALL: INTEGRITY -- the race was not fail-closed here. Read the tables above." ;;
*) echo "OVERALL: a probe failed to run (exit $worst)" ;;
esac
exit "$worst"
