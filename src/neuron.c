/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org> */

/*
 * neuron.c — The frozen orchestrator-to-neuron wire formats.
 *
 * The orchestrator never interprets what a neuron computes: it
 * formats the contract's lines in, and picks the numbers out of
 * the state text out.  Everything here is deliberate, documented
 * and tested against the formats of neuron-translator (SPEC.md
 * section 2).
 */

#include "neuron.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

size_t
orc_neuron_input_line (const double *v, int n, char *buf, size_t cap)
{
  size_t len = 0;

  for (int i = 0; i < n; i++)
    {
      int w = snprintf (buf + len, cap > len ? cap - len : 0,
                        i > 0 ? ",%.6g" : "%.6g", v[i]);

      if (w < 0 || (size_t) w >= (cap > len ? cap - len : 0))
        return 0;                           /* never truncate a line */
      len += (size_t) w;
    }
  return len;
}

/* One output line of the neuron state text looks like
 * "  [3]: 0.931223" — two spaces, the index in brackets, a colon
 * and the value.  The scan stays tolerant of the exact leading
 * whitespace but strict about the bracket shape. */
int
orc_neuron_parse_outputs (const char *text, size_t text_len,
                          double *out, int max)
{
  int highest = -1;
  size_t i = 0;

  memset (out, 0, (size_t) max * sizeof *out);

  while (i < text_len)
    {
      /* Find the next '[' that could start an output line. */
      const char *nl = memchr (text + i, '\n', text_len - i);
      size_t line_len = nl != NULL ? (size_t) (nl - (text + i))
                                   : text_len - i;
      const char *line = text + i;
      const char *lb = memchr (line, '[', line_len);

      if (lb != NULL)
        {
          /* Bounded parse: every scan below stays inside
           * [line, line_end), whatever the caller's buffer looks
           * like past text_len. */
          const char *p = lb + 1;
          const char *line_end = line + line_len;
          long idx = 0;
          bool have_digits = false;

          while (p < line_end && *p == ' ')
            p++;
          while (p < line_end && *p >= '0' && *p <= '9')
            {
              if (idx > max)
                idx = max + 1;          /* clamp: no long overflow */
              else
                idx = idx * 10 + (*p - '0');
              have_digits = true;
              p++;
            }
          if (have_digits && idx >= 0 && idx < max
              && p < line_end && *p == ']')
            {
              const char *colon = memchr (p + 1, ':',
                                          (size_t) (line_end - (p + 1)));

              if (colon != NULL)
                {
                  /* Copy the value span into a bounded buffer:
                   * strtod itself is not length aware.  64 bytes
                   * cover any %.6f double. */
                  size_t vlen = (size_t) (line_end - (colon + 1));
                  char vbuf[64];

                  if (vlen >= sizeof vbuf)
                    vlen = sizeof vbuf - 1;
                  memcpy (vbuf, colon + 1, vlen);
                  vbuf[vlen] = '\0';
                  out[idx] = strtod (vbuf, NULL);
                  if (idx > highest)
                    highest = (int) idx;
                }
            }
        }
      if (nl == NULL)
        break;
      i = (size_t) (nl - text) + 1;
    }

  return highest >= 0 ? highest + 1 : -1;
}
