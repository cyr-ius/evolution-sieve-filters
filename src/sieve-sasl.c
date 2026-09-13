/* sieve-sasl.c — see sieve-sasl.h for the contract and scope. */

#include "sieve-sasl.h"

#include <string.h>
#include <gsasl.h>

#ifdef HAVE_KRB5
#include <krb5.h>
#endif

GQuark
sieve_sasl_error_quark (void)
{
  return g_quark_from_static_string ("sieve-sasl-error-quark");
}

/* Order = decreasing preference. Negotiation takes the first mechanism in
 * this list that is both advertised by the server and satisfied by the
 * supplied credentials. GSSAPI comes first: true SSO, no secret handled by
 * this module at all — but it is only satisfiable during automatic
 * negotiation when a usable Kerberos ticket is actually detected (see
 * gssapi_ticket_available()), so it never gets picked blindly just because
 * the server advertises it. OAuth mechanisms come next: supplying a token
 * is an explicit choice by the caller. Among password-based mechanisms,
 * SCRAM (the secret never travels over the wire) comes before PLAIN,
 * which comes before CRAM-MD5 (MD5, unsalted, vulnerable offline) and
 * LOGIN (legacy). */
static const gchar * const known_mechs[] = {
  "GSSAPI",
  "OAUTHBEARER",
  "XOAUTH2",
  "SCRAM-SHA-256",
  "SCRAM-SHA-1",
  "PLAIN",
  "CRAM-MD5",
  "LOGIN",
  NULL
};

const gchar * const *
sieve_sasl_known_mechanisms (void)
{
  return known_mechs;
}

static gboolean
mech_is_known (const gchar *up)
{
  for (gsize i = 0; known_mechs[i] != NULL; i++)
    if (strcmp (known_mechs[i], up) == 0)
      return TRUE;
  return FALSE;
}

static gboolean
mech_is_oauth (const gchar *up)
{
  return strcmp (up, "OAUTHBEARER") == 0 || strcmp (up, "XOAUTH2") == 0;
}

static gboolean
mech_is_gssapi (const gchar *up)
{
  return strcmp (up, "GSSAPI") == 0;
}

#ifdef HAVE_KRB5
/* TRUE if the process's default Kerberos credential cache holds a
 * principal, so that automatic negotiation only picks GSSAPI when it
 * stands a real chance of working. Deliberately checked via libkrb5
 * (krb5_cc_get_principal), not via a GSS-API call: gss_acquire_cred()
 * through libgssglue (the generic GSS mechanism switch libgsasl itself
 * links) turned out to fail with "unsupported mechanism" against a modern
 * MIT krb5 on this project's reference distro — libgssglue 0.9 dispatches
 * to it via a private symbol, mechglue_internal_krb5_init, that no longer
 * exists in current libgssapi-krb5. libgsasl's own GSSAPI mechanism does
 * not go through that path and works fine (verified manually end-to-end
 * against a real KDC); only this probe would have been affected, so it
 * uses libkrb5 directly instead. This does not check the ticket's
 * expiry, only that a principal is present — an expired ticket still
 * makes automatic negotiation try GSSAPI, which then fails normally. */
static gboolean
gssapi_ticket_available (void)
{
  krb5_context ctx;
  krb5_ccache cc;
  krb5_principal princ;
  gboolean available = FALSE;

  if (krb5_init_context (&ctx) != 0)
    return FALSE;

  if (krb5_cc_default (ctx, &cc) == 0) {
    if (krb5_cc_get_principal (ctx, cc, &princ) == 0) {
      available = TRUE;
      krb5_free_principal (ctx, princ);
    }
    krb5_cc_close (ctx, cc);
  }

  krb5_free_context (ctx);
  return available;
}
#else
static gboolean
gssapi_ticket_available (void)
{
  return FALSE;
}
#endif

gboolean
sieve_sasl_mechanism_is_client_first (const gchar *mechanism)
{
  gchar *up = g_ascii_strup (mechanism, -1);
  gboolean client_first = mech_is_oauth (up) ||
                          mech_is_gssapi (up) ||
                          strcmp (up, "PLAIN") == 0 ||
                          g_str_has_prefix (up, "SCRAM-");
  g_free (up);
  return client_first;
}

/* A mechanism is satisfied if the caller supplies the identity and the
 * secret it consumes. authid is always required. GSSAPI needs no secret
 * here: the identity comes from the caller's Kerberos ticket cache. */
static gboolean
mech_is_satisfiable (const gchar *up, const SieveSaslCredentials *creds)
{
  if (creds == NULL || creds->authid == NULL || creds->authid[0] == '\0')
    return FALSE;
  if (mech_is_oauth (up))
    return creds->oauth2_token != NULL && creds->oauth2_token[0] != '\0';
  if (mech_is_gssapi (up))
    return TRUE;
  return creds->password != NULL;
}

/* Splits the "SASL" capability (mechanisms separated by spaces or tabs)
 * into a NULL-terminated array of UPPERCASE names. */
static gchar **
split_server_mechs (const gchar *server_mechs)
{
  GPtrArray *out = g_ptr_array_new ();
  if (server_mechs != NULL) {
    gchar **toks = g_strsplit_set (server_mechs, " \t", -1);
    for (gsize i = 0; toks[i] != NULL; i++) {
      if (toks[i][0] != '\0')
        g_ptr_array_add (out, g_ascii_strup (toks[i], -1));
    }
    g_strfreev (toks);
  }
  g_ptr_array_add (out, NULL);
  return (gchar **) g_ptr_array_free (out, FALSE);
}

static gboolean
strv_contains_ci (gchar **strv, const gchar *up)
{
  for (gsize i = 0; strv[i] != NULL; i++)
    if (strcmp (strv[i], up) == 0)
      return TRUE;
  return FALSE;
}

gchar *
sieve_sasl_select_mechanism (const gchar                *server_mechs,
                             const gchar                *force_mech,
                             const SieveSaslCredentials *creds,
                             GError                    **error)
{
  gchar **offered = split_server_mechs (server_mechs);
  gchar *result = NULL;

  if (force_mech != NULL) {
    gchar *up = g_ascii_strup (force_mech, -1);

    if (!mech_is_known (up)) {
      g_set_error (error, SIEVE_SASL_ERROR, SIEVE_SASL_ERROR_NO_MECHANISM,
                   "SASL mechanism unknown to this module: %s", force_mech);
    } else if (!strv_contains_ci (offered, up)) {
      g_set_error (error, SIEVE_SASL_ERROR, SIEVE_SASL_ERROR_NO_MECHANISM,
                   "The server doesn't advertise the SASL mechanism %s "
                   "(offered: %s)", up,
                   (server_mechs && *server_mechs) ? server_mechs : "none");
    } else if (!mech_is_satisfiable (up, creds)) {
      g_set_error (error, SIEVE_SASL_ERROR, SIEVE_SASL_ERROR_MISSING_CRED,
                   "Mechanism %s requires %s", up,
                   mech_is_oauth (up) ? "an OAuth2 token" : "a password");
    } else {
      result = up;
      up = NULL;
    }
    g_free (up);
    g_strfreev (offered);
    return result;
  }

  for (gsize i = 0; known_mechs[i] != NULL; i++) {
    if (mech_is_gssapi (known_mechs[i]) && !gssapi_ticket_available ())
      continue;
    if (strv_contains_ci (offered, known_mechs[i]) &&
        mech_is_satisfiable (known_mechs[i], creds)) {
      result = g_strdup (known_mechs[i]);
      break;
    }
  }

  if (result == NULL)
    g_set_error (error, SIEVE_SASL_ERROR, SIEVE_SASL_ERROR_NO_MECHANISM,
                 "No common SASL mechanism (server: %s; this module handles "
                 "OAUTHBEARER/XOAUTH2 with a token, otherwise SCRAM-SHA-256/"
                 "SCRAM-SHA-1/PLAIN/CRAM-MD5/LOGIN with a password)",
                 (server_mechs && *server_mechs) ? server_mechs : "none");

  g_strfreev (offered);
  return result;
}

/* --------------------------------------------------------------------------
 * Session
 * ------------------------------------------------------------------------ */

struct _SieveSasl {
  gchar *mechanism;              /* UPPERCASE */
  gboolean is_oauth;

  /* libgsasl path (PLAIN, LOGIN, CRAM-MD5, SCRAM-*). */
  Gsasl         *ctx;
  Gsasl_session *session;

  /* Internal OAuth path (OAUTHBEARER, XOAUTH2). */
  gchar   *oauth_authid;
  gchar   *oauth_authzid;       /* may be NULL */
  gchar   *oauth_token;
  gchar   *oauth_host;          /* may be NULL */
  guint16  oauth_port;
  gint     oauth_phase;         /* 0 = initial message to send, 1 = post-error kick, 2 = done */
};

/* gs2-header escaping (RFC 5801 §4): ',' -> "=2C", '=' -> "=3D". */
static void
gs2_append_escaped (GString *s, const gchar *v)
{
  for (const gchar *p = v; *p != '\0'; p++) {
    if (*p == ',')
      g_string_append (s, "=2C");
    else if (*p == '=')
      g_string_append (s, "=3D");
    else
      g_string_append_c (s, *p);
  }
}

SieveSasl *
sieve_sasl_new (const gchar                *mechanism,
                const SieveSaslCredentials *creds,
                GError                    **error)
{
  gchar *up;
  SieveSasl *self;

  g_return_val_if_fail (mechanism != NULL, NULL);
  g_return_val_if_fail (creds != NULL, NULL);

  up = g_ascii_strup (mechanism, -1);

  if (!mech_is_known (up)) {
    g_set_error (error, SIEVE_SASL_ERROR, SIEVE_SASL_ERROR_NO_MECHANISM,
                 "Unsupported SASL mechanism: %s", mechanism);
    g_free (up);
    return NULL;
  }
  if (!mech_is_satisfiable (up, creds)) {
    g_set_error (error, SIEVE_SASL_ERROR, SIEVE_SASL_ERROR_MISSING_CRED,
                 "Mechanism %s requires %s (and an identity)", up,
                 mech_is_oauth (up) ? "an OAuth2 token" : "a password");
    g_free (up);
    return NULL;
  }

  self = g_new0 (SieveSasl, 1);
  self->mechanism = up;

  if (mech_is_oauth (up)) {
    self->is_oauth = TRUE;
    self->oauth_authid  = g_strdup (creds->authid);
    self->oauth_authzid = g_strdup (creds->authzid);
    self->oauth_token   = g_strdup (creds->oauth2_token);
    self->oauth_host    = g_strdup (creds->hostname);
    self->oauth_port    = creds->port;
    self->oauth_phase   = 0;
    return self;
  }

  {
    int rc = gsasl_init (&self->ctx);
    if (rc != GSASL_OK) {
      g_set_error (error, SIEVE_SASL_ERROR, SIEVE_SASL_ERROR_MECHANISM,
                   "Unable to initialize libgsasl: %s",
                   gsasl_strerror (rc));
      sieve_sasl_free (self);
      return NULL;
    }
    rc = gsasl_client_start (self->ctx, self->mechanism, &self->session);
    if (rc != GSASL_OK) {
      g_set_error (error, SIEVE_SASL_ERROR, SIEVE_SASL_ERROR_MECHANISM,
                   "libgsasl doesn't provide mechanism %s: %s",
                   self->mechanism, gsasl_strerror (rc));
      sieve_sasl_free (self);
      return NULL;
    }

    if (creds->authid != NULL)
      gsasl_property_set (self->session, GSASL_AUTHID, creds->authid);
    if (creds->authzid != NULL)
      gsasl_property_set (self->session, GSASL_AUTHZID, creds->authzid);
    if (creds->password != NULL)
      gsasl_property_set (self->session, GSASL_PASSWORD, creds->password);
    gsasl_property_set (self->session, GSASL_SERVICE, "sieve");
    if (creds->hostname != NULL)
      gsasl_property_set (self->session, GSASL_HOSTNAME, creds->hostname);
  }

  return self;
}

static gboolean
oauth_step (SieveSasl *self, const guchar *input, gsize input_len,
            guchar **out_response, gsize *out_response_len, gboolean *out_done)
{
  GString *msg;

  if (self->oauth_phase >= 1) {
    /* The server sent back a challenge after our initial message: it's
     * an error description (OAUTHBEARER §3.2.2 returns a JSON blob). We
     * send the client "kick" — a single 0x01 byte for OAUTHBEARER,
     * nothing for XOAUTH2 — then let the server produce its NO. */
    (void) input; (void) input_len;
    if (self->oauth_phase == 1 && strcmp (self->mechanism, "OAUTHBEARER") == 0) {
      *out_response = (guchar *) g_strdup ("\x01");
      *out_response_len = 1;
    } else {
      *out_response = NULL;
      *out_response_len = 0;
    }
    self->oauth_phase = 2;
    *out_done = TRUE;
    return TRUE;
  }

  msg = g_string_new (NULL);
  if (strcmp (self->mechanism, "XOAUTH2") == 0) {
    /* user=<authid>^Aauth=Bearer <token>^A^A  (Google/Microsoft) */
    g_string_append_printf (msg, "user=%s\x01" "auth=Bearer %s\x01\x01",
                            self->oauth_authid, self->oauth_token);
  } else {
    /* OAUTHBEARER (RFC 7628 §3.1):
     *   gs2-header = "n,a=" authzid-or-authid "," ; no channel binding
     *   then ^A [host=…^A] [port=…^A] "auth=Bearer " token ^A ^A */
    const gchar *a = self->oauth_authzid != NULL ? self->oauth_authzid
                                                 : self->oauth_authid;
    g_string_append (msg, "n,a=");
    gs2_append_escaped (msg, a);
    g_string_append (msg, ",\x01");
    if (self->oauth_host != NULL && self->oauth_host[0] != '\0')
      g_string_append_printf (msg, "host=%s\x01", self->oauth_host);
    if (self->oauth_port != 0)
      g_string_append_printf (msg, "port=%u\x01", self->oauth_port);
    g_string_append_printf (msg, "auth=Bearer %s\x01\x01", self->oauth_token);
  }

  *out_response_len = msg->len;
  *out_response = (guchar *) g_string_free (msg, FALSE);
  self->oauth_phase = 1;
  /* Nothing else to send if the server accepts; on error it will send
   * back a challenge and we'll come back here for the kick. */
  *out_done = TRUE;
  return TRUE;
}

gboolean
sieve_sasl_step (SieveSasl     *self,
                 const guchar  *input,
                 gsize          input_len,
                 guchar       **out_response,
                 gsize         *out_response_len,
                 gboolean      *out_done,
                 GError       **error)
{
  char *out = NULL;
  size_t out_len = 0;
  int rc;

  g_return_val_if_fail (self != NULL, FALSE);
  g_return_val_if_fail (out_response != NULL && out_response_len != NULL, FALSE);
  g_return_val_if_fail (out_done != NULL, FALSE);

  *out_response = NULL;
  *out_response_len = 0;
  *out_done = FALSE;

  if (self->is_oauth)
    return oauth_step (self, input, input_len,
                       out_response, out_response_len, out_done);

  rc = gsasl_step (self->session, (const char *) input, input_len,
                   &out, &out_len);
  if (rc != GSASL_OK && rc != GSASL_NEEDS_MORE) {
    g_set_error (error, SIEVE_SASL_ERROR, SIEVE_SASL_ERROR_MECHANISM,
                 "SASL mechanism %s failed: %s",
                 self->mechanism, gsasl_strerror (rc));
    if (out != NULL)
      gsasl_free (out);
    return FALSE;
  }

  if (out_len > 0) {
    *out_response = g_malloc (out_len);
    memcpy (*out_response, out, out_len);
    *out_response_len = out_len;
  }
  if (out != NULL)
    gsasl_free (out);

  *out_done = (rc == GSASL_OK);
  return TRUE;
}

void
sieve_sasl_free (SieveSasl *self)
{
  if (self == NULL)
    return;

  if (self->session != NULL)
    gsasl_finish (self->session);
  if (self->ctx != NULL)
    gsasl_done (self->ctx);

  g_free (self->mechanism);
  if (self->oauth_token != NULL) {
    /* The token is a secret: wipe it before releasing the memory. */
    memset (self->oauth_token, 0, strlen (self->oauth_token));
    g_free (self->oauth_token);
  }
  g_free (self->oauth_authid);
  g_free (self->oauth_authzid);
  g_free (self->oauth_host);
  g_free (self);
}
