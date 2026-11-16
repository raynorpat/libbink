#include "bink_enc_video.h"

#include "bink_dsp.h"
#include "bink_enc_dct.h"
#include "bink_video.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static int clamp8(int value)
{
    if (value < 0)
        return 0;
    if (value > 255)
        return 255;
    return value;
}

static void rgb_to_yuv_pixel(int r, int g, int b, int *y, int *u, int *v)
{
    *y = clamp8(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16);
    *u = clamp8(((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128);
    *v = clamp8(((112 * r - 94 * g - 18 * b + 128) >> 8) + 128);
}

void bink_enc_rgb_to_yuv(const uint8_t *rgb, unsigned width, unsigned height,
                         uint8_t *y, uint8_t *u, uint8_t *v)
{
    unsigned cw = (width + 1) >> 1;
    unsigned ch = (height + 1) >> 1;
    unsigned x;
    unsigned row;
    unsigned col;

    for (row = 0; row < height; row++) {
        for (x = 0; x < width; x++) {
            const uint8_t *p = rgb + ((size_t)row * width + x) * 3;
            int yy;
            int uu;
            int vv;

            rgb_to_yuv_pixel(p[0], p[1], p[2], &yy, &uu, &vv);
            y[(size_t)row * width + x] = (uint8_t)yy;
        }
    }

    for (row = 0; row < ch; row++) {
        for (col = 0; col < cw; col++) {
            int usum = 0;
            int vsum = 0;
            int count = 0;
            unsigned dy;
            unsigned dx;

            for (dy = 0; dy < 2; dy++) {
                unsigned sy = row * 2 + dy;

                if (sy >= height)
                    continue;
                for (dx = 0; dx < 2; dx++) {
                    unsigned sx = col * 2 + dx;
                    const uint8_t *p;
                    int yy;
                    int uu;
                    int vv;

                    if (sx >= width)
                        continue;
                    p = rgb + ((size_t)sy * width + sx) * 3;
                    rgb_to_yuv_pixel(p[0], p[1], p[2], &yy, &uu, &vv);
                    usum += uu;
                    vsum += vv;
                    count++;
                }
            }
            if (count == 0)
                count = 1;
            u[(size_t)row * cw + col] = (uint8_t)(usum / count);
            v[(size_t)row * cw + col] = (uint8_t)(vsum / count);
        }
    }
}

int bink_enc_planes_init(BinkEncPlanes *planes, unsigned long width,
                         unsigned long height)
{
    int i;

    memset(planes, 0, sizeof(*planes));
    for (i = 0; i < 3; i++) {
        size_t bytes;

        bink_plane_layout(width, height, i != 0, &planes->plane_w[i],
                          &planes->plane_h[i], &planes->blocks_x[i],
                          &planes->blocks_y[i], &planes->stride[i],
                          &planes->alloc_h[i]);
        bytes = (size_t)planes->stride[i] * (size_t)planes->alloc_h[i];
        planes->cur[i] = (uint8_t *)calloc(1, bytes);
        planes->ref[i] = (uint8_t *)calloc(1, bytes);
        if (planes->cur[i] == NULL || planes->ref[i] == NULL) {
            bink_enc_planes_free(planes);
            return -1;
        }
    }
    return 0;
}

void bink_enc_planes_free(BinkEncPlanes *planes)
{
    int i;

    if (planes == NULL)
        return;
    for (i = 0; i < 3; i++) {
        free(planes->cur[i]);
        free(planes->ref[i]);
        planes->cur[i] = NULL;
        planes->ref[i] = NULL;
    }
}

typedef struct {
    uint8_t *data;
    size_t len;
    size_t cap;
} ByteBuf;

typedef struct {
    size_t off;
    size_t len;
} EncChunk;

typedef struct {
    uint8_t op;
    uint8_t c0;
    uint8_t c1;
    uint8_t pattern[8];
    uint8_t raw[64];
    int8_t mx;
    int8_t my;
    /* DCT candidates: quantized coefficients in scan order, the quantizer
     * index, and the number of coded planes. The unquantized block is
     * recovered at paint time so large frames stay inside the memory cap. */
    int16_t t[64];
    uint8_t quant;
    uint8_t cbits;
} EncBlock;

static int buf_push(ByteBuf *buf, uint8_t value)
{
    if (buf->len + 1 > buf->cap) {
        size_t cap = buf->cap != 0 ? buf->cap * 2 : 256;
        uint8_t *grown;

        if (cap < buf->len + 1)
            return -1;
        grown = (uint8_t *)realloc(buf->data, cap);
        if (grown == NULL)
            return -1;
        buf->data = grown;
        buf->cap = cap;
    }
    buf->data[buf->len++] = value;
    return 0;
}

static void buf_free(ByteBuf *buf)
{
    free(buf->data);
    buf->data = NULL;
    buf->len = 0;
    buf->cap = 0;
}

static uint8_t src_at(const uint8_t *src, int stride, int width, int height,
                      int x, int y)
{
    if (x < 0)
        x = 0;
    if (y < 0)
        y = 0;
    if (x >= width)
        x = width - 1;
    if (y >= height)
        y = height - 1;
    return src[(size_t)y * (size_t)stride + (size_t)x];
}

static int motion_in_range(int bx, int by, int mx, int my, int blocks_x,
                           int blocks_y, int stride, int alloc_h)
{
    int x = bx * 8 + mx;
    int y = by * 8 + my;
    long long ref_off;
    long long ref_end;

    if (mx < -15 || mx > 15 || my < -15 || my > 15)
        return 0;
    if (x < 0 || y < 0 || x + 7 >= stride || y + 7 >= alloc_h)
        return 0;
    ref_off = (long long)y * stride + x;
    ref_end = (long long)(blocks_x - 1 + stride * (blocks_y - 1)) * 8;
    return ref_off >= 0 && ref_off <= ref_end;
}

static int visible_match(const uint8_t desired[64], const uint8_t *ref,
                         int stride, int plane_w, int plane_h, int bx, int by,
                         int mx, int my)
{
    int dy;
    int dx;

    for (dy = 0; dy < 8; dy++) {
        int y = by * 8 + dy;
        int sy = y + my;

        if (y >= plane_h)
            continue;
        for (dx = 0; dx < 8; dx++) {
            int x = bx * 8 + dx;
            int sx = x + mx;

            if (x >= plane_w)
                continue;
            if (sx < 0 || sy < 0)
                return 0;
            if (desired[dy * 8 + dx] !=
                ref[(size_t)sy * (size_t)stride + (size_t)sx])
                return 0;
        }
    }
    return 1;
}

/* Quantizer table a painted block was coded with. */
static const int32_t *block_quant(const EncBlock *block)
{
    return block->op == INTER_BLOCK ? bink_enc_inter_quant(block->quant)
                                    : bink_enc_intra_quant(block->quant);
}

/* Scores one DCT candidate. pred is NULL for intra and points at the 8x8
 * predictor for inter. Block caps are enforced here, so a returned bit count
 * is a candidate we could actually emit. Returns estimated bits, or -1. */
static int score_dct(const uint8_t desired[64], const uint8_t *pred,
                     const int32_t *quant, int q, EncBlock *block,
                     int *maxerr, double *rmse)
{
    double coef[64];
    int32_t recon[64];
    uint8_t out[64];
    int i;
    int bits;
    unsigned maxe = 0;
    double sum = 0.0;

    if (pred != NULL) {
        int16_t residual[64];

        for (i = 0; i < 64; i++) {
            int value = (int)desired[i] - (int)pred[i];

            if (value < -(int)pred[i])
                value = -(int)pred[i];
            if (value > 255 - (int)pred[i])
                value = 255 - (int)pred[i];
            residual[i] = (int16_t)value;
        }
        bink_enc_fdct_signed(residual, coef);
    } else {
        bink_enc_fdct(desired, coef);
    }

    if (bink_enc_quantize(coef, quant, block->t) != 0)
        return -1;
    if (bink_enc_cbits(block->t) > 15)
        return -1;
    if (pred == NULL) {
        /* Intra DC is bundled as 11 unsigned bits. */
        if (block->t[0] < 0 || block->t[0] > 2047)
            return -1;
    } else {
        /* Inter DC is bundled as 10 bits plus a sign. */
        if (block->t[0] < -1023 || block->t[0] > 1023)
            return -1;
    }

    bink_enc_unquantize(block->t, quant, recon);
    if (pred != NULL) {
        memcpy(out, pred, 64);
        bink_idct_add(out, 8, recon);
    } else {
        bink_idct_put(out, 8, recon);
    }
    for (i = 0; i < 64; i++) {
        int diff = (int)out[i] - (int)desired[i];
        unsigned error = (unsigned)(diff < 0 ? -diff : diff);

        if (error > maxe)
            maxe = error;
        sum += (double)diff * (double)diff;
    }
    if (maxe > 24 || sum / 64.0 > 4.0 * 4.0)
        return -1;

    block->cbits = (uint8_t)bink_enc_cbits(block->t);
    block->quant = (uint8_t)q;
    bits = bink_enc_emit_coeffs(NULL, block->t, block->cbits, q);
    if (bits < 0)
        return -1;
    /* DC costs 11 bits intra; inter adds a 10-bit DC and a 16-bit vector. */
    bits += pred != NULL ? 10 + 16 : 11;
    *maxerr = (int)maxe;
    *rmse = sqrt(sum / 64.0);
    return bits;
}

/* Sum of squared differences over the visible pixels of one block. */
static unsigned long ssd_visible(const uint8_t desired[64], const uint8_t *ref,
                                 int stride, int plane_w, int plane_h, int bx,
                                 int by, int mx, int my)
{
    unsigned long sum = 0;
    int dy;
    int dx;

    for (dy = 0; dy < 8; dy++) {
        int y = by * 8 + dy;
        int sy = y + my;

        if (y >= plane_h)
            continue;
        for (dx = 0; dx < 8; dx++) {
            int x = bx * 8 + dx;
            int sx = x + mx;
            int diff;

            if (x >= plane_w)
                continue;
            if (sx < 0 || sy < 0)
                return (unsigned long)-1;
            diff = (int)desired[dy * 8 + dx] -
                   (int)ref[(size_t)sy * (size_t)stride + (size_t)sx];
            sum += (unsigned long)(diff * diff);
        }
    }
    return sum;
}

/* Copy the predictor block exactly the way MOTION_BLOCK painting does. */
static void copy_predicted(uint8_t out[64], const uint8_t *ref, int stride,
                           int bx, int by, int mx, int my)
{
    const uint8_t *src = ref + (size_t)(by * 8 + my) * (size_t)stride +
                         (size_t)(bx * 8 + mx);
    int y;

    for (y = 0; y < 8; y++)
        memcpy(out + y * 8, src + (size_t)y * (size_t)stride, 8);
}

static void paint_block(uint8_t *dst, int stride, const EncBlock *block,
                        const uint8_t *ref, int ref_x, int ref_y)
{
    int y;

    if (block->op == SKIP_BLOCK) {
        bink_copy_block(dst, ref + (size_t)ref_y * (size_t)stride +
                                 (size_t)ref_x,
                        stride);
        return;
    }
    if (block->op == MOTION_BLOCK) {
        uint8_t tmp[64];
        const uint8_t *src = ref +
            (size_t)(ref_y + block->my) * (size_t)stride +
            (size_t)(ref_x + block->mx);

        for (y = 0; y < 8; y++)
            memcpy(tmp + y * 8, src + (size_t)y * (size_t)stride, 8);
        for (y = 0; y < 8; y++)
            memcpy(dst + (size_t)y * (size_t)stride, tmp + y * 8, 8);
        return;
    }
    if (block->op == FILL_BLOCK) {
        bink_fill_block(dst, stride, 8, block->c0);
        return;
    }
    if (block->op == INTRA_BLOCK) {
        int32_t recon[64];

        bink_enc_unquantize(block->t, block_quant(block), recon);
        bink_idct_put(dst, stride, recon);
        return;
    }
    if (block->op == INTER_BLOCK) {
        int32_t recon[64];
        uint8_t tmp[64];
        const uint8_t *src = ref +
            (size_t)(ref_y + block->my) * (size_t)stride +
            (size_t)(ref_x + block->mx);

        for (y = 0; y < 8; y++)
            memcpy(tmp + y * 8, src + (size_t)y * (size_t)stride, 8);
        for (y = 0; y < 8; y++)
            memcpy(dst + (size_t)y * (size_t)stride, tmp + y * 8, 8);
        bink_enc_unquantize(block->t, block_quant(block), recon);
        bink_idct_add(dst, stride, recon);
        return;
    }
    if (block->op == PATTERN_BLOCK) {
        for (y = 0; y < 8; y++) {
            unsigned value = block->pattern[y];
            int x;

            for (x = 0; x < 8; x++, value >>= 1)
                dst[(size_t)y * (size_t)stride + (size_t)x] =
                    (value & 1u) ? block->c1 : block->c0;
        }
        return;
    }
    for (y = 0; y < 8; y++)
        memcpy(dst + (size_t)y * (size_t)stride, block->raw + y * 8, 8);
}

static int plan_chunks(const size_t *row_counts, int rows, int len_bits,
                       EncChunk **out, int *out_count)
{
    EncChunk *chunks;
    int count = 0;
    int row = 0;
    size_t q_pos = 0;
    size_t q_end = 0;
    size_t off = 0;
    size_t max_t;

    *out = NULL;
    *out_count = 0;
    if (len_bits <= 0 || len_bits > 30)
        return -1;
    max_t = ((size_t)1 << len_bits) - 1;
    chunks = (EncChunk *)calloc(rows > 0 ? (size_t)rows : 1, sizeof(*chunks));
    if (chunks == NULL)
        return -1;

    while (row < rows) {
        size_t need = row_counts[row];

        if (q_pos == q_end) {
            size_t take = 0;

            if (need > 0) {
                take = need;
            } else {
                int later;

                for (later = row + 1; later < rows; later++) {
                    if (row_counts[later] > 0) {
                        take = row_counts[later];
                        break;
                    }
                }
            }
            if (take == 0)
                break;
            if (take > max_t) {
                free(chunks);
                return -1;
            }
            chunks[count].off = off;
            chunks[count].len = take;
            count++;
            off += take;
            q_end += take;
        }
        if (q_end - q_pos < need) {
            free(chunks);
            return -1;
        }
        q_pos += need;
        row++;
    }
    *out = chunks;
    *out_count = count;
    return 0;
}

typedef struct {
    int write;
    int last;
    int vlc;
    int high_vlc[16];
    unsigned freq[16];
    unsigned high_freq[16][16];
} SymbolState;

static int all_bytes_equal(const uint8_t *bytes, size_t count)
{
    size_t i;

    for (i = 1; i < count; i++) {
        if (bytes[i] != bytes[0])
            return 0;
    }
    return 1;
}

static int emit_types(BinkBitWriter *bw, const uint8_t *bytes, size_t count,
                      SymbolState *state)
{
    size_t i;
    int same = all_bytes_equal(bytes, count);

    if (same) {
        if (state->write)
            return bink_bw_put(bw, 1, 1) != 0 ||
                   bink_bw_put(bw, bytes[0], 4) != 0
                       ? -1
                       : 0;
        return 0;
    }
    if (state->write && bink_bw_put(bw, 0, 1) != 0)
        return -1;
    for (i = 0; i < count; i++) {
        int symbol = bytes[i];

        if (symbol < 0 || symbol > 15)
            return -1;
        if (state->write) {
            if (bink_huff_put(bw, state->vlc, symbol) != 0)
                return -1;
        } else {
            state->freq[symbol]++;
        }
    }
    return 0;
}

static int emit_colors(BinkBitWriter *bw, const uint8_t *bytes, size_t count,
                       SymbolState *state)
{
    size_t i;
    int same = all_bytes_equal(bytes, count);
    size_t limit = same ? 1 : count;

    if (same) {
        if (state->write && bink_bw_put(bw, 1, 1) != 0)
            return -1;
    } else if (state->write && bink_bw_put(bw, 0, 1) != 0) {
        return -1;
    }
    for (i = 0; i < limit; i++) {
        int high = bytes[i] >> 4;
        int low = bytes[i] & 15;

        if (state->write) {
            if (bink_huff_put(bw, state->high_vlc[state->last], high) != 0)
                return -1;
            if (bink_huff_put(bw, state->vlc, low) != 0)
                return -1;
        } else {
            state->high_freq[state->last][high]++;
            state->freq[low]++;
        }
        state->last = high;
    }
    if (same)
        state->last = bytes[0] >> 4;
    return 0;
}

static int emit_patterns(BinkBitWriter *bw, const uint8_t *bytes, size_t count,
                         SymbolState *state)
{
    size_t i;

    for (i = 0; i < count; i++) {
        int low = bytes[i] & 15;
        int high = bytes[i] >> 4;

        if (state->write) {
            if (bink_huff_put(bw, state->vlc, low) != 0)
                return -1;
            if (bink_huff_put(bw, state->vlc, high) != 0)
                return -1;
        } else {
            state->freq[low]++;
            state->freq[high]++;
        }
    }
    return 0;
}

static int emit_motion(BinkBitWriter *bw, const uint8_t *bytes, size_t count,
                       SymbolState *state)
{
    size_t i;
    int same = all_bytes_equal(bytes, count);
    size_t limit = same ? 1 : count;

    if (same) {
        if (state->write && bink_bw_put(bw, 1, 1) != 0)
            return -1;
    } else if (state->write && bink_bw_put(bw, 0, 1) != 0) {
        return -1;
    }
    for (i = 0; i < limit; i++) {
        int value = (int)(int8_t)bytes[i];
        int mag = value < 0 ? -value : value;
        int sign = value < 0;

        if (mag > 15)
            return -1;
        if (state->write) {
            if (same) {
                if (bink_bw_put(bw, (unsigned)mag, 4) != 0)
                    return -1;
            } else if (bink_huff_put(bw, state->vlc, mag) != 0) {
                return -1;
            }
            if (mag && bink_bw_put(bw, (unsigned)sign, 1) != 0)
                return -1;
        } else if (!same) {
            state->freq[mag]++;
        }
    }
    return 0;
}

/* DC bundle values. The first value in each chunk is absolute; the rest are
 * deltas in groups of up to eight, each group led by a 4-bit width. Mirrors
 * bink_bundle_read_dcs in src/bink_video.c. */
static int emit_dcs(BinkBitWriter *bw, const uint8_t *bytes, size_t count,
                    SymbolState *state, int has_sign)
{
    const int16_t *values = (const int16_t *)(const void *)bytes;
    size_t pos = 0;

    if (count == 0)
        return 0;
    {
        int first = values[0];
        int mag = first < 0 ? -first : first;

        if (state->write) {
            if (has_sign) {
                if (mag > 1023)
                    return -1;
                if (bink_bw_put(bw, (unsigned)mag, 10) != 0)
                    return -1;
                if (mag && bink_bw_put(bw, (unsigned)(first < 0), 1) != 0)
                    return -1;
            } else {
                if (mag > 2047 || first < 0)
                    return -1;
                if (bink_bw_put(bw, (unsigned)first, 11) != 0)
                    return -1;
            }
        }
        pos = 1;
    }
    while (pos < count) {
        size_t group = count - pos < 8 ? count - pos : 8;
        int bsize = 0;
        size_t i;

        for (i = 0; i < group; i++) {
            int delta = (int)values[pos + i] - (int)values[pos + i - 1];
            int mag = delta < 0 ? -delta : delta;
            int bits = 0;

            while (mag != 0) {
                bits++;
                mag >>= 1;
            }
            if (bits > bsize)
                bsize = bits;
        }
        if (bsize > 15)
            return -1;
        if (state->write && bink_bw_put(bw, (unsigned)bsize, 4) != 0)
            return -1;
        if (bsize != 0) {
            for (i = 0; i < group; i++) {
                int delta = (int)values[pos + i] - (int)values[pos + i - 1];
                int mag = delta < 0 ? -delta : delta;

                if (state->write) {
                    if (bink_bw_put(bw, (unsigned)mag, bsize) != 0)
                        return -1;
                    if (mag && bink_bw_put(bw, (unsigned)(delta < 0), 1) != 0)
                        return -1;
                }
            }
        }
        pos += group;
    }
    return 0;
}

static int emit_intra_dcs(BinkBitWriter *bw, const uint8_t *bytes,
                          size_t count, SymbolState *state)
{
    return emit_dcs(bw, bytes, count, state, 0);
}

static int emit_inter_dcs(BinkBitWriter *bw, const uint8_t *bytes,
                          size_t count, SymbolState *state)
{
    return emit_dcs(bw, bytes, count, state, 1);
}

typedef struct {
    const uint8_t *data;
    const size_t *row_counts;
    const EncChunk *chunks;
    int chunk_count;
    int chunk_index;
    size_t q_pos;
    size_t q_end;
    int dead;
    int len_bits;
    int item_size;
    int (*emit)(BinkBitWriter *, const uint8_t *, size_t, SymbolState *);
    SymbolState *state;
} BundleWalk;

static void walk_init(BundleWalk *walk, const uint8_t *data,
                      const size_t *row_counts, const EncChunk *chunks,
                      int chunk_count, int len_bits, int item_size,
                      int (*emit)(BinkBitWriter *, const uint8_t *, size_t,
                                  SymbolState *),
                      SymbolState *state)
{
    memset(walk, 0, sizeof(*walk));
    walk->data = data;
    walk->row_counts = row_counts;
    walk->chunks = chunks;
    walk->chunk_count = chunk_count;
    walk->len_bits = len_bits;
    walk->item_size = item_size;
    walk->emit = emit;
    walk->state = state;
}

static void walk_reset(BundleWalk *walk)
{
    walk->chunk_index = 0;
    walk->q_pos = 0;
    walk->q_end = 0;
    walk->dead = 0;
}

/* One block-row of one bundle. Rows are interleaved in bundle order. */
static int walk_row(BinkBitWriter *bw, BundleWalk *walk, int row)
{
    size_t need = walk->row_counts[row];

    if (walk->dead) {
        if (need != 0)
            return -1;
        return 0;
    }
    if (walk->q_pos == walk->q_end) {
        if (walk->chunk_index >= walk->chunk_count) {
            if (walk->state->write &&
                bink_bw_put(bw, 0, walk->len_bits) != 0)
                return -1;
            walk->dead = 1;
            if (need != 0)
                return -1;
            return 0;
        }
        if (walk->state->write &&
            bink_bw_put(bw, (unsigned)walk->chunks[walk->chunk_index].len,
                        walk->len_bits) != 0)
            return -1;
        if (walk->emit(bw, walk->data + walk->chunks[walk->chunk_index].off *
                                        (size_t)walk->item_size,
                       walk->chunks[walk->chunk_index].len, walk->state) != 0)
            return -1;
        walk->q_end += walk->chunks[walk->chunk_index].len;
        walk->chunk_index++;
    }
    if (walk->q_end - walk->q_pos < need)
        return -1;
    walk->q_pos += need;
    return 0;
}

static int emit_rows(BinkBitWriter *bw, BundleWalk *walks, int walk_count,
                     int rows)
{
    int row;
    int i;

    for (row = 0; row < rows; row++) {
        for (i = 0; i < walk_count; i++) {
            if (walk_row(bw, &walks[i], row) != 0)
                return -1;
        }
    }
    return 0;
}

/* Same walk, but each row's DCT coefficient bits follow that row's bundles,
 * which is where the decoder reads them. */
static int emit_rows_dct(BinkBitWriter *bw, BundleWalk *walks, int walk_count,
                         int rows, const EncBlock *blocks, int blocks_x)
{
    int row;

    for (row = 0; row < rows; row++) {
        int i;
        int bx;

        for (i = 0; i < walk_count; i++) {
            if (walk_row(bw, &walks[i], row) != 0)
                return -1;
        }
        for (bx = 0; bx < blocks_x; bx++) {
            const EncBlock *block =
                &blocks[(size_t)row * (size_t)blocks_x + (size_t)bx];

            if (block->op != INTRA_BLOCK && block->op != INTER_BLOCK)
                continue;
            if (bink_enc_emit_coeffs(bw, block->t, block->cbits,
                                     block->quant) < 0)
                return -1;
        }
    }
    return 0;
}

static int encode_plane(BinkBitWriter *bw, const uint8_t *src, int src_stride,
                        uint8_t *dst, const uint8_t *ref, int stride,
                        int alloc_h, int plane_w, int plane_h, int blocks_x,
                        int blocks_y, int keyframe, int motion_range)
{
    EncBlock *blocks = NULL;
    ByteBuf colors;
    ByteBuf patterns;
    ByteBuf motion_x;
    ByteBuf motion_y;
    int16_t *intra_dc = NULL;
    int16_t *inter_dc = NULL;
    uint8_t *types = NULL;
    size_t *color_rows = NULL;
    size_t *pattern_rows = NULL;
    size_t *x_rows = NULL;
    size_t *y_rows = NULL;
    size_t *type_rows = NULL;
    size_t *intra_rows = NULL;
    size_t *inter_rows = NULL;
    EncChunk *type_chunks = NULL;
    EncChunk *color_chunks = NULL;
    EncChunk *pattern_chunks = NULL;
    EncChunk *x_chunks = NULL;
    EncChunk *y_chunks = NULL;
    EncChunk *intra_chunks = NULL;
    EncChunk *inter_chunks = NULL;
    int type_n = 0;
    int color_n = 0;
    int pattern_n = 0;
    int x_n = 0;
    int y_n = 0;
    int intra_n = 0;
    int inter_n = 0;
    size_t intra_len = 0;
    size_t inter_len = 0;
    int lens[BINK_NB_SRC];
    int width_arg = plane_w > 8 ? plane_w : 8;
    int by;
    int bx;
    int i;
    int failed = 0;
    SymbolState types_state;
    SymbolState color_state;
    SymbolState pattern_state;
    SymbolState x_state;
    SymbolState y_state;

    memset(&colors, 0, sizeof(colors));
    memset(&patterns, 0, sizeof(patterns));
    memset(&motion_x, 0, sizeof(motion_x));
    memset(&motion_y, 0, sizeof(motion_y));
    memset(&types_state, 0, sizeof(types_state));
    memset(&color_state, 0, sizeof(color_state));
    memset(&pattern_state, 0, sizeof(pattern_state));
    memset(&x_state, 0, sizeof(x_state));
    memset(&y_state, 0, sizeof(y_state));

    if (blocks_x <= 0 || blocks_y <= 0)
        return -1;
    blocks = (EncBlock *)calloc((size_t)blocks_x * (size_t)blocks_y,
                                sizeof(*blocks));
    types = (uint8_t *)malloc((size_t)blocks_x * (size_t)blocks_y);
    intra_dc = (int16_t *)calloc((size_t)blocks_x * (size_t)blocks_y,
                                 sizeof(*intra_dc));
    inter_dc = (int16_t *)calloc((size_t)blocks_x * (size_t)blocks_y,
                                 sizeof(*inter_dc));
    color_rows = (size_t *)calloc((size_t)blocks_y, sizeof(*color_rows));
    pattern_rows = (size_t *)calloc((size_t)blocks_y, sizeof(*pattern_rows));
    x_rows = (size_t *)calloc((size_t)blocks_y, sizeof(*x_rows));
    y_rows = (size_t *)calloc((size_t)blocks_y, sizeof(*y_rows));
    type_rows = (size_t *)calloc((size_t)blocks_y, sizeof(*type_rows));
    intra_rows = (size_t *)calloc((size_t)blocks_y, sizeof(*intra_rows));
    inter_rows = (size_t *)calloc((size_t)blocks_y, sizeof(*inter_rows));
    if (blocks == NULL || types == NULL || intra_dc == NULL ||
        inter_dc == NULL || color_rows == NULL || pattern_rows == NULL ||
        x_rows == NULL || y_rows == NULL || type_rows == NULL ||
        intra_rows == NULL || inter_rows == NULL) {
        failed = 1;
        goto done;
    }

    for (by = 0; by < blocks_y; by++) {
        size_t color_at = colors.len;
        size_t pattern_at = patterns.len;
        size_t x_at = motion_x.len;
        size_t y_at = motion_y.len;
        size_t intra_at = intra_len;
        size_t inter_at = inter_len;

        type_rows[by] = (size_t)blocks_x;
        for (bx = 0; bx < blocks_x; bx++) {
            EncBlock *block = &blocks[(size_t)by * (size_t)blocks_x + (size_t)bx];
            uint8_t desired[64];
            int unique = 0;
            uint8_t seen[2];
            int pixel;
            int found_motion = 0;
            int chosen = 0;
            int best_mx = 0;
            int best_my = 0;
            int have_ref = !keyframe && ref != NULL;

            for (pixel = 0; pixel < 64; pixel++) {
                int x = bx * 8 + (pixel & 7);
                int y = by * 8 + (pixel >> 3);

                desired[pixel] =
                    src_at(src, src_stride, plane_w, plane_h, x, y);
            }
            if (have_ref && visible_match(desired, ref, stride, plane_w,
                                          plane_h, bx, by, 0, 0)) {
                block->op = SKIP_BLOCK;
                chosen = 1;
            } else if (have_ref) {
                /* Keep the best-SSD vector even when nothing matches exactly;
                 * the residual coder uses it. */
                unsigned long best_ssd = ssd_visible(desired, ref, stride,
                                                     plane_w, plane_h, bx, by,
                                                     0, 0);
                int range = motion_range > 15 ? 15 : motion_range;
                int my;

                for (my = -range; my <= range && !found_motion; my++) {
                    int mx;

                    for (mx = -range; mx <= range; mx++) {
                        unsigned long ssd;
                        int valid;

                        if (mx == 0 && my == 0)
                            continue;
                        valid = motion_in_range(bx, by, mx, my, blocks_x,
                                                blocks_y, stride, alloc_h);
                        if (!valid)
                            continue;
                        if (visible_match(desired, ref, stride, plane_w,
                                          plane_h, bx, by, mx, my)) {
                            block->op = MOTION_BLOCK;
                            block->mx = (int8_t)mx;
                            block->my = (int8_t)my;
                            found_motion = 1;
                            chosen = 1;
                            break;
                        }
                        ssd = ssd_visible(desired, ref, stride, plane_w,
                                          plane_h, bx, by, mx, my);
                        if (ssd < best_ssd) {
                            best_ssd = ssd;
                            best_mx = mx;
                            best_my = my;
                        }
                    }
                }
            }
            if (!chosen) {
                for (pixel = 0; pixel < 64; pixel++) {
                    int s;

                    for (s = 0; s < unique; s++) {
                        if (seen[s] == desired[pixel])
                            break;
                    }
                    if (s == unique) {
                        if (unique == 2) {
                            unique = 3;
                            break;
                        }
                        seen[unique++] = desired[pixel];
                    }
                }
                if (unique <= 1) {
                    block->op = FILL_BLOCK;
                    block->c0 = unique == 0 ? 0 : seen[0];
                } else if (unique == 2) {
                    int y;

                    block->op = PATTERN_BLOCK;
                    block->c0 = seen[0];
                    block->c1 = seen[1];
                    for (y = 0; y < 8; y++) {
                        unsigned bits = 0;
                        int x;

                        for (x = 0; x < 8; x++) {
                            if (desired[y * 8 + x] == block->c1)
                                bits |= 1u << x;
                        }
                        block->pattern[y] = (uint8_t)bits;
                    }
                } else {
                    /* Three or more values: raw is the safe fallback; intra and
                     * (on P-frames) inter DCT blocks win when cheaper within
                     * the caps. */
                    static const int quants[8] = { 0, 2, 4, 6, 8, 10, 12, 14 };
                    uint8_t predicted[64];
                    int have_pred = 0;
                    int best_bits = 512; /* open-coded raw block cost */
                    int best_maxerr = 256;
                    int best_kind = 0; /* 0 raw, 1 intra, 2 inter */
                    int qi;

                    if (have_ref && motion_in_range(bx, by, best_mx, best_my,
                                                    blocks_x, blocks_y, stride,
                                                    alloc_h)) {
                        copy_predicted(predicted, ref, stride, bx, by, best_mx,
                                       best_my);
                        have_pred = 1;
                    }
                    for (qi = 0; qi < 8; qi++) {
                        EncBlock candidate;
                        int maxerr;
                        double rmse;
                        int bits;

                        memset(&candidate, 0, sizeof(candidate));
                        maxerr = 0;
                        rmse = 0.0;
                        bits = score_dct(desired, NULL,
                                         bink_enc_intra_quant(quants[qi]),
                                         quants[qi], &candidate, &maxerr,
                                         &rmse);
                        if (bits >= 0 &&
                            (bits < best_bits ||
                             (bits == best_bits && maxerr < best_maxerr))) {
                            best_bits = bits;
                            best_maxerr = maxerr;
                            best_kind = 1;
                            memcpy(block->t, candidate.t, sizeof(block->t));
                            block->quant = candidate.quant;
                            block->cbits = candidate.cbits;
                        }
                        if (!have_pred)
                            continue;
                        memset(&candidate, 0, sizeof(candidate));
                        maxerr = 0;
                        rmse = 0.0;
                        bits = score_dct(desired, predicted,
                                         bink_enc_inter_quant(quants[qi]),
                                         quants[qi], &candidate, &maxerr,
                                         &rmse);
                        if (bits >= 0 &&
                            (bits < best_bits ||
                             (bits == best_bits && maxerr < best_maxerr))) {
                            best_bits = bits;
                            best_maxerr = maxerr;
                            best_kind = 2;
                            memcpy(block->t, candidate.t, sizeof(block->t));
                            block->quant = candidate.quant;
                            block->cbits = candidate.cbits;
                        }
                    }
                    if (best_kind == 0) {
                        block->op = RAW_BLOCK;
                        memcpy(block->raw, desired, 64);
                    } else if (best_kind == 1) {
                        block->op = INTRA_BLOCK;
                    } else {
                        block->op = INTER_BLOCK;
                        block->mx = (int8_t)best_mx;
                        block->my = (int8_t)best_my;
                    }
                }
            }
            types[(size_t)by * (size_t)blocks_x + (size_t)bx] = block->op;
            if (block->op == FILL_BLOCK) {
                if (buf_push(&colors, block->c0) != 0)
                    failed = 1;
            } else if (block->op == PATTERN_BLOCK) {
                if (buf_push(&colors, block->c0) != 0 ||
                    buf_push(&colors, block->c1) != 0)
                    failed = 1;
                for (i = 0; i < 8; i++) {
                    if (buf_push(&patterns, block->pattern[i]) != 0)
                        failed = 1;
                }
            } else if (block->op == RAW_BLOCK) {
                for (i = 0; i < 64; i++) {
                    if (buf_push(&colors, block->raw[i]) != 0)
                        failed = 1;
                }
            } else if (block->op == MOTION_BLOCK ||
                       block->op == INTER_BLOCK) {
                if (buf_push(&motion_x, (uint8_t)block->mx) != 0 ||
                    buf_push(&motion_y, (uint8_t)block->my) != 0)
                    failed = 1;
            }
            if (block->op == INTRA_BLOCK)
                intra_dc[intra_len++] = block->t[0];
            else if (block->op == INTER_BLOCK)
                inter_dc[inter_len++] = block->t[0];
            if (failed)
                goto done;
        }
        color_rows[by] = colors.len - color_at;
        pattern_rows[by] = patterns.len - pattern_at;
        x_rows[by] = motion_x.len - x_at;
        y_rows[by] = motion_y.len - y_at;
        /* DC bundle rows are counted in values, not bytes. */
        intra_rows[by] = intra_len - intra_at;
        inter_rows[by] = inter_len - inter_at;
    }

    bink_bundle_init_lengths(lens, width_arg, blocks_x);
    if (plan_chunks(type_rows, blocks_y, lens[BINK_SRC_BLOCK_TYPES],
                    &type_chunks, &type_n) != 0 ||
        plan_chunks(color_rows, blocks_y, lens[BINK_SRC_COLORS], &color_chunks,
                    &color_n) != 0 ||
        plan_chunks(pattern_rows, blocks_y, lens[BINK_SRC_PATTERN],
                    &pattern_chunks, &pattern_n) != 0 ||
        plan_chunks(x_rows, blocks_y, lens[BINK_SRC_X_OFF], &x_chunks, &x_n) !=
            0 ||
        plan_chunks(y_rows, blocks_y, lens[BINK_SRC_Y_OFF], &y_chunks, &y_n) !=
            0 ||
        plan_chunks(intra_rows, blocks_y, lens[BINK_SRC_INTRA_DC],
                    &intra_chunks, &intra_n) != 0 ||
        plan_chunks(inter_rows, blocks_y, lens[BINK_SRC_INTER_DC],
                    &inter_chunks, &inter_n) != 0) {
        failed = 1;
        goto done;
    }

    {
        size_t *zeros = (size_t *)calloc((size_t)blocks_y, sizeof(*zeros));
        SymbolState unused;
        BundleWalk walks[BINK_NB_SRC];
        int pass;

        if (zeros == NULL) {
            failed = 1;
            goto done;
        }
        memset(&unused, 0, sizeof(unused));
        walk_init(&walks[BINK_SRC_BLOCK_TYPES], types, type_rows, type_chunks,
                  type_n, lens[BINK_SRC_BLOCK_TYPES], 1, emit_types,
                  &types_state);
        walk_init(&walks[BINK_SRC_SUB_BLOCK_TYPES], NULL, zeros, NULL, 0,
                  lens[BINK_SRC_SUB_BLOCK_TYPES], 1, emit_types, &unused);
        walk_init(&walks[BINK_SRC_COLORS], colors.data, color_rows, color_chunks,
                  color_n, lens[BINK_SRC_COLORS], 1, emit_colors, &color_state);
        walk_init(&walks[BINK_SRC_PATTERN], patterns.data, pattern_rows,
                  pattern_chunks, pattern_n, lens[BINK_SRC_PATTERN], 1,
                  emit_patterns, &pattern_state);
        walk_init(&walks[BINK_SRC_X_OFF], motion_x.data, x_rows, x_chunks, x_n,
                  lens[BINK_SRC_X_OFF], 1, emit_motion, &x_state);
        walk_init(&walks[BINK_SRC_Y_OFF], motion_y.data, y_rows, y_chunks, y_n,
                  lens[BINK_SRC_Y_OFF], 1, emit_motion, &y_state);
        walk_init(&walks[BINK_SRC_INTRA_DC], (const uint8_t *)intra_dc,
                  intra_rows, intra_chunks, intra_n, lens[BINK_SRC_INTRA_DC], 2,
                  emit_intra_dcs, &unused);
        walk_init(&walks[BINK_SRC_INTER_DC], (const uint8_t *)inter_dc,
                  inter_rows, inter_chunks, inter_n, lens[BINK_SRC_INTER_DC], 2,
                  emit_inter_dcs, &unused);
        walk_init(&walks[BINK_SRC_RUN], NULL, zeros, NULL, 0, lens[BINK_SRC_RUN],
                  1, emit_types, &unused);

        if (emit_rows(NULL, walks, BINK_NB_SRC, blocks_y) != 0) {
            free(zeros);
            failed = 1;
            goto done;
        }

        types_state.vlc = bink_huff_best(types_state.freq);
        types_state.write = 1;
        color_state.vlc = bink_huff_best(color_state.freq);
        color_state.write = 1;
        color_state.last = 0;
        for (i = 0; i < 16; i++)
            color_state.high_vlc[i] = bink_huff_best(color_state.high_freq[i]);
        pattern_state.vlc = bink_huff_best(pattern_state.freq);
        pattern_state.write = 1;
        x_state.vlc = bink_huff_best(x_state.freq);
        x_state.write = 1;
        y_state.vlc = bink_huff_best(y_state.freq);
        y_state.write = 1;
        unused.write = 1;
        for (pass = 0; pass < BINK_NB_SRC; pass++)
            walk_reset(&walks[pass]);

        for (i = 0; i < BINK_NB_SRC; i++) {
            if (i == BINK_SRC_COLORS) {
                int high;

                for (high = 0; high < 16; high++) {
                    if (bink_tree_write_identity(bw,
                                                 color_state.high_vlc[high]) !=
                        0) {
                        free(zeros);
                        failed = 1;
                        goto done;
                    }
                }
            }
            if (i == BINK_SRC_INTRA_DC || i == BINK_SRC_INTER_DC)
                continue;
            {
                int vlc = 0;

                if (i == BINK_SRC_BLOCK_TYPES)
                    vlc = types_state.vlc;
                else if (i == BINK_SRC_COLORS)
                    vlc = color_state.vlc;
                else if (i == BINK_SRC_PATTERN)
                    vlc = pattern_state.vlc;
                else if (i == BINK_SRC_X_OFF)
                    vlc = x_state.vlc;
                else if (i == BINK_SRC_Y_OFF)
                    vlc = y_state.vlc;
                if (bink_tree_write_identity(bw, vlc) != 0) {
                    free(zeros);
                    failed = 1;
                    goto done;
                }
            }
        }

        if (emit_rows_dct(bw, walks, BINK_NB_SRC, blocks_y, blocks,
                          blocks_x) != 0) {
            free(zeros);
            failed = 1;
            goto done;
        }
        free(zeros);
    }

    if (bink_bw_align32(bw) != 0) {
        failed = 1;
        goto done;
    }

    for (by = 0; by < blocks_y; by++) {
        for (bx = 0; bx < blocks_x; bx++) {
            EncBlock *block =
                &blocks[(size_t)by * (size_t)blocks_x + (size_t)bx];
            uint8_t *out = dst + (size_t)(by * 8) * (size_t)stride +
                           (size_t)(bx * 8);

            paint_block(out, stride, block, ref, bx * 8, by * 8);
        }
    }

done:
    free(blocks);
    free(types);
    free(color_rows);
    free(pattern_rows);
    free(x_rows);
    free(y_rows);
    free(type_rows);
    free(intra_rows);
    free(inter_rows);
    free(intra_dc);
    free(inter_dc);
    free(type_chunks);
    free(color_chunks);
    free(pattern_chunks);
    free(x_chunks);
    free(y_chunks);
    free(intra_chunks);
    free(inter_chunks);
    buf_free(&colors);
    buf_free(&patterns);
    buf_free(&motion_x);
    buf_free(&motion_y);
    return failed ? -1 : 0;
}

int bink_enc_write_frame(BinkBitWriter *bw, BinkEncPlanes *planes,
                         const uint8_t *y, const uint8_t *u, const uint8_t *v,
                         int keyframe, int motion_range)
{
    const uint8_t *srcs[3];
    int order[3];
    int i;

    if (!planes->has_prev)
        keyframe = 1;
    srcs[0] = y;
    srcs[1] = u;
    srcs[2] = v;
    /* BIKi with version >= 'h' swaps the chroma plane order to Y, V, U. */
    order[0] = 0;
    order[1] = 2;
    order[2] = 1;
    for (i = 0; i < 3; i++) {
        int plane = order[i];
        const uint8_t *ref = keyframe ? NULL : planes->ref[plane];

        if (encode_plane(bw, srcs[plane], planes->plane_w[plane],
                         planes->cur[plane], ref, planes->stride[plane],
                         planes->alloc_h[plane], planes->plane_w[plane],
                         planes->plane_h[plane], planes->blocks_x[plane],
                         planes->blocks_y[plane], keyframe, motion_range) != 0)
            return -1;
    }
    for (i = 0; i < 3; i++) {
        uint8_t *swap = planes->cur[i];

        planes->cur[i] = planes->ref[i];
        planes->ref[i] = swap;
    }
    planes->has_prev = 1;
    return 0;
}
