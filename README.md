# Meong

A small terminal cat that talks to AI through [PAIG](https://github.com/Angkuh-Org/PAIG).
C11 + ncursesw, no libcurl and no TLS: it speaks plain HTTP/1.1 to the local gateway, decodes
chunked SSE itself, and streams replies into a scrolling chat.

```
  /\_/\
 ( o.o )   Meong
  > ^ <
```

## Build

Requires `cc`, `ncursesw` headers, and `pkg-config` (optional; the build falls
back to `-lncursesw`).

```sh
./build.sh              # -> build/meong
./build.sh install      # -> $PREFIX/bin/meong (PREFIX=/usr/local by default)
./tests/test.sh         # offline mock + pty/tmux TUI tests
```

The binary is ~80 KB stripped with lld; GNU bfd defaults to 64K
max-page-size on aarch64 and links ~133 KB.

Release binaries are built on Ubuntu 22.04 and need glibc 2.35+,
`libncursesw.so.6`/`libtinfo.so.6`, and a terminfo entry (`ncurses-base`).

## Development

Module map and where new code belongs: [`docs/layout.md`](docs/layout.md).
Release and prebuilt requirements: [`docs/release.md`](docs/release.md).

## Run

```sh
./build/meong             # opens the TUI
./build/meong --oneshot 'hello' --model cc/deepseek/deepseek-v4.1-flash
./build/meong --stream --oneshot 'hello' --model cc/deepseek/deepseek-v4.1-flash
./build/meong --list-models
```

`--oneshot` prints the reply and exits; add `--stream` to print tokens as they
arrive. `--list-models` prints one prefixed model id per line.

On first launch Meong fetches and caches the model list, then opens the model
picker. The last selected model is remembered in
`$XDG_CONFIG_HOME/meong/state`. Model ids must keep their PAIG backend prefix
(`cc/…`, `opencode/…`, `kilo/…`, `bob/…`, `mistral/…`); the picker handles that
for you.

## Keys

| Key | Action |
| --- | --- |
| `enter` | send; queue the prompt while a reply streams |
| `tab` | model picker (type to filter) |
| `pgup` / `pgdn` | scroll the conversation |
| `ctrl-g` / `ctrl-t` | jump to bottom / top |
| `ctrl-c` | cancel a reply; quit when idle |
| `ctrl-d` | quit on empty input |
| `ctrl-l` | redraw |
| `up` / `down` | input history |
| mouse wheel | scroll |

## Commands

| Command | Action |
| --- | --- |
| `/model [id]` | open the picker, or switch directly |
| `/models` | refresh the gateway model list |
| `/system [p]` | show, set, or clear (`off`) the system prompt |
| `/clear` | forget the conversation |
| `/retry` | regenerate the last answer |
| `/help` | help overlay |
| `/quit` | leave |

## Configuration

| Variable | Purpose |
| --- | --- |
| `MEONG_URL` / `PAIG_URL` | gateway base, default `http://127.0.0.1:8899` |
| `PAIG_HOST` / `PAIG_PORT` | used when no URL is set |
| `MEONG_MODEL` | initial model id |
| `MEONG_CONFIG_DIR` | overrides the state directory |
| `MEONG_CACHE_DIR` | overrides the model cache directory |
| `MEONG_ASCII` | force ASCII borders and symbols |
| `MEONG_NO_MOUSE` | disable mouse capture |
| `NO_COLOR` | disable color |
| `MEONG_WIRE_DEBUG` | dump parsed HTTP/SSE lines to stderr |

The gateway does not authenticate inbound requests. Keep PAIG on loopback.

## Notes

- Transport is a hand-rolled nonblocking HTTP/1.1 client, one connection per
  request. It handles chunked transfer, SSE `data:` frames, `[DONE]`, and plain
  JSON error bodies that arrive without SSE framing.
- `reasoning_content` / `reasoning` deltas are shown dimmed above the answer.
- Requests keep the newest ~200 KB of user/assistant text. Older turns are
  dropped, replaced by a one-line system note, and the footer shows `trimmed`;
  the visible transcript keeps everything until `/clear`.
- Code fences are rendered in a separate color; input supports UTF-8 and wide
  characters.
- Vendored [`jsmn`](src/json/jsmn.h) tokenizes JSON; Meong's wrapper
  handles unescaping, surrogate pairs, and traversal.

## License

GPL-3.0-only. The vendored [`jsmn`](src/json/jsmn.h) is MIT.
