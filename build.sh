#!/bin/sh
# Meong build wrapper. POSIX sh only.
set -eu

SELF_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
SELF=$SELF_DIR/$(basename -- "$0")
cd "$SELF_DIR"

CC=${CC:-cc}
HOSTCC=${HOSTCC:-cc}
BUILD=build
OUT=$BUILD/meong
RULECHECK=$BUILD/rulecheck
STAMP=$BUILD/meong.stamp
LOG=$BUILD/.build.log
prefix=${PREFIX:-/usr/local}
LINK_NOTE=
SPIN_PID=

if [ -t 1 ] && [ -z "${NO_COLOR:-}" ] && [ "${TERM:-}" != dumb ]; then
    PRETTY=1
    RESET=$(printf '\033[0m')
    BOLD=$(printf '\033[1m')
    DIM=$(printf '\033[2m')
    GREEN=$(printf '\033[1;32m')
    CYAN=$(printf '\033[1;36m')
    BLUE=$(printf '\033[1;34m')
    YELLOW=$(printf '\033[1;33m')
    MAGENTA=$(printf '\033[1;35m')
    RED=$(printf '\033[1;31m')
    I_BUILD=$(printf '\357\206\262')
    I_OK=$(printf '\357\201\230')
    I_INSTALL=$(printf '\357\200\231')
    I_CLEAN=$(printf '\357\207\270')
    I_HELP=$(printf '\357\201\231')
    I_FAIL=$(printf '\357\200\215')
else
    PRETTY=0
    RESET= BOLD= DIM= GREEN= CYAN= BLUE= YELLOW= MAGENTA= RED=
    I_BUILD='[+]'
    I_OK='[ok]'
    I_INSTALL='[>]'
    I_CLEAN='[-]'
    I_HELP='[?]'
    I_FAIL='[x]'
fi

report() {
    # report ICON ASCII STYLE STATE PATH [SIZE] [NOTE]
    icon=$1
    ascii=$2
    style=$3
    state=$4
    path=$5
    size=${6:-}
    note=${7:-}
    [ "$PRETTY" = 1 ] || icon=$ascii
    if [ -n "$size" ]; then
        printf '%s%s%s  %s%-10s%s  %s  %s bytes' \
            "$style" "$icon" "$RESET" "$style" "$state" "$RESET" \
            "$path" "$size"
        if [ -n "$note" ]; then
            if [ "$PRETTY" = 1 ]; then
                printf '  %s· %s%s' "$DIM" "$note" "$RESET"
            else
                printf '  %s' "$note"
            fi
        fi
        printf '\n'
    else
        printf '%s%s%s  %s%-10s%s  %s\n' \
            "$style" "$icon" "$RESET" "$style" "$state" "$RESET" "$path"
    fi
}

group_size() {
    value=$1
    case $value in
        '' | *[!0-9]*)
            printf '%s' "$value"
            return 0
            ;;
    esac
    rest=$value
    out=
    while [ ${#rest} -gt 3 ]; do
        head=${rest%???}
        out=",${rest#"$head"}$out"
        rest=$head
    done
    printf '%s%s' "$rest" "$out"
}

binary_size() {
    group_size "$(stat -c %s "$OUT" 2>/dev/null || printf '?')"
}

install_binary() {
    install -d "$prefix/bin"
    install -m 0755 "$OUT" "$prefix/bin/meong"
    report "$I_INSTALL" '[>]' "$MAGENTA" installed "$prefix/bin/meong" "$(binary_size)"
}

usage() {
    printf '%sMeong build wrapper%s\n\n' "$BOLD" "$RESET"
    printf '%susage:%s ./build.sh [COMMAND]\n\n' "$DIM" "$RESET"
    printf '%scommands:%s\n' "$BOLD" "$RESET"
    printf '  %s%s%s  %-10s %s\n' "$GREEN" "$I_BUILD" "$RESET" "(none)" "build -> build/meong"
    printf '  %s%s%s  %-10s %s\n' "$MAGENTA" "$I_INSTALL" "$RESET" "install" "build, then install -> \$PREFIX/bin/meong"
    printf '  %s%s%s  %-10s %s\n' "$YELLOW" "$I_CLEAN" "$RESET" "clean" "remove build/"
    printf '  %s%s%s  %-10s %s\n' "$CYAN" "$I_OK" "$RESET" "test" "run ./tests/test.sh"
    printf '  %s%s%s  %-10s %s\n' "$BLUE" "$I_HELP" "$RESET" "help" "show this help"
    printf '\n%senvironment:%s\n' "$BOLD" "$RESET"
    printf '  %-13s %s\n' "CC / HOSTCC" "target / rulebook compiler (default: cc)"
    printf '  %-13s %s\n' "PREFIX" "install prefix (default: /usr/local)"
    printf '  %-13s %s\n' "MEONG_VERSION" "version string compiled into the binary"
    printf '  %-13s %s\n' "DEBUG" "non-empty: -O0 -g build with symbols"
    printf '  %-13s %s\n' "NO_COLOR" "non-empty: plain ASCII status lines"
    printf '\n%sexamples:%s\n' "$BOLD" "$RESET"
    printf '  ./build.sh\n'
    printf '  ./build.sh install\n'
    printf '  PREFIX=$HOME/.local ./build.sh install\n'
}

SPIN_FRAMES='\342\240\213 \342\240\231 \342\240\271 \342\240\270 \342\240\274 \342\240\264 \342\240\246 \342\240\247 \342\240\207 \342\240\217'

spin_start() {
    [ "$PRETTY" = 1 ] || return 0
    SPIN_LABEL=$1
    SPIN_TARGET=$2
    SPIN_STYLE=$3
    SPIN_START=$(date +%s)
    (
        # shellcheck disable=SC2086
        set -- $SPIN_FRAMES
        while :; do
            frame=$1
            shift
            set -- "$@" "$frame"
            elapsed=$(( $(date +%s) - SPIN_START ))
            printf '\r\033[2K%s%s%s %s%s%s %s... %ss' \
                "$SPIN_STYLE" "$frame" "$RESET" "$DIM" "$SPIN_LABEL" "$RESET" \
                "$SPIN_TARGET" "$elapsed"
            sleep 0.08 2>/dev/null || sleep 1
        done
    ) &
    SPIN_PID=$!
}

spin_stop() {
    if [ -n "$SPIN_PID" ]; then
        kill "$SPIN_PID" 2>/dev/null || true
        wait "$SPIN_PID" 2>/dev/null || true
        SPIN_PID=
        if [ "$PRETTY" = 1 ]; then
            printf '\r\033[2K'
        fi
    fi
    return 0
}

run_logged() {
    rm -f "$LOG"
    "$@" >"$LOG" 2>&1
}

flush_log() {
    if [ -s "$LOG" ]; then
        cat "$LOG" >&2
    fi
}

cleanup() {
    spin_stop
    rm -f "$LOG"
}

trap cleanup EXIT
trap 'cleanup; exit 130' HUP INT TERM

install_after=0
case "${1:-}" in
    '') ;;
    help | -h | --help)
        usage
        exit 0
        ;;
    clean)
        rm -rf "$BUILD"
        report "$I_CLEAN" '[-]' "$YELLOW" removed "$BUILD/"
        exit 0
        ;;
    test)
        exec ./tests/test.sh
        ;;
    install)
        install_after=1
        ;;
    *)
        printf 'build.sh: unknown command: %s\n\n' "$1" >&2
        usage >&2
        exit 2
        ;;
esac

mkdir -p "$BUILD"

NC_CFLAGS=$(pkg-config --cflags ncursesw 2>/dev/null || true)
NC_LIBS=$(pkg-config --libs ncursesw 2>/dev/null || echo "-lncursesw")

CFLAGS="-std=c11 -O2 -pipe -Wall -Wextra -Wshadow -Wno-unused-parameter"
CFLAGS="$CFLAGS -ffunction-sections -fdata-sections"
CFLAGS="$CFLAGS -D_DEFAULT_SOURCE -D_GNU_SOURCE -Isrc"
if [ -n "${MEONG_VERSION:-}" ]; then
    CFLAGS="$CFLAGS -DAPP_VERSION=\"$MEONG_VERSION\""
fi
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
if command -v ld.lld >/dev/null 2>&1; then
    LDFLAGS="$LDFLAGS -fuse-ld=lld"
    LINK_NOTE=ld.lld
fi

SRCS=$(find src -name '*.c' ! -path 'src/rules/*' | sort)

stamp_text() {
    # shellcheck disable=SC2086
    cc_ver=$($CC --version 2>/dev/null | head -n 1 || true)
    # shellcheck disable=SC2086
    host_ver=$($HOSTCC --version 2>/dev/null | head -n 1 || true)
    printf 'cc: %s [%s]\nhostcc: %s [%s]\ncflags: %s\nldflags: %s\nnc-cflags: %s\nnc-libs: %s\nversion: %s\ndebug: %s\n' \
        "$CC" "$cc_ver" "$HOSTCC" "$host_ver" \
        "$CFLAGS" "$LDFLAGS" "$NC_CFLAGS" "$NC_LIBS" \
        "${MEONG_VERSION:-}" "${DEBUG:-}"
}

STAMP_TEXT=$(stamp_text)

needs_build() {
    [ -f "$OUT" ] || return 0
    [ -f "$STAMP" ] || return 0
    [ -x "$RULECHECK" ] || return 0
    [ "$(cat "$STAMP")" = "$STAMP_TEXT" ] || return 0
    if [ -n "$(find src tools/rulecheck.c "$SELF" -newer "$OUT" -print -quit 2>/dev/null)" ]; then
        return 0
    fi
    if [ -n "$(find tools/rulecheck.c src/rules "$SELF" -newer "$RULECHECK" -print -quit 2>/dev/null)" ]; then
        return 0
    fi
    return 1
}

if ! needs_build; then
    report "$I_OK" '[=]' "$CYAN" "up to date" "$OUT" "$(binary_size)" "$LINK_NOTE"
    if [ "$install_after" = 1 ]; then
        install_binary
    fi
    exit 0
fi

# The rulebook gate is a host tool: a cross-built Meong still gets checked.
if [ ! -x "$RULECHECK" ] ||
    [ -n "$(find tools/rulecheck.c src/rules "$SELF" -newer "$RULECHECK" -print -quit 2>/dev/null)" ]; then
    spin_start "checking" rulebook "$BLUE"
    # shellcheck disable=SC2086
    if run_logged $HOSTCC -std=c11 -O2 -Isrc -D_DEFAULT_SOURCE -D_GNU_SOURCE \
        -o "$RULECHECK" tools/rulecheck.c src/rules/rulebook.c; then
        spin_stop
        flush_log
    else
        spin_stop
        cat "$LOG" >&2
        report "$I_FAIL" '[x]' "$RED" refused "$RULECHECK"
        exit 1
    fi
fi

if "$RULECHECK" src >"$LOG" 2>&1; then
    rule_info=$(sed -e 's/^rulebook: //' -e 's/ ok (/ · /' -e 's/)$//' \
        -e 's/loc-per-file: /loc-per-file /' "$LOG")
    report "$I_OK" '[ok]' "$CYAN" rulebook "$rule_info"
else
    cat "$LOG" >&2
    report "$I_FAIL" '[x]' "$RED" refused "src"
    exit 1
fi

if [ "$PRETTY" = 0 ]; then
    # shellcheck disable=SC2086
    set -- $SRCS
    printf '  [C] compiling %d sources -> %s\n' "$#" "$OUT"
fi

spin_start "building" "$OUT" "$GREEN"
# shellcheck disable=SC2086
if run_logged $CC $CFLAGS $NC_CFLAGS $SRCS -o "$OUT" $NC_LIBS $LDFLAGS; then
    spin_stop
    flush_log
else
    spin_stop
    cat "$LOG" >&2
    report "$I_FAIL" '[x]' "$RED" failed "$OUT"
    exit 1
fi

printf '%s\n' "$STAMP_TEXT" >"$STAMP"

report "$I_BUILD" '[+]' "$GREEN" built "$OUT" "$(binary_size)" "$LINK_NOTE"

if [ "$install_after" = 1 ]; then
    install_binary
fi
