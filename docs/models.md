# Models

Meong talks to the models PAIG exposes.
`meong --list-models` prints the full ids.

## Selection

The TUI starts from the first source that exists:

1. `--model ID`
2. `MEONG_MODEL`
3. the model saved in `state`
4. the catalogue default: the built-in default if present, else the first
   `cc/` entry, else the first entry (the built-in default when the catalogue
   is empty)

The picker opens at launch only when the first three are absent and the
catalogue is not empty. `tab` reopens it; `/model ID` switches directly.

`--oneshot` does not read the saved model or consult the catalogue: it uses
`--model`, then `MEONG_MODEL`, then the built-in default.

## State

The TUI saves the last selected model in `state`:

- `MEONG_CONFIG_DIR` overrides the directory.
- otherwise `$XDG_CONFIG_HOME/meong`, then `~/.config/meong`.

## Catalogue cache

Meong caches `/v1/models` in `models`:

- `MEONG_CACHE_DIR` overrides the directory.
- otherwise `$XDG_CACHE_HOME/meong`, then `~/.cache/meong`.

An older cache triggers one live refetch and is
rewritten when that fetch succeeds. `/models` refreshes both the catalogue and
the capability card.

## Capability card

`f2` or clicking the model on the footer's second line opens the card: context
window, output limit, vision/tools/reasoning/caching, and cost when the
gateway reports them. Fields the gateway did not report are listed as
`not reported:`;
