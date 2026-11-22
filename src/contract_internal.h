/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org> */

/*
 * contract_internal.h -- shared internals of the /orchestrate contract
 * modules (scanner.c, descriptor.c, parse.c, responses.c).  Not part of
 * the frozen public contract (contract.h).
 */

#ifndef ORC_CONTRACT_INTERNAL_H
#define ORC_CONTRACT_INTERNAL_H

#include "contract.h"

struct scanner
  {
    const char *s;
    size_t pos;
    size_t len;
    char *scratch;
    size_t scratch_cap;
    size_t scratch_len;
    int err;                       /* 0 ok, 1 JSON error, 2 arena full */
  };


/* scanner.c */
void fail (struct scanner *sc, int err);
char *arena_take (struct scanner *sc, size_t n);
void skip_ws (struct scanner *sc);
int peek (struct scanner *sc);
int next (struct scanner *sc);
const char *parse_string (struct scanner *sc, size_t *len_out);
int skip_value (struct scanner *sc, size_t *start_out, size_t *end_out);
const char *copy_raw (struct scanner *sc, size_t start, size_t end);

/* descriptor.c */
void set_detail (struct orc_request *req, const char *key);
int parse_descriptor (struct scanner *sc, struct orc_request *req);

#endif /* ORC_CONTRACT_INTERNAL_H */
