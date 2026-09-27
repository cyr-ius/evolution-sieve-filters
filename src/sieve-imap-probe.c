/* sieve-imap-probe.c — see sieve-imap-probe.h
 *
 * Single-shot, blocking helper: no persistent client object, unlike
 * sieve-managesieve-client.c (which stays open for a whole editing
 * session) -- connect, three commands, disconnect.
 */

#include "sieve-imap-probe.h"

#include <string.h>

GQuark
sieve_imap_probe_error_quark (void)
{
  return g_quark_from_static_string ("sieve-imap-probe-error-quark");
}

typedef struct {
  GSocketConnection  *connection;   /* raw TCP connection */
  GIOStream          *active_stream; /* alias of connection, or the TLS layer */
  GDataInputStream   *input;
  GOutputStream      *output;
  guint               timeout_seconds;
} ImapConn;

static void
imap_conn_clear (ImapConn *conn)
{
  g_clear_object (&conn->input);
  conn->output = NULL;

  if (conn->active_stream != NULL) {
    g_io_stream_close (conn->active_stream, NULL, NULL);
    if (conn->connection == NULL || conn->active_stream != G_IO_STREAM (conn->connection))
      g_object_unref (conn->active_stream);
    conn->active_stream = NULL;
  }
  if (conn->connection != NULL) {
    g_io_stream_close (G_IO_STREAM (conn->connection), NULL, NULL);
    g_clear_object (&conn->connection);
  }
}

static void
normalize_transport_error (GError **error)
{
  if (error == NULL || *error == NULL)
    return;
  if (g_error_matches (*error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT)) {
    g_clear_error (error);
    g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
                         "Network timeout exceeded");
  }
}

/* Rebinds input/output to the active stream (called once after the TCP
 * connect, again after a successful StartTLS). See the identical
 * pattern -- and the comment explaining why close_base_stream is
 * disabled -- in sieve-managesieve-client.c. */
static void
rebind_streams (ImapConn *conn)
{
  g_clear_object (&conn->input);
  conn->input = g_data_input_stream_new (g_io_stream_get_input_stream (conn->active_stream));
  g_filter_input_stream_set_close_base_stream (G_FILTER_INPUT_STREAM (conn->input), FALSE);
  g_data_input_stream_set_newline_type (conn->input, G_DATA_STREAM_NEWLINE_TYPE_CR_LF);
  conn->output = g_io_stream_get_output_stream (conn->active_stream);
}

static gchar *
imap_read_line (ImapConn *conn, GCancellable *cancellable, GError **error)
{
  gchar *line = g_data_input_stream_read_line_utf8 (conn->input, NULL, cancellable, error);

  if (line == NULL) {
    normalize_transport_error (error);
    if (error != NULL && *error == NULL)
      g_set_error (error, SIEVE_IMAP_PROBE_ERROR, SIEVE_IMAP_PROBE_ERROR_PROTOCOL,
                   "Connection unexpectedly closed by the server");
  }
  return line;
}

static gboolean
imap_write_line (ImapConn *conn, const gchar *line, GCancellable *cancellable, GError **error)
{
  g_autofree gchar *full = g_strdup_printf ("%s\r\n", line);
  gsize written = 0;
  gboolean ok = g_output_stream_write_all (conn->output, full, strlen (full),
                                           &written, cancellable, error);
  if (!ok)
    normalize_transport_error (error);
  return ok;
}

/* IMAP quoted-string escaping (RFC 3501 §4.3): backslash and double
 * quote escaped with a backslash -- same convention as ManageSieve /
 * Sieve. Control characters (CR, LF) are refused rather than silently
 * dropped: a user/password carrying one could otherwise inject
 * additional IMAP commands into the connection. */
static gchar *
imap_quote_string (const gchar *str, GError **error)
{
  GString *out;

  if (strpbrk (str, "\r\n") != NULL) {
    g_set_error (error, SIEVE_IMAP_PROBE_ERROR, SIEVE_IMAP_PROBE_ERROR_PROTOCOL,
                 "Invalid character in credentials");
    return NULL;
  }

  out = g_string_new ("\"");
  for (const gchar *p = str; *p != '\0'; p++) {
    if (*p == '"' || *p == '\\')
      g_string_append_c (out, '\\');
    g_string_append_c (out, *p);
  }
  g_string_append_c (out, '"');
  return g_string_free (out, FALSE);
}

typedef void (*UntaggedFunc) (const gchar *line, gpointer user_data);

/* Reads lines until the tagged response for `tag` (e.g. "a2") appears.
 * Untagged ("* ...") lines are passed to `on_untagged` (may be NULL).
 * TRUE for "<tag> OK ..."; FALSE + error (code `fail_code`, message =
 * the server's own text) for "<tag> NO/BAD ...", or for a transport /
 * parse failure. */
static gboolean
imap_read_tagged_response (ImapConn *conn, const gchar *tag,
                           UntaggedFunc on_untagged, gpointer user_data,
                           SieveImapProbeError fail_code,
                           GCancellable *cancellable, GError **error)
{
  gsize taglen = strlen (tag);

  while (TRUE) {
    g_autofree gchar *line = imap_read_line (conn, cancellable, error);
    const gchar *rest;

    if (line == NULL)
      return FALSE;

    if (strncmp (line, tag, taglen) == 0 && line[taglen] == ' ') {
      rest = line + taglen + 1;
      if (g_ascii_strncasecmp (rest, "OK", 2) == 0)
        return TRUE;
      if (g_ascii_strncasecmp (rest, "NO", 2) == 0
          || g_ascii_strncasecmp (rest, "BAD", 3) == 0) {
        g_set_error (error, SIEVE_IMAP_PROBE_ERROR, fail_code, "%s", rest);
        return FALSE;
      }
      g_set_error (error, SIEVE_IMAP_PROBE_ERROR, SIEVE_IMAP_PROBE_ERROR_PROTOCOL,
                   "Unexpected response: %s", line);
      return FALSE;
    }

    if (on_untagged != NULL)
      on_untagged (line, user_data);
  }
}

/* ---- Connection (mirrors sieve-managesieve-client.c) ------------------- */

static gboolean
imap_connect (ImapConn *conn, const gchar *host, guint16 port,
             gboolean implicit_tls, GCancellable *cancellable, GError **error)
{
  GSocketClient *client;
  g_autofree gchar *greeting = NULL;

  client = g_socket_client_new ();
  if (implicit_tls)
    g_socket_client_set_tls (client, TRUE);
  g_socket_client_set_timeout (client, conn->timeout_seconds);

  conn->connection = g_socket_client_connect_to_host (client, host, port, cancellable, error);
  g_object_unref (client);
  if (conn->connection == NULL)
    return FALSE;

  conn->active_stream = G_IO_STREAM (conn->connection);
  {
    GSocket *sock = g_socket_connection_get_socket (conn->connection);
    if (sock != NULL)
      g_socket_set_timeout (sock, conn->timeout_seconds);
  }
  rebind_streams (conn);

  /* Single-line greeting: "* OK ...", "* PREAUTH ..." (already
   * authenticated -- treated as success, LOGIN is simply skipped by the
   * caller since it isn't needed) or "* BYE ..." (refused). */
  greeting = imap_read_line (conn, cancellable, error);
  if (greeting == NULL)
    return FALSE;
  if (g_ascii_strncasecmp (greeting, "* BYE", 5) == 0) {
    g_set_error (error, SIEVE_IMAP_PROBE_ERROR, SIEVE_IMAP_PROBE_ERROR_PROTOCOL,
                 "The server refused the connection: %s", greeting);
    return FALSE;
  }

  if (!implicit_tls) {
    GIOStream *tls_stream;
    GSocketConnectable *identity;

    if (!imap_write_line (conn, "a1 STARTTLS", cancellable, error))
      return FALSE;
    if (!imap_read_tagged_response (conn, "a1", NULL, NULL,
                                    SIEVE_IMAP_PROBE_ERROR_PROTOCOL,
                                    cancellable, error))
      return FALSE;

    /* Discard anything buffered in plaintext before the handshake:
     * standard defense against STARTTLS command injection, same as
     * sieve-managesieve-client.c. */
    identity = g_network_address_new (host, port);
    tls_stream = g_tls_client_connection_new (conn->active_stream, identity, error);
    g_object_unref (identity);
    if (tls_stream == NULL)
      return FALSE;

    if (!g_tls_connection_handshake (G_TLS_CONNECTION (tls_stream), cancellable, error)) {
      g_object_unref (tls_stream);
      return FALSE;
    }
    conn->active_stream = tls_stream;
    rebind_streams (conn);
  }

  return TRUE;
}

typedef struct {
  gchar    separator;
  gboolean found;
  gboolean is_nil;
} ListSeparatorCtx;

/* Parses "* LIST (<flags>) <sep> <name>" (RFC 3501 §7.2.2), keeping
 * only the hierarchy separator: either NIL (flat namespace) or a
 * quoted-string, conventionally exactly one character. A separator
 * announced as a literal ("{1}\r\n.") is not handled -- vanishingly
 * rare for a single delimiter character, and the caller falls back to
 * manual entry on any parse failure anyway. Only the first "* LIST"
 * line is used to answer the LIST "" "" query. */
static void
handle_list_untagged (const gchar *line, gpointer user_data)
{
  ListSeparatorCtx *ctx = user_data;
  const gchar *p = line;
  gint depth;

  if (ctx->found)
    return;
  if (g_ascii_strncasecmp (p, "* LIST", 6) != 0)
    return;
  p += 6;
  while (*p == ' ')
    p++;
  if (*p != '(')
    return;

  depth = 0;
  for (; *p != '\0'; p++) {
    if (*p == '(')
      depth++;
    else if (*p == ')') {
      depth--;
      if (depth == 0) {
        p++;
        break;
      }
    }
  }
  while (*p == ' ')
    p++;

  if (g_ascii_strncasecmp (p, "NIL", 3) == 0) {
    ctx->is_nil = TRUE;
    ctx->found = TRUE;
    return;
  }

  if (*p == '"') {
    g_autoptr (GString) sep = g_string_new (NULL);
    const gchar *q = p + 1;

    while (*q != '\0' && *q != '"') {
      if (*q == '\\' && *(q + 1) != '\0')
        q++;
      g_string_append_c (sep, *q);
      q++;
    }
    if (sep->len >= 1) {
      ctx->separator = sep->str[0];
      ctx->found = TRUE;
    }
  }
}

gboolean
sieve_imap_probe_hierarchy_separator_sync (const gchar  *host,
                                           guint16       port,
                                           gboolean      implicit_tls,
                                           const gchar  *user,
                                           const gchar  *password,
                                           guint         timeout_seconds,
                                           GCancellable *cancellable,
                                           gchar        *out_separator,
                                           GError      **error)
{
  ImapConn conn = { 0 };
  ListSeparatorCtx ctx = { 0 };
  g_autofree gchar *quoted_user = NULL;
  g_autofree gchar *quoted_password = NULL;
  gboolean ok;

  g_return_val_if_fail (host != NULL, FALSE);
  g_return_val_if_fail (user != NULL, FALSE);
  g_return_val_if_fail (password != NULL, FALSE);
  g_return_val_if_fail (out_separator != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  conn.timeout_seconds = timeout_seconds;

  ok = imap_connect (&conn, host, port, implicit_tls, cancellable, error);

  if (ok) {
    quoted_user = imap_quote_string (user, error);
    quoted_password = (quoted_user != NULL) ? imap_quote_string (password, error) : NULL;
    ok = quoted_user != NULL && quoted_password != NULL;
  }

  if (ok) {
    g_autofree gchar *cmd = g_strdup_printf ("a2 LOGIN %s %s", quoted_user, quoted_password);

    ok = imap_write_line (&conn, cmd, cancellable, error)
         && imap_read_tagged_response (&conn, "a2", NULL, NULL,
                                       SIEVE_IMAP_PROBE_ERROR_LOGIN,
                                       cancellable, error);
  }

  if (ok) {
    ok = imap_write_line (&conn, "a3 LIST \"\" \"\"", cancellable, error)
         && imap_read_tagged_response (&conn, "a3", handle_list_untagged, &ctx,
                                       SIEVE_IMAP_PROBE_ERROR_PROTOCOL,
                                       cancellable, error);
  }

  if (ok && !ctx.found) {
    g_set_error (error, SIEVE_IMAP_PROBE_ERROR, SIEVE_IMAP_PROBE_ERROR_PROTOCOL,
                 "Could not find the hierarchy separator in the server's response");
    ok = FALSE;
  } else if (ok && ctx.is_nil) {
    g_set_error (error, SIEVE_IMAP_PROBE_ERROR, SIEVE_IMAP_PROBE_ERROR_NO_SEPARATOR,
                 "The server reports a flat namespace (no folder hierarchy separator)");
    ok = FALSE;
  }

  /* Best-effort LOGOUT: never overrides an already-determined result. */
  if (conn.connection != NULL) {
    if (imap_write_line (&conn, "a4 LOGOUT", cancellable, NULL))
      imap_read_tagged_response (&conn, "a4", NULL, NULL,
                                 SIEVE_IMAP_PROBE_ERROR_PROTOCOL, cancellable, NULL);
  }
  imap_conn_clear (&conn);

  if (ok)
    *out_separator = ctx.separator;
  return ok;
}
