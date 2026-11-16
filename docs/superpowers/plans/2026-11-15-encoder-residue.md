# Encoder RESIDUE Blocks Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Emit Bink `RESIDUE_BLOCK` from the encoder when an exact
motion-compensated residual is the cheapest way to code a block.

**Architecture:** A new bit-plane writer mirrors the decoder's
`read_residue` (7-bit budget, 3-bit top plane, probe/twin tree with a
sign-only leaf and a significance list refined one plane at a time). It
joins the existing intra/inter/raw cost comparison in the block selector,
stores its residual in `EncBlock.t`, and is painted by copying the
predictor and adding with `bink_add_pixels8`.

**Tech Stack:** C99, the existing `BinkBitWriter`/`BinkBits` bit I/O,
`bink_enc_dct.c`'s `CoeffBits` counting helper, and the encoder's
existing block-selection loop.

**Spec:** `docs/superpowers/specs/2026-11-15-encoder-residue-design.md`

## Global Constraints

- The decoder is the authority: `read_residue` (`src/bink_video.c:795`)
  and the `case RESIDUE_BLOCK` arm (`src/bink_video.c:1250`). Never change
  either to match the encoder; fix the encoder.
- Budget is 127 and is a cap, not a length: reject a candidate whose
  significant-bit count exceeds 127.
- `mask_bits` is 3 bits, so the coder covers residuals in -255..255; the
  limit on use is sparsity, not magnitude.
- `EncBlock` must not grow. A residue block keeps its residual in
  `EncBlock.t`, which DCT blocks use for coefficients.
- No change to `src/encoder/bink_encoder.h`.
- Licence headers and FFmpeg attribution stay as they are.

## Review Focus

- A residual of exactly 255 (top plane 7) and of exactly -255.
- An all-zero residual, where `mask_bits` is 0 and only probe bits exist.
- The budget boundary: 127 significant bits must encode, 128 must be
  rejected, because the decoder returns early on the 128th and would
  desync.
- Negative residuals: the sign bit meaning is 1 = negative.
- An odd-sized plane (17x9) on a P-frame, where padding pixels and edge
  predictors are clamped.

---

### Task 1: Residue writer and decoder hook

**Files:**
- Modify: `src/encoder/bink_enc_dct.c` (add the writer beside the
  coefficient writer)
- Modify: `src/encoder/bink_enc_dct.h` (declare it)
- Modify: `src/bink_video.c` (add the hook after `read_residue`)
- Modify: `src/bink_video.h` (declare it)
- Test: `tests/test_encoder.c`

**Interfaces:**
- Consumes: `CoeffBits`/`coeff_put` (static, in `bink_enc_dct.c`),
  `coeff_plane` (static, same file), the static `read_residue` in
  `src/bink_video.c`.
- Produces:
  - `int bink_enc_emit_residue(BinkBitWriter *bw, const int16_t residual[64]);`
    writes the 7-bit budget, the 3-bit top plane and the tree; returns the
    bit count or -1 when the residual does not fit; `bw == NULL` counts
    without writing.
  - `int bink_video_read_residue(BinkBits *bits, int16_t block[64]);`
    reads the 7-bit budget and the block exactly as `decode_plane` does;
    returns 0 or -1.

- [ ] **Step 1: Write the failing test**

Add to `tests/test_encoder.c` and register it in `main` beside
`test_coeff_writer_roundtrip`:

```c
static int residue_trial(const int16_t res[64])
{
    BinkBitWriter bw;
    BinkBits bits;
    int16_t back[64];
    int written;
    int counted;

    bink_bw_init(&bw);
    written = bink_enc_emit_residue(&bw, res);
    if (written < 0) {
        bink_bw_free(&bw);
        return 1;
    }
    counted = bink_enc_emit_residue(NULL, res);
    if (counted != written) {
        bink_bw_free(&bw);
        return 1;
    }
    bink_bits_init(&bits, bink_bw_data(&bw),
                   (size_t)((written + 7) / 8));
    if (bink_video_read_residue(&bits, back) != 0) {
        bink_bw_free(&bw);
        return 1;
    }
    bink_bw_free(&bw);
    return memcmp(res, back, sizeof(back)) != 0;
}

static int test_residue_writer_roundtrip(void)
{
    int16_t res[64];
    uint32_t seed = 12345u;
    int trial;
    int i;

    /* all zero */
    memset(res, 0, sizeof(res));
    if (residue_trial(res) != 0) {
        fprintf(stderr, "FAIL residue: zero block\n");
        return 1;
    }
    /* one pixel, both signs, extreme magnitudes */
    for (i = 0; i < 3; i++) {
        memset(res, 0, sizeof(res));
        res[i * 17] = (int16_t)(i == 0 ? 1 : (i == 1 ? -1 : 255));
        if (residue_trial(res) != 0) {
            fprintf(stderr, "FAIL residue: single %d\n", res[i * 17]);
            return 1;
        }
    }
    memset(res, 0, sizeof(res));
    res[5] = -255;
    if (residue_trial(res) != 0) {
        fprintf(stderr, "FAIL residue: -255\n");
        return 1;
    }
    /* sparse random blocks */
    for (trial = 0; trial < 32; trial++) {
        memset(res, 0, sizeof(res));
        for (i = 0; i < 64; i++) {
            seed = seed * 1103515245u + 12345u;
            if (((seed >> 16) & 7u) == 0)
                res[i] = (int16_t)((int)((seed >> 20) % 511u) - 255);
        }
        if (residue_trial(res) != 0) {
            fprintf(stderr, "FAIL residue: sparse trial %d\n", trial);
            return 1;
        }
    }
    /* 126 significant bits must fit */
    memset(res, 0, sizeof(res));
    for (i = 0; i < 63; i++)
        res[i] = 3;
    if (residue_trial(res) != 0) {
        fprintf(stderr, "FAIL residue: 126 bits\n");
        return 1;
    }
    /* 128 significant bits must be rejected */
    memset(res, 0, sizeof(res));
    for (i = 0; i < 64; i++)
        res[i] = 3;
    if (bink_enc_emit_residue(NULL, res) >= 0) {
        fprintf(stderr, "FAIL residue: 128 bits accepted\n");
        return 1;
    }
    return 0;
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --build build && ./build/tests/test_encoder`
Expected: compile errors naming `bink_enc_emit_residue` and
`bink_video_read_residue`.

- [ ] **Step 3: Add the decoder hook**

In `src/bink_video.c`, after `read_residue`:

```c
int bink_video_read_residue(BinkBits *bits, int16_t block[64])
{
    int budget;

    memset(block, 0, sizeof(int16_t) * 64);
    budget = bink_bits_read(bits, 7);
    if (budget < 0)
        return -1;
    return read_residue(bits, block, budget);
}
```

Declare it in `src/bink_video.h` beside `bink_video_read_coeffs`, with a
comment saying it is a test hook that mirrors `decode_plane`.

- [ ] **Step 4: Implement `bink_enc_emit_residue`**

In `src/encoder/bink_enc_dct.c`, after `bink_enc_emit_coeffs`. Reuse
`CoeffBits` and `coeff_put` so writing and counting share one path, and
`coeff_plane` for the top plane.

Shape: validate every value is in -255..255 and count the significant
bits (`popcount(|v|)`); reject when that exceeds 127. Write the budget in
7 bits and `mask_bits = max(coeff_plane(v))` in 3 bits. Then for
`plane = mask_bits` down to 0, with `mask = 1 << plane`:

1. For each coefficient already significant, in the order it became
   significant, write one bit: 1 when `|v| & mask`.
2. Walk the tree exactly as `read_residue` does — same roots
   `(4,0), (24,0), (44,0), (0,2)`, same mode 0/1/2/3 transitions, deferred
   nodes pushed at `list_start`. When a coefficient is resolved at this
   plane (its `coeff_plane` equals `plane`), write 0 then a sign bit
   (1 negative) and append it to the significance list; when it is not
   significant yet, write 1 and defer it as mode 3.

Declare it in `src/encoder/bink_enc_dct.h` with the comment from the
Interfaces block.

- [ ] **Step 5: Run the test to verify it passes**

Run: `cmake --build build && ./build/tests/test_encoder`
Expected: exit 0. If a round-trip case fails, the writer disagrees with
`read_residue` — fix the writer, never the decoder.

- [ ] **Step 6: Run the whole suite**

Run: `ctest --test-dir build`
Expected: 100% tests passed.

- [ ] **Step 7: Commit**

```bash
git add src/bink_video.c src/bink_video.h src/encoder/bink_enc_dct.c \
        src/encoder/bink_enc_dct.h tests/test_encoder.c
git commit -m "encoder: add the RESIDUE bit-plane writer"
```

---

### Task 2: Use residue blocks in the encoder

**Files:**
- Modify: `src/encoder/bink_enc_video.c` (block layout comment, candidate
  scoring, emission, painting, motion-vector bundling)
- Test: `tests/test_encoder.c`

**Interfaces:**
- Consumes: `bink_enc_emit_residue`, `bink_idct_add` is not used here;
  `bink_add_pixels8` from `bink_dsp.h`; the existing `BestDct`,
  `consider_candidate`, `build_residual`, `copy_predicted`, `have_pred`
  and `best_mx`/`best_my`.
- Produces: nothing outside `bink_enc_video.c`. `BestDct.kind` gains 3
  meaning `RESIDUE_BLOCK`.

- [ ] **Step 1: Write the failing test**

```c
static int test_residue_block_used(void)
{
    enum { W = 64, H = 48 };
    /* frame 0: fine detail; frame 1: the same with two pixels per 8x8 block
     * raised by 60, so every block has a sparse, large residual */
    /* assert: encode verifies, packet 1 is less than half of packet 0,
       and the decoded Y plane is exact (max error 0) */
}

static int test_residue_odd_size(void)
{
    /* same idea on a 17x9 plane, where padding and edge predictors clamp */
    /* assert: encode verifies and the decoded picture is within 24 / 4.0 */
}
```

Write them out in full in the same style as `test_inter_dct`: `mem_source`
over malloc'd RGB, `bink_enc_options_default`, `opt.keyframe_interval = 2`,
`opt.motion_range = 0`, `packet_sizes_all` for the two packet sizes, and a
local Y-only comparison with max error 0 for the first test (compare
`bink_get_plane(movie, BINK_PLANE_Y)` against `bink_enc_rgb_to_yuv` of
frame 1, visible pixels only).

- [ ] **Step 2: Run to verify they fail**

Run: `cmake --build build && ./build/tests/test_encoder`
Expected: `test_residue_block_used` FAILS — without residue the blocks go
to DCT (error non-zero) or raw (packet over half the keyframe).

- [ ] **Step 3: Score the residue candidate**

In `bink_enc_video.c`, in the block-selection `else` branch, after the
inter candidate loop:

- When `have_pred`, call `bits = bink_enc_emit_residue(NULL, residual);`
  where `residual` is the array `build_residual` already fills for the
  inter candidate.
- Estimate `bits + 16` (the motion-vector pair rides the X/Y bundles) and
  pass it to `consider_candidate(&best, &candidate, RESIDUE_BLOCK, bits,
  0)`, where `candidate` is a local `EncBlock` whose `t` is the residual.
  Its distortion is zero, so the tie-break prefers it.
- Add `RESIDUE_BLOCK` to the chosen-kind handling: copy `candidate.t`,
  set `block->mx`/`block->my` from `best_mx`/`best_my`.

Also extend the `EncBlock.t` comment to say it holds DCT coefficients for
INTRA/INTER and the signed residual for RESIDUE.

- [ ] **Step 4: Emit, bundle and paint**

- `emit_rows_dct`: for `RESIDUE_BLOCK`, call
  `bink_enc_emit_residue(bw, block->t)` in place of the coefficient call.
- `encode_plane`: append `mx`/`my` for `RESIDUE_BLOCK` beside MOTION and
  INTER. Append no DC for it.
- `paint_block`: for `RESIDUE_BLOCK`, copy the predictor at `(mx,my)` into
  `dst` with the same overlap-safe temporary MOTION uses, then
  `bink_add_pixels8(dst, block->t, stride)`.

- [ ] **Step 5: Run the tests**

Run: `cmake --build build && ./build/tests/test_encoder && ctest --test-dir build`
Expected: both new tests pass and every earlier test still passes.

- [ ] **Step 6: Measure and record**

Encode the 8-frame 320x240 photographic clip from the previous work and
record its size in the commit message next to the DCT-only figure
(38,616 bytes), so a regression is visible.

- [ ] **Step 7: Commit**

```bash
git add src/encoder/bink_enc_video.c tests/test_encoder.c
git commit -m "encoder: emit RESIDUE blocks for sparse residuals"
```

---

### Task 3: Documentation

**Files:**
- Modify: `docs/encoder.md`
- Modify: `README.md`

- [ ] **Step 1: Update `docs/encoder.md`**

Add `residue` to the block-type sentence, and say what it costs and when
it wins: an exact motion-compensated residual coded as a bit plane, used
when it is cheaper than a DCT block, which is when few pixels differ.

- [ ] **Step 2: Update the README summary**

One clause, not a paragraph.

- [ ] **Step 3: Check for stale claims**

Run: `grep -n "lossless\|bit-exact" docs/encoder.md README.md`
Expected: no hit that is now wrong.

- [ ] **Step 4: Build and run the suite**

Run: `cmake --build build && ctest --test-dir build`
Expected: 100% tests passed.

- [ ] **Step 5: Commit**

```bash
git add docs/encoder.md README.md
git commit -m "docs: describe RESIDUE blocks"
```

---

## Self-Review

- **Spec coverage:** writer and hook → Task 1; selection, storage,
  emission and painting → Task 2; the docs sentence → Task 3. The spec's
  "budget is a cap" and "storage in `t`" appear in Global Constraints and
  Task 2 Step 3.
- **Review Focus tests:** 255/-255, all-zero, 127 vs 128, negative sign
  and the 17x9 plane all have a named test in Task 1 or Task 2.
- **Type consistency:** `bink_enc_emit_residue` takes
  `const int16_t[64]` everywhere; `bink_video_read_residue` takes
  `BinkBits *` and `int16_t[64]`; `BestDct.kind` values 0..3 are used
  consistently.
- **Proportion:** three tasks, signatures and test assertions over bodies;
  the only long code block is the writer's test, which the assertions
  require.
