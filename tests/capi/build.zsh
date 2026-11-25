#!/bin/zsh
# Builds libterndb and its header into tests/capi/out/<mode>/ (mode: release or debug).
set -u
root=${0:A:h:h:h}
mode=${1:-release}
out=$root/tests/capi/out/$mode
mkdir -p $out
flag=--release
if [[ $mode == debug ]]; then flag=--debug; fi
sysl build-c $root/capi $flag --shared -o $out/libterndb.dylib --header $out/terndb.h --header-guard TERNDB_H \
  --install-name libterndb.1.dylib --compatibility-version 1.0.0 --current-version 1.0.0 || exit 1
ln -sf libterndb.dylib $out/libterndb.1.dylib
