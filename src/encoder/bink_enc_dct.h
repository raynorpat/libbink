#ifndef BINK_ENC_DCT_H
#define BINK_ENC_DCT_H

#include "bink_bits.h"

#include <stdint.h>

/* Decoder tables, for encoder use. The quantizer index must be 0..15. */
const uint8_t *bink_enc_scan(void);
const int32_t *bink_enc_intra_quant(int index);
const int32_t *bink_enc_inter_quant(int index);

/* Forward transform of one 8x8 block. coef is raster-ordered. */
void bink_enc_fdct(const uint8_t pixels[64], double coef[64]);

/* Quantize raster-ordered coef into scan-ordered t. Returns -1 when a value
 * does not fit the coefficient tree's 15 planes. */
int bink_enc_quantize(const double coef[64], const int32_t quant[64],
                      int16_t t[64]);

/* Invert bink_enc_quantize into raster-ordered coef, matching the decoder's
 * truncating >> 11. */
void bink_enc_unquantize(const int16_t t[64], const int32_t quant[64],
                         int32_t coef[64]);

/* Bit planes needed for the largest-magnitude AC value, 0 when all are zero. */
int bink_enc_cbits(const int16_t t[64]);

/* Write cbits, the coefficient tree, and the quantizer index. With bw NULL
 * the same bits are counted instead. Returns the number of bits or -1. */
int bink_enc_emit_coeffs(BinkBitWriter *bw, const int16_t t[64], int cbits,
                         int quant_index);

#endif
