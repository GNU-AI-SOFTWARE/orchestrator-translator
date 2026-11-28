/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org> */

/* tokenize.h — minimal content → input vector (PLAN phase 3:
 * "tokenisation minimale, normalisation").  The text is split into
 * N equal buckets; each value is the bucket's mean byte scaled to
 * [0,1].  Deterministic, no allocation. */

#ifndef ORC_TOKENIZE_H
#define ORC_TOKENIZE_H

#include <stddef.h>

/* Fill OUT with up to MAX values in [0,1]; returns the count. */
int orc_tokenize (const char *text, size_t len, double *out, int max);

#endif
