#ifndef _SINK_H_
#define _SINK_H_

#include <gst/gst.h>


typedef struct _SinkData
{
    GstElement *pipeline;
    GstElement *appsrc;
    GstElement *caps_filter;
    GstClockTime initial_buffer_time;
    GMainLoop *loop;
    guint bus_watch_id;
} SinkData;

gboolean init_sink (int argc, char **argv, SinkData *sink);

gboolean bus_cb (GstBus *bus, GstMessage *msg, gpointer user_data);

void release_sink (SinkData *sink);

void push_data (SinkData *sink, guint8 *data, gsize size);

#endif
