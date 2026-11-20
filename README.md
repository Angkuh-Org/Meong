<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset=".github/assets/meong-dark.png">
    <img src=".github/assets/meong-light.png" alt="Meong" width="420">
  </picture>
</p>

<p align="center">A very fast, clean, and small agent harness like a cat.</p>

<p align="center">
  <a href="https://github.com/Angkuh-Org/Meong/releases/latest"><img alt="release" src="https://img.shields.io/github/v/release/Angkuh-Org/Meong?style=flat-square&color=C5FC08"></a>
  <a href="https://github.com/Angkuh-Org/Meong/actions/workflows/ci.yml?query=branch%3Amain"><img alt="ci" src="https://img.shields.io/github/actions/workflow/status/Angkuh-Org/Meong/ci.yml?branch=main&style=flat-square&label=ci"></a>
  <a href="LICENSE"><img alt="license" src="https://img.shields.io/github/license/Angkuh-Org/Meong?style=flat-square&color=C5FC08"></a>
  <img alt="platform" src="https://img.shields.io/badge/platform-linux%20%C2%B7%20aarch64%20%2B%20x86__64-C5FC08?style=flat-square">
  <img alt="binary size" src="https://img.shields.io/badge/binary-~150%20KB-C5FC08?style=flat-square">
</p>

<p align="center">
  <a href="https://github.com/Angkuh-Org/Meong/blob/main/.github/assets/home.png"><img src=".github/assets/home.png" alt="Meong TUI" width="100%"></a>
</p>

## Install

```sh
curl -fsSL https://raw.githubusercontent.com/Angkuh-Org/Meong/main/install.sh | sh
```

The installer verifies the release SHA-256, probes the binary, and installs to
`~/.local/bin` (`/usr/local/bin` as root). Options: [`docs/install.md`](docs/install.md).

## Run

Currently Meong need [PAIG](https://github.com/Angkuh-Org/PAIG) to run;
```sh
meong                                          # open the TUI
meong --oneshot 'hello' \
  --model cc/deepseek/deepseek-v4.1-flash      # one reply, then exit
meong --stream --oneshot 'hello' \
  --model cc/deepseek/deepseek-v4.1-flash      # print tokens as they arrive
meong --effort high                            # thinking effort: off..max
meong --list-models                            # one prefixed model id per line
```

Selection order, saved state, tools and the catalogue cache:
[`docs/models.md`](docs/models.md).
[`docs/tools.md`](docs/tools.md).

## Build

Requires `cc` and `ncursesw` headers.

```sh
./build.sh              # -> build/meong
./build.sh install      # -> $PREFIX/bin/meong (PREFIX=/usr/local by default)
./build.sh help         # commands, environment, examples
./tests/test.sh         # offline mock + pty/tmux TUI tests
```

## Docs

- [Install](docs/install.md) — installer options and PATH.
- [Usage](docs/usage.md) — keys, commands, configuration, behavior.
- [Models](docs/models.md) — selection, state, cache, capability card.
- [Tools](docs/tools.md) — tool classes, permission prompt, output bounds.
- [Performance method](docs/perf.md) — benchmark method and raw numbers.
- [Layout](docs/layout.md) — module map.
- [Release](docs/release.md) — assets and runtime requirements.

## Performance

v0.2.1 removes the per-event JSON allocations, re-wraps only the streaming
tail, and parses the common SSE frame without jsmn:

![Meong v0.2.0-rc1 vs v0.2.1 benchmark](docs/images/profile-v0.2.0-rc1-vs-v0.2.1.png)

- streaming CPU: 52% fewer instructions, 2.09× less work per 1 MB
- streaming wall clock: 33% less time, 1.49× faster per 8 MB (median of 9
  interleaved pairs)
- forced layout, 3,125 × 64 B appends on 200 KB: 99.2% less work, 123× faster
- binary: 79,496 B → 86,568 B (+7,072 B)

Method and raw numbers:
[`docs/perf.md`](docs/perf.md).

## License

GPL-3.0-only. The vendored [`jsmn`](src/json/jsmn.h) is MIT.
