#!/usr/bin/env python3
"""Check that a Meong panel is drawn on one solid background.

Takes `tmux capture-pane -e -p` output as argv[1] (or on stdin). SGR state is
tracked across lines because tmux only emits an escape when it changes.

Every row inside the panel must have a single interior background; at most one
alternate row background is allowed for the selected model row. Border cells
must share the dominant row background. This catches a base-coloured hole in a
surface-coloured panel, not just holes that fall back to the terminal default.
"""
import re
import sys
import unicodedata

SGR = re.compile(r"\x1b\[[0-9;]*m")


def cell_width(ch):
    if unicodedata.combining(ch):
        return 0
    if unicodedata.east_asian_width(ch) in ("W", "F"):
        return 2
    return 1


def screen(text):
    rows = [[]]
    bg = None
    i = 0
    while i < len(text):
        m = SGR.match(text, i)
        if m:
            params = m.group(0)[2:-1].split(";")
            j = 0
            while j < len(params):
                p = params[j]
                if p in ("0", "49"):
                    bg = None
                elif p == "48" and j + 2 < len(params) and params[j + 1] == "5":
                    bg = int(params[j + 2])
                    j += 2
                elif p == "40":
                    bg = 0
                elif p.isdigit() and 41 <= int(p) <= 47:
                    bg = int(p) - 40
                elif p.isdigit() and 100 <= int(p) <= 107:
                    bg = int(p) - 100 + 8
                j += 1
            i = m.end()
            continue
        ch = text[i]
        if ch == "\n":
            rows.append([])
        elif ch != "\r":
            width = cell_width(ch)
            if width:
                rows[-1].append((ch, bg))
                if width == 2:
                    rows[-1].append((" ", bg))
        i += 1
    return rows


def main():
    if len(sys.argv) > 1 and sys.argv[1] != "-":
        text = open(sys.argv[1], encoding="utf-8").read()
        label = sys.argv[2] if len(sys.argv) > 2 else sys.argv[1]
    else:
        text = sys.stdin.read()
        label = sys.argv[2] if len(sys.argv) > 2 else "panel"
    rows = screen(text)
    lines = ["".join(ch for ch, _ in row) for row in rows]

    top = None
    for y, line in enumerate(lines):
        if (
            "choose a model" in line
            or "Meong | help" in line
            or "request failed" in line
        ):
            top = y
            break
        if "/\\_/\\" in line and y > 0:
            prev = lines[y - 1]
            if "┌" in prev or "╭" in prev or "+" in prev:
                top = y - 1
                break
    if top is None or top < 0:
        sys.exit(f"{label}: panel top border not found")
    border = lines[top]
    if "┌" in border:
        left, right = "┌", "┐"
    elif "╭" in border:
        left, right = "╭", "╮"
    else:
        left, right = "+", "+"
    px = border.index(left)
    w = border.index(right, px + 1) - px + 1

    bottom = None
    for y in range(top + 1, len(rows)):
        if len(rows[y]) > px and rows[y][px][0] in "└╰+":
            bottom = y
    if bottom is None:
        sys.exit(f"{label}: panel bottom border not found")

    row_bgs = []
    for y in range(top, bottom + 1):
        row = rows[y]
        if len(row) < px + w - 1:
            sys.exit(f"{label}: row {y} ends before the panel border")
        left = row[px][1]
        right = row[px + w - 1][1]
        if left is None or right is None:
            sys.exit(f"{label}: border has default background on row {y}")
        if left != right:
            sys.exit(f"{label}: border backgrounds differ on row {y}")
        inner = {row[x][1] for x in range(px + 1, px + w - 1)}
        if None in inner:
            sys.exit(f"{label}: interior has default background on row {y}")
        if len(inner) != 1:
            sys.exit(f"{label}: mixed backgrounds on row {y}: {sorted(inner)}")
        row_bgs.append(inner.pop())

    common = max(set(row_bgs), key=row_bgs.count)
    extra = {bg for bg in row_bgs if bg != common}
    if len(extra) > 1:
        sys.exit(f"{label}: too many row backgrounds: {sorted(set(row_bgs))}")
    if extra:
        alt = extra.pop()
        for y, bg in zip(range(top, bottom + 1), row_bgs):
            if bg == alt and not any(m in lines[y] for m in "›>"):
                sys.exit(f"{label}: unexpected background {bg} on row {y}")

    for y in range(top, bottom + 1):
        row = rows[y]
        for x in (px, px + w - 1):
            if row[x][1] != common:
                sys.exit(
                    f"{label}: border background {row[x][1]} on row {y} "
                    f"(expected {common})"
                )

    print(f"{label}: {bottom - top + 1} rows, solid background")


if __name__ == "__main__":
    main()
