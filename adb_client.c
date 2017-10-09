/*
 * adb_client.c, an adb client in glib for Linux PC to talk with adb service
 * 
 * Copyright (c) 2017, haibolei <duiyanrenda@gmail.com>
 * 
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib-unix.h>
#include <sys/socket.h>
#include <gst/gst.h>

#include "adb_client.h"

//#define G_LOG_DOMAIN    ((gchar*) 0)

MyContext *my_ctx = NULL;
static guint16 ADB_PORT = 5037;
static gchar *default_format = g_strdup ("RGBx");

void cleanup_context (MyContext *ctx)
{
    //clean up devlist
    if (NULL != ctx->dev_list)
    {
        cleanup_devlist (&(ctx->dev_list));
    }
    if (NULL != ctx->next)
    {
        cleanup_context (ctx->next);
    }
    //clean write queue
    if (NULL != ctx->image_header)
    {
        g_free (ctx->image_header);
    }
    if (NULL != ctx->sink)
    {
        release_sink (ctx->sink);
        g_free (ctx->sink);
    }
}

void socket_close (MyContext *ctx)
{
    if (ctx->chan && ctx->sc)
    {
        if (ctx->socket_ready)
        {
            g_source_remove (ctx->source_id);
        }

        GError *err = NULL;
        //close streams
        if (FALSE == g_io_stream_close (G_IO_STREAM(ctx->conn), NULL, &err))
        {
             g_debug ("failed to close stream %p, %u : %s\n", ctx->conn, err->code, err->message);
             g_clear_error (&err);
        }

        g_io_channel_unref (ctx->chan);
        g_object_unref (ctx->sc);
    }
}


void screenshot_routine (MyContext *ctx)
{
    g_debug ("current ctx: %p, source_id: %u", ctx, ctx->source_id);
    switch (ctx->state)
    {
        case UNINITIALIZED:
        {
            AdbMessage *track_devices = create_adb_request (ctx, ADBP_TRACK_DEVICES);
            send_request_try (ctx, track_devices);
            break;
        }
        case REQUEST_SENDED:
        {
            g_debug ("waiting for response\n");
            if (ctx->fin_event)
            {
                switch (ctx->req)
                {
                    case ADBP_TRACK_DEVICES:
                    case ADBP_TRACK_DEVICES_SERIAL:
                    {
                        if (ctx->req != ADBP_TRACK_DEVICES)
                        {
                            AdbMessage *track_devices = create_adb_request (ctx, ADBP_TRACK_DEVICES);
                            send_request_try (ctx, track_devices);
                        }
                        break;
                    }
                    case ADBP_TRANSPORT_DEVICE:
                    {
                        AdbMessage *transport_device  = create_adb_request (ctx, 
                                                         ADBP_TRANSPORT_DEVICE);
                        send_request_try (ctx, transport_device);
                        ctx->next_req = ADBP_FRAMEBUFFER;
                        break;
                    }
                    default:
                    {
                        g_debug ("unhandled request:%s", get_req_name (ctx->req));
                        break;
                    }
                }
            }
            break;
        }
        case RESPONSE_OKAY:
        {
            /*
             * track-devices command seems to close the socket immediately
             * which is different to SERVICE.txt
             * the reason is unknown
             * can't find the document for the reason why adb doesn't keep the connection
             * reset connection as a workaround
             * after transporting device, it seems we need to keep the connection
             */
            g_debug ("got response from host for ");
            switch (ctx->req)
            {
                case ADBP_TRACK_DEVICES_SERIAL:
                {
                    g_debug ("track devices\n");
                    reset_socketclient (ctx);
                    GList *head = g_list_first (ctx->dev_list);
                    if (NULL != head)
                    {
                        /*
                         * don't know if it's useful to give out a device list
                         * current case is to monitor the first one (assumed only one 
                         * device connected)
                         * We need to change the logic to wait the choice from GUI if 
                         * that's the case
                         */
                        ctx->serial = (GString *) head->data;
                        AdbMessage *transport_device  = create_adb_request (ctx, 
                                                         ADBP_TRANSPORT_DEVICE);
                        send_request_try (ctx, transport_device);
                        ctx->next_req = ADBP_FRAMEBUFFER;
                    }
                    else
                    {
                        g_error ("empty device list");
                    }
                    break;
                }

                case ADBP_TRANSPORT_DEVICE:
                {
                    /*
                     * transport device command is used quite often
                     * we need its next_msg to tell what to do next
                     * if necessary, we need to check if its next_msg is NULL
                     * 
                     */
                    g_debug ("transport device\n");
                    AdbMessage *next_msg = create_adb_request (ctx,
                                                     ctx->next_req);
                    
                    send_request_try (ctx, next_msg);
                    break;
                }
                case ADBP_FRAMEBUFFER:
                {
                    /*
                     * just for tracking, no action required
                     */
                    g_message ("frame buffer");
                    
                    break;
                }
                case ADBP_IMAGE_HEADER:
                {
                    /*
                     * just for tracking, no action required
                     */
                    g_debug ("image header\n");
                    break;
                }
                case ADBP_IMAGE_DATA:
                {
                    /*
                     * just for tracking, no action required
                     */
                    g_debug ("image data\n");
                    break;
                }
                case ADBP_ONEFRAME_DONE:
                {
                    /*
                     * till now, one frame is got
                     * start the next one
                     * since we have the device list for now
                     * we don't need to start it over
                     * we can request frame buffer after transport
                     */
                    g_debug ("one frame");
                    AdbMessage *transport_device  = create_adb_request (ctx, 
                                                         ADBP_TRANSPORT_DEVICE);
                    send_request_try (ctx, transport_device);
                    ctx->next_req = ADBP_FRAMEBUFFER;

                    break;
                }
                default:
                {
                    g_debug ("TODO:\n");
                    break;
                }
            }
            break;
        } 
        case RESPONSE_FAIL:
        case READ_ERROR:
        case WRITE_ERROR:
        {
            /*r/w error, for now just quit */
            g_debug ("about to quit ...");
            my_quit (ctx);
            break;
        }
        case QUITTING:
        {
            /*
             * reserved for future purpose
             * in some cases, we may get some control message from peers
             * to notify the quit
             */
            g_debug ("normal quit ...");
            break;
        }
        default:
        {
            g_debug ("error found : %d\n", ctx->state);
            break;
        }
    }
}

void my_quit (MyContext *ctx)
{
    if (g_main_loop_is_running (ctx->loop))
    {
        g_main_loop_quit (ctx->loop);
    }
}

gboolean is_okay (guint8 *buf, gchar *ok_str)
{
    /*
     * for now, the ok_str is always OKAY
     * but we don't want to hard-code it
     */
    gboolean isok = FALSE;
    gsize len = strlen (ok_str);
    gsize i = 0;
    while (i < len && buf[i] == ok_str[i])
    {
        i ++;
    }
    if (i == len)
    {
        isok = TRUE;
    }
    
    return isok;
}

gboolean is_fail (guint8 *buf)
{
    /*
     * we don't find cases with failed string other than FAIL
     * it's better to change like is_okay
     */ 
    return buf[0] == 'F' && buf[1] == 'A' && buf[2] == 'I' && buf[3] == 'L';
}

void dump_buffer (guint8 *buf, gsize size, gboolean mixed)
{
    gsize new_line_count = 16;
    GString *log_buf = g_string_new (NULL);
    if (mixed)
    {
        new_line_count /= 2;
        for (gsize i = 0; i < size; i ++)
        {
            if (i && 0 == i % new_line_count)
            {
                g_string_append_printf (log_buf, "\n");
            }
            g_string_append_printf (log_buf, "%02X(", buf[i]);
            if (g_ascii_isalpha (buf[i]) || g_ascii_isdigit(buf[i]))
            {
                g_string_append_printf (log_buf, "%c) ", buf[i]);
            }
            else if (buf[i] == '\t')
            {
                g_string_append_printf (log_buf, "%s) ", "\\t");
            }
            else if (buf[i] == '\n')
            {
                g_string_append_printf (log_buf, "%s) ", "\\n");
            }
            else
            {
                g_string_append_printf (log_buf, ".) ");
            }
        }
    }
    else
    {
        for (gsize i = 0; i < size; i ++)
        {
            if (i && 0 == i % new_line_count)
            {
                g_string_append_printf (log_buf, "\n");
            }
            g_string_append_printf (log_buf, "%02X ", buf[i]);
        }
    }
    g_debug ("%s", log_buf->str);
    g_string_free (log_buf, TRUE);
}

gsize get_int_16 (guint8 *buf)
{
    //little endian
    return (buf[1] << 8) | buf[0];
}

gsize get_int_32 (guint8 *buf)
{
    //little endian
    return (buf[3] << 24) | (buf[2] << 16) | (buf[1] << 8) | buf[0];
    //big endian
    //return (buf[0] << 24) | (buf[1] << 16) | (buf[2] << 8) | buf[3];
}

gsize get_message_len (guint8 *buf, gsize size)
{
    guint8 c = 0;
    gsize n = 0;
    while (size-- > 0)
    {
        c = *buf ++;
        if (c >= '0' && c <= '9')
        {
            c -= '0';
        }
        else if (c >= 'a' && c <= 'f')
        {
            c = c - 'a' + 10;
        }
        else if (c >= 'A' && c <= 'F')
        {
            c = c - 'A' + 10;
        }
        else
        {
            return -1;
        }
        n = (n << 4) | c;
    }
    g_debug ("message len: %d\n", n);
    return n;
}

guint8* handle_resp_status (MyContext *ctx, guint8 *buf, gsize *size)
{
    static gsize status_size = 4;
    ctx->state = RESPONSE_FAIL;
    if (*size >= status_size && is_okay (buf, get_okay_str (ctx->req)))
    {
         ctx->state = RESPONSE_OKAY;
    }
    *size -= status_size;
    return buf + status_size;
}

guint8* handle_resp_len (MyContext *ctx, guint8 *buf, gsize *size, gsize *len)
{
    static gsize len_size = 4;
    dump_buffer (buf, len_size);
    if (*size >= len_size)
    {
        *len = get_message_len (buf, len_size);
    }
    else
    {
        *len = 0;
    }
    *size -= len_size;
    return buf + len_size;
}

void handle_fail_message (guint8 *buf, gsize size)
{
    GString *message = g_string_new (NULL);
    for (gsize i = 0; i < size; i ++)
    {
        g_string_append_c (message, buf[i]);
    }
    g_debug ("error message:%s\n", message->str);
    g_string_free (message, TRUE);
}

void cleanup_devlist (GList **list)
{
    GList *l = *list;
    while (l != NULL)
    {
        GList *next = l->next;
        g_string_free ((GString *)l->data, TRUE);
        *list = g_list_delete_link (*list, l);
        l = next;
    }
}

void handle_track_devices_serial (MyContext *ctx, guint8 *buf, gsize size)
{
    /*
     * typically, device serial is got in the first response message 
     * for "track-devices"
     * however, we found the case that "OKAY" and serial are separated into 2 messages
     * 
     * if this handler is called,
     * OKAY is always got previously
     */
    gsize bytes_left = size;
    gsize msg_len = 0;
    guint8 *pbuf = handle_resp_len (ctx, buf, &bytes_left, &msg_len);

    if (msg_len > 0 && bytes_left >= msg_len)
    {
            //clear the list
        cleanup_devlist (&(ctx->dev_list));

        GString *a_dev = g_string_new (NULL);
        for (gsize i = 0; i < msg_len; i ++)
        {
            if (pbuf[i] != '\n')
            {
                g_string_append_c (a_dev, pbuf[i]);
            }
            else
            {
                int j = 0;
                for (j = 0; j < a_dev->len; j ++)
                {
                    if (a_dev->str[j] == '\t')
                    {
                        break;
                    }
                }
                if (j < a_dev->len && j > 0)
                {
                    a_dev = g_string_erase (a_dev, j, a_dev->len - j);
                }
                g_debug ("device serail:%s\n", a_dev->str);
                ctx->dev_list = g_list_append (ctx->dev_list, a_dev);
                if (i != msg_len - 1)
                {
                    a_dev = g_string_new (NULL);
                }
            }
        }
        ctx->req = ADBP_TRACK_DEVICES_SERIAL;
        ctx->state = RESPONSE_OKAY;
    }
    else
    {
        ctx->state = RESPONSE_FAIL;
    }
    screenshot_routine (ctx);
}

void handle_track_devices (MyContext *ctx, guint8 *buf, gsize size)
{
    dump_buffer (buf, size);

    gsize bytes_left = size;
    guint8 *pbuf = handle_resp_status (ctx, buf, &bytes_left);
    if (bytes_left == 0)
    {
        //only status message
        if (ctx->state == RESPONSE_OKAY)
        {
            g_debug ("waiting device serial...");
            ctx->state = REQUEST_SENDED;
            ctx->req = ADBP_TRACK_DEVICES_SERIAL;
        }
        screenshot_routine (ctx);
    }
    else
    {
        handle_track_devices_serial (ctx, pbuf, bytes_left);
    }
}

void handle_transport_device (MyContext *ctx, guint8 *buf, gsize size)
{
    dump_buffer (buf, size);
    gsize bytes_left = size;
    gsize msg_len = 0;
    guint8 *pbuf = handle_resp_status (ctx, buf, &bytes_left);
    if (ctx->state == RESPONSE_FAIL)
    {
        gsize msg_len = 0;
        pbuf = handle_resp_len (ctx, pbuf, &bytes_left, &msg_len);
        handle_fail_message (pbuf, msg_len);
    }
    screenshot_routine (ctx);
}

void dump_header (ImageHeader *header)
{
    g_message ("%u:%u:%u:%u:%u:%u:%u:%u:%u:%u:%u:%u:%u\n",
        header->version,
        header->bpp,
        header->size,
        header->width,
        header->height,
        header->red_offset,
        header->red_length,
        header->blue_offset,
        header->blue_length,
        header->green_offset,
        header->green_length,
        header->alpha_offset,
        header->alpha_length);

}


void handle_image_data (MyContext *ctx, guint8 *buf, gsize size)
{
    //dump_buffer (buf, 64, FALSE);
    g_info ("continuing copy image data... %u : %u\n", 
        ctx->image_offset, ctx->image_header->size);
    ctx->state = RESPONSE_OKAY;
    memcpy (&ctx->raw_image[ctx->image_offset], buf, size);
    ctx->image_offset += size;
    if (ctx->image_offset < ctx->image_header->size)
    {
        ctx->state = REQUEST_SENDED; //waiting next one
    }
    else
    {
        ctx->end_time = g_get_real_time () / 1000;
        g_message ("it cost %03lld (ms) to get one frame\n", 
            ctx->end_time - ctx->start_time);
        
        /*
         * we don't need to free the raw_image 
         * gstreamer pipeline should do it
         */
        push_data (ctx->sink, ctx->raw_image, ctx->image_header->size);
        /*
         * free the header data
         * assumed header info will be sent again in the first response message 
         * of frame buffer request
         */
        g_free (ctx->image_header);
        ctx->image_header = NULL;
        ctx->req = ADBP_ONEFRAME_DONE;
        /*
         * Since adb will close the connection after one frame is sended
         * We will handle it in Fin event.
         * Do nothing here.
         */
    }
    screenshot_routine (ctx);
}

gchar* get_format_from_header (ImageHeader *header)
{
   /*
    * default format
    */
    gchar *format = default_format;
    /*
     * only version for now
     */
    
    if (header->version == 1)
    {
        switch (header->bpp)
        {
            case 32:
            {
                /*
                 * RGBA or RGBx or BGRA
                 * can't tell RGBA or RGBx from header
                 * using RGBA for each case
                 */
                if (header->blue_offset == 0)
                {
                    format = g_strdup ("BGRA");
                }
                break;
            }
            case 24:
            {
                /*
                 * RGB
                 */
                format = g_strdup ("RGB");
                break;
            }
            case 16:
            {
                format = g_strdup ("RGB16");
                break;
            }
        }
    }
    return format;
}

void handle_image_header (MyContext *ctx, guint8 *buf, gsize size)
{
     ctx->state = RESPONSE_FAIL;
    /*
     * Referring to framebuffer_service.c to understand how the image data is provided
     */
    dump_buffer (buf, sizeof (ImageHeader), FALSE);
    if (size >= sizeof (ImageHeader))
    {
        ctx->image_header = (ImageHeader *) g_malloc (sizeof(ImageHeader));
        guint8 *pbuf = buf;
        ctx->image_header->version = get_int_32 (pbuf); 
        pbuf += 4;
        ctx->image_header->bpp = get_int_32 (pbuf); 
        pbuf += 4;
        ctx->image_header->size = get_int_32 (pbuf); 
        pbuf += 4;
        ctx->image_header->width = get_int_32 (pbuf); 
        pbuf += 4;
        ctx->image_header->height = get_int_32 (pbuf); 
        pbuf += 4;
        ctx->image_header->red_offset = get_int_32 (pbuf); 
        pbuf += 4;
        ctx->image_header->red_length = get_int_32 (pbuf); 
        pbuf += 4;
        ctx->image_header->green_offset = get_int_32 (pbuf); 
        pbuf += 4;
        ctx->image_header->green_length = get_int_32 (pbuf); 
        pbuf += 4;
        ctx->image_header->blue_offset = get_int_32 (pbuf); 
        pbuf += 4;
        ctx->image_header->blue_length = get_int_32 (pbuf); 
        pbuf += 4;
        ctx->image_header->alpha_offset = get_int_32 (pbuf); 
        pbuf += 4;
        ctx->image_header->alpha_length = get_int_32 (pbuf); 
        dump_header (ctx->image_header);
        pbuf += 4;

        /*
         * create caps from header
         */
        GstCaps *video_caps = gst_caps_new_simple ("video/x-raw",
            "format", G_TYPE_STRING, get_format_from_header (ctx->image_header), 
            "width", G_TYPE_INT, ctx->image_header->width,
            "height", G_TYPE_INT, ctx->image_header->height,
            NULL);

        GstCaps *scale_caps = gst_caps_new_simple ("video/x-raw",
            "format", G_TYPE_STRING, "I420",                   //for udp forwarding
            "width", G_TYPE_INT, ctx->image_header->width / 2,
            "height", G_TYPE_INT, 
                gst_util_uint64_scale_int_round (ctx->image_header->width / 2, 
                ctx->image_header->height, ctx->image_header->width),
            "framerate", GST_TYPE_FRACTION, 30, 1,
            NULL);
        g_object_set (ctx->sink->appsrc, "caps", video_caps, NULL);
        g_object_set (ctx->sink->caps_filter, "caps", scale_caps, NULL);

        gst_caps_unref (video_caps);
        gst_caps_unref (scale_caps);

        ctx->image_offset = size - sizeof (ImageHeader); //raw data after the header
        ctx->raw_image = (guint8 *)g_malloc (ctx->image_header->size);
        if (ctx->image_offset > 0)
        {
           memcpy (ctx->raw_image, pbuf, ctx->image_offset);
        }
        ctx->state = REQUEST_SENDED;
        ctx->req = ADBP_IMAGE_DATA;
    }
    screenshot_routine (ctx);
}

void handle_status_fail (MyContext *ctx, guint8 *buf, gsize size)
{
    gsize bytes_left = size;
    gsize msg_len = 0;
    guint8 *pbuf = buf;

    if (bytes_left > 0)
    {
        /*
         * Error message attached
         */
        pbuf = handle_resp_len (ctx, pbuf, &bytes_left, &msg_len);
        if (msg_len > 0 && bytes_left >= msg_len)
        {
            handle_fail_message (pbuf, msg_len);
        }
        else
        {
            g_debug ("unexpected message");
        }
    }
    else
    {
        g_debug ("just FAIL");
    }
}

void handle_framebuffer (MyContext *ctx, guint8 *buf, gsize size)
{
    dump_buffer (buf, (size < 52 ? size : 52));

    gsize bytes_left = size;
    gsize msg_len = 0;
    guint8 *pbuf = handle_resp_status (ctx, buf, &bytes_left);
    if (ctx->state == RESPONSE_OKAY)
    {
        /* 
         * trigger for next frame
         * no response expected
         */
        AdbMessage *screen_nudge = create_adb_request (ctx, ADBP_NUDGE);
        send_request_try (ctx, screen_nudge);

        if (bytes_left > 0)
        {
            /*
             *OKAY + header + image data
             */
            handle_image_header (ctx, pbuf, bytes_left);
        }
        else
        {
            /*
             * OKAY
             * it is a litte bit different compared to the SERVICE.TXT - no header and image data followed by OKAY. Instead a new message is sent.
             */
            ctx->state = REQUEST_SENDED;
            ctx->req = ADBP_IMAGE_HEADER;
        }
    }
    else
    {
        /*
         * FAIL
         */
        handle_status_fail (ctx, pbuf, bytes_left);
    }

    screenshot_routine (ctx);
}

void handle_message (MyContext *ctx, guint8 *buf, gsize size)
{
    switch (ctx->req)
    {
        case ADBP_TRACK_DEVICES:
        {
            g_debug ("track devices resp\n");
            handle_track_devices (ctx, buf, size);
            break;
        }
        case ADBP_TRACK_DEVICES_SERIAL:
        {
            g_debug ("device serial resp");
            handle_track_devices_serial (ctx, buf, size);
            break;
        }
        case ADBP_TRANSPORT_DEVICE:
        {
            g_debug ("transport device resp\n");
            handle_transport_device (ctx, buf, size);
            break;
        }
        case ADBP_FRAMEBUFFER:
        {
            g_debug ("framebuffer resp");
            handle_framebuffer (ctx, buf, size);
            break;
        }
        case ADBP_IMAGE_HEADER:
        {
            g_debug ("image header after frame buffer OKAY message\n");
            ctx->start_time = g_get_real_time () / 1000;
            handle_image_header (ctx, buf, size);
            break;
        }
        case ADBP_IMAGE_DATA:
        {
            g_debug ("image data after first buffer\n");
            handle_image_data (ctx, buf, size);
            break;
        }
        default:
        {
            g_debug ("unexpected: %s", get_req_name (ctx->req));
            break;
        }
    }
}

/*
 * handler for cases that need no special action
 */

void handle_common (MyContext *ctx, guint8 *buf, gsize size)
{
    dump_buffer (buf, (size < 52 ? size : 52));
    gsize bytes_left = size;
    gsize msg_len = 0;
    gboolean a_fail = is_fail (buf);
    guint8 *pbuf = handle_resp_status (ctx, buf, &bytes_left);
    if (ctx->state == RESPONSE_OKAY)
    {
        /*
         * OKAY is expected
         */
        if (bytes_left > 0)
        {
            g_debug ("what's the data left?");
        }
    }
    else if (a_fail)
    {
        handle_status_fail (ctx, pbuf, bytes_left);
    }
    else
    {
        handle_fail_message (buf, size);
        /*
         * ignore this message since it's NOT a failure
         */
       // ctx->state = RESPONSE_OKAY;
    }
    screenshot_routine (ctx);
}

gchar* get_state_name (State s)
{
    /*
     * update according to header file
     */
    static gchar *state_str[] = 
    {
        g_strdup ("UNINITIALIZED"),
        g_strdup ("REQUEST_SENDED"),
        g_strdup ("RESPONSE_OKAY"),
        g_strdup ("RESPONSE_FAIL"),
        g_strdup ("READ_ERROR"),
        g_strdup ("WRITE_ERROR"),
        g_strdup ("CONNECTING_ERROR"),
        g_strdup ("QUITTING"),
        g_strdup ("TASK_SIZE")
    };
    g_debug ("state: %s\n", state_str[s]);
    return state_str[s];
}

gchar* get_okay_str (ADBP_Service s)
{
    /*
     * update according to header file
     */
    static gchar *request_name[] = 
    {
        g_strdup ("NA"),
        g_strdup ("OKAY"),
        g_strdup ("OKAY"),
        g_strdup ("OKAY"),
        g_strdup ("NA"),
        g_strdup ("NA"),
        g_strdup ("NA"),
        g_strdup ("NA"),
        g_strdup ("NA"),
        g_strdup ("NA")
     };
     return request_name[s];
}

gchar* get_req_name (ADBP_Service s)
{
    static gchar *request_name[] = 
    {
        g_strdup ("ADBP_UNINITIALIZED"),
        g_strdup ("host:track_devices"),
        g_strdup ("host:transport_device"),
        g_strdup ("framebuffer"),
        g_strdup ("nudge"),
        g_strdup ("host:track_devices_serial"),
        g_strdup ("image_header"),
        g_strdup ("image_data"),
        g_strdup ("ADBP_ONEFRAME_DONE"),
        g_strdup ("ADBP_SIZE")
    };
    //g_debug ("sender: %d:%d, %s", s, ADBP_SIZE, request_name[s]);
    return request_name[s];
}

gboolean channel_read_cb (GIOChannel *source,
              GIOCondition condition,
              gpointer data)
{
    MyContext *ctx = (MyContext *) data;
    g_debug ("channel_read_cb called for context: %p", ctx);
    gsize bytes_in_buf = g_socket_get_available_bytes (g_socket_connection_get_socket (ctx->conn));
    if (bytes_in_buf == -1)
    {
        g_debug ("failed to get next buffer info, ignore\n");
    }
    else if (bytes_in_buf  == 0)
    {
        g_debug ("got FIN from peer\n");
        reset_socketclient (ctx);
        ctx->fin_event = TRUE;
    }
    else
    {
        SnapShot *data = (SnapShot *) g_malloc (sizeof (SnapShot));
        data->buffer = g_malloc (bytes_in_buf);
        data->ctx = ctx;
        memset (data->buffer, 0, bytes_in_buf);
        g_input_stream_read_async (ctx->istream,
                                   data->buffer,
                                   bytes_in_buf,
                                   G_PRIORITY_DEFAULT,
                                   NULL,
                                   read_cb,
                                   data);
        g_debug ("async read triggered: %u, %p\n", bytes_in_buf, data->buffer);
    }
    return TRUE;
    //return FALSE; // the function should return FALSE if the event source should be removed
}


void read_cb (GObject *source,
              GAsyncResult *res,
              gpointer user_data)
{
    SnapShot *data = (SnapShot *) user_data;
    guint8 *read_buf = (guint8 *) data->buffer;
    g_debug ("read_cb called for buffer ... %p\n", read_buf);
    MyContext *ctx = data->ctx;
    GError *err = NULL;
    gssize bytes_read = g_input_stream_read_finish (ctx->istream,
                                                    res,
                                                    &err);
    if (0 < bytes_read)
    {
        g_debug ("%u bytes received\n", bytes_read);
        handle_message (ctx, read_buf, bytes_read);
    }
    g_free (read_buf);
    g_free (data);
}


void connected_cb ( GObject *src_obj,
                GAsyncResult *res,
                gpointer user_data)
{
    static gint rx_size = 512 * 1024;
    MyContext *ctx = (MyContext *)user_data;
    g_debug ("connected_cb called for context ... %p", ctx);
    GError *err = NULL;
    ctx->conn = g_socket_client_connect_finish (ctx->sc, res, &err);
    if (NULL != ctx->conn)
    {
        ctx->socket_ready = TRUE;
        ctx->istream = g_io_stream_get_input_stream (G_IO_STREAM (ctx->conn));
        ctx->ostream = g_io_stream_get_output_stream (G_IO_STREAM (ctx->conn));
        GSocket *s = g_socket_connection_get_socket (ctx->conn);
        int fd = g_socket_get_fd (s);
        g_debug ("connection is created, socket:%d", fd);
        ctx->chan = g_io_channel_unix_new (fd);
        GError *err = NULL;
        if (!g_socket_set_option (s, SOL_SOCKET, SO_RCVBUF, rx_size, &err))
        {
             g_debug ("failed to set rx buffer size %d:%s\n", err->code, err->message);
             g_clear_error (&err);
        }
        g_socket_set_blocking (s, FALSE);
        g_socket_set_keepalive (s, TRUE);
        ctx->source_id = g_io_add_watch (ctx->chan, G_IO_IN, channel_read_cb, ctx);
        
        if (NULL != ctx->queue)
        {
            gboolean already_in_queue = FALSE;
            AdbMessage *msg = NULL;
            while ((msg = (AdbMessage *)g_async_queue_try_pop (ctx->queue)) != NULL)
            {
                if (msg->req == ADBP_TRACK_DEVICES
                    || msg->req == ADBP_TRACK_DEVICES_SERIAL
                    || msg->req == ADBP_TRANSPORT_DEVICE)
                {
                    already_in_queue = TRUE;
                }
                send_request (ctx, msg);
            }
            if (ctx->fin_event && !already_in_queue)
            {
                screenshot_routine (ctx);
            }
        }
        /*
         * FIN is handled
         */
        ctx->fin_event = FALSE;
    }
    else
    {
         ctx->state = CONNECTING_ERROR;
         if (NULL != err)
         { 
             g_debug ("failed to connect host %d:%s\n", err->code, err->message);
             g_clear_error (&err);
         }
    }
}


AdbMessage* create_adb_request (MyContext *ctx, ADBP_Service req)
{
    static gchar *na = g_strdup ("NA");
    static gchar *prefix[] = {
        na,
        g_strdup ("host:track-devices"),
        g_strdup ("host:transport:%s"),
        g_strdup ("framebuffer:"),
        g_strdup ("0"),
        na,
        na,
        na,
        na,
        na,
        na
        };
    AdbMessage *msg = (AdbMessage *) g_malloc (sizeof (AdbMessage));
    msg->req = req;
    msg->msg = g_string_new (NULL);
    msg->ping_pong = TRUE;
    switch (req)
    {
        case ADBP_TRANSPORT_DEVICE:
        {
            GString *temp = g_string_new (NULL);
            g_string_printf (temp, prefix[req], ctx->serial->str);
            g_string_printf (msg->msg, "%04X%s", temp->len, temp->str);
            g_string_free (temp, TRUE);
            break;
        }
        case ADBP_NUDGE:
        {
            g_string_printf (msg->msg, "%s", prefix[req]);
            msg->ping_pong = FALSE;
            break;
        }
        default:
        {
            g_string_printf (msg->msg, "%04X%s", strlen (prefix[req]), prefix[req]);
            break;
        }
    }
    g_debug ("adb_request string [%s], length:%u \n", msg->msg->str, msg->msg->len);
    return msg;
}


void write_cb ( GObject *src_obj,
                GAsyncResult *res,
                gpointer user_data)
{
    /*
     * in async mode, this callback will do nothing unless errors occur
     * it just cleans up the message buffer
     */
    SnapShot *data = (SnapShot *) user_data;
    MyContext *ctx = data->ctx;
    AdbMessage *msg = (AdbMessage *) data->buffer;
    GError *err = NULL;
    gssize written = g_output_stream_write_finish (ctx->ostream, res, &err);
    if (written > 1)
    {
         g_debug ("succeed to write %u bytes\n", written);
    }
    else if (written == 1)
    {
         g_message ("nudge message sent");
    }
    else
    {
         ctx->state = WRITE_ERROR;
         g_debug ("failed to write %d:%s\n", err->code, err->message);
         g_clear_error (&err);
         screenshot_routine (ctx);
    }
    if (msg != NULL)
    {
        g_debug ("To free %p\n", msg);
        g_string_free (msg->msg, TRUE);
        g_free (msg);
    }
    g_free (data);
}


void send_request_try (MyContext *ctx, AdbMessage *msg)
{
    if (ctx->socket_ready)
    {
         send_request (ctx, msg);
    }
    else
    {
         g_debug ("push into output queue\n");
         g_async_queue_push (ctx->queue, msg);
    }
}

void send_request (MyContext *ctx, AdbMessage *msg)
{
    g_debug ("message %p : %s, [%s], %u\n", msg, get_req_name (msg->req), 
                                           msg->msg->str, msg->msg->len);

    /*
     * only this type of messages have impact on the context
     */
    if (msg->ping_pong)
    {
        ctx->req = msg->req;
    }
    SnapShot *data = (SnapShot *) g_malloc (sizeof (SnapShot));
    data->buffer = msg;
    data->ctx = ctx;
    ctx->state = REQUEST_SENDED;
    g_output_stream_write_async (ctx->ostream, 
                                 msg->msg->str, 
                                 msg->msg->len, 
                                 G_PRIORITY_DEFAULT,
                                 NULL, 
                                 write_cb,
                                 data);
}



gboolean on_term (gpointer user_data)
{
    g_debug ("ctrl + c got\n");
    MyContext *ctx = (MyContext *) user_data;
    my_quit (ctx);
}

void create_socketclient (MyContext *ctx)
{
	static gchar adb_host[] = "127.0.0.1";
    ctx->socket_ready = FALSE;

    g_debug ("connecting to port...%u", ctx->port);
	ctx->sc = g_socket_client_new ();
    g_socket_client_set_socket_type (ctx->sc, ctx->type);
    g_socket_client_set_protocol (ctx->sc, ctx->protocol);
	g_socket_client_set_family (ctx->sc, G_SOCKET_FAMILY_IPV4);
    g_socket_client_connect_to_host_async (
                                           ctx->sc, adb_host, ctx->port, 
                                           NULL, /*cancellable*/
                                           connected_cb,
                                           ctx  /*user_data*/
                                          );
    
}

void reset_socketclient (MyContext *ctx)
{
    socket_close (ctx);
    create_socketclient (ctx);
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


    my_ctx = (MyContext *)g_malloc (sizeof (MyContext));
    memset (my_ctx, 0, sizeof (MyContext));
    my_ctx->type = G_SOCKET_TYPE_STREAM;
    my_ctx->protocol = G_SOCKET_PROTOCOL_TCP;
    my_ctx->port = ADB_PORT;
    my_ctx->state = UNINITIALIZED;
    my_ctx->sink = (SinkData *) g_malloc (sizeof (SinkData));
    if (!init_sink (argc, argv, my_ctx->sink))
    {
        g_free (my_ctx->sink);
        g_free (my_ctx);
        g_error ("failed to initialize sink");
        return -1;
    }

    g_unix_signal_add(SIGTERM, on_term, my_ctx);
    g_unix_signal_add(SIGINT, on_term, my_ctx);
    g_unix_signal_add(SIGHUP, on_term, my_ctx);

    my_ctx->queue = g_async_queue_new ();
    create_socketclient (my_ctx);

    /*
     * prepare first message to trigger the state machine
     */

    screenshot_routine (my_ctx);

    my_ctx->loop = g_main_loop_new (NULL, FALSE);
    my_ctx->sink->loop = my_ctx->loop;
    g_main_loop_run (my_ctx->loop);
   
    g_debug ("main loop quit, clear up\n");

    cleanup_context (my_ctx);
    socket_close (my_ctx);
    if (NULL != my_ctx->next)
    {
        socket_close (my_ctx->next);
        g_free (my_ctx->next);
    }
    g_async_queue_unref (my_ctx->queue);
    g_main_loop_unref (my_ctx->loop);
    g_free (my_ctx);

	return 0;
}
