/*
 * Copyright (c) 2018-2020, NVIDIA CORPORATION. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include <gst/gst.h>
#include <glib.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include <algorithm>
#include <iostream>

#include <boost/bind.hpp>
#include <boost/chrono.hpp>

#include "gstnvdsmeta.h"

#define PGIE_CONFIG_FILE  "dstest2_pgie_config.txt"
#define SGIE_CONFIG_FILE  "dstest2_sgie_config.txt"

#define MAX_DISPLAY_LEN 64

#define TRACKER_CONFIG_FILE "dstest2_tracker_config.txt"
#define MAX_TRACKING_ID_LEN 16

#define PGIE_CLASS_ID_VEHICLE 0
#define PGIE_CLASS_ID_PERSON 0
#define SGIE_CLASS_ID_WHEELCHAIR 0

/* The muxer output resolution must be set if the input streams will be of
 * different resolution. The muxer will scale all the input frames to this
 * resolution. */
#define MUXER_OUTPUT_WIDTH 1920
#define MUXER_OUTPUT_HEIGHT 1080

/* Muxer batch formation timeout, for e.g. 40 millisec. Should ideally be set
 * based on the fastest source's framerate. */
#define MUXER_BATCH_TIMEOUT_USEC 40000

#define GST_CAPS_FEATURES_NVMM "memory:NVMM"

struct Wheelie{
  int x, y, w, h;
  bool mapped;
  bool processed_status;
  bool reset_cal;
  int tracker_id;
  int mapped_tracker_id;
  int attendee_counter;
  int wheelchair_bbox_count;
  std::string status;
  std::chrono::system_clock::time_point timer;
  std::chrono::system_clock::time_point delete_timer;
};

struct Attendee{
  int x, y, w, h;
  int tracker_id;
};

using namespace std;

Wheelie wl;
Attendee a;

std::vector<Wheelie> wheelchair_tracker;
std::vector<Attendee> attendee_tracker;

gint frame_number = 0;


// Stack Overflow post,
// https://stackoverflow.com/questions/306316/determine-if-two-rectangles-overlap-each-other

bool
valueInRange(int value, int min, int max)
{ return (value >= min) && (value <= max); }

static void
map_wheelchair_person() {
  int w_x, w_y, w_w, w_h;
  int p_x, p_y, p_w, p_h;
  for (auto w_it = wheelchair_tracker.begin(); w_it != wheelchair_tracker.end(); ++w_it) {
    w_x = (*w_it).x;
    w_y = (*w_it).y;
    w_w = (*w_it).w;
    w_h = (*w_it).h;
    int mapped_counter = 0;
    for (auto p_it = attendee_tracker.begin(); p_it != attendee_tracker.end(); ++p_it) {
      p_x = (*p_it).x;
      p_y = (*p_it).y;
      p_w = (*p_it).w;
      p_h = (*p_it).h;

      bool xOverlap = valueInRange(w_x, p_x, p_x + p_w) ||
                      valueInRange(p_x, w_x, w_x + w_w);

      bool yOverlap = valueInRange(w_y, p_y, p_y + p_h) ||
                      valueInRange(p_y, w_y, w_y + w_h);


      // This block maps wheelchair bbox with person bbox and checks for proximity (i.e 200 px)
      // if any person is within the range a counter is incremented.

      if (xOverlap && yOverlap) {
        if (abs((p_y + p_h) - (w_y + w_h)) < 200) {
          mapped_counter++;
        }
      }
    }

    // Here mapped counter is checked for >= 2 due to a bbox from person sitting in wheelchair
    // along with any other person close to the wheelchair bbox
    if (mapped_counter >= 2) {
      (*w_it).mapped = true;
      (*w_it).attendee_counter++;
    }
  }
}

static void
validate_wheelchair_attended() {
  for (auto w_it = wheelchair_tracker.begin(); w_it != wheelchair_tracker.end(); ++w_it) {
    auto diff = std::chrono::duration_cast<std::chrono::milliseconds>
    (std::chrono::system_clock::now() - (*w_it).timer).count();

    if(diff > 2000) {
      // calculations are aggregated every 2 seconds and results are computed and a color change is notified as a visual queue
      (*w_it).processed_status = true;
      (*w_it).reset_cal = true;
      if ((*w_it).mapped) {
        if ((((*w_it).attendee_counter/(*w_it).wheelchair_bbox_count) < 0.69) && ((*w_it).wheelchair_bbox_count > 10)) {
          (*w_it).status = "Unattended";
        }
        else {
          (*w_it).status = "Attended";
        }
      }
      else {
        (*w_it).status = "Unattended";
      }
      (*w_it).timer = std::chrono::system_clock::now();
    }

    diff = std::chrono::duration_cast<std::chrono::milliseconds>
    (std::chrono::system_clock::now() - (*w_it).delete_timer).count();

    if (diff > 20000) {
      wheelchair_tracker.erase(w_it);
      --w_it;
    }
  }
}

void
set_object_color(NvDsFrameMeta* frame_meta) {
  for (NvDsMetaList * l_obj = frame_meta->obj_meta_list; l_obj != NULL;
      l_obj = l_obj->next) {

      NvDsObjectMeta *obj_meta = (NvDsObjectMeta *) l_obj->data;

      if (obj_meta == NULL) {
        // Ignore Null object.
        continue;
      }

      int class_index = obj_meta->class_id;
      int cur_obj_id = obj_meta->object_id;

      for (auto iter = wheelchair_tracker.begin(); iter != wheelchair_tracker.end(); ++iter) {
        if ((*iter).tracker_id == cur_obj_id) {
          if ((*iter).processed_status) {
            if ((*iter).status.compare("Unattended") == 0) {
              #ifndef PLATFORM_TEGRA
                obj_meta->rect_params.has_bg_color = 1;
                obj_meta->rect_params.bg_color.red = 1;
                obj_meta->rect_params.bg_color.green = 0;
                obj_meta->rect_params.bg_color.blue = 0;
                obj_meta->rect_params.bg_color.alpha = 0.2;
              #endif
              obj_meta->rect_params.border_width = 8;
              obj_meta->rect_params.border_color.red = 1;
              obj_meta->rect_params.border_color.green = 0;
              obj_meta->rect_params.border_color.blue = 0;
              obj_meta->rect_params.border_color.alpha = 0.2;
              obj_meta->text_params.font_params.font_size = 14;
            }
          }
        }
      }
    }
}

/* This is the buffer probe function that we have registered on the sink pad
 * of the OSD element. All the infer elements in the pipeline shall attach
 * their metadata to the GstBuffer, here we will iterate & process the metadata
 * forex: class ids to strings, counting of class_id objects etc. */
static GstPadProbeReturn
osd_sink_pad_buffer_probe (GstPad * pad, GstPadProbeInfo * info,
    gpointer u_data)
{
    GstBuffer *buf = (GstBuffer *) info->data;
    guint num_rects = 0;
    NvDsObjectMeta *obj_meta = NULL;
    guint vehicle_count = 0;
    guint person_count = 0;
    NvDsMetaList * l_frame = NULL;
    NvDsMetaList * l_obj = NULL;
    NvDsDisplayMeta *display_meta = NULL;

    NvDsBatchMeta *batch_meta = gst_buffer_get_nvds_batch_meta (buf);

    for (l_frame = batch_meta->frame_meta_list; l_frame != NULL;
      l_frame = l_frame->next) {
        NvDsFrameMeta *frame_meta = (NvDsFrameMeta *) (l_frame->data);
        int offset = 0;
        std::vector<int> wheelchair_ids;
        for (l_obj = frame_meta->obj_meta_list; l_obj != NULL;
                l_obj = l_obj->next) {
            obj_meta = (NvDsObjectMeta *) (l_obj->data);

            int x, y, wt, ht, cur_obj_id;

            x = obj_meta->rect_params.left;
            y = obj_meta->rect_params.top;
            wt = obj_meta->rect_params.width;
            ht = obj_meta->rect_params.height;

            cur_obj_id = obj_meta->object_id;

            if ((obj_meta->unique_component_id == 2) && (obj_meta->class_id == SGIE_CLASS_ID_WHEELCHAIR)) {
                vehicle_count++;

                #ifndef PLATFORM_TEGRA
                  obj_meta->rect_params.has_bg_color = 1;
                  obj_meta->rect_params.bg_color.red = 0;
                  obj_meta->rect_params.bg_color.green = 1;
                  obj_meta->rect_params.bg_color.blue = 0;
                  obj_meta->rect_params.bg_color.alpha = 0.2;
                #endif
                obj_meta->rect_params.border_width = 8;
                obj_meta->rect_params.border_color.red = 0;
                obj_meta->rect_params.border_color.green = 1;
                obj_meta->rect_params.border_color.blue = 0;
                obj_meta->rect_params.border_color.alpha = 0.2;
                obj_meta->text_params.font_params.font_size = 14;

                wl.tracker_id = cur_obj_id;
                wl.x = x;
                wl.y = y;
                wl.w = wt;
                wl.h = ht;

                auto iter = find_if(wheelchair_tracker.begin(), wheelchair_tracker.end(), boost::bind(&Wheelie::tracker_id, _1) == cur_obj_id);

                if (iter != wheelchair_tracker.end()) {
                  (*iter).x = x;
                  (*iter).y = y;
                  (*iter).w = wt;
                  (*iter).h = ht;
                  if ((*iter).reset_cal) {
                    (*iter).wheelchair_bbox_count = 0;
                    (*iter).attendee_counter = 0;
                    (*iter).reset_cal = false;
                  }
                  (*iter).wheelchair_bbox_count++;
                  (*iter).delete_timer = std::chrono::system_clock::now();
                }
                else {
                  wl.mapped = false;
                  wl.mapped_tracker_id = -1;
                  wl.wheelchair_bbox_count = 1;

                  wl.timer = std::chrono::system_clock::now();
                  wl.delete_timer = std::chrono::system_clock::now();
                  wheelchair_tracker.emplace_back(wl);
                  wheelchair_ids.emplace_back(cur_obj_id);
                }
            }
            if ((obj_meta->unique_component_id == 1) && obj_meta->class_id == PGIE_CLASS_ID_PERSON) {
                person_count++;

                a.tracker_id = cur_obj_id;
                a.x = x;
                a.y = y;
                a.w = wt;
                a.h = ht;
                attendee_tracker.emplace_back(a);
            }
        }

        map_wheelchair_person();

        validate_wheelchair_attended();

        attendee_tracker.clear();

        set_object_color(frame_meta);

        display_meta = nvds_acquire_display_meta_from_pool(batch_meta);
        NvOSD_TextParams *txt_params  = display_meta->text_params;
        display_meta->num_labels = 1;
        txt_params->display_text = (char*)g_malloc0 (MAX_DISPLAY_LEN);
        offset = snprintf(txt_params->display_text, MAX_DISPLAY_LEN, "Person = %d ", person_count);
        offset = snprintf(txt_params->display_text + offset , MAX_DISPLAY_LEN, "Wheelchair = %d ", vehicle_count);

        /* Now set the offsets where the string should appear */
        txt_params->x_offset = 10;
        txt_params->y_offset = 12;

        /* Font , font-color and font-size */
        txt_params[display_meta->num_labels].font_params.font_name = (char*)"Serif";
        txt_params[display_meta->num_labels].font_params.font_size = 20;
        txt_params[display_meta->num_labels].font_params.font_color.red = 0.0;
        txt_params[display_meta->num_labels].font_params.font_color.green = 0.0;
        txt_params[display_meta->num_labels].font_params.font_color.blue = 0.0;
        txt_params[display_meta->num_labels].font_params.font_color.alpha = 1.0;

        /* Text background color */
        txt_params[display_meta->num_labels].set_bg_clr = 1;
        txt_params[display_meta->num_labels].text_bg_clr.red = 1.0;
        txt_params[display_meta->num_labels].text_bg_clr.green = 1.0;
        txt_params[display_meta->num_labels].text_bg_clr.blue = 1.0;
        txt_params[display_meta->num_labels].text_bg_clr.alpha = 1.0;

        nvds_add_display_meta_to_frame(frame_meta, display_meta);
    }

    frame_number++;
    return GST_PAD_PROBE_OK;
}

static gboolean
bus_call (GstBus * bus, GstMessage * msg, gpointer data)
{
  GMainLoop *loop = (GMainLoop *) data;
  switch (GST_MESSAGE_TYPE (msg)) {
    case GST_MESSAGE_EOS:
      g_print ("End of stream\n");
      g_main_loop_quit (loop);
      break;
    case GST_MESSAGE_ERROR:{
      gchar *debug;
      GError *error;
      gst_message_parse_error (msg, &error, &debug);
      g_printerr ("ERROR from element %s: %s\n",
          GST_OBJECT_NAME (msg->src), error->message);
      if (debug)
        g_printerr ("Error details: %s\n", debug);
      g_free (debug);
      g_error_free (error);
      g_main_loop_quit (loop);
      break;
    }
    default:
      break;
  }
  return TRUE;
}

/* Tracker config parsing */

#define CHECK_ERROR(error) \
    if (error) { \
        g_printerr ("Error while parsing config file: %s\n", error->message); \
        goto done; \
    }

#define CONFIG_GROUP_TRACKER "tracker"
#define CONFIG_GROUP_TRACKER_WIDTH "tracker-width"
#define CONFIG_GROUP_TRACKER_HEIGHT "tracker-height"
#define CONFIG_GROUP_TRACKER_LL_CONFIG_FILE "ll-config-file"
#define CONFIG_GROUP_TRACKER_LL_LIB_FILE "ll-lib-file"
#define CONFIG_GROUP_TRACKER_ENABLE_BATCH_PROCESS "enable-batch-process"
#define CONFIG_GPU_ID "gpu-id"

static gchar *
get_absolute_file_path (gchar *cfg_file_path, gchar *file_path)
{
  gchar abs_cfg_path[PATH_MAX + 1];
  gchar *abs_file_path;
  gchar *delim;

  if (file_path && file_path[0] == '/') {
    return file_path;
  }

  if (!realpath (cfg_file_path, abs_cfg_path)) {
    g_free (file_path);
    return NULL;
  }

  // Return absolute path of config file if file_path is NULL.
  if (!file_path) {
    abs_file_path = g_strdup (abs_cfg_path);
    return abs_file_path;
  }

  delim = g_strrstr (abs_cfg_path, "/");
  *(delim + 1) = '\0';

  abs_file_path = g_strconcat (abs_cfg_path, file_path, NULL);
  g_free (file_path);

  return abs_file_path;
}

static void
decodebin_child_added (GstChildProxy * child_proxy, GObject * object,
    gchar * name, gpointer user_data)
{
  g_print ("Decodebin child added: %s\n", name);
  if (g_strrstr (name, "decodebin") == name) {
    g_signal_connect (G_OBJECT (object), "child-added",
        G_CALLBACK (decodebin_child_added), user_data);
  }
}

static void
cb_newpad (GstElement * decodebin, GstPad * decoder_src_pad, gpointer data)
{
  g_print ("In cb_newpad\n");
  GstCaps *caps = gst_pad_get_current_caps (decoder_src_pad);
  const GstStructure *str = gst_caps_get_structure (caps, 0);
  const gchar *name = gst_structure_get_name (str);
  GstElement *source_bin = (GstElement *) data;
  GstCapsFeatures *features = gst_caps_get_features (caps, 0);

  /* Need to check if the pad created by the decodebin is for video and not
   * audio. */
  if (!strncmp (name, "video", 5)) {
    /* Link the decodebin pad only if decodebin has picked nvidia
     * decoder plugin nvdec_*. We do this by checking if the pad caps contain
     * NVMM memory features. */
    if (gst_caps_features_contains (features, GST_CAPS_FEATURES_NVMM)) {
      /* Get the source bin ghost pad */
      GstPad *bin_ghost_pad = gst_element_get_static_pad (source_bin, "src");
      if (!gst_ghost_pad_set_target (GST_GHOST_PAD (bin_ghost_pad),
              decoder_src_pad)) {
        g_printerr ("Failed to link decoder src pad to source bin ghost pad\n");
      }
      gst_object_unref (bin_ghost_pad);
    } else {
      g_printerr ("Error: Decodebin did not pick nvidia decoder plugin.\n");
    }
  }
}

static gboolean
set_tracker_properties (GstElement *nvtracker)
{
  gboolean ret = FALSE;
  GError *error = NULL;
  gchar **keys = NULL;
  gchar **key = NULL;
  GKeyFile *key_file = g_key_file_new ();

  if (!g_key_file_load_from_file (key_file, TRACKER_CONFIG_FILE, G_KEY_FILE_NONE,
          &error)) {
    g_printerr ("Failed to load config file: %s\n", error->message);
    return FALSE;
  }

  keys = g_key_file_get_keys (key_file, CONFIG_GROUP_TRACKER, NULL, &error);
  CHECK_ERROR (error);

  for (key = keys; *key; key++) {
    if (!g_strcmp0 (*key, CONFIG_GROUP_TRACKER_WIDTH)) {
      gint width =
          g_key_file_get_integer (key_file, CONFIG_GROUP_TRACKER,
          CONFIG_GROUP_TRACKER_WIDTH, &error);
      CHECK_ERROR (error);
      g_object_set (G_OBJECT (nvtracker), "tracker-width", width, NULL);
    } else if (!g_strcmp0 (*key, CONFIG_GROUP_TRACKER_HEIGHT)) {
      gint height =
          g_key_file_get_integer (key_file, CONFIG_GROUP_TRACKER,
          CONFIG_GROUP_TRACKER_HEIGHT, &error);
      CHECK_ERROR (error);
      g_object_set (G_OBJECT (nvtracker), "tracker-height", height, NULL);
    } else if (!g_strcmp0 (*key, CONFIG_GPU_ID)) {
      guint gpu_id =
          g_key_file_get_integer (key_file, CONFIG_GROUP_TRACKER,
          CONFIG_GPU_ID, &error);
      CHECK_ERROR (error);
      g_object_set (G_OBJECT (nvtracker), "gpu_id", gpu_id, NULL);
    } else if (!g_strcmp0 (*key, CONFIG_GROUP_TRACKER_LL_CONFIG_FILE)) {
      char* ll_config_file = get_absolute_file_path (TRACKER_CONFIG_FILE,
                g_key_file_get_string (key_file,
                    CONFIG_GROUP_TRACKER,
                    CONFIG_GROUP_TRACKER_LL_CONFIG_FILE, &error));
      CHECK_ERROR (error);
      g_object_set (G_OBJECT (nvtracker), "ll-config-file", ll_config_file, NULL);
    } else if (!g_strcmp0 (*key, CONFIG_GROUP_TRACKER_LL_LIB_FILE)) {
      char* ll_lib_file = get_absolute_file_path (TRACKER_CONFIG_FILE,
                g_key_file_get_string (key_file,
                    CONFIG_GROUP_TRACKER,
                    CONFIG_GROUP_TRACKER_LL_LIB_FILE, &error));
      CHECK_ERROR (error);
      g_object_set (G_OBJECT (nvtracker), "ll-lib-file", ll_lib_file, NULL);
    } else if (!g_strcmp0 (*key, CONFIG_GROUP_TRACKER_ENABLE_BATCH_PROCESS)) {
      gboolean enable_batch_process =
          g_key_file_get_integer (key_file, CONFIG_GROUP_TRACKER,
          CONFIG_GROUP_TRACKER_ENABLE_BATCH_PROCESS, &error);
      CHECK_ERROR (error);
      g_object_set (G_OBJECT (nvtracker), "enable_batch_process",
                    enable_batch_process, NULL);
    } else {
      g_printerr ("Unknown key '%s' for group [%s]", *key,
          CONFIG_GROUP_TRACKER);
    }
  }

  ret = TRUE;
done:
  if (error) {
    g_error_free (error);
  }
  if (keys) {
    g_strfreev (keys);
  }
  if (!ret) {
    g_printerr ("%s failed", __func__);
  }
  return ret;
}

static GstElement *
create_source_bin (guint index, gchar * uri)
{
  GstElement *bin = NULL, *uri_decode_bin = NULL;
  gchar bin_name[16] = { };

  g_snprintf (bin_name, 15, "source-bin-%02d", index);
  /* Create a source GstBin to abstract this bin's content from the rest of the
   * pipeline */
  bin = gst_bin_new (bin_name);

  /* Source element for reading from the uri.
   * We will use decodebin and let it figure out the container format of the
   * stream and the codec and plug the appropriate demux and decode plugins. */
  uri_decode_bin = gst_element_factory_make ("uridecodebin", "uri-decode-bin");

  if (!bin || !uri_decode_bin) {
    g_printerr ("One element in source bin could not be created.\n");
    return NULL;
  }

  /* We set the input uri to the source element */
  g_object_set (G_OBJECT (uri_decode_bin), "uri", uri, NULL);

  /* Connect to the "pad-added" signal of the decodebin which generates a
   * callback once a new pad for raw data has beed created by the decodebin */
  g_signal_connect (G_OBJECT (uri_decode_bin), "pad-added",
      G_CALLBACK (cb_newpad), bin);
  g_signal_connect (G_OBJECT (uri_decode_bin), "child-added",
      G_CALLBACK (decodebin_child_added), bin);

  gst_bin_add (GST_BIN (bin), uri_decode_bin);

  /* We need to create a ghost pad for the source bin which will act as a proxy
   * for the video decoder src pad. The ghost pad will not have a target right
   * now. Once the decode bin creates the video decoder and generates the
   * cb_newpad callback, we will set the ghost pad target to the video decoder
   * src pad. */
  if (!gst_element_add_pad (bin, gst_ghost_pad_new_no_target ("src",
              GST_PAD_SRC))) {
    g_printerr ("Failed to add ghost pad in source bin\n");
    return NULL;
  }

  return bin;
}

int
main (int argc, char *argv[])
{
  GMainLoop *loop = NULL;
  GstElement *pipeline = NULL, *source = NULL, *h264parser = NULL,
      *decoder = NULL, *streammux = NULL, *sink = NULL, *pgie = NULL, *sgie = NULL, *nvvidconv = NULL,
      *nvosd = NULL, *nvtracker = NULL;
  guint num_sources = 1;
  guint i;
  g_print ("With tracker\n");
#ifdef PLATFORM_TEGRA
  GstElement *transform = NULL;
#endif
  GstBus *bus = NULL;
  guint bus_watch_id = 0;
  GstPad *osd_sink_pad = NULL;

  /* Check input arguments */
  if (argc != 2) {
    g_printerr ("Usage: %s <uri1>\n", argv[0]);
    return -1;
  }

  /* Standard GStreamer initialization */
  gst_init (&argc, &argv);
  loop = g_main_loop_new (NULL, FALSE);

  /* Create gstreamer elements */

  /* Create Pipeline element that will be a container of other elements */
  pipeline = gst_pipeline_new ("dstest2-pipeline");

  /* Create nvstreammux instance to form batches from one or more sources. */
  streammux = gst_element_factory_make ("nvstreammux", "stream-muxer");

  gst_bin_add (GST_BIN (pipeline), streammux);

  if (!pipeline || !streammux) {
    g_printerr ("One element could not be created. Exiting.\n");
    return -1;
  }

  for (i = 0; i < num_sources; i++) {
    GstPad *sinkpad, *srcpad;
    gchar pad_name[16] = { };
    GstElement *source_bin = create_source_bin (i, argv[i + 1]);

    if (!source_bin) {
      g_printerr ("Failed to create source bin. Exiting.\n");
      return -1;
    }

    gst_bin_add (GST_BIN (pipeline), source_bin);

    g_snprintf (pad_name, 15, "sink_%u", i);
    sinkpad = gst_element_get_request_pad (streammux, pad_name);
    if (!sinkpad) {
      g_printerr ("Streammux request sink pad failed. Exiting.\n");
      return -1;
    }

    srcpad = gst_element_get_static_pad (source_bin, "src");
    if (!srcpad) {
      g_printerr ("Failed to get src pad of source bin. Exiting.\n");
      return -1;
    }

    if (gst_pad_link (srcpad, sinkpad) != GST_PAD_LINK_OK) {
      g_printerr ("Failed to link source bin to stream muxer. Exiting.\n");
      return -1;
    }

    gst_object_unref (srcpad);
    gst_object_unref (sinkpad);
  }

  /* Use nvinfer to run inferencing on decoder's output,
   * behaviour of inferencing is set through config file */
  pgie = gst_element_factory_make ("nvinfer", "primary-nvinference-engine");

  /* We add another detector depeing on the requirement */
  sgie = gst_element_factory_make ("nvinfer", "secondary-nvinference-engine");

  /* We need to have a tracker to track the identified objects */
  nvtracker = gst_element_factory_make ("nvtracker", "tracker");

  /* Use convertor to convert from NV12 to RGBA as required by nvosd */
  nvvidconv = gst_element_factory_make ("nvvideoconvert", "nvvideo-converter");

  /* Create OSD to draw on the converted RGBA buffer */
  nvosd = gst_element_factory_make ("nvdsosd", "nv-onscreendisplay");

  /* Finally render the osd output */
#ifdef PLATFORM_TEGRA
  transform = gst_element_factory_make ("nvegltransform", "nvegl-transform");
#endif
  sink = gst_element_factory_make ("nveglglessink", "nvvideo-renderer");

  if (!pgie || !sgie ||
      !nvtracker || !nvvidconv || !nvosd || !sink) {
    g_printerr ("One element could not be created. Exiting.\n");
    return -1;
  }

#ifdef PLATFORM_TEGRA
  if(!transform) {
    g_printerr ("One tegra element could not be created. Exiting.\n");
    return -1;
  }
#endif

  g_object_set (G_OBJECT (streammux), "batch-size", 1, NULL);

  g_object_set (G_OBJECT (streammux), "width", MUXER_OUTPUT_WIDTH, "height",
      MUXER_OUTPUT_HEIGHT,
      "batched-push-timeout", MUXER_BATCH_TIMEOUT_USEC, NULL);

  gchar *pgie_engine_path = (char*)"./models/peoplenet/resnet18_detector.etlt_b1_gpu0_fp16.engine";
  gchar *sgie_engine_path = (char*)"./models/wheelchairnet/resnet18_detector.etlt_b1_gpu0_fp16.engine";

  /* Set all the necessary properties of the nvinfer element,
   * the necessary ones are : */
  g_object_set (G_OBJECT (pgie), "config-file-path", PGIE_CONFIG_FILE, NULL);
  g_object_set (G_OBJECT (pgie), "model-engine-file", pgie_engine_path, NULL);
  g_object_set (G_OBJECT (sgie), "config-file-path", SGIE_CONFIG_FILE, NULL);
  g_object_set (G_OBJECT (sgie), "model-engine-file", sgie_engine_path, NULL);

  /* Set necessary properties of the tracker element. */
  if (!set_tracker_properties(nvtracker)) {
    g_printerr ("Failed to set tracker properties. Exiting.\n");
    return -1;
  }

  /* we add a message handler */
  bus = gst_pipeline_get_bus (GST_PIPELINE (pipeline));
  bus_watch_id = gst_bus_add_watch (bus, bus_call, loop);
  gst_object_unref (bus);

  /* Set up the pipeline */
  /* we add all elements into the pipeline */
  /* decoder | pgie1 | nvtracker | sgie1 | sgie2 | sgie3 | etc.. */
#ifdef PLATFORM_TEGRA
  gst_bin_add_many (GST_BIN (pipeline),
      pgie, sgie, nvtracker,
      nvvidconv, nvosd, transform, sink, NULL);
#else
  gst_bin_add_many (GST_BIN (pipeline),
      pgie, sgie, nvtracker,
      nvvidconv, nvosd, sink, NULL);
#endif

  // GstPad *sinkpad, *srcpad;
  // gchar pad_name_sink[16] = "sink_0";
  // gchar pad_name_src[16] = "src";

  // sinkpad = gst_element_get_request_pad (streammux, pad_name_sink);
  // if (!sinkpad) {
  //   g_printerr ("Streammux request sink pad failed. Exiting.\n");
  //   return -1;
  // }

  // srcpad = gst_element_get_static_pad (decoder, pad_name_src);
  // if (!srcpad) {
  //   g_printerr ("Decoder request src pad failed. Exiting.\n");
  //   return -1;
  // }

  // if (gst_pad_link (srcpad, sinkpad) != GST_PAD_LINK_OK) {
  //     g_printerr ("Failed to link decoder to stream muxer. Exiting.\n");
  //     return -1;
  // }

  // gst_object_unref (sinkpad);
  // gst_object_unref (srcpad);

  /* Link the elements together */
  // if (!gst_element_link_many (source, h264parser, decoder, NULL)) {
  //   g_printerr ("Elements could not be linked: 1. Exiting.\n");
  //   return -1;
  // }

#ifdef PLATFORM_TEGRA
  if (!gst_element_link_many (streammux, pgie, sgie, nvtracker,
      nvvidconv, nvosd, transform, sink, NULL)) {
    g_printerr ("Elements could not be linked. Exiting.\n");
    return -1;
  }
#else
  if (!gst_element_link_many (streammux, pgie, sgie, nvtracker,
      nvvidconv, nvosd, sink, NULL)) {
    g_printerr ("Elements could not be linked. Exiting.\n");
    return -1;
  }
#endif

  /* Lets add probe to get informed of the meta data generated, we add probe to
   * the sink pad of the osd element, since by that time, the buffer would have
   * had got all the metadata. */
  osd_sink_pad = gst_element_get_static_pad (nvosd, "sink");
  if (!osd_sink_pad)
    g_print ("Unable to get sink pad\n");
  else
    gst_pad_add_probe (osd_sink_pad, GST_PAD_PROBE_TYPE_BUFFER,
        osd_sink_pad_buffer_probe, NULL, NULL);
  gst_object_unref (osd_sink_pad);

  /* Set the pipeline to "playing" state */
  g_print ("Now playing: %s\n", argv[1]);
  gst_element_set_state (pipeline, GST_STATE_PLAYING);

  /* Iterate */
  g_print ("Running...\n");
  g_main_loop_run (loop);

  /* Out of the main loop, clean up nicely */
  g_print ("Returned, stopping playback\n");
  gst_element_set_state (pipeline, GST_STATE_NULL);
  g_print ("Deleting pipeline\n");
  gst_object_unref (GST_OBJECT (pipeline));
  g_source_remove (bus_watch_id);
  g_main_loop_unref (loop);
  return 0;
}
