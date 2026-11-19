# Usage

Meong is a terminal UI; the composer is the only input. Press `?` for the
in-app reference.

## Keys

| Key | Action |
| --- | --- |
| `enter` | send; queue the prompt while a reply streams |
| `/` | command menu: type to filter, `up`/`down` to focus, `enter`/`tab` to complete, `esc` to close |
| `?` | open help when the composer is empty |
| `tab` | model picker (type to filter) |
| `pgup` / `pgdn` | scroll the conversation |
| `ctrl-g` / `ctrl-t` | jump to bottom / top |
| `ctrl-c` | cancel a reply; quit when idle |
| `ctrl-d` | quit on empty input |
| `ctrl-l` | redraw |
| `ctrl-o` | fold the last collapsible block (reasoning or tool) |
| `f1` / click `Meong` | pet the cat (live cat card) |
| `f2` / click the model | show the model capability card |
| `up` / `down` | input history |
| mouse wheel | scroll |
| click / tap thinking or a tool row | expand or fold it |

The pet card reacts, counts pets, and shows the live model, message and token
totals, and mood. Typing closes it and still reaches the composer.

`reasoning_content` / `reasoning` deltas are shown dimmed above the answer
while streaming, then fold when the reply finishes; a cancelled reply stays
open. Click or tap the thinking line — or any reasoning line — to expand or
fold it, or press `ctrl-o` for the last collapsible block.

## Request failures

When a request fails, Meong opens a `request failed` window with two choices.
`retry` replays the request that failed; `close` dismisses the window. The
failure lives only in the window, so no error row is added to the transcript.
Use `up`/`down` (or `tab`) to choose, `enter` to activate, `r` to retry,
`c`/`esc` to close, or click a row.

## Commands

Type `/` in the composer to list these in a filterable menu; `enter` or `tab`
completes the focused command, and an already-complete command runs on the
first `enter`.

| Command | Action |
| --- | --- |
| `/model [id]` | open the picker, or switch directly |
| `/models` | refresh the gateway model list |
| `/system [p]` | show, set, or clear (`off`) the system prompt |
| `/clear` | forget the conversation |
| `/retry` | regenerate the last answer |
| `/help` | help overlay |
| `/quit` | leave |

## Tools

Read-only tools render as a tree under the assistant; rows show status,
shortened arguments, and duration/size, and open into a box. Permission,
display details, and output bounds: [`tools.md`](tools.md).

## Configuration

| Variable | Purpose |
| --- | --- |
| `MEONG_URL` / `PAIG_URL` | gateway base, default `http://127.0.0.1:8899` |
| `PAIG_HOST` / `PAIG_PORT` | used when no URL is set |
| `MEONG_MODEL` | initial model id |
| `MEONG_CONFIG_DIR` | overrides the config directory |
| `MEONG_CACHE_DIR` | overrides the cache directory |
| `MEONG_ASCII` | force ASCII borders and symbols |
| `MEONG_NO_NERD` | hide the Nerd Font cat and permission shield |
| `MEONG_NO_MOUSE` | disable mouse and touch input |
| `MEONG_YOLO` | allow tools outside the working directory |
| `NO_COLOR` | disable color |
| `MEONG_WIRE_DEBUG` | dump parsed HTTP/SSE lines to stderr |

The gateway does not authenticate inbound requests. Keep PAIG on loopback.
Model state and cache paths: [`models.md`](models.md).

## Rendering

- Assistant replies are rendered as markdown: `**bold**`, `*italic*`,
  `` `inline code` ``, `~~strike~~`, links (the label is shown, the URL is
  hidden), ATX headings, bullet/ordered/task lists, blockquotes, tables, and
  fenced code as a box.  The raw markdown stays in the message sent back to
  the gateway; only the display buffer is transformed.
- With a Nerd Font, the header draws `nf-fa-cat` (U+EEED) before `Meong`;
  `--no-nerd` or `MEONG_NO_NERD` drops just those glyphs, while `--ascii`
  drops all non-ASCII chrome. The middle of the header is the chat title,
  generated once per chat from the first prompt by one extra request to the
  current model; before it arrives the slot reads `new chat`.
- Input supports UTF-8 and wide characters.
- Reasoning rows use the `│` rail; the last row closes with `╰─`.

## Context and footer

- Requests keep the newest ~200 KB of user/assistant text. Older turns are
  dropped, replaced by a one-line system note, and the footer shows `trimmed`;
  the visible transcript keeps everything until `/clear`.
- The footer is two lines: the hint with `ctx N%` and `cache N%` on the first,
  the full model id on the second. Clicking the model opens the capability
  card, same as `f2`. `cache -` means the gateway omitted
  `usage.prompt_tokens_details.cached_tokens`; `cache 0%` means it reported a
  real zero.
- Each finished turn prints one `N tok | Xs` line at the bottom of its final
  response. Tokens and elapsed time are summed across the thinking and tool
  rounds of that turn; intermediate responses do not print their own line.
