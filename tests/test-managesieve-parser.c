/* test-managesieve-parser.c
 *
 * Unit tests for the ManageSieve response parser of
 * src/sieve-managesieve-client.c — no network, no server. The client is
 * attached (sieve_managesieve_client_attach_stream_for_testing()) to
 * in-memory streams carrying canned server bytes, which lets malformed,
 * hostile or merely unusual responses be fed to it deterministically.
 * The round trip against a real Dovecot stays in tests/dovecot/smoke.sh.
 */

#include <glib.h>
#include <gio/gio.h>
#include <string.h>

#include "sieve-managesieve-client.h"
#include "sieve-managesieve-client-private.h"

#define BANNER "\"IMPLEMENTATION\" \"Test\"\r\n" \
               "\"SASL\" \"PLAIN LOGIN\"\r\n" \
               "\"SIEVE\" \"fileinto\"\r\n" \
               "OK \"Ready.\"\r\n"

/* A client attached to a server that will send exactly `server_bytes`
 * (`len` bytes, or strlen() if -1), then EOF. The banner at its head is
 * read here; `banner_error` (optional) receives its outcome. */
static SieveManageSieveClient *
client_with_server (const gchar *server_bytes, gssize len, GError **banner_error)
{
  SieveManageSieveClient *client = sieve_managesieve_client_new ("localhost", 4190, FALSE);
  gsize n = len < 0 ? strlen (server_bytes) : (gsize) len;
  GInputStream *in = g_memory_input_stream_new_from_data (g_memdup2 (server_bytes, n),
                                                          n, g_free);
  GOutputStream *out = g_memory_output_stream_new_resizable ();
  GIOStream *io = g_simple_io_stream_new (in, out);
  GError *error = NULL;
  gboolean ok;

  ok = sieve_managesieve_client_attach_stream_for_testing (client, io, NULL, &error);
  if (banner_error != NULL)
    g_propagate_error (banner_error, error);
  else {
    g_assert_no_error (error);
    g_assert_true (ok);
  }

  g_object_unref (io);
  g_object_unref (in);
  g_object_unref (out);
  return client;
}

/* ---- Banner ------------------------------------------------------------- */

static void
test_banner_quoted (void)
{
  SieveManageSieveClient *client = client_with_server (BANNER, -1, NULL);

  g_assert_cmpstr (sieve_managesieve_client_get_sasl_capability (client), ==,
                   "PLAIN LOGIN");
  g_object_unref (client);
}

/* A capability value sent as a literal is as valid as a quoted one. */
static void
test_banner_literal_value (void)
{
  SieveManageSieveClient *client = client_with_server (
      "\"SASL\" {11}\r\nPLAIN LOGIN\r\nOK\r\n", -1, NULL);

  g_assert_cmpstr (sieve_managesieve_client_get_sasl_capability (client), ==,
                   "PLAIN LOGIN");
  g_object_unref (client);
}

static void
test_banner_eof (void)
{
  GError *error = NULL;
  SieveManageSieveClient *client = client_with_server ("\"SASL\" \"PLAIN\"\r\n", -1, &error);

  g_assert_error (error, SIEVE_MANAGESIEVE_ERROR, SIEVE_MANAGESIEVE_ERROR_PROTOCOL);
  g_clear_error (&error);
  g_object_unref (client);
}

/* A server that never ends its line must not be buffered forever. */
static void
test_line_too_long (void)
{
  gsize n = 70 * 1024;
  gchar *bytes = g_malloc (n);
  GError *error = NULL;
  SieveManageSieveClient *client;

  memset (bytes, 'A', n);
  client = client_with_server (bytes, n, &error);
  g_assert_error (error, SIEVE_MANAGESIEVE_ERROR, SIEVE_MANAGESIEVE_ERROR_PROTOCOL);
  g_assert_nonnull (strstr (error->message, "too long"));

  g_clear_error (&error);
  g_object_unref (client);
  g_free (bytes);
}

static void
test_nul_in_line (void)
{
  static const gchar bytes[] = "\"SASL\" \"PL\0AIN\"\r\nOK\r\n";
  GError *error = NULL;
  SieveManageSieveClient *client = client_with_server (bytes, sizeof bytes - 1, &error);

  g_assert_error (error, SIEVE_MANAGESIEVE_ERROR, SIEVE_MANAGESIEVE_ERROR_PROTOCOL);
  g_clear_error (&error);
  g_object_unref (client);
}

static void
test_literal_oversized (void)
{
  GError *error = NULL;
  SieveManageSieveClient *client = client_with_server (
      "\"SASL\" {99999999999999999999}\r\n", -1, &error);

  g_assert_error (error, SIEVE_MANAGESIEVE_ERROR, SIEVE_MANAGESIEVE_ERROR_PROTOCOL);
  g_assert_nonnull (strstr (error->message, "oversized literal"));
  g_clear_error (&error);
  g_object_unref (client);
}

/* Each literal is under its own cap, but together they exceed what a
 * single response may carry. */
static void
test_response_too_large (void)
{
  gsize lit = 12 * 1024 * 1024;
  GString *bytes = g_string_new (NULL);
  GError *error = NULL;
  SieveManageSieveClient *client;
  guint i;

  for (i = 0; i < 3; i++) {
    g_string_append_printf (bytes, "{%" G_GSIZE_FORMAT "}\r\n", lit);
    g_string_set_size (bytes, bytes->len + lit);
    memset (bytes->str + bytes->len - lit, 'x', lit);
    g_string_append (bytes, "\r\n");
  }
  g_string_append (bytes, "OK\r\n");

  client = client_with_server (bytes->str, bytes->len, &error);
  g_assert_error (error, SIEVE_MANAGESIEVE_ERROR, SIEVE_MANAGESIEVE_ERROR_PROTOCOL);
  g_assert_nonnull (strstr (error->message, "too large"));

  g_clear_error (&error);
  g_object_unref (client);
  g_string_free (bytes, TRUE);
}

/* ---- GETSCRIPT ---------------------------------------------------------- */

/* The CRLF that ends the line after the literal is framing, not content:
 * it must neither be appended to the script nor leak into the next
 * response. */
static void
test_getscript_literal_exact (void)
{
  SieveManageSieveClient *client = client_with_server (
      BANNER
      "{6}\r\nkeep;\n\r\nOK\r\n"
      "\"other\"\r\nOK\r\n", -1, NULL);
  GError *error = NULL;
  gchar *content = sieve_managesieve_client_get_script_sync (client, "s", NULL, &error);
  GPtrArray *names;

  g_assert_no_error (error);
  g_assert_cmpstr (content, ==, "keep;\n");
  g_free (content);

  names = sieve_managesieve_client_list_scripts_sync (client, NULL, NULL, &error);
  g_assert_no_error (error);
  g_assert_cmpuint (names->len, ==, 1);
  g_assert_cmpstr (g_ptr_array_index (names, 0), ==, "other");
  g_ptr_array_unref (names);

  g_object_unref (client);
}

static void
test_getscript_quoted (void)
{
  SieveManageSieveClient *client = client_with_server (
      BANNER "\"keep; # \\\"q\\\"\"\r\nOK\r\n", -1, NULL);
  GError *error = NULL;
  gchar *content = sieve_managesieve_client_get_script_sync (client, "s", NULL, &error);

  g_assert_no_error (error);
  g_assert_cmpstr (content, ==, "keep; # \"q\"");
  g_free (content);
  g_object_unref (client);
}

static void
test_getscript_malformed (void)
{
  SieveManageSieveClient *client = client_with_server (
      BANNER "keep;\r\nOK\r\n", -1, NULL);
  GError *error = NULL;
  gchar *content = sieve_managesieve_client_get_script_sync (client, "s", NULL, &error);

  g_assert_null (content);
  g_assert_error (error, SIEVE_MANAGESIEVE_ERROR, SIEVE_MANAGESIEVE_ERROR_PROTOCOL);
  g_clear_error (&error);
  g_object_unref (client);
}

static void
test_literal_with_nul (void)
{
  static const gchar bytes[] = BANNER "{3}\r\na\0b\r\nOK\r\n";
  SieveManageSieveClient *client = client_with_server (bytes, sizeof bytes - 1, NULL);
  GError *error = NULL;
  gchar *content = sieve_managesieve_client_get_script_sync (client, "s", NULL, &error);

  g_assert_null (content);
  g_assert_error (error, SIEVE_MANAGESIEVE_ERROR, SIEVE_MANAGESIEVE_ERROR_PROTOCOL);
  g_clear_error (&error);
  g_object_unref (client);
}

/* ---- LISTSCRIPTS -------------------------------------------------------- */

static void
test_listscripts (void)
{
  SieveManageSieveClient *client = client_with_server (
      BANNER
      "\"a\"\r\n"
      "\"b \\\"x\\\"\" ACTIVE\r\n"
      "{3}\r\nc d\r\n"
      "{8}\r\nACTIVE e\r\n"
      "OK\r\n", -1, NULL);
  GError *error = NULL;
  gchar *active = NULL;
  GPtrArray *names = sieve_managesieve_client_list_scripts_sync (client, &active, NULL, &error);

  g_assert_no_error (error);
  g_assert_cmpuint (names->len, ==, 4);
  g_assert_cmpstr (g_ptr_array_index (names, 0), ==, "a");
  g_assert_cmpstr (g_ptr_array_index (names, 1), ==, "b \"x\"");
  g_assert_cmpstr (g_ptr_array_index (names, 2), ==, "c d");
  /* A name merely containing "ACTIVE" doesn't mark the script active. */
  g_assert_cmpstr (g_ptr_array_index (names, 3), ==, "ACTIVE e");
  g_assert_cmpstr (active, ==, "b \"x\"");

  g_ptr_array_unref (names);
  g_free (active);
  g_object_unref (client);
}

static void
test_listscripts_literal_active (void)
{
  SieveManageSieveClient *client = client_with_server (
      BANNER "{4}\r\nmain ACTIVE\r\nOK\r\n", -1, NULL);
  GError *error = NULL;
  gchar *active = NULL;
  GPtrArray *names = sieve_managesieve_client_list_scripts_sync (client, &active, NULL, &error);

  g_assert_no_error (error);
  g_assert_cmpuint (names->len, ==, 1);
  g_assert_cmpstr (active, ==, "main");

  g_ptr_array_unref (names);
  g_free (active);
  g_object_unref (client);
}

static void
test_listscripts_malformed (void)
{
  SieveManageSieveClient *client = client_with_server (
      BANNER "\"a\" GARBAGE\r\nOK\r\n", -1, NULL);
  GError *error = NULL;
  gchar *active = NULL;
  GPtrArray *names = sieve_managesieve_client_list_scripts_sync (client, &active, NULL, &error);

  g_assert_null (names);
  g_assert_null (active);
  g_assert_error (error, SIEVE_MANAGESIEVE_ERROR, SIEVE_MANAGESIEVE_ERROR_PROTOCOL);
  g_clear_error (&error);
  g_object_unref (client);
}

/* ---- OK / NO / BYE ------------------------------------------------------ */

/* "NO {N}" (Dovecot's CHECKSCRIPT error): the message is the literal, and
 * the session stays in sync for the next command. */
static void
test_no_literal_then_next_command (void)
{
  SieveManageSieveClient *client = client_with_server (
      BANNER
      "NO {13}\r\nline 1: oops\n\r\n"
      "OK\r\n", -1, NULL);
  GError *error = NULL;

  g_assert_false (sieve_managesieve_client_check_script_sync (client, "x", NULL, &error));
  g_assert_error (error, SIEVE_MANAGESIEVE_ERROR, SIEVE_MANAGESIEVE_ERROR_SERVER_NO);
  g_assert_true (g_str_has_suffix (error->message, ": line 1: oops\n"));
  g_clear_error (&error);

  g_assert_true (sieve_managesieve_client_set_active_sync (client, "x", NULL, &error));
  g_assert_no_error (error);
  g_object_unref (client);
}

static void
test_no_response_code (void)
{
  SieveManageSieveClient *client = client_with_server (
      BANNER "NO (NONEXISTENT) \"Script does not exist.\"\r\n", -1, NULL);
  GError *error = NULL;

  g_assert_false (sieve_managesieve_client_delete_script_sync (client, "x", NULL, &error));
  g_assert_error (error, SIEVE_MANAGESIEVE_ERROR, SIEVE_MANAGESIEVE_ERROR_SERVER_NO);
  g_assert_cmpstr (error->message, ==,
                   "The server rejected the command: Script does not exist.");
  g_clear_error (&error);
  g_object_unref (client);
}

static void
test_bye (void)
{
  SieveManageSieveClient *client = client_with_server (
      BANNER "BYE \"Shutting down\"\r\n", -1, NULL);
  GError *error = NULL;

  g_assert_false (sieve_managesieve_client_delete_script_sync (client, "x", NULL, &error));
  g_assert_error (error, SIEVE_MANAGESIEVE_ERROR, SIEVE_MANAGESIEVE_ERROR_SERVER_BYE);
  g_clear_error (&error);
  g_object_unref (client);
}

/* "NO"/"OK" only count as whole tokens: "NONSENSE" isn't a refusal. */
static void
test_keyword_whole_token (void)
{
  SieveManageSieveClient *client = client_with_server (
      BANNER "NONSENSE\r\nOKAY\r\nOK\r\n", -1, NULL);
  GError *error = NULL;

  g_assert_true (sieve_managesieve_client_set_active_sync (client, "x", NULL, &error));
  g_assert_no_error (error);
  g_object_unref (client);
}

/* ---- AUTHENTICATE ------------------------------------------------------- */

static gboolean
authenticate (SieveManageSieveClient *client, const gchar *mech, GError **error)
{
  SieveManageSieveAuth auth = { .authid = "alice", .password = "pw" };
  return sieve_managesieve_client_authenticate_sync (client, mech, &auth, NULL, error);
}

/* "OK {N}": the final message as a literal is accepted like a quoted one. */
static void
test_auth_ok_literal (void)
{
  SieveManageSieveClient *client = client_with_server (
      BANNER "OK {6}\r\nLogged\r\n", -1, NULL);
  GError *error = NULL;

  g_assert_true (authenticate (client, "PLAIN", &error));
  g_assert_no_error (error);
  g_assert_cmpstr (sieve_managesieve_client_get_auth_mechanism (client), ==, "PLAIN");
  g_object_unref (client);
}

/* A SASL challenge sent as a literal (libgsasl's LOGIN puts the username
 * in its initial response: one challenge, for the password, remains). */
static void
test_auth_literal_challenge (void)
{
  SieveManageSieveClient *client = client_with_server (
      BANNER
      "{12}\r\nUGFzc3dvcmQ6\r\n"   /* "Password:" */
      "OK\r\n", -1, NULL);
  GError *error = NULL;

  gboolean ok = authenticate (client, "LOGIN", &error);
  g_assert_no_error (error);
  g_assert_true (ok);
  g_object_unref (client);
}

static void
test_auth_bad_base64 (void)
{
  SieveManageSieveClient *client = client_with_server (
      BANNER "\"not base64!\"\r\n", -1, NULL);
  GError *error = NULL;

  g_assert_false (authenticate (client, "LOGIN", &error));
  g_assert_error (error, SIEVE_MANAGESIEVE_ERROR, SIEVE_MANAGESIEVE_ERROR_PROTOCOL);
  g_clear_error (&error);
  g_object_unref (client);
}

static void
test_auth_unquoted_challenge (void)
{
  SieveManageSieveClient *client = client_with_server (
      BANNER "VXNlcm5hbWU6\r\n", -1, NULL);
  GError *error = NULL;

  g_assert_false (authenticate (client, "LOGIN", &error));
  g_assert_error (error, SIEVE_MANAGESIEVE_ERROR, SIEVE_MANAGESIEVE_ERROR_PROTOCOL);
  g_clear_error (&error);
  g_object_unref (client);
}

static void
test_auth_bad_sasl_response_code (void)
{
  SieveManageSieveClient *client = client_with_server (
      BANNER "OK (SASL \"%%%\")\r\n", -1, NULL);
  GError *error = NULL;

  g_assert_false (authenticate (client, "PLAIN", &error));
  g_assert_error (error, SIEVE_MANAGESIEVE_ERROR, SIEVE_MANAGESIEVE_ERROR_PROTOCOL);
  g_clear_error (&error);
  g_object_unref (client);
}

static void
test_auth_no (void)
{
  SieveManageSieveClient *client = client_with_server (
      BANNER "NO \"Authentication failed.\"\r\n", -1, NULL);
  GError *error = NULL;

  g_assert_false (authenticate (client, "PLAIN", &error));
  g_assert_error (error, SIEVE_MANAGESIEVE_ERROR, SIEVE_MANAGESIEVE_ERROR_AUTH);
  g_assert_true (g_str_has_suffix (error->message, "Authentication failed."));
  g_clear_error (&error);
  g_object_unref (client);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/parser/banner/quoted", test_banner_quoted);
  g_test_add_func ("/parser/banner/literal-value", test_banner_literal_value);
  g_test_add_func ("/parser/banner/eof", test_banner_eof);
  g_test_add_func ("/parser/limits/line-too-long", test_line_too_long);
  g_test_add_func ("/parser/limits/nul-in-line", test_nul_in_line);
  g_test_add_func ("/parser/limits/literal-oversized", test_literal_oversized);
  g_test_add_func ("/parser/limits/response-too-large", test_response_too_large);
  g_test_add_func ("/parser/getscript/literal-exact", test_getscript_literal_exact);
  g_test_add_func ("/parser/getscript/quoted", test_getscript_quoted);
  g_test_add_func ("/parser/getscript/malformed", test_getscript_malformed);
  g_test_add_func ("/parser/getscript/literal-with-nul", test_literal_with_nul);
  g_test_add_func ("/parser/listscripts/mixed", test_listscripts);
  g_test_add_func ("/parser/listscripts/literal-active", test_listscripts_literal_active);
  g_test_add_func ("/parser/listscripts/malformed", test_listscripts_malformed);
  g_test_add_func ("/parser/status/no-literal-then-next", test_no_literal_then_next_command);
  g_test_add_func ("/parser/status/no-response-code", test_no_response_code);
  g_test_add_func ("/parser/status/bye", test_bye);
  g_test_add_func ("/parser/status/keyword-whole-token", test_keyword_whole_token);
  g_test_add_func ("/parser/auth/ok-literal", test_auth_ok_literal);
  g_test_add_func ("/parser/auth/literal-challenge", test_auth_literal_challenge);
  g_test_add_func ("/parser/auth/bad-base64", test_auth_bad_base64);
  g_test_add_func ("/parser/auth/unquoted-challenge", test_auth_unquoted_challenge);
  g_test_add_func ("/parser/auth/bad-sasl-response-code", test_auth_bad_sasl_response_code);
  g_test_add_func ("/parser/auth/no", test_auth_no);

  return g_test_run ();
}
