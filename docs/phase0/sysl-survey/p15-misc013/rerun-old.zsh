#!/bin/zsh
# Reruns the 0.11.0 probes for the two compiler bugs and the seqlock refusal on the current sysl.
D=${0:A:h}/..
for p in p08-nested/ref-for p02-seqlock/refused p08-nested/ref-flat; do
    echo "=== $p"
    cd $D/$p
    sysl run main.sysl 2>&1 | head -12
done
