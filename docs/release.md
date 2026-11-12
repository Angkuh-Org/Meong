# Release

Tags matching `v*` trigger `.github/workflows/release.yml`. It builds on
`ubuntu-22.04` (x86_64) and `ubuntu-22.04-arm` (aarch64), runs the full test
suite, and publishes one binary per arch with a `.sha256` file and a build
provenance attestation.

## Assets

- `meong-linux-x86_64`
- `meong-linux-aarch64`
- matching `.sha256` files

Tags containing `-` (for example `v0.2.0-rc1`) are published as prereleases.

## Runtime requirements

- glibc 2.35 or newer. Release builds stay on Ubuntu 22.04 on purpose: a local
  build leaks newer symbols (`__isoc23_*` from glibc 2.38) and will not start
  on 22.04 or Debian 12.
- `libncursesw.so.6` and `libtinfo.so.6` (`libncursesw6`/`libtinfo6` on
  Debian/Ubuntu, `ncurses-libs` on Fedora, `ncurses` on Arch).
- A terminfo entry for `$TERM` (`ncurses-base` on Debian/Ubuntu/Fedora).
  ncurses reads `/usr/share/terminfo` at runtime; an unknown `$TERM` exits with
  `ncurses: cannot initialize terminal type`.
- Termux/Android: these are glibc binaries and will not run there. Build from
  source with `./build.sh` on the device.

ncursesw stays dynamically linked on purpose. Static ncursesw removes the
`.so` but grows the binary from ~78 KB to ~338 KB and still needs terminfo, so
the default stays small. A fully static glibc build is not shipped: its
`getaddrinfo` loads NSS modules from the host, which breaks name resolution
across distros.

## Version stamping

`--version` prints `APP_VERSION`. Release builds pass the tag through
`MEONG_VERSION`, which `build.sh` compiles into the binary; local builds keep
the `0.1.0` default from `src/app/app.h`.

```sh
MEONG_VERSION=v0.1.0 ./tests/test.sh
test "$(./build/meong --version)" = "Meong v0.1.0"
```

The workflow refuses a tag whose binary reports a different version, and fails
if the binary requires glibc newer than 2.35.

## Rehearsal

```sh
export MEONG_VERSION=v0.1.0
./build.sh
./build/meong --version
./tests/test.sh
```

The pty test in `tests/test.sh` is timing-sensitive under load; a single
failure there is not a release blocker until a rerun confirms it. The tmux
`capture-pane` assertions are the screen-level gate.
