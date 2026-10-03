/* sieve-managesieve-client.c
 *
 * Minimal but functional implementation of the ManageSieve protocol.
 * See the limitations listed in the .h before any production use.
 */

#include "sieve-managesieve-client.h"
#include "sieve-managesieve-client-private.h"
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

/* Longest physical response line accepted (CRLF excluded). Real lines are
 * short (a capability, a script name, a status with its message — long
 * texts such as CHECKSCRIPT errors come as literals, which have their own
 * cap above): without a bound, a server that never sends CRLF would make
 * the client buffer forever. */
#define SIEVE_MANAGESIEVE_MAX_LINE_SIZE (64 * 1024)

/* Total bytes (lines + literals) accepted for a single response, up to and
 * including its final OK/NO/BYE. Bounds what the two caps above can't on
 * their own: an endless stream of individually valid data lines or
 * literals. */
#define SIEVE_MANAGESIEVE_MAX_RESPONSE_SIZE (32 * 1024 * 1024)

/* AUTHENTICATE challenge/response round trips accepted before giving up
 * (every real mechanism needs at most a handful): a server that keeps
 * sending challenges must not keep the client looping. */
#define SIEVE_MANAGESIEVE_MAX_SASL_ROUNDS 16

struct _SieveManageSieveClient {
  GObject parent_instance;

  gchar *host;
  guint16 port;
  gboolean implicit_tls;
  guint timeout_seconds;           /* 0 = no limit; see set_timeout() */

  GSocketConnection *connection;   /* raw TCP connection (always owned) */
  GIOStream *active_stream;        /* current stream: alias of connection, or the TLS layer
                                    * from an explicit StartTLS (then owned separately) */
  GBufferedInputStream *input;
  GOutputStream *output;
  gsize response_budget;           /* bytes still accepted for the response
                                    * being read (see MAX_RESPONSE_SIZE) */

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

static void
set_protocol_error (GError **error, const gchar *message)
{
  g_set_error_literal (error, SIEVE_MANAGESIEVE_ERROR,
                       SIEVE_MANAGESIEVE_ERROR_PROTOCOL, message);
}

/* Charges `n` received bytes to the current response's budget (see
 * SIEVE_MANAGESIEVE_MAX_RESPONSE_SIZE). FALSE + error once exhausted. */
static gboolean
consume_budget (SieveManageSieveClient *self, gsize n, GError **error)
{
  if (n > self->response_budget) {
    set_protocol_error (error, "Server response too large");
    return FALSE;
  }
  self->response_budget -= n;
  return TRUE;
}

/* Reads a physical line (without its line terminator). NULL + error on
 * failure. Bounded (SIEVE_MANAGESIEVE_MAX_LINE_SIZE), unlike
 * g_data_input_stream_read_line(), which buffers until it finds a
 * terminator. Accepts CRLF or a lone LF (RFC 5804 requires CRLF, but
 * neither a quoted-string nor an atom can contain a LF, so this isn't
 * ambiguous). Rejects NUL bytes (they would silently truncate the line as
 * a C string) and invalid UTF-8. */
static gchar *
read_line (SieveManageSieveClient *self, GCancellable *cancellable, GError **error)
{
  GString *line = g_string_new (NULL);

  while (TRUE) {
    gsize avail;
    const gchar *buf = g_buffered_input_stream_peek_buffer (self->input, &avail);
    const gchar *nl;
    gsize take;

    if (avail == 0) {
      gssize n = g_buffered_input_stream_fill (self->input, -1, cancellable, error);

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
    if (line->len + take > SIEVE_MANAGESIEVE_MAX_LINE_SIZE + 2) {
      set_protocol_error (error, "Server response line too long");
      goto fail;
    }
    if (!consume_budget (self, take, error))
      goto fail;

    g_string_append_len (line, buf, take);
    /* Data already buffered: skipping it never blocks. */
    g_input_stream_skip (G_INPUT_STREAM (self->input), take, NULL, NULL);

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

static const gchar *
skip_ws (const gchar *p)
{
  while (*p == ' ' || *p == '\t')
    p++;
  return p;
}

/* Parses a quoted-string starting at *pp (after optional whitespace).
 * On success, advances *pp past the closing quote and, if `out` isn't
 * NULL, stores the unescaped contents there (to free). Returns FALSE,
 * leaving *pp untouched, if no complete quoted-string starts there. */
static gboolean
scan_quoted_string (const gchar **pp, gchar **out)
{
  const gchar *p = skip_ws (*pp);
  GString *val;

  if (*p != '"')
    return FALSE;

  val = g_string_new (NULL);
  for (p++; *p != '\0' && *p != '"'; p++) {
    if (*p == '\\') {
      if (p[1] == '\0')
        break;
      p++;
    }
    g_string_append_c (val, *p);
  }
  if (*p != '"') {
    g_string_free (val, TRUE);
    return FALSE;
  }

  *pp = p + 1;
  if (out != NULL)
    *out = g_string_free (val, FALSE);
  else
    g_string_free (val, TRUE);
  return TRUE;
}

/* TRUE if `line` starts with the keyword `kw` (case-insensitive) as a
 * whole token: "NO" matches "NO" and "NO (…) …", not "NONSENSE". */
static gboolean
keyword_is (const gchar *line, const gchar *kw)
{
  gsize n = strlen (kw);
  return g_ascii_strncasecmp (line, kw, n) == 0
      && (line[n] == '\0' || line[n] == ' ');
}

typedef enum {
  RESPONSE_LINE_DATA,
  RESPONSE_LINE_OK,
  RESPONSE_LINE_NO,
  RESPONSE_LINE_BYE
} ResponseLineKind;

static ResponseLineKind
classify_line (const gchar *line)
{
  if (keyword_is (line, "OK"))
    return RESPONSE_LINE_OK;
  if (keyword_is (line, "NO"))
    return RESPONSE_LINE_NO;
  if (keyword_is (line, "BYE"))
    return RESPONSE_LINE_BYE;
  return RESPONSE_LINE_DATA;
}

/* Skips the "(response code)" that may follow OK/NO/BYE (RFC 5804 §1.3),
 * honoring quoted-strings inside it (e.g. (SASL "…")). `p` points at '('.
 * Returns the position after the matching ')', or NULL if unbalanced. */
static const gchar *
skip_response_code (const gchar *p)
{
  guint depth = 0;

  while (*p != '\0') {
    if (*p == '"') {
      if (!scan_quoted_string (&p, NULL))
        return NULL;
      continue;
    }
    if (*p == '(')
      depth++;
    else if (*p == ')' && --depth == 0)
      return p + 1;
    p++;
  }
  return NULL;
}

/* Human-readable text of an OK/NO/BYE line: the quoted-string following
 * the keyword and its optional response code, otherwise the whole line. */
static gchar *
response_text (const gchar *line)
{
  const gchar *p = strchr (line, ' ');
  gchar *text;

  if (p == NULL)
    return g_strdup (line);
  p = skip_ws (p);
  if (*p == '(') {
    p = skip_response_code (p);
    if (p == NULL)
      return g_strdup (line);
  }
  if (scan_quoted_string (&p, &text))
    return text;
  return g_strdup (line);
}

/* Extracts at most two quoted-strings at the head of a line: "NAME" ["VALUE"]
 * (capability line format, RFC 5804 §1.7, e.g. "STARTTLS" or
 * "SASL" "PLAIN LOGIN" — literals have already been turned into
 * quoted-strings by read_logical_line()). Returns FALSE if the line
 * doesn't start with a valid quoted-string. *out_key is uppercased for a
 * case-insensitive lookup; *out_value may remain NULL. */
static gboolean
parse_capability_line (const gchar *line, gchar **out_key, gchar **out_value)
{
  const gchar *p = line;
  gchar *key;

  if (!scan_quoted_string (&p, &key))
    return FALSE;

  *out_value = NULL;
  scan_quoted_string (&p, out_value);

  *out_key = g_ascii_strup (key, -1);
  g_free (key);
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
 * banner. `error` must therefore not be NULL. */
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
  if (!g_ascii_isdigit (*p))
    return FALSE;
  errno = 0;
  value = g_ascii_strtoull (p, &endptr, 10);

  if (*endptr == '+') /* non-synchronizing literal: no ack expected */
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
  gchar *buf;
  gsize bytes_read = 0;

  /* Checked before allocating: the size is network input. */
  if (!consume_budget (self, size, error))
    return NULL;

  buf = g_malloc (size + 1);
  if (!g_input_stream_read_all (G_INPUT_STREAM (self->input), buf, size,
                                 &bytes_read, cancellable, error)) {
    normalize_transport_error (error);
    g_free (buf);
    return NULL;
  }
  if (bytes_read < size) {
    /* read_all returned TRUE but short: the stream ended in the middle of
     * the literal → connection lost mid-operation. */
    set_protocol_error (error, "Connection interrupted while reading server data");
    g_free (buf);
    return NULL;
  }
  /* Returned as a C string: a NUL byte would silently truncate it. */
  if (memchr (buf, '\0', size) != NULL) {
    set_protocol_error (error, "NUL byte in server data");
    g_free (buf);
    return NULL;
  }
  /* Same rule as read_line(): a script is UTF-8 (RFC 5228 §2.1), and a
   * GTK text buffer silently refuses anything else -- which used to leave
   * the editor empty, one "Save" away from overwriting the server's
   * script with nothing (e.g. an old hand-edited Latin-1 script). */
  if (!g_utf8_validate (buf, size, NULL)) {
    set_protocol_error (error, "Invalid UTF-8 in server data");
    g_free (buf);
    return NULL;
  }
  buf[bytes_read] = '\0';
  return buf;
}

/* Reads one logical response line: a physical line where each "{N}" /
 * "{N+}" literal announced at its end is replaced by the literal's
 * contents, re-encoded as a quoted-string, and the line then continues
 * with the physical line following the literal (RFC 5804 §4: a literal
 * is just one form of "string", the rest of its line follows it). The
 * rest of the parser therefore only ever sees quoted-strings, whichever
 * form the server chose — and the CRLF that ends the line after a literal
 * is consumed here, instead of leaking into the response as an extra
 * empty data line (which used to append a stray "\n" to every GETSCRIPT
 * result, and to leave that CRLF pending for the next command after a
 * "NO {N}"). */
static gchar *
read_logical_line (SieveManageSieveClient *self, GCancellable *cancellable, GError **error)
{
  GString *out = g_string_new (NULL);

  while (TRUE) {
    gchar *line = read_line (self, cancellable, error);
    gchar *prefix = NULL, *literal, *quoted;
    gsize size = 0;
    GError *local = NULL;

    if (line == NULL)
      break;

    if (!parse_line_with_optional_literal (line, &prefix, &size, &local)) {
      if (local != NULL) {
        g_propagate_error (error, local);
        g_free (line);
        break;
      }
      g_string_append (out, line);
      g_free (line);
      return g_string_free (out, FALSE);
    }
    g_free (line);

    literal = read_literal_bytes (self, size, cancellable, error);
    if (literal == NULL) {
      g_free (prefix);
      break;
    }

    g_string_append (out, prefix);
    if (*prefix != '\0')
      g_string_append_c (out, ' ');
    quoted = managesieve_quote_string (literal);
    g_string_append (out, quoted);
    g_free (quoted);
    g_free (literal);
    g_free (prefix);
  }

  g_string_free (out, TRUE);
  return NULL;
}

/* Reads a complete response: accumulates data lines into out_data (may be
 * NULL if the caller doesn't need them), one per "\n"-terminated line,
 * literals already turned into quoted-strings; stops on the final
 * OK/NO/BYE line. */
static gboolean
read_response (SieveManageSieveClient *self, GString *out_data,
                GCancellable *cancellable, GError **error)
{
  self->response_budget = SIEVE_MANAGESIEVE_MAX_RESPONSE_SIZE;

  while (TRUE) {
    gchar *line = read_logical_line (self, cancellable, error);
    gchar *text;

    if (line == NULL)
      return FALSE;

    switch (classify_line (line)) {
    case RESPONSE_LINE_OK:
      g_free (line);
      return TRUE;

    case RESPONSE_LINE_NO:
      text = response_text (line);
      g_set_error (error, SIEVE_MANAGESIEVE_ERROR, SIEVE_MANAGESIEVE_ERROR_SERVER_NO,
                   "The server rejected the command: %s", text);
      g_free (text);
      g_free (line);
      return FALSE;

    case RESPONSE_LINE_BYE:
      text = response_text (line);
      g_set_error (error, SIEVE_MANAGESIEVE_ERROR, SIEVE_MANAGESIEVE_ERROR_SERVER_BYE,
                   "The server closed the session: %s", text);
      g_free (text);
      g_free (line);
      return FALSE;

    case RESPONSE_LINE_DATA:
    default:
      /* Data line (capability, LISTSCRIPTS entry, GETSCRIPT string…). */
      if (out_data != NULL) {
        g_string_append (out_data, line);
        g_string_append_c (out_data, '\n');
      }
      g_free (line);
      break;
    }
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
  self->input = G_BUFFERED_INPUT_STREAM (
      g_buffered_input_stream_new (g_io_stream_get_input_stream (self->active_stream)));
  /* self->active_stream (TCP, or the TLS layer after StartTLS) is closed
   * explicitly by sieve_managesieve_client_disconnect(); without this,
   * destroying this simple read wrapper would cascade-close the base
   * stream (GFilterInputStream's default behavior), which breaks the
   * underlying connection at the exact moment StartTLS replaces this
   * wrapper — bug observed while testing against a real Dovecot server
   * (TLS handshake succeeded server-side, connection immediately closed
   * client-side with "Stream is already closed"). */
  g_filter_input_stream_set_close_base_stream (G_FILTER_INPUT_STREAM (self->input), FALSE);
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
     * fresh GBufferedInputStream): this is deliberate, it's the standard
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

gboolean
sieve_managesieve_client_attach_stream_for_testing (SieveManageSieveClient *self,
                                                    GIOStream *stream,
                                                    GCancellable *cancellable,
                                                    GError **error)
{
  GString *greeting;
  gboolean ok;

  g_return_val_if_fail (SIEVE_IS_MANAGESIEVE_CLIENT (self), FALSE);
  g_return_val_if_fail (G_IS_IO_STREAM (stream), FALSE);

  sieve_managesieve_client_disconnect (self);
  /* No self->connection: disconnect() then unrefs active_stream itself. */
  self->active_stream = g_object_ref (stream);
  rebind_streams_to_active_stream (self);

  greeting = g_string_new (NULL);
  ok = read_response (self, greeting, cancellable, error);
  if (ok)
    update_capabilities_from_greeting (self, greeting->str);
  g_string_free (greeting, TRUE);
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

/* Decodes SASL data sent as base64 (RFC 5804 §2.1). Unlike a bare
 * g_base64_decode(), which silently skips invalid characters, anything
 * that isn't well-formed base64 is rejected: it can only be a protocol
 * error, and must not reach the SASL engine as garbage. */
static guchar *
decode_base64_strict (const gchar *b64, gsize *out_len, GError **error)
{
  gsize len = strlen (b64);
  gsize i, pad = 0;

  for (i = 0; i < len; i++) {
    gchar c = b64[i];

    if (c == '=') {
      pad++;
      continue;
    }
    if (pad > 0 || !(g_ascii_isalnum (c) || c == '+' || c == '/'))
      break;
  }
  if (i < len || len % 4 != 0 || pad > 2) {
    set_protocol_error (error, "Malformed base64 SASL data from the server");
    *out_len = 0;
    return NULL;
  }

  if (len == 0) {
    *out_len = 0;
    return (guchar *) g_strdup ("");
  }
  return g_base64_decode (b64, out_len);
}

/* Extracts, from an "OK …" line, the optional final SASL data sent as the
 * response code (SASL "base64") — RFC 5804 §1.7. *out_final stays NULL if
 * absent. FALSE + error if present but malformed. */
static gboolean
extract_ok_sasl_data (const gchar *line, guchar **out_final, gsize *out_len,
                      GError **error)
{
  const gchar *p = skip_ws (line + 2);   /* after "OK" */
  gchar *b64;

  *out_final = NULL;
  *out_len = 0;
  if (g_ascii_strncasecmp (p, "(SASL ", 6) != 0)
    return TRUE;

  p += 6;
  if (!scan_quoted_string (&p, &b64) || *skip_ws (p) != ')') {
    set_protocol_error (error, "Malformed SASL response code from the server");
    return FALSE;
  }
  *out_final = decode_base64_strict (b64, out_len, error);
  g_free (b64);
  return *out_final != NULL;
}

/* Reads a server message during the AUTHENTICATE phase and classifies it.
 * CHALLENGE: *out_challenge / *out_challenge_len receive the challenge
 * bytes, already base64-decoded (to free with g_free). OK: *out_final /
 * *out_final_len receive any final SASL data. NO/BYE: returns TRUE with
 * the error set. FALSE: transport or protocol error. */
static gboolean
read_auth_reply (SieveManageSieveClient *self,
                 AuthReplyType *out_type,
                 guchar **out_challenge, gsize *out_challenge_len,
                 guchar **out_final, gsize *out_final_len,
                 GCancellable *cancellable, GError **error)
{
  gchar *line, *text;
  gboolean ok = TRUE;

  *out_challenge = NULL; *out_challenge_len = 0;
  *out_final = NULL; *out_final_len = 0;

  self->response_budget = SIEVE_MANAGESIEVE_MAX_RESPONSE_SIZE;
  line = read_logical_line (self, cancellable, error);
  if (line == NULL)
    return FALSE;

  switch (classify_line (line)) {
  case RESPONSE_LINE_OK:
    *out_type = AUTH_REPLY_OK;
    ok = extract_ok_sasl_data (line, out_final, out_final_len, error);
    break;

  case RESPONSE_LINE_NO:
    text = response_text (line);
    g_set_error (error, SIEVE_MANAGESIEVE_ERROR, SIEVE_MANAGESIEVE_ERROR_SERVER_NO,
                 "The server rejected authentication: %s", text);
    g_free (text);
    *out_type = AUTH_REPLY_NO;
    break;

  case RESPONSE_LINE_BYE:
    text = response_text (line);
    g_set_error (error, SIEVE_MANAGESIEVE_ERROR, SIEVE_MANAGESIEVE_ERROR_SERVER_BYE,
                 "The server closed the session: %s", text);
    g_free (text);
    *out_type = AUTH_REPLY_BYE;
    break;

  case RESPONSE_LINE_DATA:
  default: {
    /* SASL challenge: a single string (quoted, or a literal already
     * turned into one by read_logical_line()), nothing else. */
    const gchar *p = line;
    gchar *b64 = NULL;

    *out_type = AUTH_REPLY_CHALLENGE;
    if (!scan_quoted_string (&p, &b64) || *skip_ws (p) != '\0') {
      set_protocol_error (error, "Malformed SASL challenge from the server");
      ok = FALSE;
    } else {
      *out_challenge = decode_base64_strict (b64, out_challenge_len, error);
      ok = *out_challenge != NULL;
    }
    g_free (b64);
    break;
  }
  }

  g_free (line);
  return ok;
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

/* One authentication attempt with an already chosen mechanism. `chosen`
 * stays owned by the caller. *out_retryable (optional) is set to TRUE when
 * the attempt failed but the session is still usable for another
 * AUTHENTICATE: the failure happened before anything was sent, or the
 * exchange was ended cleanly (server NO / our own cancellation). */
static gboolean
authenticate_with (SieveManageSieveClient *self, const gchar *chosen,
                   const SieveSaslCredentials *creds,
                   GCancellable *cancellable, GError **error,
                   gboolean *out_retryable)
{
  SieveSasl *sasl;
  GError *sub = NULL;
  guchar *resp = NULL;
  gsize resp_len = 0;
  gboolean done = FALSE;
  gboolean ok;
  guint rounds = 0;

  if (out_retryable != NULL)
    *out_retryable = FALSE;

  sasl = sieve_sasl_new (chosen, creds, &sub);
  if (sasl == NULL) {
    if (out_retryable != NULL)
      *out_retryable = TRUE;
    return fail_auth (error, sub, chosen);
  }

  /* First step: initial response (client-first mechanisms) or nothing. */
  if (!sieve_sasl_step (sasl, NULL, 0, &resp, &resp_len, &done, &sub)) {
    /* Nothing sent yet: the session is untouched. */
    if (out_retryable != NULL)
      *out_retryable = TRUE;
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

    if (++rounds > SIEVE_MANAGESIEVE_MAX_SASL_ROUNDS) {
      set_protocol_error (error, "Too many SASL challenges from the server");
      ok = FALSE;
      goto out;
    }

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
       * relabel a NO as an authentication failure. A NO ends the
       * exchange but keeps the session; a BYE closes it. */
      if (type == AUTH_REPLY_NO) {
        if (error != NULL && *error != NULL)
          (*error)->code = SIEVE_MANAGESIEVE_ERROR_AUTH;
        if (out_retryable != NULL)
          *out_retryable = TRUE;
      }
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
        /* Our side gave up mid-exchange: cancel it (RFC 5804 §2.1, a
         * lone "*") and consume the server's NO, so that the session
         * can be reused for another attempt. */
        if (write_line (self, "\"*\"", cancellable, NULL)) {
          AuthReplyType ctype;
          guchar *c1 = NULL, *c2 = NULL;
          gsize l1 = 0, l2 = 0;

          if (read_auth_reply (self, &ctype, &c1, &l1, &c2, &l2, cancellable, NULL) &&
              ctype == AUTH_REPLY_NO && out_retryable != NULL)
            *out_retryable = TRUE;
          g_free (c1);
          g_free (c2);
        }
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
  return ok;
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
  GError *sub = NULL;
  gboolean ok, retryable = FALSE;

  g_return_val_if_fail (SIEVE_IS_MANAGESIEVE_CLIENT (self), FALSE);
  g_return_val_if_fail (auth != NULL && auth->authid != NULL, FALSE);

  sasl_cap = g_hash_table_lookup (self->capabilities, "SASL");

  creds.authid       = auth->authid;
  creds.authzid      = auth->authzid;
  creds.password     = auth->password;
  creds.oauth2_token = auth->oauth2_token;
  creds.hostname     = self->host;
  creds.port         = self->port;
  creds.gssapi_hostname = auth->gssapi_hostname;
  creds.gssapi_canonicalize_hostname = auth->gssapi_canonicalize_hostname;

  chosen = sieve_sasl_select_mechanism (sasl_cap, mechanism, &creds, &sub);
  if (chosen == NULL)
    return fail_auth (error, sub, mechanism);

  ok = authenticate_with (self, chosen, &creds, cancellable, error, &retryable);

  /* Automatic negotiation (or forced GSSAPI with auth->gssapi_fallback):
   * GSSAPI is picked as soon as a Kerberos ticket exists, but whether it
   * works also depends on things the user doesn't necessarily control
   * (ticket expiry, service principal in the server's keytab, DNS/CNAME
   * canonicalization…). Unlike a password mechanism, a GSSAPI failure
   * says nothing about the password, so trying the next mechanism
   * doesn't multiply failed password attempts. Any other forced
   * mechanism is never replaced. */
  if (!ok && (mechanism == NULL || auth->gssapi_fallback) && retryable &&
      g_strcmp0 (chosen, "GSSAPI") == 0 &&
      (error == NULL || *error != NULL) && !g_cancellable_is_cancelled (cancellable)) {
    const gchar *excluded[] = { "GSSAPI", NULL };
    gchar *fallback = sieve_sasl_select_mechanism_excluding (sasl_cap, NULL, &creds,
                                                             excluded, NULL);

    if (fallback != NULL) {
      GError *retry_err = NULL;

      g_warning ("sieve: SASL GSSAPI failed (%s); falling back to %s",
                 error != NULL ? (*error)->message : "unknown error", fallback);

      if (authenticate_with (self, fallback, &creds, cancellable, &retry_err, NULL)) {
        g_clear_error (error);
        ok = TRUE;
      } else if (error != NULL) {
        /* Report both: the GSSAPI failure is usually the real cause. */
        g_prefix_error (&retry_err, "%s; then ", (*error)->message);
        g_clear_error (error);
        g_propagate_error (error, retry_err);
      } else {
        g_clear_error (&retry_err);
      }
      g_free (fallback);
    }
  }

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

  /* Each line looks like: "scriptname" [ACTIVE] (RFC 5804 §2.7) — a
   * literal name has already been turned into a quoted-string. Anything
   * else is a protocol error rather than a name guessed from it. */
  for (i = 0; lines[i] != NULL; i++) {
    const gchar *p = lines[i];
    gboolean active = FALSE;
    gchar *name = NULL;

    if (*skip_ws (p) == '\0')
      continue;

    if (scan_quoted_string (&p, &name)) {
      p = skip_ws (p);
      if (keyword_is (p, "ACTIVE")) {
        active = TRUE;
        p = skip_ws (p + strlen ("ACTIVE"));
      }
    }
    if (name == NULL || *p != '\0') {
      g_set_error (error, SIEVE_MANAGESIEVE_ERROR, SIEVE_MANAGESIEVE_ERROR_PROTOCOL,
                   "Malformed LISTSCRIPTS entry: %s", lines[i]);
      g_free (name);
      g_strfreev (lines);
      g_ptr_array_unref (names);
      if (out_active != NULL)
        g_clear_pointer (out_active, g_free);
      return NULL;
    }

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

  /* Exactly one string (RFC 5804 §2.9), returned as-is: no quotes, and
   * none of the line terminators that frame it on the wire. */
  {
    const gchar *p = data->str;
    gchar *content = NULL;

    if (!scan_quoted_string (&p, &content) || p[strspn (p, " \t\n")] != '\0') {
      g_set_error_literal (error, SIEVE_MANAGESIEVE_ERROR, SIEVE_MANAGESIEVE_ERROR_PROTOCOL,
                           "Malformed GETSCRIPT response");
      g_free (content);
      content = NULL;
    }
    g_string_free (data, TRUE);
    return content;
  }
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
