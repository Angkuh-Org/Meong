# Tools

Meong's TUI exposes three **read-only** tools. They are sent only when the
selected model reports `supports_function_calling: true`; `--oneshot` does not
send a `tools` array. There is no shell, write, or delete tool.

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

## Permission model

- A path inside Meong's working directory runs immediately.
- A path outside it appends a `permission required` note and waits for `y`
  (allow) or `n` (deny). The decision applies to that call only.
- `--yolo` or `MEONG_YOLO=1` disables the prompt and allows every path.
  **This includes credential files** such as `~/.ssh/*` and `~/.paig/config.json`;
  only use it when you accept that a prompt-injected tool result could read
  them and send their contents upstream through PAIG.

## Results and limits

- Tool results are one `role:"tool"` message per call, in call order, with the
  call id and name.
- Results up to 16 KiB stay inline. Larger results keep the first 1/5 and last
  4/5 with an omission notice; the full text is written under the cache
  directory's `tool-output/` path.
- A turn stops after 8 tool rounds. Cancelling a tool chain synthesizes an
  `[interrupted by user]` result for every unanswered call id.
- Tool output is treated as untrusted data: the system policy tells the model
  not to follow instructions found inside it. That reduces, but does not
  eliminate, prompt-injection risk.

Keys and configuration: [usage.md](usage.md). Internal module map:
[layout.md](layout.md).
