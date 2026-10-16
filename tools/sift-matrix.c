/*
 * sift-matrix: score saved captures with the driver's own SIFT matcher
 * (elan0c7f-sift.cpp, linked from a libfprint build tree), in the same N/S
 * format as score-matrix and sift-score.py.  Used to check that the
 * driver's C++ matcher reproduces the measured prototype.
 *
 * Build against a configured and built libfprint tree (L):
 *   gcc -O2 -Wall -o sift-matrix sift-matrix.c -I$L/libfprint/drivers \
 *     $L/_build/libfprint/libfprint-drivers.a \
 *     $(pkg-config --cflags --libs glib-2.0 opencv4) -lstdc++ -lm
 *
 * Copyright (C) 2026 John Hill-Tanner
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include <stdio.h>
#include <string.h>

#include "elan0c7f-sift.h"

static gboolean
load_pgm (const char *path, guint8 **pixels, int *w, int *h)
{
  g_autofree gchar *contents = NULL;
  gsize len;
  int maxval, off = 0;

  if (!g_file_get_contents (path, &contents, &len, NULL))
    return FALSE;
  if (sscanf (contents, "P5 %d %d %d%n", w, h, &maxval, &off) != 3 ||
      maxval != 255 || len < (gsize) off + 1 + (gsize) (*w * *h))
    return FALSE;
  *pixels = g_memdup2 (contents + off + 1, *w * *h);
  return TRUE;
}

int
main (int argc, char **argv)
{
  int n = argc - 1;
  g_autofree Elan0c7fFeatures *f = g_new0 (Elan0c7fFeatures, MAX (n, 1));

  if (n < 2)
    {
      g_printerr ("usage: %s IMAGE.pgm IMAGE.pgm...\n", argv[0]);
      return 2;
    }

  for (int i = 0; i < n; i++)
    {
      g_autofree guint8 *pixels = NULL;
      int w, h;

      if (!load_pgm (argv[i + 1], &pixels, &w, &h))
        {
          g_printerr ("cannot read %s as an 8-bit P5 PGM\n", argv[i + 1]);
          return 1;
        }
      elan0c7f_sift_extract (pixels, w, h, &f[i]);
      printf ("N %d %u %s\n", i, f[i].n, argv[i + 1]);
    }

  for (int i = 0; i < n; i++)
    for (int j = 0; j < n; j++)
      if (i != j)
        printf ("S %d %d %d\n", i, j, elan0c7f_sift_score (&f[i], &f[j]));

  for (int i = 0; i < n; i++)
    elan0c7f_features_clear (&f[i]);
  return 0;
}
