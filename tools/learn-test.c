/*
 * learn-test: unit tests for the driver's learned-sample store
 * (elan0c7f-learn.c), which needs no sensor and no OpenCV.
 *
 * Build against a libfprint tree with the driver (L) and run:
 *   gcc -O2 -Wall -o learn-test learn-test.c $L/libfprint/drivers/elan0c7f-learn.c \
 *     -I$L/libfprint/drivers $(pkg-config --cflags --libs glib-2.0)
 *   ./learn-test
 *
 * Copyright (C) 2026 John Hill-Tanner
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include <sys/stat.h>
#include <glib.h>
#include <glib/gstdio.h>

#include "elan0c7f-learn.h"

/* A sample whose single keypoint is at (tag, tag): distinguishable, and the
 * descriptor length does not matter to the store. */
static GVariant *
sample (double tag)
{
  double xy[2] = { tag, tag };
  guint8 desc[4] = { (guint8) tag, 1, 2, 3 };

  return g_variant_ref_sink (g_variant_new ("(@ad@ay)",
                                            g_variant_new_fixed_array (G_VARIANT_TYPE_DOUBLE, xy, 2, sizeof (double)),
                                            g_variant_new_fixed_array (G_VARIANT_TYPE_BYTE, desc, 4, 1)));
}

static double
tag_of (GVariant *samples, gsize i)
{
  g_autoptr(GVariant) s = g_variant_get_child_value (samples, i);
  g_autoptr(GVariant) xy = g_variant_get_child_value (s, 0);
  gsize n;
  const double *v = g_variant_get_fixed_array (xy, &n, sizeof (double));

  return v[0];
}

static GVariant *
samples_of (int from, int to)
{
  GVariant *out = NULL;

  for (int t = from; t <= to; t++)
    {
      g_autoptr(GVariant) s = sample (t);
      GVariant *next = elan0c7f_learn_add (out, s, 1000);

      g_clear_pointer (&out, g_variant_unref);
      out = next;
    }
  return out;
}

static gchar *
tmpdir (void)
{
  g_autoptr(GError) error = NULL;
  gchar *d = g_dir_make_tmp ("learn-test-XXXXXX", &error);

  g_assert_no_error (error);
  return d;
}

static void
rm_rf (const char *dir)
{
  g_autoptr(GDir) d = g_dir_open (dir, 0, NULL);
  const char *name;

  if (!d)
    return;
  while ((name = g_dir_read_name (d)))
    {
      g_autofree gchar *p = g_build_filename (dir, name, NULL);

      if (g_file_test (p, G_FILE_TEST_IS_DIR))
        rm_rf (p);
      else
        g_unlink (p);
    }
  g_rmdir (dir);
}

static void
test_dir (void)
{
  g_unsetenv ("ELAN0C7F_LEARN_DIR");
  g_assert_cmpstr (elan0c7f_learn_dir (), ==, ELAN0C7F_LEARN_DIR_DEFAULT);
  g_setenv ("ELAN0C7F_LEARN_DIR", "", TRUE);
  g_assert_null (elan0c7f_learn_dir ());
  g_setenv ("ELAN0C7F_LEARN_DIR", "/somewhere", TRUE);
  g_assert_cmpstr (elan0c7f_learn_dir (), ==, "/somewhere");
  g_unsetenv ("ELAN0C7F_LEARN_DIR");
}

static void
test_key (void)
{
  g_autoptr(GVariant) a = samples_of (1, 3);
  g_autoptr(GVariant) a2 = samples_of (1, 3);
  g_autoptr(GVariant) b = samples_of (1, 4);
  g_autofree gchar *ka = elan0c7f_learn_key (a);
  g_autofree gchar *ka2 = elan0c7f_learn_key (a2);
  g_autofree gchar *kb = elan0c7f_learn_key (b);

  g_assert_cmpuint (strlen (ka), ==, 64);
  g_assert_cmpstr (ka, ==, ka2);
  g_assert_cmpstr (ka, !=, kb);
}

static void
test_add_caps_dropping_oldest (void)
{
  g_autoptr(GVariant) s = sample (99);
  g_autoptr(GVariant) one = elan0c7f_learn_add (NULL, s, 3);
  g_autoptr(GVariant) five = samples_of (1, 5);
  g_autoptr(GVariant) capped = elan0c7f_learn_add (five, s, 3);
  g_autoptr(GVariant) two = samples_of (1, 2);
  g_autoptr(GVariant) grown = elan0c7f_learn_add (two, s, 3);

  g_assert_cmpuint (g_variant_n_children (one), ==, 1);

  /* Newest last, oldest gone. */
  g_assert_cmpuint (g_variant_n_children (capped), ==, 3);
  g_assert_cmpfloat (tag_of (capped, 0), ==, 4);
  g_assert_cmpfloat (tag_of (capped, 1), ==, 5);
  g_assert_cmpfloat (tag_of (capped, 2), ==, 99);

  /* Under the cap nothing is dropped. */
  g_assert_cmpuint (g_variant_n_children (grown), ==, 3);
  g_assert_cmpfloat (tag_of (grown, 0), ==, 1);
}

static void
test_round_trip_and_permissions (void)
{
  g_autofree gchar *base = tmpdir ();
  g_autofree gchar *dir = g_build_filename (base, "nested", "learned", NULL);
  g_autoptr(GVariant) learned = samples_of (1, 3);
  g_autoptr(GVariant) back = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree gchar *path = g_build_filename (dir, "k", NULL);
  GStatBuf st;

  g_assert_null (elan0c7f_learn_load (dir, "k"));   /* missing: nothing yet */

  g_assert_true (elan0c7f_learn_save (dir, "k", learned, &error));
  g_assert_no_error (error);

  back = elan0c7f_learn_load (dir, "k");
  g_assert_nonnull (back);
  g_assert_true (g_variant_equal (back, learned));

  g_assert_cmpint (g_stat (dir, &st), ==, 0);
  g_assert_cmpint (st.st_mode & 0777, ==, 0700);
  g_assert_cmpint (g_stat (path, &st), ==, 0);
  g_assert_cmpint (st.st_mode & 0777, ==, 0600);

  rm_rf (base);
}

static void
write_raw (const char *dir, const char *key, const void *data, gsize len)
{
  g_autofree gchar *p = g_build_filename (dir, key, NULL);

  g_assert_true (g_file_set_contents (p, data, len, NULL));
}

static void
test_bad_files_read_as_nothing (void)
{
  g_autofree gchar *dir = tmpdir ();
  g_autoptr(GVariant) learned = samples_of (1, 3);
  g_autoptr(GVariant) other = NULL;
  g_autoptr(GVariant) normal = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree gchar *path = g_build_filename (dir, "good", NULL);
  g_autofree gchar *contents = NULL;
  gsize len;

  g_assert_true (elan0c7f_learn_save (dir, "good", learned, &error));
  g_assert_true (g_file_get_contents (path, &contents, &len, NULL));

  /* Truncated: the version survives but the samples do not. */
  write_raw (dir, "truncated", contents, len - 5);
  g_test_expect_message (NULL, G_LOG_LEVEL_WARNING, "*malformed*");
  g_assert_null (elan0c7f_learn_load (dir, "truncated"));
  g_test_assert_expected_messages ();

  /* Another format version. */
  other = g_variant_ref_sink (g_variant_new ("(u@a(aday))", 2, learned));
  normal = g_variant_get_normal_form (other);
  write_raw (dir, "v2", g_variant_get_data (normal), g_variant_get_size (normal));
  g_assert_null (elan0c7f_learn_load (dir, "v2"));

  /* Empty, and something else entirely. */
  write_raw (dir, "empty", "", 0);
  g_test_expect_message (NULL, G_LOG_LEVEL_WARNING, "*malformed*");
  g_assert_null (elan0c7f_learn_load (dir, "empty"));
  g_test_assert_expected_messages ();

  write_raw (dir, "garbage", "FP3 not a learned file at all", 29);
  g_test_expect_message (NULL, G_LOG_LEVEL_WARNING, "*malformed*");
  g_assert_null (elan0c7f_learn_load (dir, "garbage"));
  g_test_assert_expected_messages ();

  rm_rf (dir);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/learn/dir", test_dir);
  g_test_add_func ("/learn/key", test_key);
  g_test_add_func ("/learn/add-caps-dropping-oldest", test_add_caps_dropping_oldest);
  g_test_add_func ("/learn/round-trip-and-permissions", test_round_trip_and_permissions);
  g_test_add_func ("/learn/bad-files-read-as-nothing", test_bad_files_read_as_nothing);
  return g_test_run ();
}
