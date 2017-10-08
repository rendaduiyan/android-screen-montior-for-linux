#include <string.h>
#include <gst/gst.h>
#include <gtk/gtk.h>
#include <gdk/gdk.h>
#include <gdk/gdkx.h>
#include <gtk/gtkbutton.h>
#include <gst/video/videooverlay.h>

#include "player.h"

static gsize buffer_size = 100 * 1024;
static guint16 udp_port = 54321;

void close_cb (GtkButton *button, PlayerData *player)
{
    gst_element_set_state (player->pipeline, GST_STATE_READY);
    gtk_main_quit ();
}

void delete_event_cb (GtkWidget *widget, GdkEvent *event, PlayerData *player)
{
    gst_element_set_state (player->pipeline, GST_STATE_READY);
    gtk_main_quit ();
}

GstBusSyncReply bus_sync_handler (GstBus * bus, GstMessage * message, PlayerData *player)
{
    if (!gst_is_video_overlay_prepare_window_handle_message (message))
    {
        return GST_BUS_PASS;
    }
    if (player->video_window_handle != 0)
    {
        GstVideoOverlay *overlay =  GST_VIDEO_OVERLAY (GST_MESSAGE_SRC (message));
        gst_video_overlay_set_window_handle (overlay, player->video_window_handle);
        g_debug ("set the overlay window handle ...");
    }
    else
    {
        g_warning ("window handle should be got for now");
    }
    gst_message_unref (message);
    return GST_BUS_DROP;
}

void realize_cb (GtkWidget *widget, PlayerData *player)
{
    GdkWindow *window = gtk_widget_get_window (widget);
    if (!gdk_window_ensure_native (window))
    {
        g_error ("Couldn't create native window needed for GstXOverlay!");
    }
    player->video_window_handle = GDK_WINDOW_XID (window);
    g_debug ("video_window_handle = %d", player->video_window_handle);
}

gboolean draw_cb (GtkWidget *widget, GdkEventExpose *event, PlayerData *player) 
{
    if (player->state < GST_STATE_PAUSED)
    {
        GtkAllocation allocation;
        gtk_widget_get_allocation (widget, &allocation);
        cairo_t *cr = gdk_cairo_create(event->window);
        cairo_set_source_rgb (cr, 0, 0, 0);
        cairo_rectangle (cr, 0, 0, allocation.width, allocation.height);
        cairo_fill (cr);
        cairo_destroy(cr);
        g_debug ("draw background ...");
    }
    return FALSE;
}

void create_gui (PlayerData *player)
{
    GtkWidget *main_window = gtk_window_new (GTK_WINDOW_TOPLEVEL);
    g_signal_connect (G_OBJECT (main_window), "delete-event", G_CALLBACK (delete_event_cb), player);

    GtkWidget *video_window = gtk_drawing_area_new ();
    gtk_widget_set_size_request (video_window, 540, 720);
    gtk_widget_set_double_buffered (video_window, FALSE);
    g_signal_connect (video_window, "realize", G_CALLBACK (realize_cb), player);
    g_signal_connect (video_window, "expose_event", G_CALLBACK (draw_cb), player);

    GtkWidget *close_button = gtk_button_new_from_stock ("gtk-close");
    g_signal_connect (G_OBJECT (close_button), "clicked", G_CALLBACK (close_cb), player);

    GtkWidget *controls = gtk_hbox_new (TRUE, 2);
    gtk_box_pack_start (GTK_BOX (controls), close_button, TRUE, TRUE, 2);

    GtkWidget *main_box = gtk_vbox_new (FALSE, 2);
    gtk_box_pack_start (GTK_BOX (main_box), video_window, TRUE, TRUE, 0);
    gtk_box_pack_start (GTK_BOX (main_box), controls, FALSE, FALSE, 0);
    gtk_container_add (GTK_CONTAINER (main_window), main_box);
    gtk_window_set_default_size (GTK_WINDOW (main_window), 640, 720);
    gtk_widget_show_all (main_window);
    gtk_widget_realize (video_window);
}

gboolean init_player (int argc, char **argv, PlayerData *player)
{
    gboolean ret = FALSE;
    GstElement *udpsrc;
    GstElement *capsfilter;
    GstElement *rtp_depay;
    GstElement *dec;
    GstElement *autovideosink;
    /*
     *Initialize gstreamer
     */
    gst_init (&argc, &argv);

    gtk_init (&argc, &argv);

    /*
     * Create gstreamer elements
     */
    udpsrc = gst_element_factory_make ("udpsrc", "video_source");
    capsfilter = gst_element_factory_make ("capsfilter", "caps_filter");
    rtp_depay = gst_element_factory_make ("rtpjpegdepay", "rtp_depay"); 
    dec = gst_element_factory_make ("jpegdec", "dec"); 
    autovideosink = gst_element_factory_make ("autovideosink", "video_sink");

    /*
     * Create empty pipeline
     */
    player->pipeline = gst_pipeline_new ("player-pipeline");
    
    /*
     * check
     */
    if (   !udpsrc
        || !capsfilter
        || !rtp_depay
        || !dec
        || !autovideosink
        )
    {
        g_error ("failed to create gstreamer elements");
        return ret;
    }


    /*
     * Link all elements
     */
    gst_bin_add_many (GST_BIN (player->pipeline), 
                      udpsrc,
                      capsfilter,
                      rtp_depay,
                      dec,
                      autovideosink,
                      NULL);

    if (gst_element_link_many (udpsrc, 
                               capsfilter, 
                               rtp_depay, 
                               dec,
                               autovideosink, 
                               NULL) != TRUE)
    {
        g_error ("failed to link elements");
        gst_object_unref (player->pipeline);
        return ret;
    }

    /*
     * test only
     */


    GstCaps *video_caps = gst_caps_new_simple ("application/x-rtp",
        "encoding-name", G_TYPE_STRING, "JPEG",
        "payload", G_TYPE_INT, 26,
        NULL);

    g_object_set (udpsrc, "buffer-size", buffer_size, 
                          "address", "127.0.0.1",
                          "port", udp_port,
                                                   NULL);
    g_object_set (capsfilter, "caps", video_caps, NULL); 
    gst_caps_unref (video_caps);

    /*
     * Instruct the bus to emit signal
     */
    GstBus *bus = gst_element_get_bus (player->pipeline);
    gst_bus_set_sync_handler (bus, (GstBusSyncHandler) bus_sync_handler, player,
                                                                         NULL);

    player->bus_watch_id = gst_bus_add_watch (bus, bus_cb, player);
    gst_object_unref (bus);

    /*
     * ready to play
     */
    GstClock *clock = gst_pipeline_get_clock (GST_PIPELINE (player->pipeline));
    player->initial_buffer_time = gst_clock_get_time (clock);
    gst_element_set_state (player->pipeline, GST_STATE_PLAYING);
}

gboolean bus_cb (GstBus *bus, GstMessage *msg, gpointer user_data)
{
    g_info ("Got %s message", GST_MESSAGE_TYPE_NAME (msg));
    PlayerData *player = (PlayerData *) user_data;
    switch (GST_MESSAGE_TYPE (msg))
    {
        case GST_MESSAGE_ERROR:
        {
            GError *err = NULL;
            gchar *debug_info = NULL;

            gst_message_parse_error (msg, &err, &debug_info);
            g_debug ("Error received from element %s: %s", GST_OBJECT_NAME (msg->src),
                        err->message);
            g_clear_error (&err);
            g_free (debug_info);
            gst_element_set_state (player->pipeline, GST_STATE_READY);
            break;
        }
        case GST_MESSAGE_EOS:
        {
            gtk_main_quit ();
            break;
        }
        case GST_MESSAGE_STATE_CHANGED:
        {
            GstState old_state, new_state, pending_state;
            gst_message_parse_state_changed (msg, &old_state, &new_state, &pending_state);
            if (GST_MESSAGE_SRC (msg) == GST_OBJECT (player->pipeline))
            {
               player->state = new_state;
               g_debug ("new state: %s", gst_element_state_get_name (new_state));
            }
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

void release_player (PlayerData *player)
{
    if (NULL != player->video_caps)
    {
        gst_caps_unref (player->video_caps);
    }
    g_source_remove (player->bus_watch_id);
    gst_element_set_state (player->pipeline, GST_STATE_NULL);
    gst_object_unref (player->pipeline);
}

static void dummy (const gchar *log_domain,
                   GLogLevelFlags log_level,
                   const gchar *message,
                   gpointer user_data)

{
    //does nothing
    return;
}

int main (int argc, char **argv)
{
    g_log_set_handler (G_LOG_DOMAIN, G_LOG_LEVEL_MASK, dummy, NULL);
    if (argc == 2)
    {
        if (!strncmp ("-vv", argv[1], 3))
        {
            g_print ("debugging messages will be printed ...\n");
            g_log_set_handler (G_LOG_DOMAIN, G_LOG_LEVEL_DEBUG, g_log_default_handler, NULL);
            g_log_set_handler (G_LOG_DOMAIN, G_LOG_LEVEL_INFO, g_log_default_handler, NULL);
            g_log_set_handler (G_LOG_DOMAIN, G_LOG_LEVEL_MESSAGE, g_log_default_handler, NULL);
        }
        else if (!strncmp ("-v", argv[1], 2))
        {
            g_print ("messaging messages will be printed ...\n");
            g_log_set_handler (G_LOG_DOMAIN, G_LOG_LEVEL_MESSAGE, g_log_default_handler, NULL);
        }
        else
        {
            g_print ("default\n");
            g_log_set_handler (G_LOG_DOMAIN, G_LOG_LEVEL_MASK, g_log_default_handler, NULL);
        }
    }
    else
    {
        g_print ("more critical than warning\n");
        g_log_set_handler (G_LOG_DOMAIN, G_LOG_LEVEL_WARNING, g_log_default_handler, NULL);
    }



    PlayerData *player = (PlayerData *)g_malloc (sizeof (PlayerData));
    memset (player, 0, sizeof (PlayerData));
    init_player (argc, argv, player);

    create_gui (player);


    gtk_main ();
    release_player (player);

    return 0;
}
