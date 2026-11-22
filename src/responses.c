/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org> */

/*
 * contract.c — The frozen command line of /orchestrate (SPEC.md
 * sections 5 and 6).
 *
 * Hand written, allocation free, single pass JSON scanner of the
 * command shapes: a task descriptor (bare or under "run"), or an
 * explicit command ("status", "result").  Only what the contract
 * freezes is accepted; everything else is a readable "invalid"
 * answer with its reason.
 */
#include "contract.h"
#include "contract_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
const char *
orc_invalid_reason (enum orc_invalid reason)
{
  switch (reason)
    {
    case ORC_INVALID_LINE:   return "line";
    case ORC_INVALID_JSON:   return "json";
    case ORC_INVALID_FIELD:  return "field";
    case ORC_INVALID_TYPE:   return "type";
    case ORC_INVALID_VALUE:  return "value";
    case ORC_INVALID_KEY:    return "key";
    case ORC_INVALID_TOOLONG: return "toolong";
    }
  return "line";
}
/* ---------------------------------------------------------------------
 *  Response builders
 * ------------------------------------------------------------------- */

size_t
orc_response_invalid (char *buf, size_t cap,
                      enum orc_invalid reason, const char *detail)
{
  const char *r = detail != NULL && detail[0] != '\0'
                    ? detail : orc_invalid_reason (reason);
  int n = snprintf (buf, cap, "{\"invalid\": \"%s\"}", r);

  return n < 0 || (size_t) n >= cap ? 0 : (size_t) n;
}

size_t
orc_response_empty (char *buf, size_t cap)
{
  int n = snprintf (buf, cap, "{\"empty\": true}");

  return n < 0 || (size_t) n >= cap ? 0 : (size_t) n;
}
