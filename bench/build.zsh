#!/bin/zsh
# Builds the two programs a benchmark run needs into `<bin dir>`, as a release would build them:
# release mode (integer overflow wraps rather than traps) at -O3, for the generic target CPU.
#   build.zsh <repo root> <bin dir>
set -eu
root=$1
bins=$2
mkdir -p $bins
cd $root
sysl --version >$bins/sysl-version.txt
sysl build cli --release -O3 -o $bins/terndb
sysl build bench --release -O3 --include-path duckdb=/opt/homebrew/include --link-path /opt/homebrew/lib -o $bins/bench
