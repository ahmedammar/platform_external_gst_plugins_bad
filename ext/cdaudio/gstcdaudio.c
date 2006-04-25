/* GStreamer
 * Copyright (C) <1999> Erik Walthinsen <omega@cse.ogi.edu>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include <string.h>
#include <cdaudio.h>

#include <gst/gst.h>

#define GST_TYPE_CDAUDIO                (gst_cdaudio_get_type())
#define GST_CDAUDIO(obj)                (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_CDAUDIO,GstCDAudio))
#define GST_CDAUDIO_CLASS(klass)        (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_CDAUDIO,GstCDAudioClass))
#define GST_IS_CDAUDIO(obj)             (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_CDAUDIO))
#define GST_IS_CDAUDIO_CLASS(klass)     (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_CDAUDIO))

typedef struct _GstCDAudio GstCDAudio;
typedef struct _GstCDAudioClass GstCDAudioClass;

struct _GstCDAudio
{
  GstBin element;

  /* properties */
  gchar *device;
  gchar *uri;

  gint cd_desc;
  gulong discid;

  gboolean was_playing;

  struct disc_info info;
  struct disc_volume volume;

  GTimer *timer;
};

struct _GstCDAudioClass
{
  GstBinClass parent_class;

  void (*close_tray) (GstElement * element);
  /* signal callbacks */
  void (*track_change) (GstElement * element, guint track);
};

/* props */
enum
{
  ARG_0,
  ARG_DEVICE,
  ARG_DISCID,
  ARG_VOLUME_FR,
  ARG_VOLUME_FL,
  ARG_VOLUME_BR,
  ARG_VOLUME_BL
};

/* signals */
enum
{
  TRACK_CHANGE,
  LAST_SIGNAL
};

static void gst_cdaudio_class_init (GstCDAudioClass * klass);
static void gst_cdaudio_init (GstCDAudio * cdaudio);
static void gst_cdaudio_finalize (GObject * object);

static void gst_cdaudio_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * spec);
static void gst_cdaudio_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * spec);
static GstStateChangeReturn gst_cdaudio_change_state (GstElement * element,
    GstStateChange transition);

static const GstEventMask *gst_cdaudio_get_event_masks (GstElement * element);
static gboolean gst_cdaudio_send_event (GstElement * element, GstEvent * event);
static const GstFormat *gst_cdaudio_get_formats (GstElement * element);
static gboolean gst_cdaudio_convert (GstElement * element,
    GstFormat src_format, gint64 src_value,
    GstFormat * dest_format, gint64 * dest_value);
static const GstQueryType *gst_cdaudio_get_query_types (GstElement * element);
static gboolean gst_cdaudio_query (GstElement * element, GstQueryType type,
    GstFormat * format, gint64 * value);

static void cdaudio_uri_handler_init (gpointer g_iface, gpointer iface_data);

static GstFormat track_format;
static GstFormat sector_format;

static GstBinClass *parent_class;
static guint gst_cdaudio_signals[LAST_SIGNAL] = { 0 };

static const GstElementDetails gst_cdaudio_details =
GST_ELEMENT_DETAILS ("CD player",
    "Generic/Bin",
    "Play CD audio through the CD Drive",
    "Wim Taymans <wim@fluendo.com>");


static void
_do_init (GType cdaudio_type)
{
  static const GInterfaceInfo urihandler_info = {
    cdaudio_uri_handler_init,
    NULL,
    NULL,
  };

  g_type_add_interface_static (cdaudio_type, GST_TYPE_URI_HANDLER,
      &urihandler_info);
}


GST_BOILERPLATE_FULL (GstCDAudio, gst_cdaudio, GstBin, GST_TYPE_BIN, _do_init);

static void
gst_cdaudio_base_init (gpointer g_class)
{
  GstElementClass *element_class = GST_ELEMENT_CLASS (g_class);

  gst_element_class_set_details (element_class, &gst_cdaudio_details);

  /* Register the track and sector format */
  track_format = gst_format_register ("track", "CD track");
  sector_format = gst_format_register ("sector", "CD sector");
}

static void
gst_cdaudio_class_init (GstCDAudioClass * klass)
{
  GObjectClass *gobject_klass;
  GstElementClass *gstelement_klass;
  GstBinClass *gstbin_klass;

  gobject_klass = (GObjectClass *) klass;
  gstelement_klass = (GstElementClass *) klass;
  gstbin_klass = (GstBinClass *) klass;

  parent_class = g_type_class_peek_parent (klass);

  gobject_klass->set_property = gst_cdaudio_set_property;
  gobject_klass->get_property = gst_cdaudio_get_property;

  g_object_class_install_property (gobject_klass, ARG_DEVICE,
      g_param_spec_string ("device", "Device", "CDROM device",
          NULL, G_PARAM_READWRITE));
  g_object_class_install_property (gobject_klass, ARG_DISCID,
      g_param_spec_ulong ("discid", "Disc ID", "CDDB Disc ID",
          0, G_MAXULONG, 0, G_PARAM_READABLE));
  g_object_class_install_property (gobject_klass, ARG_VOLUME_FL,
      g_param_spec_int ("volume_fl", "Volume fl", "Front left volume",
          0, 255, 255, G_PARAM_READWRITE));
  g_object_class_install_property (gobject_klass, ARG_VOLUME_FR,
      g_param_spec_int ("volume_fr", "Volume fr", "Front right volume",
          0, 255, 255, G_PARAM_READWRITE));
  g_object_class_install_property (gobject_klass, ARG_VOLUME_BL,
      g_param_spec_int ("volume_bl", "Volume bl", "Back left volume",
          0, 255, 255, G_PARAM_READWRITE));
  g_object_class_install_property (gobject_klass, ARG_VOLUME_BR,
      g_param_spec_int ("volume_br", "Volume br", "Back right volume",
          0, 255, 255, G_PARAM_READWRITE));

  gst_cdaudio_signals[TRACK_CHANGE] =
      g_signal_new ("track-change", G_TYPE_FROM_CLASS (klass),
      G_SIGNAL_RUN_LAST, G_STRUCT_OFFSET (GstCDAudioClass, track_change), NULL,
      NULL, gst_marshal_VOID__INT, G_TYPE_NONE, 1, G_TYPE_INT);

  gobject_klass->finalize = GST_DEBUG_FUNCPTR (gst_cdaudio_finalize);

  gstelement_klass->change_state = GST_DEBUG_FUNCPTR (gst_cdaudio_change_state);
  gstelement_klass->get_event_masks =
      GST_DEBUG_FUNCPTR (gst_cdaudio_get_event_masks);
  gstelement_klass->send_event = GST_DEBUG_FUNCPTR (gst_cdaudio_send_event);
  gstelement_klass->get_formats = GST_DEBUG_FUNCPTR (gst_cdaudio_get_formats);
  gstelement_klass->convert = GST_DEBUG_FUNCPTR (gst_cdaudio_convert);
  gstelement_klass->get_query_types =
      GST_DEBUG_FUNCPTR (gst_cdaudio_get_query_types);
  gstelement_klass->query = GST_DEBUG_FUNCPTR (gst_cdaudio_query);
}

static void
gst_cdaudio_init (GstCDAudio * cdaudio, GstCDAudioClass * g_class)
{
  cdaudio->device = g_strdup ("/dev/cdrom");
  cdaudio->was_playing = FALSE;
  cdaudio->timer = g_timer_new ();

  GST_OBJECT_FLAG_SET (cdaudio, GST_BIN_FLAG_MANAGER);
  GST_OBJECT_FLAG_SET (cdaudio, GST_BIN_SELF_SCHEDULABLE);
}

static void
gst_cdaudio_finalize (GObject * object)
{
  GstCDAudio *cdaudio = GST_CDAUDIO (object);

  g_timer_destroy (cdaudio->timer);
  g_free (cdaudio->device);

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static void
gst_cdaudio_set_property (GObject * object, guint prop_id, const GValue * value,
    GParamSpec * spec)
{
  GstCDAudio *cdaudio;

  g_return_if_fail (GST_IS_CDAUDIO (object));

  cdaudio = GST_CDAUDIO (object);

  switch (prop_id) {
    case ARG_DEVICE:
      break;
    case ARG_VOLUME_FR:
      break;
    case ARG_VOLUME_FL:
      break;
    case ARG_VOLUME_BR:
      break;
    case ARG_VOLUME_BL:
      break;
    default:
      break;
  }
}

static void
gst_cdaudio_get_property (GObject * object, guint prop_id, GValue * value,
    GParamSpec * spec)
{
  GstCDAudio *cdaudio;

  g_return_if_fail (GST_IS_CDAUDIO (object));

  cdaudio = GST_CDAUDIO (object);

  switch (prop_id) {
    case ARG_DEVICE:
      g_value_set_string (value, cdaudio->device);
      break;
    case ARG_DISCID:
      g_value_set_ulong (value, cdaudio->discid);
      break;
    case ARG_VOLUME_FR:
      g_value_set_int (value, cdaudio->volume.vol_front.right);
      break;
    case ARG_VOLUME_FL:
      g_value_set_int (value, cdaudio->volume.vol_front.left);
      break;
    case ARG_VOLUME_BR:
      g_value_set_int (value, cdaudio->volume.vol_back.right);
      break;
    case ARG_VOLUME_BL:
      g_value_set_int (value, cdaudio->volume.vol_back.left);
      break;
    default:
      break;
  }
}

static void
debug_track_info (GstCDAudio * cdaudio)
{
  gint i;

  for (i = 0; i < cdaudio->info.disc_total_tracks; i++) {
    GST_DEBUG_OBJECT (cdaudio, "%d %d %d %d:%02d", i,
        cdaudio->info.disc_track[i].track_length.frames,
        cdaudio->info.disc_track[i].track_pos.frames,
        cdaudio->info.disc_track[i].track_length.minutes,
        cdaudio->info.disc_track[i].track_length.seconds);
  }
}

static GstStateChangeReturn
gst_cdaudio_change_state (GstElement * element, GstStateChange transition)
{
  GstCDAudio *cdaudio;

  cdaudio = GST_CDAUDIO (element);

  switch (transition) {
    case GST_STATE_CHANGE_NULL_TO_READY:
      break;
    case GST_STATE_CHANGE_READY_TO_PAUSED:
      cdaudio->cd_desc = cd_init_device (cdaudio->device);
      if (cdaudio->cd_desc < 0)
        return GST_STATE_CHANGE_FAILURE;

      /* close tray */
      if (cd_close (cdaudio->cd_desc) < 0)
        return GST_STATE_CHANGE_FAILURE;

      if (cd_stat (cdaudio->cd_desc, &cdaudio->info) < 0)
        return GST_STATE_CHANGE_FAILURE;

      debug_track_info (cdaudio);

      cdaudio->discid = cddb_discid (cdaudio->cd_desc);
      g_object_notify (G_OBJECT (cdaudio), "discid");

      cdaudio->was_playing = FALSE;
      break;
    case GST_STATE_CHANGE_PAUSED_TO_PLAYING:
    {
      gint res;

      if (cdaudio->was_playing)
        res = cd_resume (cdaudio->cd_desc);
      else
        res = cd_play (cdaudio->cd_desc, 1);

      if (res < 0)
        return GST_STATE_CHANGE_FAILURE;

      cdaudio->was_playing = TRUE;
      g_timer_start (cdaudio->timer);
      break;
    }
    case GST_STATE_CHANGE_PLAYING_TO_PAUSED:
      if (cd_pause (cdaudio->cd_desc) < 0)
        return GST_STATE_CHANGE_FAILURE;
      g_timer_stop (cdaudio->timer);
      break;
    case GST_STATE_CHANGE_PAUSED_TO_READY:
      if (cd_stop (cdaudio->cd_desc) < 0)
        return GST_STATE_CHANGE_FAILURE;
      if (cd_finish (cdaudio->cd_desc) < 0)
        return GST_STATE_CHANGE_FAILURE;
      break;
    case GST_STATE_CHANGE_READY_TO_NULL:
      break;
    default:
      break;
  }

  if (GST_ELEMENT_CLASS (parent_class)->change_state) {
    return GST_ELEMENT_CLASS (parent_class)->change_state (element, transition);
  }

  return GST_STATE_CHANGE_SUCCESS;
}

static const GstEventMask *
gst_cdaudio_get_event_masks (GstElement * element)
{
  static const GstEventMask masks[] = {
    {GST_EVENT_SEEK, GST_SEEK_METHOD_SET |
          GST_SEEK_METHOD_CUR | GST_SEEK_METHOD_END | GST_SEEK_FLAG_FLUSH},
    {GST_EVENT_SEEK_SEGMENT, GST_SEEK_METHOD_SET |
          GST_SEEK_METHOD_CUR | GST_SEEK_METHOD_END | GST_SEEK_FLAG_FLUSH},
    {0,}
  };

  return masks;
}

static gboolean
gst_cdaudio_send_event (GstElement * element, GstEvent * event)
{
  GstCDAudio *cdaudio;
  gboolean res = TRUE;

  cdaudio = GST_CDAUDIO (element);

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_SEEK:
      switch (GST_EVENT_SEEK_FORMAT (event)) {
        case GST_FORMAT_TIME:
        {
          if (cd_play_pos (cdaudio->cd_desc, 1,
                  (gint) (GST_EVENT_SEEK_OFFSET (event) / (GST_SECOND))) == -1)
            res = FALSE;
          break;
        }
        default:
          res = FALSE;
          break;
      }
      break;
    default:
      res = FALSE;
      break;
  }
  gst_event_unref (event);
  return res;
}

const GstFormat *
gst_cdaudio_get_formats (GstElement * element)
{
  static GstFormat formats[] = {
    GST_FORMAT_TIME,
    GST_FORMAT_BYTES,
    GST_FORMAT_DEFAULT,
    0,                          /* fillted below */
    0,                          /* fillted below */
    0,
  };

  formats[3] = track_format;
  formats[4] = sector_format;

  return formats;
}

gboolean
gst_cdaudio_convert (GstElement * element,
    GstFormat src_format, gint64 src_value,
    GstFormat * dest_format, gint64 * dest_value)
{
  return FALSE;
}

const GstQueryType *
gst_cdaudio_get_query_types (GstElement * element)
{
  static const GstQueryType query_types[] = {
    GST_QUERY_TOTAL,
    GST_QUERY_POSITION,
    GST_QUERY_START,
    GST_QUERY_SEGMENT_END,
    0
  };

  return query_types;
}

gboolean
gst_cdaudio_query (GstElement * element, GstQueryType type,
    GstFormat * format, gint64 * value)
{
  GstCDAudio *cdaudio;
  gboolean res = TRUE;
  gulong micros;
  gdouble seconds;

  cdaudio = GST_CDAUDIO (element);

  /* take new snapshot every 1000 miliseconds */
  seconds = g_timer_elapsed (cdaudio->timer, &micros);
  if (micros > 1000 || seconds > 1) {
    cd_stat (cdaudio->cd_desc, &cdaudio->info);
    g_timer_start (cdaudio->timer);
  }

  switch (type) {
    case GST_QUERY_TOTAL:
      switch (*format) {
        case GST_FORMAT_TIME:
          *value = (cdaudio->info.disc_length.minutes * 60 +
              cdaudio->info.disc_length.seconds) * GST_SECOND;
          break;
        default:
        {
          if (*format == track_format) {
            *value = cdaudio->info.disc_total_tracks;
          } else {
            res = FALSE;
          }
          break;
        }
      }
      break;
    case GST_QUERY_POSITION:
      switch (*format) {
        case GST_FORMAT_TIME:
          *value = (cdaudio->info.disc_time.minutes * 60 +
              cdaudio->info.disc_time.seconds) * GST_SECOND;
          break;
        default:
        {
          if (*format == track_format) {
            *value = cdaudio->info.disc_current_track;
          } else {
            res = FALSE;
          }
          break;
        }
      }
      break;
    default:
      res = FALSE;
      break;
  }
  return res;
}

static gboolean
plugin_init (GstPlugin * plugin)
{
  if (!gst_element_register (plugin, "cdaudio", GST_RANK_NONE,
          GST_TYPE_CDAUDIO))
    return FALSE;

  return TRUE;
}

/*** GSTURIHANDLER INTERFACE *************************************************/

static guint
cdaudio_uri_get_type (void)
{
  return GST_URI_SRC;
}
static gchar **
cdaudio_uri_get_protocols (void)
{
  static gchar *protocols[] = { "cd", NULL };

  return protocols;
}
static const gchar *
cdaudio_uri_get_uri (GstURIHandler * handler)
{
  GstCDAudio *cdaudio = GST_CDAUDIO (handler);

  return cdaudio->uri;
}

static gboolean
cdaudio_uri_set_uri (GstURIHandler * handler, const gchar * uri)
{
  gchar *protocol, *location;
  gboolean ret;

  ret = TRUE;

  //GstCDAudio *cdaudio = GST_CDAUDIO(handler);

  protocol = gst_uri_get_protocol (uri);
  if (strcmp (protocol, "cd") != 0) {
    g_free (protocol);
    return FALSE;
  }
  g_free (protocol);

  location = gst_uri_get_location (uri);
  /*
     cdaudio->uri_track = strtol(location,NULL,10);
     if (cdaudio->uri_track > 0) {
     cdaudio->seek_request = cdaudio->uri_track;
     }
   */
  g_free (location);

  return ret;
}

static void
cdaudio_uri_handler_init (gpointer g_iface, gpointer iface_data)
{
  GstURIHandlerInterface *iface = (GstURIHandlerInterface *) g_iface;

  iface->get_type = cdaudio_uri_get_type;
  iface->get_protocols = cdaudio_uri_get_protocols;
  iface->get_uri = cdaudio_uri_get_uri;
  iface->set_uri = cdaudio_uri_set_uri;
}

GST_PLUGIN_DEFINE (GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    "cdaudio",
    "Play CD audio through the CD Drive",
    plugin_init, VERSION, "LGPL", GST_PACKAGE_NAME, GST_PACKAGE_ORIGIN)
