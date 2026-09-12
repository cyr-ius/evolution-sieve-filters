/* test-timeout.c
 *
 * Unit tests for item 6 (standalone client side): network timeout and
 * cancellation via GCancellable. A local GSocketService is set up that
 * accepts the TCP connection but never says anything — the client must
 * then give up once the allotted delay elapses instead of staying
 * blocked, and return a SIEVE_MANAGESIEVE_ERROR_TIMEOUT error. No real
 * ManageSieve server required; the full round trip stays in
 * tests/dovecot/smoke.sh.
 */

#include <glib.h>
#include <gio/gio.h>

#include "sieve-managesieve-client.h"

typedef struct {
  GSocketService *service;
  guint16         port;
  GList          *held;   /* accepted connections, kept open and silent */
} SilentServer;

static gboolean
on_incoming (GSocketService    *service,
             GSocketConnection *connection,
             GObject           *source_object,
             gpointer           user_data)
{
  SilentServer *srv = user_data;

  (void) service;
  (void) source_object;
  /* We keep a reference to the connection without ever writing anything:
   * the client's banner read will stall until the timeout. */
  srv->held = g_list_prepend (srv->held, g_object_ref (connection));
  return TRUE;
}

static void
silent_server_start (SilentServer *srv)
{
  GError *error = NULL;

  srv->service = g_socket_service_new ();
  srv->port = g_socket_listener_add_any_inet_port (
      G_SOCKET_LISTENER (srv->service), NULL, &error);
  g_assert_no_error (error);
  g_assert_cmpuint (srv->port, >, 0);

  g_signal_connect (srv->service, "incoming", G_CALLBACK (on_incoming), srv);
  g_socket_service_start (srv->service);
}

static void
silent_server_stop (SilentServer *srv)
{
  g_socket_service_stop (srv->service);
  g_list_free_full (srv->held, g_object_unref);
  g_clear_object (&srv->service);
}

/* A fresh client's default is indeed the documented constant. */
static void
test_timeout_default (void)
{
  SieveManageSieveClient *client =
      sieve_managesieve_client_new ("localhost", 4190, FALSE);

  g_assert_cmpuint (sieve_managesieve_client_get_timeout (client), ==,
                    SIEVE_MANAGESIEVE_DEFAULT_TIMEOUT_SECONDS);

  sieve_managesieve_client_set_timeout (client, 5);
  g_assert_cmpuint (sieve_managesieve_client_get_timeout (client), ==, 5);

  sieve_managesieve_client_set_timeout (client, 0); /* 0 = unlimited, accepted */
  g_assert_cmpuint (sieve_managesieve_client_get_timeout (client), ==, 0);

  g_object_unref (client);
}

/* Silent server + short timeout => connect_sync fails with _ERROR_TIMEOUT,
 * and returns control in ~1s, not after the default minute. */
static void
test_timeout_fires_on_silent_server (void)
{
  SilentServer srv = { 0 };
  SieveManageSieveClient *client;
  GError *error = NULL;
  gint64 start, elapsed_ms;
  gboolean ok;

  silent_server_start (&srv);

  client = sieve_managesieve_client_new ("127.0.0.1", srv.port, FALSE);
  sieve_managesieve_client_set_timeout (client, 1);

  start = g_get_monotonic_time ();
  ok = sieve_managesieve_client_connect_sync (client, NULL, &error);
  elapsed_ms = (g_get_monotonic_time () - start) / 1000;

  g_assert_false (ok);
  g_assert_error (error, SIEVE_MANAGESIEVE_ERROR, SIEVE_MANAGESIEVE_ERROR_TIMEOUT);
  /* Wide margin: we just want to prove we didn't wait 30s. */
  g_assert_cmpint (elapsed_ms, <, 10000);

  g_clear_error (&error);
  sieve_managesieve_client_disconnect (client);
  g_object_unref (client);
  silent_server_stop (&srv);
}

/* An already-cancelled GCancellable makes connect_sync fail immediately,
 * and the error stays G_IO_ERROR_CANCELLED (especially NOT relabeled as a
 * timeout: the UI must be able to distinguish "the user cancelled" from
 * a real network incident). */
static void
test_cancelled_before_connect (void)
{
  SilentServer srv = { 0 };
  SieveManageSieveClient *client;
  GCancellable *cancellable = g_cancellable_new ();
  GError *error = NULL;
  gboolean ok;

  silent_server_start (&srv);
  g_cancellable_cancel (cancellable);

  client = sieve_managesieve_client_new ("127.0.0.1", srv.port, FALSE);
  ok = sieve_managesieve_client_connect_sync (client, cancellable, &error);

  g_assert_false (ok);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_CANCELLED);

  g_clear_error (&error);
  sieve_managesieve_client_disconnect (client);
  g_object_unref (client);
  g_object_unref (cancellable);
  silent_server_stop (&srv);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/client/timeout/default", test_timeout_default);
  g_test_add_func ("/client/timeout/fires-on-silent-server",
                   test_timeout_fires_on_silent_server);
  g_test_add_func ("/client/timeout/cancelled-before-connect",
                   test_cancelled_before_connect);

  return g_test_run ();
}
