/* test-imap-probe-parser.c
 *
 * Unit tests for the response reader of src/sieve-imap-probe.c — no
 * network, no server. The probe is run
 * (sieve_imap_probe_hierarchy_separator_on_stream_for_testing()) on
 * in-memory streams carrying canned server bytes, which lets malformed or
 * hostile responses be fed to it deterministically. The round trip
 * against a real Dovecot stays in tests/dovecot/smoke.sh.
 */

#include <glib.h>
#include <gio/gio.h>
#include <string.h>

#include "sieve-imap-probe.h"
#include "sieve-imap-probe-private.h"

/* Mirrors the caps in sieve-imap-probe.c. */
#define MAX_LINE_SIZE     (64 * 1024)
#define MAX_RESPONSE_SIZE (1024 * 1024)

#define GREETING   "* OK [CAPABILITY IMAP4rev1 LOGIN] Ready.\r\n"
#define LOGIN_OK   "a2 OK Logged in\r\n"
#define LIST_DOT   "* LIST (\\Noselect) \".\" \"\"\r\na3 OK List completed\r\n"
#define LOGOUT_OK  "* BYE Logging out\r\na4 OK Logout completed\r\n"

/* Runs the probe against a server that will send exactly `server_bytes`
 * (`len` bytes), then EOF. `out_sent` (optional) receives what the probe
 * wrote. */
static gboolean
probe_with_server (const gchar *server_bytes, gsize len, gchar *out_separator,
                   gchar **out_sent, GError **error)
{
  GInputStream *in = g_memory_input_stream_new_from_data (g_memdup2 (server_bytes, len),
                                                          len, g_free);
  GOutputStream *out = g_memory_output_stream_new_resizable ();
  GIOStream *io = g_simple_io_stream_new (in, out);
  gboolean ok;

  ok = sieve_imap_probe_hierarchy_separator_on_stream_for_testing (io, "testuser", "testpass",
                                                                   NULL, out_separator, error);
  if (out_sent != NULL)
    *out_sent = g_strndup (g_memory_output_stream_get_data (G_MEMORY_OUTPUT_STREAM (out)),
                           g_memory_output_stream_get_data_size (G_MEMORY_OUTPUT_STREAM (out)));

  g_object_unref (io);
  g_object_unref (in);
  g_object_unref (out);
  return ok;
}

static gboolean
probe_with_string (GString *server, gchar *out_separator, GError **error)
{
  return probe_with_server (server->str, server->len, out_separator, NULL, error);
}

/* `n` untagged lines of exactly `line_len` bytes each (CRLF included). */
static void
append_untagged_flood (GString *s, gsize n, gsize line_len)
{
  for (gsize i = 0; i < n; i++) {
    g_string_append (s, "* OK ");
    for (gsize j = 5; j < line_len - 2; j++)
      g_string_append_c (s, 'x');
    g_string_append (s, "\r\n");
  }
}

/* ---- Normal flow -------------------------------------------------------- */

static void
test_separator_found (void)
{
  const gchar *bytes = GREETING LOGIN_OK LIST_DOT LOGOUT_OK;
  g_autofree gchar *sent = NULL;
  GError *error = NULL;
  gchar sep = 0;

  g_assert_true (probe_with_server (bytes, strlen (bytes), &sep, &sent, &error));
  g_assert_no_error (error);
  g_assert_cmpint (sep, ==, '.');
  g_assert_cmpstr (sent, ==,
                   "a2 LOGIN \"testuser\" \"testpass\"\r\n"
                   "a3 LIST \"\" \"\"\r\n"
                   "a4 LOGOUT\r\n");
}

/* A lone LF is tolerated, as in the ManageSieve client. */
static void
test_lone_lf (void)
{
  const gchar *bytes = "* OK Ready.\na2 OK\n* LIST () \"/\" \"\"\na3 OK\n";
  GError *error = NULL;
  gchar sep = 0;

  g_assert_true (probe_with_server (bytes, strlen (bytes), &sep, NULL, &error));
  g_assert_no_error (error);
  g_assert_cmpint (sep, ==, '/');
}

static void
test_eof (void)
{
  const gchar *bytes = GREETING LOGIN_OK;
  GError *error = NULL;
  gchar sep = 0;

  g_assert_false (probe_with_server (bytes, strlen (bytes), &sep, NULL, &error));
  g_assert_error (error, SIEVE_IMAP_PROBE_ERROR, SIEVE_IMAP_PROBE_ERROR_PROTOCOL);
  g_assert_nonnull (strstr (error->message, "closed"));
  g_clear_error (&error);
}

/* ---- Line cap ----------------------------------------------------------- */

/* A greeting exactly at the cap is still accepted. */
static void
test_line_at_cap (void)
{
  g_autoptr (GString) s = g_string_new ("* OK ");
  GError *error = NULL;
  gchar sep = 0;

  while (s->len < MAX_LINE_SIZE)
    g_string_append_c (s, 'x');
  g_string_append (s, "\r\n" LOGIN_OK LIST_DOT LOGOUT_OK);

  g_assert_true (probe_with_string (s, &sep, &error));
  g_assert_no_error (error);
  g_assert_cmpint (sep, ==, '.');
}

/* A greeting that never ends (what an on-path attacker can send before
 * STARTTLS) is refused once the cap is crossed, not buffered until EOF. */
static void
test_greeting_without_crlf (void)
{
  g_autoptr (GString) s = g_string_new ("* OK ");
  GError *error = NULL;
  gchar sep = 0;

  while (s->len < 4 * MAX_LINE_SIZE)
    g_string_append_c (s, 'x');

  g_assert_false (probe_with_string (s, &sep, &error));
  g_assert_error (error, SIEVE_IMAP_PROBE_ERROR, SIEVE_IMAP_PROBE_ERROR_PROTOCOL);
  g_assert_nonnull (strstr (error->message, "too long"));
  g_clear_error (&error);
}

/* Same, inside a tagged response's untagged lines. */
static void
test_untagged_line_too_long (void)
{
  g_autoptr (GString) s = g_string_new (GREETING LOGIN_OK "* LIST () \".\" \"");
  GError *error = NULL;
  gchar sep = 0;

  while (s->len < 4 * MAX_LINE_SIZE)
    g_string_append_c (s, 'x');
  g_string_append (s, "\"\r\na3 OK\r\n");

  g_assert_false (probe_with_string (s, &sep, &error));
  g_assert_error (error, SIEVE_IMAP_PROBE_ERROR, SIEVE_IMAP_PROBE_ERROR_PROTOCOL);
  g_assert_nonnull (strstr (error->message, "too long"));
  g_clear_error (&error);
}

/* ---- Response budget ---------------------------------------------------- */

/* An endless stream of short, individually valid untagged lines is cut
 * off by the per-response budget. */
static void
test_untagged_flood (void)
{
  g_autoptr (GString) s = g_string_new (GREETING LOGIN_OK);
  GError *error = NULL;
  gchar sep = 0;

  append_untagged_flood (s, 2 * MAX_RESPONSE_SIZE / 100, 100);
  g_string_append (s, LIST_DOT);

  g_assert_false (probe_with_string (s, &sep, &error));
  g_assert_error (error, SIEVE_IMAP_PROBE_ERROR, SIEVE_IMAP_PROBE_ERROR_PROTOCOL);
  g_assert_nonnull (strstr (error->message, "too large"));
  g_clear_error (&error);
}

/* The budget is per response: two responses each under it pass, even if
 * their sum is over it. */
static void
test_budget_is_per_response (void)
{
  g_autoptr (GString) s = g_string_new (GREETING);
  GError *error = NULL;
  gchar sep = 0;

  append_untagged_flood (s, (MAX_RESPONSE_SIZE * 3 / 4) / 100, 100);
  g_string_append (s, LOGIN_OK);
  append_untagged_flood (s, (MAX_RESPONSE_SIZE * 3 / 4) / 100, 100);
  g_string_append (s, LIST_DOT LOGOUT_OK);

  g_assert_true (probe_with_string (s, &sep, &error));
  g_assert_no_error (error);
  g_assert_cmpint (sep, ==, '.');
}

/* ---- Content checks ----------------------------------------------------- */

static void
test_nul_byte (void)
{
  static const gchar bytes[] = "* OK Re\0dy\r\n";
  GError *error = NULL;
  gchar sep = 0;

  g_assert_false (probe_with_server (bytes, sizeof bytes - 1, &sep, NULL, &error));
  g_assert_error (error, SIEVE_IMAP_PROBE_ERROR, SIEVE_IMAP_PROBE_ERROR_PROTOCOL);
  g_assert_nonnull (strstr (error->message, "NUL"));
  g_clear_error (&error);
}

static void
test_invalid_utf8 (void)
{
  const gchar *bytes = "* OK \xff\xfe\r\n";
  GError *error = NULL;
  gchar sep = 0;

  g_assert_false (probe_with_server (bytes, strlen (bytes), &sep, NULL, &error));
  g_assert_error (error, SIEVE_IMAP_PROBE_ERROR, SIEVE_IMAP_PROBE_ERROR_PROTOCOL);
  g_assert_nonnull (strstr (error->message, "UTF-8"));
  g_clear_error (&error);
}

/* A hostile/broken server announcing a non-ASCII separator ("é", two
 * UTF-8 bytes), a multi-character one, or one that would need escaping
 * in a Sieve string is refused -- never truncated to its first byte. */
static void
test_invalid_separator (void)
{
  const gchar *seps[] = { "\xc3\xa9", "..", "\\\"", "\\\\", " " };

  for (gsize i = 0; i < G_N_ELEMENTS (seps); i++) {
    g_autofree gchar *bytes =
      g_strdup_printf (GREETING LOGIN_OK
                       "* LIST (\\Noselect) \"%s\" \"\"\r\na3 OK List completed\r\n"
                       LOGOUT_OK, seps[i]);
    GError *error = NULL;
    gchar sep = 0;

    g_assert_false (probe_with_server (bytes, strlen (bytes), &sep, NULL, &error));
    g_assert_error (error, SIEVE_IMAP_PROBE_ERROR, SIEVE_IMAP_PROBE_ERROR_PROTOCOL);
    g_assert_nonnull (strstr (error->message, "unsupported hierarchy separator"));
    g_assert_cmpint (sep, ==, 0);
    g_clear_error (&error);
  }
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/imap-probe/separator-found", test_separator_found);
  g_test_add_func ("/imap-probe/lone-lf", test_lone_lf);
  g_test_add_func ("/imap-probe/eof", test_eof);
  g_test_add_func ("/imap-probe/line/at-cap", test_line_at_cap);
  g_test_add_func ("/imap-probe/line/greeting-without-crlf", test_greeting_without_crlf);
  g_test_add_func ("/imap-probe/line/untagged-too-long", test_untagged_line_too_long);
  g_test_add_func ("/imap-probe/budget/untagged-flood", test_untagged_flood);
  g_test_add_func ("/imap-probe/budget/per-response", test_budget_is_per_response);
  g_test_add_func ("/imap-probe/content/nul-byte", test_nul_byte);
  g_test_add_func ("/imap-probe/content/invalid-utf8", test_invalid_utf8);
  g_test_add_func ("/imap-probe/content/invalid-separator", test_invalid_separator);

  return g_test_run ();
}
