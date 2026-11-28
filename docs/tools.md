# Tools

Meong's TUI exposes six tools: four **read-only** (`read_file`, `search`,
`list_dir`, `glob`) and two mutating (`edit`, `write`). They are sent only when
the selected model reports `supports_function_calling: true`; `--oneshot` does
not send a `tools` array. There is no shell or delete tool yet.

**Status 2026-11-28: Phase 7A A9-lite ships in v0.21.0; both release
legs passed 108/108.** Phase 5 `glob` ships in v0.20.0; live gate PASS. Phase 4 remains the
mutation result on the old catalog: v3.2 PASS (safety **60/60**, integrity
**60/60**, capability **58/60**, CP lower **0.8988**); the old v2 arm-a5 run
remains a preserved **FAIL as-run** and is not re-scored. The mutation code is
unchanged. A9-lite is read-only metadata: no new tool, no new argument, no new
permission class, and `TOOLS_JSON` is unchanged at **3,538 B**
(`6b35fba3…`). Local build **208,360 B**; suite **108/108**; the A9-lite
header fields and cwd line are covered by the tools test and two shell
checks. A 2026-11-28 codegen pass replaced the per-line EOL memchr passes
with a 16-byte-lane bulk counter after the requested range, so a 16 MiB
LF read is 2.1× faster than the first A9-lite cut and 2.5% faster than
`v0.20.0`. Release assets: aarch64 **205,080 B**, x86_64 **215,224 B**,
both GLIBC 2.34; v0.21.0 is latest.
`MEONG_NO_MUTATE=1` / `--no-mutate` refuses both mutating tools for the run.

| Tool | Arguments | Behavior |
| --- | --- | --- |
| `read_file` | `path`, `start_line?`, `end_line?` | Metadata + range; default 400 lines; mode/EOL/range bytes/longest line |
| `search` | `pattern`, `path`, `glob?`, `context_lines?` | POSIX regex; glob; default context 2 |
| `list_dir` | `path` | Type + size, sorted |
| `glob` | `pattern`, `path` | Filename match; `**` recursive; files only; sorted |
| `edit` | `path`, `old_string`, `new_string`, `replace_all?` | Exact-unique replace; atomic; mutating |
| `write` | `path`, `content`, `overwrite?` | Create-only default; atomic; mutating |

Details:

- `read_file` accepts regular files only, rejects binary files (a NUL in the
  first 4 KiB), and reports size, mtime, total lines, **mode**, and **EOL**.
  Mode is `%04o` of the opened file's `st_mode` (`0644`, `4755`, `1777`).
  EOL is `lf`, `crlf`, `cr`, `mixed`, `none`, or `binary`, classified over the
  whole scanned window (up to 16 MiB); a trailing `+` means the window did not
  cover the file. When a line range is selected, the header also prints
  `range_bytes` (raw bytes of the selected lines, when different from the file
  size) and `max_line` (longest selected line, when over 200 bytes). Both are
  range-local; EOL is a file/window property. The header keeps
  `[file: <path-as-given>` first; control characters, `|`, `]`, and `\` in a
  printed path are escaped.
- The tool-on system message carries `Working directory: <abs>` once per
  request. `read_file` does not repeat cwd in every result. The value comes
  from `getcwd`, is canonicalised at startup, and is escaped for display.
- `search` skips `.git`, `build`, and `node_modules`, never follows symlinks,
  and caps the content scan at 500 opened files / 200 matches / depth 32.
  `context_lines` is capped at 20. Its `glob` argument is a basename filter
  on content results; it is not the `glob` tool.
- `list_dir` caps at 500 entries.
- `glob` matches file names without reading contents. A pattern without `/`
  matches the basename at any depth (`*.c`); with `/` it is relative to
  `path` (`src/*.c`, `src/**/*.c`). `**` crosses directories; `*`, `?`, `[]`
  do not. `./` anchors at the root. Case-sensitive; braces are not expanded.
  It skips `.git`, `build`, `node_modules`, never follows or lists symlinks,
  and never emits a protected name. Caps: **50,000 visited entries**, **200
  results**, depth 32, 64 KiB output. A regular-file `path` is matched
  directly. Empty results say `[no matches]`; an incomplete walk names the
  cap. The matcher is `src/tools/glob.c`.
- `edit` reads the whole regular file (≤1 MiB) and requires `old_string` to
  occur exactly once, byte-for-byte; `replace_all` opts into every occurrence.
  Empty or identical `old_string`/`new_string` are refused. It preserves every
  byte outside the match, line endings, and the file mode, and writes
  temp+`fsync`+`rename` in the same directory. It refuses a binary file, a
  final or mid-path symlink, a hard-linked file, a foreign-owned file, and a
  setuid/setgid target. The result is
  `OK: edited path:Lstart-Lend (+M -N)` (plus `xK` for `replace_all`) and a
  bounded excerpt of the edited lines; it never echoes the full arguments.
- `write` creates a new text file by default. If the target exists, it is
  refused until `overwrite:true` is passed; an existing file is never replaced
  silently. The parent directory must already exist. `content` is limited to
  24 KiB and must not contain NUL; because the 64 KiB argument cap applies to
  the raw JSON, a payload full of escapes (control characters) can hit the
  wire limit before 24 KiB. Create writes a 0600 temp, sets the final
  mode to `0666 & ~umask`, `fsync`s, and publishes with
  `renameat2(RENAME_NOREPLACE)` (a `linkat` fallback only when the kernel or
  filesystem lacks the flag); it never falls back to a replacing `rename`.
  Overwrite preserves the target mode and re-checks the full-file identity,
  then writes temp+`fsync`+`rename`. A final or mid-path symlink, a directory,
  a hard-linked file, a foreign-owned file, a setuid/setgid target, a file on a
  different filesystem, a binary target, and a target over 1 MiB are refused.
  Identical bytes return `OK: unchanged` after approval without replacing the
  inode. The result is `OK: created path (N B)`, `OK: overwrote path (M -> N B)`,
  or `OK: unchanged path (N B)`; content is never echoed.

## How paths resolve

- Tool paths resolve against Meong's working directory.
- When tools are on, the system message carries `Working directory: <abs>`
  once per request. `read_file` keeps the path as the model wrote it, so a
  relative path is still short; the cwd line supplies the base.
- Printed paths escape `\n`, `\r`, `|`, `]`, `\`, and C0/DEL. A filename
  cannot forge a header field or a new result line.
- Meong sends its process cwd as `x-paig-workdir` on each request, so PAIG's
  free-model `<env>` block matches the directory these tools resolve against.
  PAIG may also print a `Working directory:` line; Meong's line is independent
  and is the one the tool contract relies on.

## Permission model

Every tool is registered with one action class — `read`, `mutate`, or `exec`.
The class is bound in the dispatch table, not chosen by a handler or by the
model's tool-name string.

- **`read`** (the four tools above): a path inside the working directory runs
  immediately; a path outside turns the call row into `? name  args` and
  replaces the composer with a permission card. `search` asks on its root
  argument — the path the card shows — and one approval covers that call.
- **`mutate`** (`edit`, `write`): always asks, even inside the cwd. The edit
  card shows the path, line, the old/new `old_string`/`new_string` byte counts
  and a bounded `-`/`+` diff; a line that fills the preview buffer is followed
  by `... (line truncated)`, and tabs render as four spaces. The write card
  shows the resolved path, the create or overwrite verb, the byte/line counts,
  and a sanitized sample. `--allow-writes` is the explicit standing opt-out; it
  skips the card but **not** the protected list, the
  symlink/hardlink/owner checks, the stale-content re-check, or the
  no-replace create primitive. `MEONG_NO_MUTATE=1` or `--no-mutate` is a hard
  deny for the class: it outranks `--allow-writes`, a session mutate grant, and
  a one-call approval. A protected path still reports its specific refusal.
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
reads, a bounded path/line/`-`/`+` diff for `edit`, or a
`create`/`overwrite` path + size + sample preview for `write`. It offers
`[ Allow ]`,
`[ Reject ]` and, for reads only, `[ Always allow this session ]`. It opens
with **Reject** focused, so `enter` rejects. `←→`/`tab` move the highlight,
`enter`/`space` activate it, `y`/`n` are shortcuts, and a click on a button
activates it. Allow and Reject apply to that call only. Always grants
outside-cwd **reads** for the rest of the process and is cleared by `/clear`;
it is never offered for `edit` or `write` and does not unlock writes or exec.
The mutate card grows with the preview (up to 15 rows): `edit` shows up to
three removed and three added lines of the changed block, with
`... (+K lines)` when it is larger; `write` shows the first three and last
content line, with `... K lines not shown` between them. Short terminals show
the header plus fewer lines.

The card is a confirmation, not a sandbox. `--yolo`, `--allow-writes`, and
`--allow-exec` are standing opt-outs: they carry the same credential risk as
running the action yourself. `--allow-writes` still cannot touch a protected
path or a linked/foreign-owned target; `--yolo` cannot read a protected path.
`MEONG_NO_MUTATE=1` / `--no-mutate` is the field kill switch: it refuses
`edit` and `write` for the whole run and outranks every allow bit.
Only use them when you accept that a prompt-injected tool result could
exercise that access and send the contents upstream through PAIG.

**Metadata is not a security check.** `mode`, `eol`, `range_bytes`, and
`max_line` describe what was opened; they do not grant access, prove the file
is safe, or replace the protected-path and permission checks.

## Display

Tool calls hang under the assistant turn as a tree: `├─` marks a call with a
sibling after it, `╰─` the last call. A row carries the status glyph (`✓` ok, `✗` error, `?` permission), the tool name, a shortened argument summary, and flat metadata on the right. 

Results are collapsed by default; click the row or press `ctrl-o` to open the output in a box, and click
again to fold.

An error previews its first line on the row without opening the
box. Arguments are shortened for display only: the request keeps the exact
JSON from the model, and the stored `role:"tool"` text is only the handler
result, never the tree or metadata.

The `read_file` metadata header is one logical line and grows with the
optional `range_bytes`/`max_line` fields. In a narrow output box it can wrap
or clip; the model still receives the full line. The A9-lite canary is the
40×20 ASCII box test, not a promise that every path length fits.

## Results and limits

- Tool results are one `role:"tool"` message per call, in call order, with the call id and name.
- `read_file` results start with `[file: <path-as-given> | bytes | mtime |
  lines | mode | eol]`; a selected range may add `range_bytes` and `max_line`.
  The path is the model's spelling, not a resolved absolute path.
- Results up to 16 KiB stay inline. Larger results keep the first 1/5 and last 4/5 with an omission notice; the full text is written under the cache directory's `tool-output/` path. This bound applies to successful results; an error is a short string.
- `edit` results are intentionally small: the success envelope plus an excerpt of the edited lines, and one-line errors for a missing/duplicate match, a stale approval, a protected path, a link, or a write failure. The full old/new arguments are never echoed into the transcript.
- `write` results are one short line: `OK: created path (N B)`, `OK: overwrote path (M -> N B)`, or `OK: unchanged path (N B)`. Errors name the path and the reason (already exists, missing parent, directory, symlink, hardlink, owner, binary, cap, NUL, disk, stale). Content is never echoed.
- `glob` results are one path per line in walk spelling, byte-wise sorted; 200 results normally stay under the 16 KiB inline cap. The result is names only, never content. Incomplete and result-cap markers are part of the result text; do not read a capped list as complete.
- A turn stops after 8 tool rounds. Cancelling a tool chain fills an `[interrupted by user]` result into every unanswered call position.
- Tool output is treated as untrusted data: the system policy tells the model not to follow instructions found inside it. That reduces, but does not eliminate, prompt-injection risk.

Keys and configuration: [usage.md](usage.md). Internal module map:
[layout.md](layout.md).
