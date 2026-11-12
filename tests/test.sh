#!/bin/sh
set -eu
cd "$(dirname "$0")/.."

TMP=$(mktemp -d)
MOCK_PID=""
TMUX_SESS=""
CHECKS=0
BIN=${MEONG_BIN:-./build/meong}
export MEONG_BIN="$BIN"
HOSTCC=${HOSTCC:-cc}
NC_LIBS=$(pkg-config --libs ncursesw 2>/dev/null || echo "-lncursesw")

cleanup() {
    if [ -n "$TMUX_SESS" ]; then
        tmux kill-session -t "$TMUX_SESS" 2>/dev/null || true
    fi
    if [ -n "$MOCK_PID" ]; then
        kill "$MOCK_PID" 2>/dev/null || true
        wait "$MOCK_PID" 2>/dev/null || true
    fi
    rm -rf "$TMP"
}
trap cleanup EXIT HUP INT TERM

ok() {
    CHECKS=$((CHECKS + 1))
    printf 'ok %d - %s\n' "$CHECKS" "$1"
}

fail() {
    printf 'FAIL: %s\n' "$1" >&2
    if [ -f "$TMP/mock.log" ]; then
        printf '%s\n' '--- mock log ---' >&2
        cat "$TMP/mock.log" >&2
    fi
    exit 1
}

./build.sh

if [ "${ASAN_BUILD:-0}" = "1" ]; then
    strings "$BIN" | grep -q __asan_init || fail "ASAN_BUILD=1 but $BIN is not ASan-instrumented"
fi

"$PWD/build/rulecheck" src >/dev/null || fail "rulebook audit failed on src/"
ok "rulebook: src/ stays within the law"

mkdir -p "$TMP/law"
seq 1 2001 >"$TMP/law/big.c"
if "$PWD/build/rulecheck" "$TMP/law" >/dev/null 2>&1; then
    fail "rulebook accepted a 2001-line source file"
fi
ok "rulebook: loc-per-file law bites"

$HOSTCC -std=c11 -O2 -D_DEFAULT_SOURCE -D_GNU_SOURCE -Isrc \
    -o "$TMP/json_skip_test" \
    tests/json_skip_test.c src/json/json.c src/util/util.c \
    || fail "json skip test did not compile"
"$TMP/json_skip_test" >/dev/null \
    || fail "json skip table diverged from the recursive walk"
ok "json: cached skip matches the recursive walk"

LAYOUT_SRCS=$(find src -name '*.c' ! -path 'src/rules/*' \
    ! -path 'src/main.c' ! -path 'src/ui/view.c' | sort)
# shellcheck disable=SC2086
$HOSTCC -std=c11 -O2 -D_DEFAULT_SOURCE -D_GNU_SOURCE -Isrc -Isrc/ui \
    -o "$TMP/layout_truncate_test" \
    tests/layout_truncate_test.c $LAYOUT_SRCS $NC_LIBS \
    || fail "layout test did not compile"
"$TMP/layout_truncate_test" >/dev/null \
    || fail "layout truncation left stale rows"
ok "layout: /retry truncation drops removed rows"

PORT=$(python3 - <<'PY'
import socket
s = socket.socket()
s.bind(("127.0.0.1", 0))
print(s.getsockname()[1])
s.close()
PY
)

python3 tests/python/mock_paig.py --port "$PORT" --port-file "$TMP/port" \
    >"$TMP/mock.log" 2>&1 &
MOCK_PID=$!

i=0
while [ ! -s "$TMP/port" ] && [ "$i" -lt 100 ]; do
    i=$((i + 1))
    sleep 0.05
done
[ -s "$TMP/port" ] || fail "mock gateway did not start"
PORT=$(cat "$TMP/port")
URL="http://127.0.0.1:$PORT"

models=$("$BIN" --url "$URL" --list-models) || fail "list-models failed"
printf '%s\n' "$models" | grep -Fqx 'mock/split' || fail "mock/split missing"
printf '%s\n' "$models" | grep -Fqx 'mock/error200' || fail "mock/error200 missing"
ok "list models"

models=$("$BIN" --url "$URL/v1" --list-models) || fail "base /v1 failed"
printf '%s\n' "$models" | grep -Fqx 'mock/plain' || fail "base /v1 list wrong"
ok "url with /v1 base"

models=$(PAIG_HOST=127.0.0.1 PAIG_PORT="$PORT" "$BIN" --list-models) \
    || fail "PAIG_HOST/PAIG_PORT failed"
printf '%s\n' "$models" | grep -Fqx 'mock/slow' || fail "env list wrong"
ok "PAIG_HOST/PAIG_PORT"

out=$("$BIN" --url "$URL" --model mock/plain --oneshot 'hi') \
    || fail "oneshot failed"
[ "$out" = "pong from mock" ] || fail "oneshot output: $out"
ok "oneshot"

out=$("$BIN" --url "$URL" --model mock/split --stream --oneshot 'hi') \
    || fail "stream failed"
case "$out" in
    *"Hello, Meong"*) ;;
    *) fail "stream lost the first tokens" ;;
esac
case "$out" in
    *"🌊🐟"*) ;;
    *) fail "stream lost split UTF-8" ;;
esac
case "$out" in
    *"int x = 1;"*) ;;
    *) fail "stream lost the code block" ;;
esac
ok "streaming SSE, split chunks, code block"

out=$("$BIN" --url "$URL" --model mock/json200 --stream --oneshot 'hi') \
    || fail "stream raw-json fallback failed"
[ "$out" = "pong from mock" ] || fail "stream raw-json output: $out"
ok "stream mode accepts a non-SSE 200 reply"

if "$BIN" --url "$URL" --model mock/error --oneshot hi \
    >/dev/null 2>"$TMP/err"; then
    fail "429 error returned success"
fi
grep -Fq '429' "$TMP/err" || fail "429 status missing from error"
grep -Fq 'mock says no' "$TMP/err" || fail "429 message missing"
ok "http error shape"

if "$BIN" --url "$URL" --model mock/error200 --stream --oneshot hi \
    >/dev/null 2>"$TMP/err"; then
    fail "200 bare-json stream error returned success"
fi
grep -Fq 'mock says no' "$TMP/err" || fail "bare-json stream error message missing"
ok "200 bare json error in stream mode"

if "$BIN" --url "$URL" --model mock/error200 --oneshot hi \
    >/dev/null 2>"$TMP/err"; then
    fail "200 bare-json error returned success"
fi
grep -Fq 'mock says no' "$TMP/err" || fail "bare-json error message missing"
ok "200 bare json error in oneshot mode"

if "$BIN" --url http://127.0.0.1:1 --oneshot hi \
    >/dev/null 2>"$TMP/err"; then
    fail "connection refused returned success"
fi
grep -Fq 'refused' "$TMP/err" || fail "connection refused message missing"
ok "connection refused"

MEONG_CONFIG_DIR="$TMP/config" XDG_CACHE_HOME="$TMP/cache" \
    timeout 30 python3 tests/python/tui_test.py "$URL" || fail "tui pty test"
CHECKS=$((CHECKS + 1))

if command -v tmux >/dev/null 2>&1; then
    BIN_ABS=$(readlink -f "$BIN")
    TMUX_SESS="Meong-gap-$$"
    tmux kill-session -t "$TMUX_SESS" 2>/dev/null || true
    tmux new-session -d -s "$TMUX_SESS" -x 110 -y 40 \
        "env TERM=xterm-256color LC_ALL=C.UTF-8 LANG=C.UTF-8 \
         MEONG_CONFIG_DIR='$TMP/config' XDG_CACHE_HOME='$TMP/cache' \
         '$BIN_ABS' --url '$URL' --model mock/split --no-mouse"
    i=0
    while [ "$i" -lt 100 ]; do
        if tmux capture-pane -p -t "$TMUX_SESS" 2>/dev/null |
            grep -Fq 'ask anything'; then
            break
        fi
        i=$((i + 1))
        sleep 0.05
    done
    tmux send-keys -l -t "$TMUX_SESS" 'hello'
    tmux send-keys -t "$TMUX_SESS" Enter
    for needle in 'Hello, Meong' '🌊🐟' 'int x = 1;' 'ctx 42'; do
        i=0
        while [ "$i" -lt 100 ]; do
            if tmux capture-pane -p -t "$TMUX_SESS" 2>/dev/null |
                grep -Fq "$needle"; then
                break
            fi
            i=$((i + 1))
            sleep 0.05
        done
        tmux capture-pane -p -t "$TMUX_SESS" 2>/dev/null |
            grep -Fq "$needle" || fail "streamed reply missing: $needle"
    done
    ok "streamed reply rendered on screen"
    tmux send-keys -l -t "$TMUX_SESS" 'aku'
    found=0
    i=0
    while [ "$i" -lt 100 ]; do
        if tmux capture-pane -p -t "$TMUX_SESS" 2>/dev/null |
            grep -Fq '❯ aku'; then
            found=1
            break
        fi
        i=$((i + 1))
        sleep 0.05
    done
    [ "$found" = 1 ] || fail "input text is glued to the prompt"
    ok "prompt gap before input text (tmux screen)"

    tmux send-keys -t "$TMUX_SESS" C-u
    tmux send-keys -l -t "$TMUX_SESS" '/model'
    tmux send-keys -t "$TMUX_SESS" Enter
    i=0
    while [ "$i" -lt 100 ]; do
        if tmux capture-pane -p -t "$TMUX_SESS" 2>/dev/null |
            grep -Fq 'choose a model'; then
            break
        fi
        i=$((i + 1))
        sleep 0.05
    done
    tmux capture-pane -e -p -t "$TMUX_SESS" >"$TMP/panel.ansi"
    tmux capture-pane -p -t "$TMUX_SESS" | grep -Fq '┌── choose a model' \
        || fail "picker corners are not square"
    ok "picker uses grid corners"
    python3 tests/python/panel_attr.py "$TMP/panel.ansi" 'empty filter' \
        || fail "picker background (empty filter)"
    tmux send-keys -l -t "$TMUX_SESS" 'mock'
    i=0
    while [ "$i" -lt 100 ]; do
        if tmux capture-pane -p -t "$TMUX_SESS" 2>/dev/null |
            grep -Fq 'filter mock'; then
            break
        fi
        i=$((i + 1))
        sleep 0.05
    done
    tmux capture-pane -e -p -t "$TMUX_SESS" >"$TMP/panel.ansi"
    python3 tests/python/panel_attr.py "$TMP/panel.ansi" 'typed filter' \
        || fail "picker background (typed filter)"
    tmux send-keys -t "$TMUX_SESS" BSpace
    i=0
    while [ "$i" -lt 100 ]; do
        if tmux capture-pane -p -t "$TMUX_SESS" 2>/dev/null |
            grep -Fq 'filter moc'; then
            break
        fi
        i=$((i + 1))
        sleep 0.05
    done
    tmux capture-pane -p -t "$TMUX_SESS" | grep -Fq 'filter moc' \
        || fail "picker filter ignores backspace"
    ok "picker filter backspace"
    tmux send-keys -l -t "$TMUX_SESS" 'zzz'
    i=0
    while [ "$i" -lt 100 ]; do
        if tmux capture-pane -p -t "$TMUX_SESS" 2>/dev/null |
            grep -Fq 'no matches'; then
            break
        fi
        i=$((i + 1))
        sleep 0.05
    done
    tmux capture-pane -e -p -t "$TMUX_SESS" >"$TMP/panel.ansi"
    python3 tests/python/panel_attr.py "$TMP/panel.ansi" 'no matches' \
        || fail "picker background (no matches)"
    tmux send-keys -t "$TMUX_SESS" Escape
    ok "picker panel has a solid background"

    tmux send-keys -l -t "$TMUX_SESS" '/help'
    tmux send-keys -t "$TMUX_SESS" Enter
    i=0
    while [ "$i" -lt 100 ]; do
        if tmux capture-pane -p -t "$TMUX_SESS" 2>/dev/null |
            grep -Fq 'Meong · help'; then
            break
        fi
        i=$((i + 1))
        sleep 0.05
    done
    tmux capture-pane -e -p -t "$TMUX_SESS" >"$TMP/panel.ansi"
    python3 tests/python/panel_attr.py "$TMP/panel.ansi" 'help panel' \
        || fail "help panel background"
    ok "help panel has a solid background"

    tmux send-keys -t "$TMUX_SESS" q
    tmux send-keys -t "$TMUX_SESS" C-u
    tmux send-keys -l -t "$TMUX_SESS" '/model mock/slow'
    tmux send-keys -t "$TMUX_SESS" Enter
    i=0
    while [ "$i" -lt 100 ]; do
        if tmux capture-pane -p -t "$TMUX_SESS" 2>/dev/null |
            grep -Fq 'model mock/slow'; then
            break
        fi
        i=$((i + 1))
        sleep 0.05
    done
    tmux send-keys -l -t "$TMUX_SESS" 'go'
    tmux send-keys -t "$TMUX_SESS" Enter
    i=0
    while [ "$i" -lt 100 ]; do
        if tmux capture-pane -p -t "$TMUX_SESS" 2>/dev/null |
            grep -Fq 'generating'; then
            break
        fi
        i=$((i + 1))
        sleep 0.05
    done
    i=0
    while [ "$i" -lt 100 ]; do
        if tmux capture-pane -p -t "$TMUX_SESS" 2>/dev/null |
            grep -Fq '◑ go'; then
            break
        fi
        i=$((i + 1))
        sleep 0.05
    done
    tmux capture-pane -p -t "$TMUX_SESS" | grep -Fq '◑ go' \
        || fail "user role icon is not inline with the prompt"
    i=0
    while [ "$i" -lt 100 ]; do
        if tmux capture-pane -p -t "$TMUX_SESS" 2>/dev/null |
            grep -Fq '✦ slow'; then
            break
        fi
        i=$((i + 1))
        sleep 0.05
    done
    tmux capture-pane -p -t "$TMUX_SESS" | grep -Fq '✦ slow' \
        || fail "assistant role icon is not inline with the reply"
    ok "chat uses inline role icons without labels"
    tmux send-keys -l -t "$TMUX_SESS" 'sec'
    i=0
    while [ "$i" -lt 100 ]; do
        if tmux capture-pane -p -t "$TMUX_SESS" 2>/dev/null |
            grep -Fq '❯ sec'; then
            break
        fi
        i=$((i + 1))
        sleep 0.05
    done
    tmux capture-pane -p -t "$TMUX_SESS" | grep -Fq '❯ sec' \
        || fail "composer rejected typing while streaming"
    tmux capture-pane -p -t "$TMUX_SESS" | grep -Fq 'generating' \
        || fail "stream ended before the queued prompt was entered"
    tmux send-keys -l -t "$TMUX_SESS" 'ond'
    tmux send-keys -t "$TMUX_SESS" Enter
    i=0
    while [ "$i" -lt 100 ]; do
        if tmux capture-pane -p -t "$TMUX_SESS" 2>/dev/null |
            grep -Fq 'queue ↓'; then
            break
        fi
        i=$((i + 1))
        sleep 0.05
    done
    pane=$(tmux capture-pane -p -t "$TMUX_SESS")
    gen_line=$(printf '%s\n' "$pane" | grep -n 'generating' | head -1 |
        cut -d: -f1)
    label_line=$(printf '%s\n' "$pane" | grep -n 'queue ↓' | head -1 |
        cut -d: -f1)
    prompt_line=$(printf '%s\n' "$pane" | grep -n '^    second' | head -1 |
        cut -d: -f1)
    box_line=$(printf '%s\n' "$pane" | grep -n '^ ┌' | tail -1 | cut -d: -f1)
    [ -n "$gen_line" ] && [ -n "$label_line" ] && [ -n "$prompt_line" ] &&
        [ -n "$box_line" ] || fail "streaming status lines are missing"
    [ "$gen_line" -lt "$box_line" ] \
        || fail "generating status is not above the composer"
    [ "$label_line" -eq $((gen_line + 1)) ] \
        || fail "queue label is not directly under generating"
    [ "$prompt_line" -eq $((gen_line + 2)) ] \
        || fail "queued prompt is not directly under the queue label"
    [ "$prompt_line" -lt "$box_line" ] \
        || fail "queued prompt is not above the composer"
    ok "composer stays editable, queue sits above the box"
    tmux send-keys -t "$TMUX_SESS" C-c
    tmux kill-session -t "$TMUX_SESS" 2>/dev/null || true
    TMUX_SESS=""
else
    printf 'skip - tmux not found, screen check for the input gap skipped\n'
fi

printf '\nall %d checks passed\n' "$CHECKS"
