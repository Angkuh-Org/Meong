#!/bin/sh
# Meong installer: download a release binary, verify its SHA-256, install it.
set -eu

repo=${MEONG_REPO:-Angkuh-Org/Meong}
version=${1:-${MEONG_VERSION:-latest}}

case "$version" in
    help | -h | --help)
        printf 'meong installer: download a release binary, verify it, install it.\n\n'
        printf 'usage: install.sh [VERSION]\n\n'
        printf 'environment:\n'
        printf '  %-20s %s\n' "MEONG_INSTALL_DIR" "install directory (default: ~/.local/bin)"
        printf '  %-20s %s\n' "MEONG_VERSION" "version to install (default: latest)"
        printf '  %-20s %s\n' "MEONG_REPO" "repository override"
        printf '  %-20s %s\n' "MEONG_DOWNLOAD_BASE" "release URL override"
        printf '\npin a version:\n'
        printf '  curl -fsSL .../install.sh | sh -s -- v0.6.1\n'
        exit 0
        ;;
esac

if [ "$(uname -s)" != Linux ]; then
    echo "meong: only Linux binaries are published" >&2
    exit 1
fi

if [ -n "${TERMUX_VERSION:-}" ] ||
    [ "${PREFIX:-}" = "/data/data/com.termux/files/usr" ] ||
    [ "$(uname -o 2>/dev/null || true)" = Android ]; then
    echo "meong: Termux/Android is not supported (glibc binary); build from source: ./build.sh" >&2
    exit 1
fi

case "$(uname -m)" in
    x86_64 | amd64) arch=x86_64 ;;
    aarch64 | arm64) arch=aarch64 ;;
    *)
        echo "meong: unsupported architecture: $(uname -m)" >&2
        exit 1
        ;;
esac

asset=meong-linux-$arch
if [ -n "${MEONG_DOWNLOAD_BASE:-}" ]; then
    base=$MEONG_DOWNLOAD_BASE
elif [ "$version" = latest ]; then
    base=https://github.com/$repo/releases/latest/download
else
    base=https://github.com/$repo/releases/download/$version
fi
case "$base" in
    http://*) proto='=https,http' ;;
    *) proto='=https' ;;
esac

if [ -n "${MEONG_INSTALL_DIR:-}" ]; then
    dest=$MEONG_INSTALL_DIR
elif [ "$(id -u)" -eq 0 ]; then
    dest=/usr/local/bin
else
    dest=$HOME/.local/bin
fi

ui=
if [ -t 1 ] && [ "${TERM:-}" != dumb ]; then
    ui=1
fi
color=
if [ -n "$ui" ] && [ -z "${NO_COLOR:-}" ]; then
    color=1
fi
utf8=
case "${LC_ALL:-${LC_CTYPE:-${LANG:-}}}" in
    *UTF-8* | *utf8* | *UTF8*) utf8=1 ;;
esac

reset='' bold='' dim='' purple='' cyan='' green='' yellow='' red=''
if [ -n "$color" ]; then
    reset=$(printf '\033[0m')
    bold=$(printf '\033[1m')
    dim=$(printf '\033[2m')
    purple=$(printf '\033[38;5;141m')
    cyan=$(printf '\033[38;5;117m')
    green=$(printf '\033[38;5;84m')
    yellow=$(printf '\033[38;5;220m')
    red=$(printf '\033[38;5;203m')
fi

if [ -n "$utf8" ]; then
    ok='✔'
    bad='✖'
    warn='!'
    sep='·'
    rule_char='─'
    frames='⠋ ⠙ ⠹ ⠸ ⠼ ⠴ ⠦ ⠧ ⠇ ⠏'
    icon_width=1
else
    ok='[+]'
    bad='[x]'
    warn='[!]'
    sep='-'
    rule_char='-'
    frames='[|] [/] [-] [\]'
    icon_width=3
fi

repeat() {
    r_out=
    r_i=0
    while [ "$r_i" -lt "$2" ]; do
        r_out=$r_out$1
        r_i=$((r_i + 1))
    done
    printf '%s' "$r_out"
}

clear_line() {
    printf '\r%62s\r' ''
}

animate() {
    a_label=$1
    a_value=$2
    while :; do
        for a_frame in $frames; do
            printf '\r  %s%s%s %-9s %s' "$cyan" "$a_frame" "$reset" "$a_label" "$a_value"
            sleep 0.08
        done
    done
}

stop_job() {
    if [ -n "${anim:-}" ]; then
        kill "$anim" 2>/dev/null || true
        wait "$anim" 2>/dev/null || true
        anim=
        clear_line
    fi
}

show_log() {
    if [ -s "$1" ]; then
        sed 's/^/    /' "$1" >&2
    fi
}

fail() {
    stop_job
    if [ -n "$ui" ]; then
        printf '\n  %s%s%s %s\n' "$red" "$bad" "$reset" "$1" >&2
    else
        printf 'meong: %s\n' "$1" >&2
    fi
    exit 1
}

step() {
    printf '  %s%s%s %-9s %s\n' "$green" "$ok" "$reset" "$1" "$2"
}

note() {
    printf '  %s%s%s %-9s %s\n' "$yellow" "$warn" "$reset" "$1" "$2"
}

human_size() {
    awk -v bytes="$1" 'BEGIN {
        if (bytes >= 1048576) printf "%.1f MiB", bytes / 1048576;
        else if (bytes >= 1024) printf "%.1f KiB", bytes / 1024;
        else printf "%d B", bytes;
    }'
}

banner() {
    printf '\n  %s%sM E O N G%s  %s%s%s  %sprobably a cat%s\n' \
        "$bold" "$purple" "$reset" "$dim" "$sep" "$reset" "$dim" "$reset"
    printf '  %s%s%s\n' "$dim" "$(repeat "$rule_char" 36)" "$reset"
    printf '  %slinux / %s %s %s%s\n\n' "$dim" "$arch" "$sep" "$version" "$reset"
}

tmp=$(mktemp -d)
anim=
job=
cleaned=
dest_set=
cleanup() {
    if [ -n "$cleaned" ]; then
        return 0
    fi
    cleaned=1
    if [ -n "${anim:-}" ]; then
        kill "$anim" 2>/dev/null || true
        wait "$anim" 2>/dev/null || true
        anim=
    fi
    if [ -n "${job:-}" ]; then
        kill "$job" 2>/dev/null || true
        wait "$job" 2>/dev/null || true
        job=
    fi
    if [ -n "$dest_set" ]; then
        rm -f "$dest/.meong.new"
    fi
    rm -rf "$tmp"
}
trap cleanup EXIT
trap 'cleanup; exit 1' INT TERM

if [ -n "$ui" ]; then
    banner
else
    printf 'downloading %s (%s)\n' "$asset" "$version"
fi

(
    curl --proto "$proto" --tlsv1.2 -fsSL "$base/$asset" -o "$tmp/$asset" &&
        curl --proto "$proto" --tlsv1.2 -fsSL "$base/$asset.sha256" -o "$tmp/$asset.sha256"
) 2>"$tmp/curl.log" &
job=$!
if [ -n "$ui" ]; then
    animate "download" "$asset" &
    anim=$!
fi
if ! wait "$job"; then
    stop_job
    show_log "$tmp/curl.log"
    fail "download failed: $asset"
fi
job=
stop_job

if [ -n "$ui" ]; then
    step "download" "$asset ($(human_size "$(wc -c < "$tmp/$asset")"))"
    (cd "$tmp" && sha256sum -c "$asset.sha256") >"$tmp/sha.log" 2>&1 &
    job=$!
    animate "checksum" "sha256" &
    anim=$!
    if ! wait "$job"; then
        stop_job
        show_log "$tmp/sha.log"
        fail "SHA-256 verification failed"
    fi
    job=
    stop_job
    step "checksum" "sha256 verified"
else
    (cd "$tmp" && sha256sum -c "$asset.sha256")
fi

chmod 0755 "$tmp/$asset"
if ! installed=$("$tmp/$asset" --version 2>/dev/null) || [ -z "$installed" ]; then
    fail "downloaded binary does not run (glibc >= 2.35, libncursesw.so.6, libtinfo.so.6 required)"
fi

if [ -n "$ui" ]; then
    step "verify" "$installed"
fi

dest_set=1
mkdir -p "$dest"
if ! install -m 0755 "$tmp/$asset" "$dest/.meong.new" ||
    ! mv -f "$dest/.meong.new" "$dest/meong"; then
    rm -f "$dest/.meong.new"
    fail "install failed: $dest"
fi

if [ -n "$ui" ]; then
    step "install" "$dest/meong"
else
    printf 'installed %s -> %s/meong\n' "$installed" "$dest"
fi

case ":${PATH:-}:" in
    *":$dest:"*) ;;
    *)
        if [ -n "$ui" ]; then
            note "path" "$dest is not on PATH"
            printf '%s%s%s%s\n' "$dim" "$(repeat ' ' $((13 + icon_width)))" "export PATH=\"$dest:\$PATH\"" "$reset"
        else
            printf 'add it to PATH:\n  export PATH="%s:%s"\n' "$dest" "\$PATH"
        fi
        ;;
esac

if [ -n "$ui" ]; then
    printf '\n  %snext%s  meong\n' "$dim" "$reset"
fi
