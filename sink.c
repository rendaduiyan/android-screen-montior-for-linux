#include "sink.h"

static guint16 udp_port = 54321;
static gsize buffer_size = 100 * 1024;

gboolean init_sink (int argc, char **argv, SinkData *sink)
{
    gboolean ret = FALSE;
    /*
     *Initialize gstreamer
     */
    gst_init (&argc, &argv);

    GstElement *converter;
    GstElement *capsfilter;
    GstElement *scale;
    GstElement *converter2;
    GstElement *enc;
    GstElement *rtp_pay;
    GstElement *udpsink;

    /*
     * Create gstreamer elements
     */
    sink->appsrc = gst_element_factory_make ("appsrc", "video_source");
    converter = gst_element_factory_make ("videoconvert", "converter"); 
    sink->caps_filter = gst_element_factory_make ("capsfilter", "caps_filter");
    scale = gst_element_factory_make ("videoscale", "scale"); 
    capsfilter = gst_element_factory_make ("capsfilter", "caps_filter1");
    converter2 = gst_element_factory_make ("videoconvert", "converter2"); 
    enc = gst_element_factory_make ("jpegenc", "encoder"); 
    rtp_pay = gst_element_factory_make ("rtpjpegpay", "rtp_pay"); 
    udpsink = gst_element_factory_make ("udpsink", "video_sink");

    /*
     * Create empty pipeline
     */
    sink->pipeline = gst_pipeline_new ("screenshot-pipeline");
    
    /*
     * check
     */
    if (!sink->appsrc
        || !converter
        || !capsfilter
        || !scale
        || !sink->caps_filter
        || !converter2
        || !enc
        || !rtp_pay
        || !udpsink
        )
    {
        g_error ("failed to create gstreamer elements");
        return ret;
    }

    g_object_set (sink->appsrc, "do-timestamp", TRUE, NULL);
    g_object_set (rtp_pay, "mtu", 65000, NULL);
    g_object_set (udpsink, "port", udp_port, "host", "127.0.0.1",
                                 "sync", FALSE,
                                 "buffer-size", buffer_size, NULL);
    /*
     * Set default video caps
     * can't set the caps from the begining because the video format is unknown 
     * for now
     * we can change the caps later
     */
    GstCaps *video_caps = gst_caps_new_simple ("video/x-raw",
        "format", G_TYPE_STRING, "RGBA",
        "width", G_TYPE_INT, 1080,
        "height", G_TYPE_INT, 1920,
        NULL);
    g_object_set (sink->appsrc, "caps", video_caps, "format", GST_FORMAT_TIME, NULL);

    GstCaps *scale_caps = gst_caps_new_simple ("video/x-raw",
        "format", G_TYPE_STRING, "I420",
        NULL);
    GstCaps *scale_caps2 = gst_caps_new_simple ("video/x-raw",
        "format", G_TYPE_STRING, "I420",
        "width", G_TYPE_INT, 540,
        "height", G_TYPE_INT, gst_util_uint64_scale_int_round (540, 1920, 1080),
        NULL);

    g_object_set (capsfilter, "caps", scale_caps, NULL);
    g_object_set (sink->caps_filter, "caps", scale_caps2, NULL);

    gst_caps_unref (video_caps);
    gst_caps_unref (scale_caps);
    gst_caps_unref (scale_caps2);

    /*
     * Link all elements
     */
    gst_bin_add_many (GST_BIN (sink->pipeline), 
                      sink->appsrc,
                      converter,
                      capsfilter,
                      scale,
                      sink->caps_filter,
                      converter2,
                      enc,
                      rtp_pay,
                      udpsink,
                      NULL);
    if (gst_element_link_many (sink->appsrc, 
                               converter, 
                               capsfilter,
                               scale,
                               sink->caps_filter,
                               converter2,
                               enc,
                               rtp_pay,
                               udpsink, 
                               NULL) != TRUE)
    {
        g_error ("failed to link elements");
        gst_object_unref (sink->pipeline);
        return ret;
    }

    /*
     * Instruct the bus to emit signal
     */
    GstBus *bus = gst_element_get_bus (sink->pipeline);
    sink->bus_watch_id = gst_bus_add_watch (bus, bus_cb, sink);
    gst_object_unref (bus);

    /*
     * ready to play
     */
    GstClock *clock = gst_pipeline_get_clock (GST_PIPELINE (sink->pipeline));
    sink->initial_buffer_time = gst_clock_get_time (clock);
    gst_element_set_state (sink->pipeline, GST_STATE_PLAYING);
}

gboolean bus_cb (GstBus *bus, GstMessage *msg, gpointer user_data)
{
    g_info ("Got %s message", GST_MESSAGE_TYPE_NAME (msg));
    SinkData *sink = (SinkData *) user_data;
    switch (GST_MESSAGE_TYPE (msg))
    {
        case GST_MESSAGE_ERROR:
        {
            GError *err = NULL;
            gchar *debug_info = NULL;

            gst_message_parse_error (msg, &err, &debug_info);
            //g_error ("Error received from element %s: %s", GST_OBJECT_NAME (msg->src),
            //            err->message);
            g_clear_error (&err);
            g_free (debug_info);
            g_main_loop_quit (sink->loop);
            break;
        }
        case GST_MESSAGE_EOS:
        {
            g_main_loop_quit (sink->loop);
            break;
        }
        default:
        {
            g_info ("unhandled messages");
            break;
        }
    }
    /*
     * keep watching
     */
    return TRUE;
}

void release_sink (SinkData *sink)
{
    g_source_remove (sink->bus_watch_id);
    gst_element_set_state (sink->pipeline, GST_STATE_NULL);
    gst_object_unref (sink->pipeline);
}

void push_data (SinkData *sink, guint8 *data, gsize size)
{
    GstFlowReturn ret = GST_FLOW_OK;
    GstBuffer *buffer = gst_buffer_new_wrapped (data, size);

    /*
    GstClock *clock = gst_pipeline_get_clock (GST_PIPELINE (sink->pipeline));
    GstClockTime buffer_time = gst_clock_get_time (clock) - sink->initial_buffer_time;
    buffer_time = buffer_time / 1000000000 * 1000000000;
    buffer->pts = buffer_time;
    buffer->dts = buffer_time;
    */

    g_signal_emit_by_name (sink->appsrc, "push-buffer", buffer, &ret);
    if (GST_FLOW_OK != ret)
    {
        g_error ("failed to push buffer");
    }
    gst_buffer_unref (buffer);
}

