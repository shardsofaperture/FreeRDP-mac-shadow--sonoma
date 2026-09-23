# Mac Shadow 0.2.0N spatial coverage diagnostic (2026-09-22)

This uncommitted experiment starts from `master` at
`c72de69d5066a5a48346385484f3becf8f424483` plus the preserved
uncommitted latency work. N uses F's fixed 153,600 B/s admission and
system-default socket buffer. The large-refresh burst and adaptive ramp are
disabled. Existing F, L, M, RC3, and 0.1.9 recovery bundles remain untouched.

## Physical observation versus source finding

The user's RC3 physical comparison reports about five seconds for a large
window promotion, about 13 visible horizontal bands versus F's 7–8, and
F-like mouse/menu response. The running RC3 log contains post-startup bursts
with nonzero published-region area and zero client-refresh area; some end at
the local socket queue guard. This verifies the corrected trigger. It does not
establish that sender tile order alone determines the client's visible bands.

The source does establish a default row-major sender order. `shadow_bitmap_new`
caps tiles at 64×64 pixels, reducing their dimensions when the negotiated
`MultifragMaxRequestSize` requires it. `columns = ceil(width/tileWidth)`;
index `row*columns+column` maps low indexes to the upper left. At 64×64,
1920×1080 has 30 columns and 17 rows, and 1024×768 has 16 columns and 12 rows.
`shadow_bitmap_stage` copies the newest pixels, compares each tile with the
ordered sent cache, and rebuilds a byte-array dirty map and pending count.
The latest stage replaces obsolete unsent pixels; the sent cache changes only
on commit. `shadow_bitmap_next` scans cyclically from a persistent cursor
through that row-major map. A new publication does not reset the cursor to
zero. `shadow_bitmap_commit` advances it only after an ordered write succeeds.
With the cursor at zero, a broad ordinary bitmap update walks upper rows
before lower rows. A test against the production functions confirmed this
case: the first eight tiles from a full 512×512 publication all had Y=0; the
new coverage assertion failed before the scheduler change.

The copy path can reverse X or Y traversal after probing an eligible ScrBlt
source. That dependency order is retained in N. The custom 16-bit interleaved
and 32-bit planar path emits one rectangle per `BitmapUpdateProxy` call, with
up to eight operations, 16 KiB of estimated wire work, and an 8 ms cooperative
flush target. There is no extra rectangle group imposing a horizontal sweep.
The reversible scheduling decision is `shadow_bitmap_next`, after staging and
before compression, exact-size pacing admission, `BitmapUpdateProxy`, and
`shadow_publication_commit`. These source findings support an isolated order
test; they do not prove an end-to-end presentation-time improvement.

## N algorithm and boundaries

N activates coverage only for a published region covering at least 25% of the
desktop with at least `max(8, 2*tileRows)` actually dirty tiles. It keeps the
existing dirty array, sent cache, staging, compression, pacing, and write path.
The new cursor visits one tile in spatially separated rows before visiting a
second column. It uses the largest row step at or below half the row count
that is coprime with the row count. For eight rows the order starts
0, 3, 6, 1, 4, 7, 2, 5. The step and inverse are computed once when the
bitmap state is created. Each ordinal maps to one tile and the inverse maps a
successfully committed tile back to the next ordinal. This permutation is
bounded and visits every tile in a pass. It adds no encoded queue or replay.

The cursor is not reset on a new stage, so frequent upper-screen changes do
not repeatedly send the first upper tile ahead of older lower damage.
Smaller publications retain active coverage until the backlog converges;
small isolated updates never activate it. If the existing copy probe detects
a coherent move, N uses the original direction-aware traversal for that
generation. Individual ScrBlt candidates still require a known, matching
source in the current ordered sent-cache model. A failed/deferred admission
does not commit the tile or advance the cursor; only successful ordered
submission calls `shadow_publication_commit`. An empty pending set clears
coverage. The F/RC3 code path stays on its existing row-major traversal unless
the N environment flag is explicitly set.

## Deterministic checks

`TestShadowCoverage` uses the production surface staging, bitmap selection,
and commit functions. It checks the initial row spread, F-disabled row-major
control, 16/32-bit client reconstruction, first-frame/static convergence,
small localized damage, replacement with newer pixels, failed/deferred
admission, exact-once successful charge/commit, lower-screen progress under
repeated upper changes, and overlapping ScrBlt move fallback.
`TestShadowLatencyWorkload` runs identical patterned windows with actual
interleaved/planar compression and the same mock transport/1 ms clock.
F and N have equal charged bytes and base rate; N never starts a burst:

| Surface | F complete / early Y in first 200 ms | N complete / early Y in first 200 ms | Bytes each |
| --- | --- | --- | ---: |
| 1920×1080×32 | 3171 ms / 135–192 | 3170 ms / 135–896 | 348,076 |
| 1024×768×16 | 6730 ms / 96–96 | 6730 ms / 96–640 | 896,376 |

At 32-bit the first 200 ms included 42 F and 46 N operations; at 16-bit it
included 11 F and 6 N operations. The simulation proves spatially separated
submission under the same rate, not faster client display or more useful
perceived pixels. Queue, SSH, and client presentation are not modeled.
The complete targeted Release/json-c suite passed 19/19. The socket test used
only an isolated ephemeral loopback port. No source change was made to the
input-before-graphics or bounded channel service order; those remain
physical regression checks alongside Android, Win98, and Mac RDC.

## Package and physical decision

The expected package is `dist/FreeRDP Shadow 0.2.0N.app`, built by
`python3 scripts/build-macos-shadow-app.py --experimental-arm N --build-dir build-macos-shadow-n-package`.
Its signed configuration identifies N, fixed 150 KiB/s, no burst, zero socket
cap request, coverage enabled, and `127.0.0.1:3390`. Signed source provenance
and adjacent SHA-256 sidecars permit exact source/artifact checks. It is a
diagnostic candidate, not an installed or accepted replacement.

With pacing diagnostics enabled, one `N coverage early` row for each isolated
broad publication reports its publication ID, first 200 ms submission count,
first/last/min/max and sampled Y values, pending tiles, cursor, column pass,
row step, copy-move fallback, and publication age. It does not log pixels,
keystrokes, clipboard text, or every tile. Compare the same large-window
promotion against F, recording client-visible elapsed time, bands, text
readability, mouse/menu response, and stopped-screen convergence. If N merely
turns a five-second banded wipe into a five-second scattered paint, it has not
met the usability goal; use the preserved F artifact.
