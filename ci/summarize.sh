#!/usr/bin/env bash
# summarize.sh -- turn the one-line records probe.sh emits into a markdown table.
#
# Usage: ci/summarize.sh result-*/record.tsv > table.md
#
# The column order is the one probe.sh writes; it is spelled out here so the two
# files can be checked against each other.
set -u

if [ "$#" -eq 0 ]; then
    echo "usage: $0 <record.tsv> [...]" >&2
    exit 1
fi

cat "$@" | sort | awk -F'\t' '
BEGIN {
    print "| runner | OS | build | arch | cores | CPU | fs | size | openat O_CREAT spurious ENOENT | controls | verdict | Go |"
    print "| --- | --- | --- | --- | --: | --- | --- | --- | --- | --: | --- | --- |"
}
{
    label=$1; os=$2; ver=$3; build=$4; arch=$5; cores=$6; cpu=$7; fs=$8;
    stage=$9; threads=$10; trials=$11; calls=$12; suspect=$13; control=$14;
    verdict=$15; goversion=$16; gores=$17;

    rate = (calls > 0) ? sprintf("%d / %d (%.0f%%)", suspect, calls, 100*suspect/calls) \
                       : sprintf("%d / ?", suspect);
    size = sprintf("%sx%s (stage %s)", threads, trials, stage);
    mark = (verdict == "REPRODUCES") ? "**REPRODUCES**" \
         : (verdict == "CLEAN")      ? "clean" : verdict;
    go = (goversion == "-") ? "-" : sprintf("%s %s", goversion, gores);

    printf "| `%s` | %s %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s |\n",
        label, os, ver, build, arch, cores, cpu, fs, size, rate, control, mark, go;

    if (verdict == "REPRODUCES") repro++
    else if (verdict == "CLEAN") clean++
    else odd++
    n++
}
END {
    printf "\n%d runner(s): %d reproduced, %d clean, %d neither.\n", n, repro+0, clean+0, odd+0
    if (odd > 0)
        print "\nA verdict that is neither means a control row failed too (ANOMALOUS) or the race was not fail-closed (INTEGRITY). Read that job'"'"'s log before drawing any conclusion from this table."
    if (clean > 0 && repro > 0)
        print "\nA clean row is only evidence of absence if its size column shows the ladder was climbed (stage 3). Fewer cores means less true parallelism and a lower rate."
}
'
