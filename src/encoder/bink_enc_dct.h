/*
 * Bink 1 encoder DCT coefficient math and coefficient tree writer.
 *
 * Copyright (c) 2026 libbink contributors.
 * Licensed under the GNU Lesser General Public License, version 2.1 or later.
 */

#ifndef BINK_ENC_DCT_H
#define BINK_ENC_DCT_H

#include "bink_bits.h"

#include <stdint.h>

/* Decoder tables, for encoder use. The quantizer index must be 0..15. */
const uint8_t *bink_enc_scan(void);
const int32_t *bink_enc_intra_quant(int index);
const int32_t *bink_enc_inter_quant(int index);

/* Forward transform of one 8x8 block. coef is raster-ordered. An internal
 * init failure (impossible for the fixed transform) zeroes coef, so the void
 * return never hides a partial result. */
void bink_enc_fdct(const uint8_t pixels[64], double coef[64]);

/* Same transform over a signed block, for motion-compensated residuals. */
void bink_enc_fdct_signed(const int16_t pixels[64], double coef[64]);

/* Quantize raster-ordered coef into scan-ordered t. Returns -1 when a value
 * does not fit the coefficient tree's 15 planes. t is undefined when the
 * function returns -1. */
int bink_enc_quantize(const double coef[64], const int32_t quant[64],
                      int16_t t[64]);

/* Invert bink_enc_quantize into raster-ordered coef, matching the decoder's
 * truncating >> 11. */
void bink_enc_unquantize(const int16_t t[64], const int32_t quant[64],
                         int32_t coef[64]);

/* Bit planes needed for the largest-magnitude AC value, 0 when all are zero.
 * bink_enc_emit_coeffs accepts 0..15, which covers every value
 * bink_enc_quantize can produce; a raw coefficient of -32768 needs 16 and is
 * rejected there (and is unreachable through quantization). */
int bink_enc_cbits(const int16_t t[64]);

/* Write one RESIDUE payload: the 7-bit budget, the 3-bit top plane, and
 * the bit-plane tree. residual is raster-ordered, each value in -255..255.
 * Returns the number of bits, or -1 when the block does not fit (a value
 * out of range, or more than 127 significant bits). bw == NULL counts
 * without writing. */
int bink_enc_emit_residue(BinkBitWriter *bw, const int16_t residual[64]);

/* Write cbits, the coefficient tree, and the quantizer index. With bw NULL
 * the same bits are counted instead. cbits must equal bink_enc_cbits(t).
 * Returns the number of bits or -1. */
int bink_enc_emit_coeffs(BinkBitWriter *bw, const int16_t t[64], int cbits,
                         int quant_index);

#endif
