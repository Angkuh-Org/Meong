# Tools

Meong's TUI (for now) exposes three **read-only** tools. They are sent only when the
selected model reports `supports_function_calling: true`; `--oneshot` does not
send a `tools` array. There is no (yet) shell, write, or delete tool.

| Tool | Arguments | Behavior |
| --- | --- | --- |
| `read_file` | `path`, `start_line?`, `end_line?` | Metadata + range; default 400 lines |
| `search` | `pattern`, `path`, `glob?`, `context_lines?` | POSIX regex; glob; default context 2 |
| `list_dir` | `path` | Type + size, sorted |

Details:

- `read_file` accepts regular files only, rejects binary files, and reports
  size, mtime, and total lines. The line count stops at 16 MiB.
- `search` skips `.git`, `build`, and `node_modules`, never follows symlinks,
  and caps the walk at 500 files / 200 matches / depth 32. `context_lines` is
  capped at 20.
- `list_dir` caps at 500 entries.

## How paths resolve

- Tool paths resolve against Meong's working directory.
- Meong sends its process cwd as `x-paig-workdir` on each request, so PAIG's
  free-model `<env>` block matches the directory these tools resolve against.

## Permission model

Every tool is registered with one action class — `read`, `mutate`, or `exec`.
The class is bound in the dispatch table, not chosen by a handler or by the
model's tool-name string.

- **`read`** (the three tools above): a path inside the working directory runs
  immediately; a path outside turns the call row into `? name  args` and
  replaces the composer with a permission card. `search` asks on its root
  argument — the path the card shows — and one approval covers that call.
- **`mutate`** (a future `edit`/`write`): always asks, even inside the cwd.
  `--allow-writes` is the explicit opt-out.
- **`exec`** (a future allowlisted `run`): always asks, even inside the cwd.
  `--allow-exec` is the explicit opt-out; it only covers allowlisted commands,
  never a free shell.
- `--yolo` or `MEONG_YOLO=1` allows **reads outside the cwd only**. It does
  not unlock mutate or exec. `MEONG_YOLO=0`/`false` means off.

The card repeats the pending call with the model's raw JSON arguments (or the
command/preview for a future non-read class) and offers `[ Allow ]`,
`[ Reject ]` and, for reads only, `[ Always allow this session ]`. It opens
with **Reject** focused, so `enter` rejects. `←→`/`tab` move the highlight,
`enter`/`space` activate it, `y`/`n`/`a` are shortcuts, and a click on a
button activates it. Allow and Reject apply to that call only. Always grants
outside-cwd **reads** for the rest of the process and is cleared by `/clear`;
it does not unlock writes or exec.

The card is a confirmation, not a sandbox. `--yolo`, `--allow-writes`, and
`--allow-exec` are standing opt-outs: they carry the same credential risk as
running the action yourself. `--allow-writes` can write anywhere your user
account can (including `~/.ssh/*` and shell rc files); `--yolo` can read
`~/.ssh/*` and `~/.paig/config.json`. Only use them when you accept that a
prompt-injected tool result could exercise that access and send the contents
upstream through PAIG.

## Display

Tool calls hang under the assistant turn as a tree: `├─` marks a call with a
sibling after it, `╰─` the last call. A row carries the status glyph (`✓` ok, `✗` error, `?` permission), the tool name, a shortened argument summary, and flat metadata on the right. 

Results are collapsed by default; click the row or press `ctrl-o` to open the output in a box, and click
again to fold.

An error previews its first line on the row without opening the
box. Arguments are shortened for display only: the request keeps the exact
JSON from the model, and the stored `role:"tool"` text is only the handler
result, never the tree or metadata.

## Results and limits

- Tool results are one `role:"tool"` message per call, in call order, with the call id and name.
- Results up to 16 KiB stay inline. Larger results keep the first 1/5 and last 4/5 with an omission notice; the full text is written under the cache directory's `tool-output/` path.
- A turn stops after 8 tool rounds. Cancelling a tool chain fills an `[interrupted by user]` result into every unanswered call position.
- Tool output is treated as untrusted data: the system policy tells the model not to follow instructions found inside it. That reduces, but does not eliminate, prompt-injection risk.

Keys and configuration: [usage.md](usage.md). Internal module map:
[layout.md](layout.md).
