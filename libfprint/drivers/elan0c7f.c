/*
 * ELAN 04f3:0c7f Fingerprint Sensor driver for libfprint
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
 * Protocol reverse-engineered from Windows driver analysis and USB captures.
 *
 * Sensor: 52x150 pixels, 16-bit little-endian, 15600 bytes per frame.
 * All endpoints are bulk (not interrupt).
 *
 * Init sequence:
 *   write [9a 10] -> read EP83 (discard)
 *   write [9a 10] -> read EP83 (discard)
 *   write [00 0b]
 *   write [9a 10] -> read EP83 (discard)
 *
 * Arm and wait for finger (start of each capture cycle):
 *   write [40 31]
 *   write [40 3f] -> read EP83, which completes only once a finger is
 *   placed: 0x55 = finger detected, 0xca = busy, re-arm after 200 ms.
 *   Waiting on this instead of polling frames avoids streaming ~23 blank
 *   frames/s (~350 KB/s) over USB while idle.
 *
 * Frame capture:
 *   write [00 09] -> read EP82 -> 15600 bytes
 *   Pixels are uint16-LE, 150 rows x 52 cols.
 *   Used directly as portrait image (52 wide x 150 tall) without rotation.
 *   Ridges run vertically; this gives NBIS enough ridge length for minutiae.
 *   Convert to 8-bit by scaling (not just shifting).
 *
 * Match-on-chip (MoC):
 *   This sensor does not support match-on-chip.  Analysis of the Windows
 *   driver binaries (WbfUsbDriver.dll, EngineAdapter.dll, ELANFPService.exe)
 *   and a live USB capture of a Windows Hello session both confirm that the
 *   Windows driver operates in the same image-capture mode as this driver.
 *   All fingerprint matching is performed on the host.  The elanmoc command
 *   signatures [40 ff 01/03/13] are absent from all binaries and from the
 *   capture.  The earlier hypothesis that [40 ff 01] might be a MoC template-
 *   commit command is disproved.
 */

#define FP_COMPONENT "elan0c7f"

#include "drivers_api.h"
#include "elan0c7f-sift.h"

/* -- Constants -- */

#define EP_OUT 0x01
#define EP_IN 0x82
#define EP_STATUS 0x83
#define FRAME_SIZE 15600
#define RAW_WIDTH 52
#define RAW_HEIGHT 150
#define IMG_WIDTH RAW_WIDTH      /* 52 */
#define IMG_HEIGHT RAW_HEIGHT    /* 150 */
#define TIMEOUT_MS 3000
#define QUALITY_MIN 20
#define SENSOR_PPMM 19.685f   /* ~500 DPI */
#define REARM_AFTER_LOW_FRAMES 5  /* Re-arm after this many consecutive misses */
#define BUSY_RETRY_MS 200

#define RESP_FINGER 0x55
#define RESP_BUSY 0xca

/* Commands */
static const guint8 CMD_9A10[]   = { 0x9a, 0x10 };
static const guint8 CMD_000B[]   = { 0x00, 0x0b };
static const guint8 CMD_4031[]   = { 0x40, 0x31 };
static const guint8 CMD_403F[]   = { 0x40, 0x3f };
static const guint8 CMD_FRAME[]  = { 0x00, 0x09 };

/* -- State machine states -- */

/* Init/ARM SSM states */
enum {
  INIT_FLUSH_1,
  INIT_FLUSH_2,
  INIT_FLUSH_3,
  INIT_9A10_1_SEND,
  INIT_9A10_1_READ,
  INIT_9A10_2_SEND,
  INIT_9A10_2_READ,
  INIT_000B_SEND,
  INIT_9A10_3_SEND,
  INIT_9A10_3_READ,
  INIT_DONE,
  INIT_NUM_STATES,
};

/* Capture SSM states (separate enum, separate SSM) */
enum {
  CAPTURE_ARM_4031,
  CAPTURE_ARM_403F,
  CAPTURE_WAIT_FINGER,      /* Block on EP83 until the sensor sees a finger */
  CAPTURE_SEND_FRAME_CMD,
  CAPTURE_READ_FRAME,
  CAPTURE_CHECK_QUALITY,
  CAPTURE_AWAIT_LIFT,       /* Keep reading until finger is gone */
  CAPTURE_READ_LIFT_FRAME,  /* Read a frame to check for lift */
  CAPTURE_NUM_STATES,
};

/* -- Device struct -- */

struct _FpiDeviceElan0c7f
{
  FpImageDevice parent;

  guint8       *frame_buf;        /* Raw frame buffer (15600 bytes) */
  FpImage      *best_img;         /* Best captured image so far */
  gint          best_quality;     /* Quality of best image */
  gint          frame_count;      /* Frames captured in this cycle */
  gint          low_quality_count; /* Consecutive low-quality frames */
  gboolean      finger_on;        /* Finger currently detected? */
  gboolean      deactivating;     /* Deactivation requested? */
  gboolean      ssm_active;       /* An init or capture SSM is in flight? */
  GCancellable *cancellable;      /* Cancels the finger wait on deactivate */
};

G_DECLARE_FINAL_TYPE (FpiDeviceElan0c7f, fpi_device_elan0c7f, FPI,
                      DEVICE_ELAN0C7F, FpImageDevice);
G_DEFINE_TYPE (FpiDeviceElan0c7f, fpi_device_elan0c7f, FP_TYPE_IMAGE_DEVICE);

/* -- USB helpers -- */

static void
tolerant_read_cb (FpiUsbTransfer *transfer, FpDevice *dev,
                  gpointer user_data, GError *error)
{
  /* Swallow any errors (timeouts, pipe errors, etc) and just advance
   * the SSM. Used for flush reads and status discards where errors
   * are expected and harmless. */
  if (error)
    g_clear_error (&error);

  if (transfer->ssm)
    fpi_ssm_next_state (transfer->ssm);
}

static void
send_cmd (FpiSsm *ssm, FpDevice *dev, const guint8 *data, gsize len)
{
  FpiUsbTransfer *transfer = fpi_usb_transfer_new (dev);

  fpi_usb_transfer_fill_bulk_full (transfer, EP_OUT,
                                   (guint8 *) data, len, NULL);
  transfer->short_is_error = TRUE;
  transfer->ssm = ssm;
  fpi_usb_transfer_submit (transfer, TIMEOUT_MS, NULL,
                           fpi_ssm_usb_transfer_cb, NULL);
}

static void
read_status_discard (FpiSsm *ssm, FpDevice *dev)
{
  FpiUsbTransfer *transfer = fpi_usb_transfer_new (dev);

  fpi_usb_transfer_fill_bulk (transfer, EP_STATUS, 64);
  transfer->short_is_error = FALSE;
  transfer->ssm = ssm;
  fpi_usb_transfer_submit (transfer, 1000, NULL,
                           tolerant_read_cb, NULL);
}

/* -- Image conversion -- */

static FpImage *
frame_to_image (const guint8 *raw_data)
{
  /*
   * Raw data: 15600 bytes = 7800 uint16-LE pixels, 150 rows x 52 cols.
   * Used directly as a portrait image (52 wide x 150 tall).
   * Scale 16-bit to 8-bit using the actual per-frame value range.
   */
  const guint16 *pixels = (const guint16 *) raw_data;
  guint16 min_val = 65535, max_val = 0;
  FpImage *img;
  guint8 *out;

  /* Find the actual value range */
  for (int i = 0; i < RAW_HEIGHT * RAW_WIDTH; i++)
    {
      guint16 v = GUINT16_FROM_LE (pixels[i]);
      if (v < min_val)
        min_val = v;
      if (v > max_val)
        max_val = v;
    }

  fp_dbg ("frame stats: min=%u max=%u range=%u", min_val, max_val, max_val - min_val);

  img = fp_image_new (IMG_WIDTH, IMG_HEIGHT);
  out = img->data;

  if (max_val <= min_val)
    {
      fp_dbg ("frame is blank/uniform -- skipping");
      return img;
    }

  /* Scale 16-bit pixels to 8-bit. */
  for (int y = 0; y < RAW_HEIGHT; y++)
    {
      for (int x = 0; x < RAW_WIDTH; x++)
        {
          guint16 v = GUINT16_FROM_LE (pixels[y * RAW_WIDTH + x]);
          guint32 scaled = (guint32) (v - min_val) * 255u / (guint32) (max_val - min_val);
          out[y * IMG_WIDTH + x] = (guint8) scaled;
        }
    }

  /* 3x3 separable Gaussian blur to smooth ridge-edge noise. */
  {
    guint8 tmp[IMG_HEIGHT * IMG_WIDTH];

    /* Horizontal pass */
    for (int y = 0; y < IMG_HEIGHT; y++)
      {
        for (int x = 0; x < IMG_WIDTH; x++)
          {
            int xm = MAX (x - 1, 0);
            int xp = MIN (x + 1, IMG_WIDTH - 1);
            tmp[y * IMG_WIDTH + x] = (guint8)
                                     ((out[y * IMG_WIDTH + xm] +
                                       2 * (guint16) out[y * IMG_WIDTH + x] +
                                       out[y * IMG_WIDTH + xp] + 2) / 4);
          }
      }
    /* Vertical pass */
    for (int y = 0; y < IMG_HEIGHT; y++)
      {
        for (int x = 0; x < IMG_WIDTH; x++)
          {
            int ym = MAX (y - 1, 0);
            int yp = MIN (y + 1, IMG_HEIGHT - 1);
            out[y * IMG_WIDTH + x] = (guint8)
                                     ((tmp[ym * IMG_WIDTH + x] +
                                       2 * (guint16) tmp[y  * IMG_WIDTH + x] +
                                       tmp[yp * IMG_WIDTH + x] + 2) / 4);
          }
      }
  }

  return img;
}

static gint
image_quality (FpImage *img)
{
  /*
   * Estimate quality as the standard deviation of pixel values.
   * No finger: all zeros, std = 0.
   * Good print: varied values, std > 0.
   *
   * Transitional first-touch frames have blank rows where the sensor
   * had no capacitance (raw value = 0), which scale to 0 in 8-bit and
   * create a large uniform region that inflates the std-dev without
   * being useful fingerprint data.  These frames perform poorly with
   * NBIS because the blank rows produce low-contrast map blocks at the
   * top and fragment the valid direction region.
   *
   * Stable fully-covered frames (no blank rows) have far fewer zero
   * pixels (only the very darkest valley pixels hit 0).  We penalise
   * frames with more than 200 zero pixels so that stable frames win
   * the quality comparison even though their raw std-dev is lower.
   */
  guint8 *data = img->data;
  gint npix = IMG_WIDTH * IMG_HEIGHT;
  gint64 sum = 0, sum_sq = 0;
  gint zero_count = 0;

  for (int i = 0; i < npix; i++)
    {
      sum += data[i];
      sum_sq += (gint64) data[i] * data[i];
      if (data[i] == 0)
        zero_count++;
    }

  gint64 mean = sum / npix;
  gint64 variance = sum_sq / npix - mean * mean;

  if (variance < 0)
    variance = 0;

  gint std = 0;
  while (std * std < variance)
    std++;

  /* Penalise frames with blank rows (transitional first-touch frames). */
  if (zero_count > 200)
    std = std / 2;

  fp_dbg ("image quality: std=%d zero_count=%d", std, zero_count);
  return std;
}

/* -- State machine -- */

static void init_run_state (FpiSsm   *ssm,
                            FpDevice *dev);
static void init_done (FpiSsm   *ssm,
                       FpDevice *dev,
                       GError   *error);
static void capture_done (FpiSsm   *ssm,
                          FpDevice *dev,
                          GError   *error);
static void reinit_done (FpiSsm   *ssm,
                         FpDevice *dev,
                         GError   *error);

static void start_capture_ssm (FpImageDevice *idev);

/* -- Init SSM -- */


static void
release_resources (FpiDeviceElan0c7f *self)
{
  g_clear_pointer (&self->frame_buf, g_free);
  g_clear_object (&self->best_img);
  g_clear_object (&self->cancellable);
}

static void
init_run_state (FpiSsm *ssm, FpDevice *dev)
{
  switch (fpi_ssm_get_cur_state (ssm))
    {
    /* Flush stale data from EP83 */
    case INIT_FLUSH_1:
    case INIT_FLUSH_2:
    case INIT_FLUSH_3:
      {
        FpiUsbTransfer *transfer = fpi_usb_transfer_new (dev);
        fpi_usb_transfer_fill_bulk (transfer, EP_STATUS, 64);
        transfer->short_is_error = FALSE;
        transfer->ssm = ssm;
        fpi_usb_transfer_submit (transfer, 100, NULL,
                                 tolerant_read_cb, NULL);
      }
      break;

    /* Send 9a10, read response (discard) */
    case INIT_9A10_1_SEND:
    case INIT_9A10_2_SEND:
    case INIT_9A10_3_SEND:
      send_cmd (ssm, dev, CMD_9A10, sizeof (CMD_9A10));
      break;

    case INIT_9A10_1_READ:
    case INIT_9A10_2_READ:
    case INIT_9A10_3_READ:
      read_status_discard (ssm, dev);
      break;

    /* Send 000b */
    case INIT_000B_SEND:
      send_cmd (ssm, dev, CMD_000B, sizeof (CMD_000B));
      break;

    case INIT_DONE:
      fpi_ssm_mark_completed (ssm);
      break;

    default:
      fp_err ("Unknown init state %d", fpi_ssm_get_cur_state (ssm));
      fpi_ssm_mark_failed (ssm,
                           fpi_device_error_new (FP_DEVICE_ERROR_GENERAL));
      break;
    }
}

static void
init_done (FpiSsm *ssm, FpDevice *dev, GError *error)
{
  FpiDeviceElan0c7f *self = FPI_DEVICE_ELAN0C7F (dev);
  FpImageDevice *idev = FP_IMAGE_DEVICE (dev);

  self->ssm_active = FALSE;

  if (error)
    {
      /* libfprint does not call deactivate after a failed activation. */
      release_resources (self);
      fpi_image_device_activate_complete (idev, error);
      return;
    }

  fpi_image_device_activate_complete (idev, NULL);
}

/* Called after re-arm init between enrol stages -- start capture directly */
static void
reinit_done (FpiSsm *ssm, FpDevice *dev, GError *error)
{
  FpiDeviceElan0c7f *self = FPI_DEVICE_ELAN0C7F (dev);
  FpImageDevice *idev = FP_IMAGE_DEVICE (dev);

  self->ssm_active = FALSE;

  if (self->deactivating)
    {
      /* The SSM hands us ownership of error; it is moot once deactivating. */
      g_clear_error (&error);
      release_resources (self);
      fpi_image_device_deactivate_complete (idev, NULL);
      return;
    }

  if (error)
    {
      fpi_image_device_session_error (idev, error);
      return;
    }

  start_capture_ssm (idev);
}

/* -- Capture SSM -- */

static void
finger_wait_cb (FpiUsbTransfer *transfer, FpDevice *dev,
                gpointer user_data, GError *error)
{
  FpiSsm *ssm  = user_data;
  FpiDeviceElan0c7f *self = FPI_DEVICE_ELAN0C7F (dev);

  /* dev_deactivate cancels this read; that is the normal way out. */
  if (self->deactivating)
    {
      g_clear_error (&error);
      fpi_ssm_mark_completed (ssm);
      return;
    }

  if (error)
    {
      fpi_ssm_mark_failed (ssm, error);
      return;
    }

  if (transfer->actual_length >= 1 && transfer->buffer[0] == RESP_FINGER)
    {
      fp_dbg ("sensor reports finger present");
      fpi_ssm_next_state (ssm);
      return;
    }

  if (transfer->actual_length >= 1 && transfer->buffer[0] == RESP_BUSY)
    fp_dbg ("sensor busy (0xca), re-arming");
  else
    fp_warn ("unexpected finger-wait response (%d bytes, first 0x%02x), re-arming",
             (int) transfer->actual_length,
             transfer->actual_length ? transfer->buffer[0] : 0);

  fpi_ssm_jump_to_state_delayed (ssm, CAPTURE_ARM_4031, BUSY_RETRY_MS);
}

static void
frame_read_cb (FpiUsbTransfer *transfer, FpDevice *dev,
               gpointer user_data, GError *error)
{
  FpiSsm *ssm  = user_data;
  FpiDeviceElan0c7f *self  = FPI_DEVICE_ELAN0C7F (dev);

  if (error)
    {
      fpi_ssm_mark_failed (ssm, error);
      return;
    }

  /* dev_deactivate may have been called while this transfer was in flight.
   * frame_buf is still alive (freed only after the SSM exits), but we
   * should not submit any more work. */
  if (self->deactivating)
    {
      fpi_ssm_mark_completed (ssm);
      return;
    }

  if (transfer->actual_length < FRAME_SIZE)
    {
      fp_warn ("Short frame: %d bytes (expected %d)",
               (int) transfer->actual_length, FRAME_SIZE);
      /* Try again */
      fpi_ssm_jump_to_state (ssm, CAPTURE_SEND_FRAME_CMD);
      return;
    }

  /* Copy frame data */
  memcpy (self->frame_buf, transfer->buffer, FRAME_SIZE);
  fpi_ssm_next_state (ssm);
}

static void
lift_frame_read_cb (FpiUsbTransfer *transfer, FpDevice *dev,
                    gpointer user_data, GError *error)
{
  FpiSsm *ssm  = user_data;
  FpiDeviceElan0c7f *self = FPI_DEVICE_ELAN0C7F (dev);
  FpImageDevice *idev = FP_IMAGE_DEVICE (dev);

  /* dev_deactivate may have been called while this transfer was in flight.
   * frame_buf and best_img are still alive (freed only after the SSM exits),
   * but skip all image submission work and let capture_done handle teardown. */
  if (self->deactivating)
    {
      g_clear_error (&error);
      fpi_ssm_mark_completed (ssm);
      return;
    }

  if (error)
    {
      g_clear_error (&error);
      /* On error assume finger lifted */
    }
  else if (transfer->actual_length >= FRAME_SIZE)
    {
      memcpy (self->frame_buf, transfer->buffer, FRAME_SIZE);
      FpImage *img = frame_to_image (self->frame_buf);
      gint quality = image_quality (img);
      g_object_unref (img);

      fp_dbg ("lift check quality: %d", quality);

      if (quality >= QUALITY_MIN)
        {
          /* Finger still present -- keep waiting */
          fpi_ssm_jump_to_state (ssm, CAPTURE_AWAIT_LIFT);
          return;
        }
    }

  /* Quality dropped -- finger has been lifted. Submit image. */
  self->finger_on = FALSE;
  self->best_img->ppmm = SENSOR_PPMM;
  /* ELAN capacitive sensor: ridges produce higher raw values (lighter pixels
   * after scaling).  NBIS expects dark ridges, so invert colors here. */
  self->best_img->flags = FPI_IMAGE_COLORS_INVERTED;

  fpi_image_device_image_captured (idev, self->best_img);
  /* Null our pointer before report_finger_status, which may synchronously
   * call dev_deactivate → g_clear_object(&self->best_img).  If best_img is
   * still set at that point, dev_deactivate would consume our initial ref,
   * leaving the image under-retained and causing a use-after-free when
   * fp_print_finalize later tries to unref print->image. */
  self->best_img = NULL;
  self->best_quality = 0;
  self->frame_count = 0;
  fpi_image_device_report_finger_status (idev, FALSE);
  fpi_ssm_mark_completed (ssm);
}

static void
capture_run_state (FpiSsm *ssm, FpDevice *dev)
{
  FpiDeviceElan0c7f *self = FPI_DEVICE_ELAN0C7F (dev);
  FpImageDevice *idev = FP_IMAGE_DEVICE (dev);

  if (self->deactivating)
    {
      fpi_ssm_mark_completed (ssm);
      return;
    }

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case CAPTURE_ARM_4031:
      send_cmd (ssm, dev, CMD_4031, sizeof (CMD_4031));
      break;

    case CAPTURE_ARM_403F:
      send_cmd (ssm, dev, CMD_403F, sizeof (CMD_403F));
      break;

    case CAPTURE_WAIT_FINGER:
      {
        /* No timeout: the sensor answers only when touched, and
         * dev_deactivate cancels the read. */
        FpiUsbTransfer *transfer = fpi_usb_transfer_new (dev);
        fpi_usb_transfer_fill_bulk (transfer, EP_STATUS, 64);
        transfer->short_is_error = FALSE;
        self->low_quality_count = 0;
        fpi_usb_transfer_submit (transfer, 0, self->cancellable,
                                 finger_wait_cb, ssm);
        return;
      }

    case CAPTURE_SEND_FRAME_CMD:
      send_cmd (ssm, dev, CMD_FRAME, sizeof (CMD_FRAME));
      break;

    case CAPTURE_READ_FRAME:
      {
        FpiUsbTransfer *transfer = fpi_usb_transfer_new (dev);
        fpi_usb_transfer_fill_bulk (transfer, EP_IN, FRAME_SIZE);
        transfer->short_is_error = FALSE;
        fpi_usb_transfer_submit (transfer, TIMEOUT_MS, NULL,
                                 frame_read_cb, ssm);
        return;  /* Callback handles state transition */
      }

    case CAPTURE_CHECK_QUALITY:
      {
        FpImage *img = frame_to_image (self->frame_buf);
        gint quality = image_quality (img);

        if (quality >= QUALITY_MIN)
          {
            self->frame_count++;
            self->low_quality_count = 0;

            if (!self->finger_on)
              {
                self->finger_on = TRUE;
                fpi_image_device_report_finger_status (idev, TRUE);
              }

            if (quality > self->best_quality)
              {
                g_clear_object (&self->best_img);
                self->best_img = img;
                self->best_quality = quality;
              }
            else
              {
                g_object_unref (img);
              }

            /* Collected enough good frames -- move to lift-wait.
             * Do NOT report finger-off yet; wait for the real lift. */
            if (self->frame_count >= 10)
              fpi_ssm_jump_to_state (ssm, CAPTURE_AWAIT_LIFT);
            else
              fpi_ssm_jump_to_state (ssm, CAPTURE_SEND_FRAME_CMD);
          }
        else
          {
            g_object_unref (img);
            self->low_quality_count++;
            /* After finger_on, two consecutive low-quality frames mean the
             * finger has been lifted — submit the best image we have now
             * rather than waiting for 10 frames (which may never come or
             * may come from a subsequent, different press).
             * Require at least 3 good frames first: a single-frame press
             * rarely has enough ridge area for NBIS to extract ≥ 5 minutiae,
             * causing the enrollment stage or verify to fail and fprintd to
             * report a retry.  3 frames at ~5 fps is only ~0.6 s. */
            if (self->finger_on && self->best_img != NULL &&
                self->low_quality_count >= 2 && self->frame_count >= 3)
              fpi_ssm_jump_to_state (ssm, CAPTURE_AWAIT_LIFT);
            /* No usable finger for a while (a false trigger, or a tap too
             * short to use): stop streaming frames and wait on the sensor's
             * finger detect again.  Any good frames so far are kept. */
            else if (self->low_quality_count >= REARM_AFTER_LOW_FRAMES)
              fpi_ssm_jump_to_state (ssm, CAPTURE_ARM_4031);
            else
              fpi_ssm_jump_to_state (ssm, CAPTURE_SEND_FRAME_CMD);
          }
        break;
      }

    case CAPTURE_AWAIT_LIFT:
      {
        /* Send a frame command and read it to check if finger is still present */
        FpiUsbTransfer *transfer = fpi_usb_transfer_new (dev);
        fpi_usb_transfer_fill_bulk_full (transfer, EP_OUT,
                                         (guint8 *) CMD_FRAME,
                                         sizeof (CMD_FRAME), NULL);
        transfer->short_is_error = TRUE;
        transfer->ssm = ssm;
        fpi_usb_transfer_submit (transfer, TIMEOUT_MS, NULL,
                                 fpi_ssm_usb_transfer_cb, NULL);
        break;
      }

    case CAPTURE_READ_LIFT_FRAME:
      {
        FpiUsbTransfer *transfer = fpi_usb_transfer_new (dev);
        fpi_usb_transfer_fill_bulk (transfer, EP_IN, FRAME_SIZE);
        transfer->short_is_error = FALSE;
        fpi_usb_transfer_submit (transfer, TIMEOUT_MS, NULL,
                                 lift_frame_read_cb, ssm);
        return;
      }

    default:
      fp_err ("Unknown capture state %d", fpi_ssm_get_cur_state (ssm));
      fpi_ssm_mark_failed (ssm,
                           fpi_device_error_new (FP_DEVICE_ERROR_GENERAL));
      break;
    }
}

static void
capture_done (FpiSsm *ssm, FpDevice *dev, GError *error)
{
  FpiDeviceElan0c7f *self = FPI_DEVICE_ELAN0C7F (dev);
  FpImageDevice *idev = FP_IMAGE_DEVICE (dev);

  self->ssm_active = FALSE;

  if (self->deactivating)
    {
      /* The SSM hands us ownership of error; it is moot once deactivating. */
      g_clear_error (&error);
      release_resources (self);
      fpi_image_device_deactivate_complete (idev, NULL);
      return;
    }

  if (error)
    {
      fpi_image_device_session_error (idev, error);
      return;
    }

  /* Capture cycle complete. libfprint will call
   * change_state(AWAIT_FINGER_ON) when it is ready for the next touch,
   * which will re-arm the sensor and start a new capture SSM. */
}

static void
start_capture_ssm (FpImageDevice *idev)
{
  FpiDeviceElan0c7f *self = FPI_DEVICE_ELAN0C7F (idev);
  FpiSsm *ssm;

  self->frame_count = 0;
  self->best_quality = 0;
  self->low_quality_count = 0;
  self->finger_on = FALSE;
  g_clear_object (&self->best_img);

  ssm = fpi_ssm_new (FP_DEVICE (idev), capture_run_state,
                     CAPTURE_NUM_STATES);
  self->ssm_active = TRUE;
  fpi_ssm_start (ssm, capture_done);
}

/* -- FpImageDevice vfuncs -- */

static void
dev_open (FpImageDevice *dev)
{
  GError *error = NULL;
  GUsbDevice *usb_dev = fpi_device_get_usb_device (FP_DEVICE (dev));

  if (!g_usb_device_claim_interface (usb_dev, 0, 0, &error))
    {
      fpi_image_device_open_complete (dev, error);
      return;
    }

  fpi_image_device_open_complete (dev, NULL);
}

static void
dev_close (FpImageDevice *dev)
{
  GError *error = NULL;
  GUsbDevice *usb_dev = fpi_device_get_usb_device (FP_DEVICE (dev));

  g_usb_device_release_interface (usb_dev, 0, 0, &error);
  g_clear_error (&error);
  fpi_image_device_close_complete (dev, NULL);
}

static void
dev_activate (FpImageDevice *dev)
{
  FpiDeviceElan0c7f *self = FPI_DEVICE_ELAN0C7F (dev);
  FpiSsm *ssm;

  self->deactivating = FALSE;
  self->ssm_active = TRUE;
  self->finger_on = FALSE;
  self->frame_count = 0;
  self->best_quality = 0;
  self->frame_buf = g_malloc0 (FRAME_SIZE);
  self->cancellable = g_cancellable_new ();

  ssm = fpi_ssm_new (FP_DEVICE (dev), init_run_state, INIT_NUM_STATES);
  fpi_ssm_start (ssm, init_done);
}

static void
dev_deactivate (FpImageDevice *dev)
{
  FpiDeviceElan0c7f *self = FPI_DEVICE_ELAN0C7F (dev);

  self->deactivating = TRUE;

  /* Only free resources and complete immediately when no SSM is in flight.
   * If an SSM is running, its pending USB callbacks still hold pointers into
   * frame_buf and best_img — freeing them here causes a use-after-free when
   * those callbacks fire.  Instead, defer cleanup to capture_done /
   * reinit_done which run only after all USB transfers have completed.
   * Cancelling ends a finger wait that would otherwise never complete. */
  if (self->ssm_active)
    {
      g_cancellable_cancel (self->cancellable);
    }
  else
    {
      release_resources (self);
      fpi_image_device_deactivate_complete (dev, NULL);
    }
}

static void
dev_change_state (FpImageDevice *dev, FpiImageDeviceState state)
{
  switch (state)
    {
    case FPI_IMAGE_DEVICE_STATE_AWAIT_FINGER_ON:
      /* Re-arm the sensor and start capture. Called by libfprint after
       * it has finished processing a submitted image. */
      {
        FpiSsm *init_ssm = fpi_ssm_new (FP_DEVICE (dev), init_run_state,
                                        INIT_NUM_STATES);
        FPI_DEVICE_ELAN0C7F (dev)->ssm_active = TRUE;
        fpi_ssm_start (init_ssm, reinit_done);
      }
      break;

    case FPI_IMAGE_DEVICE_STATE_CAPTURE:
      /* Already capturing -- nothing to do */
      break;

    case FPI_IMAGE_DEVICE_STATE_AWAIT_FINGER_OFF:
      /* lift_frame_read_cb will call report_finger_status(FALSE)
      * once it detects the actual lift -- nothing to do here. */
      break;

    case FPI_IMAGE_DEVICE_STATE_INACTIVE:
    case FPI_IMAGE_DEVICE_STATE_ACTIVATING:
    case FPI_IMAGE_DEVICE_STATE_DEACTIVATING:
    case FPI_IMAGE_DEVICE_STATE_IDLE:
      break;
    }
}

/* -- Matching --
 *
 * NBIS/Bozorth3 cannot tell fingers apart on this sensor: each 52 px wide
 * image yields only 3-13 minutiae, and measured on 70 captures a wrong
 * finger was accepted 56% of the time at the old settings.  Instead each
 * scan is described by SIFT keypoints and scored by how many keypoint
 * matches agree on one rotation + translation (elan0c7f-sift.cpp), after
 * dividing out the power-key icon that appears in every scan
 * (elan0c7f-gain.h).
 *
 * Prints are FPI_PRINT_RAW with fpi-data PRINT_FORMAT: a format version and
 * one (positions, descriptors) sample per enrollment scan.  A probe holds a
 * single sample, and matches a template when any sample scores at least
 * MATCH_THRESHOLD.  Version 1 prints were made without the icon correction
 * and are not comparable, so they must be re-enrolled. */

#define PRINT_FORMAT "(ua(aday))"
#define PRINT_FORMAT_VERSION 2
#define MATCH_THRESHOLD 8
#define MIN_KEYPOINTS 20

static GVariant *
sample_new (const Elan0c7fFeatures *f)
{
  return g_variant_new ("(@ad@ay)",
                        g_variant_new_fixed_array (G_VARIANT_TYPE_DOUBLE,
                                                   f->xy, 2 * f->n,
                                                   sizeof (gdouble)),
                        g_variant_new_fixed_array (G_VARIANT_TYPE_BYTE,
                                                   f->desc,
                                                   f->n * ELAN0C7F_SIFT_DESC_LEN,
                                                   sizeof (guint8)));
}

static gboolean
sample_parse (GVariant *sample, Elan0c7fFeatures *f)
{
  g_autoptr(GVariant) xy_v = g_variant_get_child_value (sample, 0);
  g_autoptr(GVariant) desc_v = g_variant_get_child_value (sample, 1);
  const gdouble *xy;
  const guint8 *desc;
  gsize n_xy, n_desc;

  xy = g_variant_get_fixed_array (xy_v, &n_xy, sizeof (gdouble));
  desc = g_variant_get_fixed_array (desc_v, &n_desc, sizeof (guint8));
  if (n_xy % 2 != 0 || n_desc != n_xy / 2 * ELAN0C7F_SIFT_DESC_LEN)
    return FALSE;

  f->n = n_xy / 2;
  f->xy = g_memdup2 (xy, n_xy * sizeof (gdouble));
  f->desc = g_memdup2 (desc, n_desc);
  return TRUE;
}

/* The print's samples array, or NULL if it is not in this driver's format
 * (e.g. an NBIS print enrolled by an older version of the driver). */
static GVariant *
print_samples (FpPrint *print)
{
  g_autoptr(GVariant) data = NULL;
  GVariant *samples;
  guint32 version;

  if (fpi_print_get_type (print) != FPI_PRINT_RAW)
    return NULL;

  g_object_get (print, "fpi-data", &data, NULL);
  if (!data || !g_variant_is_of_type (data, G_VARIANT_TYPE (PRINT_FORMAT)))
    return NULL;

  g_variant_get (data, "(u@a(aday))", &version, &samples);
  if (version != PRINT_FORMAT_VERSION)
    {
      g_variant_unref (samples);
      return NULL;
    }
  return samples;
}

static void
set_samples (FpPrint *print, GVariantBuilder *samples)
{
  g_object_set (print, "fpi-data",
                g_variant_new ("(u@a(aday))", PRINT_FORMAT_VERSION,
                               g_variant_builder_end (samples)),
                NULL);
}

/* Debugging aid, off unless ELAN0C7F_DUMP_DIR is set: save each scan that
 * is matched or enrolled, as an 8-bit PGM with dark ridges (the format of
 * the capture tool in the elan0c7f-libfprint-driver repository), so live
 * sessions can be replayed offline.  These are fingerprint images: point
 * it at a private directory and delete them after use. */
static void
dump_scan (FpDevice *dev, const guint8 *pixels, gint width, gint height)
{
  const char *dir = g_getenv ("ELAN0C7F_DUMP_DIR");
  FpiDeviceAction current = fpi_device_get_current_action (dev);
  const char *action = "other";
  g_autofree char *name = NULL;
  g_autofree char *path = NULL;

  g_autoptr(GString) pgm = NULL;
  g_autoptr(GError) error = NULL;

  if (!dir || !*dir)
    return;

  if (current == FPI_DEVICE_ACTION_ENROLL)
    action = "enroll";
  else if (current == FPI_DEVICE_ACTION_IDENTIFY)
    action = "identify";

  name = g_strdup_printf ("%" G_GINT64_FORMAT "-%s.pgm",
                          g_get_real_time (), action);
  path = g_build_filename (dir, name, NULL);
  pgm = g_string_new (NULL);
  g_string_printf (pgm, "P5\n%d %d\n255\n", width, height);
  g_string_append_len (pgm, (const char *) pixels, (gssize) width * height);
  if (!g_file_set_contents_full (path, pgm->str, pgm->len,
                                 G_FILE_SET_CONTENTS_CONSISTENT, 0600, &error))
    g_warning ("Could not save scan to %s: %s", path, error->message);
  else
    fp_dbg ("saved scan to %s", path);
}

static gboolean
dev_print_from_image (FpImageDevice *dev, FpImage *image, FpPrint *print,
                      GError **error)
{
  gsize npix = (gsize) image->width * image->height;
  g_autofree guint8 *pixels = g_memdup2 (image->data, npix);
  Elan0c7fFeatures f = { 0 };
  GVariantBuilder samples;

  /* Minutiae detection normally leaves dark ridges with the flag cleared,
   * but not when it failed, so normalise here. */
  if (image->flags & FPI_IMAGE_COLORS_INVERTED)
    for (gsize i = 0; i < npix; i++)
      pixels[i] = 255 - pixels[i];

  dump_scan (FP_DEVICE (dev), pixels, image->width, image->height);

  elan0c7f_sift_extract (pixels, image->width, image->height, &f);
  fp_dbg ("sift: %u keypoints", f.n);
  if (f.n < MIN_KEYPOINTS)
    {
      elan0c7f_features_clear (&f);
      g_set_error_literal (error, FP_DEVICE_RETRY, FP_DEVICE_RETRY_CENTER_FINGER,
                           "Too little fingerprint detail, center your finger on the sensor");
      return FALSE;
    }

  g_variant_builder_init (&samples, G_VARIANT_TYPE ("a(aday)"));
  g_variant_builder_add_value (&samples, sample_new (&f));
  elan0c7f_features_clear (&f);

  fpi_print_set_type (print, FPI_PRINT_RAW);
  set_samples (print, &samples);
  return TRUE;
}

static void
dev_print_merge (FpImageDevice *dev, FpPrint *enroll_print, FpPrint *print)
{
  g_autoptr(GVariant) old = print_samples (enroll_print);
  g_autoptr(GVariant) add = print_samples (print);
  GVariantBuilder samples;
  GVariantIter iter;
  GVariant *sample;

  g_return_if_fail (add != NULL);

  g_variant_builder_init (&samples, G_VARIANT_TYPE ("a(aday)"));
  if (old)
    {
      g_variant_iter_init (&iter, old);
      while ((sample = g_variant_iter_next_value (&iter)))
        {
          g_variant_builder_add_value (&samples, sample);
          g_variant_unref (sample);
        }
    }
  g_variant_iter_init (&iter, add);
  while ((sample = g_variant_iter_next_value (&iter)))
    {
      g_variant_builder_add_value (&samples, sample);
      g_variant_unref (sample);
    }
  set_samples (enroll_print, &samples);
}

static FpiMatchResult
dev_print_match (FpImageDevice *dev, FpPrint *print_template, FpPrint *print,
                 GError **error)
{
  g_autoptr(GVariant) templates = print_samples (print_template);
  g_autoptr(GVariant) probes = print_samples (print);
  g_autoptr(GVariant) probe_v = NULL;
  Elan0c7fFeatures probe = { 0 };
  FpiMatchResult result = FPI_MATCH_FAIL;
  gsize n;

  if (!probes || g_variant_n_children (probes) != 1)
    {
      g_set_error_literal (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_GENERAL,
                           "Invalid probe print");
      return FPI_MATCH_ERROR;
    }

  /* Not an error: that would also abort fprintd's duplicate check, and so
   * block re-enrolling while an old print is still stored. */
  if (!templates)
    {
      g_warning ("Stored fingerprint was enrolled with an older version of "
                 "the elan0c7f driver and cannot match; delete and re-enroll it");
      return FPI_MATCH_FAIL;
    }

  probe_v = g_variant_get_child_value (probes, 0);
  if (!sample_parse (probe_v, &probe))
    {
      g_set_error_literal (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_GENERAL,
                           "Invalid probe print");
      return FPI_MATCH_ERROR;
    }

  n = g_variant_n_children (templates);
  for (gsize i = 0; i < n && result != FPI_MATCH_SUCCESS; i++)
    {
      g_autoptr(GVariant) sample_v = g_variant_get_child_value (templates, i);
      Elan0c7fFeatures sample = { 0 };
      gint score;

      if (!sample_parse (sample_v, &sample))
        {
          g_warning ("Skipping corrupt sample %" G_GSIZE_FORMAT " of stored fingerprint", i);
          continue;
        }

      score = elan0c7f_sift_score (&probe, &sample);
      fp_dbg ("sift score %d/%d (sample %" G_GSIZE_FORMAT "/%" G_GSIZE_FORMAT ")",
              score, MATCH_THRESHOLD, i + 1, n);
      if (score >= MATCH_THRESHOLD)
        result = FPI_MATCH_SUCCESS;
      elan0c7f_features_clear (&sample);
    }

  elan0c7f_features_clear (&probe);
  return result;
}

/* -- Driver registration -- */

static const FpIdEntry id_table[] = {
  { .vid = 0x04f3, .pid = 0x0c7f },
  { .vid = 0,      .pid = 0      }
};

static void
fpi_device_elan0c7f_init (FpiDeviceElan0c7f *self)
{
}

static void
fpi_device_elan0c7f_class_init (FpiDeviceElan0c7fClass *klass)
{
  FpDeviceClass *dev_class = FP_DEVICE_CLASS (klass);
  FpImageDeviceClass *img_class = FP_IMAGE_DEVICE_CLASS (klass);

  dev_class->id        = "elan0c7f";
  dev_class->full_name = "ELAN 04f3:0c7f Fingerprint Sensor";
  dev_class->type      = FP_DEVICE_TYPE_USB;
  dev_class->id_table  = id_table;
  dev_class->scan_type = FP_SCAN_TYPE_PRESS;

  /* Each scan covers only part of the finger, so more samples mean a
   * later scan is more likely to overlap one of them.  In the offline
   * measurements, 12 rather than 8 cut genuine rejections from ~22% to
   * ~18% at MATCH_THRESHOLD; 16 gained little more. */
  dev_class->nr_enroll_stages = 12;

  /* IDENTIFY must stay enabled: since 1.94.100 image devices implement
   * verify via identify with a single-print gallery, so masking it out
   * makes every verify fail with FP_DEVICE_ERROR_NOT_SUPPORTED.  (On
   * 1.94.10 it was masked to avoid an fprintd crash on identify with an
   * empty gallery during first enrollment.) */

  img_class->img_width    = IMG_WIDTH;
  img_class->img_height   = IMG_HEIGHT;
  img_class->img_open     = dev_open;
  img_class->img_close    = dev_close;
  img_class->activate     = dev_activate;
  img_class->deactivate   = dev_deactivate;
  img_class->change_state = dev_change_state;

  img_class->print_from_image = dev_print_from_image;
  img_class->print_merge      = dev_print_merge;
  img_class->print_match      = dev_print_match;
}
