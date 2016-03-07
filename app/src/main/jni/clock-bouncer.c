/*
 * Copyright (C) 2016 Jan Schmidt <thaytan@noraisin.net>
 *               2016 Arun Raghavan <arun@centricular.com>
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

#ifdef CONFIG_H
#include "config.h"
#endif

#ifdef __ANDROID__
#include <jni.h>
#include <android/log.h>
#endif

#include <glib.h>
#include <gio/gio.h>
#include <gio/gio.h>
#include <gst/net/gstnettimepacket.h>

#ifdef G_OS_UNIX
#include <glib-unix.h>
#include <sys/wait.h>
#endif

typedef struct PortInfo
{
  GSocket *socket; /* socket we used to send packet to the master */
  GSocketAddress *src_address; /* Where to return the packet */
  GInetAddress *inet_addr; /* The client's inet_addr, from src_address */
  gint port; /* The client's port, from src_address */
} PortInfo;

GMainLoop *loop;
GSocket *masterSocket;
GSocketAddress *serverAddress;
GList *ports = NULL;
#ifdef __ANDROID__
GThread *thread;
#endif

static void listen_on_socket(gint listenPort, GSourceFunc handler, gpointer user_data);
static void send_packet_to_master(GSocketAddress *src_address, gchar *buf);

#ifdef G_OS_UNIX
static gboolean
intr_handler (G_GNUC_UNUSED gpointer user_data)
{
  g_print("Exiting.\n");
  g_main_loop_quit(loop);
  return FALSE;
}
#endif

static gboolean
receive_clock_packet(GSocket *socket, G_GNUC_UNUSED GIOCondition condition,
    gpointer user_data)
{
  gchar buffer[GST_NET_TIME_PACKET_SIZE];
  GSocketAddress *src_address;
  gssize ret;

  g_print ("Got a packet");

  ret = g_socket_receive_from (socket, &src_address, buffer,
            GST_NET_TIME_PACKET_SIZE, NULL, NULL);
  if (ret < GST_NET_TIME_PACKET_SIZE) {
    g_print ("Packet too small: %" G_GSSIZE_FORMAT "\n", ret);
    return TRUE;
  }

  if (user_data == NULL) {
    g_print ("Sending to master");
    send_packet_to_master(src_address, buffer);
  }
  else {
    /* Return the reply to the client */
    PortInfo *portinfo = (PortInfo *)(user_data);
    g_print ("Returning to client");
    g_socket_send_to (masterSocket, portinfo->src_address, (const gchar *) buffer,
      GST_NET_TIME_PACKET_SIZE, NULL, NULL);
  }
  return TRUE;
}


static void
send_packet_to_master(GSocketAddress *src_address, gchar *buf)
{
  PortInfo *portinfo = NULL; 
  GList *cur;
  GInetAddress *inet_addr;
  gint inet_port;

  /* Locate the socket for this source address */
  inet_port = g_inet_socket_address_get_port (G_INET_SOCKET_ADDRESS (src_address));
  inet_addr =
      g_inet_socket_address_get_address (G_INET_SOCKET_ADDRESS (src_address));
  cur = g_list_first(ports);
  while (cur != NULL) {
    PortInfo *cur_port = (PortInfo *)(cur->data);
    if (g_inet_address_equal (inet_addr, cur_port->inet_addr) && inet_port == cur_port->port) {
      portinfo = cur_port;
      break;
    }
    cur = g_list_next(cur);
  }

  /* Otherwise, create one */
  if (portinfo == NULL) {
    gchar *tmp;

    portinfo = g_new0(PortInfo, 1);
    portinfo->src_address = src_address;
    portinfo->inet_addr = inet_addr;
    portinfo->port = inet_port;

    tmp = g_inet_address_to_string(inet_addr);
    g_print ("Packet from new client %s:%d\n", tmp, inet_port);
    g_free (tmp);

    listen_on_socket(0, (GSourceFunc)(receive_clock_packet), portinfo);
    ports = g_list_prepend(ports, portinfo);
  }

  /* And send the packet to the master clock provider */
  g_socket_send_to (masterSocket, serverAddress, (const gchar *) buf,
      GST_NET_TIME_PACKET_SIZE, NULL, NULL);
}

static void
listen_on_socket(gint listenPort, GSourceFunc handler, gpointer user_data)
{
  GInetAddress *localAddress;
  GSocketAddress *localSocketAddress;
  GSource *source;

  localAddress = g_inet_address_new_from_string("0.0.0.0");
  localSocketAddress = g_inet_socket_address_new(localAddress, listenPort);
  masterSocket = g_socket_new(G_SOCKET_FAMILY_IPV4, G_SOCKET_TYPE_DATAGRAM, G_SOCKET_PROTOCOL_UDP, NULL);
  g_socket_bind (masterSocket, localSocketAddress, FALSE, NULL);

  source = g_socket_create_source (masterSocket, G_IO_IN, NULL);
  g_source_set_callback (source, handler, user_data, NULL);
  g_source_attach(source, NULL);

  g_socket_listen (masterSocket, NULL);
}

static GInetAddress *
make_inet_address(const gchar *hostname)
{
  GResolver *r;
  GList *result;
  GInetAddress *addr;
  r = g_resolver_get_default();
  result = g_resolver_lookup_by_name(r, hostname, NULL, NULL);
  g_object_unref(r);
  if (result == NULL)
    return NULL;
  addr = g_object_ref (result->data);
  g_resolver_free_addresses(result);
  return addr;
}

static gboolean
gst_clock_bouncer_init (const gchar *provider_addr, guint provider_port)
{
  GInetAddress *provider_inet_addr = make_inet_address(provider_addr);

  if (provider_inet_addr == NULL) {
    g_warning ("Failed to resolve hostname %s\n", provider_addr);
    return FALSE;
  }

  serverAddress = g_inet_socket_address_new(provider_inet_addr, provider_port);
  g_object_unref(provider_inet_addr);

  return TRUE;
}

static void
gst_clock_bouncer_start (guint local_port)
{
  listen_on_socket(local_port, (GSourceFunc)(receive_clock_packet), NULL);
}

#ifdef __ANDROID__
static gpointer
thread_func (gpointer userdata)
{
  gint local_port = GPOINTER_TO_INT (userdata);

  loop = g_main_loop_new(NULL, FALSE);

  gst_clock_bouncer_start (local_port);

  g_main_loop_run(loop);
}

static jboolean
native_start (JNIEnv * env, jobject thiz, jobject addr, jint provider_port,
    jint local_port)
{
  const char *provider_addr;
  gboolean ret;

  provider_addr = (*env)->GetStringUTFChars (env, addr, NULL);

  ret = gst_clock_bouncer_init (provider_addr, provider_port);

  (*env)->ReleaseStringUTFChars (env, addr, provider_addr);
  
  if (ret) {
    thread = g_thread_new ("gst-clock-bouncer", thread_func,
      GINT_TO_POINTER(local_port));
  }

  return ret;
}

static void
native_stop (JNIEnv * env, jobject thiz)
{
  g_main_loop_quit (loop);
  g_thread_join (thread);
}

/* List of implemented native methods */
static JNINativeMethod native_methods[] = {
  {"nativeStart", "(Ljava/lang/String;II)Z", (void *) native_start},
  {"nativeStop", "()V", (void *) native_stop},
};


/* Library initializer */
jint
JNI_OnLoad (JavaVM * vm, void *reserved)
{
  JNIEnv *env = NULL;

  if ((*vm)->GetEnv (vm, (void **) &env, JNI_VERSION_1_4) != JNI_OK) {
    __android_log_print (ANDROID_LOG_ERROR, "gst-net-client-clock-sim",
        "Could not retrieve JNIEnv");
    return 0;
  }

  jclass klass =
      (*env)->FindClass (env,
          "com/centricular/gstclockrecorder/GstClockBouncerActivity");
  (*env)->RegisterNatives (env, klass, native_methods,
      G_N_ELEMENTS (native_methods));

  return JNI_VERSION_1_4;
}


#else
/* Standalone command line program */

int
main(int argc, char **argv)
{
  guint signal_watch_id;
  gchar *server;
  gint local_clock_port, server_clock_port;

  if (argc < 4) {
    g_print ("Usage %s <localport> <server> <serverport>\n  Listen on port <localport> and forward to <server>:<serverport>\n", argv[0]);
    return 1;
  }

  local_clock_port = atoi(argv[1]);
  server = argv[2];
  server_clock_port = atoi(argv[3]);

#ifdef G_OS_UNIX
  signal_watch_id =
      g_unix_signal_add (SIGINT, (GSourceFunc) intr_handler, NULL);
#endif

  if (!gst_clock_bouncer_init (server, server_clock_port))
    goto done;

  gst_clock_bouncer_start (local_clock_port);

  loop = g_main_loop_new(NULL, FALSE);

  g_main_loop_run(loop);  

done:
  g_source_remove (signal_watch_id);
  g_main_loop_unref (loop);

  return 0;
}
#endif
