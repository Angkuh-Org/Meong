#!/bin/sh
set -eu
cd "$(dirname "$0")"

CC=${CC:-cc}
BUILD=build
OUT=$BUILD/meong

if [ "${1:-}" = "clean" ]; then
    rm -rf "$BUILD"
    exit 0
fi
if [ "${1:-}" = "test" ]; then
    exec ./tests/test.sh
fi

prefix=${PREFIX:-/usr/local}
mkdir -p "$BUILD"

# The rulebook gate is a host tool: a cross-built Meong still gets checked.
HOSTCC=${HOSTCC:-cc}
RULECHECK=$BUILD/rulecheck
$HOSTCC -std=c11 -O2 -Isrc -D_DEFAULT_SOURCE -D_GNU_SOURCE \
    -o "$RULECHECK" tools/rulecheck.c src/rules/rulebook.c
if ! "$RULECHECK" src; then
    printf 'build refused: fix the rulebook violations above\n' >&2
    exit 1
fi

NC_CFLAGS=$(pkg-config --cflags ncursesw 2>/dev/null || true)
NC_LIBS=$(pkg-config --libs ncursesw 2>/dev/null || echo "-lncursesw")

CFLAGS="-std=c11 -O2 -pipe -Wall -Wextra -Wshadow -Wno-unused-parameter"
CFLAGS="$CFLAGS -ffunction-sections -fdata-sections"
CFLAGS="$CFLAGS -D_DEFAULT_SOURCE -D_GNU_SOURCE -Isrc"
if [ -n "${DEBUG:-}" ]; then
    CFLAGS="$CFLAGS -g -O0"
    LDFLAGS=""
else
    # This C never unwinds; dropping .eh_frame buys back the space lost when
    # hot helpers moved across translation units.
    CFLAGS="$CFLAGS -fno-asynchronous-unwind-tables -fno-unwind-tables"
    LDFLAGS="-s -Wl,-O1,--as-needed,--gc-sections"
fi

# Prefer lld when the host provides it: faster links and smaller output.
LINK_NOTE=""
if command -v ld.lld >/dev/null 2>&1; then
    LDFLAGS="$LDFLAGS -fuse-ld=lld"
    LINK_NOTE=", ld.lld"
fi

SRCS=$(find src -name '*.c' ! -path 'src/rules/*' | sort)

# shellcheck disable=SC2086
$CC $CFLAGS $NC_CFLAGS $SRCS \
    -o "$OUT" \
    $NC_LIBS $LDFLAGS

size=$(stat -c %s "$OUT" 2>/dev/null || echo '?')
printf 'built %s (%s bytes%s)\n' "$OUT" "$size" "$LINK_NOTE"

if [ "${1:-}" = "install" ]; then
    install -d "$prefix/bin"
    install -m 0755 "$OUT" "$prefix/bin/meong"
    printf 'installed %s/bin/meong\n' "$prefix"
fi
