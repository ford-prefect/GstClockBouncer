/* FIXME: Copyright */

#include <gst/gst.h>

typedef struct _GstNetClientClockSim GstNetClientClockSim;

GstNetClientClockSim *
gst_net_client_clock_sim_start (const gchar *addr, guint port,
    guint provider_port, const gchar *file_path);

void
gst_net_client_clock_sim_stop (GstNetClientClockSim * sim);
