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

/*
 * The 52 px wide images yield only 3-13 minutiae, too few for Bozorth3 to
 * tell fingers apart.  Matching image texture with SIFT keypoints gives
 * ~50-130 features per image instead.  The parameters match the prototype
 * the accuracy was measured with (tools/sift-score.py in the
 * elan0c7f-libfprint-driver repository); change them together.
 */

#include "elan0c7f-sift.h"
#include "elan0c7f-gain.h"

#include <cmath>
#include <cstring>
#include <vector>

#include <opencv2/calib3d.hpp>
#include <opencv2/features2d.hpp>
#include <opencv2/imgproc.hpp>

namespace {

const int UPSCALE = 3;          /* SIFT finds too few features at 52 px wide */
const float RATIO = 0.8f;       /* Lowe's ratio test */
const double RANSAC_PX = 3.0 * UPSCALE;
const double MIN_SCALE = 0.8;   /* Same sensor, so scale stays near 1 */
const double MAX_SCALE = 1.25;

/* Divide out the power-icon pattern (elan0c7f-gain.h) and rescale to the
 * full 8-bit range.  Other image sizes are passed through unchanged. */
cv::Mat
flatten (const cv::Mat &img)
{
  cv::Mat gain, f, out;

  if (img.cols != ELAN0C7F_GAIN_WIDTH || img.rows != ELAN0C7F_GAIN_HEIGHT)
    return img;

  cv::Mat (ELAN0C7F_GAIN_HEIGHT, ELAN0C7F_GAIN_WIDTH, CV_8U,
           const_cast<unsigned char *> (elan0c7f_gain_map))
  .convertTo (gain, CV_32F, 1.0 / 255, 0.5);
  img.convertTo (f, CV_32F);
  cv::normalize (f / gain, out, 0, 255, cv::NORM_MINMAX, CV_8U);
  return out;
}

cv::Mat
descriptors_of (const Elan0c7fFeatures *f)
{
  cv::Mat bytes (f->n, ELAN0C7F_SIFT_DESC_LEN, CV_8U, f->desc);
  cv::Mat floats;

  bytes.convertTo (floats, CV_32F);
  return floats;
}

}

void
elan0c7f_sift_extract (const guint8 *pixels, gint width, gint height,
                       Elan0c7fFeatures *out)
{
  cv::Mat img (height, width, CV_8U, const_cast<guint8 *> (pixels));
  cv::Mat big, eq, desc;
  std::vector<cv::KeyPoint> kps;

  cv::resize (flatten (img), big, cv::Size (), UPSCALE, UPSCALE, cv::INTER_CUBIC);
  cv::createCLAHE (2.0, cv::Size (4, 4))->apply (big, eq);
  cv::SIFT::create ()->detectAndCompute (eq, cv::noArray (), kps, desc);

  out->n = kps.size ();
  out->xy = g_new (gdouble, 2 * out->n);
  out->desc = g_new (guint8, (gsize) out->n * ELAN0C7F_SIFT_DESC_LEN);
  for (guint i = 0; i < out->n; i++)
    {
      out->xy[2 * i] = kps[i].pt.x;
      out->xy[2 * i + 1] = kps[i].pt.y;
      /* SIFT descriptor values are whole numbers in 0-255. */
      for (int j = 0; j < ELAN0C7F_SIFT_DESC_LEN; j++)
        out->desc[i * ELAN0C7F_SIFT_DESC_LEN + j] =
          cv::saturate_cast<guint8> (desc.at<float> (i, j));
    }
}

gint
elan0c7f_sift_score (const Elan0c7fFeatures *a, const Elan0c7fFeatures *b)
{
  std::vector<std::vector<cv::DMatch>> knn;
  std::vector<cv::Point2f> src, dst;
  cv::Mat inliers;

  if (a->n < 2 || b->n < 2)
    return 0;

  cv::BFMatcher (cv::NORM_L2).knnMatch (descriptors_of (a),
                                        descriptors_of (b), knn, 2);
  for (const auto &m : knn)
    {
      if (m.size () == 2 && m[0].distance < RATIO * m[1].distance)
        {
          src.emplace_back (a->xy[2 * m[0].queryIdx],
                            a->xy[2 * m[0].queryIdx + 1]);
          dst.emplace_back (b->xy[2 * m[0].trainIdx],
                            b->xy[2 * m[0].trainIdx + 1]);
        }
    }
  if (src.size () < 3)
    return 0;

  cv::Mat model = cv::estimateAffinePartial2D (src, dst, inliers,
                                               cv::RANSAC, RANSAC_PX);
  if (model.empty ())
    return 0;

  double scale = std::hypot (model.at<double> (0, 0), model.at<double> (1, 0));
  if (scale < MIN_SCALE || scale > MAX_SCALE)
    return 0;

  return cv::countNonZero (inliers);
}

void
elan0c7f_features_clear (Elan0c7fFeatures *features)
{
  g_clear_pointer (&features->xy, g_free);
  g_clear_pointer (&features->desc, g_free);
  features->n = 0;
}
