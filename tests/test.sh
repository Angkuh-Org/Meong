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
    tests/c/json_skip_test.c src/json/json.c src/util/util.c \
    || fail "json skip test did not compile"
"$TMP/json_skip_test" >/dev/null \
    || fail "json skip table diverged from the recursive walk"
ok "json: cached skip matches the recursive walk"

$HOSTCC -std=c11 -O2 -D_DEFAULT_SOURCE -D_GNU_SOURCE -Isrc \
    -o "$TMP/jdelta_test" tests/c/jdelta_test.c src/json/json.c \
    src/util/util.c \
    || fail "jdelta test did not compile"
"$TMP/jdelta_test" >/dev/null \
    || fail "fast delta scanner diverged from jsmn"
ok "json: fast delta scanner matches jsmn"

LAYOUT_SRCS=$(find src -name '*.c' ! -path 'src/rules/*' \
    ! -path 'src/main.c' ! -path 'src/ui/view.c' | sort)
# shellcheck disable=SC2086
$HOSTCC -std=c11 -O2 -D_DEFAULT_SOURCE -D_GNU_SOURCE -Isrc -Isrc/ui \
    -o "$TMP/layout_truncate_test" \
    tests/c/layout_truncate_test.c $LAYOUT_SRCS $NC_LIBS \
    || fail "layout test did not compile"
"$TMP/layout_truncate_test" >/dev/null \
    || fail "layout truncation left stale rows"
ok "layout: /retry truncation drops removed rows"

# shellcheck disable=SC2086
$HOSTCC -std=c11 -O2 -D_DEFAULT_SOURCE -D_GNU_SOURCE -Isrc -Isrc/ui \
    -o "$TMP/layout_stream_test" \
    tests/c/layout_stream_test.c $LAYOUT_SRCS $NC_LIBS \
    || fail "layout stream test did not compile"
"$TMP/layout_stream_test" >/dev/null \
    || fail "incremental layout diverged from a full rebuild"
ok "layout: streaming cursor matches a full rebuild"

# shellcheck disable=SC2086
$HOSTCC -std=c11 -O2 -D_DEFAULT_SOURCE -D_GNU_SOURCE -Isrc -Isrc/ui \
    -o "$TMP/layout_bench" tests/c/layout_bench.c $LAYOUT_SRCS $NC_LIBS \
    || fail "layout benchmark did not compile"
"$TMP/layout_bench" stream 4096 64 >/dev/null \
    || fail "layout benchmark failed"
ok "layout: benchmark harness builds and runs"

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

    TMUX_SESS="Meong-mouse-$$"
    tmux kill-session -t "$TMUX_SESS" 2>/dev/null || true
    tmux new-session -d -s "$TMUX_SESS" -x 110 -y 40 \
        "env TERM=xterm-256color LC_ALL=C.UTF-8 LANG=C.UTF-8 \
         MEONG_CONFIG_DIR='$TMP/config' XDG_CACHE_HOME='$TMP/cache' \
         '$BIN_ABS' --url '$URL' --model mock/split"
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
    i=0
    while [ "$i" -lt 100 ]; do
        if tmux capture-pane -p -t "$TMUX_SESS" 2>/dev/null |
            grep -Fq 'ctx 42'; then
            break
        fi
        i=$((i + 1))
        sleep 0.05
    done
    sleep 0.2

    click_at() {
        tmux send-keys -l -t "$TMUX_SESS" \
            "$(printf '\033[<0;8;%dM' "$1")"
        tmux send-keys -l -t "$TMUX_SESS" \
            "$(printf '\033[<0;8;%dm' "$1")"
    }

    tmux capture-pane -p -t "$TMUX_SESS" |
        grep -Fq '○ thinking ▸' \
        || fail "thinking block did not fold after the reply finished"
    tmux send-keys -t "$TMUX_SESS" C-o
    i=0
    while [ "$i" -lt 100 ]; do
        if tmux capture-pane -p -t "$TMUX_SESS" 2>/dev/null |
            grep -Fq 'thinking a little'; then
            break
        fi
        i=$((i + 1))
        sleep 0.05
    done
    pane=$(tmux capture-pane -p -t "$TMUX_SESS")
    printf '%s\n' "$pane" | grep -Fq '○ thinking ▾' \
        || fail "ctrl-o did not expand the thinking block"
    printf '%s\n' "$pane" | grep -Fq '┆ thinking a little' \
        || fail "reasoning gutter disappeared after expanding"

    reason_row=$(printf '%s\n' "$pane" | grep -n 'thinking a little' |
        head -1 | cut -d: -f1)
    click_at "$reason_row"
    i=0
    while [ "$i" -lt 100 ]; do
        if ! tmux capture-pane -p -t "$TMUX_SESS" 2>/dev/null |
            grep -Fq 'thinking a little'; then
            break
        fi
        i=$((i + 1))
        sleep 0.05
    done
    if tmux capture-pane -p -t "$TMUX_SESS" |
        grep -Fq 'thinking a little'; then
        fail "reasoning text stayed visible after folding"
    fi
    tmux capture-pane -p -t "$TMUX_SESS" | grep -Fq '○ thinking ▸' \
        || fail "clicking a reasoning line did not fold the block"

    head_row=$(tmux capture-pane -p -t "$TMUX_SESS" |
        grep -n '○ thinking' | head -1 | cut -d: -f1)
    click_at "$head_row"
    i=0
    while [ "$i" -lt 100 ]; do
        if tmux capture-pane -p -t "$TMUX_SESS" 2>/dev/null |
            grep -Fq 'thinking a little'; then
            break
        fi
        i=$((i + 1))
        sleep 0.05
    done
    answer_row=$(tmux capture-pane -p -t "$TMUX_SESS" |
        grep -n '✦ Hello' | head -1 | cut -d: -f1)
    [ -n "$answer_row" ] || fail "answer row is missing"
    click_at "$answer_row"
    sleep 0.2
    tmux capture-pane -p -t "$TMUX_SESS" |
        grep -Fq 'thinking a little' \
        || fail "clicking the answer folded the reasoning block"

    tmux send-keys -t "$TMUX_SESS" C-o
    i=0
    while [ "$i" -lt 100 ]; do
        if ! tmux capture-pane -p -t "$TMUX_SESS" 2>/dev/null |
            grep -Fq 'thinking a little'; then
            break
        fi
        i=$((i + 1))
        sleep 0.05
    done
    tmux capture-pane -p -t "$TMUX_SESS" | grep -Fq '○ thinking ▸' \
        || fail "ctrl-o did not fold the thinking block"
    ok "mouse click and ctrl-o fold and unfold reasoning"

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
    before=$(tmux capture-pane -p -t "$TMUX_SESS" |
        grep -F 'esc' | head -1)
    tmux send-keys -l -t "$TMUX_SESS" "$(printf '\033[<64;20;12M')"
    sleep 0.2
    after=$(tmux capture-pane -p -t "$TMUX_SESS" |
        grep -F 'esc' | head -1)
    [ -n "$before" ] && [ "$before" != "$after" ] \
        || fail "mouse wheel did not scroll the picker"
    ok "mouse wheel still scrolls the picker"
    tmux kill-session -t "$TMUX_SESS" 2>/dev/null || true
    TMUX_SESS=""

    TMUX_SESS="Meong-reasononly-$$"
    tmux kill-session -t "$TMUX_SESS" 2>/dev/null || true
    tmux new-session -d -s "$TMUX_SESS" -x 110 -y 40 \
        "env TERM=xterm-256color LC_ALL=C.UTF-8 LANG=C.UTF-8 \
         MEONG_CONFIG_DIR='$TMP/config' XDG_CACHE_HOME='$TMP/cache' \
         '$BIN_ABS' --url '$URL' --model mock/reasononly --no-mouse"
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
    i=0
    while [ "$i" -lt 100 ]; do
        if tmux capture-pane -p -t "$TMUX_SESS" 2>/dev/null |
            grep -Fq 'tok ·'; then
            break
        fi
        i=$((i + 1))
        sleep 0.05
    done
    sleep 0.2
    pane=$(tmux capture-pane -p -t "$TMUX_SESS")
    printf '%s\n' "$pane" | grep -Fq '○ thinking ▾' \
        || fail "reasoning-only reply folded its only content"
    printf '%s\n' "$pane" | grep -Fq '┆ thinking a little' \
        || fail "reasoning-only reply lost its reasoning text"
    printf '%s\n' "$pane" | grep -Fq 'with no answer' \
        || fail "reasoning-only reply lost its second chunk"
    ok "reasoning-only reply stays unfolded"
    tmux kill-session -t "$TMUX_SESS" 2>/dev/null || true
    TMUX_SESS=""

    TMUX_SESS="Meong-slash-$$"
    tmux kill-session -t "$TMUX_SESS" 2>/dev/null || true
    tmux new-session -d -s "$TMUX_SESS" -x 110 -y 40 \
        "env TERM=xterm-256color LC_ALL=C.UTF-8 LANG=C.UTF-8 \
         MEONG_CONFIG_DIR='$TMP/config' XDG_CACHE_HOME='$TMP/cache' \
         '$BIN_ABS' --url '$URL' --model mock/plain --no-mouse"
    i=0
    while [ "$i" -lt 100 ]; do
        if tmux capture-pane -p -t "$TMUX_SESS" 2>/dev/null |
            grep -Fq 'ask anything'; then
            break
        fi
        i=$((i + 1))
        sleep 0.05
    done
    tmux send-keys -l -t "$TMUX_SESS" '/'
    i=0
    while [ "$i" -lt 100 ]; do
        if tmux capture-pane -p -t "$TMUX_SESS" 2>/dev/null |
            grep -Fq 'pick a model, or switch directly'; then
            break
        fi
        i=$((i + 1))
        sleep 0.05
    done
    pane=$(tmux capture-pane -p -t "$TMUX_SESS")
    for cmd in /model /models /system /clear /retry /help /quit; do
        printf '%s\n' "$pane" | grep -Fq "$cmd" \
            || fail "slash menu is missing $cmd"
    done
    menu_line=$(printf '%s\n' "$pane" |
        grep -n 'pick a model, or switch directly' | head -1 | cut -d: -f1)
    box_line=$(printf '%s\n' "$pane" | grep -n '^ ┌' | tail -1 | cut -d: -f1)
    [ -n "$menu_line" ] && [ -n "$box_line" ] &&
        [ "$menu_line" -lt "$box_line" ] \
        || fail "slash menu is not above the composer"
    ok "slash menu lists commands above the composer"

    tmux send-keys -t "$TMUX_SESS" Down
    tmux send-keys -t "$TMUX_SESS" Down
    i=0
    while [ "$i" -lt 100 ]; do
        if tmux capture-pane -p -t "$TMUX_SESS" 2>/dev/null |
            grep -Fq '› /system'; then
            break
        fi
        i=$((i + 1))
        sleep 0.05
    done
    tmux capture-pane -p -t "$TMUX_SESS" | grep -Fq '› /system' \
        || fail "down did not focus /system"
    tmux send-keys -t "$TMUX_SESS" Enter
    i=0
    while [ "$i" -lt 100 ]; do
        if tmux capture-pane -p -t "$TMUX_SESS" 2>/dev/null |
            grep -Fq '❯ /system'; then
            break
        fi
        i=$((i + 1))
        sleep 0.05
    done
    pane=$(tmux capture-pane -p -t "$TMUX_SESS")
    printf '%s\n' "$pane" | grep -Fq '❯ /system' \
        || fail "enter did not complete /system into the composer"
    if printf '%s\n' "$pane" | grep -Fq 'pick a model, or switch directly'; then
        fail "slash menu stayed open after completion"
    fi
    tmux send-keys -l -t "$TMUX_SESS" 'off'
    tmux send-keys -t "$TMUX_SESS" Enter
    i=0
    while [ "$i" -lt 100 ]; do
        if tmux capture-pane -p -t "$TMUX_SESS" 2>/dev/null |
            grep -Fq 'system prompt cleared'; then
            break
        fi
        i=$((i + 1))
        sleep 0.05
    done
    tmux capture-pane -p -t "$TMUX_SESS" | grep -Fq 'system prompt cleared' \
        || fail "completed /system did not accept its argument"
    ok "slash menu focus and enter completion"

    tmux send-keys -l -t "$TMUX_SESS" '/cl'
    i=0
    while [ "$i" -lt 100 ]; do
        if tmux capture-pane -p -t "$TMUX_SESS" 2>/dev/null |
            grep -Fq '/clear       forget the conversation'; then
            break
        fi
        i=$((i + 1))
        sleep 0.05
    done
    tmux capture-pane -p -t "$TMUX_SESS" |
        grep -Fq '/clear       forget the conversation' \
        || fail "typing /cl did not filter to /clear"
    tmux send-keys -t "$TMUX_SESS" Enter
    i=0
    while [ "$i" -lt 100 ]; do
        if tmux capture-pane -p -t "$TMUX_SESS" 2>/dev/null |
            grep -Fq '❯ /clear'; then
            break
        fi
        i=$((i + 1))
        sleep 0.05
    done
    tmux capture-pane -p -t "$TMUX_SESS" | grep -Fq '❯ /clear' \
        || fail "enter did not complete /cl to /clear"
    tmux send-keys -t "$TMUX_SESS" Enter
    i=0
    while [ "$i" -lt 100 ]; do
        if tmux capture-pane -p -t "$TMUX_SESS" 2>/dev/null |
            grep -Fq 'talk to anything behind PAIG'; then
            break
        fi
        i=$((i + 1))
        sleep 0.05
    done
    tmux capture-pane -p -t "$TMUX_SESS" |
        grep -Fq 'talk to anything behind PAIG' \
        || fail "completed /clear did not run"
    ok "slash filter narrows and enter completes"

    tmux send-keys -l -t "$TMUX_SESS" '/'
    sleep 0.3
    tmux send-keys -t "$TMUX_SESS" Escape
    sleep 0.3
    if tmux capture-pane -p -t "$TMUX_SESS" |
        grep -Fq 'pick a model, or switch directly'; then
        fail "escape did not close the slash menu"
    fi
    tmux send-keys -t "$TMUX_SESS" C-u
    tmux send-keys -l -t "$TMUX_SESS" '/he'
    sleep 0.3
    tmux send-keys -t "$TMUX_SESS" Tab
    i=0
    while [ "$i" -lt 100 ]; do
        if tmux capture-pane -p -t "$TMUX_SESS" 2>/dev/null |
            grep -Fq '❯ /help'; then
            break
        fi
        i=$((i + 1))
        sleep 0.05
    done
    tmux capture-pane -p -t "$TMUX_SESS" | grep -Fq '❯ /help' \
        || fail "tab did not complete /he to /help"
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
    tmux capture-pane -p -t "$TMUX_SESS" | grep -Fq 'Meong · help' \
        || fail "tab completion did not run /help"
    tmux send-keys -t "$TMUX_SESS" q
    tmux send-keys -l -t "$TMUX_SESS" '/help'
    sleep 0.3
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
    tmux capture-pane -p -t "$TMUX_SESS" | grep -Fq 'Meong · help' \
        || fail "an exact command plus enter no longer runs"
    ok "slash menu escape, tab, and exact-command enter"

    tmux send-keys -t "$TMUX_SESS" q
    tmux send-keys -l -t "$TMUX_SESS" '/quit'
    tmux send-keys -t "$TMUX_SESS" Enter
    i=0
    while [ "$i" -lt 100 ]; do
        if ! tmux has-session -t "$TMUX_SESS" 2>/dev/null; then
            break
        fi
        i=$((i + 1))
        sleep 0.05
    done
    tmux has-session -t "$TMUX_SESS" 2>/dev/null \
        && fail "slash /quit did not exit Meong"
    ok "slash menu /quit exits"
    TMUX_SESS=""

    TMUX_SESS="Meong-slash-cancel-$$"
    tmux kill-session -t "$TMUX_SESS" 2>/dev/null || true
    tmux new-session -d -s "$TMUX_SESS" -x 110 -y 40 \
        "env TERM=xterm-256color LC_ALL=C.UTF-8 LANG=C.UTF-8 \
         MEONG_CONFIG_DIR='$TMP/config' XDG_CACHE_HOME='$TMP/cache' \
         '$BIN_ABS' --url '$URL' --model mock/slow --no-mouse"
    i=0
    while [ "$i" -lt 100 ]; do
        if tmux capture-pane -p -t "$TMUX_SESS" 2>/dev/null |
            grep -Fq 'ask anything'; then
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
    tmux send-keys -l -t "$TMUX_SESS" '/'
    i=0
    while [ "$i" -lt 100 ]; do
        if tmux capture-pane -p -t "$TMUX_SESS" 2>/dev/null |
            grep -Fq 'pick a model, or switch directly'; then
            break
        fi
        i=$((i + 1))
        sleep 0.05
    done
    tmux send-keys -t "$TMUX_SESS" C-c
    i=0
    while [ "$i" -lt 100 ]; do
        if tmux capture-pane -p -t "$TMUX_SESS" 2>/dev/null |
            grep -Fq 'stopped'; then
            break
        fi
        i=$((i + 1))
        sleep 0.05
    done
    tmux capture-pane -p -t "$TMUX_SESS" | grep -Fq 'stopped' \
        || fail "ctrl-c was swallowed by the slash menu while streaming"
    ok "ctrl-c still cancels while the slash menu is open"
    tmux send-keys -t "$TMUX_SESS" C-u
    tmux send-keys -l -t "$TMUX_SESS" '/quit'
    tmux send-keys -t "$TMUX_SESS" Enter
    i=0
    while [ "$i" -lt 100 ]; do
        if ! tmux has-session -t "$TMUX_SESS" 2>/dev/null; then
            break
        fi
        i=$((i + 1))
        sleep 0.05
    done
    tmux has-session -t "$TMUX_SESS" 2>/dev/null \
        && fail "cancel slash session did not exit"
    TMUX_SESS=""

    TMUX_SESS="Meong-slash-short-$$"
    tmux kill-session -t "$TMUX_SESS" 2>/dev/null || true
    tmux new-session -d -s "$TMUX_SESS" -x 40 -y 12 \
        "env TERM=xterm-256color LC_ALL=C.UTF-8 LANG=C.UTF-8 \
         MEONG_CONFIG_DIR='$TMP/config' XDG_CACHE_HOME='$TMP/cache' \
         '$BIN_ABS' --url '$URL' --model mock/plain --no-mouse"
    i=0
    while [ "$i" -lt 100 ]; do
        if tmux capture-pane -p -t "$TMUX_SESS" 2>/dev/null |
            grep -Fq 'ask anything'; then
            break
        fi
        i=$((i + 1))
        sleep 0.05
    done
    tmux send-keys -l -t "$TMUX_SESS" '/'
    sleep 0.3
    if tmux capture-pane -p -t "$TMUX_SESS" |
        grep -Fq 'pick a model, or switch directly'; then
        fail "slash menu was drawn over the composer on a short terminal"
    fi
    tmux send-keys -t "$TMUX_SESS" Enter
    i=0
    while [ "$i" -lt 100 ]; do
        if tmux capture-pane -p -t "$TMUX_SESS" 2>/dev/null |
            grep -Fq 'unknown command'; then
            break
        fi
        i=$((i + 1))
        sleep 0.05
    done
    pane=$(tmux capture-pane -p -t "$TMUX_SESS")
    printf '%s\n' "$pane" | grep -Fq 'unknown command' \
        || fail "enter on a hidden slash menu did not submit"
    if printf '%s\n' "$pane" | grep -Fq '❯ /model'; then
        fail "a hidden slash menu still completed the composer"
    fi
    ok "slash menu stays hidden when the terminal is too short"

    tmux resize-window -t "$TMUX_SESS" -x 110 -y 40
    tmux send-keys -t "$TMUX_SESS" C-l
    tmux send-keys -l -t "$TMUX_SESS" '/clear'
    tmux send-keys -t "$TMUX_SESS" Enter
    i=0
    while [ "$i" -lt 100 ]; do
        if tmux capture-pane -p -t "$TMUX_SESS" 2>/dev/null |
            grep -Fq 'ask anything'; then
            break
        fi
        i=$((i + 1))
        sleep 0.05
    done
    tmux send-keys -l -t "$TMUX_SESS" '/'
    sleep 0.3
    tmux resize-window -t "$TMUX_SESS" -x 34 -y 20
    i=0
    while [ "$i" -lt 100 ]; do
        if tmux capture-pane -p -t "$TMUX_SESS" 2>/dev/null |
            grep -Fq 'terminal too small'; then
            break
        fi
        i=$((i + 1))
        sleep 0.05
    done
    tmux send-keys -H -t "$TMUX_SESS" 1b 4f 4d
    sleep 0.5
    tmux resize-window -t "$TMUX_SESS" -x 110 -y 40
    tmux send-keys -t "$TMUX_SESS" C-l
    i=0
    while [ "$i" -lt 100 ]; do
        if tmux capture-pane -p -t "$TMUX_SESS" 2>/dev/null |
            grep -Fq 'unknown command'; then
            break
        fi
        i=$((i + 1))
        sleep 0.05
    done
    pane=$(tmux capture-pane -p -t "$TMUX_SESS")
    printf '%s\n' "$pane" | grep -Fq 'unknown command' \
        || fail "keypad enter below the draw minimum did not submit"
    if printf '%s\n' "$pane" | grep -Fq '❯ /model'; then
        fail "hidden slash menu completed the composer at 34 columns"
    fi
    ok "slash menu follows the draw minimum at 34 columns"

    tmux send-keys -l -t "$TMUX_SESS" '/quit'
    tmux send-keys -t "$TMUX_SESS" Enter
    i=0
    while [ "$i" -lt 100 ]; do
        if ! tmux has-session -t "$TMUX_SESS" 2>/dev/null; then
            break
        fi
        i=$((i + 1))
        sleep 0.05
    done
    tmux has-session -t "$TMUX_SESS" 2>/dev/null \
        && fail "short slash session did not exit"
    TMUX_SESS=""
else
    printf 'skip - tmux not found, screen check for the input gap skipped\n'
fi

printf '\nall %d checks passed\n' "$CHECKS"
