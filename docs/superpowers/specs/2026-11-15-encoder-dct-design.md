# Encoder DCT: video blocks and audio coefficients

Status: draft for builder review
Date: 2026-11-15
Project: libbink
Scope: `src/encoder/bink_enc_video.c`, `src/encoder/bink_enc_audio.c`,
`src/bink_audio.c`, `src/bink_audio.h`, `src/bink_video.c`,
`src/bink_video.h`, `src/encoder/bink_enc_verify.c`, `tests/test_encoder.c`,
`docs/encoder.md`, `README.md`

## Intent

The encoder currently writes only non-transform video blocks (skip, motion,
fill, pattern, raw) and a DC-only audio track. Photographic video therefore
falls back to raw and barely compresses; audio is intelligible only as a
slow envelope. The goal is to make the encoder use Bink 1's real DCT
machinery on both sides:

- Video: intra DCT blocks on keyframes and inter DCT blocks on
  motion-compensated residuals, so photographic frames compress instead of
  falling back to raw.
- Audio: full Bink DCT coefficient coding, so tones and music decode
  recognizably instead of as a DC contour.

The existing lossless block modes stay as fallbacks. A file that uses them
is still lossless on the encoder's own YUV planes; a file that uses DCT is
lossy within a bounded error.

## Goals

- Emit valid Bink 1 intra and inter DCT blocks that libbink and VLC decode.
- Choose DCT when it reduces bits within a bounded reconstruction error.
- Keep skip, motion, fill, pattern, and raw blocks available and correct.
- Encode full audio DCT coefficients with per-band quantization and
  zero-run RLE.
- Preserve the public encoder API in `bink_encoder.h`.
- Keep the decoder, tests, and tools building and passing.
- Keep LGPL-2.1-or-later licensing and FFmpeg attribution intact.

## Non-goals

- Matching the original Bink encoder's exact choices or bitstream.
- Bink 2, alpha planes, or new container variants.
- RDFT audio encoding. The encoder keeps using Bink DCT audio
  (`use_dct = 1`).
- Psychoacoustic masking or perceptual bit allocation.
- Full rate-distortion optimization over every mode, motion vector, and
  quantizer.
- Real-time encoding speed.
- A new public quality option. Quality constants are internal in v1.

## Anti-goals

- A file that libbink or VLC decodes incorrectly, or that crashes either.
- DCT blocks the decoder cannot reach because the coefficient tree was
  written wrong.
- Regressing the existing lossless modes or `test_encoder`.
- Guessing the forward transform from a textbook DCT instead of inverting
  the decoder's actual integer IDCT.

## Constraints

- The encoder writes the `BIKi` signature; colors are stored as written.
- Video planes are the BT.601 studio-range YUV planes the existing
  conversion produces. Chroma is 4:2:0.
- The decoder's integer IDCT wraps out-of-range pixels through
  `unsigned char`. Encoder reconstruction must simulate that exactly.
- The decoder's coefficient tree is the authority for which scan indices
  can be coded and at which bit plane.
- Audio block layout, band boundaries, quant tables, overlap length, and
  frame length come from `bink_audio_describe`.
- No new external dependencies.
- Match the existing C style: 4-space indent, C99 declarations, existing
  error handling, no new global state.

## Approaches considered

### Video

| Option | Trade-off |
|---|---|
| A. Intra-only DCT | Simplest. Keyframes compress, but P-frames still fall back to raw without an exact motion match, so the main win is missed. |
| B. Intra + inter DCT with heuristic selection | Recommended. Adds intra and motion-compensated inter candidates, keeps existing modes as fallbacks, and picks a quantizer from a small candidate set by estimated bits plus distortion. Good compression without a full search. |
| C. Full rate-distortion search | Best quality per byte. Searches modes, motion vectors, and all quantizers with true distortion. Much slower and much more code. |

Chosen: **B**. The user picked it explicitly.

### Audio

| Option | Trade-off |
|---|---|
| A. Fixed per-band quantizer | Simple. Full DCT, but one fixed step per band and no RLE. Recognizable, larger files. |
| B. Adaptive per-band quantizer + RLE | Recommended. Picks each band's step to keep coefficients in a target range, and skips zero runs. Good quality/size balance. |
| C. Psychoacoustic model | Highest quality at low rates. Needs masking models and bit allocation; out of scope. |

Chosen: **B**. The user picked it explicitly.

## Current behavior that this changes

- `encode_plane` decides each block as skip, motion, fill, pattern, or raw.
  It never emits `INTRA_BLOCK` or `INTER_BLOCK`.
- The DC bundles (`BINK_SRC_INTRA_DC`, `BINK_SRC_INTER_DC`) are emitted as
  empty walks. The decoder reads one 11-bit DC value per intra/inter block;
  today there are none.
- The main bitstream after the per-row bundles carries no video block data.
  That is why the encoder can emit all bundle rows first and align once.
- `bink_audio_write_dc_block` writes a 2-bit DCT marker, two float-coded
  coefficients, zero quantizers, and zero-width chunks. It reconstructs
  only the block mean.
- `bink_enc_verify` requires an exact match between the decoded picture and
  the source YUV. That must become a tolerance check once DCT is used.

## Video DCT design

### Bitstream contract the encoder must satisfy

For each plane, `decode_plane` reads:

1. Bundle headers (trees for every source except the DC bundles).
2. For each block row:
   - one row from every bundle, in `BINK_SRC_*` order;
   - then the blocks in that row, left to right, reading DC values from the
     DC bundles and coefficient bits from the main stream as needed.
3. A 32-bit alignment at the end of the plane.

For an `INTRA_BLOCK` the decoder:

- reads an 11-bit unsigned DC value from `BINK_SRC_INTRA_DC`;
- sets `dctblock[0] = dc`, everything else zero;
- calls `read_dct_coeffs`, which reads the 4-bit `cbits`, the coefficient
  tree bits, and the 4-bit quantizer index;
- unquantizes with `bink_intra_quant[quant_idx]`;
- calls `bink_idct_put`, which overwrites the block.

For an `INTER_BLOCK` the decoder:

- reads the motion vector from `BINK_SRC_X_OFF` and `BINK_SRC_Y_OFF`;
- copies the 8x8 reference block with `bink_put_pixels` (integer copy,
  no interpolation);
- reads a signed DC value from `BINK_SRC_INTER_DC` (10 bits plus sign);
- reads and unquantizes the coefficient tree with `bink_inter_quant`;
- calls `bink_idct_add`, which adds the IDCT output to the predicted block
  and casts through `unsigned char`.

`read_dct_coeffs` writes `block[bink_scan[idx]] = t` and stores `idx` in
`coef_idx`; `unquantize_dct_coeffs` then does
`block[bink_scan[idx]] = (t * quant[idx]) >> 11`, with truncation, and
handles the DC as `block[0] = (t * quant[0]) >> 11`.

The 64-entry `bink_scan` maps scan index to raster position. DC is scan
index 0 and is never coded by the tree; it always comes from the DC bundle.

### Forward transform

The decoder's integer IDCT is two passes of the same 8-point column
transform, with the row pass dividing by 256:

```
out = (C * coef * C^T) / 256
```

`C` is the ideal real-valued version of the decoder's fixed-point column
transform. Build it from the decoder constants scaled by 2048:

```
A1 = 2896 / 2048.0
A2 = 2217 / 2048.0
A3 = 3784 / 2048.0
A4 = -5352 / 2048.0

col(s):
  a0 = s[0] + s[4]
  a1 = s[0] - s[4]
  a2 = s[2] + s[6]
  a3 = A1 * (s[2] - s[6])
  a4 = s[5] + s[3]
  a5 = s[5] - s[3]
  a6 = s[1] + s[7]
  a7 = s[1] - s[7]
  b0 = a4 + a6
  b1 = A3 * (a5 + a7)
  b2 = A4 * a5 - b0 + b1
  b3 = A1 * (a6 - a4) - b2
  b4 = A2 * a7 + b3 - b1
  return [a0 + a2 + b0,
          a1 + a3 - a2 + b2,
          a1 - a3 + a2 + b3,
          a0 - a2 - b4,
          a0 - a2 + b4,
          a1 - a3 + a2 - b3,
          a1 + a3 - a2 - b2,
          a0 + a2 - b0]
```

`C[s][f] = col(unit_vector(f))[s]`. Invert `C` once with an 8x8 Gaussian
elimination to get `Ci`. The forward transform for an 8x8 pixel block `P`
is:

```
coef = Ci * (256 * P) * Ci^T
```

Use doubles for the two 8x8 multiplies and round to integers only when
quantizing. `256 * P` keeps the coefficient scale aligned with the
decoder's `>> 11` unquantization and `/256` IDCT.

Why 256 and not a textbook DCT scale: the decoder's `C` is not the
orthonormal DCT matrix, and the `>> 11` in `MUL` is part of the integer
transform. Inverting the ideal `C` (the linear part of the decoder's
column transform) is what makes the round trip exact. A textbook DCT with
guessed scaling produces the wrong basis.

Validation: with `t = round(coef * 2048 / quant[idx])` and the decoder's
`unquantize`/IDCT, a Python model recovered gradient, edge, flat, and
random 8x8 blocks with 0-2 maximum pixel error. The same model matched the
C `bink_idct_put` output for DC-plus-AC test blocks. The design relies on
that result; the implementation should keep the double-precision forward
transform and the integer reconstruction simulation.

### Quantization

For a chosen quantizer index `q` in 0..15:

```
t[i] = round(coef[bink_scan[i]] * 2048 / quant[q][i])   for i = 0..63
```

`t[0]` is the DC value. `t[1..63]` are the AC values the tree may code.
For reconstruction:

```
coef_raster[0] = (t[0] * quant[q][0]) >> 11
coef_raster[bink_scan[i]] = (t[i] * quant[q][i]) >> 11   for coded i > 0
```

Uncoded entries stay zero.

Reject a candidate quantizer if any `|t[i]| >= 32768` (the tree's 4-bit
`cbits` can represent at most 15 planes, and 32768 needs plane 15). With
the candidate set and small blocks this should be rare; the fallback is a
coarser quantizer or raw.

The candidate set is internal. Start with `{0, 2, 4, 6, 8, 10, 12, 14}`
for both intra and inter; tune it after measuring size and quality. Each
candidate is scored; the block keeps the best one.

### DC values

The DC bundle value is `t[0]` for the chosen quantizer:

- Intra: `t[0]` must be in `0..2047`. For quantizer 0 the maximum block
  mean (255) gives 2040, so this is naturally satisfied; a coarser
  quantizer gives a smaller value. Reject if it does not fit.
- Inter: `t[0]` must be in `-1023..1023` because the decoder reads 10 bits
  plus a sign. Motion compensation must be good enough for the residual DC
  to fit; otherwise reject the inter candidate and let intra or raw win.

The DC bundle is not Huffman-coded. `bink_bundle_read_dcs` reads:

- the first value of a chunk absolutely: 11 bits for intra, 10 bits plus a
  sign bit for inter;
- then groups of up to eight deltas, each group prefixed by a 4-bit
  `bsize`;
- if `bsize == 0`, every delta in the group is zero;
- otherwise each delta is `bsize` bits of magnitude plus a sign bit when
  nonzero, added to a running value.

The encoder must mirror that exactly. The first value of each chunk is
absolute; deltas do not cross chunk boundaries. A chunk can span rows.
`plan_chunks` groups row counts into chunks; for the DC bundles the row
counts are numbers of DC values, not bytes.

### Coefficient tree writer

The tree is a bit-plane coder with a fixed node structure. The decoder
starts with six root nodes:

```
(4, mode 0), (24, mode 0), (44, mode 0), (1, mode 3), (2, mode 3), (3, mode 3)
```

The encoder should not hand-derive the resulting coefficient groups. It
should simulate the decoder's loop and decide one bit at each node. The
decision rule is driven by each coefficient's target plane:

```
tp[i] = floor(log2(|t[i]|))     when t[i] != 0
tp[i] = -1                      when t[i] == 0
cbits = max(tp[i]) + 1          when any AC coefficient is nonzero
cbits = 0                       when all AC coefficients are zero
```

Write `cbits` as 4 bits first. Then run the decoder's loop for
`p = cbits - 1` down to `0`, with the same node list, `list_start`,
`list_end`, and `list_pos` mechanics. At each node:

- **Dead node:** advance.
- **Group node (mode 0, 1, or 2):** decide whether to open it at this
  plane. Open it when any coefficient the node will code or spawn has
  `tp == p`; otherwise write 0 and advance. Once opened, reproduce the
  decoder's state transitions exactly:
  - mode 0: code the four coefficients `c..c+3`; the node becomes
    `(c+4, mode 1)` and is revisited in the same plane;
  - mode 1: set the node to mode 2 and append `(c+8, mode 2)`,
    `(c+12, mode 2)`, `(c+16, mode 2)` at `list_end`; revisit in the same
    plane;
  - mode 2: mark the node dead and advance; code `c..c+3`.
- **Individual node (mode 3):** write 0 to leave it for a lower plane,
  or write 1 to code the coefficient now. Only open it when
  `tp[c] == p`; a zero coefficient is left closed forever.
- **Within an opened group**, for each of its four coefficients:
  - `tp == p`: write 0 and then the value: `cbits == 0` means a single
    sign bit for +/-1; otherwise write `|t| - 2^p` in `p` bits and a sign
    bit (1 = negative).
  - `tp < p` or `t == 0`: write 1 to push a new `(c, mode 3)` node at
    `list_start`. It is visited on the next plane.
  - `tp > p` must not happen; it means a coefficient missed its plane.

`cbits = 0` writes no tree bits. The decoder skips the loop and reads the
quantizer index.

The writer should share one implementation with a bit counter: pass a
`BinkBitWriter *` or `NULL`; when `NULL`, accumulate the same bits into a
counter. The counter is used for block selection. The writer itself is the
authority for the bitstream.

The tree reaches all scan indices 1..63 (DC is index 0 and is bundled).
It does not need a list of "unreachable" indices; the simulation is the
contract.

Do not read too much into the fixed root list. A root mode-0 node covers
eight coefficients in its own plane (`c..c+7`) and spawns three groups
(`c+8`, `c+12`, `c+16`). The exact revisit order is the decoder's; mirror
it rather than inventing a group table.

### DC bundle writer

Add an `emit_dcs` callback alongside `emit_types`, `emit_colors`,
`emit_patterns`, and `emit_motion`. It consumes a chunk of `int16_t`
values:

- Write the first value absolutely: 11 bits for intra, 10 bits plus a
  sign bit when nonzero for inter.
- Then for each group of up to eight deltas after the first:
  - compute `bsize` as the smallest number of bits that holds every
    `|delta|` in the group, or 0 when all deltas are zero;
  - write `bsize` in 4 bits;
  - if `bsize != 0`, write each `|delta|` in `bsize` bits, then a sign bit
    when the delta is nonzero.
- Advance the running value by each delta exactly as the decoder does.

The DC buffers should be `int16_t` buffers (or byte buffers with an
`item_size == 2` convention in `BundleWalk`). `plan_chunks` and the row
counts must count values, not bytes. The simplest change is to extend
`BundleWalk` with an `item_size` field and compute
`data + chunk.off * item_size` when calling the emitter. All existing
bundles keep `item_size == 1`.

Do not write a Huffman tree for either DC bundle. `read_bundle_header`
skips the tree for them.

### Block selection

Keep the existing lossless decisions where they are cheapest:

1. **Skip** when the block exactly matches the reference at the same
   position on a P-frame.
2. **Motion** when a motion vector in range gives an exact visible match.
3. **Fill** when the block has one unique value.
4. **Pattern** when it has exactly two unique values.

For everything else, evaluate:

- **Raw:** 64 colors, lossless.
- **Intra DCT:** forward transform of the desired block, quantizer
  candidates, reconstruction, rate.
- **Inter DCT:** on non-keyframes, forward transform of
  `desired - predicted`, quantizer candidates, reconstruction, rate.

The motion search should retain the best-SSD vector even when no exact
match exists. The existing search already scans the full range when there
is no match; add SSD tracking and keep the best vector. Exact matches
still break early. `--motion-range` still bounds the search.

**`motion_range == 0` disables the search, not inter prediction.** The
public option is documented as "0 disables motion search", so keep that
meaning: on a non-keyframe an inter candidate at `(0,0)` is still
evaluated, using the co-located reference block. This keeps the option's
contract, exercises inter DCT in motion-free tests, and is the right mode
for a static camera. When `motion_range > 0`, the best-SSD vector is used
and `(0,0)` is just one of the candidates. The new inter test must cover
both: a `motion_range == 0` P-frame that picks inter at `(0,0)`, and the
existing `test_skip_and_motion` path with a real vector.

Distortion is measured on the visible pixels of the 8x8 block. Use two
caps:

```
max absolute pixel error <= 24
root-mean-square error   <= 4.0
```

A candidate that exceeds either cap is rejected. The caps are internal
constants; tune them with the test media. The max-error cap also catches
`unsigned char` wraparound, which produces errors near 250 and should
never be accepted.

Estimated bits for the existing modes (type bits are common and omitted):

```
skip    = 0
motion  = 16
fill    = 8
pattern = 80
raw     = 512
dct     = 4 (cbits) + 4 (quantizer) + tree bits + 11 (DC estimate)
```

These are heuristics. The actual color/pattern/motion costs come from the
per-frame Huffman trees built later, so the estimates do not need to be
exact; they only need to rank candidates sensibly. A future two-pass
refinement is out of scope.

Pick the candidate with the lowest estimated bits among those that meet
the distortion caps. Break ties toward lower distortion. If no DCT
candidate meets the caps, use raw. Fill and pattern are already lossless
and usually cheaper than DCT, so they can be chosen without evaluating
DCT for one- and two-color blocks.

**Inter prediction and wrap safety.** The decoder copies the reference
block with `bink_put_pixels` (integer copy) and adds the IDCT with
`unsigned char` arithmetic. To keep the ideal reconstruction in range,
clamp the residual before the forward transform:

```
res[y][x] = clamp(desired[y][x] - predicted[y][x],
                  -predicted[y][x], 255 - predicted[y][x])
```

Quantization can still overshoot by a pixel or two. The encoder must
simulate the exact decoder operation (`bink_idct_add` on the predicted
block) and measure distortion from the wrapped result. If a wrap occurs,
the huge distortion rejects the candidate naturally. Do not "fix" the
coefficient block after the fact unless a simple fallback is needed; raw
is always available.

**Intra reconstruction.** Use `bink_idct_put` on a scratch 8x8 buffer with
stride 8 for candidate scoring, and on the destination plane with the
real stride during painting. The decoder overwrites the block, so there is
no reference to add.

**Edge blocks.** The existing `src_at` clamps source coordinates to the
visible plane, so edge blocks replicate the last row/column. Forward-
transform that clamped 8x8 block. Measure distortion only on visible
pixels. The decoder still writes the full 8x8 including padding; painting
must reconstruct the full block so future motion prediction matches the
decoder's reference exactly.

### Streaming and layout changes

The current `encode_plane` emits all bundle rows, aligns, then paints.
With DCT, the decoder reads the DCT bits for a row immediately after that
row's bundle data. Restructure the emission:

1. Plan every block and fill the bundle buffers, including the DC values.
2. Compute chunks and run the frequency pass with a `NULL` writer. This
   pass must not emit DCT bits.
3. Build the Huffman trees and write the tree headers.
4. For each row:
   - write one row from every bundle with `walk_row`;
   - if writing, emit the DCT bits for the intra/inter blocks in that row,
     left to right.
5. Align 32 bits.

The first pass can keep using the existing `emit_rows` with a `write == 0`
state. The second pass needs a row loop that calls `walk_row` for every
bundle and then a new `emit_dct_row`. Do not write DCT bits when the
writer is `NULL`.

The per-block `EncBlock` should hold:

```
int op;
int c0, c1;
uint8_t pattern[8];
uint8_t raw[64];
int8_t mx, my;
int16_t t[64];       /* quantized values, indexed by scan index */
int quant;           /* chosen quantizer index */
int cbits;           /* 4-bit plane count used by the tree */
```

About 207 bytes per block. Do **not** also store an unquantized
`int32_t coef[64]`: it is exactly recoverable at paint time from `t` and
`quant`, because the decoder's unquantization is
`coef[scan[i]] = (t[i] * quant[i]) >> 11` for every `i` and gives 0 when
`t[i] == 0`. Dropping it saves ~150 MB at maximum resolution. The same
applies to `raw`: keep the existing `raw[64]` (already present today) and
do not duplicate source pixels anywhere else.

At the library maximum (7680x4800) the luma plane has 576,000 blocks, so
the block array is about 118 MB, on top of the ~110 MB RGB buffer and
~55 MB of planes the encoder already allocates. That is the same order as
today's memory use (the current `EncBlock` is 77 bytes, ~44 MB at that
size, plus the colors bundle). Document the ceiling rather than
restructuring to row-band planning; if it ever matters, re-plan rows in the
second pass instead of storing `t`.

`paint_block` gains:

- `INTRA_BLOCK`: unquantize `t` into a local `int32_t tmp[64]`
  (`tmp[scan[i]] = (t[i] * quant[i]) >> 11`), then call
  `bink_idct_put(dst, stride, tmp)`.
- `INTER_BLOCK`: copy the reference block into `dst` with the existing
  overlap-safe temporary, then unquantize `t` the same way and call
  `bink_idct_add(dst, stride, tmp)`.

Use one shared helper, `unquantize_block(const EncBlock *b, int32_t out[64])`,
for this; candidate scoring uses it on a scratch buffer so the painted
result is bit-identical to the decoder's.

The encoder's `cur`/`ref` ping-pong already makes the next frame's motion
search run against the reconstruction, so DCT drift does not accumulate
relative to the decoder.

### Table access

`bink_scan`, `bink_intra_quant`, and `bink_inter_quant` are static in
`binkdata.h`. Add small accessors in `bink_video.c` and declare them in
`bink_video.h`:

```
const uint8_t *bink_video_scan(void);
const int32_t *bink_video_intra_quant(int index);
const int32_t *bink_video_inter_quant(int index);
```

The encoder calls these. This avoids including `binkdata.h` into the
encoder and duplicating every table (and avoids unused-table warnings).
Validate `index` is 0..15.

### Verification

`bink_enc_verify` currently requires an exact decoded/source match. Replace
`planes_match` with a tolerance check that computes, per plane:

```
max absolute error
root-mean-square error
```

Fail if any plane exceeds:

```
max absolute error <= 32
root-mean-square error <= 5.0
```

These are looser than the per-block caps so that accumulated error and the
verify's independent YUV conversion do not cause false failures. The
verify still checks frame count, dimensions, audio metadata, and decoded
audio sizes. Document that `--verify` means "decodes within tolerance"
once DCT is in use, not "bit-exact".

## Audio DCT design

### Decoder chain

For each block the decoder:

1. Reads a 2-bit DCT marker (ignored).
2. For each channel:
   - reads two float-coded coefficients, multiplies by `root`;
   - reads one 8-bit quantizer index per band, clamped to 95;
   - decodes coefficients 2..`frame_len-1` in chunks starting at index 2;
   - doubles coefficient 0 and calls `dct_iii`, producing `frame_len`
     time-domain samples;
   - blends the first `overlap_len` samples with `previous` unless this is
     the first block;
   - saves the last `overlap_len` samples of the unblended output as the
     next block's `previous`.
3. Emits `block_out = frame_len - overlap_len` samples per channel as
   interleaved signed 16-bit PCM.

The blend is:

```
count = overlap_len * channels
j = ch + i * channels
out[i] = (prev[i] * (count - j) + out[i] * j) / count
```

This is a linear crossfade over the interleaved channel stream. For mono,
`j == 0` at `i == 0`, so the first output sample is forced to `prev[0]`;
the crossfade weight on the current block is zero there. For stereo, the
first left sample is forced, and the first right sample has weight
`1 / count`.

### Overlap policy

The encoder chooses the decoder's time-domain `out` block directly. The
simple policy is exact before quantization:

```
out[i]             = current[i]              for i in [0, block_out)
out[block_out + i] = next[i]                 for i in [0, overlap_len)
```

where `current` is the current block's `block_out` source samples and
`next` is the next block's first `overlap_len` source samples. Why it
works: each block's tail is the next block's first overlap samples, so the
next block's `prev` equals its own `current` head. The decoder's blend is
then `(cur*(count-j) + cur*j)/count == cur`, including the forced
`j == 0` sample. The first block has no blend, and its tail still feeds
the second block correctly. The last block's tail can be zeros because
there is no next block.

This means the encoder needs a lookahead of `overlap_len` samples. The
source adapter is position-addressed (`read_audio(user, track, pos, count,
samples)`), so read the next overlap samples from
`source_pos + block_out`. Beyond `sample_count`, use zeros.

Do not solve the head from a reconstructed `prev`. The simple policy keeps
the coefficients bounded; correcting an early-overlap error would divide
by the small crossfade weight `j` and amplify quantization noise. The
overlap error is a bounded crossfade of the previous tail and current head
quantization errors, which is acceptable.

### Forward transform

`dct_iii` is an unnormalized DCT-III. Its actual response, measured by
calling the decoder's own `dct_iii` on unit coefficients, is:

```
out[t] = (2 / N) * sum(k = 0..N-1, E[k] * cos(pi / N * (t + 0.5) * k))
```

with the twist that the decoder passes `2 * E[0]` in slot 0, which cancels
the factor of two on the DC term. So the matching forward transform is the
**unnormalized** sum, not a scaled one:

```
C[k] = sum(n = 0..N-1, x[n] * cos(pi / N * (n + 0.5) * k))

E[0] = C[0] / 2
E[m] = C[m]      for m >= 1
```

`E[0] = C[0] / 2` is required because the DC basis vector has norm `N`,
not `N/2` like every other basis vector. Concretely, `C[0] = sum(x) =
N * mean`, so `E[0] = N * mean / 2`; the decoder doubles it back to
`N * mean` and its `1/N` DC gain returns `mean`. Do not add a `2/N`
normalization: that makes the decoder output `2/N` of the intended signal
(effectively silence) and pins every band quantizer to index 0. The
existing `dc_response` path confirms the same scale: feeding `src[0] = 1`
produces `1/N` at the output.

For a full-scale cosine over a 2048-sample frame, `C[k]` reaches about
`N/2 = 1024`, which lands the band quantizer index mid-table (around
41/95) as expected.

The decoder reads the first two coefficients as floats and multiplies by
`root`; write `E[0] / root` and `E[1] / root` with the existing
`write_float`. For `m >= 2`, the decoder reconstructs `E[m]` as
`+/-quant[index] * integer`, and `quant` already includes `root`; quantize
with `round(E[m] / quant[index])`.

Implementation: a direct DCT-II over doubles with a precomputed cosine
table, allocated once per `BinkEncAudio` for the current `frame_len`.
`frame_len` is 512, 1024, or 2048, so a table is at most a few megabytes.
Use double accumulation and round only when writing. If profiling shows
this is too slow, a later change can use the existing FFT; correctness
comes first.

### Per-band quantization

`bink_audio_describe` supplies `num_bands`, the boundary array `bands`
(with `bands[num_bands] = frame_len`), the 96-entry `quant` table, and
`root`. Band 0 starts at coefficient 2. For each band:

- compute `maxabs = max(|E[m]|)` over its coefficients;
- if `maxabs == 0`, use quantizer index 0;
- otherwise choose the smallest index `q` in 0..95 such that
  `maxabs / quant[q] <= 1023` (about 10 bits). This keeps the integer
  coefficients comfortably inside the 4-bit chunk width;
- clamp to 95.

Write one 8-bit index per band, in band order. The target of 1023 is a
tunable constant. A smaller target gives finer quantization and larger
files; a larger target gives coarser quantization and smaller files. Use
1023 initially and measure SNR and size on a sine and a short music-like
signal.

### Coefficient chunks and RLE

The decoder's loop starts at index 2. Each iteration reads one RLE flag:

- flag 0: the chunk covers the next 8 coefficients;
- flag 1: read a 4-bit index into
  `{2,3,4,5,6,8,9,10,11,12,13,14,15,16,32,64}` and cover
  `index * 8` coefficients.

The chunk end is clamped to `frame_len`. Then read a 4-bit width:

- width 0: zero-fill the whole chunk;
- otherwise, for each coefficient in the chunk, write `width` bits of
  magnitude and a sign bit when the magnitude is nonzero. The decoder
  multiplies by the current band's quantizer, updating at band boundaries.

Encoder strategy:

- If the next 8 coefficients are all zero, look for the longest zero run
  that starts at `i`, ends at or before `frame_len`, and whose length in
  groups is in the RLE table. Use the largest allowed multiplier. If the
  run is at least two groups, write the RLE flag, the 4-bit table index,
  and width 0. If the run is only one group, write a normal width-0 chunk.
  If the remainder is partial (the last chunk is shorter than 8 because
  `frame_len - 2` is not a multiple of 8), a normal chunk or an RLE chunk
  clamped to `frame_len` both work; prefer the shorter encoding.
- Otherwise write a normal 8-coefficient chunk. The width is the maximum
  bit length of the integer magnitudes in the chunk. Write the magnitude
  and sign for each coefficient, using the per-band quantizer values
  chosen above.

The integer magnitudes are already computed per band, so a chunk can mix
bands; the width is simply the maximum over the chunk. The decoder's band
updates are position-based and do not need extra bits.

Write exactly `num_bands` quantizer indices per channel, in band order,
independent of how chunks are laid out. The decoder tracks a band cursor
`k`: for a width-0 chunk it advances `k` past every `bands[k] < i`, and
for a nonzero-width chunk it advances when `bands[k] == i`. So a zero run
that spans band boundaries is fine, and the encoder never needs to align
chunks to band edges.

### Block and stream state

`BinkEncAudio` gains:

```
float *out;        /* channels * frame_len */
float *scratch;    /* frame_len, for the forward DCT */
float *prev;       /* channels * overlap_len, decoder's previous */
int first_block;
```

Per block:

1. Read the current `block_out` samples and the next `overlap_len` samples
   from the source. Normalize by 32768.
2. Build `out` with the overlap policy above.
3. Call the audio block writer for each channel: forward DCT, quantize,
   write the 2-bit marker, two floats, band indices, and chunks.
4. Align 32 bits after the block.
5. Copy `out[block_out .. frame_len)` into `prev` and clear `first_block`.
   (The simple policy makes this the next block's source head; the decoder
   will reconstruct the tail from the coefficients. Keeping `prev` in the
   encoder is only needed if a later refinement uses reconstructed tails;
   the simple policy itself does not read it.)
6. Advance `source_pos` by `block_out`.

The existing 32-bit PCM byte-count header and block scheduling stay
unchanged. `block_out`, `pcm_bytes`, and `max_decoded` are unchanged, so
container metadata and verify size checks continue to work.

The 2-bit marker is written once per block, before the channel loop.

### Silence, first/last blocks, and errors

- All-zero input produces all-zero `out` (the tails are zero), zero
  quantizers, and zero-width chunks. Long zero runs use RLE. The decoder
  reconstructs silence.
- The first block skips the decoder's overlap blend; the tail policy still
  makes the second block exact.
- The last block's lookahead returns zeros beyond `sample_count`. The tail
  is not consumed by a later block.
- Reject `frame_len` values outside the supported range and any
  coefficient that would not fit in 15 bits. The per-band index selection
  prevents the latter; if it happens, raise the index or zero the
  coefficient.
- Allocation failures return `-1` through the existing paths.

## API, files, and documentation

- Public API: no changes. `bink_encoder.h` stays as it is.
- `src/encoder/bink_enc_video.c`: forward transform, quantizer candidates,
  tree writer, DC emitter, block selection, inter residual handling,
  row-interleaved emission, `paint_block` extensions.
- `src/encoder/bink_enc_audio.c`: overlap state, lookahead, block writer
  call, remove the DC-only mean path.
- `src/bink_audio.c`: add the forward DCT and the full block writer;
  remove or replace `bink_audio_write_dc_block` and `dc_response`. Keep
  `write_float`, `bink_audio_describe`, and the decoder untouched.
- `src/bink_audio.h`: declare the new block writer and any forward DCT
  helper.
- `src/bink_video.c` / `src/bink_video.h`: add the table accessors.
- `src/encoder/bink_enc_verify.c`: tolerance-based plane comparison.
- `tests/test_encoder.c`: new video and audio coverage.
- `docs/encoder.md`: describe intra/inter DCT, the lossless fallbacks, the
  audio DCT/RLE behavior, the tolerance meaning of `--verify`, and the
  internal quality constants.
- `README.md`: update the short encoder description.
- No new source files are required. If the audio forward DCT or the tree
  writer grows beyond a comfortable size, split it into a new pair of
  files and update `CMakeLists.txt`; keep the same licensing headers.

Licensing: new code in these files is LGPL-2.1-or-later. The transform
constants and the coefficient tree are derived from the existing
FFmpeg-derived decoder; keep the FFmpeg attribution in the files that
carry it and add a short provenance comment where the forward transform
and tree writer are introduced.

## Testing and acceptance

### Video

- Existing `test_encoder` cases must still pass.
- Add an intra-DCT case: a photographic/gradient 320x240 frame (or a few
  frames) with more than two unique values per block. Encode, decode with
  libbink, and assert the decoded YUV planes are within the DCT error caps.
  Assert the file is materially smaller than the raw baseline (the current
  8-frame 320x240 gradient is about 905 KB; the target is a large
  reduction, with the exact threshold set after measuring).
- Add an inter-DCT case: two frames with a moving gradient or pattern.
  Encode with a keyframe interval greater than one, decode, and assert
  both frames are within the caps and the P-frame is smaller than the
  intra-only baseline.
- Add a wrap-safety case: a near-black/near-white block with a small
  residual that would wrap if added naively. Assert the decoded block has
  no gross error. The fallback should choose raw or intra if inter would
  wrap.
- Keep the existing solid/pattern/raw/odd/skip/motion cases. For blocks
  that use the lossless modes, the decoded output must still be exact.
- Manual compatibility: encode a short clip and play it in VLC. Optionally
  capture VLC's YUV output with the existing `tests/capture_reference.sh`
  pattern and compare a few frames.

### Audio

- Existing silence/constant-tone cases must still pass.
- Add a 440 Hz sine at 44.1 kHz. Encode, decode, and assert:
  - the output is not silent;
  - the signal-to-noise ratio against the source is at least a chosen
    threshold (start at 20 dB; raise after measuring);
  - the dominant frequency is 440 Hz (a simple Goertzel/DFT check or a
    zero-crossing check).
- Add a stereo case with different tones per channel; check each channel.
- Add 22050 Hz and 48000 Hz cases so all three frame lengths
  (512/1024/2048) are exercised.
- Assert the encoded audio payload is much smaller than raw PCM for the
  tone (it should be, because most coefficients are zero).
- Keep the `--verify` audio-size check working; it depends only on the
  block scheduling, which does not change.

### Build and tools

- `cmake --build build` and `ctest --test-dir build` pass.
- `bink_build --help` still works.
- `test_encoder` runs without media files. Media-dependent tests still
  skip unless `BINK_TEST_MEDIA` is set.
- `bink_build --verify` on a DCT file succeeds within tolerance.

### Acceptance targets

- A photographic/gradient test frame uses DCT blocks (proven by size
  reduction, not by internal counters) and decodes within the caps.
- A moving test sequence uses inter DCT and decodes within the caps.
- A sine tone decodes at the right frequency with >= 20 dB SNR.
- Silence decodes to silence.
- No regression in the lossless modes.
- VLC plays the result.

## Risks and mitigations

| Risk | Mitigation |
|---|---|
| The tree writer produces a bitstream the decoder parses differently. | Mirror the decoder's exact state machine; unit-test the writer against a local decoder model for random coefficient arrays; then test with libbink and VLC. |
| DC delta coding is wrong. | Mirror `bink_bundle_read_dcs` group by group; test intra and inter DC values, including negative inter DC. |
| The forward transform scale is wrong. | Use the ideal `C` inverse, not a textbook DCT; assert round-trip error on synthetic blocks before wiring it into the encoder. |
| Inter residual wrap. | Clamp the residual, simulate `bink_idct_add` with `unsigned char`, and reject candidates with high error; raw is the fallback. |
| DCT makes the encoder too slow. | Keep the quantizer candidate set small; the test media is small. Optimize later if needed. |
| `--verify` fails on lossy files. | Replace exact matching with max/RMSE tolerance checks; document the change. |
| Audio overlap clicks. | The tail policy makes the overlap exact before quantization; measure SNR and listen to a tone. If boundary clicks appear, revisit the tail handling before adding complexity. |
| Audio DCT is too slow. | Direct DCT-II with a precomputed cosine table is acceptable for the test lengths; switch to an FFT only if profiling demands it. |
| The per-block `t[64]` array makes the encoder allocate too much at the 7680x4800 maximum. | Do not store a second `coef[64]`; recompute it at paint time. If the remaining ~118 MB per luma plane is still too much, re-plan rows in the second pass instead of storing `t`. |

## What is left to the builder

- Choose the exact quantizer candidate set and tune the distortion caps
  (`24` max error, `4.0` RMS) and mode-rate constants on the test media.
- Decide whether the DCT tree writer and audio forward DCT live in the
  existing files or a new pair of files; update `CMakeLists.txt` if
  split.
- Choose `int16_t` buffers with `item_size == 2` versus a dedicated DC
  buffer type for the bundle walk. Either is fine if `plan_chunks` and the
  row counts stay in values.
- Tune the audio target coefficient magnitude (start at 1023) and the SNR
  test threshold.
- Decide whether to use `bink_idct_put/add` on stride-8 scratch buffers
  for candidate scoring or a dedicated helper.
- Set the exact size-reduction and SNR thresholds after the first
  measured run.
- Handle allocation and bit-writer failures consistently with the
  surrounding code.
- Update `docs/encoder.md` wording and the README summary.
- Run VLC manually after the automated tests pass.

## Validation notes

The following were checked before writing this design:

- The decoder's ideal `C` matrix was built from the A constants divided by
  2048 and inverted. The float round trip
  `Ci * (256*P) * Ci^T` then `C * coef * C^T / 256` recovered test blocks
  exactly.
- The integer `bink_idct_put` pipeline matched the Python model of the same
  fixed-point steps, including the `unsigned char` cast.
- With quantizer tables and `t = round(coef*2048/quant)`, the integer
  round trip recovered gradient, edge, flat, and random blocks with 0-2
  maximum pixel error.
- The decoder's `read_dct_coeffs` root list, mode transitions, and bit-plane
  value coding were traced; the tree reaches scan indices 1..63. DC is
  scan index 0 and is bundled.
- The audio decoder's `dct_iii`, `coeffs[0] *= 2`, overlap blend, and
  frame/overlap lengths were traced. A C harness that calls the decoder's
  real `dct_iii` on unit coefficients measured
  `out[t] = (2/N) sum(E[k] cos(pi/N (t+0.5) k))`, confirming the
  unnormalized forward sum with `E[0] = C[0]/2` (an earlier Python model
  reported by hand had a sign error in `rdft_idft_c2r`; the C harness is
  authoritative). `dc_response` also reports `gain = 1/N`.
- The existing 8-frame 320x240 gradient encodes to about 905 KB with the
  current non-DCT encoder and plays in VLC. That is the baseline for size
  comparisons.
