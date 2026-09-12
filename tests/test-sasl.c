/* test-sasl.c
 *
 * Unit tests for src/sieve-sasl.[ch] — no network, no server.
 * The full round trip of mechanisms against a real Dovecot is covered
 * by tests/dovecot/smoke.sh; here we check negotiation, the formatting
 * of the OAuth mechanisms (which libgsasl does not provide), and the
 * libgsasl wiring.
 */

#include <glib.h>
#include <string.h>

#include "sieve-sasl.h"

/* ---- Negotiation ------------------------------------------------------------ */

static void
test_select_auto_prefers_scram (void)
{
  SieveSaslCredentials creds = { .authid = "alice", .password = "pw" };
  GError *error = NULL;
  gchar *m = sieve_sasl_select_mechanism (
      "PLAIN LOGIN CRAM-MD5 SCRAM-SHA-1 SCRAM-SHA-256", NULL, &creds, &error);

  g_assert_no_error (error);
  g_assert_cmpstr (m, ==, "SCRAM-SHA-256");
  g_free (m);
}

static void
test_select_auto_falls_back_to_plain (void)
{
  SieveSaslCredentials creds = { .authid = "alice", .password = "pw" };
  GError *error = NULL;
  gchar *m = sieve_sasl_select_mechanism ("LOGIN PLAIN", NULL, &creds, &error);

  g_assert_no_error (error);
  g_assert_cmpstr (m, ==, "PLAIN");
  g_free (m);
}

static void
test_select_auto_token_prefers_oauthbearer (void)
{
  SieveSaslCredentials creds = { .authid = "alice", .oauth2_token = "tok" };
  GError *error = NULL;
  gchar *m = sieve_sasl_select_mechanism ("PLAIN OAUTHBEARER XOAUTH2", NULL,
                                          &creds, &error);

  g_assert_no_error (error);
  g_assert_cmpstr (m, ==, "OAUTHBEARER");
  g_free (m);
}

static void
test_select_force_ok_case_insensitive (void)
{
  SieveSaslCredentials creds = { .authid = "alice", .password = "pw" };
  GError *error = NULL;
  gchar *m = sieve_sasl_select_mechanism ("PLAIN LOGIN", "plain", &creds, &error);

  g_assert_no_error (error);
  g_assert_cmpstr (m, ==, "PLAIN");
  g_free (m);
}

static void
test_select_force_unknown_mechanism (void)
{
  SieveSaslCredentials creds = { .authid = "alice", .password = "pw" };
  GError *error = NULL;
  gchar *m = sieve_sasl_select_mechanism ("PLAIN GSSAPI", "GSSAPI", &creds, &error);

  g_assert_null (m);
  g_assert_error (error, SIEVE_SASL_ERROR, SIEVE_SASL_ERROR_NO_MECHANISM);
  g_clear_error (&error);
}

static void
test_select_force_not_offered (void)
{
  SieveSaslCredentials creds = { .authid = "alice", .password = "pw" };
  GError *error = NULL;
  gchar *m = sieve_sasl_select_mechanism ("PLAIN LOGIN", "SCRAM-SHA-256",
                                          &creds, &error);

  g_assert_null (m);
  g_assert_error (error, SIEVE_SASL_ERROR, SIEVE_SASL_ERROR_NO_MECHANISM);
  g_clear_error (&error);
}

static void
test_select_force_missing_credential (void)
{
  SieveSaslCredentials creds = { .authid = "alice", .password = "pw" }; /* no token */
  GError *error = NULL;
  gchar *m = sieve_sasl_select_mechanism ("PLAIN OAUTHBEARER", "OAUTHBEARER",
                                          &creds, &error);

  g_assert_null (m);
  g_assert_error (error, SIEVE_SASL_ERROR, SIEVE_SASL_ERROR_MISSING_CRED);
  g_clear_error (&error);
}

static void
test_select_no_common_mechanism (void)
{
  SieveSaslCredentials creds = { .authid = "alice", .password = "pw" }; /* password only */
  GError *error = NULL;
  gchar *m = sieve_sasl_select_mechanism ("OAUTHBEARER XOAUTH2", NULL, &creds, &error);

  g_assert_null (m);
  g_assert_error (error, SIEVE_SASL_ERROR, SIEVE_SASL_ERROR_NO_MECHANISM);
  g_clear_error (&error);
}

/* ---- Introspection ------------------------------------------------------- */

static void
test_client_first_classification (void)
{
  g_assert_true  (sieve_sasl_mechanism_is_client_first ("PLAIN"));
  g_assert_true  (sieve_sasl_mechanism_is_client_first ("scram-sha-256"));
  g_assert_true  (sieve_sasl_mechanism_is_client_first ("OAUTHBEARER"));
  g_assert_true  (sieve_sasl_mechanism_is_client_first ("XOAUTH2"));
  g_assert_false (sieve_sasl_mechanism_is_client_first ("LOGIN"));
  g_assert_false (sieve_sasl_mechanism_is_client_first ("CRAM-MD5"));
}

static void
test_known_mechanisms_listed (void)
{
  const gchar * const *m = sieve_sasl_known_mechanisms ();
  gboolean saw_scram = FALSE, saw_plain = FALSE;

  g_assert_nonnull (m);
  for (gsize i = 0; m[i] != NULL; i++) {
    if (g_strcmp0 (m[i], "SCRAM-SHA-256") == 0) saw_scram = TRUE;
    if (g_strcmp0 (m[i], "PLAIN") == 0) saw_plain = TRUE;
  }
  g_assert_true (saw_scram);
  g_assert_true (saw_plain);
}

/* ---- Response formatting ---------------------------------------------------- */

static void
test_plain_initial_response (void)
{
  SieveSaslCredentials creds = { .authid = "alice", .password = "s3cr" };
  GError *error = NULL;
  SieveSasl *s = sieve_sasl_new ("PLAIN", &creds, &error);
  guchar *resp = NULL;
  gsize resp_len = 0;
  gboolean done = FALSE;

  g_assert_no_error (error);
  g_assert_nonnull (s);

  g_assert_true (sieve_sasl_step (s, NULL, 0, &resp, &resp_len, &done, &error));
  g_assert_no_error (error);
  g_assert_true (done);
  g_assert_cmpuint (resp_len, ==, sizeof ("\0alice\0s3cr") - 1);
  g_assert_cmpint (memcmp (resp, "\0alice\0s3cr", resp_len), ==, 0);

  g_free (resp);
  sieve_sasl_free (s);
}

static void
test_xoauth2_initial_response (void)
{
  SieveSaslCredentials creds = { .authid = "alice@example.com", .oauth2_token = "vF9dft4qmT" };
  GError *error = NULL;
  SieveSasl *s = sieve_sasl_new ("XOAUTH2", &creds, &error);
  guchar *resp = NULL;
  gsize resp_len = 0;
  gboolean done = FALSE;
  const char *expected = "user=alice@example.com\x01" "auth=Bearer vF9dft4qmT\x01\x01";

  g_assert_no_error (error);
  g_assert_nonnull (s);

  g_assert_true (sieve_sasl_step (s, NULL, 0, &resp, &resp_len, &done, &error));
  g_assert_no_error (error);
  g_assert_true (done);
  g_assert_cmpuint (resp_len, ==, strlen (expected));
  g_assert_cmpint (memcmp (resp, expected, resp_len), ==, 0);

  g_free (resp);
  sieve_sasl_free (s);
}

static void
test_oauthbearer_initial_response (void)
{
  SieveSaslCredentials creds = {
    .authid = "alice@example.com", .oauth2_token = "vF9dft4qmT",
    .hostname = "sieve.example.com", .port = 4190,
  };
  GError *error = NULL;
  SieveSasl *s = sieve_sasl_new ("OAUTHBEARER", &creds, &error);
  guchar *resp = NULL;
  gsize resp_len = 0;
  gboolean done = FALSE;
  const char *expected =
    "n,a=alice@example.com,\x01" "host=sieve.example.com\x01" "port=4190\x01"
    "auth=Bearer vF9dft4qmT\x01\x01";

  g_assert_no_error (error);
  g_assert_nonnull (s);

  g_assert_true (sieve_sasl_step (s, NULL, 0, &resp, &resp_len, &done, &error));
  g_assert_no_error (error);
  g_assert_true (done);
  g_assert_cmpuint (resp_len, ==, strlen (expected));
  g_assert_cmpint (memcmp (resp, expected, resp_len), ==, 0);

  g_free (resp);
  sieve_sasl_free (s);
}

static void
test_oauthbearer_error_kick (void)
{
  SieveSaslCredentials creds = { .authid = "a", .oauth2_token = "t" };
  GError *error = NULL;
  SieveSasl *s = sieve_sasl_new ("OAUTHBEARER", &creds, &error);
  guchar *resp = NULL;
  gsize resp_len = 0;
  gboolean done = FALSE;

  g_assert_nonnull (s);
  g_assert_true (sieve_sasl_step (s, NULL, 0, &resp, &resp_len, &done, &error));
  g_free (resp);
  resp = NULL;

  /* The server returns an error JSON: the client must emit the single
   * byte 0x01 and then declare itself done. */
  g_assert_true (sieve_sasl_step (s, (const guchar *) "{\"status\":\"invalid\"}", 20,
                                  &resp, &resp_len, &done, &error));
  g_assert_no_error (error);
  g_assert_true (done);
  g_assert_cmpuint (resp_len, ==, 1);
  g_assert_cmpint (resp[0], ==, 0x01);

  g_free (resp);
  sieve_sasl_free (s);
}

static void
test_scram_sha1_client_first (void)
{
  SieveSaslCredentials creds = { .authid = "alice", .password = "pw" };
  GError *error = NULL;
  SieveSasl *s = sieve_sasl_new ("SCRAM-SHA-1", &creds, &error);
  guchar *resp = NULL;
  gsize resp_len = 0;
  gboolean done = FALSE;
  gchar *as_str;

  g_assert_no_error (error);
  g_assert_nonnull (s);

  g_assert_true (sieve_sasl_step (s, NULL, 0, &resp, &resp_len, &done, &error));
  g_assert_no_error (error);
  g_assert_false (done);          /* SCRAM: at least one round trip left */
  g_assert_cmpuint (resp_len, >, 0);

  as_str = g_strndup ((const gchar *) resp, resp_len);
  /* client-first-message : "n,,n=alice,r=<nonce>" (RFC 5802 §7). */
  g_assert_true (g_str_has_prefix (as_str, "n,,n=alice,r="));
  g_free (as_str);

  g_free (resp);
  sieve_sasl_free (s);
}

static void
test_login_missing_password (void)
{
  SieveSaslCredentials creds = { .authid = "alice" }; /* no password */
  GError *error = NULL;
  SieveSasl *s = sieve_sasl_new ("LOGIN", &creds, &error);

  g_assert_null (s);
  g_assert_error (error, SIEVE_SASL_ERROR, SIEVE_SASL_ERROR_MISSING_CRED);
  g_clear_error (&error);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/sasl/select/auto-prefers-scram", test_select_auto_prefers_scram);
  g_test_add_func ("/sasl/select/auto-fallback-plain", test_select_auto_falls_back_to_plain);
  g_test_add_func ("/sasl/select/auto-token-oauthbearer", test_select_auto_token_prefers_oauthbearer);
  g_test_add_func ("/sasl/select/force-ci", test_select_force_ok_case_insensitive);
  g_test_add_func ("/sasl/select/force-unknown", test_select_force_unknown_mechanism);
  g_test_add_func ("/sasl/select/force-not-offered", test_select_force_not_offered);
  g_test_add_func ("/sasl/select/force-missing-cred", test_select_force_missing_credential);
  g_test_add_func ("/sasl/select/no-common", test_select_no_common_mechanism);
  g_test_add_func ("/sasl/introspect/client-first", test_client_first_classification);
  g_test_add_func ("/sasl/introspect/known-mechs", test_known_mechanisms_listed);
  g_test_add_func ("/sasl/step/plain", test_plain_initial_response);
  g_test_add_func ("/sasl/step/xoauth2", test_xoauth2_initial_response);
  g_test_add_func ("/sasl/step/oauthbearer", test_oauthbearer_initial_response);
  g_test_add_func ("/sasl/step/oauthbearer-error-kick", test_oauthbearer_error_kick);
  g_test_add_func ("/sasl/step/scram-sha1-client-first", test_scram_sha1_client_first);
  g_test_add_func ("/sasl/new/login-missing-password", test_login_missing_password);

  return g_test_run ();
}
