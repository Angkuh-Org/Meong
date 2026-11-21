# Layout

`src/` holds the linked client.
`src/rules/` and `tools/` hold the host-side
rulebook gate: rulebook code never enters `build/meong`.

```text
src/
  main.c  event loop and key dispatch
  app/    conversation state, queue, config paths, saved model
  api/    wire parsing and the model catalog
  chat/   request building, stream lifecycle, slash commands
  cli/    flags and non-TUI modes
  json/   vendored jsmn and wrapper
  net/    HTTP/1.1 and SSE
  rules/  the rulebook; host-side only
  ui/     chrome, layout, panels, composer
  util/   buffers and string helpers
tools/    rulecheck, the host-side rulebook gate
tests/    shell harness, C harnesses (tests/c/), pty/tmux TUI tests
```

## `src/main.c`

`main()` dispatches the CLI modes and leaves the default path to `tui_main()`,
which seeds the `App`, resolves the endpoint, loads the model list, and picks
the initial model. `tui_run()` is the event loop: read a key, tick the active
reply and title streams, redraw dirty frames on a 30 ms throttle (50 ms ticks
while streaming), and animate the spinner at 5 Hz (one clockwise turn per 0.8 s
over four quadrant frames). `handle_key()` routes keys to the request error
window first, then the composer, scrolling, the slash menu, the picker, the
help overlay, the pet card, or the model capability card; `handle_mouse()` owns
the header brand box, the footer model hit-box, the pet-card hit-box, the
error-window rows, and the chat click.

## `src/app/`

`App` in `app.h` is the single mutable state object: messages, model list and
per-model capabilities, picker, help, pet-card, capability-card and
error-window state, the active reply stream, the one-shot title stream and its
attempt flag, the prompt queue, the scroll/layout cache, composer text,
history, and theme flags. `app.c` owns
message and queue storage and the config/cache directory rules; it also calls
`md_reset`/`md_release` so a `Msg` owns its display buffer even though the
markdown translator itself lives in `ui/md.c`. `state.c`
reads and writes the last selected model; user-facing behavior is in
[`models.md`](models.md).

## `src/api/`

`wire.c` parses the non-streaming reply. `extract_chat_text()` pulls
`choices[0].message.content` (or `choices[0].text`) plus usage tokens,
`extract_api_error()` digs `error.message` or `error.detail`, and
`drive_stream()` polls a stream to completion under a total timeout. `models.c`
fetches `/v1/models` (relative to the configured base), parses each entry's
`capabilities` object (tri-state: absent, no, yes) alongside the id, sorts and
caches the pairs in `models` with a `#meong-models-v3` header, and picks a
default. An older cache returns a refetch flag so `tui_main()` migrates it
once; a failed refresh keeps the old ids. Selection, state, and cache behavior
are documented in [`models.md`](models.md).

## `src/chat/`

`chat.c` selects turns newest-first within `CHAT_BUDGET` and emits them
chronologically, adding a one-line system note when older turns are dropped.
Its SSE callback reuses one `JDoc`/`Buf` pair on `App`, appends `delta.content`
to the reply and `reasoning_content`/`reasoning` to the dimmed reasoning line,
and falls back to the full parser on error or odd frames. It owns
`start_generation()`, `finish_generation()`, `cancel_generation()`, and
`drain_queue()`; both generation paths open the error window when a request
fails, and failures stay out of the transcript (a partial reply is kept).
`commands.c` implements `/model`, `/models`, `/system`, `/clear`,
`/retry`, `/help`, and `/quit`, owns the command table behind the composer's
slash menu, and shares `retry_last()` with the error window's retry row.
`title.c` owns the chat title: the first user prompt of a chat fires exactly
one extra request to the current model through `stream_start_async()`, so a
slow TCP handshake cannot delay the chat request; it ticks on that independent
`Stream` and stores a sanitised single-line reply from either an SSE or JSON
body (PAIG's free route forces streaming). A failed request leaves the header
at the `new chat` placeholder, and `/clear` resets the attempt.
`tool_accum.c` is the index-keyed streaming accumulator: whole calls replace a
slot on `cc/*`, fragmented `arguments` append on passthrough routes, and a call
with no index/id starts a new slot unless it is an argument-only continuation.
`chat.c` also owns tool display identity: a `role:"tool"` message stores its
parent assistant index and call position, a pending permission row is filled in
place instead of appended twice, and cancellation fills unanswered rows rather
than leaving an empty result in the wire history. A tool round rebuilds layout
from the last user message, so the assistant's mid-stream placeholder and gap
rows are replaced by the compact final form.

## `src/tools/`

User contract: [`tools.md`](tools.md).

`tools.c` owns the static, byte-stable OpenAI tool array and dispatch, plus
`tool_call_summary()`, the display-only shortening of a call's arguments;
`exec.c` implements the three read-only handlers (`read_file` with a metadata
header and line range, `search` with glob/context, `list_dir` with type/size);
`edit.c` implements the one mutating handler: exact-unique match, bounded
preview, full-file identity, then atomic temp+fsync+rename. Every tool is
registered with an action class in `tools.c`; `tool_execute()` resolves the
top-level path, denies a protected path, runs the prepare hook, then calls the
gate before the handler. `perm.c` holds the pure policy, the protected list,
the dirfd mutation resolver, and the identity comparison. A read path inside
the cwd runs, an outside path asks the TUI; `mutate`/`exec` always ask unless
the matching `--allow-writes`/`--allow-exec` is set. `--yolo` covers
outside-cwd reads only and never a protected path. `search` keeps a per-file
check so a multi-path call cannot bypass the class gate. Results larger than
16 KiB keep a head/tail inline and spill the full text under the cache
directory. `chat.c` runs at most eight tool rounds and fills an
`[interrupted by user]` result into every unanswered call position on cancel.

The permission card replaces the composer: 7 rows below 11 terminal rows, 8
rows at 11–13 (a read stays at 8). A mutate card grows with its preview lines
(up to 15 rows) and shows up to 3+3 changed lines; short terminals show the
`path:line` header plus fewer lines. `chat_height()` and `perm_draw()` share
`perm_card_h()`, so the reserved rows and the drawn card cannot disagree.

## `src/cli/`

`parse_args()` handles the long options. `resolve_url()` applies the URL
precedence: `--url`, `MEONG_URL`, `PAIG_URL`, `PAIG_HOST`/`PAIG_PORT`, then
`http://127.0.0.1:8899`. `cli_oneshot()` and `cli_list_models()` are the
non-TUI paths; `usage()` and `version()` print the help and version text.

## `src/json/`

`jsmn.h` is the vendored MIT tokenizer. `json.c` owns the reusable `JDoc`
wrapper (a combined token+skip block, grown on demand), object/array traversal,
string unescaping including surrogate pairs, and `jdelta_scan()`, the fast
OpenAI delta/usage scanner that falls back to jsmn for errors and odd shapes.
`_Static_assert`s pin the wrapper's token enum to jsmn's; the skip table and the
fast scanner each have a differential harness in `tests/c/`.

## `src/net/`

A nonblocking HTTP/1.1 client with no TLS and no libcurl. `endpoint_parse()`
splits the base URL and strips a trailing `/v1`, so request paths such as
`/v1/models` do not double up; `stream_start()` opens the connection and sends
the request, while `stream_start_async()` defers the TCP handshake to the
first `stream_tick()` so the one-shot title request cannot delay the chat
request. `stream_tick()` returns `0` while running, `1` on completion, and
a negative value on error. It decodes chunked transfer and SSE `data:` frames,
stops at `[DONE]`, tolerates an un-framed JSON body in stream mode, and uses
`Connection: close` — one request per connection. Caps: 128 KB headers, 8 MB
line, 1 MB non-SSE fallback body. `MEONG_WIRE_DEBUG` dumps parsed lines to
stderr.

## `src/rules/`

`rulebook.c` holds the `MEONG_RULEBOOK` table and `meong_rules_audit()`. It
compiles with the host compiler into `build/rulecheck`, never into the client.
Current law: `loc-per-file` — no `.c` or `.h` under `src/` may exceed 2000
physical lines.

## `src/ui/`

`ui.c` handles terminal and theme setup (locale, UTF-8 detection, colour pairs,
mouse), the header, status, input, and footer chrome, the splash and scrollbar,
and the `ui_draw()` frame; the header draws the
`nf-fa-cat` mark and records the `Meong` brand box so a click or tap pets the
cat, shows the chat title (or a dim `new chat`) in the middle, and leaves the
model id on the footer's second line as the model-card hit box. `md.c` owns the append-only assistant display buffer: it consumes raw
markdown as it streams, holds ambiguous tails until the next chunk, and emits
visible text plus zero-width style and block-metadata commands. `view.c` owns
`RLine`, the wrapped row model behind the transcript:
paragraph wrapping, code boxes, reasoning lines, notes, tool tree rows, the
expanded-output box, `draw_slice()` for UTF-8, wide cells and markdown style
runs, the click hit-test that expands or folds a reasoning block or tool
result, and a per-message tail cursor so streaming re-wraps only the last row
plus the new bytes. `layout_reason()` owns the reasoning rail: source line
breaks, one-row paragraph gaps, list and quote hanging indents, and ATX marker
stripping. A tool row's rail state comes from `(tool_parent,
tool_pos)`, so an incremental layout never leaves a stale `╰─` on the first of
two calls.
`pet.c` owns the pet card: the ASCII cat, reaction pools, mood and stats
selection, and its responsive layout. `panel.c` draws the model picker, help
overlay, and slash-command menu, and owns the shared `panel_frame()` primitive.
`model.c` owns the model capability card: reported-only rows, the compact
`not reported:` line, the models.dev estimate footer, and its responsive
layout. `errorview.c` owns the request error window: the wrapped message,
retry/close rows, keyboard selection, and button hit-testing. `input.c` is the
composer: insert, delete, word kill, and history.

## `src/util/`

`Buf` is the growable byte buffer, with `buf_printf()` and JSON string escaping.
The file also provides allocation helpers that exit on out-of-memory,
`now_sec()` on `CLOCK_MONOTONIC`, UTF-8 decode/encode, case-insensitive
comparison, count formatting, and file helpers including atomic writes and
`mkdir_p()`.

## `tools/` and `tests/`

`tools/rulecheck.c` is the gate's CLI: it loads `MEONG_RULEBOOK` and audits a
directory. `tests/c/` holds the differential harnesses (JSON skip/reuse, fast
delta scanner, layout truncation, streaming cursor) and the layout benchmark.
`tests/test.sh` builds the client, proves the rulebook rejects a 2001-line file,
runs `tests/python/mock_paig.py` for the wire paths, runs
`tests/python/tui_test.py` as the pty test, and then makes its own tmux
`capture-pane` screen assertions for the TUI, including the tool tree, the
expanded output box, and the permission row.
