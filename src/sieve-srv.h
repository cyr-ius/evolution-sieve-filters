/* sieve-srv.h
 *
 * Auto-discovery of the ManageSieve server via the `_sieve._tcp` DNS SRV
 * record (RFC 5804 §3: the "sieve" service is registered there).
 *
 * Standalone module — GLib/GIO only, no dependency on Evolution or GTK:
 * reusable and testable on its own (tests/test-sieve-srv). Some
 * providers publish this record, many don't; the caller must therefore
 * always provide a manual entry fallback.
 */

#ifndef SIEVE_SRV_H
#define SIEVE_SRV_H

#include <gio/gio.h>

G_BEGIN_DECLS

/* Domain to query via SRV, derived from an account identity.
 *   "alice@example.tld"   -> "example.tld"   (part after the @)
 *   "imap.example.tld"    -> "imap.example.tld" (hostname left as-is)
 *   "  "  / NULL / "a@"    -> NULL
 * String to free with g_free(). */
gchar *sieve_srv_domain_from_identity (const gchar *email_or_host);

/* Resolves `_sieve._tcp.<domain>` (blocking: call from a worker thread,
 * never from the main GTK loop).
 *
 * Returns:
 *   TRUE  — at least one target; *out_host / *out_port receive the
 *           highest-priority target (g_resolver_lookup_service already
 *           sorts by priority/weight). *out_host is to be freed with
 *           g_free().
 *   FALSE, *error NOT set — no record (normal case: the provider
 *           publishes nothing, or publishes the root target "." meaning
 *           "service not provided here"). The caller falls back to the
 *           IMAP host.
 *   FALSE, *error set — network / resolver failure.
 *
 * *out_host / *out_port are only written when the function returns TRUE. */
gboolean sieve_srv_lookup_sync (const gchar   *domain,
                                gchar        **out_host,
                                guint16       *out_port,
                                GCancellable  *cancellable,
                                GError       **error);

G_END_DECLS

#endif /* SIEVE_SRV_H */
