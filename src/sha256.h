/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org> */

/* sha256.h — minimal SHA-256 (FIPS 180-4), for the training_data
 * checksums of the /db contract (64 lowercase hex chars). */

#ifndef ORC_SHA256_H
#define ORC_SHA256_H

#include <stddef.h>

struct orc_sha256
  {
    unsigned long long len;
    unsigned char buf[64];
    size_t buflen;
    unsigned int h[8];
  };

void orc_sha256_init (struct orc_sha256 *c);
void orc_sha256_update (struct orc_sha256 *c, const void *data, size_t len);
void orc_sha256_final (struct orc_sha256 *c, unsigned char out[32]);

/* One-shot: hex (64 chars + NUL) of DATA/LEN into OUT (65 bytes). */
void orc_sha256_hex (const void *data, size_t len, char out[65]);

#endif
