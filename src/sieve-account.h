/* sieve-account.h
 *
 * Reads Evolution mail account settings to pre-fill the Sieve dialog:
 * host, user, and (if available) an email identity used as the basis
 * for `_sieve._tcp` SRV auto-discovery.
 *
 * Also provides acquisition of an OAuth 2.0 access token for accounts
 * that authenticate this way (Gmail, Microsoft 365...): Evolution's
 * OAuth2 flow (`ESource` + `e-credentials-prompter` + keyring) is
 * delegated to evolution-data-server, which handles both the initial
 * acquisition AND the refresh. The bare token obtained is then fed
 * into `SieveManageSieveAuth.oauth2_token` (the client's OAUTHBEARER /
 * XOAUTH2 mechanisms, already in place).
 *
 * Unlike the rest of the reusable core, this module DEPENDS on
 * Evolution Data Server (libedataserver): it is only built together
 * with the Evolution module. The ManageSieve port cannot be inferred
 * from the IMAP account (RFC 5804: port 4190, distinct from IMAP) —
 * the caller sets the default value, later adjusted via SRV or
 * manually.
 */

#ifndef SIEVE_ACCOUNT_H
#define SIEVE_ACCOUNT_H

#include <libedataserver/libedataserver.h>

G_BEGIN_DECLS

typedef struct {
  gchar *source_uid;     /* UID of the mail account's ESource */
  gchar *display_name;   /* human-readable account label */
  gchar *host;           /* IMAP authentication host — fallback if no SRV */
  gchar *user;           /* login identifier */
  gchar *identity_address; /* account's email address, or NULL — SRV basis */
  gboolean uses_oauth2;  /* TRUE: the account authenticates via OAuth2 (token
                          * to obtain via sieve_account_dup_oauth2_token) —
                          * no password to enter or store */
} SieveAccountInfo;

void sieve_account_info_free (SieveAccountInfo *info);

/* Enabled Evolution mail accounts that expose an authentication host
 * (this excludes "On This Computer" and any account without a
 * server). Returns: a GList of SieveAccountInfo*, possibly empty, to
 * be freed with
 *   g_list_free_full (list, (GDestroyNotify) sieve_account_info_free);
 * `registry` may be NULL (in which case NULL is returned). */
GList *sieve_account_list (ESourceRegistry *registry);

/* TRUE if the `account_source` account (or one of its parent sources:
 * a GNOME Online Accounts collection, a grouped account...)
 * authenticates via OAuth2. In that case there is no password to enter
 * or store in the keyring: the token is obtained via
 * sieve_account_dup_oauth2_token(). `registry` or `account_source`
 * NULL => FALSE. */
gboolean sieve_account_source_uses_oauth2 (ESourceRegistry *registry,
                                           ESource         *account_source);

/* Obtains an OAuth 2.0 access token for the `source_uid` account,
 * relying entirely on evolution-data-server: initial acquisition (via
 * e-credentials-prompter on Evolution's side) and transparent refresh
 * of an expired token are handled by EDS. The returned token is bare
 * (to be passed as-is to SieveManageSieveAuth.oauth2_token); free it
 * with g_free() (it is wiped from memory beforehand if non-NULL).
 *
 * Blocking: call from a worker thread (the dialog already does this
 * for every network operation). `out_expires_in` (optional) receives
 * the remaining validity in seconds, 0 if unknown.
 *
 * Returns: the token (string to free), or NULL + `error`. An account
 * not using OAuth2, a revoked token, or lack of connectivity all
 * yield NULL + error — the caller then shows a message inviting the
 * user to re-authorize the account in Evolution's preferences. */
gchar *sieve_account_dup_oauth2_token (ESourceRegistry *registry,
                                       const gchar     *source_uid,
                                       GCancellable    *cancellable,
                                       gint            *out_expires_in,
                                       GError         **error);

/* Reads back the password already stored by evolution-data-server for
 * the `source_uid` account (the "Mail Account" keyring entry, managed
 * by EDS's credentials provider). Serves as a last resort when the
 * user hasn't typed anything and the plugin has no entry of its own:
 * the ManageSieve password is very often the same as IMAP's
 * (Dovecot).
 *
 * Blocking: worker thread. Best-effort — a missing entry, a locked
 * keyring, or an OAuth2 account simply yield NULL (without
 * necessarily setting an `error`). Token/string to free with g_free()
 * (wipe it from memory beforehand: it's a secret). */
gchar *sieve_account_dup_stored_password (ESourceRegistry *registry,
                                          const gchar     *source_uid,
                                          GCancellable    *cancellable,
                                          GError         **error);

G_END_DECLS

#endif /* SIEVE_ACCOUNT_H */
