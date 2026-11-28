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
/* ---------------------------------------------------------------------
 *  Descriptor binding (SPEC.md section 6).
 *  The scanner stands ON the opening '{' of the descriptor.
 * ------------------------------------------------------------------- */

void
set_detail (struct orc_request *req, const char *key)
{
  size_t n = strlen (key);

  if (n > sizeof req->invalid_detail - 1)
    n = sizeof req->invalid_detail - 1;
  memcpy (req->invalid_detail, key, n);
  req->invalid_detail[n] = '\0';
}

int
parse_descriptor (struct scanner *sc, struct orc_request *req)
{
  struct orc_descriptor *d = &req->desc;
  size_t desc_start;
  size_t desc_end;
  bool have_instances = false;
  bool have_topologies = false;
  bool have_input = false;
  bool have_aggregate = false;

  desc_start = sc->pos;
  if (next (sc) != '{')
    {
      req->invalid = ORC_INVALID_JSON;
      return -1;
    }
  skip_ws (sc);
  if (peek (sc) == '}')
    {
      sc->pos++;
      desc_end = sc->pos;
      req->invalid = ORC_INVALID_FIELD;
      set_detail (req, "instances");
      return -1;
    }

  for (;;)
    {
      const char *key;
      size_t keylen = 0;
      char keybuf[32];

      skip_ws (sc);
      key = parse_string (sc, &keylen);
      if (key == NULL || keylen >= sizeof keybuf)
        {
          if (sc->err == 0)
            {
              req->invalid = ORC_INVALID_JSON;
              return -1;
            }
          req->invalid = ORC_INVALID_JSON;
          return -1;
        }
      memcpy (keybuf, key, keylen);
      keybuf[keylen] = '\0';

      skip_ws (sc);
      if (next (sc) != ':')
        {
          req->invalid = ORC_INVALID_JSON;
          return -1;
        }
      skip_ws (sc);

      if (strcmp (keybuf, "instances") == 0)
        {
          size_t s0, e0;

          if (skip_value (sc, &s0, &e0) < 0)
            {
              req->invalid = ORC_INVALID_JSON;
              return -1;
            }
          if (e0 - s0 < 1)
            {
              req->invalid = ORC_INVALID_TYPE;
              set_detail (req, "instances");
              return -1;
            }
          /* A pure integer is required: no fraction, no exponent. */
          {
            const char *p = sc->s + s0;
            size_t n = e0 - s0;
            bool negative = false;
            long v = 0;

            if (*p == '-')
              {
                negative = true;
                p++;
                n--;
              }
            if (n == 0)
              {
                req->invalid = ORC_INVALID_TYPE;
                set_detail (req, "instances");
                return -1;
              }
            for (size_t i = 0; i < n; i++)
              if (p[i] < '0' || p[i] > '9')
                {
                  req->invalid = ORC_INVALID_TYPE;
                  set_detail (req, "instances");
                  return -1;
                }
            for (size_t i = 0; i < n; i++)
              {
                /* Clamp before the multiply: a long digit run must
                 * never overflow (atoi () on it is undefined). */
                if (v > ORC_MAX_INSTANCES)
                  v = ORC_MAX_INSTANCES + 1;
                else
                  v = v * 10 + (p[i] - '0');
              }
            d->instances = negative ? -(int) v : (int) v;
          }
          if (d->instances < 1 || d->instances > ORC_MAX_INSTANCES)
            {
              req->invalid = ORC_INVALID_VALUE;
              set_detail (req, "instances");
              return -1;
            }
          have_instances = true;
        }
      else if (strcmp (keybuf, "topologies") == 0)
        {
          if (peek (sc) != '[')
            {
              req->invalid = ORC_INVALID_TYPE;
              set_detail (req, "topologies");
              return -1;
            }
          sc->pos++;
          skip_ws (sc);
          if (peek (sc) == ']')
            {
              sc->pos++;
            }
          else
            for (;;)
              {
                const char *t;
                size_t tlen = 0;

                skip_ws (sc);
                t = parse_string (sc, &tlen);
                if (t == NULL)
                  {
                    req->invalid = ORC_INVALID_JSON;
                    return -1;
                  }
                if (tlen == 0 || tlen > ORC_MAX_TOPOLOGY
                    || d->n_topologies >= ORC_MAX_TOPOLOGIES)
                  {
                    req->invalid = ORC_INVALID_VALUE;
                    set_detail (req, "topologies");
                    return -1;
                  }
                strcpy (d->topologies[d->n_topologies++], t);
                skip_ws (sc);
                {
                  int c2 = next (sc);

                  if (c2 == ',')
                    continue;
                  if (c2 == ']')
                    break;
                  req->invalid = ORC_INVALID_JSON;
                  return -1;
                }
            }
          have_topologies = true;
        }
      else if (strcmp (keybuf, "input") == 0)
        {
          if (peek (sc) != '[')
            {
              req->invalid = ORC_INVALID_TYPE;
              set_detail (req, "input");
              return -1;
            }
          sc->pos++;
          skip_ws (sc);
          if (peek (sc) == ']')
            {
              sc->pos++;
            }
          else
            for (;;)
              {
                size_t s0, e0;
                const char *num;

                skip_ws (sc);
                if (skip_value (sc, &s0, &e0) < 0)
                  {
                    req->invalid = ORC_INVALID_JSON;
                    return -1;
                  }
                if (d->n_input >= ORC_MAX_INPUT)
                  {
                    req->invalid = ORC_INVALID_VALUE;
                    set_detail (req, "input");
                    return -1;
                  }
                num = copy_raw (sc, s0, e0);
                if (num == NULL)
                  {
                    req->invalid = ORC_INVALID_JSON;
                    return -1;
                  }
                d->input[d->n_input++] = strtod (num, NULL);
                skip_ws (sc);
                {
                  int c2 = next (sc);

                  if (c2 == ',')
                    continue;
                  if (c2 == ']')
                    break;
                  req->invalid = ORC_INVALID_JSON;
                  return -1;
                }
            }
          have_input = true;
        }
      else if (strcmp (keybuf, "aggregate") == 0)
        {
          const char *a;
          size_t alen = 0;

          a = parse_string (sc, &alen);
          if (a == NULL)
            {
              req->invalid = ORC_INVALID_JSON;
              return -1;
            }
          if (strcmp (a, "majority") != 0 && strcmp (a, "mean") != 0)
            {
              req->invalid = ORC_INVALID_VALUE;
              set_detail (req, "aggregate");
              return -1;
            }
          strcpy (d->aggregate, a);
          have_aggregate = true;
        }
      else
        {
          req->invalid = ORC_INVALID_KEY;
          set_detail (req, keybuf);
          return -1;
        }

      skip_ws (sc);
      {
        int c = next (sc);

        if (c == ',')
          continue;
        if (c == '}')
          {
            desc_end = sc->pos;
            break;
          }
        req->invalid = ORC_INVALID_JSON;
        return -1;
      }
    }

  if (!have_instances)
    {
      req->invalid = ORC_INVALID_FIELD;
      set_detail (req, "instances");
      return -1;
    }
  if (!have_topologies || d->n_topologies == 0)
    {
      req->invalid = ORC_INVALID_FIELD;
      set_detail (req, "topologies");
      return -1;
    }
  if ((!have_input || d->n_input == 0) && !req->input_optional)
    {
      req->invalid = ORC_INVALID_FIELD;
      set_detail (req, "input");
      return -1;
    }
  if (!have_aggregate)
    {
      req->invalid = ORC_INVALID_FIELD;
      set_detail (req, "aggregate");
      return -1;
    }

  /* Persist the descriptor verbatim: what the caller wrote is
   * what the base keeps (SPEC.md section 6). */
  {
    size_t n = desc_end - desc_start;

    if (n >= sizeof d->raw)
      n = sizeof d->raw - 1;
    memcpy (d->raw, sc->s + desc_start, n);
    d->raw[n] = '\0';
  }
  return 0;
}
