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
 *  orc_parse — one command line to one request.
 * ------------------------------------------------------------------- */

void
orc_parse (const char *line, size_t len,
           char *scratch, size_t scratch_cap,
           struct orc_request *req)
{
  struct scanner sc;
  bool have_command = false;
  bool have_run = false;
  bool have_descriptor_keys = false;

  memset (req, 0, sizeof *req);
  req->command = ORC_CMD_INVALID;
  req->invalid = ORC_INVALID_LINE;

  sc.s = line;
  sc.pos = 0;
  sc.len = len;
  sc.scratch = scratch;
  sc.scratch_cap = scratch_cap;
  sc.scratch_len = 0;
  sc.err = 0;

  skip_ws (&sc);
  if (peek (&sc) != '{')
    return;
  sc.pos++;

  skip_ws (&sc);
  if (peek (&sc) == '}')
    {
      sc.pos++;
      skip_ws (&sc);
      if (sc.pos == sc.len)
        req->invalid = ORC_INVALID_FIELD;   /* empty is not a command */
      else
        req->invalid = ORC_INVALID_LINE;
      set_detail (req, "run");
      return;
    }

  for (;;)
    {
      const char *key;
      size_t keylen = 0;
      char keybuf[32];

      skip_ws (&sc);
      key = parse_string (&sc, &keylen);
      if (key == NULL)
        {
          req->invalid = ORC_INVALID_JSON;
          return;
        }
      if (keylen >= sizeof keybuf)
        {
          req->invalid = ORC_INVALID_KEY;
          set_detail (req, key);
          return;
        }
      memcpy (keybuf, key, keylen);
      keybuf[keylen] = '\0';

      skip_ws (&sc);
      if (next (&sc) != ':')
        {
          req->invalid = ORC_INVALID_JSON;
          return;
        }
      skip_ws (&sc);

      if (strcmp (keybuf, "run") == 0)
        {
          if (parse_descriptor (&sc, req) < 0)
            {
              if (req->invalid == ORC_INVALID_LINE)
                req->invalid = ORC_INVALID_JSON;
              return;
            }
          have_run = true;
        }
      else if (strcmp (keybuf, "command") == 0)
        {
          const char *c;
          size_t clen = 0;

          c = parse_string (&sc, &clen);
          if (c == NULL)
            {
              req->invalid = ORC_INVALID_JSON;
              return;
            }
          if (strcmp (c, "status") == 0)
            req->command = ORC_CMD_STATUS;
          else if (strcmp (c, "result") == 0)
            req->command = ORC_CMD_RESULT;
          else
            {
              req->invalid = ORC_INVALID_VALUE;
              set_detail (req, "command");
              return;
            }
          have_command = true;
        }
      else if (strcmp (keybuf, "instances") == 0
               || strcmp (keybuf, "topologies") == 0
               || strcmp (keybuf, "input") == 0
               || strcmp (keybuf, "aggregate") == 0)
        {
          /* A bare descriptor: rewind onto the key and parse the
           * whole object as the descriptor.  Mixed with an
           * explicit "run" it is not a command shape. */
          if (have_run)
            {
              req->invalid = ORC_INVALID_KEY;
              set_detail (req, keybuf);
              return;
            }
          while (sc.pos > 0 && sc.s[sc.pos] != '{')
            sc.pos--;
          if (sc.s[sc.pos] != '{')
            {
              req->invalid = ORC_INVALID_JSON;
              return;
            }
          if (parse_descriptor (&sc, req) < 0)
            {
              if (req->invalid == ORC_INVALID_LINE)
                req->invalid = ORC_INVALID_JSON;
              return;
            }
          have_descriptor_keys = true;
          /* A bare descriptor IS the whole top-level object: it
           * was consumed entirely, the command is complete. */
          break;
        }
      else
        {
          req->invalid = ORC_INVALID_KEY;
          set_detail (req, keybuf);
          return;
        }

      skip_ws (&sc);
      {
        int c = next (&sc);

        if (c == ',')
          continue;
        if (c == '}')
          break;
        req->invalid = ORC_INVALID_JSON;
        return;
      }
    }

  skip_ws (&sc);
  if (sc.pos != sc.len)
    {
      req->invalid = ORC_INVALID_LINE;
      return;
    }

  if (have_run || have_descriptor_keys)
    {
      if (have_command)
        {
          req->invalid = ORC_INVALID_KEY;
          set_detail (req, "command");
          return;
        }
      req->command = ORC_CMD_RUN;
      return;
    }
  if (have_command)
    return;                                 /* STATUS or RESULT */

  req->invalid = ORC_INVALID_FIELD;
  set_detail (req, "run");
}
