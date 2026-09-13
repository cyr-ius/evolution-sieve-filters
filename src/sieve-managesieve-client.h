/* sieve-managesieve-client.h
 *
 * Generic ManageSieve protocol client (RFC 5804), independent of
 * Evolution: any GLib/GIO app can reuse it.
 *
 * Deliberately minimal scope for a first cut:
 *   - TCP connection + implicit TLS OR StartTLS (RFC 5804 §2.2, certificate
 *     validation delegated to GTlsClientConnection's default policy)
 *   - proper SASL via src/sieve-sasl.[ch]: automatic negotiation among
 *     GSSAPI (Kerberos ticket cache, only when one is actually usable),
 *     PLAIN, LOGIN, CRAM-MD5, SCRAM-SHA-1, SCRAM-SHA-256 (libgsasl) and
 *     OAUTHBEARER / XOAUTH2 (token supplied by the caller); full
 *     challenge/response loop (RFC 5804 §2.1)
 *   - LISTSCRIPTS, GETSCRIPT, PUTSCRIPT, SETACTIVE, DELETESCRIPT, CHECKSCRIPT
 *   - capability banner parsed (see _has_capability / _get_capability)
 *
 * What's missing for real-world use:
 *   - handling of synchronizing literals "{N+}" (currently we assume the
 *     server sends everything without waiting for an ack — true for most
 *     implementations but not guaranteed by the RFC)
 *   - SCRAM-*-PLUS (TLS channel binding), GS2-KRB5, EXTERNAL
 *   - automatic reconnection / command queuing
 *
 * Cancellation and timeouts: the GCancellable passed to each operation is
 * genuinely honored (GIO already did so; it is now used on the UI side,
 * see sieve-editor-dialog.c). A network timeout is applied by default to
 * establishing the connection AND to every blocking read/write — see
 * sieve_managesieve_client_set_timeout().
 */

#ifndef SIEVE_MANAGESIEVE_CLIENT_H
#define SIEVE_MANAGESIEVE_CLIENT_H

#include <gio/gio.h>

G_BEGIN_DECLS

#define SIEVE_TYPE_MANAGESIEVE_CLIENT (sieve_managesieve_client_get_type ())
G_DECLARE_FINAL_TYPE (SieveManageSieveClient, sieve_managesieve_client,
                      SIEVE, MANAGESIEVE_CLIENT, GObject)

typedef enum {
  SIEVE_MANAGESIEVE_ERROR_PROTOCOL,   /* malformed server response */
  SIEVE_MANAGESIEVE_ERROR_SERVER_NO,  /* server replied "NO ..." */
  SIEVE_MANAGESIEVE_ERROR_SERVER_BYE, /* server closed the session */
  SIEVE_MANAGESIEVE_ERROR_AUTH,
  SIEVE_MANAGESIEVE_ERROR_TIMEOUT     /* network timeout exceeded */
} SieveManageSieveError;

#define SIEVE_MANAGESIEVE_ERROR (sieve_managesieve_error_quark ())
GQuark sieve_managesieve_error_quark (void);

SieveManageSieveClient *
sieve_managesieve_client_new (const gchar *host,
                               guint16      port,
                               gboolean     implicit_tls);

/* Timeout applied to the TCP connection + TLS handshake, then to every
 * blocking read/write of the session. Default value for a fresh client.
 * 0 = no limit (wait indefinitely). */
#define SIEVE_MANAGESIEVE_DEFAULT_TIMEOUT_SECONDS 30

/* Changes the network timeout (in seconds). Set it before
 * sieve_managesieve_client_connect_sync() to cover the connection; a call
 * after connecting applies to subsequent operations. Past this delay, the
 * ongoing operation fails with code SIEVE_MANAGESIEVE_ERROR_TIMEOUT (the
 * GCancellable remains the way to interrupt earlier, at the user's
 * request). */
void  sieve_managesieve_client_set_timeout (SieveManageSieveClient *self,
                                            guint                   timeout_seconds);
guint sieve_managesieve_client_get_timeout (SieveManageSieveClient *self);

/* Connect + (StartTLS if requested) + read the capability banner.
 * Blocking: call from a worker thread, never from Evolution's main GTK
 * thread. */
gboolean sieve_managesieve_client_connect_sync (SieveManageSieveClient *self,
                                                 GCancellable           *cancellable,
                                                 GError                **error);

/* Identity and secrets presented to the server. Set `password` for the
 * classic mechanisms (PLAIN/LOGIN/CRAM-MD5/SCRAM-*) OR `oauth2_token` for
 * OAUTHBEARER/XOAUTH2 — acquiring the OAuth2 token is the caller's
 * responsibility. `authzid` is almost always NULL. */
typedef struct {
  const gchar *authid;        /* username (required) */
  const gchar *authzid;       /* authorization identity (often NULL) */
  const gchar *password;      /* PLAIN, LOGIN, CRAM-MD5, SCRAM-* */
  const gchar *oauth2_token;  /* bare access token: OAUTHBEARER, XOAUTH2 */
} SieveManageSieveAuth;

/* Authenticates the session (AUTHENTICATE, RFC 5804 §2.1).
 *   mechanism == NULL: automatic negotiation — the best mechanism common
 *     to the server's SASL capabilities and what `auth` allows.
 *   mechanism != NULL: forces this mechanism (fails if not advertised by
 *     the server or not supported).
 * Call after sieve_managesieve_client_connect_sync(), from a worker
 * thread. On authentication failure, the error carries the code
 * SIEVE_MANAGESIEVE_ERROR_AUTH. */
gboolean sieve_managesieve_client_authenticate_sync (SieveManageSieveClient     *self,
                                                     const gchar                *mechanism,
                                                     const SieveManageSieveAuth *auth,
                                                     GCancellable               *cancellable,
                                                     GError                    **error);

/* Raw value of the "SASL" capability advertised by the server (mechanisms
 * separated by spaces), or NULL if absent. Valid after connect_sync().
 * String owned by the client. */
const gchar *sieve_managesieve_client_get_sasl_capability (SieveManageSieveClient *self);

/* SASL mechanism actually negotiated during the last successful
 * authenticate_sync(), or NULL if authentication hasn't succeeded yet.
 * String owned by the client. */
const gchar *sieve_managesieve_client_get_auth_mechanism (SieveManageSieveClient *self);

/* Returns a list of script names (gchar* to be freed);
 * *out_active receives the active script's name (may be NULL), to be freed. */
GPtrArray *sieve_managesieve_client_list_scripts_sync (SieveManageSieveClient *self,
                                                        gchar                 **out_active,
                                                        GCancellable           *cancellable,
                                                        GError                **error);

gchar *sieve_managesieve_client_get_script_sync (SieveManageSieveClient *self,
                                                  const gchar            *name,
                                                  GCancellable           *cancellable,
                                                  GError                **error);

gboolean sieve_managesieve_client_put_script_sync (SieveManageSieveClient *self,
                                                    const gchar            *name,
                                                    const gchar            *content,
                                                    GCancellable           *cancellable,
                                                    GError                **error);

gboolean sieve_managesieve_client_set_active_sync (SieveManageSieveClient *self,
                                                    const gchar            *name,
                                                    GCancellable           *cancellable,
                                                    GError                **error);

gboolean sieve_managesieve_client_delete_script_sync (SieveManageSieveClient *self,
                                                       const gchar            *name,
                                                       GCancellable           *cancellable,
                                                       GError                **error);

/* Validates a script's syntax on the server side without installing it
 * (CHECKSCRIPT extension, RFC 5804 §2.10 — not supported by all servers:
 * check the "SIEVE" capability returned on connect). */
gboolean sieve_managesieve_client_check_script_sync (SieveManageSieveClient *self,
                                                      const gchar            *content,
                                                      GCancellable           *cancellable,
                                                      GError                **error);

void sieve_managesieve_client_disconnect (SieveManageSieveClient *self);

G_END_DECLS

#endif /* SIEVE_MANAGESIEVE_CLIENT_H */
