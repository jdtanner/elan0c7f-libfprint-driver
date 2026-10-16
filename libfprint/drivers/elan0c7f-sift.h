/*
 * SIFT/RANSAC fingerprint matcher for the ELAN 04f3:0c7f sensor
 *
 * Copyright (C) 2026 John Hill-Tanner
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#pragma once

#include <glib.h>

G_BEGIN_DECLS

#define ELAN0C7F_SIFT_DESC_LEN 128

/* Keypoints of one image: n positions (x, y interleaved, in upscaled image
 * coordinates) and n descriptors of ELAN0C7F_SIFT_DESC_LEN bytes each. */
typedef struct
{
  guint    n;
  gdouble *xy;
  guint8  *desc;
} Elan0c7fFeatures;

/* Extract features from an 8-bit image with dark ridges, after dividing out
 * the sensor's fixed power-icon pattern (for 52 x 150 images).  Fills @out,
 * which must be released with elan0c7f_features_clear(). */
void elan0c7f_sift_extract (const guint8     *pixels,
                            gint              width,
                            gint              height,
                            Elan0c7fFeatures *out);

/* Number of descriptor matches between @a and @b that agree on a single
 * rotation + translation (near-unit scale), found with RANSAC.  Unrelated
 * fingers score low because coincidental matches point in random
 * directions. */
gint elan0c7f_sift_score (const Elan0c7fFeatures *a,
                          const Elan0c7fFeatures *b);

void elan0c7f_features_clear (Elan0c7fFeatures *features);

G_END_DECLS
