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
 *  Scanner (the same proven shape as the /db contract layer).
 * ------------------------------------------------------------------- */

void
fail (struct scanner *sc, int err)
{
  if (sc->err == 0)
    sc->err = err;
}

char *
arena_take (struct scanner *sc, size_t n)
{
  char *p;

  if (sc->scratch_len + n > sc->scratch_cap)
    {
      fail (sc, 2);
      return NULL;
    }
  p = sc->scratch + sc->scratch_len;
  sc->scratch_len += n;
  return p;
}

void
skip_ws (struct scanner *sc)
{
  while (sc->pos < sc->len)
    {
      char c = sc->s[sc->pos];

      if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
        sc->pos++;
      else
        break;
    }
}

int
peek (struct scanner *sc)
{
  return sc->pos < sc->len ? (unsigned char) sc->s[sc->pos] : -1;
}

int
next (struct scanner *sc)
{
  return sc->pos < sc->len ? (unsigned char) sc->s[sc->pos++] : -1;
}

static size_t
utf8_len (unsigned long cp)
{
  if (cp < 0x80)
    return 1;
  if (cp < 0x800)
    return 2;
  if (cp < 0x10000)
    return 3;
  return 4;
}

static void
put_utf8 (char **out, unsigned long cp)
{
  char *p = *out;

  if (cp < 0x80)
    p[0] = (char) cp;
  else if (cp < 0x800)
    {
      p[0] = (char) (0xC0 | (cp >> 6));
      p[1] = (char) (0x80 | (cp & 0x3F));
    }
  else if (cp < 0x10000)
    {
      p[0] = (char) (0xE0 | (cp >> 12));
      p[1] = (char) (0x80 | ((cp >> 6) & 0x3F));
      p[2] = (char) (0x80 | (cp & 0x3F));
    }
  else
    {
      p[0] = (char) (0xF0 | (cp >> 18));
      p[1] = (char) (0x80 | ((cp >> 12) & 0x3F));
      p[2] = (char) (0x80 | ((cp >> 6) & 0x3F));
      p[3] = (char) (0x80 | (cp & 0x3F));
    }
  *out = p + utf8_len (cp);
}

static unsigned long
read_hex4 (struct scanner *sc)
{
  unsigned long cp = 0;

  for (int i = 0; i < 4; i++)
    {
      int c = next (sc);
      int v;

      if (c >= '0' && c <= '9') v = c - '0';
      else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
      else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
      else
        {
          fail (sc, 1);
          return (unsigned long) -1;
        }
      cp = cp * 16 + (unsigned long) v;
    }
  return cp;
}

/* Decode a JSON string into the arena (reservation trimmed at the
 * closing quote, so at most one reservation is live at a time). */
const char *
parse_string (struct scanner *sc, size_t *len_out)
{
  size_t base = sc->scratch_len;
  char *start;
  char *out;

  if (next (sc) != '"')
    {
      fail (sc, 1);
      return NULL;
    }
  start = arena_take (sc, sc->len - sc->pos + 1);
  if (start == NULL)
    return NULL;
  out = start;

  for (;;)
    {
      int c = peek (sc);

      if (c < 0 || c < 0x20)
        {
          fail (sc, 1);
          return NULL;
        }
      if (c == '"')
        {
          sc->pos++;
          *out = '\0';
          sc->scratch_len = base + (size_t) (out - start) + 1;
          if (len_out)
            *len_out = (size_t) (out - start);
          return start;
        }
      if (c == '\\')
        {
          sc->pos++;
          int e = next (sc);

          switch (e)
            {
            case '"':  *out++ = '"';  break;
            case '\\': *out++ = '\\'; break;
            case '/':  *out++ = '/';  break;
            case 'b':  *out++ = '\b'; break;
            case 'f':  *out++ = '\f'; break;
            case 'n':  *out++ = '\n'; break;
            case 'r':  *out++ = '\r'; break;
            case 't':  *out++ = '\t'; break;
            case 'u':
              {
                unsigned long cp = read_hex4 (sc);

                if (sc->err)
                  return NULL;
                if (cp >= 0xD800 && cp <= 0xDBFF
                    && sc->pos + 1 < sc->len
                    && sc->s[sc->pos] == '\\'
                    && sc->s[sc->pos + 1] == 'u')
                  {
                    unsigned long lo;

                    sc->pos += 2;
                    lo = read_hex4 (sc);
                    if (sc->err)
                      return NULL;
                    if (lo >= 0xDC00 && lo <= 0xDFFF)
                      cp = 0x10000 + ((cp - 0xD800) << 10)
                           + (lo - 0xDC00);
                    else
                      {
                        fail (sc, 1);
                        return NULL;
                      }
                  }
                put_utf8 (&out, cp);
                break;
              }
            default:
              fail (sc, 1);
              return NULL;
            }
          continue;
        }
      *out++ = (char) c;
      sc->pos++;
    }
}

/* Walk one JSON value without decoding; returns its [start, end)
 * span inside the LINE. */
int
skip_value (struct scanner *sc, size_t *start_out, size_t *end_out)
{
  size_t start = sc->pos;
  int c = peek (sc);

  if (c == '"')
    {
      size_t ignored;

      if (parse_string (sc, &ignored) == NULL)
        return -1;
      *start_out = start;
      *end_out = sc->pos;
      return 0;
    }
  if (c == '{' || c == '[')
    {
      int depth = 0;

      while (sc->pos < sc->len)
        {
          int d = peek (sc);

          if (d == '"')
            {
              size_t ignored;

              if (parse_string (sc, &ignored) == NULL)
                return -1;
              continue;
            }
          sc->pos++;
          if (d == '{' || d == '[')
            depth++;
          else if (d == '}' || d == ']')
            {
              depth--;
              if (depth == 0)
                {
                  *start_out = start;
                  *end_out = sc->pos;
                  return 0;
                }
              if (depth < 0)
                {
                  fail (sc, 1);
                  return -1;
                }
            }
        }
      fail (sc, 1);
      return -1;
    }
  if (c == 't' && sc->len - sc->pos >= 4
      && memcmp (sc->s + sc->pos, "true", 4) == 0)
    {
      sc->pos += 4;
      *start_out = start;
      *end_out = sc->pos;
      return 0;
    }
  if (c == 'f' && sc->len - sc->pos >= 5
      && memcmp (sc->s + sc->pos, "false", 5) == 0)
    {
      sc->pos += 5;
      *start_out = start;
      *end_out = sc->pos;
      return 0;
    }
  if (c == 'n' && sc->len - sc->pos >= 4
      && memcmp (sc->s + sc->pos, "null", 4) == 0)
    {
      sc->pos += 4;
      *start_out = start;
      *end_out = sc->pos;
      return 0;
    }
  if (c == '-' || (c >= '0' && c <= '9'))
    {
      sc->pos++;
      for (;;)
        {
          int d = peek (sc);

          if ((d >= '0' && d <= '9') || d == '.' || d == 'e'
              || d == 'E' || d == '+' || d == '-')
            sc->pos++;
          else
            break;
        }
      *start_out = start;
      *end_out = sc->pos;
      return 0;
    }
  fail (sc, 1);
  return -1;
}

/* Copy a span of the LINE into the arena, verbatim, NUL
 * terminated. */
const char *
copy_raw (struct scanner *sc, size_t start, size_t end)
{
  char *p = arena_take (sc, end - start + 1);

  if (p == NULL)
    return NULL;
  memcpy (p, sc->s + start, end - start);
  p[end - start] = '\0';
  return p;
}
