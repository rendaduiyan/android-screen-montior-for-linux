#ifndef _ADB_CLIENT_H_
#define _ADB_CLIENT_H_

#include <glib.h>
#include <gio/gio.h>

#include "sink.h"

typedef enum {
    ADBP_UNINITIALIZED,
    ADBP_TRACK_DEVICES,            //request to get devices serials
    ADBP_TRANSPORT_DEVICE,         //request to transport android device
    ADBP_FRAMEBUFFER,              //request to get framebuffer
    ADBP_NUDGE,                    //request to next frame according to SERVICES.TXT
    ADBP_TRACK_DEVICES_SERIAL,     //requests for exceptional cases, not in SERVICES.TXT
    ADBP_IMAGE_HEADER,             
    ADBP_IMAGE_DATA,
    ADBP_ONEFRAME_DONE,            //flag for one frame, will send one byte for another
    ADBP_SIZE
} ADBP_Service;

typedef enum {
    UNINITIALIZED, 
    REQUEST_SENDED,                //send out a request and then waiting for the response
    RESPONSE_OKAY,                 //receive a response message and hosts claim OKAY
    RESPONSE_FAIL,                 //receive a response messsage and hosts claim FAIL 
                                   //or something wrong in the message
    READ_ERROR,                    //read error from io
    WRITE_ERROR,                   //write error from io
    CONNECTING_ERROR,              //failed to connection
    QUITTING,
    TASK_SIZE 
} State;

typedef struct _AdbMessage
{
    ADBP_Service req;             //a pair for req type and content
    GString *msg;
    gboolean ping_pong;
} AdbMessage;

/*
 * this is the head format for version 1
 * version 16 has a smaller header, which inludes only size, width, height
 * TODO: support version 16
 */

typedef struct _ImageHeader
{
    gsize version;
    gsize bpp;
    gsize size;
    gsize width;
    gsize height;
    gsize red_offset;
    gsize red_length;
    gsize blue_offset;
    gsize blue_length;
    gsize green_offset;
    gsize green_length;
    gsize alpha_offset;
    gsize alpha_length;
} ImageHeader;

typedef struct _MyContext
{
    GSocketClient *sc;              //socket client
    GSocketConnection *conn;        //socket connection
    GInputStream *istream;          //iostream in socket connection
    GOutputStream *ostream;
    GMainLoop *loop;                //main loop
    GIOChannel *chan;
    State state;                    //states for different steps
    GList *dev_list;                //the ability to handle more than one android device
    GAsyncQueue *queue;             //async queue to buffer messages
    gboolean socket_ready;          //flat for async socket connection
    guint source_id;                //source id socket channel io
    ADBP_Service req;               //current request
    ADBP_Service next_req;          //next request to be sent
    ImageHeader *image_header;
    guint8 *raw_image;
    gint64 start_time;              //time to get the first image buffer
    gint64 end_time;                //time to get the last image buffer
    gsize image_offset;
    gboolean fin_event;             //special event for TCP FIN
    GString *serial;
    _MyContext *next;                //link 2 contexts
    guint16 port;
    GSocketType type;
    GSocketProtocol protocol;
    SinkData *sink;
} MyContext;

/*
 * for async io, we need a reading buffer for each async read 
 * passing buffer to the callback
 */
typedef struct _SnapShot
{
    gpointer buffer;
    MyContext *ctx;
} SnapShot;

void dump_header (ImageHeader *header);

/*
 * It's more like handling a state-machine
 * state-event->next state
 */
void screenshot_routine (MyContext *ctx);

void cleanup_context (MyContext *ctx);

void cleanup_devlist (GList **list);

void my_quit (MyContext *ctx);

gboolean channel_read_cb (GIOChannel *source,
              GIOCondition condition,
              gpointer data);

void read_cb (GObject *source,
              GAsyncResult *res,
              gpointer user_data);

void connected_cb ( GObject *src_obj,
                GAsyncResult *res,
                gpointer user_data);

AdbMessage* create_adb_request (MyContext *ctx, ADBP_Service req);

void write_cb ( GObject *src_obj,
                GAsyncResult *res,
                gpointer user_data);

void send_request (MyContext *ctx, AdbMessage *msg);
void send_request_try (MyContext *ctx, AdbMessage *msg);

/*
 * entry for handling messages from socket
 * all others are internal use
 */
void handle_message (MyContext *ctx, guint8 *buf, gsize size);

/*
 * Internal use
 */
void handle_track_devices (MyContext *ctx, guint8 *buf, gsize size);
void handle_track_devices_serial (MyContext *ctx, guint8 *buf, gsize size);
void handle_transport_device (MyContext *ctx, guint8 *buf, gsize size);
void handle_framebuffer (MyContext *ctx, guint8 *buf, gsize size);
void handle_image_header (MyContext *ctx, guint8 *buf, gsize size);
void handle_image_data (MyContext *ctx, guint8 *buf, gsize size);
void handle_status_fail (MyContext *ctx, guint8 *buf, gsize size);
void handle_fail_message (guint8 *buf, gsize size);
void handle_common (MyContext *ctx, guint8 *buf, gsize size);


/*
 * utilities for handling message
 */

gboolean is_okay (guint8 *buf, gchar *ok_str);
gboolean is_fail (guint8 *buf);


void dump_buffer (guint8 *buf, gsize size, gboolean mixed = true);

void socket_close (MyContext *ctx);
void reset_socketclient (MyContext *ctx);
void create_socketclient (MyContext *ctx);

gsize get_message_len (guint8 *buf, gsize size);
gsize get_int_32 (guint8 *buf);
gsize get_int_16 (guint8 *buf);

gchar* get_state_name (State s);
gchar* get_req_name (ADBP_Service s);
gchar* get_okay_str (ADBP_Service s);
gchar* get_format_from_header (ImageHeader *header);


#endif
