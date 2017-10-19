# android-screen-montior-for-linux
Native Linux application for monitoring android device screen based on ADB protocol

## overview
This project is to implement a screen monitoring for android devices with adb-enabled. All services it uses are described in OVERVIEW.TXT and SERVICES.TXT in project ADB:
* track devices
* transport <serial>
* framebuffer

With that in mind, we can start to connect adb service (installed from android platform tools). After TCP connection is created on port 5037,those adb messages are to be sent to adb, and screen buffer begins to send back if adb respond with OKAY for its framebuffer message. Once all data for one frame is reached, it will try to forward data to a player. Here gstreamer is used to play those frame buffers since it keeps requesting screencap. Especially, the player and adb client are separate into 2 processes so that they can be deployed on different machines. Here is the data flow for the image data:
  
  
  adb -> | adb_client -> gstreamer udpsink | -> |gstreamer udpsrc -> gstreamer xoverlay|         
         
  adb -> |             asm                 | -> |                 player               |
  

## Developing environment:
 * Ubuntu 16.04 32-bit

## Dependencies:
 * glib                       (libgtk2.0-dev)
 * gtk+-2.0                   (libgtk2.0-dev)
 * gstreamer-1.0/gstvideo-1.0 (libgstreamer1.0-dev, libgstreamer1.0-dev)
 
## Build:
 * target asm:
   g++ -g -o asm adb_client.c sink.c `pkg-config --cflags --libs gstreamer-1.0 gio-2.0`
 * target player
   g++ -g -o player player.c `pkg-config --cflags --libs gstreamer-1.0 gstreamer-video-1.0 gtk+-2.0`
## Run:
 * ./player [-v|-vv]
   
   \-v: verbose
   \-vv: more details than verbose
 * ./asm [-v|-vv]
 
   \-v: verbose
   \-vv: more details than verbose
 
## IO in Async mode:
 All socket reading and writing are in aysnc mode - gio async apis are called instead of blocking ones. All callbacks are paid espeicial attention. For example, callback "channel_read_cb" g_io_add_watch will allocate a reading buffer for g_input_stream_read_async and pass buffer pointer as the parameter for the callback "read_cb". The latter will take the responsibility to free the allocated buffer when the message processing is done.
 
## GStreamer:
 Current smart phones have 1080x1920 resolution (or above) by default, in most cases we don't want to display such a high resolution. In gstreamer we need videoconvert and video scale to do that. We can display the screen either on the local machine or on a remote one (not configurable for now, minor change is needed, by setting the host for udpsrc and udpsink). So udpsink and udpsrc are introduced. More than that, we can start multiple asm instances to get the screenshot and interlace them in the player ; on the other hand, the screenshot can be multicasted.
 
## Typical CPU and memory usage:
 
 PID USER      PR  NI    VIRT    RES    SHR S  %CPU %MEM     TIME+ COMMAND                                     
 4891 lhb       20   0   33312   6080   3676 S  12.5  0.2  14:52.51 adb                                         
 9045 lhb       20   0   74684  28296  10140 S   6.2  1.1   0:31.22 asm                                         
 9028 lhb       20   0   89504  24592  21100 S   0.0  1.0   0:03.49 player
 
## Source files:
* adb_client.h
* adb_client.c

Acting as a client to adb service and interface with adb to get the framebuffer based on ADB protocol.

* sink.h
* sink.c

Gstreamer pipeline to convert the image data and send to remote player by use of udpsink.

* player.h
* player.c

GStreamer pipeline to play the screenshots continuously by use of udpsrc.

## screenshot
![Alt text](multicast_screenshot.jpeg?raw=true "multicast example")


 
