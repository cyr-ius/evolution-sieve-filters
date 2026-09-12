/* sieve-secret.h
 *
 * Stores the ManageSieve password in the keyring (Secret Service /
 * libsecret) rather than in plaintext in a volatile GtkEntry — see
 * README.md "What's missing", point 2.
 *
 * Thin layer above libsecret's "simple password" API: everything is
 * synchronous (like the ManageSieve client), the caller wraps it in a
 * GTask if needed. Dependencies: GLib/GIO + libsecret only, no dependency
 * on Evolution or GTK — hence testable on its own.
 *
 * The entry is indexed by (host, port, user): that's the triplet
 * identifying a ManageSieve account. The schema is specific to this
 * plugin ("net.ipocus.evolution.SieveFilters"); we do not claim to share
 * the IMAP account entry managed by evolution-data-server.
 */
#ifndef SIEVE_SECRET_H
#define SIEVE_SECRET_H

#include <glib.h>
#include <gio/gio.h>

G_BEGIN_DECLS

/* Returns the password stored for (host, port, user), or NULL if there is
 * none (in that case *error stays NULL). NULL with *error set = a real
 * failure (Secret Service unreachable, collection locked…).
 * The returned string is allocated in "non-pageable" memory by libsecret:
 * free it with secret_password_free() — exposed here as
 * sieve_secret_password_free() so callers don't need <libsecret/secret.h>. */
gchar *sieve_secret_lookup_password_sync (const gchar  *host,
                                          guint16       port,
                                          const gchar  *user,
                                          GCancellable *cancellable,
                                          GError      **error);

/* Stores (or replaces) the password for (host, port, user) in the
 * keyring's default collection. FALSE + *error on failure. */
gboolean sieve_secret_store_password_sync (const gchar  *host,
                                           guint16       port,
                                           const gchar  *user,
                                           const gchar  *password,
                                           GCancellable *cancellable,
                                           GError      **error);

/* Removes the entry for (host, port, user). TRUE if an entry was
 * removed, FALSE otherwise; *error only on a real failure. */
gboolean sieve_secret_clear_password_sync (const gchar  *host,
                                           guint16       port,
                                           const gchar  *user,
                                           GCancellable *cancellable,
                                           GError      **error);

/* Frees a string returned by sieve_secret_lookup_password_sync().
 * Wipes the buffer before releasing it (secret_password_free). NULL is
 * accepted. */
void sieve_secret_password_free (gchar *password);

G_END_DECLS

#endif /* SIEVE_SECRET_H */
