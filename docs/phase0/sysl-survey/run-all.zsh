#!/bin/zsh
# Runs every probe given (default: all) and writes each one's output to <probe>/out.txt.
D=${0:A:h}
probes=("$@")
if (( ${#probes} == 0 )); then
    probes=(p01-atomics p02-seqlock p02-seqlock/refused p03-page p04-threads p05-io p06-simd p06-simd/bitmask p07-jit p08-nested p08-nested/ref p09-export p10-hash)
fi
for p in $probes; do
    dir=$D/$p
    cd $dir
    echo "=== $p"
    case $p in
        p07-jit)
            sysl run --link-path /opt/homebrew/opt/llvm/lib main.sysl > out.txt 2>&1
            ;;
        p08-nested)
            SYSL_COUNT_ALLOCS=1 sysl run main.sysl > out.txt 2>&1
            echo "--- emit-llvm: retain/release calls in the walk" >> out.txt
            sysl emit-llvm main.sysl > walk.ll 2>> out.txt
            sysl emit-llvm -O2 main.sysl > walk-O2.ll 2>> out.txt
            echo "exit $?" >> out.txt
            ;;
        p09-export)
            {
                echo "--- build-c"
                sysl build-c . -o libdbx.a
                echo "exit $?"
                echo "--- build-lib"
                sysl build-lib .
                echo "exit $?"
                echo "--- C driver"
                clang driver/main.c -I. libdbx.a -lm -o driver-bin
                echo "exit $?"
                ./driver-bin
                echo "exit $?"
            } > out.txt 2>&1
            ;;
        *)
            sysl run main.sysl > out.txt 2>&1
            ;;
    esac
    echo "exit $?" >> out.txt
    cat out.txt | head -40
done
