#ifndef BINK_ENC_VERIFY_H
#define BINK_ENC_VERIFY_H

#include "bink_encoder.h"

bink_enc_status bink_enc_verify(const char *path, const bink_enc_source *source,
                                const bink_enc_options *options,
                                char *err, size_t errlen);

#endif
