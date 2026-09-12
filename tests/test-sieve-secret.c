/* test-sieve-secret.c
 *
 * Keyring round trip: store → lookup → replace → clear.
 *
 * This test needs a Secret Service reachable via D-Bus (gnome-keyring,
 * kwallet, or libsecret's mock service). Without one, it cleanly
 * "skips" — unless SIEVE_SECRET_REQUIRE_SERVICE is set in the
 * environment, in which case the absence of a service is a failure.
 * That's what tests/secret/smoke.sh does: it sets up a disposable
 * gnome-keyring under dbus-run-session before running this binary.
 *
 * The entries created use a dummy host specific to the test and are
 * erased at the end of the run; the hostname includes the PID so it
 * doesn't clash with a parallel run or a real user entry.
 */

#include <glib.h>
#include <stdlib.h>

#include "sieve-secret.h"

typedef struct {
  gchar   *host;
  guint16  port;
  gchar   *user;
} Fixture;

static void
fixture_set_up (Fixture *fx, gconstpointer user_data)
{
  (void) user_data;
  fx->host = g_strdup_printf ("test-sieve-secret.%d.invalid", (int) getpid ());
  fx->port = 4190;
  fx->user = g_strdup ("testuser");
}

static void
fixture_tear_down (Fixture *fx, gconstpointer user_data)
{
  (void) user_data;
  /* Best-effort: we don't want to leave a stray entry behind if an
   * assert failed along the way. Errors here have no consequence. */
  sieve_secret_clear_password_sync (fx->host, fx->port, fx->user, NULL, NULL);
  g_free (fx->host);
  g_free (fx->user);
}

/* TRUE if a Secret Service responds. On failure: skip, or hard abort if
 * SIEVE_SECRET_REQUIRE_SERVICE is set. */
static gboolean
require_service_or_skip (Fixture *fx)
{
  GError *error = NULL;
  gchar *pw;

  /* A lookup on a nonexistent entry: NULL with no error if the service
   * is present, NULL with an error if it's unreachable. */
  pw = sieve_secret_lookup_password_sync (fx->host, fx->port, fx->user, NULL, &error);
  if (pw != NULL) {
    /* Unlikely leftover from a previous run: clean up and carry on. */
    sieve_secret_password_free (pw);
    return TRUE;
  }

  if (error == NULL)
    return TRUE; /* service present, simply nothing stored */

  if (g_getenv ("SIEVE_SECRET_REQUIRE_SERVICE") != NULL) {
    g_error ("Secret Service unreachable while SIEVE_SECRET_REQUIRE_SERVICE "
             "is set: %s", error->message);
  }

  g_test_skip (error->message);
  g_clear_error (&error);
  return FALSE;
}

static void
test_roundtrip (Fixture *fx, gconstpointer user_data)
{
  GError *error = NULL;
  gchar *got;

  (void) user_data;
  if (!require_service_or_skip (fx))
    return;

  /* store */
  g_assert_true (sieve_secret_store_password_sync (fx->host, fx->port, fx->user,
                                                   "s3cr3t", NULL, &error));
  g_assert_no_error (error);

  /* lookup → same value */
  got = sieve_secret_lookup_password_sync (fx->host, fx->port, fx->user, NULL, &error);
  g_assert_no_error (error);
  g_assert_cmpstr (got, ==, "s3cr3t");
  sieve_secret_password_free (got);

  /* store again → replaces, no duplicate */
  g_assert_true (sieve_secret_store_password_sync (fx->host, fx->port, fx->user,
                                                   "new-value", NULL, &error));
  g_assert_no_error (error);
  got = sieve_secret_lookup_password_sync (fx->host, fx->port, fx->user, NULL, &error);
  g_assert_no_error (error);
  g_assert_cmpstr (got, ==, "new-value");
  sieve_secret_password_free (got);

  /* clear → the entry disappears */
  g_assert_true (sieve_secret_clear_password_sync (fx->host, fx->port, fx->user,
                                                   NULL, &error));
  g_assert_no_error (error);

  got = sieve_secret_lookup_password_sync (fx->host, fx->port, fx->user, NULL, &error);
  g_assert_no_error (error);
  g_assert_null (got);

  /* clear is idempotent: FALSE (nothing to remove), but no error */
  g_assert_false (sieve_secret_clear_password_sync (fx->host, fx->port, fx->user,
                                                    NULL, &error));
  g_assert_no_error (error);
}

/* An entry for (host, port_a, user) must not be returned for
 * (host, port_b, user): the port is part of the key. */
static void
test_port_scoped (Fixture *fx, gconstpointer user_data)
{
  GError *error = NULL;
  gchar *got;

  (void) user_data;
  if (!require_service_or_skip (fx))
    return;

  g_assert_true (sieve_secret_store_password_sync (fx->host, 4190, fx->user,
                                                   "for-4190", NULL, &error));
  g_assert_no_error (error);

  got = sieve_secret_lookup_password_sync (fx->host, 2000, fx->user, NULL, &error);
  g_assert_no_error (error);
  g_assert_null (got);

  g_assert_true (sieve_secret_clear_password_sync (fx->host, 4190, fx->user,
                                                   NULL, &error));
  g_assert_no_error (error);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add ("/sieve-secret/roundtrip", Fixture, NULL,
              fixture_set_up, test_roundtrip, fixture_tear_down);
  g_test_add ("/sieve-secret/port-scoped", Fixture, NULL,
              fixture_set_up, test_port_scoped, fixture_tear_down);

  return g_test_run ();
}
