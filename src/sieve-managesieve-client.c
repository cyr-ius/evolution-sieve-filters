/* sieve-managesieve-client.c
 *
 * Minimal but functional implementation of the ManageSieve protocol.
 * See the limitations listed in the .h before any production use.
 */

#include "sieve-managesieve-client.h"
#include "sieve-sasl.h"
#include <errno.h>
#include <string.h>

/* Sieve scripts are always small text; a server announcing a "{N}" literal
 * this large can only be a protocol error or an attempt at resource
 * exhaustion (a malicious/compromised server, or a MITM attacker on the
 * plaintext banner before StartTLS). Cap it well above any plausible
 * script size instead of trusting the network-supplied length and calling
 * g_malloc() on it unchecked. */
#define SIEVE_MANAGESIEVE_MAX_LITERAL_SIZE (16 * 1024 * 1024)

struct _SieveManageSieveClient {
  GObject parent_instance;

  gchar *host;
  guint16 port;
  gboolean implicit_tls;
  guint timeout_seconds;           /* 0 = no limit; see set_timeout() */

  GSocketConnection *connection;   /* raw TCP connection (always owned) */
  GIOStream *active_stream;        /* current stream: alias of connection, or the TLS layer
                                    * from an explicit StartTLS (then owned separately) */
  GDataInputStream *input;
  GOutputStream *output;

  GHashTable *capabilities;        /* name (uppercase) -> value (may be NULL) */

  gchar *auth_mechanism;           /* SASL mechanism negotiated on the last successful auth, or NULL */
};

G_DEFINE_TYPE (SieveManageSieveClient, sieve_managesieve_client, G_TYPE_OBJECT)

GQuark
sieve_managesieve_error_quark (void)
{
  return g_quark_from_static_string ("sieve-managesieve-error-quark");
}

static void
sieve_managesieve_client_finalize (GObject *object)
{
  SieveManageSieveClient *self = SIEVE_MANAGESIEVE_CLIENT (object);

  sieve_managesieve_client_disconnect (self);
  g_clear_pointer (&self->host, g_free);
  g_clear_pointer (&self->auth_mechanism, g_free);
  g_clear_pointer (&self->capabilities, g_hash_table_unref);

  G_OBJECT_CLASS (sieve_managesieve_client_parent_class)->finalize (object);
}

static void
sieve_managesieve_client_class_init (SieveManageSieveClientClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = sieve_managesieve_client_finalize;
}

static void
sieve_managesieve_client_init (SieveManageSieveClient *self)
{
  self->capabilities = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  self->timeout_seconds = SIEVE_MANAGESIEVE_DEFAULT_TIMEOUT_SECONDS;
}

SieveManageSieveClient *
sieve_managesieve_client_new (const gchar *host, guint16 port, gboolean implicit_tls)
{
  SieveManageSieveClient *self;

  g_return_val_if_fail (host != NULL, NULL);

  self = g_object_new (SIEVE_TYPE_MANAGESIEVE_CLIENT, NULL);
  self->host = g_strdup (host);
  self->port = port;
  self->implicit_tls = implicit_tls;

  return self;
}

/* Applies the current timeout to the already-open socket (no effect while
 * the connection isn't established yet; connect_sync handles it then). */
static void
apply_timeout_to_socket (SieveManageSieveClient *self)
{
  GSocket *sock;

  if (self->connection == NULL)
    return;
  sock = g_socket_connection_get_socket (self->connection);
  if (sock != NULL)
    g_socket_set_timeout (sock, self->timeout_seconds);
}

void
sieve_managesieve_client_set_timeout (SieveManageSieveClient *self, guint timeout_seconds)
{
  g_return_if_fail (SIEVE_IS_MANAGESIEVE_CLIENT (self));
  self->timeout_seconds = timeout_seconds;
  apply_timeout_to_socket (self);
}

guint
sieve_managesieve_client_get_timeout (SieveManageSieveClient *self)
{
  g_return_val_if_fail (SIEVE_IS_MANAGESIEVE_CLIENT (self), 0);
  return self->timeout_seconds;
}

/* ---- Low-level read / write --------------------------------------------- */

/* Normalizes transport errors: a GIO timeout (G_IO_ERROR_TIMED_OUT, raised
 * by the socket when g_socket_set_timeout is armed) becomes an error in
 * the client's own domain, with a clear message. Other errors — including
 * G_IO_ERROR_CANCELLED, which the caller must be able to distinguish —
 * are left as-is. */
static void
normalize_transport_error (GError **error)
{
  if (error == NULL || *error == NULL)
    return;
  if (g_error_matches (*error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT)) {
    g_clear_error (error);
    g_set_error_literal (error, SIEVE_MANAGESIEVE_ERROR,
                         SIEVE_MANAGESIEVE_ERROR_TIMEOUT,
                         "Network timeout exceeded");
  }
}

/* Reads a text line (without the trailing CRLF). NULL + error on failure. */
static gchar *
read_line (SieveManageSieveClient *self, GCancellable *cancellable, GError **error)
{
  gchar *line;
  gsize len;

  line = g_data_input_stream_read_line_utf8 (self->input, &len, cancellable, error);
  if (line == NULL) {
    normalize_transport_error (error);
    if (error != NULL && *error == NULL) {
      g_set_error (error, SIEVE_MANAGESIEVE_ERROR, SIEVE_MANAGESIEVE_ERROR_PROTOCOL,
                   "Connection unexpectedly closed by the server");
    }
  }
  return line;
}

/* Writes `len` raw bytes to the current output stream, translating any
 * timeout that occurs. Used by write_line() and by literal sends
 * (PUTSCRIPT / CHECKSCRIPT). */
static gboolean
write_bytes (SieveManageSieveClient *self, const gchar *data, gsize len,
             GCancellable *cancellable, GError **error)
{
  gsize written = 0;
  gboolean ok = g_output_stream_write_all (self->output, data, len,
                                            &written, cancellable, error);
  if (!ok)
    normalize_transport_error (error);
  return ok;
}

static gboolean
write_line (SieveManageSieveClient *self, const gchar *line,
            GCancellable *cancellable, GError **error)
{
  gchar *full = g_strdup_printf ("%s\r\n", line);
  gboolean ok = write_bytes (self, full, strlen (full), cancellable, error);
  g_free (full);
  return ok;
}

/* ManageSieve uses a "quoted-string" format (like IMAP/Sieve: enclosed in
 * double quotes, backslash and quote escaped with a backslash), NOT shell
 * quoting. g_shell_quote()/g_shell_unquote() produce a different result
 * (single quotes): using them here generated syntactically invalid
 * commands for the server (bug found during the first real test against
 * Dovecot — see project history). */
static gchar *
managesieve_quote_string (const gchar *str)
{
  GString *out = g_string_new ("\"");
  for (const gchar *p = str; *p != '\0'; p++) {
    if (*p == '"' || *p == '\\')
      g_string_append_c (out, '\\');
    g_string_append_c (out, *p);
  }
  g_string_append_c (out, '"');
  return g_string_free (out, FALSE);
}

/* Unescapes a ManageSieve quoted-string. If the string doesn't start/end
 * with quotes (unquoted atoms, rare here), returns it as-is. */
static gchar *
managesieve_unquote_string (const gchar *str)
{
  gsize len = strlen (str);
  GString *out;
  gsize i;

  if (len < 2 || str[0] != '"' || str[len - 1] != '"')
    return g_strdup (str);

  out = g_string_new (NULL);
  for (i = 1; i < len - 1; i++) {
    if (str[i] == '\\' && i + 1 < len - 1) {
      i++;
      g_string_append_c (out, str[i]);
    } else {
      g_string_append_c (out, str[i]);
    }
  }
  return g_string_free (out, FALSE);
}

/* Extracts at most two quoted-strings at the head of a line: "NAME" ["VALUE"]
 * (capability line format, RFC 5804 §1.7, e.g. "STARTTLS" or
 * "SASL" "PLAIN LOGIN"). Returns FALSE if the line doesn't start with a
 * valid quoted-string (capabilities as literals aren't handled here — see
 * the limitations in the .h file). *out_key is uppercased for a
 * case-insensitive lookup; *out_value may remain NULL. */
static gboolean
parse_capability_line (const gchar *line, gchar **out_key, gchar **out_value)
{
  const gchar *p = line;
  const gchar *start, *end;
  gchar *token, *key;
  gchar *value = NULL;

  while (*p == ' ' || *p == '\t')
    p++;
  if (*p != '"')
    return FALSE;

  start = p++;
  while (*p != '\0' && *p != '"') {
    if (*p == '\\' && *(p + 1) != '\0')
      p++;
    p++;
  }
  if (*p != '"')
    return FALSE;
  end = p + 1;

  token = g_strndup (start, end - start);
  key = managesieve_unquote_string (token);
  g_free (token);

  p = end;
  while (*p == ' ' || *p == '\t')
    p++;
  if (*p == '"') {
    start = p++;
    while (*p != '\0' && *p != '"') {
      if (*p == '\\' && *(p + 1) != '\0')
        p++;
      p++;
    }
    if (*p == '"') {
      end = p + 1;
      token = g_strndup (start, end - start);
      value = managesieve_unquote_string (token);
      g_free (token);
    }
  }

  *out_key = g_ascii_strup (key, -1);
  g_free (key);
  *out_value = value;
  return TRUE;
}

/* Fills self->capabilities from the data lines accumulated by
 * read_response() for a capability banner (initial connection, or
 * re-announcement after StartTLS). Existing entries with the same name
 * are replaced. */
static void
update_capabilities_from_greeting (SieveManageSieveClient *self, const gchar *raw_data)
{
  gchar **lines = g_strsplit (raw_data, "\n", -1);
  guint i;

  for (i = 0; lines[i] != NULL; i++) {
    gchar *key = NULL, *value = NULL;

    if (*lines[i] == '\0')
      continue;
    if (!parse_capability_line (lines[i], &key, &value))
      continue;

    g_hash_table_replace (self->capabilities, key, value);
  }
  g_strfreev (lines);
}

/* Spots a literal at the end of a line: "{123}" or "{123+}", with an
 * optional prefix before it (e.g. "NO {131}" — Dovecot sends the
 * human-readable CHECKSCRIPT/NO error message this way, as a literal
 * rather than a quoted-string, confirmed by testing against a real
 * server — see project history). *out_prefix receives the part before
 * the literal (may be an empty string for a literal alone on its line,
 * as with GETSCRIPT data).
 *
 * Returns FALSE with *error left untouched if the line simply isn't a
 * literal (caller falls back to treating it as plain text) — but FALSE
 * with *error SET if it looks like a literal announcing a size that
 * overflows or exceeds SIEVE_MANAGESIEVE_MAX_LITERAL_SIZE: the caller
 * must then abort the response rather than keep parsing, since accepting
 * the value as-is would mean an unbounded g_malloc() driven entirely by
 * network input (CWE-789), reachable before StartTLS via the plaintext
 * banner. */
static gboolean
parse_line_with_optional_literal (const gchar *line, gchar **out_prefix, gsize *out_size,
                                   GError **error)
{
  const gchar *brace = strrchr (line, '{');
  const gchar *p;
  gchar *endptr;
  guint64 value;
  gsize prefix_len;
  gchar *prefix;

  if (brace == NULL)
    return FALSE;

  p = brace + 1;
  errno = 0;
  value = g_ascii_strtoull (p, &endptr, 10);
  if (endptr == p)
    return FALSE;

  if (*endptr == '+') /* synchronizing literal: no ack expected */
    endptr++;

  if (*endptr != '}' || *(endptr + 1) != '\0')
    return FALSE;

  if (errno == ERANGE || value > SIEVE_MANAGESIEVE_MAX_LITERAL_SIZE) {
    g_set_error (error, SIEVE_MANAGESIEVE_ERROR, SIEVE_MANAGESIEVE_ERROR_PROTOCOL,
                 "Server announced an oversized literal (%" G_GUINT64_FORMAT
                 " bytes, max %d)", value, SIEVE_MANAGESIEVE_MAX_LITERAL_SIZE);
    return FALSE;
  }

  prefix_len = (gsize) (brace - line);
  prefix = g_strndup (line, prefix_len);
  g_strchomp (prefix);

  *out_prefix = prefix;
  *out_size = (gsize) value;
  return TRUE;
}

static gchar *
read_literal_bytes (SieveManageSieveClient *self, gsize size,
                     GCancellable *cancellable, GError **error)
{
  gchar *buf = g_malloc (size + 1);
  gsize bytes_read = 0;

  if (!g_input_stream_read_all (G_INPUT_STREAM (self->input), buf, size,
                                 &bytes_read, cancellable, error)) {
    normalize_transport_error (error);
    g_free (buf);
    return NULL;
  }
  if (bytes_read < size) {
    /* read_all returned TRUE but short: the stream ended in the middle of
     * the literal → connection lost mid-operation. */
    g_set_error (error, SIEVE_MANAGESIEVE_ERROR, SIEVE_MANAGESIEVE_ERROR_PROTOCOL,
                 "Connection interrupted while reading server data");
    g_free (buf);
    return NULL;
  }
  buf[bytes_read] = '\0';
  return buf;
}

/* Reads a complete response: accumulates data lines into out_data (may be
 * NULL if the caller doesn't need them), stops on the final OK/NO/BYE
 * line — whether its accompanying message is a classic quoted-string or a
 * literal. See limitations in the .h. */
static gboolean
read_response (SieveManageSieveClient *self, GString *out_data,
                GCancellable *cancellable, GError **error)
{
  while (TRUE) {
    gchar *line = read_line (self, cancellable, error);
    gchar *prefix = NULL;
    gsize literal_size;

    if (line == NULL)
      return FALSE;

    if (parse_line_with_optional_literal (line, &prefix, &literal_size, error)) {
      gchar *literal = read_literal_bytes (self, literal_size, cancellable, error);
      gboolean prefix_is_ok  = g_ascii_strncasecmp (prefix, "OK", 2) == 0;
      gboolean prefix_is_no  = g_ascii_strncasecmp (prefix, "NO", 2) == 0;
      gboolean prefix_is_bye = g_ascii_strncasecmp (prefix, "BYE", 3) == 0;

      g_free (line);
      if (literal == NULL) { g_free (prefix); return FALSE; }

      if (*prefix == '\0') {
        /* Literal alone on its line: it's data (e.g. GETSCRIPT content).
         * Keep reading, the next line carries the final OK. */
        if (out_data != NULL)
          g_string_append (out_data, literal);
        g_free (literal);
        g_free (prefix);
        continue;
      }

      if (prefix_is_ok) {
        g_free (literal); g_free (prefix);
        return TRUE;
      }
      if (prefix_is_no) {
        g_set_error (error, SIEVE_MANAGESIEVE_ERROR, SIEVE_MANAGESIEVE_ERROR_SERVER_NO,
                     "The server rejected the command: %s", literal);
        g_free (literal); g_free (prefix);
        return FALSE;
      }
      if (prefix_is_bye) {
        g_set_error (error, SIEVE_MANAGESIEVE_ERROR, SIEVE_MANAGESIEVE_ERROR_SERVER_BYE,
                     "The server closed the session: %s", literal);
        g_free (literal); g_free (prefix);
        return FALSE;
      }

      /* Unexpected prefix before a literal: treat it as extra raw data
       * rather than failing outright. */
      if (out_data != NULL) {
        g_string_append (out_data, prefix);
        g_string_append_c (out_data, ' ');
        g_string_append (out_data, literal);
        g_string_append_c (out_data, '\n');
      }
      g_free (literal);
      g_free (prefix);
      continue;
    }

    /* parse_line_with_optional_literal() sets *error (without returning
     * TRUE) only for a literal whose announced size is invalid — abort
     * rather than fall through and treat the line as plain text. */
    if (error != NULL && *error != NULL) {
      g_free (line);
      return FALSE;
    }

    if (g_ascii_strncasecmp (line, "OK", 2) == 0) {
      g_free (line);
      return TRUE;
    }
    if (g_ascii_strncasecmp (line, "NO", 2) == 0) {
      g_set_error (error, SIEVE_MANAGESIEVE_ERROR, SIEVE_MANAGESIEVE_ERROR_SERVER_NO,
                   "The server rejected the command: %s", line);
      g_free (line);
      return FALSE;
    }
    if (g_ascii_strncasecmp (line, "BYE", 3) == 0) {
      g_set_error (error, SIEVE_MANAGESIEVE_ERROR, SIEVE_MANAGESIEVE_ERROR_SERVER_BYE,
                   "The server closed the session: %s", line);
      g_free (line);
      return FALSE;
    }

    /* "Raw" data line (e.g. LISTSCRIPTS returns "name" [ACTIVE] lines
     * before the final OK). */
    if (out_data != NULL) {
      g_string_append (out_data, line);
      g_string_append_c (out_data, '\n');
    }
    g_free (line);
  }
}

/* ---- Connection ---------------------------------------------------------- */

/* Rebinds self->input/self->output to point at self->active_stream
 * (called after opening the TCP connection, then again after the
 * StartTLS negotiation once self->active_stream has switched to the TLS
 * layer). */
static void
rebind_streams_to_active_stream (SieveManageSieveClient *self)
{
  g_clear_object (&self->input);
  self->input = g_data_input_stream_new (g_io_stream_get_input_stream (self->active_stream));
  /* self->active_stream (TCP, or the TLS layer after StartTLS) is closed
   * explicitly by sieve_managesieve_client_disconnect(); without this,
   * destroying this simple read wrapper would cascade-close the base
   * stream (GFilterInputStream's default behavior), which breaks the
   * underlying connection at the exact moment StartTLS replaces this
   * wrapper — bug observed while testing against a real Dovecot server
   * (TLS handshake succeeded server-side, connection immediately closed
   * client-side with "Stream is already closed"). */
  g_filter_input_stream_set_close_base_stream (G_FILTER_INPUT_STREAM (self->input), FALSE);
  g_data_input_stream_set_newline_type (self->input, G_DATA_STREAM_NEWLINE_TYPE_CR_LF);
  self->output = g_io_stream_get_output_stream (self->active_stream);
}

gboolean
sieve_managesieve_client_connect_sync (SieveManageSieveClient *self,
                                        GCancellable *cancellable, GError **error)
{
  GSocketClient *client;
  GString *greeting;
  gboolean ok;

  g_return_val_if_fail (SIEVE_IS_MANAGESIEVE_CLIENT (self), FALSE);

  client = g_socket_client_new ();
  if (self->implicit_tls)
    g_socket_client_set_tls (client, TRUE);
  /* Bounds establishing the connection (DNS resolution + connect() +
   * implicit TLS handshake). See also apply_timeout_to_socket() below for
   * subsequent reads/writes. */
  g_socket_client_set_timeout (client, self->timeout_seconds);

  self->connection = g_socket_client_connect_to_host (client, self->host,
                                                       self->port, cancellable, error);
  g_object_unref (client);
  if (self->connection == NULL) {
    normalize_transport_error (error);
    return FALSE;
  }

  self->active_stream = G_IO_STREAM (self->connection);
  apply_timeout_to_socket (self);
  rebind_streams_to_active_stream (self);

  /* Banner: capabilities up to the "OK". */
  greeting = g_string_new (NULL);
  ok = read_response (self, greeting, cancellable, error);
  if (ok)
    update_capabilities_from_greeting (self, greeting->str);
  g_string_free (greeting, TRUE);

  if (ok && !self->implicit_tls) {
    GIOStream *tls_stream;
    GSocketConnectable *identity;

    if (!g_hash_table_contains (self->capabilities, "STARTTLS")) {
      g_set_error (error, SIEVE_MANAGESIEVE_ERROR, SIEVE_MANAGESIEVE_ERROR_PROTOCOL,
                   "The server doesn't advertise STARTTLS in its capabilities "
                   "(connection aborted rather than continued in plaintext)");
      return FALSE;
    }

    ok = write_line (self, "STARTTLS", cancellable, error);
    if (ok)
      ok = read_response (self, NULL, cancellable, error);
    if (!ok)
      return FALSE;

    /* Any data already buffered in plaintext by self->input at this point
     * is discarded (rebind_streams_to_active_stream below recreates a
     * fresh GDataInputStream): this is deliberate, it's the standard
     * defense against plaintext command injection before StartTLS (cf.
     * "STARTTLS command injection" attacks on IMAP/SMTP/POP3) — an active
     * attacker who slipped bytes right after the "OK" must never see them
     * treated as post-TLS data. */
    identity = g_network_address_new (self->host, self->port);
    tls_stream = g_tls_client_connection_new (self->active_stream, identity, error);
    g_object_unref (identity);

    if (tls_stream == NULL)
      return FALSE;

    /* Certificate validation (hostname + trust chain) is handled by
     * GTlsClientConnection's default policy: with no "accept-certificate"
     * handler connected, any validation error fails the handshake below. */
    ok = g_tls_connection_handshake (G_TLS_CONNECTION (tls_stream), cancellable, error);
    if (!ok) {
      g_object_unref (tls_stream);
      return FALSE;
    }

    self->active_stream = tls_stream; /* self->connection stays the underlying TCP connection */
    rebind_streams_to_active_stream (self);

    /* RFC 5804 §2.2: the server MUST re-emit its capabilities right after
     * StartTLS (they may differ from the plaintext banner — STARTTLS
     * disappears, new SASL mechanisms appear…). We discard the old ones
     * rather than merging them: keeping them would mean trusting
     * capabilities seen in plaintext, hence forgeable by an active
     * attacker before the TLS negotiation. */
    g_hash_table_remove_all (self->capabilities);
    greeting = g_string_new (NULL);
    ok = read_response (self, greeting, cancellable, error);
    if (ok)
      update_capabilities_from_greeting (self, greeting->str);
    g_string_free (greeting, TRUE);
  }

  return ok;
}

void
sieve_managesieve_client_disconnect (SieveManageSieveClient *self)
{
  g_return_if_fail (SIEVE_IS_MANAGESIEVE_CLIENT (self));

  g_clear_object (&self->input);
  self->output = NULL;

  if (self->active_stream != NULL) {
    g_io_stream_close (self->active_stream, NULL, NULL);
    /* After a successful StartTLS, active_stream is a distinct object
     * (the TLS layer) with its own reference, in addition to
     * self->connection (the underlying TCP connection): the implicit-TLS
     * or no-TLS case has only one object, a direct alias of
     * self->connection, which must not be unreffed twice. */
    if (self->connection == NULL || self->active_stream != G_IO_STREAM (self->connection))
      g_object_unref (self->active_stream);
    self->active_stream = NULL;
  }

  if (self->connection != NULL) {
    g_io_stream_close (G_IO_STREAM (self->connection), NULL, NULL);
    g_clear_object (&self->connection);
  }
}

/* ---- Authentication ------------------------------------------------------ */

const gchar *
sieve_managesieve_client_get_sasl_capability (SieveManageSieveClient *self)
{
  g_return_val_if_fail (SIEVE_IS_MANAGESIEVE_CLIENT (self), NULL);
  return g_hash_table_lookup (self->capabilities, "SASL");
}

const gchar *
sieve_managesieve_client_get_auth_mechanism (SieveManageSieveClient *self)
{
  g_return_val_if_fail (SIEVE_IS_MANAGESIEVE_CLIENT (self), NULL);
  return self->auth_mechanism;
}

typedef enum {
  AUTH_REPLY_OK,         /* final "OK": authentication accepted */
  AUTH_REPLY_NO,         /* final "NO": rejected (error set) */
  AUTH_REPLY_BYE,        /* "BYE": session closed (error set) */
  AUTH_REPLY_CHALLENGE   /* intermediate SASL challenge (bytes in out_challenge) */
} AuthReplyType;

/* Extracts, from an "OK …" line, the optional final SASL data sent as
 * (SASL "base64") — RFC 5804 §1.7. Returns NULL if absent. */
static guchar *
extract_ok_sasl_data (const gchar *line, gsize *out_len)
{
  const gchar *p = strstr (line, "(SASL ");
  const gchar *q, *r;
  gchar *b64;
  guchar *decoded;

  *out_len = 0;
  if (p == NULL)
    return NULL;
  q = strchr (p + 6, '"');
  if (q == NULL)
    return NULL;
  q++;
  r = strchr (q, '"');           /* base64 never contains a quote character */
  if (r == NULL)
    return NULL;

  b64 = g_strndup (q, r - q);
  decoded = g_base64_decode (b64, out_len);
  g_free (b64);
  return decoded;
}

/* Reads a server message during the AUTHENTICATE phase and classifies it.
 * CHALLENGE: *out_challenge / *out_challenge_len receive the challenge
 * bytes, already base64-decoded (to free with g_free). OK: *out_final /
 * *out_final_len receive any final SASL data. */
static gboolean
read_auth_reply (SieveManageSieveClient *self,
                 AuthReplyType *out_type,
                 guchar **out_challenge, gsize *out_challenge_len,
                 guchar **out_final, gsize *out_final_len,
                 GCancellable *cancellable, GError **error)
{
  gchar *line;
  gchar *prefix = NULL;
  gsize literal_size = 0;

  *out_challenge = NULL; *out_challenge_len = 0;
  *out_final = NULL; *out_final_len = 0;

  line = read_line (self, cancellable, error);
  if (line == NULL)
    return FALSE;

  if (parse_line_with_optional_literal (line, &prefix, &literal_size, error)) {
    gchar *literal = read_literal_bytes (self, literal_size, cancellable, error);
    gchar *tail;

    g_free (line);
    if (literal == NULL) { g_free (prefix); return FALSE; }

    /* Consumes the CRLF that ends the literal's line (empty line). */
    tail = read_line (self, cancellable, error);
    g_free (tail);

    if (*prefix == '\0') {
      /* Literal alone: it's the SASL challenge (base64). */
      *out_challenge = g_base64_decode (literal, out_challenge_len);
      *out_type = AUTH_REPLY_CHALLENGE;
      g_free (literal); g_free (prefix);
      return TRUE;
    }
    if (g_ascii_strncasecmp (prefix, "OK", 2) == 0) {
      *out_type = AUTH_REPLY_OK;
      g_free (literal); g_free (prefix);
      return TRUE;
    }
    if (g_ascii_strncasecmp (prefix, "NO", 2) == 0) {
      g_set_error (error, SIEVE_MANAGESIEVE_ERROR, SIEVE_MANAGESIEVE_ERROR_SERVER_NO,
                   "The server rejected authentication: %s", literal);
      *out_type = AUTH_REPLY_NO;
      g_free (literal); g_free (prefix);
      return TRUE;
    }
    if (g_ascii_strncasecmp (prefix, "BYE", 3) == 0) {
      g_set_error (error, SIEVE_MANAGESIEVE_ERROR, SIEVE_MANAGESIEVE_ERROR_SERVER_BYE,
                   "The server closed the session: %s", literal);
      *out_type = AUTH_REPLY_BYE;
      g_free (literal); g_free (prefix);
      return TRUE;
    }

    g_set_error (error, SIEVE_MANAGESIEVE_ERROR, SIEVE_MANAGESIEVE_ERROR_PROTOCOL,
                 "Unexpected response during AUTHENTICATE: %s {…}", prefix);
    g_free (literal); g_free (prefix);
    return FALSE;
  }

  /* parse_line_with_optional_literal() sets *error (without returning
   * TRUE) only for a literal whose announced size is invalid — abort
   * rather than fall through and treat the line as a challenge. */
  if (error != NULL && *error != NULL) {
    g_free (line);
    return FALSE;
  }

  if (g_ascii_strncasecmp (line, "OK", 2) == 0 &&
      (line[2] == '\0' || line[2] == ' ')) {
    *out_final = extract_ok_sasl_data (line, out_final_len);
    *out_type = AUTH_REPLY_OK;
    g_free (line);
    return TRUE;
  }
  if (g_ascii_strncasecmp (line, "NO", 2) == 0 &&
      (line[2] == '\0' || line[2] == ' ')) {
    g_set_error (error, SIEVE_MANAGESIEVE_ERROR, SIEVE_MANAGESIEVE_ERROR_SERVER_NO,
                 "The server rejected authentication: %s", line);
    *out_type = AUTH_REPLY_NO;
    g_free (line);
    return TRUE;
  }
  if (g_ascii_strncasecmp (line, "BYE", 3) == 0) {
    g_set_error (error, SIEVE_MANAGESIEVE_ERROR, SIEVE_MANAGESIEVE_ERROR_SERVER_BYE,
                 "The server closed the session: %s", line);
    *out_type = AUTH_REPLY_BYE;
    g_free (line);
    return TRUE;
  }

  /* Otherwise: SASL challenge as a quoted-string. */
  {
    gchar *unq = managesieve_unquote_string (line);
    *out_challenge = g_base64_decode (unq, out_challenge_len);
    *out_type = AUTH_REPLY_CHALLENGE;
    g_free (unq);
    g_free (line);
    return TRUE;
  }
}

/* Converts an error from the SASL subsystem / the server into an
 * authentication error in the client's own domain, keeping the message. */
static gboolean
fail_auth (GError **error, GError *sub, const gchar *mechanism)
{
  g_set_error (error, SIEVE_MANAGESIEVE_ERROR, SIEVE_MANAGESIEVE_ERROR_AUTH,
               "SASL authentication failed%s%s%s: %s",
               mechanism ? " (" : "", mechanism ? mechanism : "",
               mechanism ? ")" : "",
               sub != NULL ? sub->message : "unknown error");
  g_clear_error (&sub);
  return FALSE;
}

gboolean
sieve_managesieve_client_authenticate_sync (SieveManageSieveClient *self,
                                             const gchar *mechanism,
                                             const SieveManageSieveAuth *auth,
                                             GCancellable *cancellable, GError **error)
{
  const gchar *sasl_cap;
  SieveSaslCredentials creds;
  gchar *chosen;
  SieveSasl *sasl;
  GError *sub = NULL;
  guchar *resp = NULL;
  gsize resp_len = 0;
  gboolean done = FALSE;
  gboolean ok;

  g_return_val_if_fail (SIEVE_IS_MANAGESIEVE_CLIENT (self), FALSE);
  g_return_val_if_fail (auth != NULL && auth->authid != NULL, FALSE);

  sasl_cap = g_hash_table_lookup (self->capabilities, "SASL");

  creds.authid       = auth->authid;
  creds.authzid      = auth->authzid;
  creds.password     = auth->password;
  creds.oauth2_token = auth->oauth2_token;
  creds.hostname     = self->host;
  creds.port         = self->port;

  chosen = sieve_sasl_select_mechanism (sasl_cap, mechanism, &creds, &sub);
  if (chosen == NULL)
    return fail_auth (error, sub, mechanism);

  sasl = sieve_sasl_new (chosen, &creds, &sub);
  if (sasl == NULL) {
    ok = fail_auth (error, sub, chosen);
    g_free (chosen);
    return ok;
  }

  /* First step: initial response (client-first mechanisms) or nothing. */
  if (!sieve_sasl_step (sasl, NULL, 0, &resp, &resp_len, &done, &sub)) {
    ok = fail_auth (error, sub, chosen);
    goto out;
  }

  {
    gchar *mech_q = managesieve_quote_string (chosen);
    gchar *cmd;

    if (resp_len > 0) {
      gchar *b64 = g_base64_encode (resp, resp_len);
      cmd = g_strdup_printf ("AUTHENTICATE %s \"%s\"", mech_q, b64);
      g_free (b64);
    } else {
      cmd = g_strdup_printf ("AUTHENTICATE %s", mech_q);
    }
    g_free (mech_q);
    g_clear_pointer (&resp, g_free);

    ok = write_line (self, cmd, cancellable, error);
    g_free (cmd);
    if (!ok)
      goto out;
  }

  while (TRUE) {
    AuthReplyType type;
    guchar *chal = NULL, *final = NULL;
    gsize chal_len = 0, final_len = 0;

    if (!read_auth_reply (self, &type, &chal, &chal_len, &final, &final_len,
                          cancellable, error)) {
      ok = FALSE;
      goto out;
    }

    if (type == AUTH_REPLY_OK) {
      /* Final SASL data piggybacked on the OK (e.g. server-final SCRAM):
       * fed to the engine to verify the server's proof. Absent? Still
       * accepted — some servers don't send it, mutual verification is
       * then simply skipped. */
      if (final != NULL && !done) {
        guchar *tmp = NULL;
        gsize tmp_len = 0;
        if (!sieve_sasl_step (sasl, final, final_len, &tmp, &tmp_len, &done, &sub)) {
          g_free (final);
          g_free (tmp);
          ok = fail_auth (error, sub, chosen);
          goto out;
        }
        g_free (tmp);
      }
      g_free (chal);
      g_free (final);

      g_free (self->auth_mechanism);
      self->auth_mechanism = g_strdup (chosen);
      ok = TRUE;
      goto out;
    }

    if (type == AUTH_REPLY_NO || type == AUTH_REPLY_BYE) {
      g_free (chal);
      g_free (final);
      /* read_auth_reply already set the error (SERVER_NO / SERVER_BYE);
       * relabel a NO as an authentication failure. */
      if (type == AUTH_REPLY_NO && error != NULL && *error != NULL)
        (*error)->code = SIEVE_MANAGESIEVE_ERROR_AUTH;
      ok = FALSE;
      goto out;
    }

    /* CHALLENGE: fed to the SASL engine, then we send back the response
     * (base64 as a quoted-string; "" for an empty response). */
    {
      guchar *out_bytes = NULL;
      gsize out_len = 0;
      gchar *b64, *resp_line;

      if (!sieve_sasl_step (sasl, chal, chal_len, &out_bytes, &out_len, &done, &sub)) {
        g_free (chal);
        ok = fail_auth (error, sub, chosen);
        goto out;
      }
      g_free (chal);

      b64 = out_len > 0 ? g_base64_encode (out_bytes, out_len) : g_strdup ("");
      g_free (out_bytes);
      resp_line = g_strdup_printf ("\"%s\"", b64);
      g_free (b64);

      ok = write_line (self, resp_line, cancellable, error);
      g_free (resp_line);
      if (!ok)
        goto out;
    }
  }

out:
  g_clear_pointer (&resp, g_free);
  sieve_sasl_free (sasl);
  g_free (chosen);
  return ok;
}

/* ---- Script management commands ------------------------------------------ */

GPtrArray *
sieve_managesieve_client_list_scripts_sync (SieveManageSieveClient *self,
                                             gchar **out_active,
                                             GCancellable *cancellable, GError **error)
{
  GString *data;
  GPtrArray *names;
  gchar **lines;
  guint i;

  g_return_val_if_fail (SIEVE_IS_MANAGESIEVE_CLIENT (self), NULL);

  if (out_active != NULL)
    *out_active = NULL;

  if (!write_line (self, "LISTSCRIPTS", cancellable, error))
    return NULL;

  data = g_string_new (NULL);
  if (!read_response (self, data, cancellable, error)) {
    g_string_free (data, TRUE);
    return NULL;
  }

  names = g_ptr_array_new_with_free_func (g_free);
  lines = g_strsplit (data->str, "\n", -1);
  g_string_free (data, TRUE);

  /* Each line looks like: "scriptname" [ACTIVE] */
  for (i = 0; lines[i] != NULL; i++) {
    gchar *line = g_strstrip (lines[i]);
    gboolean active = FALSE;
    gchar *name;

    if (*line == '\0')
      continue;

    if (g_str_has_suffix (line, "ACTIVE")) {
      active = TRUE;
      line[strlen (line) - strlen ("ACTIVE")] = '\0';
      g_strchomp (line);
    }

    /* strips the surrounding quotes (ManageSieve format, not shell) */
    name = managesieve_unquote_string (line);

    if (active && out_active != NULL) {
      g_free (*out_active);
      *out_active = g_strdup (name);
    }
    g_ptr_array_add (names, name);
  }
  g_strfreev (lines);

  return names;
}

gchar *
sieve_managesieve_client_get_script_sync (SieveManageSieveClient *self,
                                           const gchar *name,
                                           GCancellable *cancellable, GError **error)
{
  gchar *quoted, *command;
  GString *data;
  gboolean ok;

  g_return_val_if_fail (SIEVE_IS_MANAGESIEVE_CLIENT (self), NULL);

  quoted = managesieve_quote_string (name);
  command = g_strdup_printf ("GETSCRIPT %s", quoted);
  g_free (quoted);

  ok = write_line (self, command, cancellable, error);
  g_free (command);
  if (!ok)
    return NULL;

  data = g_string_new (NULL);
  ok = read_response (self, data, cancellable, error);
  if (!ok) {
    g_string_free (data, TRUE);
    return NULL;
  }
  return g_string_free (data, FALSE);
}

gboolean
sieve_managesieve_client_put_script_sync (SieveManageSieveClient *self,
                                           const gchar *name, const gchar *content,
                                           GCancellable *cancellable, GError **error)
{
  gchar *quoted, *command;
  gboolean ok;

  g_return_val_if_fail (SIEVE_IS_MANAGESIEVE_CLIENT (self), FALSE);

  quoted = managesieve_quote_string (name);
  command = g_strdup_printf ("PUTSCRIPT %s {%" G_GSIZE_FORMAT "+}", quoted,
                              strlen (content));
  g_free (quoted);

  ok = write_line (self, command, cancellable, error);
  g_free (command);
  if (!ok)
    return FALSE;

  /* The content follows immediately, terminated by CRLF, with no extra
   * header (synchronizing literal "+" = no ack expected). */
  {
    gchar *payload = g_strdup_printf ("%s\r\n", content);
    ok = write_bytes (self, payload, strlen (payload), cancellable, error);
    g_free (payload);
    if (!ok)
      return FALSE;
  }

  return read_response (self, NULL, cancellable, error);
}

gboolean
sieve_managesieve_client_set_active_sync (SieveManageSieveClient *self,
                                           const gchar *name,
                                           GCancellable *cancellable, GError **error)
{
  gchar *quoted, *command;
  gboolean ok;

  g_return_val_if_fail (SIEVE_IS_MANAGESIEVE_CLIENT (self), FALSE);

  /* SETACTIVE "" deactivates all scripts (none active). */
  quoted = managesieve_quote_string (name != NULL ? name : "");
  command = g_strdup_printf ("SETACTIVE %s", quoted);
  g_free (quoted);

  ok = write_line (self, command, cancellable, error);
  g_free (command);
  if (!ok)
    return FALSE;

  return read_response (self, NULL, cancellable, error);
}

gboolean
sieve_managesieve_client_delete_script_sync (SieveManageSieveClient *self,
                                              const gchar *name,
                                              GCancellable *cancellable, GError **error)
{
  gchar *quoted, *command;
  gboolean ok;

  g_return_val_if_fail (SIEVE_IS_MANAGESIEVE_CLIENT (self), FALSE);

  quoted = managesieve_quote_string (name);
  command = g_strdup_printf ("DELETESCRIPT %s", quoted);
  g_free (quoted);

  ok = write_line (self, command, cancellable, error);
  g_free (command);
  if (!ok)
    return FALSE;

  return read_response (self, NULL, cancellable, error);
}

gboolean
sieve_managesieve_client_check_script_sync (SieveManageSieveClient *self,
                                             const gchar *content,
                                             GCancellable *cancellable, GError **error)
{
  gchar *command;
  gboolean ok;

  g_return_val_if_fail (SIEVE_IS_MANAGESIEVE_CLIENT (self), FALSE);

  command = g_strdup_printf ("CHECKSCRIPT {%" G_GSIZE_FORMAT "+}", strlen (content));
  ok = write_line (self, command, cancellable, error);
  g_free (command);
  if (!ok)
    return FALSE;

  {
    gchar *payload = g_strdup_printf ("%s\r\n", content);
    ok = write_bytes (self, payload, strlen (payload), cancellable, error);
    g_free (payload);
    if (!ok)
      return FALSE;
  }

  return read_response (self, NULL, cancellable, error);
}
