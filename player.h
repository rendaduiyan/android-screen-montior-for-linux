#ifndef _PLAYER_H_
#define _PLAYER_H_


typedef struct _PlayerData
{
    GstElement *pipeline;
    GstClockTime initial_buffer_time;
    GMainLoop *loop;
    guint bus_watch_id;
    GstState state;
    GstCaps *video_caps;
    guintptr video_window_handle;
} PlayerData;

GstBusSyncReply bus_sync_handler (GstBus * bus, GstMessage * message, PlayerData *player);

void close_cb (GtkButton *button, PlayerData *player);

void delete_event_cb (GtkWidget *widget, GdkEvent *event, PlayerData *player);

void realize_cb (GtkWidget *widget, PlayerData *player);

gboolean draw_cb (GtkWidget *widget, GdkEventExpose *event, PlayerData *player) ;

void create_gui (PlayerData *player);

gboolean init_player (int argc, char **argv, PlayerData *player);

gboolean bus_cb (GstBus *bus, GstMessage *msg, gpointer user_data);

void release_player (PlayerData *player);


#endif
