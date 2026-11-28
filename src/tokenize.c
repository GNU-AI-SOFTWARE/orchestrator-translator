/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org> */

#include "tokenize.h"

int
orc_tokenize (const char *text, size_t len, double *out, int max)
{
  int n = max;

  if (n <= 0)
    return 0;
  for (int i = 0; i < n; i++)
    {
      size_t start = len * (size_t) i / (size_t) n;
      size_t end = len * (size_t) (i + 1) / (size_t) n;
      unsigned long long sum = 0;
      size_t cnt = 0;

      for (size_t j = start; j < end; j++)
        {
          sum += (unsigned char) text[j];
          cnt++;
        }
      out[i] = cnt > 0 ? (double) sum / (double) cnt / 255.0 : 0.0;
    }
  return n;
}
