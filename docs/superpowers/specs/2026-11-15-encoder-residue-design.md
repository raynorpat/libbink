# Encoder RESIDUE blocks — design

Status: awaiting approval. Follow-on to
`2026-11-15-encoder-dct-design.md`.

## Intent

Add Bink's `RESIDUE_BLOCK` to the encoder. It codes a motion-compensated
residual **exactly** as a bit-plane tree, and for sparse residuals it is
much cheaper than a DCT block, which has to pay a 10-bit DC, a 4-bit
`cbits`, a 4-bit quantizer and tree overhead before it codes anything.
Today the encoder jumps from an exact motion match straight to DCT (or
raw), so small differences around good motion are expensive.

## Goals

- Emit `RESIDUE_BLOCK` when it is the cheapest coding for a block.
- Reproduce the residual exactly: a residue block decodes to the source
  pixels, unlike every DCT block.
- Keep the existing block choices and their error caps untouched.

## Non-goals

- Changing the decoder's residue parser or its 7-bit budget semantics.
- Emitting residue on keyframes (there is no predictor).
- Trying to beat DCT on dense residuals.

## Constraints

- The decoder is the authority: `read_residue` in `src/bink_video.c` and
  the `case RESIDUE_BLOCK` arm in `decode_plane`.
- No public API change; `src/encoder/bink_encoder.h` stays as it is.
- `EncBlock` must not grow. Blocks are allocated per plane; at 7680x4800
  there are 576,000 of them, so 64 bytes each is 36 MB.

## Approach

Three were considered:

- **A (chosen) — a fourth candidate in the existing cost comparison.**
  Build the residual from the best-SSD predictor, reject it when the
  coder cannot represent it, encode to count bits, and let it compete
  with intra, inter and raw on estimated bits. No thresholds.
- **B — the reference encoder's SAD gate** (mean |residual| ≤ 8 → residue
  else DCT). Cheap, but it picks residue on dense residuals where DCT is
  cheaper, and the threshold is magic.
- **C — sparse-only rule** (≤ N differing pixels → residue, no cost
  comparison). Still a threshold, and it can lose to intra on flat blocks.

A wins because it is the same shape the encoder already uses, needs no
tuning, and residue blocks are exact, so the tie-break prefers them for
free.

## The bitstream contract

Copied from the decoder, not from the reference encoder:

```
budget   : 7 bits, number of significant bits the decoder may consume
mask_bits: 3 bits, the block's top plane; top mask = 1 << mask_bits
levels   : mask = 1 << mask_bits down to 1
```

Each level, in this order:

1. For every coefficient already significant, in the order it became
   significant: one bit, 1 meaning "add `mask` to its magnitude". The
   decoder applies it with the coefficient's own sign.
2. A tree walk with roots `(4,mode 0), (24,mode 0), (44,mode 0),
   (0,mode 2)` — the same probe/twin structure the coefficient coder
   uses, so the existing node-list semantics carry over:
   - one bit per live node: 1 opens it, 0 skips;
   - mode 0 and mode 2 open four consecutive coefficients: for each, if
     its top plane is `mask`, write 0 and then a sign bit (1 negative)
     and mark it significant; otherwise write 1 and push it as mode 3 at
     the front of the list;
   - mode 1 spawns three mode-2 nodes at +8, +12, +16 and writes nothing
     else;
   - mode 3 writes a sign bit and marks the coefficient significant.

A coefficient becomes significant at the plane of its magnitude's highest
bit, so its leading 1 is implicit and only the sign is written; lower
bits arrive through step 1.

`budget` is 127, the same sentinel the reference writes. It is a cap, not
a length: the decoder returns early on the 128th significant bit, so a
candidate whose popcount exceeds 127 must be rejected rather than
truncated. `mask_bits` is 7 for any residual, so the coder covers the
full -255..255 range; the real limit is sparsity, not magnitude.

## Encoder shape

**Writer — `src/encoder/bink_enc_dct.c`, declared in `bink_enc_dct.h`:**

```c
/* Write one RESIDUE payload (7-bit budget, 3-bit top plane, bit-plane
 * tree). Returns the bit count, or -1 when the residual does not fit.
 * bw == NULL counts without writing. */
int bink_enc_emit_residue(BinkBitWriter *bw, const int16_t residual[64]);
```

It mirrors `bink_enc_emit_coeffs`' single-implementation rule: one code
path writes and counts, and the count is what selection compares.

**Decoder hook — `src/bink_video.c`, declared in `bink_video.h`:**

```c
/* Test hook: read one residue block the way decode_plane does, including
 * its 7-bit budget. block is raster-ordered and zeroed on entry. */
int bink_video_read_residue(BinkBits *bits, int16_t block[64]);
```

Same role `bink_video_read_coeffs` plays for the coefficient writer: the
writer is tested against the real parser, never a copy of it.

**Selection — `bink_enc_video.c`:** the residue candidate joins the
existing `BestDct` comparison with `kind = 3`. It is considered only when
a predictor exists, and its estimated bits are
`bink_enc_emit_residue(NULL, residual)` plus 16 for the motion-vector
pair (the vector rides the X/Y bundles). Its distortion is zero, so it
needs no cap check.

**Storage:** a residue block keeps its signed residual in `EncBlock.t`,
which DCT blocks use for coefficients. The two never coexist, so this
costs no memory; the field gets a comment naming both uses.

**Emission and painting:** `emit_rows_dct` writes the residue payload for
`RESIDUE_BLOCK` blocks at the same point in the row that DCT coefficients
go; `paint_block` copies the predictor at `(mx,my)` and calls
`bink_add_pixels8`, exactly as the decoder does. `encode_plane` appends
`mx`/`my` for `RESIDUE_BLOCK` beside MOTION and INTER, and appends no DC.

## Tests

- `test_residue_writer_roundtrip`: sparse random residuals through
  `bink_enc_emit_residue` and back through `bink_video_read_residue`,
  bit-exact. Cases: all zero, one pixel, negatives, magnitude 255, a
  dense block with popcount just under 127, and one over 127 expecting -1.
- `test_residue_block_used`: a P-frame whose blocks differ from the
  predictor in two pixels by a large amount. Assert the decode is exact
  (max error 0, which no DCT block can be) and that the packet size stays
  under a measured ceiling, so the block cannot have fallen back to raw.
- `test_audio`-style guard is not needed; video tests above cover it.

## What is left to the builder

Exact function bodies, the test thresholds (calibrated by measurement and
recorded in the plan), and the docs sentence. The bitstream contract
above is not open for reinterpretation — the decoder is the authority,
and a mismatch shows up as a wrong picture, not a decode error.
