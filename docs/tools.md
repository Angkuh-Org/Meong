# Tools

Meong's TUI exposes four tools: three **read-only** (`read_file`, `search`,
`list_dir`) and one mutating (`edit`). They are sent only when the selected
model reports `supports_function_calling: true`; `--oneshot` does not send a
`tools` array. There is no shell, `write`, or delete tool yet.

| Tool | Arguments | Behavior |
| --- | --- | --- |
| `read_file` | `path`, `start_line?`, `end_line?` | Metadata + range; default 400 lines |
| `search` | `pattern`, `path`, `glob?`, `context_lines?` | POSIX regex; glob; default context 2 |
| `list_dir` | `path` | Type + size, sorted |
| `edit` | `path`, `old_string`, `new_string`, `replace_all?` | Exact-unique replace; atomic; mutating |

Details:

- `read_file` accepts regular files only, rejects binary files, and reports
  size, mtime, and total lines. The line count stops at 16 MiB.
- `search` skips `.git`, `build`, and `node_modules`, never follows symlinks,
  and caps the walk at 500 files / 200 matches / depth 32. `context_lines` is
  capped at 20.
- `list_dir` caps at 500 entries.
- `edit` reads the whole regular file (≤1 MiB) and requires `old_string` to
  occur exactly once, byte-for-byte; `replace_all` opts into every occurrence.
  Empty or identical `old_string`/`new_string` are refused. It preserves every
  byte outside the match, line endings, and the file mode, and writes
  temp+`fsync`+`rename` in the same directory. It refuses a binary file, a
  final or mid-path symlink, a hard-linked file, a foreign-owned file, and a
  setuid/setgid target. The result is
  `OK: edited path:Lstart-Lend (+M -N)` (plus `xK` for `replace_all`) and a
  bounded excerpt of the edited lines; it never echoes the full arguments.

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
- **`mutate`** (`edit`): always asks, even inside the cwd. The card shows the
  path, line and a bounded `-`/`+` diff. `--allow-writes` is the explicit
  standing opt-out; it skips the card but **not** the protected list, the
  symlink/hardlink/owner checks, or the stale-content re-check.
- **`exec`** (a future allowlisted `run`): always asks, even inside the cwd.
  `--allow-exec` is the explicit opt-out; it only covers allowlisted commands,
  never a free shell.
- `--yolo` or `MEONG_YOLO=1` allows **reads outside the cwd only**. It does
  not unlock mutate or exec, and it does not read a protected path.
  `MEONG_YOLO=0`/`false` means off.

## Protected paths

These are refused before the class branch, so no flag, session grant, or
one-call approval can waive them:

- any path component `.git`, `.ssh`, `.gnupg`, `.paig`, `.aws`, `.kube`;
- `.env`/`.env.*` (except `.env.example`, `.sample`, `.template`, `.dist`),
  `authorized_keys`, `.netrc`, and the shell rc files;
- when running as root, `/data/adb`, `/data/system`, `/data/misc`, `/system`,
  `/vendor`, `/product`, `/apex`.

A denied call returns a model-facing `ERROR: ... protected path ...`; do not
retry it. A hardlink to a protected file inside the cwd is still readable;
the model cannot create one because there is no exec tool.

The card repeats the pending call with the model's raw JSON arguments for
reads, or a bounded path/line/`-`/`+` diff for `edit`. It offers `[ Allow ]`,
`[ Reject ]` and, for reads only, `[ Always allow this session ]`. It opens
with **Reject** focused, so `enter` rejects. `←→`/`tab` move the highlight,
`enter`/`space` activate it, `y`/`n` are shortcuts, and a click on a button
activates it. Allow and Reject apply to that call only. Always grants
outside-cwd **reads** for the rest of the process and is cleared by `/clear`;
it is never offered for `edit` and does not unlock writes or exec. The mutate
card grows with the preview (up to 15 rows) and shows up to three removed and
three added lines of the changed block, with `... (+K lines)` when it is
larger; short terminals show the header plus fewer lines.

The card is a confirmation, not a sandbox. `--yolo`, `--allow-writes`, and
`--allow-exec` are standing opt-outs: they carry the same credential risk as
running the action yourself. `--allow-writes` still cannot touch a protected
path or a linked/foreign-owned target; `--yolo` cannot read a protected path.
Only use them when you accept that a prompt-injected tool result could
exercise that access and send the contents upstream through PAIG.

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
- Results up to 16 KiB stay inline. Larger results keep the first 1/5 and last 4/5 with an omission notice; the full text is written under the cache directory's `tool-output/` path. This bound applies to successful results; an error is a short string.
- `edit` results are intentionally small: the success envelope plus an excerpt of the edited lines, and one-line errors for a missing/duplicate match, a stale approval, a protected path, a link, or a write failure. The full old/new arguments are never echoed into the transcript.
- A turn stops after 8 tool rounds. Cancelling a tool chain fills an `[interrupted by user]` result into every unanswered call position.
- Tool output is treated as untrusted data: the system policy tells the model not to follow instructions found inside it. That reduces, but does not eliminate, prompt-injection risk.

Keys and configuration: [usage.md](usage.md). Internal module map:
[layout.md](layout.md).
