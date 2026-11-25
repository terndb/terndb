#!/bin/zsh
# The C ABI's conformance run (docs/spec/c-api.md §15): builds libterndb in a release and a debug
# build, checks each generated header against §12 and each library's exports against its header,
# writes the driver's inputs, and builds and runs the C driver against each build.
#
#   caffeinate -dimsu marshal run --dir <repo> -- zsh tests/capi/run.zsh [test names...]
set -u
root=${0:A:h:h:h}
here=$root/tests/capi
out=$here/out
mkdir -p $out
failed=0

echo "== inputs"
sysl run $here/tools -- inputs $root/docs/spec/vectors/wire $out/inputs.h || exit 1

for mode in release debug; do
  echo "== build ($mode)"
  zsh $here/build.zsh $mode || exit 1
  lib=$out/$mode
  nm -gU $lib/libterndb.dylib > $lib/nm.txt
  echo "== header and exports ($mode)"
  sysl run $here/tools -- header $root/docs/spec/c-api.md $lib/terndb.h $lib/nm.txt || failed=1
  otool -L $lib/libterndb.dylib | grep libterndb.1.dylib || failed=1
  echo "== driver ($mode)"
  cc -Wall -Wextra -Werror -std=c11 -pthread $here/driver.c -I$lib -I$out -L$lib -lterndb -o $lib/driver || exit 1
  DYLD_LIBRARY_PATH=$lib $lib/driver "$@" || failed=1
done

exit $failed
