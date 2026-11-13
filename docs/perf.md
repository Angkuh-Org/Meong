# Performance

Two passes so far. v0.2.1 removes the per-event JSON allocations, re-wraps only
the streaming tail, and parses the common SSE frame without jsmn.

![Meong v0.2.0-rc1 vs v0.2.1 benchmark](images/profile-v0.2.0-rc1-vs-v0.2.1.png)

## v0.2.1 — allocator reuse, tail layout, fast delta scanner

| Workload | v0.2.0-rc1 | v0.2.1 | Delta |
| --- | --- | --- | --- |
| Instructions / 1 MB SSE (callgrind) | 65.4 M | 31.3 M | 52% fewer, 2.09× less work |
| Wall time / 8 MB SSE (9 interleaved pairs) | 218 ms | 146 ms | 33% less time, 1.49× faster |
| Per-event free() entries (JSON/Buf) / 1 MB SSE | 49,009 | 2 | per-event frees gone |
| Forced layout, 200 KB single line (3,125 × 64 B) | 1,048 ms | 8.5 ms | 99.2% less, 123× faster |
| Forced layout, 200 KB mixed text (3,125 × 64 B) | 1,433 ms | 8.5 ms | 99.4% less, 168× faster |
| SSE frames bypassing jsmn | 0 | 16,385 | all frames take the fast path |
| Binary size | 79,496 B | 86,568 B | +7,072 B (+8.9%) |

- The fast scanner handles all 16,385 frames in the 1 MB run (16,384 deltas plus
  the usage chunk). The full parser remains the fallback for errors, odd shapes
  and malformed input.
- Layout numbers are 3,125 forced appends of 64 B into a 200 KB message, median
  of 5 runs. The harness calls `layout_build` on every append to isolate the
  rewrap cost; the live client throttles streaming rebuilds to ~11/s, so this is
  a frame-time bound, not stream duration. The incremental rows are compared
  field-for-field against a full rebuild at every append
  (`tests/c/layout_stream_test.c`).
- Stream output is byte-identical to the baseline on the 64 KB benchmark stream
  and on `mock/split` (split UTF-8, code fence, reasoning).

### What changed

- `JDoc` keeps its token+skip block across events; `App` and `CliStream` reuse
  one `JDoc` and one `Buf`. Per-event allocator calls drop to zero.
- `layout_build` checkpoints the last wrapped row and re-wraps only that row
  plus the new bytes. Width changes and reason growth invalidate the cursor.
- `jdelta_scan` walks the OpenAI delta/usage shape directly and returns -1 on
  anything it cannot prove; the caller then runs the old jsmn path. A 10,000
  random-byte differential fuzz backs the fallback contract.
- `start_generation` and `msgs_clear` reset `layout_from`, so rows for messages
  removed by `/retry` or `/clear` cannot outlive their text.

### Method

- Baseline: `git archive v0.2.0-rc1 | tar -x -C /tmp/meong-base-1d30020`,
  then `./build.sh`; candidate: `./build.sh` on this tree. Both are `-O2`,
  stripped, and linked with `ld.lld`; only the source changed.
- SSE: a loopback mock (`tests/python/bench_sse.py`) emits 64 B content deltas
  with chunked transfer; 1 MB for callgrind and 8 MB for wall clock (median of
  9 interleaved baseline/candidate pairs of `--stream --oneshot`). The client's
  per-event `fflush` is part of the workload on both sides.
- Callgrind: `valgrind --tool=callgrind ./build/meong-g ...`; profile an
  `-O2 -g` twin, never the stripped release binary. The free() row counts the
  libc `free` entries reached from `jdoc_free` and `buf_free`: the baseline
  frees the token+skip pair and the event Buf per frame, the candidate only at
  teardown.
- Layout: `tests/c/layout_bench.c`, compiled against each tree. The same source
  drives `layout_from` and `layout_build`, so it measures the baseline's full
  rewrap and the candidate's tail cursor without conditional compilation. It
  forces a rebuild on every append; the live client's 90 ms throttle is not part
  of this harness.
- `./tests/test.sh` is 29/29 green; the same suite passes under the patched
  39-bit ASan/LSan runtime at `/home/han/asan39-res`.

### Caveats

- Instruction counts are loopback client-side work, not gateway latency. The
  Python mock can saturate before the client, so wall clock is a lower bound.
- The binary grew 8.9%: the fast scanner and the layout cursor are real code.
  The trade is 2.09× less streaming work for 7 KB.
- The layout panels are a forced per-append worst case. The live client only
  rebuilds while streaming when 90 ms have passed, so the user-visible effect
  is a lower frame-time bound, not a shorter stream.
- Reasoning growth still invalidates the layout cursor, so a reasoning-heavy
  stream re-wraps the message while reasoning arrives; the tail cursor covers
  answer text.

## v0.1.1 — per-byte calls and full-history relayout

The v0.1.1 pass removed two structural costs: the SSE decoder and JSON string
unescape called `buf_addc` once per byte, and every streaming redraw re-wrapped
the whole transcript. Both are gone; the compiler flags did not change. This
section used a different mock (`mock_paig.py`) and tree; compare rows only
within a section.

![Meong v0.1.0 vs v0.1.1 benchmark](images/profile-v0.1.0-vs-v0.1.1.png)

| Workload | v0.1.0 | v0.1.1 | Delta |
| --- | --- | --- | --- |
| Instructions / 1 MB SSE (callgrind) | 104.7 M | 32.4 M | 69% fewer, 3.23× less work |
| Wall time / 8 MB SSE | 285 ms | 140 ms | 51% less time, 2.04× faster |
| Allocator Ir / 1 MB SSE | 8.95 M | 4.97 M | 44% fewer |
| Layout redraw, 201 messages | 4.60 ms | 0.010 ms | 99.8% less, 460× less work |
| Full layout, 200 KB message | 4.84 ms | 1.27 ms | 74% fewer |
| `draw_slice`, 117 ASCII chars | 13.32 µs | 8.82 µs | 34% fewer |
| Binary size | 78,128 B | 78,792 B | +664 B (+0.85%) |

Method: baseline `git archive v0.1.0`, candidate on the release branch, same
`-O2` flags and `ld.lld`. SSE used a loopback chunked mock; wall clock was the
median of 9 interleaved runs. Layout and `draw_slice` used one harness compiled
against both trees. The `draw_slice` number needs a pty at least as wide as the
line (120 columns here). The skip table has a differential harness in `tests/`.
`-O3` and LTO were measured and rejected: no gain after the structural fix, and
GCC LTO does not link with `ld.lld` on this host.
