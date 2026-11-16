# Encoder DCT (video blocks + audio coefficients) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make libbink's Bink 1 encoder emit real intra/inter DCT video blocks and full DCT audio coefficients, so photographic video and tones compress and decode recognizably.

**Architecture:** Video gets a forward transform that inverts the decoder's integer IDCT exactly, a coefficient-tree writer that mirrors the decoder's bit-plane state machine, and DC bundle emission; existing skip/motion/fill/pattern/raw blocks stay as lossless fallbacks selected against DCT by estimated bits within an error cap. Audio gets the decoder's own time-domain buffer chosen with one block of lookahead, an unnormalized DCT-II matching `dct_iii`, per-band quantizers, and zero-run RLE.

**Tech Stack:** C99, CMake, the existing `BinkBitWriter`/`BinkBits` bit I/O and `bink_idct_put`/`bink_idct_add` DSP helpers. No new dependencies.

**Spec:** `docs/superpowers/specs/2026-11-15-encoder-dct-design.md`

## Global Constraints

- License headers stay LGPL-2.1-or-later; keep all FFmpeg attribution and add a short provenance comment where the forward transform and the tree writer are introduced.
- Do not change the public API in `src/encoder/bink_encoder.h`.
- Match existing style: 4-space indent, C99 declarations, existing error handling, no new global mutable state (a lazily built constant table is fine if it is idempotent).
- Video forward transform constant: the ideal `C` from `A1=2896/2048.0`, `A2=2217/2048.0`, `A3=3784/2048.0`, `A4=-5352/2048.0`, inverted; `coef = Ci * (256*P) * Ci^T`.
- Video quantization: `t[i] = round(coef[scan[i]] * 2048 / quant[i])`; reject any `|t| >= 32768`.
- Video reconstruction: `coef[scan[i]] = (t[i] * quant[i]) >> 11` with C integer truncation, exactly like `unquantize_dct_coeffs`.
- Video candidate quantizers: `{0, 2, 4, 6, 8, 10, 12, 14}`.
- Video block acceptance caps: max absolute pixel error `<= 24`, RMSE `<= 4.0`.
- Video verify caps: max absolute error `<= 32`, RMSE `<= 5.0` per plane.
- Audio forward transform: `C[k] = sum(x[n] * cos(pi/N * (n+0.5) * k))`, unnormalized; `E[0] = C[0]/2`, `E[m] = C[m]`.
- Audio per-band target: smallest index with `maxabs / quant[index] <= 1023`, clamped to 95.
- Audio RLE table: `{2,3,4,5,6,8,9,10,11,12,13,14,15,16,32,64}`, each entry covering `entry * 8` coefficients.
- `motion_range == 0` disables the motion *search* only; inter prediction at `(0,0)` stays available on non-keyframes.
- `test_encoder` must run without media files; media-dependent tests skip unless `BINK_TEST_MEDIA` is set.

## Review Focus

- **Odd plane sizes** (`17x9` in the existing suite): edge 8x8 blocks read replicated last row/column. The forward transform and the coefficient writers must never read past `plane_w`/`plane_h`, and painting must reconstruct the full padded block.
- **Inter DC range**: a residual DC outside `-1023..1023` cannot be coded. It must be rejected and fall through to intra or raw, never truncated into a wrong picture.
- **Wrap near 0 and 255**: inter adds the IDCT through `unsigned char`. A block at 250 with a +10 residual must not wrap to 4. Reject on measured error, do not assume.
- **Short audio frames**: at 22050 Hz `frame_len` is 512, so the coefficient tail after index 2 is 510 = 63*8 + 6. Chunk clamping must handle the partial final chunk.
- **First/last audio block and silence**: the first block skips the decoder's crossfade; the last block's lookahead is past the end of the source. Neither may read out of bounds, divide by zero, or emit a NaN.

---

### Task 1: DCT coefficient math and coefficient-tree writer

**Files:**
- Create: `src/encoder/bink_enc_dct.h`, `src/encoder/bink_enc_dct.c`
- Modify: `src/bink_video.h` (add accessor + test-hook declarations)
- Modify: `src/bink_video.c` (implement accessors and `bink_video_read_coeffs`)
- Modify: `CMakeLists.txt` (add `src/encoder/bink_enc_dct.c` to `bink_encoder`)
- Test: `tests/test_encoder.c`

**Interfaces:**
- Consumes: `bink_scan`, `bink_intra_quant`, `bink_inter_quant` from `src/binkdata.h`; `bink_idct_put` from `bink_dsp.h`; `read_dct_coeffs` (static) in `src/bink_video.c`.
- Produces (declared in `src/encoder/bink_enc_dct.h`):
  - `const uint8_t *bink_enc_scan(void);`
  - `const int32_t *bink_enc_intra_quant(int index);`
  - `const int32_t *bink_enc_inter_quant(int index);`
  - `void bink_enc_fdct(const uint8_t pixels[64], double coef[64]);`
  - `int bink_enc_quantize(const double coef[64], const int32_t quant[64], int16_t t[64]);`
  - `void bink_enc_unquantize(const int16_t t[64], const int32_t quant[64], int32_t coef[64]);`
  - `int bink_enc_cbits(const int16_t t[64]);`
  - `int bink_enc_emit_coeffs(BinkBitWriter *bw, const int16_t t[64], int cbits, int quant_index);` — returns the number of bits, or `-1`; writes only when `bw != NULL`.
- Produces (declared in `src/bink_video.h`):
  - `const uint8_t *bink_video_scan(void);`
  - `const int32_t *bink_video_intra_quant(int index);`
  - `const int32_t *bink_video_inter_quant(int index);`
  - `int bink_video_read_coeffs(BinkBits *bits, int16_t t[64]);` — reads 4-bit `cbits`, the tree bits, and the 4-bit quantizer, and returns the quantizer index or `-1`. `t` is scan-ordered and `t[0]` is always 0 (DC is bundled).

- [ ] **Step 1: Write the failing tests**

Add to `tests/test_encoder.c`, and register them in `main` next to the existing video tests:

```c
static int test_dct_roundtrip(void)
{
    /* gradient, hard edge, flat, and pseudo-random blocks */
    const int kinds = 4;
    int kind;
    int failures = 0;

    for (kind = 0; kind < kinds; kind++) {
        uint8_t pixels[64];
        double coef[64];
        int16_t t[64];
        int32_t recon[64];
        uint8_t out[64];
        const int32_t *quant = bink_enc_intra_quant(0);
        int i;
        int maxerr = 0;

        for (i = 0; i < 64; i++) {
            int x = i & 7;
            int y = i >> 3;

            if (kind == 0)
                pixels[i] = (uint8_t)(20 + x * 25 + y * 2);
            else if (kind == 1)
                pixels[i] = x < 4 ? 230 : 20;
            else if (kind == 2)
                pixels[i] = 123;
            else
                pixels[i] = (uint8_t)((i * 37 + 11) & 255);
        }
        bink_enc_fdct(pixels, coef);
        if (bink_enc_quantize(coef, quant, t) != 0)
            return 1;
        bink_enc_unquantize(t, quant, recon);
        bink_idct_put(out, 8, recon);
        for (i = 0; i < 64; i++) {
            int e = out[i] - (int)pixels[i];
            if (e < 0)
                e = -e;
            if (e > maxerr)
                maxerr = e;
        }
        if (maxerr > 2)
            failures++;
    }
    return failures;
}

static int test_coeff_writer_roundtrip(void)
{
    BinkBitWriter bw;
    int16_t t[64];
    int16_t back[64];
    int seed = 12345;
    int trial;

    for (trial = 0; trial < 64; trial++) {
        BinkBits bits;
        int cbits;
        int q;
        int i;
        int bits_written;
        int bits_counted;

        memset(t, 0, sizeof(t));
        /* sparse AC values; DC is bundled and must stay 0 here */
        for (i = 1; i < 64; i++) {
            seed = seed * 1103515245 + 12345;
            if (((seed >> 16) & 3) == 0)
                t[i] = (int16_t)(((seed >> 20) % 2047) - 1023);
        }
        if (trial == 0)
            memset(t, 0, sizeof(t)); /* all-zero AC must round-trip too */

        cbits = bink_enc_cbits(t);
        bink_bw_init(&bw);
        bits_written = bink_enc_emit_coeffs(&bw, t, cbits, trial & 15);
        if (bits_written < 0) {
            bink_bw_free(&bw);
            return 1;
        }
        bits_counted = bink_enc_emit_coeffs(NULL, t, cbits, trial & 15);
        if (bits_counted != bits_written) {
            bink_bw_free(&bw);
            return 1;
        }
        bink_bits_init(&bits, bink_bw_data(&bw),
                       (size_t)((bits_written + 7) / 8));
        q = bink_video_read_coeffs(&bits, back);
        bink_bw_free(&bw);
        if (q != (trial & 15))
            return 1;
        if (memcmp(t, back, sizeof(t)) != 0)
            return 1;
    }
    return 0;
}

static int test_cbits(void)
{
    int16_t t[64];

    memset(t, 0, sizeof(t));
    if (bink_enc_cbits(t) != 0)
        return 1;
    t[5] = 1;
    if (bink_enc_cbits(t) != 1)
        return 1;
    t[5] = -1023;
    if (bink_enc_cbits(t) != 10)
        return 1;
    t[5] = 1024;
    if (bink_enc_cbits(t) != 11)
        return 1;
    return 0;
}

static int test_quantize_rejects_out_of_range(void)
{
    double coef[64];
    int16_t t[64];
    const int32_t *quant = bink_enc_intra_quant(0);
    int i;

    memset(coef, 0, sizeof(coef));
    for (i = 0; i < 64; i++)
        coef[i] = 1.0e7;
    return bink_enc_quantize(coef, quant, t) == 0 ? 1 : 0;
}
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --build build && ./build/tests/test_encoder`
Expected: compiles fail on missing `bink_enc_fdct` etc. (or, if the test binary is not rebuilt, link errors). No pass.

- [ ] **Step 3: Add the decoder accessors and test hook**

In `src/bink_video.c`, after `unquantize_dct_coeffs`:

```c
const uint8_t *bink_video_scan(void) { return bink_scan; }

const int32_t *bink_video_intra_quant(int index)
{
    return (index < 0 || index > 15) ? NULL : bink_intra_quant[index];
}

const int32_t *bink_video_inter_quant(int index)
{
    return (index < 0 || index > 15) ? NULL : bink_inter_quant[index];
}

int bink_video_read_coeffs(BinkBits *bits, int16_t t[64])
{
    int32_t block[64];
    int coef_count;
    int coef_idx[64];
    int i;
    int q;

    memset(block, 0, sizeof(block));
    q = read_dct_coeffs(bits, block, bink_scan, &coef_count, coef_idx, -1);
    if (q < 0)
        return -1;
    for (i = 0; i < 64; i++)
        t[i] = (int16_t)block[bink_scan[i]];
    return q;
}
```

Declare all four in `src/bink_video.h`. This is the inverse of the writer; it exists so the writer can be tested against the decoder's real parser instead of a duplicate model.

- [ ] **Step 4: Implement `src/encoder/bink_enc_dct.c`**

- `bink_enc_scan()`, `bink_enc_intra_quant()`, `bink_enc_inter_quant()` return `bink_video_scan()`, `bink_video_intra_quant()`, `bink_video_inter_quant()`.
- `bink_enc_fdct`: build `C` from the `col()` function in the spec's "Forward transform" section, invert it once into a static `double Ci[8][8]` (compute on first call, idempotent, no cleanup needed), then `coef = Ci * (256*P) * Ci^T` with two 8x8 multiplies over doubles. Add a provenance comment pointing at the decoder's `IDCT_TRANSFORM`.
- `bink_enc_quantize`: for `i` in 0..63, `t[i] = (int16_t)lround(coef[scan[i]] * 2048.0 / quant[i])`; if any `|t[i]| >= 32768`, return `-1`.
- `bink_enc_unquantize`: `coef[scan[i]] = (int32_t)(((int64_t)t[i] * quant[i]) >> 11)` for `i` 0..63 (the decoder truncates; `t[0]` is the DC).
- `bink_enc_cbits`: bit length of the largest-magnitude AC entry, `0` when all AC are zero.
- `bink_enc_emit_coeffs`: write `cbits` in 4 bits, then reproduce the decoder's `read_dct_coeffs` loop exactly (root list `(4,0),(24,0),(44,0),(1,3),(2,3),(3,3)`, `list_start`/`list_end`, mode 0/1/2/3 transitions, deferred nodes pushed at `list_start`). Decide one bit per node from each coefficient's target plane `tp[i] = floor(log2(|t[i]|))` for nonzero, `-1` for zero:
  - group node opens iff some coefficient it will code or spawn has `tp == p`;
  - mode-3 node gets `1` iff `tp[c] == p`;
  - inside an opened group, a coefficient with `tp == p` gets `0` plus its value (`cbits == 0`: one sign bit; else `|t| - 2^p` in `p` bits then a sign bit, 1 = negative), and any other coefficient gets `1` which defers it as mode 3.
  Then write `quant_index` in 4 bits. When `bw == NULL`, count the same bits and return the total. A coefficient whose plane is above the current loop indicates a bug: return `-1`.

- [ ] **Step 5: Run the tests to verify they pass**

Run: `cmake --build build && ./build/tests/test_encoder`
Expected: all tests pass, including the four new ones. If `test_coeff_writer_roundtrip` fails, the writer and `read_dct_coeffs` disagree — fix the writer, never the decoder.

- [ ] **Step 6: Run the whole suite**

Run: `ctest --test-dir build`
Expected: 17 existing tests plus the updated encoder test all pass.

- [ ] **Step 7: Commit**

```bash
git add src/encoder/bink_enc_dct.c src/encoder/bink_enc_dct.h \
        src/bink_video.c src/bink_video.h CMakeLists.txt tests/test_encoder.c
git commit -m "encoder: add DCT coefficient math and coefficient tree writer"
```

---

### Task 2: Tolerance-based verify

**Files:**
- Modify: `src/encoder/bink_enc_verify.c:18-56` (replace `planes_match`)
- Test: `tests/test_encoder.c` (existing cases)

**Interfaces:**
- Consumes: `bink_get_plane`, `bink_get_stride` (already used).
- Produces: nothing new; `bink_enc_verify` keeps its signature.

- [ ] **Step 1: Note the current behavior**

Run: `./build/tests/test_encoder`
Expected: passes today with exact matching. Record that it passes; this task must not change that.

- [ ] **Step 2: Replace `planes_match` with a tolerance check**

Keep the loop over the three planes, but compute:

```
max |decoded - expected| over visible pixels
sqrt(sum((decoded - expected)^2) / count)
```

Accept when max `<= 32` and RMSE `<= 5.0` for every plane (`MAX_ERROR` and `MAX_RMSE` as file-local constants). On failure, `snprintf` the offending plane and both values into `err` so a failing verify says which plane and how far off it is, then return 0. Update the call site's message from `"decoded picture does not match"` to `"decoded picture exceeds tolerance"`.

- [ ] **Step 3: Run the suite**

Run: `cmake --build build && ./build/tests/test_encoder && ctest --test-dir build`
Expected: still all pass. Exact output is trivially within tolerance; the lossy case is exercised by Task 3.

- [ ] **Step 4: Commit**

```bash
git add src/encoder/bink_enc_verify.c tests/test_encoder.c
git commit -m "encoder: accept lossy pictures in --verify within a tolerance"
```

---

### Task 3: Intra DCT blocks

**Files:**
- Modify: `src/encoder/bink_enc_video.c` (EncBlock, BundleWalk, DC bundles, selection, painting)
- Test: `tests/test_encoder.c`

**Interfaces:**
- Consumes: `bink_enc_*` from Task 1; `bink_idct_put`; the existing `src_at`, `plan_chunks`, `walk_*`, `BundleWalk`.
- Produces: `EncBlock` gains `int16_t t[64]; uint8_t quant; uint8_t cbits;`. `BundleWalk` gains `int item_size;` and `walk_init` gains an `item_size` parameter after `len_bits`. No header changes.

- [ ] **Step 1: Write the failing test**

```c
static int test_intra_dct(void)
{
    enum { W = 64, H = 48 };
    mem_source mem;
    bink_enc_options opt;
    char path[1024];
    char err[256];
    bink_enc_status status;
    uint8_t *rgb = (uint8_t *)malloc((size_t)W * H * 3);
    uint8_t *y = (uint8_t *)malloc((size_t)W * H);
    uint8_t *u = (uint8_t *)malloc((size_t)(W / 2) * (H / 2));
    uint8_t *v = (uint8_t *)malloc((size_t)(W / 2) * (H / 2));
    unsigned x;
    unsigned yy;
    bink movie;
    struct stat st;
    int rc = 1;

    for (yy = 0; yy < H; yy++) {
        for (x = 0; x < W; x++) {
            size_t p = ((size_t)yy * W + x) * 3;

            rgb[p + 0] = (uint8_t)(x * 3 + yy);
            rgb[p + 1] = (uint8_t)(x + yy * 2);
            rgb[p + 2] = (uint8_t)(255 - x - yy);
        }
    }
    memset(&mem, 0, sizeof(mem));
    mem.frames = rgb;
    mem.width = W;
    mem.height = H;
    mem.count = 1;
    mem.fail_index = -1;
    bink_enc_options_default(&opt);
    opt.motion_range = 0;
    path_for(path, sizeof(path), "intra_dct.bik");
    err[0] = '\0';
    status = encode_mem(&mem, &opt, path, 0, err, sizeof(err));
    if (expect_ok(status, err, "intra dct") != 0)
        goto done;
    if (stat(path, &st) != 0)
        goto done;
    /* Lossless fallback would be ~W*H + 2*(W/2)*(H/2) bytes of block data. */
    if ((long)st.st_size >= (long)(W * H + 2 * (W / 2) * (H / 2) ))
        goto done;
    movie = bink_open_file(path, BINK_MODE_DISK);
    if (movie == NULL)
        goto done;
    bink_enc_rgb_to_yuv(rgb, W, H, y, u, v);
    if (bink_enable_video(movie, 1) != 0)
        goto close_movie;
    {
        char status = bink_first(movie);

        if (status != BINK_MORE && status != BINK_LAST)
            goto close_movie;
    }
    if (!planes_within(movie, y, u, v, W, H, 24, 4.0))
        goto close_movie;
    rc = 0;
close_movie:
    bink_close(movie);
done:
    remove(path);
    free(rgb);
    free(y);
    free(u);
    free(v);
    return rc;
}
```

`planes_within` is a test helper modelled on `bink_enc_verify`'s check: per plane, max absolute error and RMSE against the given planes with the given caps. Add it beside `expect_ok`.

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --build build && ./build/tests/test_encoder`
Expected: FAIL — the file is larger than the raw plane size because every block is `RAW_BLOCK`.

- [ ] **Step 3: Add the DC bundle writer and `item_size` to the bundle walk**

- `EncBlock` gains `int16_t t[64]; uint8_t quant; uint8_t cbits;` after `my`. Do not add a second unquantized array: `bink_enc_unquantize` recovers it at paint time.
- `BundleWalk` gains `item_size`; `walk_init` takes it after `len_bits`; `walk_row` uses `walk->data + walk->chunks[walk->chunk_index].off * (size_t)walk->item_size`. Every existing call passes `1`.
- Add `emit_dcs(BinkBitWriter *bw, const uint8_t *bytes, size_t count, SymbolState *state, int has_sign)` plus the two adapters `emit_intra_dcs` and `emit_inter_dcs` matching the emit-callback signature. View `bytes` as `const int16_t *`. For each chunk: write the first value absolutely (11 bits when `has_sign == 0`; 10 bits plus a sign bit when nonzero and `has_sign == 1`); then for each group of up to eight remaining values, compute the smallest `bsize` holding every `|delta|` (0 when all are zero), write `bsize` in 4 bits, then each `|delta|` in `bsize` bits plus a sign bit when nonzero, accumulating the running value exactly like `bink_bundle_read_dcs`.
- In `encode_plane`, add `int16_t *intra_dc`, `int16_t *inter_dc`, `size_t *intra_rows`, `size_t *inter_rows`, and their chunks. Allocate the DC buffers for `blocks_x * blocks_y` values each. Append `block->t[0]` to the matching buffer for intra/inter blocks and set the row counts in values. Call `plan_chunks` for both with `lens[BINK_SRC_INTRA_DC]` / `lens[BINK_SRC_INTER_DC]`. Free every new buffer in the `done:` block beside the existing ones.
- Wire the two walks with `item_size = 2` and the DC emitters, and stop passing `zeros` to them.

- [ ] **Step 4: Add intra block selection**

Replace the final `else { RAW }` branch with a comparison. Keep skip, motion, fill, and pattern exactly as they are. For blocks with three or more unique values:

- Raw candidate: `op = RAW_BLOCK`, `memcpy(block->raw, desired, 64)`, estimated 512 bits.
- Intra candidate: `bink_enc_fdct(desired, coef)`, then for each `q` in `{0,2,4,6,8,10,12,14}`: quantize with `bink_enc_intra_quant(q)`; skip the candidate when quantization fails, when `t[0] < 0 || t[0] > 2047`, or when `cbits > 15`; unquantize, `bink_idct_put` into a stride-8 scratch block, and measure max error and RMSE over all 64 pixels. Reject candidates over max 24 or RMSE 4.0. Estimated bits: `11 + 4 + 4 + tree bits` (`4` for `cbits`, `4` for the quantizer index, `11` for the DC estimate).
- Pick the lowest estimated bits, ties toward lower distortion. Store `block->t`, `block->quant`, `block->cbits`, set `op = INTRA_BLOCK`.

Factor the candidate evaluation into a static helper so Task 4 reuses it:

```c
/* Scores one DCT candidate. pred is NULL for intra. Returns bits, or -1. */
static int score_dct(const uint8_t desired[64], const uint8_t *pred,
                     const int32_t *quant, int q, EncBlock *block,
                     int *maxerr, double *rmse);
```

- [ ] **Step 5: Interleave the DCT bits with their block row**

Add `emit_rows_dct(BinkBitWriter *bw, BundleWalk *walks, int walk_count, int rows, const EncBlock *blocks, int blocks_x)`. For each row it calls `walk_row` for every bundle in order, then for each block in the row left to right, if `op` is `INTRA_BLOCK` or `INTER_BLOCK`, calls `bink_enc_emit_coeffs(bw, block->t, block->cbits, block->quant)`. Use it for the writing pass only; the frequency pass keeps calling `emit_rows(NULL, ...)` with `write == 0` so no DCT bits are counted.

- [ ] **Step 6: Paint DCT blocks**

Extend `paint_block` (add a `const int32_t *quant` argument or select from `block->op` internally):

- `INTRA_BLOCK`: `bink_enc_unquantize(block->t, quant_for(op), tmp)` then `bink_idct_put(dst, stride, tmp)`.
- `INTER_BLOCK` in Task 4.

`quant_for` returns `bink_enc_intra_quant(block->quant)` or `bink_enc_inter_quant(block->quant)` by `op`.

- [ ] **Step 7: Run the tests**

Run: `cmake --build build && ./build/tests/test_encoder && ctest --test-dir build`
Expected: `test_intra_dct` passes; all earlier tests still pass. `--verify` now exercises the tolerance path on a lossy file.

- [ ] **Step 8: Commit**

```bash
git add src/encoder/bink_enc_video.c tests/test_encoder.c
git commit -m "encoder: emit intra DCT video blocks with bundled DC values"
```

---

### Task 4: Inter DCT blocks

**Files:**
- Modify: `src/encoder/bink_enc_video.c`
- Test: `tests/test_encoder.c`

**Interfaces:**
- Consumes: `score_dct`, the DC bundle writer, `emit_rows_dct`, `bink_enc_inter_quant`, `bink_idct_add`.
- Produces: nothing new outside the file.

- [ ] **Step 1: Write the failing tests**

```c
static int test_inter_dct(void)
{
    enum { W = 64, H = 48, N = 3 };
    /* three frames: the same gradient shifted right by 3 px each frame */
    /* assert: keyframe 0 is intra; frames 1 and 2 use smaller packets;
       every decoded plane is within 24 / 4.0 of its source frame */
}

static int test_inter_dct_no_motion_search(void)
{
    /* frame 0: a gradient; frame 1: the same gradient plus a +3
       brightness offset, so no exact match exists. keyframe_interval = 2,
       motion_range = 0. Skip cannot apply, the search is off, so the
       P-frame must use inter at (0,0). Assert decode succeeds, the
       picture is within tolerance, and the P-frame packet is much
       smaller than the keyframe packet. */
}

static int test_inter_wrap_safety(void)
{
    /* frame 0: a block at 250 and a block at 5; frame 1: the same blocks
       with a +10 step. motion_range = 0 so prediction is at (0,0) and
       the residual is real. Encode, decode, assert no pixel error above
       32; the near-255 block must not wrap to a dark value */
}
```

Reuse `packet_sizes` for the per-frame packet check and `planes_within` for the picture check. Use `bink_enc_options_default`, set `opt.keyframe_interval` and `opt.motion_range` explicitly.

- [ ] **Step 2: Run to verify they fail**

Run: `cmake --build build && ./build/tests/test_encoder`
Expected: `test_inter_dct` FAILS — P-frames fall back to raw/fill and the packet is not smaller; `test_inter_wrap_safety` FAILS or is satisfied by raw, which Step 3 changes.

- [ ] **Step 3: Track the best-SSD motion vector**

In the existing search, when no exact `visible_match` is found, keep the vector with the lowest sum of squared differences over the visible pixels. Store it in local `best_mx`/`best_my`. Exact matches still break early. Do not change `--motion-range` bounds or `motion_in_range`. When `motion_range == 0`, set `best_mx = best_my = 0` so inter prediction at `(0,0)` is still evaluated.

- [ ] **Step 4: Add the inter candidate**

Replace the raw-vs-intra selection with raw-vs-intra-vs-inter on non-keyframes:

- Predictor: copy the 8x8 reference block at `(best_mx, best_my)` exactly as `MOTION_BLOCK` painting does, into a local `uint8_t predicted[64]`.
- Residual: `res[y][x] = clamp(desired - predicted, -predicted, 255 - predicted)`.
- `bink_enc_fdct(res, coef)`, then the same quantizer loop with `bink_enc_inter_quant(q)`.
- Skip a candidate when quantization fails, when `cbits > 15`, or when `t[0] < -1023 || t[0] > 1023`.
- Reconstruction: unquantize, copy `predicted` to a stride-8 scratch, `bink_idct_add(scratch, 8, tmp)`, then measure max error and RMSE against `desired`. The `unsigned char` wrap the decoder performs is exactly what `bink_idct_add` does; a wrapped block fails the caps automatically.
- Estimated bits: `10 + 4 + 4 + tree bits + 16` (16 approximates the motion vector pair).
- Store `mx`/`my` on the block and set `op = INTER_BLOCK`.

- [ ] **Step 5: Emit and paint inter blocks**

- `encode_plane` already appends motion values only for `MOTION_BLOCK`; also append `mx`/`my` for `INTER_BLOCK` so the decoder reads a vector.
- `paint_block` `INTER_BLOCK`: copy the reference block at `(mx,my)` with the existing overlap-safe temporary, unquantize `t`, and `bink_idct_add(dst, stride, tmp)`.

- [ ] **Step 6: Run the tests**

Run: `cmake --build build && ./build/tests/test_encoder && ctest --test-dir build`
Expected: all three new tests pass; `test_skip_and_motion` still passes with exact matches.

- [ ] **Step 7: Commit**

```bash
git add src/encoder/bink_enc_video.c tests/test_encoder.c
git commit -m "encoder: emit inter DCT residual blocks with best-SSD motion"
```

---

### Task 5: Audio forward DCT and full coefficient blocks

**Files:**
- Modify: `src/bink_audio.h` (replace the DC-only declaration)
- Modify: `src/bink_audio.c` (remove `dc_response`/`bink_audio_write_dc_block`; add the table builder and block writer)
- Modify: `src/encoder/bink_enc_audio.c` (overlap lookahead, block writer call)
- Test: `tests/test_encoder.c`

**Interfaces:**
- Consumes: `write_float` (static), `BinkAudioInfo`, `BinkBitWriter`.
- Produces (declared in `src/bink_audio.h`):
  - `float *bink_audio_dct_table(int frame_len);` — `frame_len * frame_len` floats, `tab[k*frame_len + t] = cos(pi/frame_len * (t + 0.5) * k)`, or `NULL`.
  - `int bink_audio_write_dct_block(BinkBitWriter *bw, const BinkAudioInfo *info, const float *const out[2], const float *tab);` — writes the 2-bit marker, then per channel the two floats, the band indices, and the coefficient chunks.
- Removes: `bink_audio_write_dc_block`, `dc_response`.

- [ ] **Step 1: Write the failing test**

```c
static int test_audio_sine_frequency(void)
{
    enum { RATE = 44100, SECONDS = 1, FREQ = 440 };
    /* mono 16-bit sine at amplitude 12000 */
    /* encode with bink_enc_options_default, motion_range 0 */
    /* decode with bink_open_file / bink_enable_audio / bink_get_audio */
    /* assert: decoded bytes > 0; RMS > 1000; zero crossings within
       1% of 2 * FREQ per second */
}
```

Add a helper that counts sign changes on the decoded `int16_t` stream. Register the test in `main`.

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --build build && ./build/tests/test_encoder`
Expected: FAIL — the DC-only encoder reproduces a slow envelope, not a 440 Hz waveform, so the zero-crossing count is far below `2 * FREQ`.

- [ ] **Step 3: Add the transform table and block writer to `src/bink_audio.c`**

- `bink_audio_dct_table`: `malloc(frame_len * frame_len * sizeof(float))`, fill with `cos(M_PI / frame_len * (t + 0.5) * k)` over doubles.
- `bink_audio_write_dct_block`:
  - Write `bink_bw_put(bw, 0, 2)` for the DCT marker.
  - Per channel: `C[k] = sum(tab[k*n + t] * out[ch][t])` over doubles; `E[0] = C[0] * 0.5`, `E[m] = C[m]`.
  - Write `write_float(bw, E[0] / info->root)` and `write_float(bw, E[1] / info->root)`.
  - Choose one quantizer index for the whole frame in this task: the smallest `q` in 0..95 with `max(|E[m]|) / info->quant[q] <= 1023` for `m >= 2`, clamped to 95, and write that index for all `info->num_bands` bands.
  - Encode `m = 2 .. frame_len-1` in 8-coefficient chunks: read the 4-bit RLE flag, then either a normal chunk or (next task) an RLE run; this task writes flag `0` and width 0 when the chunk is all zeros, otherwise the maximum bit width of the integer magnitudes, then each magnitude and a sign bit when nonzero. Clamp the final chunk to `frame_len`.
  - Do not align; the caller does.
- Delete `dc_response` and `bink_audio_write_dc_block`, and their declarations.

- [ ] **Step 4: Choose the encoder's output buffer with one block of lookahead**

In `bink_enc_audio.c`:

- Add to `BinkEncAudio`: `float *out;` (`channels * frame_len`), `float *tab;`.
- Allocate both in `bink_enc_audio_open`; `tab = bink_audio_dct_table(info.frame_len)`; free both in `bink_enc_audio_close`.
- Per block, read `block_out` samples at `source_pos` as today, plus `overlap_len` lookahead samples at `source_pos + block_out`. The lookahead read clamps `count` to `sample_count - (source_pos + block_out)`, and stays zero when that is not positive.
- Build `out[ch][i] = samples[i * channels + ch] / 32768.0f` for `i < block_out`, and `out[ch][block_out + i] = look[i * channels + ch] / 32768.0f` for `i < overlap_len`.
- Replace the mean computation and `bink_audio_write_dc_block` call with `bink_audio_write_dct_block(&writer, &audio->info, (const float *const *)out_ptrs, audio->tab)` followed by `bink_bw_align32` as today. `out_ptrs[0] = audio->out`, `out_ptrs[1] = audio->out + frame_len` when stereo, `NULL` otherwise.
- `source_pos` still advances by `block_out`.

- [ ] **Step 5: Run the tests**

Run: `cmake --build build && ./build/tests/test_encoder && ctest --test-dir build`
Expected: `test_audio_sine_frequency` passes; the existing silence and constant-tone tests still pass (a constant signal now produces only `E[0]`, so the decoded block stays flat).

- [ ] **Step 6: Commit**

```bash
git add src/bink_audio.c src/bink_audio.h src/encoder/bink_enc_audio.c \
        tests/test_encoder.c
git commit -m "encoder: write full Bink audio DCT coefficient blocks"
```

---

### Task 6: Adaptive per-band quantizers and zero-run RLE

**Files:**
- Modify: `src/bink_audio.c`
- Test: `tests/test_encoder.c`

**Interfaces:**
- Consumes: Task 5's block writer.
- Produces: unchanged signatures; `bink_audio_write_dct_block` now picks an index per band and emits RLE chunks.

- [ ] **Step 1: Write the failing tests**

```c
static int test_audio_sine_snr(void)
{
    /* 440 Hz at 44100, mono; encode; decode; compute SNR against the
       source over the decoded length; assert SNR >= 20.0 dB */
}

static int test_audio_size(void)
{
    /* the same sine: assert payload bytes < sample_count * 2 / 4 */
}

static int test_audio_stereo_channels(void)
{
    /* left 440 Hz, right 660 Hz; decode; per channel count zero
       crossings near 2*freq and assert the two channels differ */
}

static int test_audio_rates(void)
{
    /* 22050 and 48000 Hz sines: encode, decode, assert non-silent and
       dominant frequency within 2%; this covers frame_len 512 and 2048 */
}
```

- [ ] **Step 2: Run to verify they fail**

Run: `cmake --build build && ./build/tests/test_encoder`
Expected: `test_audio_sine_snr` FAILS (one global quantizer set by the loudest band makes low bands coarse) and `test_audio_size` FAILS (no zero-run RLE).

- [ ] **Step 3: Pick a quantizer index per band**

For each band `b`, let `lo = info->bands[b]` and `hi = info->bands[b + 1]` (the last band ends at `frame_len`), restricted to `m >= 2`. Compute `maxabs` over those coefficients. Choose the smallest `q` in 0..95 with `maxabs / info->quant[q] <= 1023`, clamped to 95; use 0 when `maxabs == 0`. Write one 8-bit index per band in order. Quantize each `E[m]` with its band's `quant` to an integer magnitude, recording the sign.

- [ ] **Step 4: Emit zero-run RLE**

At coefficient index `i`, if the next group of up to 8 coefficients is all zero:

- Find the largest RLE table entry `d` with `i + d * 8 <= frame_len` such that those `d * 8` coefficients are all zero. If `d >= 2`, write flag 1, the 4-bit table index, and width 0, then advance by `d * 8`.
- Otherwise write flag 0 and width 0 for the 8-coefficient group (clamped at `frame_len`).

If the group is not all zero, write flag 0 and a nonzero-width chunk: width is the maximum bit length of the integer magnitudes in the chunk, then each magnitude in `width` bits plus a sign bit when nonzero.

The final chunk may be shorter than 8; clamp `j` to `frame_len` on both encode and decode like the decoder's loop does.

- [ ] **Step 5: Run the tests**

Run: `cmake --build build && ./build/tests/test_encoder && ctest --test-dir build`
Expected: all audio tests pass, and the earlier video tests are unaffected.

- [ ] **Step 6: Commit**

```bash
git add src/bink_audio.c tests/test_encoder.c
git commit -m "encoder: adaptive per-band audio quantization with zero-run RLE"
```

---

### Task 7: Documentation and external compatibility

**Files:**
- Modify: `docs/encoder.md`
- Modify: `README.md`

**Interfaces:**
- Consumes: the behavior landed in Tasks 1–6.
- Produces: nothing code-visible.

- [ ] **Step 1: Update `docs/encoder.md`**

Replace the claims that the encoder is lossless and that audio is DC-only. Describe:

- Video uses intra DCT on keyframes and inter DCT on motion-compensated residuals, with skip/motion/fill/pattern/raw as lossless fallbacks.
- `--verify` means "decodes within tolerance" (max 32, RMSE 5.0 per plane), not bit-exact.
- Audio uses full Bink DCT coefficients with per-band quantizers and zero-run RLE.
- The quality constants are internal in v1 (quantizer set `{0,2,4,6,8,10,12,14}`, block caps 24 / 4.0, audio target 1023).

- [ ] **Step 2: Update the README summary**

One or two sentences matching the new behavior; do not overclaim.

- [ ] **Step 3: Check no stale claims remain**

Run: `grep -rn "DC-only\|bit-exact\|lossless" docs/encoder.md README.md`
Expected: no hit that still describes the whole encoder as DC-only or bit-exact. Mentions that specifically name the fallback block modes as lossless are fine.

- [ ] **Step 4: Build and run the full suite**

Run: `cmake --build build && ctest --test-dir build`
Expected: all tests pass.

- [ ] **Step 5: Manual VLC check**

Encode a short photographic clip and open it in `/Applications/VLC.app/Contents/MacOS/VLC`. Confirm the picture plays and the audio is recognizable. Record the file size next to the pre-change baseline (8 frames at 320x240 was about 905 KB).

- [ ] **Step 6: Commit**

```bash
git add docs/encoder.md README.md
git commit -m "docs: describe DCT video and audio encoding"
```

---

## Self-Review

- **Spec coverage:** Video transform/quantizer/tree/DC → Task 1 and Task 3; intra/inter selection → Tasks 3–4; wrap safety → Task 4; streaming/row interleave → Task 3 step 5; table access → Task 1; verify tolerance → Task 2; audio overlap/forward DCT/chunking → Task 5; per-band quant + RLE → Task 6; docs → Task 7. `motion_range == 0` → Task 4 step 3. Memory ceiling (`t` only, no `coef`) → Task 3 step 3, pinned by the Review Focus on maximum size and the spec's risk row.
- **Type consistency:** `score_dct`, `emit_rows_dct`, `bink_enc_emit_coeffs`, `bink_audio_write_dct_block`, and `bink_audio_dct_table` are declared once and used with the same signatures in later tasks.
- **Review Focus tests:** odd sizes → `test_intra_dct` uses 64x48 and the existing `odd` case stays; inter DC range → rejected in Task 4 step 4 and covered by `test_inter_wrap_safety`; wrap → `test_inter_wrap_safety`; short audio frame → `test_audio_rates` at 22050; first/last/silence → existing audio tests plus `test_audio_sine_frequency` (last block lookahead past the end).
- **Proportion:** seven tasks, signatures and test assertions over pasted bodies.
