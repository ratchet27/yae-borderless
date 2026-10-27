#!/bin/sh
# Builds mod/dist/version.dll (32-bit, matches you_are_empty.exe).
# VERSION=1.2.3 sets the DLL version resource (default 1.0.0).
set -eu
cd "$(dirname "$0")"
VERSION="${VERSION:-1.0.0}"
VERSION_NUM="$(echo "$VERSION" | tr . ,),0"
mkdir -p dist build
i686-w64-mingw32-windres src/version.rc -O coff -o build/version.res \
    -DVERSION_NUM="$VERSION_NUM" -DVERSION_STR="\\\"$VERSION\\\""
i686-w64-mingw32-gcc -O2 -s -shared -nostdlib -Wall -Wextra \
    -o dist/version.dll src/borderless.c src/present.c src/version.def build/version.res \
    -Wl,--enable-stdcall-fixup -Wl,-e,_DllMain@12 \
    -lkernel32 -luser32
cp yae_borderless.ini dist/
