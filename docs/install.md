# Install

Release binaries are built on Ubuntu 22.04 for Linux `x86_64` and `aarch64`.
Both need glibc 2.35 or newer, `libncursesw.so.6`/`libtinfo.so.6`, and a
terminfo entry for `$TERM`.

## One-liner

```sh
curl -fsSL https://raw.githubusercontent.com/Angkuh-Org/Meong/main/install.sh | sh
```

Expected output on a terminal (version and size depend on the latest release):

```text
  M E O N G  ·  probably a cat
  ────────────────────────────────────
  linux / x86_64 · latest

  ✔ download  meong-linux-x86_64 (103.4 KiB)
  ✔ checksum  sha256 verified
  ✔ verify    Meong v0.6.1
  ✔ install   /home/you/.local/bin/meong

  next  meong
```

When stdout is not a terminal (a pipe or CI), the installer prints plain lines
instead: `downloading ...`, the `sha256sum` result, and `installed ...`.

The installer verifies the release asset against its published SHA-256, runs
`--version` on the downloaded binary, and only then replaces the installed
copy. It installs to `~/.local/bin` (or `/usr/local/bin` when run as root). If
that directory is not on `PATH`, it prints the `export` line to add.

## Options

Pin a version:

```sh
curl -fsSL https://raw.githubusercontent.com/Angkuh-Org/Meong/main/install.sh | sh -s -- v0.6.1
```

Choose the install directory:

```sh
curl -fsSL https://raw.githubusercontent.com/Angkuh-Org/Meong/main/install.sh \
  | MEONG_INSTALL_DIR=/opt/meong/bin sh
```

`MEONG_VERSION` is the environment-variable form of the version argument;
note that `build.sh` and `release.yml` also use `MEONG_VERSION` as the
compile-time stamp, so unset it (or override it per command) when installing a
version other than the one you last built.

`MEONG_REPO` overrides the repository (for forks). `MEONG_DOWNLOAD_BASE`
replaces the GitHub release URL for mirrors; `http://` is accepted only
through that override.

## Runtime requirements

The release binaries are glibc builds: glibc 2.35+, `libncursesw.so.6`/
`libtinfo.so.6`, and a terminfo entry for `$TERM`. Per-distro package names and
the static-link rationale live in [`release.md`](release.md).

Termux/Android is refused: these are glibc binaries. Build from source with
`./build.sh` on the device instead.

The installer probes the downloaded binary with `--version` before touching the
installed copy, so a missing loader dependency leaves an existing `meong` in
place. A missing terminfo entry still fails at first launch, not at install.

## Manual install

```sh
version=v0.6.1
arch=x86_64   # or aarch64
base=https://github.com/Angkuh-Org/Meong/releases/download/$version
curl -fLO "$base/meong-linux-$arch"
curl -fLO "$base/meong-linux-$arch.sha256"
sha256sum -c "meong-linux-$arch.sha256"
install -m 0755 "meong-linux-$arch" ~/.local/bin/meong
```

## Verify provenance

Every release binary has a GitHub build-provenance attestation. After
downloading it:

```sh
gh attestation verify meong-linux-x86_64 --repo Angkuh-Org/Meong
```

This needs `gh` 2.49 or newer; older releases do not include the `attestation`
command.

## Uninstall

```sh
rm ~/.local/bin/meong
```

To build from source instead, see the README quick start or
[`release.md`](release.md).
