/*
 * Copyright (C) 2014 Sebastian Dröge <sebastian@centricular.com>
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
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#ifdef __ANDROID__
#include <jni.h>
#include <android/log.h>
#endif

#include <gio/gio.h>

#include <gst/net/gstnettimepacket.h>
#include <gst/net/gstnettimeprovider.h>

#include "gst-net-client-clock-sim.h"

/*
 * These macros provide a way to store the native pointer to
 * GstNetClientClockSim, which might be 32 or 64 bits, into a jlong, which is
 * always 64 bits, without warnings.
 */
#if GLIB_SIZEOF_VOID_P == 8
# define GET_CUSTOM_DATA(env, thiz, fieldID) (GstNetClientClockSim *)(*env)->GetLongField (env, thiz, fieldID)
# define SET_CUSTOM_DATA(env, thiz, fieldID, data) (*env)->SetLongField (env, thiz, fieldID, (jlong)data)
#else
# define GET_CUSTOM_DATA(env, thiz, fieldID) (GstNetClientClockSim *)(jint)(*env)->GetLongField (env, thiz, fieldID)
# define SET_CUSTOM_DATA(env, thiz, fieldID, data) (*env)->SetLongField (env, thiz, fieldID, (jlong)(jint)data)
#endif

struct _GstNetClientClockSim {
#ifdef __ANDROID__
  jobject app;
#endif
  GstNetTimeProvider *provider;
  GSocketAddress *dst_addr;
  GSocket *socket;
  GFileOutputStream *out;
  GThread *thread;
  volatile int quit;
};

#ifdef __ANDROID__
static jfieldID app_data_field_id;
#endif

static gpointer thread_func (gpointer userdata)
{
  GstNetClientClockSim *sim = (GstNetClientClockSim *) userdata;
  GstClock *clock = gst_system_clock_obtain ();
  gsize outlen;

  g_print ("Starting thread ...");

  while (!g_atomic_int_get (&sim->quit)) {
    GstNetTimePacket *packet;
    GstClockTime local_2;
    GError *error = NULL;

    packet = gst_net_time_packet_new (NULL);
    packet->local_time = gst_clock_get_time (clock);
    
    g_print ("Sending packet ...");
    if (!gst_net_time_packet_send (packet, sim->socket, sim->dst_addr, &error)) {
      g_warning ("Could not send packet: %s", error->message);
      g_error_free (error);
      goto next;
    }

    gst_net_time_packet_free (packet);

    g_print ("Waiting for packet to return ...");
    if (!g_socket_condition_timed_wait (sim->socket, G_IO_IN,
          2 * G_USEC_PER_SEC, NULL, NULL)) {
      g_warning ("Timed out");
      goto next;
    }

    if (!(packet = gst_net_time_packet_receive (sim->socket, NULL, &error))) {
      g_warning ("Could not receive packet: %s", error->message);
      g_error_free (error);
      goto next;
    }

    local_2 = gst_clock_get_time (clock);

    if (!g_output_stream_printf (G_OUTPUT_STREAM (sim->out), &outlen, NULL,
          &error,
          "%" G_GUINT64_FORMAT " %" G_GUINT64_FORMAT " %" G_GUINT64_FORMAT
          " %" G_GUINT64_FORMAT "\n", packet->local_time, packet->remote_time,
          packet->remote_time, local_2)) {
      g_warning ("Failed to write to file: %s", error->message);
      g_error_free (error);
    }

    g_print ("%" G_GUINT64_FORMAT " %" G_GUINT64_FORMAT " %" G_GUINT64_FORMAT
        " %" G_GUINT64_FORMAT "\n", packet->local_time, packet->remote_time,
          packet->remote_time, local_2);

next:
    g_usleep (G_USEC_PER_SEC);
  }

  gst_object_unref (clock);

  return NULL;
}

GstNetClientClockSim *
gst_net_client_clock_sim_start (const gchar *addr, guint port,
    guint provider_port, const gchar *file_path)
{
  GstNetClientClockSim *sim;
  GstClock *clock;
  GInetAddress *inetaddr = NULL;
  GSocketAddress *sockaddr = NULL;
  GFile *file;
  GError *error = NULL;

  sim = g_new0 (GstNetClientClockSim, 1);

  sim->dst_addr = g_inet_socket_address_new_from_string (addr, port);
  if (!sim->dst_addr) {
    g_warning ("Invalid address or port");
    goto fail;
  }

  sim->socket = g_socket_new (G_SOCKET_FAMILY_IPV4, G_SOCKET_TYPE_DATAGRAM,
      G_SOCKET_PROTOCOL_DEFAULT, &error);
  if (!sim->socket) {
    g_warning ("Could not create socket: %s", error->message);
    g_error_free (error);
    goto fail;
  }

  g_socket_set_blocking (sim->socket, FALSE);

  inetaddr = g_inet_address_new_any (G_SOCKET_FAMILY_IPV4);
  sockaddr = g_inet_socket_address_new (inetaddr, 0);
  g_object_unref (inetaddr);

  if (!sockaddr) {
    g_warning ("Could not parse address");
    goto fail;
  }

  if (!g_socket_bind (sim->socket, sockaddr, FALSE, &error)) {
    g_warning ("Could not bind socket: %s", error->message);
    g_error_free (error);
    goto fail;
  }

  g_object_unref (sockaddr);

  clock = gst_system_clock_obtain ();
  sim->provider = gst_net_time_provider_new (clock, NULL, provider_port);
  gst_object_unref (clock);

  if (!sim->provider) {
    g_warning ("Could not create net time provider");
    goto fail;
  }

  file = g_file_new_for_path (file_path);
  sim->out = g_file_create (file, 0, NULL, &error);
  if (!sim->out) {
    g_warning ("Could not create file: %s", error->message);
    g_error_free (error);
    goto fail;
  }

  sim->thread = g_thread_new ("net-client-clock-sim", thread_func, sim);
  if (!sim->thread) {
    g_warning ("Could not create thread");
    goto fail;
  }
  
  return sim;

fail:
  if (sim) {
    if (sim->dst_addr)
      g_object_unref (sim->dst_addr);

    if (sim->socket)
      g_object_unref (sim->socket);

    if (sockaddr)
      g_object_unref (sockaddr);

    g_free (sim);
  }

  return NULL;
}

void
gst_net_client_clock_sim_stop (GstNetClientClockSim * sim)
{
  if (sim->thread) {
    g_atomic_int_set (&sim->quit, 1);
    g_thread_join (sim->thread);

    g_output_stream_close (G_OUTPUT_STREAM (sim->out), NULL, NULL);
    g_object_unref (sim->out);

    g_object_unref (sim->provider);

    g_socket_close (sim->socket, NULL);
    g_object_unref (sim->socket);
    g_object_unref (sim->dst_addr);
  }

  g_free (sim);
}

#ifdef __ANDROID__
static jboolean
native_class_init (JNIEnv * env, jclass klass)
{
  app_data_field_id = (*env)->GetFieldID (env, klass, "native_app_data", "J");

  if (!app_data_field_id) {
    /* We emit this message through the Android log instead of the GStreamer
     * log because the latter has not been initialized yet.
     */
    __android_log_print (ANDROID_LOG_ERROR, "gst-net-client-clock-sim",
        "The calling class does not implement all necessary interface methods");
    return JNI_FALSE;
  }

  return JNI_TRUE;
}

static jboolean
native_start (JNIEnv * env, jobject thiz, jobject addr, jint port,
    jint provider_port, jobject path)
{
  const char *ip_addr, *file_path;
  GstNetClientClockSim *sim;

  ip_addr = (*env)->GetStringUTFChars (env, addr, NULL);
  file_path = (*env)->GetStringUTFChars (env, path, NULL);

  sim =
    gst_net_client_clock_sim_start (ip_addr, port, provider_port, file_path);

  (*env)->ReleaseStringUTFChars (env, addr, ip_addr);
  (*env)->ReleaseStringUTFChars (env, path, file_path);

  if (sim)
    SET_CUSTOM_DATA (env, thiz, app_data_field_id, sim);

  return !!sim;
}

static void
native_stop (JNIEnv * env, jobject thiz)
{
  GstNetClientClockSim *sim = GET_CUSTOM_DATA (env, thiz, app_data_field_id);

  gst_net_client_clock_sim_stop (sim);
}

/* List of implemented native methods */
static JNINativeMethod native_methods[] = {
  {"nativeClassInit", "()Z", (void *) native_class_init},
  {"nativeStart", "(Ljava/lang/String;IILjava/lang/String;)Z",
    (void *) native_start},
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
          "com/centricular/gstclockbouncer/GstNetClientClockSimActivity");
  (*env)->RegisterNatives (env, klass, native_methods,
      G_N_ELEMENTS (native_methods));

  return JNI_VERSION_1_4;
}

#else
/* Standalone build on the desktop */

int main (int argc, char *argv[])
{
  GstNetClientClockSim *sim;

  if (argc != 5) {
    g_warning ("Usage: %s remote_addr remort_port provider_port file-path", argv[0]);
    return 1;
  }

  sim = gst_net_client_clock_sim_start (argv[1],
      g_ascii_strtoull (argv[2], NULL, 10),
      g_ascii_strtoull (argv[3], NULL, 10),
      argv[4]);
  if (!sim)
    return 1;

  g_usleep (60 * G_USEC_PER_SEC);

  gst_net_client_clock_sim_stop (sim);

  return 0;
}
#endif
