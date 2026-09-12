/* sieve-sasl.h
 *
 * SASL layer for the ManageSieve client, isolated behind a minimal
 * interface with no dependency on Evolution: reusable and testable on its
 * own (tests/test-sasl).
 *
 * The cryptographic work for the "classic" mechanisms (PLAIN, LOGIN,
 * CRAM-MD5, SCRAM-SHA-1, SCRAM-SHA-256) is delegated to GNU SASL (libgsasl).
 * The OAuth 2.0 mechanisms (OAUTHBEARER — RFC 7628 — and XOAUTH2, a
 * Google/Microsoft proprietary one) aren't provided by libgsasl: they are
 * built here, from an access token the caller has already obtained
 * (acquiring and refreshing the token stays out of this module's scope).
 *
 * Out of scope for now: SCRAM-*-PLUS (TLS channel binding),
 * GSSAPI/GS2-KRB5, EXTERNAL.
 */

#ifndef SIEVE_SASL_H
#define SIEVE_SASL_H

#include <glib.h>

G_BEGIN_DECLS

#define SIEVE_SASL_ERROR (sieve_sasl_error_quark ())
GQuark sieve_sasl_error_quark (void);

typedef enum {
  SIEVE_SASL_ERROR_NO_MECHANISM,  /* no common mechanism / forced one unavailable */
  SIEVE_SASL_ERROR_MISSING_CRED,  /* required credential missing for this mechanism */
  SIEVE_SASL_ERROR_MECHANISM,     /* internal mechanism failure (libgsasl, encoding…) */
} SieveSaslError;

typedef struct {
  const gchar *authid;        /* authentication identity (required) */
  const gchar *authzid;       /* authorization identity (often NULL) */
  const gchar *password;      /* required by PLAIN, LOGIN, CRAM-MD5, SCRAM-* */
  const gchar *oauth2_token;  /* bare access token, required by OAUTHBEARER / XOAUTH2 */
  const gchar *hostname;      /* OAUTHBEARER: host= field (NULL => omitted) */
  guint16      port;          /* OAUTHBEARER: port= field (0 => omitted) */
} SieveSaslCredentials;

/* Mechanisms this module can drive, listed from most to least desirable.
 * Static NULL-terminated array (do not free). Used as the reference table
 * for negotiation and to validate a forced mechanism. */
const gchar * const *sieve_sasl_known_mechanisms (void);

/* Chooses the SASL mechanism to use.
 *   server_mechs : raw content of the "SASL" capability advertised by the
 *                  server (names separated by spaces), or NULL / "".
 *   force_mech   : if non-NULL, forces this mechanism — it must be known
 *                  to the module, advertised by the server, and satisfied
 *                  by creds, otherwise NULL + error.
 *   creds        : used to discard mechanisms whose credential is missing
 *                  (SCRAM/PLAIN/… without a password, OAUTH* without a token).
 * Returns: the mechanism's name (string to free with g_free), or NULL + error. */
gchar *sieve_sasl_select_mechanism (const gchar                *server_mechs,
                                    const gchar                *force_mech,
                                    const SieveSaslCredentials *creds,
                                    GError                    **error);

/* TRUE if the mechanism sends data with its first message, before any
 * server challenge (PLAIN, SCRAM-*, OAUTHBEARER, XOAUTH2); FALSE if the
 * server speaks first (LOGIN, CRAM-MD5). Introspection helper —
 * sieve_sasl_step() handles both cases regardless. */
gboolean sieve_sasl_mechanism_is_client_first (const gchar *mechanism);

typedef struct _SieveSasl SieveSasl;

/* mechanism: case-insensitive; must appear in
 * sieve_sasl_known_mechanisms(). Fails if a required credential is missing. */
SieveSasl *sieve_sasl_new (const gchar                *mechanism,
                           const SieveSaslCredentials *creds,
                           GError                    **error);

/* One round-trip of the negotiation.
 *   input / input_len   : server challenge ALREADY decoded (base64
 *                         removed); NULL / 0 to kick off a client-first
 *                         mechanism.
 *   out_response / _len  : raw bytes to send back (the caller base64-encodes
 *                          them); (NULL, 0) is a valid empty response.
 *                          Free with g_free().
 *   out_done            : TRUE when the client has nothing more to send
 *                          (the caller then reads the final OK/NO response).
 * Returns FALSE + error on mechanism failure. */
gboolean sieve_sasl_step (SieveSasl     *self,
                          const guchar  *input,
                          gsize          input_len,
                          guchar       **out_response,
                          gsize         *out_response_len,
                          gboolean      *out_done,
                          GError       **error);

void sieve_sasl_free (SieveSasl *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (SieveSasl, sieve_sasl_free)

G_END_DECLS

#endif /* SIEVE_SASL_H */
