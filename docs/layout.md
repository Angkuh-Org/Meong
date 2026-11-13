# Layout

`src/` holds the linked client. `src/rules/` and `tools/` hold the host-side
rulebook gate: `build.sh` compiles them with `HOSTCC` into `build/rulecheck`
before the client, so rulebook code never enters `build/meong`.

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
stream, redraw dirty frames on a 30 ms throttle (50 ms ticks while streaming),
and animate the spinner at 10 Hz. `handle_key()` routes keys to the composer,
scrolling, the picker, or the help overlay.

## `src/app/`

`App` in `app.h` is the single mutable state object: messages, model list,
picker and help state, the active stream, the prompt queue, the scroll/layout
cache, composer text, history, and theme flags. `app.c` owns message and queue
storage and the config/cache directory rules — `MEONG_CONFIG_DIR`,
`XDG_CONFIG_HOME`, then `~/.config/meong` (and the cache equivalents).
`state.c` reads and writes the last selected model as one line in `state`.

## `src/api/`

`wire.c` parses the non-streaming reply. `extract_chat_text()` pulls
`choices[0].message.content` (or `choices[0].text`) plus usage tokens,
`extract_api_error()` digs `error.message` or `error.detail`, and
`drive_stream()` polls a stream to completion under a total timeout. `models.c`
fetches `/v1/models` (relative to the configured base), sorts and caches the
ids, and picks a default in this order: exact `DEFAULT_MODEL`, the first `cc/`
id, the first entry.

## `src/chat/`

`chat.c` selects turns newest-first within `CHAT_BUDGET` and emits them
chronologically, adding a one-line system note when older turns are dropped.
Its SSE callback reuses one `JDoc`/`Buf` pair on `App`, appends `delta.content`
to the reply and `reasoning_content`/`reasoning` to the dimmed reasoning line,
and falls back to the full parser on error or odd frames. It owns
`start_generation()`, `finish_generation()`, `cancel_generation()`, and
`drain_queue()`. `commands.c` implements `/model`, `/models`, `/system`,
`/clear`, `/retry`, `/help`, and `/quit`.

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
the request; `stream_tick()` returns `0` while running, `1` on completion, and
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
and the `ui_draw()` frame. `view.c` owns `RLine`, the wrapped row model behind
the transcript: paragraph wrapping, code fences, reasoning lines, notes,
`draw_slice()` for UTF-8 and wide cells, the click hit-test that expands or
folds a reasoning block, and a per-message tail cursor so streaming re-wraps
only the last row plus the new bytes. `panel.c` draws the model picker and help
overlays. `input.c` is the composer: insert, delete, word kill, and history.

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
`capture-pane` screen assertions for the TUI.
