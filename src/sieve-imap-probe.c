/* sieve-imap-probe.c — see sieve-imap-probe.h
 *
 * Single-shot, blocking helper: no persistent client object, unlike
 * sieve-managesieve-client.c (which stays open for a whole editing
 * session) -- connect, three commands, disconnect.
 */

#include "sieve-imap-probe.h"
#include "sieve-imap-probe-private.h"
#include "sieve-folder-separator.h"

#include <string.h>

/* Longest physical response line accepted (CRLF excluded). The probe only
 * ever reads a greeting, a CAPABILITY, one LIST line and tagged
 * statuses -- all short: without a bound, a server (or, before STARTTLS,
 * anyone on the network path) that never sends CRLF would make the
 * process -- Evolution itself -- buffer forever. Same cap as
 * SIEVE_MANAGESIEVE_MAX_LINE_SIZE in sieve-managesieve-client.c. */
#define SIEVE_IMAP_PROBE_MAX_LINE_SIZE (64 * 1024)

/* Total bytes accepted for a single response (the greeting, or every
 * untagged line up to and including a tagged status). Bounds what the
 * line cap can't on its own: an endless stream of individually valid
 * untagged lines. Far above anything LOGIN / LIST "" "" legitimately
 * produce. */
#define SIEVE_IMAP_PROBE_MAX_RESPONSE_SIZE (1024 * 1024)

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
  gsize               response_budget; /* bytes still accepted for the
                                        * response being read (see
                                        * MAX_RESPONSE_SIZE) */
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

static void
set_protocol_error (GError **error, const gchar *message)
{
  g_set_error_literal (error, SIEVE_IMAP_PROBE_ERROR,
                       SIEVE_IMAP_PROBE_ERROR_PROTOCOL, message);
}

/* Reads a physical line (without its line terminator). NULL + error on
 * failure. Bounded (SIEVE_IMAP_PROBE_MAX_LINE_SIZE per line, and the
 * current response's budget), unlike g_data_input_stream_read_line(),
 * which buffers until it finds a terminator. Same logic as read_line()
 * in sieve-managesieve-client.c: CRLF or a lone LF accepted, NUL bytes
 * and invalid UTF-8 rejected. */
static gchar *
imap_read_line (ImapConn *conn, GCancellable *cancellable, GError **error)
{
  GBufferedInputStream *input = G_BUFFERED_INPUT_STREAM (conn->input);
  GString *line = g_string_new (NULL);

  while (TRUE) {
    gsize avail;
    const gchar *buf = g_buffered_input_stream_peek_buffer (input, &avail);
    const gchar *nl;
    gsize take;

    if (avail == 0) {
      gssize n = g_buffered_input_stream_fill (input, -1, cancellable, error);

      if (n < 0) {
        normalize_transport_error (error);
        goto fail;
      }
      if (n == 0) {
        set_protocol_error (error, "Connection unexpectedly closed by the server");
        goto fail;
      }
      continue;
    }

    nl = memchr (buf, '\n', avail);
    take = nl != NULL ? (gsize) (nl - buf) + 1 : avail;

    /* +2: the terminator doesn't count against the line itself. */
    if (line->len + take > SIEVE_IMAP_PROBE_MAX_LINE_SIZE + 2) {
      set_protocol_error (error, "Server response line too long");
      goto fail;
    }
    if (take > conn->response_budget) {
      set_protocol_error (error, "Server response too large");
      goto fail;
    }
    conn->response_budget -= take;

    g_string_append_len (line, buf, take);
    /* Data already buffered: skipping it never blocks. */
    g_input_stream_skip (G_INPUT_STREAM (input), take, NULL, NULL);

    if (nl != NULL)
      break;
  }

  g_string_truncate (line, line->len - 1);                 /* '\n' */
  if (line->len > 0 && line->str[line->len - 1] == '\r')
    g_string_truncate (line, line->len - 1);

  if (memchr (line->str, '\0', line->len) != NULL) {
    set_protocol_error (error, "NUL byte in a server response line");
    goto fail;
  }
  if (!g_utf8_validate (line->str, line->len, NULL)) {
    set_protocol_error (error, "Invalid UTF-8 in a server response line");
    goto fail;
  }
  return g_string_free (line, FALSE);

fail:
  g_string_free (line, TRUE);
  return NULL;
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

  conn->response_budget = SIEVE_IMAP_PROBE_MAX_RESPONSE_SIZE;

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

/* Single-line greeting: "* OK ...", "* PREAUTH ..." (already
 * authenticated -- treated as success, LOGIN is simply skipped by the
 * caller since it isn't needed) or "* BYE ..." (refused). */
static gboolean
imap_read_greeting (ImapConn *conn, GCancellable *cancellable, GError **error)
{
  g_autofree gchar *greeting = NULL;

  conn->response_budget = SIEVE_IMAP_PROBE_MAX_RESPONSE_SIZE;
  greeting = imap_read_line (conn, cancellable, error);
  if (greeting == NULL)
    return FALSE;
  if (g_ascii_strncasecmp (greeting, "* BYE", 5) == 0) {
    g_set_error (error, SIEVE_IMAP_PROBE_ERROR, SIEVE_IMAP_PROBE_ERROR_PROTOCOL,
                 "The server refused the connection: %s", greeting);
    return FALSE;
  }
  return TRUE;
}

static gboolean
imap_connect (ImapConn *conn, const gchar *host, guint16 port,
             gboolean implicit_tls, GCancellable *cancellable, GError **error)
{
  GSocketClient *client;

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

  if (!imap_read_greeting (conn, cancellable, error))
    return FALSE;

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
  gboolean invalid;   /* found, but not a usable separator */
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
    /* Exactly one valid character, never the first byte of something
     * longer: a hostile server answering "é" must not leave a lone
     * 0xC3 in fileinto paths (see sieve-folder-separator.h). */
    if (sep->len >= 1) {
      ctx->found = TRUE;
      if (sieve_folder_separator_text_is_valid (sep->str))
        ctx->separator = sep->str[0];
      else
        ctx->invalid = TRUE;
    }
  }
}

/* LOGIN + LIST "" "" + best-effort LOGOUT on an already-connected (and
 * greeted) `conn`, which is always cleared on return. */
static gboolean
imap_probe_session (ImapConn     *conn,
                    const gchar  *user,
                    const gchar  *password,
                    GCancellable *cancellable,
                    gchar        *out_separator,
                    GError      **error)
{
  ListSeparatorCtx ctx = { 0 };
  g_autofree gchar *quoted_user = NULL;
  g_autofree gchar *quoted_password = NULL;
  gboolean ok;

  quoted_user = imap_quote_string (user, error);
  quoted_password = (quoted_user != NULL) ? imap_quote_string (password, error) : NULL;
  ok = quoted_user != NULL && quoted_password != NULL;

  if (ok) {
    g_autofree gchar *cmd = g_strdup_printf ("a2 LOGIN %s %s", quoted_user, quoted_password);

    ok = imap_write_line (conn, cmd, cancellable, error)
         && imap_read_tagged_response (conn, "a2", NULL, NULL,
                                       SIEVE_IMAP_PROBE_ERROR_LOGIN,
                                       cancellable, error);
  }

  if (ok) {
    ok = imap_write_line (conn, "a3 LIST \"\" \"\"", cancellable, error)
         && imap_read_tagged_response (conn, "a3", handle_list_untagged, &ctx,
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
  } else if (ok && ctx.invalid) {
    g_set_error (error, SIEVE_IMAP_PROBE_ERROR, SIEVE_IMAP_PROBE_ERROR_PROTOCOL,
                 "The server reports an unsupported hierarchy separator "
                 "(only one printable ASCII character other than '\"' and '\\' "
                 "is supported)");
    ok = FALSE;
  }

  /* Best-effort LOGOUT: never overrides an already-determined result. */
  if (imap_write_line (conn, "a4 LOGOUT", cancellable, NULL))
    imap_read_tagged_response (conn, "a4", NULL, NULL,
                               SIEVE_IMAP_PROBE_ERROR_PROTOCOL, cancellable, NULL);
  imap_conn_clear (conn);

  if (ok)
    *out_separator = ctx.separator;
  return ok;
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

  g_return_val_if_fail (host != NULL, FALSE);
  g_return_val_if_fail (user != NULL, FALSE);
  g_return_val_if_fail (password != NULL, FALSE);
  g_return_val_if_fail (out_separator != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  conn.timeout_seconds = timeout_seconds;

  if (!imap_connect (&conn, host, port, implicit_tls, cancellable, error)) {
    imap_conn_clear (&conn);
    return FALSE;
  }
  return imap_probe_session (&conn, user, password, cancellable, out_separator, error);
}

gboolean
sieve_imap_probe_hierarchy_separator_on_stream_for_testing (GIOStream    *stream,
                                                            const gchar  *user,
                                                            const gchar  *password,
                                                            GCancellable *cancellable,
                                                            gchar        *out_separator,
                                                            GError      **error)
{
  ImapConn conn = { 0 };

  g_return_val_if_fail (G_IS_IO_STREAM (stream), FALSE);
  g_return_val_if_fail (user != NULL, FALSE);
  g_return_val_if_fail (password != NULL, FALSE);
  g_return_val_if_fail (out_separator != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  conn.active_stream = g_object_ref (stream);
  rebind_streams (&conn);

  if (!imap_read_greeting (&conn, cancellable, error)) {
    imap_conn_clear (&conn);
    return FALSE;
  }
  return imap_probe_session (&conn, user, password, cancellable, out_separator, error);
}
