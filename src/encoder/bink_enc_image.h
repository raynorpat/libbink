#ifndef BINK_ENC_IMAGE_H
#define BINK_ENC_IMAGE_H

#include "bink_encoder.h"

#include <stdint.h>

/*
 * Load a PPM (P6), BMP, PNG, or JPEG file as tightly packed RGB24.
 * BMP, PNG, and JPEG go through stb_image. Alpha is dropped.
 */
bink_enc_status bink_enc_image_load(const char *path, uint8_t **rgb,
                                    unsigned *width, unsigned *height,
                                    char *err, size_t errlen);

#endif
