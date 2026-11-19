#!/bin/sh
set -eu
cd "$(dirname "$0")/.."

TMP=$(mktemp -d)
MOCK_PID=""
TMUX_SESS=""
DEEP_SESS=""
CHECKS=0
BIN=${MEONG_BIN:-./build/meong}
export MEONG_BIN="$BIN"
HOSTCC=${HOSTCC:-cc}
NC_LIBS=$(pkg-config --libs ncursesw 2>/dev/null || echo "-lncursesw")

cleanup() {
    if [ -n "$TMUX_SESS" ]; then
        tmux kill-session -t "$TMUX_SESS" 2>/dev/null || true
    fi
    if [ -n "$DEEP_SESS" ]; then
        tmux kill-session -t "$DEEP_SESS" 2>/dev/null || true
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
    -o "$TMP/utf8_cols_test" tests/c/utf8_cols_test.c src/util/util.c \
    || fail "utf8 cols test did not compile"
"$TMP/utf8_cols_test" >/dev/null \
    || fail "utf8 column helpers disagree with wcwidth"
ok "util: utf8 columns and tail stay on codepoint boundaries"

$HOSTCC -std=c11 -O2 -D_DEFAULT_SOURCE -D_GNU_SOURCE -Isrc \
    -o "$TMP/jdelta_test" tests/c/jdelta_test.c src/json/json.c \
    src/chat/tool_accum.c src/util/util.c \
    || fail "jdelta test did not compile"
"$TMP/jdelta_test" >/dev/null \
    || fail "fast delta scanner diverged from jsmn"
ok "json: fast delta scanner matches jsmn"

$HOSTCC -std=c11 -O2 -D_DEFAULT_SOURCE -D_GNU_SOURCE -Isrc \
    -o "$TMP/msg_release_test" \
    tests/c/msg_release_test.c src/app/app.c src/chat/tool_accum.c \
    src/ui/md.c src/util/util.c \
    || fail "msg_release test did not compile"
"$TMP/msg_release_test" >/dev/null \
    || fail "msg_release left stale state"
ok "app: msg_release clears the message and is idempotent"

$HOSTCC -std=c11 -O2 -D_DEFAULT_SOURCE -D_GNU_SOURCE -Isrc \
    -o "$TMP/state_test" \
    tests/c/state_test.c src/app/state.c src/app/app.c \
    src/chat/tool_accum.c src/ui/md.c src/util/util.c \
    || fail "state test did not compile"
"$TMP/state_test" >/dev/null \
    || fail "state file lost model or effort"
ok "state: model and effort round-trip without clobbering"

$HOSTCC -std=c11 -O2 -D_DEFAULT_SOURCE -D_GNU_SOURCE -Isrc \
    -o "$TMP/markdown_test" \
    tests/c/markdown_test.c src/app/app.c src/chat/tool_accum.c \
    src/ui/md.c src/util/util.c \
    || fail "markdown test did not compile"
"$TMP/markdown_test" >/dev/null \
    || fail "markdown translation diverged"
ok "markdown: streaming syntax becomes styled display text"

$HOSTCC -std=c11 -O2 -D_DEFAULT_SOURCE -D_GNU_SOURCE -Isrc \
    -o "$TMP/tool_accum_test" \
    tests/c/tool_accum_test.c src/chat/tool_accum.c src/util/util.c \
    || fail "tool accumulator test did not compile"
"$TMP/tool_accum_test" >/dev/null \
    || fail "tool accumulator merged or lost calls"
ok "tools: accumulator handles whole and fragmented calls"

$HOSTCC -std=c11 -O2 -D_DEFAULT_SOURCE -D_GNU_SOURCE -Isrc \
    -o "$TMP/tools_test" \
    tests/c/tools_test.c src/tools/tools.c src/tools/exec.c \
    src/json/json.c src/util/util.c \
    || fail "tools test did not compile"
"$TMP/tools_test" >/dev/null \
    || fail "tool handlers failed"
ok "tools: read_file, search, list_dir and result bounding"

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

# shellcheck disable=SC2086
$HOSTCC -std=c11 -O2 -D_DEFAULT_SOURCE -D_GNU_SOURCE -Isrc -Isrc/ui \
    -o "$TMP/header_mid_test" \
    tests/c/header_mid_test.c $LAYOUT_SRCS $NC_LIBS \
    || fail "header title test did not compile"
"$TMP/header_mid_test" >/dev/null \
    || fail "header title clamp cut by bytes or past the buffer"
ok "header: over-long chat title keeps a UTF-8 prefix and width"

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

out=$("$BIN" --url "$URL" --model mock/effort --oneshot 'hi') \
    || fail "effort baseline failed"
[ "$out" = "effort=none" ] || fail "unset effort leaked: $out"

out=$("$BIN" --url "$URL" --model mock/effort --effort high --oneshot 'hi') \
    || fail "effort flag failed"
[ "$out" = "effort=high" ] || fail "effort flag wire: $out"

out=$(MEONG_EFFORT=low "$BIN" --url "$URL" --model mock/effort \
    --oneshot 'hi') || fail "effort env failed"
[ "$out" = "effort=low" ] || fail "effort env wire: $out"

out=$(MEONG_EFFORT=medium "$BIN" --url "$URL" --model mock/effort \
    --effort default --oneshot 'hi') || fail "effort default failed"
[ "$out" = "effort=none" ] || fail "explicit default leaked: $out"

out=$("$BIN" --url "$URL" --model mock/effort --effort xhigh --stream \
    --oneshot 'hi') || fail "stream effort failed"
[ "$out" = "effort=xhigh" ] || fail "stream effort wire: $out"

if "$BIN" --effort turbo --oneshot hi >/dev/null 2>"$TMP/err"; then
    fail "invalid effort returned success"
fi
grep -Fq -e '--effort must be' "$TMP/err" \
    || fail "invalid effort message missing"
ok "thinking effort: flag, env, default, stream and validation"

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
    tmux new-session -d -s "$TMUX_SESS" -x 110 -y 40 -c "$PWD" \
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
    for needle in 'Hello, Meong' '🌊🐟' 'int x = 1;' 'ctx <1%'; do
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
    tmux capture-pane -p -t "$TMUX_SESS" | grep -Fq 'enter send | ? help' \
        || fail "idle footer lost the help hint"
    if tmux capture-pane -p -t "$TMUX_SESS" | grep -Fq 'tab models'; then
        fail "idle footer still advertises the old tab hint"
    fi
    ok "footer uses pipes and the ? help hint"
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
            grep -Fq 'Meong | help'; then
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
            grep -Eq '^  . [0-9]+\.[0-9]+s +[^ ]+ ch'; then
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
    tmux capture-pane -p -t "$TMUX_SESS" |
        grep -Eq '^  . [0-9]+\.[0-9]+s +[^ ]+ ch' \
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
    gen_line=$(printf '%s\n' "$pane" | grep -nE ' ch$' | head -1 |
        cut -d: -f1)
    label_line=$(printf '%s\n' "$pane" | grep -n 'queue ↓' | head -1 |
        cut -d: -f1)
    prompt_line=$(printf '%s\n' "$pane" | grep -n '^    second' | head -1 |
        cut -d: -f1)
    box_line=$(printf '%s\n' "$pane" | grep -n '^ ┌' | tail -1 | cut -d: -f1)
    [ -n "$gen_line" ] && [ -n "$label_line" ] && [ -n "$prompt_line" ] &&
        [ -n "$box_line" ] || fail "streaming status lines are missing"
    [ "$gen_line" -lt "$box_line" ] \
        || fail "status line is not above the composer"
    [ "$label_line" -eq $((gen_line + 1)) ] \
        || fail "queue label is not directly under the status line"
    [ "$prompt_line" -eq $((gen_line + 2)) ] \
        || fail "queued prompt is not directly under the queue label"
    [ "$prompt_line" -lt "$box_line" ] \
        || fail "queued prompt is not above the composer"
    ok "composer stays editable, queue sits above the box"
    tmux send-keys -t "$TMUX_SESS" C-c
    tmux kill-session -t "$TMUX_SESS" 2>/dev/null || true
    TMUX_SESS=""

    TMUX_SESS="Meong-effort-$$"
    tmux kill-session -t "$TMUX_SESS" 2>/dev/null || true
    tmux new-session -d -s "$TMUX_SESS" -x 110 -y 40 -c "$PWD" \
        "env TERM=xterm-256color LC_ALL=C.UTF-8 LANG=C.UTF-8 \
         MEONG_CONFIG_DIR='$TMP/config-effort' \
         XDG_CACHE_HOME='$TMP/cache-effort' \
         '$BIN_ABS' --url '$URL' --model mock/plain"
    i=0
    while [ "$i" -lt 100 ]; do
        if tmux capture-pane -p -t "$TMUX_SESS" 2>/dev/null |
            grep -Fq 'ask anything'; then
            break
        fi
        i=$((i + 1))
        sleep 0.05
    done
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
    tmux send-keys -l -t "$TMUX_SESS" 'effort'
    i=0
    while [ "$i" -lt 100 ]; do
        if tmux capture-pane -p -t "$TMUX_SESS" 2>/dev/null |
            grep -Fq 'filter effort'; then
            break
        fi
        i=$((i + 1))
        sleep 0.05
    done
    tmux send-keys -t "$TMUX_SESS" Enter
    i=0
    while [ "$i" -lt 100 ]; do
        if tmux capture-pane -p -t "$TMUX_SESS" 2>/dev/null |
            grep -Fq 'choose thinking effort'; then
            break
        fi
        i=$((i + 1))
        sleep 0.05
    done
    tmux capture-pane -p -t "$TMUX_SESS" |
        grep -Fq 'choose thinking effort' \
        || fail "picking a model did not open the effort picker"
    tmux capture-pane -p -t "$TMUX_SESS" | grep -Fq 'default' \
        || fail "effort picker lost the default row"
    tmux send-keys -t "$TMUX_SESS" Down Down Down Down
    tmux send-keys -t "$TMUX_SESS" Enter
    i=0
    while [ "$i" -lt 100 ]; do
        if tmux capture-pane -p -t "$TMUX_SESS" 2>/dev/null |
            grep -Fq 'thinking effort high'; then
            break
        fi
        i=$((i + 1))
        sleep 0.05
    done
    tmux capture-pane -p -t "$TMUX_SESS" | grep -Fq 'thinking effort high' \
        || fail "effort picker did not apply high"
    tmux send-keys -l -t "$TMUX_SESS" 'hello'
    tmux send-keys -t "$TMUX_SESS" Enter
    i=0
    while [ "$i" -lt 100 ]; do
        if tmux capture-pane -p -t "$TMUX_SESS" 2>/dev/null |
            grep -Fq 'effort=high'; then
            break
        fi
        i=$((i + 1))
        sleep 0.05
    done
    tmux capture-pane -p -t "$TMUX_SESS" | grep -Fq 'effort=high' \
        || fail "TUI request omitted reasoning_effort"
    tmux send-keys -l -t "$TMUX_SESS" '/effort'
    tmux send-keys -t "$TMUX_SESS" Enter
    i=0
    while [ "$i" -lt 100 ]; do
        if tmux capture-pane -p -t "$TMUX_SESS" 2>/dev/null |
            grep -Fq 'choose thinking effort'; then
            break
        fi
        i=$((i + 1))
        sleep 0.05
    done
    tmux send-keys -t "$TMUX_SESS" Escape
    i=0
    while [ "$i" -lt 100 ]; do
        if ! tmux capture-pane -p -t "$TMUX_SESS" 2>/dev/null |
            grep -Fq 'choose thinking effort'; then
            break
        fi
        i=$((i + 1))
        sleep 0.05
    done
    if tmux capture-pane -p -t "$TMUX_SESS" |
        grep -Fq 'choose thinking effort'; then
        fail "escape did not close the effort picker"
    fi
    tmux send-keys -l -t "$TMUX_SESS" '/effort medium'
    tmux send-keys -t "$TMUX_SESS" Enter
    i=0
    while [ "$i" -lt 100 ]; do
        if tmux capture-pane -p -t "$TMUX_SESS" 2>/dev/null |
            grep -Fq 'thinking effort medium'; then
            break
        fi
        i=$((i + 1))
        sleep 0.05
    done
    tmux capture-pane -p -t "$TMUX_SESS" | grep -Fq 'thinking effort medium' \
        || fail "/effort level did not apply directly"
    tmux send-keys -t "$TMUX_SESS" F2
    i=0
    while [ "$i" -lt 100 ]; do
        if tmux capture-pane -p -t "$TMUX_SESS" 2>/dev/null |
            grep -Eq '│  effort +medium'; then
            break
        fi
        i=$((i + 1))
        sleep 0.05
    done
    pane=$(tmux capture-pane -p -t "$TMUX_SESS")
    printf '%s\n' "$pane" | grep -Eq '│  effort +medium' \
        || fail "model card does not show the thinking effort"
    printf '%s\n' "$pane" | grep -Fq 'reasoning' \
        || fail "model card lost its capability rows"
    ok "effort: model picker chains into the effort picker"
    tmux send-keys -t "$TMUX_SESS" Escape
    sleep 0.2
    tmux capture-pane -e -p -t "$TMUX_SESS" >"$TMP/footer.ansi"
    python3 - "$TMP/footer.ansi" <<'PY' || fail "footer effort label"
import sys
sys.path.insert(0, "tests/python")
from panel_attr import cells

rows = cells(open(sys.argv[1], encoding="utf-8").read())
line = next(row for row in reversed(rows) if row)
text = "".join(ch for ch, _, _ in line)
mi = text.find("mock/effort")
ei = text.find("medium")
if mi < 0 or ei < 0:
    sys.exit(f"footer text: {text!r}")
if ei <= mi:
    sys.exit("footer effort is not after the model")
if line[mi][1] != 154:
    sys.exit(f"footer model fg={line[mi][1]}, want 154")
if line[ei][1] != 208:
    sys.exit(f"footer effort fg={line[ei][1]}, want 208")
print(f"footer: model fg=154, effort fg=208, text={text!r}")
PY
    ok "footer shows the effort in orange next to the model"
    footer=$(tmux capture-pane -p -t "$TMUX_SESS" | tail -1)
    ex=$(printf '%s\n' "$footer" | awk '{print index($0,"medium")}')
    [ "$ex" -gt 0 ] || fail "footer effort label is missing"
    rows=$(tmux display-message -p -t "$TMUX_SESS" '#{pane_height}')
    tmux send-keys -l -t "$TMUX_SESS" \
        "$(printf '\033[<0;%d;%dM' "$ex" "$rows")"
    tmux send-keys -l -t "$TMUX_SESS" \
        "$(printf '\033[<0;%d;%dm' "$ex" "$rows")"
    i=0
    while [ "$i" -lt 100 ]; do
        if tmux capture-pane -p -t "$TMUX_SESS" 2>/dev/null |
            grep -Fq 'choose thinking effort'; then
            break
        fi
        i=$((i + 1))
        sleep 0.05
    done
    pane=$(tmux capture-pane -p -t "$TMUX_SESS")
    printf '%s\n' "$pane" | grep -Fq 'choose thinking effort' \
        || fail "clicking the footer effort did not open the picker"
    printf '%s\n' "$pane" | grep -Eq '› medium' \
        || fail "clicking the footer effort lost the current level"
    tmux send-keys -t "$TMUX_SESS" Escape
    ok "clicking the footer effort opens the effort picker"
    tmux kill-session -t "$TMUX_SESS" 2>/dev/null || true
    TMUX_SESS=""

    TMUX_SESS="Meong-cache-$$"
    tmux kill-session -t "$TMUX_SESS" 2>/dev/null || true
    tmux new-session -d -s "$TMUX_SESS" -x 110 -y 40 \
        "env TERM=xterm-256color LC_ALL=C.UTF-8 LANG=C.UTF-8 \
         MEONG_CONFIG_DIR='$TMP/config' XDG_CACHE_HOME='$TMP/cache' \
         '$BIN_ABS' --url '$URL' --model mock/cache --no-mouse"
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
            grep -Fq 'cache 71%'; then
            break
        fi
        i=$((i + 1))
        sleep 0.05
    done
    tmux capture-pane -p -t "$TMUX_SESS" | grep -Fq 'cache 71%' \
        || fail "footer did not show the cached-token ratio"
    ok "footer shows the cache hit ratio"
    tmux kill-session -t "$TMUX_SESS" 2>/dev/null || true
    TMUX_SESS=""

    TMUX_SESS="Meong-cache-narrow-$$"
    tmux kill-session -t "$TMUX_SESS" 2>/dev/null || true
    tmux new-session -d -s "$TMUX_SESS" -x 40 -y 20 \
        "env TERM=xterm-256color LC_ALL=C.UTF-8 LANG=C.UTF-8 \
         MEONG_CONFIG_DIR='$TMP/config' XDG_CACHE_HOME='$TMP/cache' \
         '$BIN_ABS' --url '$URL' --model mock/cache --no-mouse"
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
            tail -2 | head -1 | grep -Fq 'ctx '; then
            break
        fi
        i=$((i + 1))
        sleep 0.05
    done
    narrow=$(tmux capture-pane -p -t "$TMUX_SESS" | tail -2 | head -1)
    printf '%s\n' "$narrow" | grep -Fq 'ctx ' \
        || fail "narrow footer lost the context segment"
    if printf '%s\n' "$narrow" | grep -Fq 'cache'; then
        fail "narrow footer kept the cache segment"
    fi
    ok "narrow footer drops cache before context"
    tmux kill-session -t "$TMUX_SESS" 2>/dev/null || true
    TMUX_SESS=""

    TMUX_SESS="Meong-nocache-$$"
    tmux kill-session -t "$TMUX_SESS" 2>/dev/null || true
    tmux new-session -d -s "$TMUX_SESS" -x 110 -y 40 \
        "env TERM=xterm-256color LC_ALL=C.UTF-8 LANG=C.UTF-8 \
         MEONG_CONFIG_DIR='$TMP/config' XDG_CACHE_HOME='$TMP/cache' \
         '$BIN_ABS' --url '$URL' --model mock/nocache --no-mouse"
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
            grep -Fq 'cache -'; then
            break
        fi
        i=$((i + 1))
        sleep 0.05
    done
    tmux capture-pane -p -t "$TMUX_SESS" | grep -Fq 'cache -' \
        || fail "footer did not distinguish an absent cache field"
    ok "footer shows cache - when the gateway omits the field"
    tmux kill-session -t "$TMUX_SESS" 2>/dev/null || true
    TMUX_SESS=""

    TMUX_SESS="Meong-spin-$$"
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
            grep -Eq '^  . [0-9]+\.[0-9]+s +[^ ]+ ch'; then
            break
        fi
        i=$((i + 1))
        sleep 0.05
    done
    first_spin=""
    spin_changed=0
    i=0
    while [ "$i" -lt 10 ]; do
        line=$(tmux capture-pane -p -t "$TMUX_SESS" 2>/dev/null |
            grep -E '^  . [0-9]+\.[0-9]+s +[^ ]+ ch' | head -1)
        [ -n "$line" ] || break
        glyph=${line#  }
        glyph=${glyph%% *}
        case "$glyph" in
            ▖|▘|▝|▗) ;;
            *) fail "unexpected spinner glyph: $glyph" ;;
        esac
        layout=$(python3 -c 'import re,sys; print(1 if re.match(r"^  [▖▘▝▗] \d+\.\d+s +\S+ ch", sys.argv[1]) else 0)' "$line")
        [ "$layout" = 1 ] || fail "status line layout changed: |$line|"
        [ -n "$first_spin" ] || first_spin=$glyph
        [ "$glyph" = "$first_spin" ] || spin_changed=1
        i=$((i + 1))
        sleep 0.1
    done
    [ "$spin_changed" = 1 ] || fail "spinner glyph never changed"
    ok "status spinner rotates with a stable layout"
    header=$(tmux capture-pane -p -t "$TMUX_SESS" | head -1)
    case "$header" in
        *streaming*) ;;
        *) fail "streaming header lost its status" ;;
    esac
    width=$(printf '%s\n' "$header" | python3 -c '
import sys
sys.path.insert(0, "tests/python")
from panel_attr import screen
rows = screen(sys.stdin.read())
print(len("".join(ch for ch, _ in rows[0]).rstrip()))
')
    [ "$width" = 109 ] \
        || fail "streaming header ends at $width columns, want 109"
    ok "streaming header keeps its right edge at cols-2"
    tmux send-keys -t "$TMUX_SESS" C-c
    tmux kill-session -t "$TMUX_SESS" 2>/dev/null || true
    TMUX_SESS=""

    TMUX_SESS="Meong-md-$$"
    tmux kill-session -t "$TMUX_SESS" 2>/dev/null || true
    tmux new-session -d -s "$TMUX_SESS" -x 110 -y 60 -c "$PWD" \
        "env TERM=xterm-256color LC_ALL=C.UTF-8 LANG=C.UTF-8 \
         MEONG_CONFIG_DIR='$TMP/config-md' XDG_CACHE_HOME='$TMP/cache-md' \
         '$BIN_ABS' --url '$URL' --model mock/markdown --no-mouse"
    i=0
    while [ "$i" -lt 100 ]; do
        if tmux capture-pane -p -t "$TMUX_SESS" 2>/dev/null |
            grep -Fq 'ask anything'; then
            break
        fi
        i=$((i + 1))
        sleep 0.05
    done
    tmux send-keys -l -t "$TMUX_SESS" 'hi'
    tmux send-keys -t "$TMUX_SESS" Enter
    i=0
    while [ "$i" -lt 120 ]; do
        if tmux capture-pane -p -t "$TMUX_SESS" 2>/dev/null |
            grep -Fq '7 tok'; then
            break
        fi
        i=$((i + 1))
        sleep 0.05
    done
    pane=$(tmux capture-pane -p -t "$TMUX_SESS" 2>/dev/null || true)
    for needle in 'Head' 'This is bold, italic and code.' '• one' \
        '1. first' '☐ todo' '☑ done' 'gone text' '│ quoted' \
        '│ name │ age │' '│ meong │ 9 │' 'int x = 1;' 'Docs'; do
        printf '%s\n' "$pane" | grep -Fq "$needle" \
            || fail "markdown screen missing: $needle"
    done
    for banned in '**' '```' '|---' '~~' 'https://example.com' \
        '# Heading'; do
        if printf '%s\n' "$pane" | grep -Fq "$banned"; then
            fail "markdown syntax leaked to the screen: $banned"
        fi
    done
    ok "assistant markdown renders as styled text and boxes"
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
    pane=$(tmux capture-pane -p -t "$TMUX_SESS")
    header=$(printf '%s\n' "$pane" | head -1)
    footer=$(printf '%s\n' "$pane" | tail -1)
    cat_icon=$(printf '\356\273\255')
    case "$header" in
        " $cat_icon Meong  |  "*) ;;
        *) fail "header does not start with the cat icon and pipe-separated brand" ;;
    esac
    case "$header" in
        *'new chat'*) ;;
        *) fail "header lost the new-chat placeholder" ;;
    esac
    case "$footer" in
        *'mock/split'*) ;;
        *) fail "footer lost the full model id" ;;
    esac
    case "$header" in
        *'·'*) fail "header still uses the dot separator" ;;
    esac
    cwd=$(readlink "/proc/$(tmux list-panes -F '#{pane_pid}' -t "$TMUX_SESS")/cwd" 2>/dev/null || true)
    [ -n "$cwd" ] || fail "cannot resolve the pane cwd"
    case "$header" in
        *"$cwd"*) ;;
        *) fail "header lost the working directory" ;;
    esac

    # A cwd past the old 511-byte getcwd() buffer must keep its tail, not
    # drop to "ready". Separate session: the main one has to survive.
    DEEP="$TMP/deep"
    i=0
    while [ "${#DEEP}" -lt 600 ]; do
        DEEP="$DEEP/deep-dir-abcdefghijklmnopqrstuvwxyz-0123456789-ABCDEFG"
        i=$((i + 1))
    done
    DEEP="$DEEP/tail-marker"
    mkdir -p "$DEEP"
    DEEP_SESS="Meong-deep-$$"
    tmux kill-session -t "$DEEP_SESS" 2>/dev/null || true
    tmux new-session -d -s "$DEEP_SESS" -x 110 -y 40 -c "$DEEP" \
        "env TERM=xterm-256color LC_ALL=C.UTF-8 LANG=C.UTF-8 \
         MEONG_CONFIG_DIR='$TMP/config-deep' XDG_CACHE_HOME='$TMP/cache-deep' \
         '$BIN_ABS' --url '$URL' --model mock/split --no-mouse"
    i=0
    while [ "$i" -lt 100 ]; do
        if tmux capture-pane -p -t "$DEEP_SESS" 2>/dev/null |
            grep -Fq 'ask anything'; then
            break
        fi
        i=$((i + 1))
        sleep 0.05
    done
    header=$(tmux capture-pane -p -t "$DEEP_SESS" 2>/dev/null | head -1)
    case "$header" in
        *'tail-marker'*) ;;
        *)
            tmux kill-session -t "$DEEP_SESS" 2>/dev/null || true
            fail "header lost the tail of a >600-byte cwd"
            ;;
    esac
    case "$header" in
        *'ready'*)
            tmux kill-session -t "$DEEP_SESS" 2>/dev/null || true
            fail "long cwd fell back to ready"
            ;;
    esac
    tmux kill-session -t "$DEEP_SESS" 2>/dev/null || true
    DEEP_SESS=""
    ok "header keeps the tail of a >600-byte cwd"

    click_rc() {
        tmux send-keys -l -t "$TMUX_SESS" \
            "$(printf '\033[<0;%d;%dM' "$2" "$1")"
        tmux send-keys -l -t "$TMUX_SESS" \
            "$(printf '\033[<0;%d;%dm' "$2" "$1")"
    }

    click_at() {
        click_rc "$1" 8
    }

    click_rc 1 3
    i=0
    while [ "$i" -lt 100 ]; do
        if tmux capture-pane -p -t "$TMUX_SESS" 2>/dev/null |
            grep -Fq '( ^.^ )'; then
            break
        fi
        i=$((i + 1))
        sleep 0.05
    done
    pane=$(tmux capture-pane -p -t "$TMUX_SESS")
    printf '%s\n' "$pane" | grep -Fq '( ^.^ )' \
        || fail "clicking the header brand did not open the pet card"
    printf '%s\n' "$pane" | grep -Fq 'purrr... jangan berhenti.' \
        || fail "first pet reaction is missing"
    printf '%s\n' "$pane" | grep -Eq 'dielus +1x' \
        || fail "pet counter did not start at 1"
    printf '%s\n' "$pane" | grep -Fq 'mood' \
        || fail "pet card is missing the mood line"
    ok "clicking Meong pets it and opens the cat card"

    tmux capture-pane -e -p -t "$TMUX_SESS" >"$TMP/pet.ansi"
    python3 tests/python/panel_attr.py "$TMP/pet.ansi" 'pet card' \
        || fail "pet card background"
    ok "pet card has a solid background"

    click_rc 20 50
    i=0
    while [ "$i" -lt 100 ]; do
        if tmux capture-pane -p -t "$TMUX_SESS" 2>/dev/null |
            grep -Eq 'dielus +2x'; then
            break
        fi
        i=$((i + 1))
        sleep 0.05
    done
    pane=$(tmux capture-pane -p -t "$TMUX_SESS")
    printf '%s\n' "$pane" | grep -Eq 'dielus +2x' \
        || fail "clicking inside the card did not pet again"
    printf '%s\n' "$pane" | grep -Fq 'meow. aku dengerin kok.' \
        || fail "the reaction did not change on the second pet"

    click_rc 1 20
    i=0
    while [ "$i" -lt 100 ]; do
        if ! tmux capture-pane -p -t "$TMUX_SESS" 2>/dev/null |
            grep -Fq '( ^.^ )'; then
            break
        fi
        i=$((i + 1))
        sleep 0.05
    done
    if tmux capture-pane -p -t "$TMUX_SESS" |
        grep -Fq '( ^.^ )'; then
        fail "clicking outside the pet card did not close it"
    fi
    ok "petting the card grows the counter; outside click closes"

    click_rc 1 3
    i=0
    while [ "$i" -lt 100 ]; do
        if tmux capture-pane -p -t "$TMUX_SESS" 2>/dev/null |
            grep -Fq '( ^.^ )'; then
            break
        fi
        i=$((i + 1))
        sleep 0.05
    done
    tmux send-keys -l -t "$TMUX_SESS" 'h'
    i=0
    while [ "$i" -lt 100 ]; do
        if ! tmux capture-pane -p -t "$TMUX_SESS" 2>/dev/null |
            grep -Fq '( ^.^ )'; then
            break
        fi
        i=$((i + 1))
        sleep 0.05
    done
    tmux capture-pane -p -t "$TMUX_SESS" | grep -Fq '❯ h' \
        || fail "typing closed the pet card and ate the first key"
    tmux send-keys -t "$TMUX_SESS" C-u
    ok "typing closes the pet card without eating the key"

    click_rc 40 5
    i=0
    while [ "$i" -lt 100 ]; do
        if tmux capture-pane -p -t "$TMUX_SESS" 2>/dev/null |
            grep -Eq 'provider +mock'; then
            break
        fi
        i=$((i + 1))
        sleep 0.05
    done
    pane=$(tmux capture-pane -p -t "$TMUX_SESS")
    printf '%s\n' "$pane" | grep -Eq 'provider +mock' \
        || fail "clicking the footer model did not open its capability card"
    printf '%s\n' "$pane" | grep -Fq 'mock/split' \
        || fail "capability card does not name the current model"
    printf '%s\n' "$pane" | grep -Eq 'context +131,072' \
        || fail "capability card has the wrong context window"
    printf '%s\n' "$pane" | grep -Eq 'max output +65,536' \
        || fail "capability card has the wrong output limit"
    printf '%s\n' "$pane" | grep -Fq 'not reported: vision' \
        || fail "capability card did not summarize the missing vision flag"
    if printf '%s\n' "$pane" | grep -Eq 'vision +(yes|no)'; then
        fail "capability card invented a vision answer"
    fi
    printf '%s\n' "$pane" | grep -Eq 'tools +yes' \
        || fail "capability card lost the tool flag"
    printf '%s\n' "$pane" | grep -Eq 'reasoning +no' \
        || fail "capability card lost the reasoning flag"
    printf '%s\n' "$pane" | grep -Eq 'prompt cache +yes' \
        || fail "capability card lost the prompt-cache flag"
    printf '%s\n' "$pane" | grep -Eq 'cost +free' \
        || fail "capability card lost the cost line"
    printf '%s\n' "$pane" | grep -Fq '~ models.dev estimate for missing fields' \
        || fail "capability card lost the estimate marker"
    ok "clicking the footer model opens its capability card"

    click_rc 20 5
    i=0
    while [ "$i" -lt 100 ]; do
        if ! tmux capture-pane -p -t "$TMUX_SESS" 2>/dev/null |
            grep -Eq 'provider +mock'; then
            break
        fi
        i=$((i + 1))
        sleep 0.05
    done
    if tmux capture-pane -p -t "$TMUX_SESS" |
        grep -Eq 'provider +mock'; then
        fail "clicking outside the capability card did not close it"
    fi
    ok "clicking outside the capability card closes it"

    tmux send-keys -t "$TMUX_SESS" F2
    i=0
    while [ "$i" -lt 100 ]; do
        if tmux capture-pane -p -t "$TMUX_SESS" 2>/dev/null |
            grep -Eq 'provider +mock'; then
            break
        fi
        i=$((i + 1))
        sleep 0.05
    done
    tmux capture-pane -p -t "$TMUX_SESS" | grep -Eq 'provider +mock' \
        || fail "f2 did not open the capability card"
    tmux send-keys -t "$TMUX_SESS" Escape
    i=0
    while [ "$i" -lt 100 ]; do
        if ! tmux capture-pane -p -t "$TMUX_SESS" 2>/dev/null |
            grep -Eq 'provider +mock'; then
            break
        fi
        i=$((i + 1))
        sleep 0.05
    done
    tmux capture-pane -p -t "$TMUX_SESS" | grep -Eq 'provider +mock' \
        && fail "escape did not close the capability card"
    ok "f2 opens the capability card; escape closes it"

    tmux send-keys -t "$TMUX_SESS" F2
    i=0
    while [ "$i" -lt 100 ]; do
        if tmux capture-pane -p -t "$TMUX_SESS" 2>/dev/null |
            grep -Eq 'provider +mock'; then
            break
        fi
        i=$((i + 1))
        sleep 0.05
    done
    tmux capture-pane -e -p -t "$TMUX_SESS" >"$TMP/card.ansi"
    python3 - "$TMP/card.ansi" <<'PY' || fail "capability card shadow"
import sys
sys.path.insert(0, "tests/python")
from panel_attr import screen

text = open(sys.argv[1], encoding="utf-8").read()
if "48;5;" not in text:
    sys.exit("card shadow: capture has no 256-colour SGR (TERM not 256?)")
rows = screen(text)
lines = ["".join(ch for ch, _ in row) for row in rows]
provider = None
for y, line in enumerate(lines):
    if "provider" in line and "mock" in line:
        provider = y
        break
if provider is None:
    sys.exit("card shadow: capability card body not found")
top = None
for y in range(provider - 1, -1, -1):
    if "┌" in lines[y] and "┐" in lines[y]:
        top = y
        break
if top is None:
    sys.exit("card shadow: capability card top border not found")
px = lines[top].index("┌")
rx = lines[top].index("┐")
w = rx - px + 1
bottom = None
for y in range(top + 1, len(rows)):
    if len(rows[y]) > px and rows[y][px][0] in "└╰+":
        bottom = y
if bottom is None or bottom + 1 >= len(rows):
    sys.exit("card shadow: bottom border or shadow row missing")
if any(len(rows[y]) <= px + w for y in range(top + 1, bottom + 2)):
    sys.exit("card shadow: capture rows end before the shadow column")
if any(len(rows[y]) <= px + 2 for y in range(top + 1, bottom)):
    sys.exit("card shadow: capture rows end before the card interior")
inner = {rows[y][px + 2][1] for y in range(top + 1, bottom)}
shadow = {rows[y][px + w][1] for y in range(top + 1, bottom + 1)}
if len(inner) != 1 or len(shadow) != 1:
    sys.exit(f"card shadow: mixed bgs inner={inner} shadow={shadow}")
panel_bg = inner.pop()
shadow_bg = shadow.pop()
if panel_bg is None or shadow_bg is None or panel_bg == shadow_bg:
    sys.exit(
        f"card shadow: no distinct shadow (panel={panel_bg}, shadow={shadow_bg})"
    )
if rows[bottom + 1][px + 1][1] != shadow_bg:
    sys.exit("card shadow: bottom shadow row is missing")
print(f"card shadow: panel={panel_bg} shadow={shadow_bg}")
PY
    ok "capability card casts a right and bottom shadow"
    tmux send-keys -t "$TMUX_SESS" Escape
    i=0
    while [ "$i" -lt 100 ]; do
        if ! tmux capture-pane -p -t "$TMUX_SESS" 2>/dev/null |
            grep -Eq 'provider +mock'; then
            break
        fi
        i=$((i + 1))
        sleep 0.05
    done

    tmux send-keys -t "$TMUX_SESS" F2
    i=0
    while [ "$i" -lt 100 ]; do
        if tmux capture-pane -p -t "$TMUX_SESS" 2>/dev/null |
            grep -Eq 'provider +mock'; then
            break
        fi
        i=$((i + 1))
        sleep 0.05
    done
    tmux send-keys -l -t "$TMUX_SESS" 'h'
    i=0
    while [ "$i" -lt 100 ]; do
        if ! tmux capture-pane -p -t "$TMUX_SESS" 2>/dev/null |
            grep -Eq 'provider +mock'; then
            break
        fi
        i=$((i + 1))
        sleep 0.05
    done
    tmux capture-pane -p -t "$TMUX_SESS" | grep -Fq '❯ h' \
        || fail "typing closed the capability card and ate the first key"
    tmux send-keys -t "$TMUX_SESS" C-u
    ok "typing closes the capability card without eating the key"

    head -1 "$TMP/cache/meong/models" | grep -Fq '#meong-models-v3' \
        || fail "model cache is not v3"
    awk -F '\t' '$1=="mock/split" && $2==131072 && $3==65536 &&
        $4==-1 && $5==1 && $6==0 && $7==1 && $8==0 && $9==0 && $10==1 {found=1}
        END {exit found ? 0 : 1}' "$TMP/cache/meong/models" \
        || fail "model cache lost the capability row"
    ok "model cache persists capabilities"

    tmux send-keys -l -t "$TMUX_SESS" 'hello'
    tmux send-keys -t "$TMUX_SESS" Enter
    i=0
    while [ "$i" -lt 100 ]; do
        if tmux capture-pane -p -t "$TMUX_SESS" 2>/dev/null |
            grep -Fq 'ctx <1%'; then
            break
        fi
        i=$((i + 1))
        sleep 0.05
    done
    sleep 0.2

    head_line=$(tmux capture-pane -p -t "$TMUX_SESS" |
        grep -F '○ thinking ▸' | head -1)
    [ -n "$head_line" ] \
        || fail "thinking block did not fold after the reply finished"
    if printf '%s\n' "$head_line" | grep -Fq 'tok |'; then
        fail "stats stayed on the folded thinking head"
    fi
    tmux capture-pane -p -t "$TMUX_SESS" | grep -Fq 'tok |' \
        || fail "reply lost its bottom stats"
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
    head_line=$(printf '%s\n' "$pane" | grep -F '○ thinking ▾' | head -1)
    if printf '%s\n' "$head_line" | grep -Fq 'tok |'; then
        fail "stats stayed on the expanded thinking head"
    fi
    printf '%s\n' "$pane" | grep -Fq 'tok |' \
        || fail "expanded reply lost its bottom stats"
    printf '%s\n' "$pane" | grep -Fq '╰─ thinking a little' \
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

    TMUX_SESS="Meong-title-$$"
    tmux kill-session -t "$TMUX_SESS" 2>/dev/null || true
    tmux new-session -d -s "$TMUX_SESS" -x 110 -y 40 \
        "env TERM=xterm-256color LC_ALL=C.UTF-8 LANG=C.UTF-8 \
         MEONG_CONFIG_DIR='$TMP/config-title' XDG_CACHE_HOME='$TMP/cache-title' \
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
    header=$(tmux capture-pane -p -t "$TMUX_SESS" | head -1)
    case "$header" in
        *'new chat'*) ;;
        *) fail "header lost the new-chat placeholder" ;;
    esac
    hits0=$(python3 - "$URL/stats" <<'PY'
import json
import sys
import urllib.request

with urllib.request.urlopen(sys.argv[1], timeout=2) as r:
    print(json.loads(r.read().decode())["title_hits"])
PY
)
    tmux send-keys -l -t "$TMUX_SESS" 'first prompt about the cat'
    tmux send-keys -t "$TMUX_SESS" Enter
    i=0
    while [ "$i" -lt 100 ]; do
        header=$(tmux capture-pane -p -t "$TMUX_SESS" 2>/dev/null | head -1)
        case "$header" in
            *'mock chat title'*) break ;;
        esac
        i=$((i + 1))
        sleep 0.05
    done
    case "$header" in
        *'mock chat title'*) ;;
        *) fail "first prompt did not set the AI chat title" ;;
    esac
    tmux capture-pane -p -t "$TMUX_SESS" | tail -1 |
        grep -Fq 'mock/split' || fail "title run lost the footer model"
    tmux send-keys -l -t "$TMUX_SESS" 'second prompt'
    tmux send-keys -t "$TMUX_SESS" Enter
    sleep 0.6
    delta=$(python3 - "$URL/stats" "$hits0" <<'PY'
import json
import sys
import urllib.request

with urllib.request.urlopen(sys.argv[1], timeout=2) as r:
    now = json.loads(r.read().decode())["title_hits"]
print(now - int(sys.argv[2]))
PY
)
    [ "$delta" = 1 ] || fail "title requests for this chat: $delta"
    ok "first prompt sets one AI title; the model stays in the footer"
    tmux kill-session -t "$TMUX_SESS" 2>/dev/null || true
    TMUX_SESS=""

    TMUX_SESS="Meong-title-live-$$"
    tmux kill-session -t "$TMUX_SESS" 2>/dev/null || true
    tmux new-session -d -s "$TMUX_SESS" -x 110 -y 40 \
        "env TERM=xterm-256color LC_ALL=C.UTF-8 LANG=C.UTF-8 \
         MEONG_CONFIG_DIR='$TMP/config-title-live' \
         XDG_CACHE_HOME='$TMP/cache-title-live' \
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
    hits0=$(python3 - "$URL/stats" <<'PY'
import json
import sys
import urllib.request

with urllib.request.urlopen(sys.argv[1], timeout=2) as r:
    print(json.loads(r.read().decode())["title_hits"])
PY
)
    tmux send-keys -l -t "$TMUX_SESS" 'overlap prompt'
    tmux send-keys -t "$TMUX_SESS" Enter
    seen=""
    i=0
    while [ "$i" -lt 100 ]; do
        seen=$(tmux capture-pane -p -t "$TMUX_SESS" 2>/dev/null | head -1)
        case "$seen" in
            *'mock chat title'*'streaming'*) break ;;
        esac
        i=$((i + 1))
        sleep 0.05
    done
    case "$seen" in
        *'mock chat title'*'streaming'*) ;;
        *) fail "title did not land while the chat was still streaming" ;;
    esac
    hits1=$(python3 - "$URL/stats" <<'PY'
import json
import sys
import urllib.request

with urllib.request.urlopen(sys.argv[1], timeout=2) as r:
    print(json.loads(r.read().decode())["title_hits"])
PY
)
    [ "$((hits1 - hits0))" -eq 1 ] \
        || fail "title requests during the slow chat: $((hits1 - hits0))"
    tmux kill-session -t "$TMUX_SESS" 2>/dev/null || true
    TMUX_SESS=""
    ok "title lands while the chat stream is still running"

    mkdir -p "$TMP/toolfix/sub"
    printf 'alpha\nbeta\ngamma\n' >"$TMP/toolfix/notes.md"
    TMUX_SESS="Meong-tools-$$"
    tmux kill-session -t "$TMUX_SESS" 2>/dev/null || true
    tmux new-session -d -s "$TMUX_SESS" -x 110 -y 44 -c "$TMP/toolfix" \
        "env TERM=xterm-256color LC_ALL=C.UTF-8 LANG=C.UTF-8 \
         MEONG_CONFIG_DIR='$TMP/config' XDG_CACHE_HOME='$TMP/cache' \
         '$BIN_ABS' --url '$URL' --model mock/tools-multi"
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
            grep -Fq 'tool said:'; then
            break
        fi
        i=$((i + 1))
        sleep 0.05
    done
    tmux send-keys -t "$TMUX_SESS" C-t
    sleep 0.2
    pane=$(tmux capture-pane -p -t "$TMUX_SESS")
    printf '%s\n' "$pane" | grep -Fq '├─ ✓ read_file  notes.md:1-3' \
        || fail "first tool call lost its tree rail"
    printf '%s\n' "$pane" | grep -Fq '╰─ ✓ list_dir  .' \
        || fail "last tool call lost its terminal rail"
    printf '%s\n' "$pane" | grep -Fq '▸' \
        || fail "collapsed tool call lost its fold marker"
    if printf '%s\n' "$pane" | grep -Fq '[file: notes.md'; then
        fail "tool result is expanded by default"
    fi

    row=$(printf '%s\n' "$pane" | grep -n 'read_file  notes.md' |
        head -1 | cut -d: -f1)
    [ -n "$row" ] || fail "tool row is missing"
    click_at "$row"
    i=0
    while [ "$i" -lt 100 ]; do
        if tmux capture-pane -p -t "$TMUX_SESS" 2>/dev/null |
            grep -Fq '[file: notes.md'; then
            break
        fi
        i=$((i + 1))
        sleep 0.05
    done
    pane=$(tmux capture-pane -p -t "$TMUX_SESS")
    printf '%s\n' "$pane" | grep -Fq '[file: notes.md' \
        || fail "clicking a tool row did not open its box"
    printf '%s\n' "$pane" | grep -Fq '▾' \
        || fail "expanded tool call lost its open marker"
    row=$(printf '%s\n' "$pane" | grep -n 'read_file  notes.md' |
        head -1 | cut -d: -f1)
    click_at "$row"
    i=0
    while [ "$i" -lt 100 ]; do
        if ! tmux capture-pane -p -t "$TMUX_SESS" 2>/dev/null |
            grep -Fq '[file: notes.md'; then
            break
        fi
        i=$((i + 1))
        sleep 0.05
    done
    if tmux capture-pane -p -t "$TMUX_SESS" |
        grep -Fq '[file: notes.md'; then
        fail "clicking the open tool row did not fold the box"
    fi
    tmux send-keys -t "$TMUX_SESS" C-o
    i=0
    while [ "$i" -lt 100 ]; do
        if tmux capture-pane -p -t "$TMUX_SESS" 2>/dev/null |
            grep -Fq '│ f notes.md'; then
            break
        fi
        i=$((i + 1))
        sleep 0.05
    done
    tmux capture-pane -p -t "$TMUX_SESS" | grep -Fq '│ f notes.md' \
        || fail "ctrl-o did not expand the last tool call"
    tmux send-keys -t "$TMUX_SESS" C-o
    i=0
    while [ "$i" -lt 100 ]; do
        if ! tmux capture-pane -p -t "$TMUX_SESS" 2>/dev/null |
            grep -Fq '│ f notes.md'; then
            break
        fi
        i=$((i + 1))
        sleep 0.05
    done
    if tmux capture-pane -p -t "$TMUX_SESS" | grep -Fq '│ f notes.md'; then
        fail "ctrl-o did not fold the last tool call"
    fi
    ok "tool calls render as a tree and expand into a box"
    tmux kill-session -t "$TMUX_SESS" 2>/dev/null || true
    TMUX_SESS=""

    TMUX_SESS="Meong-tools-ascii-$$"
    tmux kill-session -t "$TMUX_SESS" 2>/dev/null || true
    tmux new-session -d -s "$TMUX_SESS" -x 40 -y 20 -c "$TMP/toolfix" \
        "env TERM=xterm-256color LC_ALL=C LANG=C \
         MEONG_ASCII=1 NO_COLOR=1 \
         MEONG_CONFIG_DIR='$TMP/config' XDG_CACHE_HOME='$TMP/cache' \
         '$BIN_ABS' --url '$URL' --model mock/tools-multi"
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
            grep -Fq 'tool said:'; then
            break
        fi
        i=$((i + 1))
        sleep 0.05
    done
    tmux send-keys -t "$TMUX_SESS" C-t
    sleep 0.1
    tmux send-keys -t "$TMUX_SESS" C-o
    i=0
    while [ "$i" -lt 100 ]; do
        if tmux capture-pane -p -t "$TMUX_SESS" 2>/dev/null |
            grep -Fq '| f notes.md'; then
            break
        fi
        i=$((i + 1))
        sleep 0.05
    done
    sleep 0.2
    pane=$(tmux capture-pane -p -t "$TMUX_SESS")
    row=$(printf '%s\n' "$pane" | grep -n '| f notes.md' |
        head -1 | cut -d: -f1)
    top=$(printf '%s\n' "$pane" |
        grep -nE '^(     |  \|  )\+-{5,}\+$' | head -1 | cut -d: -f1)
    bot=$(printf '%s\n' "$pane" |
        grep -nE '^(     |  \|  )\+-{5,}\+$' | tail -1 | cut -d: -f1)
    [ -n "$row" ] || fail "ascii tool box lost its body row"
    [ -n "$top" ] && [ -n "$bot" ] && [ "$top" -lt "$row" ] &&
        [ "$row" -lt "$bot" ] \
        || fail "ascii tool box lost a horizontal rule"
    if printf '%s\n' "$pane" | grep -Fq '┌'; then
        fail "ascii tool box used a unicode corner"
    fi
    ok "narrow ascii tool box draws one-cell rules"
    tmux kill-session -t "$TMUX_SESS" 2>/dev/null || true
    TMUX_SESS=""

    TMUX_SESS="Meong-tools-think-$$"
    tmux kill-session -t "$TMUX_SESS" 2>/dev/null || true
    tmux new-session -d -s "$TMUX_SESS" -x 110 -y 44 -c "$TMP/toolfix" \
        "env TERM=xterm-256color LC_ALL=C.UTF-8 LANG=C.UTF-8 \
         MEONG_CONFIG_DIR='$TMP/config' XDG_CACHE_HOME='$TMP/cache' \
         '$BIN_ABS' --url '$URL' --model mock/tools-think"
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
            grep -Fq 'done with tools'; then
            break
        fi
        i=$((i + 1))
        sleep 0.05
    done
    sleep 0.2
    pane=$(tmux capture-pane -p -t "$TMUX_SESS")
    printf '%s\n' "$pane" | grep -Fq '○ thinking ▾' \
        || fail "thinking-plus-tools: reasoning did not stay open"
    printf '%s\n' "$pane" | grep -Fq '╰─ thinking a little with tools' \
        || fail "thinking-plus-tools: reasoning text is missing"
    printf '%s\n' "$pane" | grep -Fq '╰─ ✓ read_file  notes.md:1-3' \
        || fail "thinking-plus-tools: tool row is missing"
    head_row=$(printf '%s\n' "$pane" | grep -n '○ thinking' |
        head -1 | cut -d: -f1)
    [ -n "$head_row" ] || fail "thinking-plus-tools: head row is missing"
    click_at "$head_row"
    i=0
    while [ "$i" -lt 100 ]; do
        if tmux capture-pane -p -t "$TMUX_SESS" 2>/dev/null |
            grep -Fq '○ thinking ▸'; then
            break
        fi
        i=$((i + 1))
        sleep 0.05
    done
    pane=$(tmux capture-pane -p -t "$TMUX_SESS")
    head_line=$(printf '%s\n' "$pane" | grep -F '○ thinking ▸' | head -1)
    if printf '%s\n' "$head_line" | grep -Fq 'tok |'; then
        fail "thinking-plus-tools: intermediate thinking kept its stats"
    fi
    printf '%s\n' "$pane" | grep -Fq '14 tok |' \
        || fail "thinking-plus-tools: final stats are not accumulated"
    if printf '%s\n' "$pane" | grep -Fq '7 tok |'; then
        fail "thinking-plus-tools: per-response stats survived"
    fi
    head_row=$(printf '%s\n' "$pane" | grep -n '○ thinking ▸' |
        head -1 | cut -d: -f1)
    next_line=$(printf '%s\n' "$pane" | sed -n "$((head_row + 1))p")
    printf '%s\n' "$next_line" | grep -Fq '╰─ ✓ read_file  notes.md:1-3' \
        || fail "thinking-plus-tools: blank row before the tool group"
    ok "folded thinking attaches tool rows without a blank line"
    tmux kill-session -t "$TMUX_SESS" 2>/dev/null || true
    TMUX_SESS=""

    TMUX_SESS="Meong-tools-stream-$$"
    tmux kill-session -t "$TMUX_SESS" 2>/dev/null || true
    tmux new-session -d -s "$TMUX_SESS" -x 100 -y 30 -c "$TMP/toolfix" \
        "env TERM=xterm-256color LC_ALL=C.UTF-8 LANG=C.UTF-8 \
         MEONG_CONFIG_DIR='$TMP/config' XDG_CACHE_HOME='$TMP/cache' \
         '$BIN_ABS' --url '$URL' --model mock/tools-think-slow"
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
            grep -Fq '○ thinking ▾'; then
            break
        fi
        i=$((i + 1))
        sleep 0.05
    done
    pane=$(tmux capture-pane -p -t "$TMUX_SESS")
    printf '%s\n' "$pane" | grep -Fq '○ thinking ▾' \
        || fail "slow stream: thinking did not open"
    printf '%s\n' "$pane" | grep -Fq '╰─ thinking' \
        || fail "slow stream: reasoning close is missing"
    if printf '%s\n' "$pane" | grep -Fq '✦'; then
        fail "slow stream: empty assistant placeholder survived"
    fi
    ok "streaming thinking has no empty placeholder row"

    i=0
    while [ "$i" -lt 140 ]; do
        if tmux capture-pane -p -t "$TMUX_SESS" 2>/dev/null |
            grep -Fq '14 tok |'; then
            break
        fi
        i=$((i + 1))
        sleep 0.05
    done
    sleep 0.2
    pane=$(tmux capture-pane -p -t "$TMUX_SESS")
    printf '%s\n' "$pane" | grep -Fq 'done with tools' \
        || fail "slow stream: final answer is missing"
    marker_row=$(printf '%s\n' "$pane" | grep -n '✦   1 tool call' |
        head -1 | cut -d: -f1)
    [ -n "$marker_row" ] || fail "slow stream: tool-call marker is missing"
    next_line=$(printf '%s\n' "$pane" | sed -n "$((marker_row + 1))p")
    printf '%s\n' "$next_line" | grep -Fq '╰─ ✓ read_file  notes.md:1-3' \
        || fail "slow stream: blank row before the tool group"
    printf '%s\n' "$pane" | grep -Fq '14 tok |' \
        || fail "slow stream: final stats are not accumulated"
    ok "slow stream attaches tool rows without a gap"
    tmux kill-session -t "$TMUX_SESS" 2>/dev/null || true
    TMUX_SESS=""

    TMUX_SESS="Meong-tools-perm-$$"
    tmux kill-session -t "$TMUX_SESS" 2>/dev/null || true
    tmux new-session -d -s "$TMUX_SESS" -x 90 -y 26 -c "$PWD" \
        "env TERM=xterm-256color LC_ALL=C.UTF-8 LANG=C.UTF-8 \
         MEONG_CONFIG_DIR='$TMP/config' XDG_CACHE_HOME='$TMP/cache' \
         '$BIN_ABS' --url '$URL' --model mock/tools --no-mouse"
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
            grep -Fq '[ Reject ]'; then
            break
        fi
        i=$((i + 1))
        sleep 0.05
    done
    pane=$(tmux capture-pane -p -t "$TMUX_SESS")
    printf '%s\n' "$pane" | grep -Fq '? read_file  /proj/version.txt' \
        || fail "permission row lost its ask glyph"
    printf '%s\n' "$pane" | grep -Fq 'permission' \
        || fail "permission card did not replace the composer"
    printf '%s\n' "$pane" | grep -Fq 'outside cwd' \
        || fail "permission card lost the outside-cwd label"
    printf '%s\n' "$pane" | grep -Fq '{"path":"/proj/version.txt"}' \
        || fail "permission card hid the requested arguments"
    tmux capture-pane -e -p -t "$TMUX_SESS" >"$TMP/perm.ansi"
    python3 - "$TMP/perm.ansi" <<'PY' || fail "permission card layout"
import sys
sys.path.insert(0, "tests/python")
from panel_attr import cells

rows = cells(open(sys.argv[1], encoding="utf-8").read())
lines = ["".join(ch for ch, _, _ in row) for row in rows]
title = next(
    (y for y, line in enumerate(lines)
     if "permission" in line and "outside cwd" in line),
    None,
)
if title is None:
    sys.exit("permission card: title line not found")
call = next(
    (y for y, line in enumerate(lines)
     if y > title and "? read_file" in line),
    None,
)
if call is None:
    sys.exit("permission card: pending call row not found")
if call != title + 2:
    sys.exit(f"permission card: want one blank row (title={title}, call={call})")
if lines[title + 1].strip(" │|"):
    sys.exit("permission card: the gap row is not blank")
x = lines[title].index("outside cwd")
fg = rows[title][x][1]
if fg != 208:
    sys.exit(f"permission card: outside cwd fg={fg}, want 208")
if "\U000f0ecd" not in lines[title]:
    sys.exit("permission card: title lost the nf-md-shield_alert_outline glyph")
panel_bg = rows[title + 1][2][2]
qx = lines[call].index("?")
q_fg, q_bg = rows[call][qx][1], rows[call][qx][2]
if q_bg != panel_bg or q_fg != 154:
    sys.exit(
        f"permission card: ask glyph fg/bg={q_fg}/{q_bg}, "
        f"want 154/{panel_bg}"
    )
if not lines[title].lstrip(" ").startswith("┌──") or lines[title].index("permission") < 3:
    sys.exit("permission card: title lost its border prefix")
print(
    f"permission card: title={title}, call={call}, outside cwd fg={fg}, "
    f"ask fg/bg={q_fg}/{q_bg}"
)
PY
    printf '%s\n' "$pane" | grep -Fq '[ Allow ]' \
        || fail "permission card lost the allow button"
    printf '%s\n' "$pane" | grep -Fq '[ Reject ]' \
        || fail "permission card lost the reject button"
    printf '%s\n' "$pane" | grep -Fq '[ Always allow this session ]' \
        || fail "permission card lost the session button"
    printf '%s\n' "$pane" | grep -Fq 'always = this session' \
        || fail "permission card did not scope the session button"
    tmux send-keys -t "$TMUX_SESS" Right
    tmux send-keys -t "$TMUX_SESS" Enter
    i=0
    while [ "$i" -lt 100 ]; do
        if tmux capture-pane -p -t "$TMUX_SESS" 2>/dev/null |
            grep -Fq 'permission denied'; then
            break
        fi
        i=$((i + 1))
        sleep 0.05
    done
    pane=$(tmux capture-pane -p -t "$TMUX_SESS")
    printf '%s\n' "$pane" | grep -Fq '✗ read_file  /proj/version.txt' \
        || fail "denied permission did not turn the row into an error"
    if printf '%s\n' "$pane" | grep -Fq '[ Reject ]'; then
        fail "permission card survived the answer"
    fi
    ok "permission card denies with right-arrow and enter"
    tmux kill-session -t "$TMUX_SESS" 2>/dev/null || true
    TMUX_SESS=""

    TMUX_SESS="Meong-tools-always-$$"
    tmux kill-session -t "$TMUX_SESS" 2>/dev/null || true
    tmux new-session -d -s "$TMUX_SESS" -x 90 -y 26 -c "$PWD" \
        "env TERM=xterm-256color LC_ALL=C.UTF-8 LANG=C.UTF-8 \
         MEONG_CONFIG_DIR='$TMP/config' XDG_CACHE_HOME='$TMP/cache' \
         '$BIN_ABS' --url '$URL' --model mock/tools-session --no-mouse"
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
            grep -Fq '[ Reject ]'; then
            break
        fi
        i=$((i + 1))
        sleep 0.05
    done
    pane=$(tmux capture-pane -p -t "$TMUX_SESS")
    printf '%s\n' "$pane" | grep -Fq '[ Always allow this session ]' \
        || fail "session allow: the card lost its third button"
    tmux send-keys -t "$TMUX_SESS" a
    i=0
    while [ "$i" -lt 200 ]; do
        if tmux capture-pane -p -t "$TMUX_SESS" 2>/dev/null |
            grep -Fq 'tool said:'; then
            break
        fi
        i=$((i + 1))
        sleep 0.05
    done
    pane=$(tmux capture-pane -p -t "$TMUX_SESS")
    printf '%s\n' "$pane" | grep -Fq 'tool said:' \
        || fail "session allow: the tool round never finished"
    if printf '%s\n' "$pane" | grep -Fq '[ Reject ]'; then
        fail "session allow prompted again for the second outside path"
    fi
    calls=$(printf '%s\n' "$pane" | grep -Fc '✗ read_file' || true)
    [ "${calls:-0}" -ge 2 ] \
        || fail "session allow did not run both outside calls"
    ok "always allow this session skips the next outside path"
    tmux kill-session -t "$TMUX_SESS" 2>/dev/null || true
    TMUX_SESS=""

    TMUX_SESS="Meong-tools-click-$$"
    tmux kill-session -t "$TMUX_SESS" 2>/dev/null || true
    tmux new-session -d -s "$TMUX_SESS" -x 90 -y 26 -c "$PWD" \
        "env TERM=xterm-256color LC_ALL=C.UTF-8 LANG=C.UTF-8 \
         MEONG_CONFIG_DIR='$TMP/config' XDG_CACHE_HOME='$TMP/cache' \
         '$BIN_ABS' --url '$URL' --model mock/tools"
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
            grep -Fq '[ Reject ]'; then
            break
        fi
        i=$((i + 1))
        sleep 0.05
    done
    # 90x26 card: buttons at row 22 (1-based), Reject spans columns 15-24.
    tmux send-keys -l -t "$TMUX_SESS" "$(printf '\033[<0;19;22M')"
    tmux send-keys -l -t "$TMUX_SESS" "$(printf '\033[<0;19;22m')"
    i=0
    while [ "$i" -lt 100 ]; do
        if tmux capture-pane -p -t "$TMUX_SESS" 2>/dev/null |
            grep -Fq 'permission denied'; then
            break
        fi
        i=$((i + 1))
        sleep 0.05
    done
    pane=$(tmux capture-pane -p -t "$TMUX_SESS")
    printf '%s\n' "$pane" | grep -Fq '✗ read_file  /proj/version.txt' \
        || fail "clicking reject did not deny the call"
    ok "permission card rejects on a mouse click"
    tmux kill-session -t "$TMUX_SESS" 2>/dev/null || true
    TMUX_SESS=""

    TMUX_SESS="Meong-pet-narrow-$$"
    tmux kill-session -t "$TMUX_SESS" 2>/dev/null || true
    tmux new-session -d -s "$TMUX_SESS" -x 44 -y 22 \
        "env TERM=xterm-256color LC_ALL=C.UTF-8 LANG=C.UTF-8 \
         MEONG_CONFIG_DIR='$TMP/config' XDG_CACHE_HOME='$TMP/cache' \
         '$BIN_ABS' --url '$URL' --model mock/plain"
    i=0
    while [ "$i" -lt 100 ]; do
        if tmux capture-pane -p -t "$TMUX_SESS" 2>/dev/null |
            grep -Fq 'ask anything'; then
            break
        fi
        i=$((i + 1))
        sleep 0.05
    done
    tmux send-keys -l -t "$TMUX_SESS" "$(printf '\033[<0;3;1M')"
    tmux send-keys -l -t "$TMUX_SESS" "$(printf '\033[<0;3;1m')"
    i=0
    while [ "$i" -lt 100 ]; do
        if tmux capture-pane -p -t "$TMUX_SESS" 2>/dev/null |
            grep -Fq '( ^.^ )'; then
            break
        fi
        i=$((i + 1))
        sleep 0.05
    done
    pane=$(tmux capture-pane -p -t "$TMUX_SESS")
    printf '%s\n' "$pane" | grep -Fq '( ^.^ )' \
        || fail "pet card did not open on a narrow terminal"
    printf '%s\n' "$pane" | grep -Eq 'dielus +1x' \
        || fail "narrow pet card clipped the pet counter"
    printf '%s\n' "$pane" | grep -Fq 'mood' \
        || fail "narrow pet card clipped the mood line"
    printf '%s\n' "$pane" | grep -Fq 'model' \
        || fail "narrow pet card clipped the model line"
    printf '%s\n' "$pane" | grep -Fq 'purrr' \
        || fail "narrow pet card clipped the reaction"

    tmux resize-window -t "$TMUX_SESS" -x 36 -y 10
    sleep 0.4
    pane=$(tmux capture-pane -p -t "$TMUX_SESS")
    header=$(printf '%s\n' "$pane" | head -1)
    case "$header" in
        " $cat_icon Meong  |  "*) ;;
        *) fail "36-column header lost the cat brand" ;;
    esac
    tmux capture-pane -p -t "$TMUX_SESS" | tail -1 |
        grep -Fq 'mock/plain' || fail "36-column footer lost the model id"
    printf '%s\n' "$pane" | grep -Fq '( ^.^ )' \
        || fail "pet card vanished at 36x10"
    printf '%s\n' "$pane" | grep -Eq 'dielus +1x' \
        || fail "pet card lost its counter at 36x10"
    ok "pet card fits 44x22 and 36x10 terminals"
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
            grep -Fq 'tok |'; then
            break
        fi
        i=$((i + 1))
        sleep 0.05
    done
    sleep 0.2
    pane=$(tmux capture-pane -p -t "$TMUX_SESS")
    printf '%s\n' "$pane" | grep -Fq '○ thinking ▾' \
        || fail "reasoning-only reply folded its only content"
    printf '%s\n' "$pane" | grep -Fq '╰─ thinking a little' \
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
    tmux send-keys -t "$TMUX_SESS" Tab
    i=0
    while [ "$i" -lt 100 ]; do
        if tmux capture-pane -p -t "$TMUX_SESS" 2>/dev/null |
            grep -Fq 'choose a model'; then
            break
        fi
        i=$((i + 1))
        sleep 0.05
    done
    tmux send-keys -t "$TMUX_SESS" F1
    i=0
    while [ "$i" -lt 100 ]; do
        if tmux capture-pane -p -t "$TMUX_SESS" 2>/dev/null |
            grep -Fq '( ^.^ )'; then
            break
        fi
        i=$((i + 1))
        sleep 0.05
    done
    pane=$(tmux capture-pane -p -t "$TMUX_SESS")
    printf '%s\n' "$pane" | grep -Fq '( ^.^ )' \
        || fail "f1 did not open the pet card"
    if printf '%s\n' "$pane" | grep -Fq 'choose a model'; then
        fail "f1 left the model picker open under the pet card"
    fi
    tmux send-keys -t "$TMUX_SESS" Escape
    i=0
    while [ "$i" -lt 100 ]; do
        if ! tmux capture-pane -p -t "$TMUX_SESS" 2>/dev/null |
            grep -Fq '( ^.^ )'; then
            break
        fi
        i=$((i + 1))
        sleep 0.05
    done
    pane=$(tmux capture-pane -p -t "$TMUX_SESS")
    if printf '%s\n' "$pane" | grep -Fq '( ^.^ )'; then
        fail "escape did not close the pet card"
    fi
    if printf '%s\n' "$pane" | grep -Fq 'choose a model'; then
        fail "the picker came back after closing the pet card"
    fi
    if ! printf '%s\n' "$pane" | grep -Fq 'ask anything'; then
        fail "closing the pet card leaked a character into the composer"
    fi
    ok "f1 pets Meong over the picker without swallowing keys"

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
            grep -Fq 'Meong | help'; then
            break
        fi
        i=$((i + 1))
        sleep 0.05
    done
    tmux capture-pane -p -t "$TMUX_SESS" | grep -Fq 'Meong | help' \
        || fail "tab completion did not run /help"
    tmux send-keys -t "$TMUX_SESS" q
    tmux send-keys -l -t "$TMUX_SESS" '/help'
    sleep 0.3
    tmux send-keys -t "$TMUX_SESS" Enter
    i=0
    while [ "$i" -lt 100 ]; do
        if tmux capture-pane -p -t "$TMUX_SESS" 2>/dev/null |
            grep -Fq 'Meong | help'; then
            break
        fi
        i=$((i + 1))
        sleep 0.05
    done
    tmux capture-pane -p -t "$TMUX_SESS" | grep -Fq 'Meong | help' \
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
            grep -Eq '^  . [0-9]+\.[0-9]+s +[^ ]+ ch'; then
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

    LEGACY="$TMP/legacy-cache"
    mkdir -p "$LEGACY"
    printf 'mock/plain\nmock/split\n' >"$LEGACY/models"
    TMUX_SESS="Meong-legacy-$$"
    tmux new-session -d -s "$TMUX_SESS" -x 110 -y 40 \
        "env TERM=xterm-256color LC_ALL=C.UTF-8 LANG=C.UTF-8 \
         MEONG_CACHE_DIR='$LEGACY' \
         '$BIN_ABS' --url '$URL' --model mock/split"
    i=0
    while [ "$i" -lt 200 ]; do
        if tmux capture-pane -p -t "$TMUX_SESS" 2>/dev/null |
            grep -Fq 'mock/split'; then
            break
        fi
        i=$((i + 1))
        sleep 0.05
    done
    head -1 "$LEGACY/models" | grep -Fq '#meong-models-v3' \
        || fail "legacy cache was not migrated to v3"
    click_rc 40 5
    i=0
    while [ "$i" -lt 100 ]; do
        if tmux capture-pane -p -t "$TMUX_SESS" 2>/dev/null |
            grep -Eq 'provider +mock'; then
            break
        fi
        i=$((i + 1))
        sleep 0.05
    done
    tmux capture-pane -p -t "$TMUX_SESS" | grep -Eq 'context +131,072' \
        || fail "legacy cache migration did not load capabilities"
    ok "legacy model cache migrates to v3 with capabilities"
    tmux kill-session -t "$TMUX_SESS" 2>/dev/null || true
    TMUX_SESS=""

    V2CACHE="$TMP/v2-cache"
    mkdir -p "$V2CACHE"
    printf '#meong-models-v2\nmock/plain\t-1\t-1\t-1\t-1\t-1\t-1\t-1\t-1\nmock/split\t131072\t65536\t-1\t1\t0\t1\t0\t0\n' \
        >"$V2CACHE/models"
    TMUX_SESS="Meong-v2-$$"
    tmux new-session -d -s "$TMUX_SESS" -x 110 -y 40 \
        "env TERM=xterm-256color LC_ALL=C.UTF-8 LANG=C.UTF-8 \
         MEONG_CACHE_DIR='$V2CACHE' \
         '$BIN_ABS' --url '$URL' --model mock/split"
    i=0
    while [ "$i" -lt 200 ]; do
        if tmux capture-pane -p -t "$TMUX_SESS" 2>/dev/null |
            grep -Fq 'mock/split'; then
            break
        fi
        i=$((i + 1))
        sleep 0.05
    done
    head -1 "$V2CACHE/models" | grep -Fq '#meong-models-v3' \
        || fail "v2 cache was not migrated to v3"
    click_rc 40 5
    i=0
    while [ "$i" -lt 100 ]; do
        if tmux capture-pane -p -t "$TMUX_SESS" 2>/dev/null |
            grep -Eq 'provider +mock'; then
            break
        fi
        i=$((i + 1))
        sleep 0.05
    done
    tmux capture-pane -p -t "$TMUX_SESS" |
        grep -Fq '~ models.dev estimate for missing fields' \
        || fail "v2 cache migration did not load enriched capabilities"
    ok "v2 model cache migrates to v3 with estimates"
    tmux kill-session -t "$TMUX_SESS" 2>/dev/null || true
    TMUX_SESS=""

    TMUX_SESS="Meong-errorwin-$$"
    tmux kill-session -t "$TMUX_SESS" 2>/dev/null || true
    tmux new-session -d -s "$TMUX_SESS" -x 110 -y 40 \
        "env TERM=xterm-256color LC_ALL=C.UTF-8 LANG=C.UTF-8 \
         MEONG_CONFIG_DIR='$TMP/config' XDG_CACHE_HOME='$TMP/cache' \
         '$BIN_ABS' --url '$URL' --model mock/flaky-tmux"
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
    while [ "$i" -lt 200 ]; do
        if tmux capture-pane -p -t "$TMUX_SESS" 2>/dev/null |
            grep -Fq 'request failed'; then
            break
        fi
        i=$((i + 1))
        sleep 0.05
    done
    pane=$(tmux capture-pane -p -t "$TMUX_SESS")
    printf '%s\n' "$pane" | grep -Fq 'request failed' \
        || fail "a failed request did not open the error window"
    printf '%s\n' "$pane" | grep -Fq 'mock flaky failure' \
        || fail "error window lost the gateway message"
    printf '%s\n' "$pane" | grep -Fq 'retry' \
        || fail "error window lost the retry choice"
    printf '%s\n' "$pane" | grep -Fq 'close' \
        || fail "error window lost the close choice"
    ok "a failed request opens a window with retry and close"

    tmux capture-pane -e -p -t "$TMUX_SESS" >"$TMP/errorwin.ansi"
    python3 tests/python/panel_attr.py "$TMP/errorwin.ansi" 'error window' \
        || fail "error window background"
    ok "error window has a solid background"

    row=$(tmux capture-pane -p -t "$TMUX_SESS" |
        grep -n 'retry' | head -1 | cut -d: -f1)
    [ -n "$row" ] || fail "error window retry row not found"
    click_rc "$row" 30
    i=0
    while [ "$i" -lt 200 ]; do
        if tmux capture-pane -p -t "$TMUX_SESS" 2>/dev/null |
            grep -Fq 'recovered after retry'; then
            break
        fi
        i=$((i + 1))
        sleep 0.05
    done
    tmux capture-pane -p -t "$TMUX_SESS" |
        grep -Fq 'recovered after retry' \
        || fail "clicking retry did not replay the failed request"
    ok "clicking retry replays the failed request"

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
        && fail "error window session did not exit"
    TMUX_SESS=""

    TMUX_SESS="Meong-errornoinline-$$"
    tmux kill-session -t "$TMUX_SESS" 2>/dev/null || true
    tmux new-session -d -s "$TMUX_SESS" -x 110 -y 40 \
        "env TERM=xterm-256color LC_ALL=C.UTF-8 LANG=C.UTF-8 \
         MEONG_CONFIG_DIR='$TMP/config' XDG_CACHE_HOME='$TMP/cache' \
         '$BIN_ABS' --url '$URL' --model mock/error200 --no-mouse"
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
    while [ "$i" -lt 200 ]; do
        if tmux capture-pane -p -t "$TMUX_SESS" 2>/dev/null |
            grep -Fq 'request failed'; then
            break
        fi
        i=$((i + 1))
        sleep 0.05
    done
    tmux send-keys -t "$TMUX_SESS" Escape
    i=0
    while [ "$i" -lt 100 ]; do
        if ! tmux capture-pane -p -t "$TMUX_SESS" 2>/dev/null |
            grep -Fq 'request failed'; then
            break
        fi
        i=$((i + 1))
        sleep 0.05
    done
    pane=$(tmux capture-pane -p -t "$TMUX_SESS")
    printf '%s\n' "$pane" | grep -Fq '◑ hello' \
        || fail "closing the error window dropped the user turn"
    if printf '%s\n' "$pane" | grep -Fq 'mock says no'; then
        fail "the closed error window left an inline error row"
    fi
    ok "closing the error window leaves no inline error row"
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
        && fail "inline-error session did not exit"
    TMUX_SESS=""
else
    printf 'skip - tmux not found, screen check for the input gap skipped\n'
fi

printf '\nall %d checks passed\n' "$CHECKS"
