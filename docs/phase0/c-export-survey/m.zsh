#!/bin/zsh
# usage: m.zsh <dir> <label> <cmd...>   -- runs through marshal, prints the job log
dir=$1; label=$2; shift 2
out=$(caffeinate -dimsu /Users/ed/.local/bin/marshal run --quick --units 2 --deadline 2m --label "survey-cexport $label" --dir $dir -- "$@" 2>&1)
rc=$?
log=$(print -r -- "$out" | grep -o '/Users/ed/.marshal/jobs/[0-9]*\.log' | head -1)
grep -v '^marshal:' $log
echo "== exit $rc"
