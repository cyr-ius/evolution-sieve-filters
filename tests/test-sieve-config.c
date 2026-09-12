/* test-sieve-config.c
 *
 * Unit tests for src/sieve-config.[ch] — no network, no keyring.
 *
 * $XDG_CONFIG_HOME is set once and for all in main(), BEFORE any GLib
 * call: g_get_user_config_dir() caches its result on first call, so
 * changing the variable afterwards (per test) would have no effect.
 * Each test starts from a fresh state.ini (fixture_reset).
 *
 * Coverage: "missing file = defaults", save → load round trip for an
 * account profile, isolation between accounts + the "manual" profile,
 * the "last account" pointer, migration from the old single-profile
 * [connection] format.
 */

#include <glib.h>
#include <glib/gstdio.h>

#include "sieve-config.h"

static gchar *config_home = NULL;

static gchar *
state_file_path (void)
{
  return g_build_filename (config_home, "evolution-sieve-filters",
                           "state.ini", NULL);
}

/* Clears any leftover state from a previous test. */
static void
fixture_reset (void)
{
  g_autofree gchar *state_file = state_file_path ();
  g_remove (state_file);
}

static void
test_missing_file_defaults (void)
{
  SieveConfig *config;
  g_autofree gchar *last = NULL;

  fixture_reset ();

  config = sieve_config_load_for_account ("acc-1");
  g_assert_nonnull (config);
  g_assert_cmpstr (config->account_uid, ==, "acc-1");
  g_assert_null (config->host);
  g_assert_null (config->user);
  g_assert_cmpuint (config->port, ==, 0);
  g_assert_false (config->implicit_tls);
  g_assert_true (config->auto_connect); /* armed by default */
  g_assert_true (config->remember_password); /* checked by default */
  sieve_config_free (config);

  /* "Manual" profile: account_uid normalized to NULL. */
  config = sieve_config_load_for_account (NULL);
  g_assert_null (config->account_uid);
  sieve_config_free (config);

  last = sieve_config_dup_last_account ();
  g_assert_null (last);
}

static void
test_account_round_trip_and_isolation (void)
{
  GError *error = NULL;
  SieveConfig a1 = {
    .account_uid = (gchar *) "acc-1",
    .host = (gchar *) "sieve.one.tld",
    .port = 4190,
    .user = (gchar *) "alice",
    .implicit_tls = FALSE,
    .auto_connect = TRUE,
    .remember_password = FALSE,
  };
  SieveConfig a2 = {
    .account_uid = (gchar *) "acc-2",
    .host = (gchar *) "sieve.two.tld",
    .port = 4191,
    .user = (gchar *) "bob",
    .implicit_tls = TRUE,
    .auto_connect = FALSE,
    .remember_password = TRUE,
  };
  SieveConfig *read;

  fixture_reset ();

  g_assert_true (sieve_config_save_for_account (&a1, &error));
  g_assert_no_error (error);
  g_assert_true (sieve_config_save_for_account (&a2, &error));
  g_assert_no_error (error);

  read = sieve_config_load_for_account ("acc-1");
  g_assert_cmpstr (read->account_uid, ==, "acc-1");
  g_assert_cmpstr (read->host, ==, "sieve.one.tld");
  g_assert_cmpuint (read->port, ==, 4190);
  g_assert_cmpstr (read->user, ==, "alice");
  g_assert_false (read->implicit_tls);
  g_assert_true (read->auto_connect);
  g_assert_false (read->remember_password);
  sieve_config_free (read);

  read = sieve_config_load_for_account ("acc-2");
  g_assert_cmpstr (read->host, ==, "sieve.two.tld");
  g_assert_cmpuint (read->port, ==, 4191);
  g_assert_cmpstr (read->user, ==, "bob");
  g_assert_true (read->implicit_tls);
  g_assert_false (read->auto_connect);
  g_assert_true (read->remember_password);
  sieve_config_free (read);

  /* An account never written keeps the defaults. */
  read = sieve_config_load_for_account ("acc-3");
  g_assert_null (read->host);
  g_assert_true (read->auto_connect);
  sieve_config_free (read);
}

static void
test_manual_profile_distinct (void)
{
  GError *error = NULL;
  SieveConfig manual = {
    .account_uid = NULL,
    .host = (gchar *) "manual.tld",
    .port = 2000,
    .user = (gchar *) "free",
    .implicit_tls = TRUE,
    .auto_connect = TRUE,
    .remember_password = TRUE,
  };
  SieveConfig account = {
    .account_uid = (gchar *) "acc-1",
    .host = (gchar *) "account.tld",
    .port = 4190,
    .user = (gchar *) "alice",
  };
  SieveConfig *read;

  fixture_reset ();

  g_assert_true (sieve_config_save_for_account (&manual, &error));
  g_assert_no_error (error);
  g_assert_true (sieve_config_save_for_account (&account, &error));
  g_assert_no_error (error);

  read = sieve_config_load_for_account (NULL);
  g_assert_null (read->account_uid);
  g_assert_cmpstr (read->host, ==, "manual.tld");
  g_assert_cmpuint (read->port, ==, 2000);
  sieve_config_free (read);

  read = sieve_config_load_for_account ("acc-1");
  g_assert_cmpstr (read->host, ==, "account.tld");
  sieve_config_free (read);

  /* "" is treated as NULL (manual profile). */
  read = sieve_config_load_for_account ("");
  g_assert_null (read->account_uid);
  g_assert_cmpstr (read->host, ==, "manual.tld");
  sieve_config_free (read);
}

static void
test_last_account (void)
{
  GError *error = NULL;
  g_autofree gchar *last = NULL;

  fixture_reset ();

  g_assert_true (sieve_config_set_last_account ("acc-42", &error));
  g_assert_no_error (error);

  last = sieve_config_dup_last_account ();
  g_assert_cmpstr (last, ==, "acc-42");
  g_clear_pointer (&last, g_free);

  /* NULL clears it (empty string => dup returns NULL). */
  g_assert_true (sieve_config_set_last_account (NULL, &error));
  g_assert_no_error (error);
  last = sieve_config_dup_last_account ();
  g_assert_null (last);
}

static void
test_last_account_survives_profile_writes (void)
{
  GError *error = NULL;
  SieveConfig cfg = {
    .account_uid = (gchar *) "acc-1",
    .host = (gchar *) "h.tld",
    .port = 4190,
  };
  g_autofree gchar *last = NULL;

  fixture_reset ();

  g_assert_true (sieve_config_set_last_account ("acc-1", &error));
  g_assert_no_error (error);
  /* Writing a profile must not lose [state].last-account. */
  g_assert_true (sieve_config_save_for_account (&cfg, &error));
  g_assert_no_error (error);

  last = sieve_config_dup_last_account ();
  g_assert_cmpstr (last, ==, "acc-1");
}

static void
test_legacy_migration (void)
{
  g_autofree gchar *path = NULL;
  g_autofree gchar *dir = NULL;
  g_autoptr (GKeyFile) kf = g_key_file_new ();
  SieveConfig *read;
  g_autofree gchar *last = NULL;

  fixture_reset ();

  /* Old format: a single [connection] group with an account-uid key. */
  dir = g_build_filename (config_home, "evolution-sieve-filters", NULL);
  path = state_file_path ();
  g_assert_cmpint (g_mkdir_with_parents (dir, 0700), ==, 0);
  g_key_file_set_string  (kf, "connection", "account-uid", "legacy-acc");
  g_key_file_set_string  (kf, "connection", "host", "old.tld");
  g_key_file_set_integer (kf, "connection", "port", 4190);
  g_key_file_set_string  (kf, "connection", "user", "olduser");
  g_key_file_set_boolean (kf, "connection", "implicit-tls", TRUE);
  g_key_file_set_boolean (kf, "connection", "auto-connect", FALSE);
  g_assert_true (g_key_file_save_to_file (kf, path, NULL));

  read = sieve_config_load_for_account ("legacy-acc");
  g_assert_cmpstr (read->host, ==, "old.tld");
  g_assert_cmpuint (read->port, ==, 4190);
  g_assert_cmpstr (read->user, ==, "olduser");
  g_assert_true (read->implicit_tls);
  g_assert_false (read->auto_connect);
  sieve_config_free (read);

  last = sieve_config_dup_last_account ();
  g_assert_cmpstr (last, ==, "legacy-acc");
}

int
main (int argc, char **argv)
{
  int status;

  /* BEFORE g_test_init / any GLib call: see the comment at the top. */
  config_home = g_dir_make_tmp ("sieve-config-XXXXXX", NULL);
  g_assert_nonnull (config_home);
  g_setenv ("XDG_CONFIG_HOME", config_home, TRUE);

  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/sieve-config/missing-file-defaults",
                   test_missing_file_defaults);
  g_test_add_func ("/sieve-config/account-round-trip-and-isolation",
                   test_account_round_trip_and_isolation);
  g_test_add_func ("/sieve-config/manual-profile-distinct",
                   test_manual_profile_distinct);
  g_test_add_func ("/sieve-config/last-account", test_last_account);
  g_test_add_func ("/sieve-config/last-account-survives-profile-writes",
                   test_last_account_survives_profile_writes);
  g_test_add_func ("/sieve-config/legacy-migration", test_legacy_migration);

  status = g_test_run ();

  fixture_reset ();
  {
    g_autofree gchar *state_dir =
      g_build_filename (config_home, "evolution-sieve-filters", NULL);
    g_rmdir (state_dir);
    g_rmdir (config_home);
  }
  g_clear_pointer (&config_home, g_free);

  return status;
}
