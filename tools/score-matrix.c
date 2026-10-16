/*
 * score-matrix: offline Bozorth3 scoring of saved elan0c7f captures.
 *
 * Runs each PGM through the same libfprint minutiae extraction and
 * Bozorth3 code that fprintd uses (linked from a libfprint build tree that
 * includes the elan0c7f patches), and prints:
 *
 *   N <index> <nrows> <path>          one line per image (nrows = minutiae
 *                                     kept for matching; -1 = rejected)
 *   S <probe> <gallery> <score>       one line per ordered pair
 *
 * Images must come from capture.py: libfprint has already applied the
 * driver's colour inversion, so they are loaded with no image flags.
 *
 * Build against a configured and built libfprint tree (L):
 *   gcc -O2 -Wall -o score-matrix score-matrix.c \
 *     -I$L/libfprint -I$L/_build/libfprint -I$L/_build \
 *     -I$L/libfprint/nbis/include -I$L/libfprint/nbis/libfprint-include \
 *     $L/_build/libfprint/libfprint-private.a $L/_build/libfprint/libnbis.a \
 *     -L$L/_build/libfprint -lfprint-2 -Wl,-rpath,$L/_build/libfprint \
 *     $(pkg-config --cflags --libs glib-2.0 gobject-2.0 gio-2.0 gmodule-2.0 \
 *       gusb pixman-1 libudev openssl) -lm
 *
 * Copyright (C) 2026 John Hill-Tanner
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include <stdio.h>
#include <string.h>

#include "fpi-image.h"
#include "fpi-print.h"
#include "fp-print-private.h"
#include "bozorth.h"

#define SENSOR_PPMM 19.685f

static FpImage *
load_pgm (const char *path)
{
  g_autofree gchar *contents = NULL;
  gsize len;
  int w, h, maxval, off = 0;
  FpImage *img;

  if (!g_file_get_contents (path, &contents, &len, NULL))
    return NULL;
  if (sscanf (contents, "P5 %d %d %d%n", &w, &h, &maxval, &off) != 3 ||
      maxval != 255 || len < (gsize) off + 1 + (gsize) (w * h))
    return NULL;

  img = fp_image_new (w, h);
  memcpy (img->data, contents + off + 1, w * h);
  img->ppmm = SENSOR_PPMM;
  img->flags = 0;
  return img;
}

static void
detect_done (GObject *src, GAsyncResult *res, gpointer user_data)
{
  gboolean *done = user_data;
  g_autoptr(GError) error = NULL;

  if (!fp_image_detect_minutiae_finish (FP_IMAGE (src), res, &error))
    g_printerr ("minutiae detection failed: %s\n", error->message);
  *done = TRUE;
}

/* Returns the matching template, or NULL if libfprint would reject it. */
static struct xyt_struct *
image_to_xyt (FpImage *img)
{
  g_autoptr(FpPrint) print = NULL;
  g_autoptr(GError) error = NULL;
  gboolean done = FALSE;

  fp_image_detect_minutiae (img, NULL, detect_done, &done);
  while (!done)
    g_main_context_iteration (NULL, TRUE);

  /* No device is involved; FpPrint only insists these are set. */
  print = g_object_new (FP_TYPE_PRINT,
                        "driver", "elan0c7f",
                        "device-id", "offline",
                        NULL);
  fpi_print_set_type (print, FPI_PRINT_NBIS);
  if (!fpi_print_add_from_image (print, img, &error))
    return NULL;

  return g_memdup2 (g_ptr_array_index (print->prints, 0),
                    sizeof (struct xyt_struct));
}

int
main (int argc, char **argv)
{
  g_autoptr(GPtrArray) xyts = g_ptr_array_new_with_free_func (g_free);

  if (argc < 3)
    {
      g_printerr ("usage: %s IMAGE.pgm IMAGE.pgm...\n", argv[0]);
      return 2;
    }

  for (int i = 1; i < argc; i++)
    {
      g_autoptr(FpImage) img = load_pgm (argv[i]);
      struct xyt_struct *xyt = NULL;

      if (!img)
        {
          g_printerr ("cannot read %s as an 8-bit P5 PGM\n", argv[i]);
          return 1;
        }
      xyt = image_to_xyt (img);
      g_ptr_array_add (xyts, xyt);
      printf ("N %d %d %s\n", i - 1, xyt ? xyt->nrows : -1, argv[i]);
    }

  for (guint p = 0; p < xyts->len; p++)
    {
      struct xyt_struct *probe = g_ptr_array_index (xyts, p);
      int probe_len;

      if (!probe)
        continue;
      probe_len = bozorth_probe_init (probe);
      for (guint g = 0; g < xyts->len; g++)
        {
          struct xyt_struct *gallery = g_ptr_array_index (xyts, g);

          if (g == p || !gallery)
            continue;
          printf ("S %u %u %d\n", p, g,
                  bozorth_to_gallery (probe_len, probe, gallery));
        }
    }

  return 0;
}
