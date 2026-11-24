#!/bin/zsh
# Builds the C ABI probe as an archive and as a shared library, then runs a C driver against each.
cd ${0:A:h}
sysl build-c . -o libterndb.a --header terndb.h --header-guard TERNDB_H
echo "archive exit $?"
head -12 terndb.h
clang driver/main.c libterndb.a -I. -lm -o drv-a && ./drv-a
sysl build-c --shared . -o libterndb.dylib --header terndb.h --header-guard TERNDB_H
echo "shared exit $?"
nm -gU libterndb.dylib
clang driver/main.c libterndb.dylib -Wl,-rpath,@loader_path -I. -lm -o drv-s && ./drv-s
