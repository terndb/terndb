#!/bin/zsh
# One benchmark job: `run.zsh <bin dir> <mode> <scale> <out file> [workloads]`, mode one of verify,
# inproc, server, concurrent. It expects PostgreSQL already running from its own data directory
# (bench/README.md), makes a fresh database for the job, starts a TernDB server for the modes that
# need one, runs the harness, and stops the server it started.
set -eu

bins=$1
mode=$2
scale=$3
out=$4
only=${5:-}

pgbin=/opt/homebrew/opt/postgresql@16/bin
sockdir=/Users/ed/.claude/tmp/tdbb
conninfo="host=$sockdir dbname=bench"
export TERNDB_PASSWORD=bench

$pgbin/dropdb -h $sockdir --if-exists bench
$pgbin/createdb -h $sockdir bench

server_pid=""
if [[ $mode != inproc ]]; then
  rm -f $sockdir/terndb.sock
  $bins/terndb serve --socket $sockdir/terndb.sock >$out.server.log 2>&1 &
  server_pid=$!
  for i in {1..100}; do
    [[ -S $sockdir/terndb.sock ]] && break
    sleep 0.1
  done
fi

stop_server() {
  if [[ -n $server_pid ]]; then
    kill -INT $server_pid 2>/dev/null || true
    wait $server_pid 2>/dev/null || true
  fi
}
trap stop_server EXIT

args=()
[[ -n $only ]] && args=(--only $only)

case $mode in
  verify)
    $bins/bench verify --scale $scale --pg $conninfo --socket $sockdir/terndb.sock --out $out $args ;;
  inproc)
    $bins/bench time --pair inproc --scale $scale --out $out $args ;;
  server)
    $bins/bench time --pair server --scale $scale --pg $conninfo --socket $sockdir/terndb.sock --out $out $args ;;
  concurrent)
    $bins/bench concurrent --engine terndb --clients 1 --scale $scale --socket $sockdir/terndb.sock --load yes --out $out
    for c in 4 16; do
      $bins/bench concurrent --engine terndb --clients $c --scale $scale --socket $sockdir/terndb.sock --out $out
    done
    $bins/bench concurrent --engine postgres --clients 1 --scale $scale --pg $conninfo --load yes --out $out
    for c in 4 16; do
      $bins/bench concurrent --engine postgres --clients $c --scale $scale --pg $conninfo --out $out
    done ;;
esac
