/* sieve-imap-probe.h
 *
 * Minimal, read-only IMAP4rev1 probe (RFC 3501) with a single purpose:
 * discovering the account's REAL mailbox hierarchy separator -- the
 * character the server's own IMAP namespace uses between folder path
 * segments (often '.', sometimes '/').
 *
 * This matters because the Sieve model (sieve-model.[ch]) always works
 * with '/' internally -- Camel's own canonical convention, used
 * regardless of what the server actually speaks on the wire (confirmed
 * against evolution-data-server's IMAPX provider:
 * camel_imapx_mailbox_to_folder_path() normalizes to '/' unconditionally).
 * The same Dovecot server's Sieve interpreter, however, expects fileinto
 * mailbox names using its OWN separator: a literal '/' is rejected when
 * that separator is '.' (issue #3 --
 * https://github.com/cyr-ius/evolution-sieve-filters/issues/3).
 *
 * There is no public Camel/EDS API exposing this real separator (the
 * IMAPX provider's headers, including the ones that carry it, are
 * deliberately not installed -- see AGENTS.md), so the plugin opens its
 * own tiny IMAP connection to ask the server directly:
 *   - connect: implicit TLS, or STARTTLS right after the greeting
 *     (mirroring sieve-managesieve-client.c -- no plaintext mode)
 *   - LOGIN "user" "password" (RFC 3501 §6.2.3)
 *   - LIST "" "" (RFC 3501 §6.3.8): guaranteed to return the hierarchy
 *     delimiter without requiring INBOX to exist or be selectable
 *   - LOGOUT
 *
 * Deliberately narrow: plain LOGIN only. OAuth2 and GSSAPI accounts are
 * out of scope -- the caller falls back to manual entry for those.
 * Independent of Evolution: pure GLib/GIO, testable on its own (see
 * tests/test-imap-probe.c).
 */

#ifndef SIEVE_IMAP_PROBE_H
#define SIEVE_IMAP_PROBE_H

#include <gio/gio.h>

G_BEGIN_DECLS

#define SIEVE_IMAP_PROBE_ERROR (sieve_imap_probe_error_quark ())
GQuark sieve_imap_probe_error_quark (void);

typedef enum {
  SIEVE_IMAP_PROBE_ERROR_PROTOCOL,     /* malformed / unparsable server response */
  SIEVE_IMAP_PROBE_ERROR_LOGIN,        /* server rejected LOGIN ("NO"/"BAD") */
  SIEVE_IMAP_PROBE_ERROR_NO_SEPARATOR, /* flat namespace: server returned NIL */
} SieveImapProbeError;

#define SIEVE_IMAP_PROBE_DEFAULT_TIMEOUT_SECONDS 15

/* Blocking: call from a worker thread, never from the GTK main loop.
 * `implicit_tls` FALSE means STARTTLS is required right after the
 * greeting (the connection is aborted rather than continued in
 * plaintext if the server doesn't advertise it). `timeout_seconds` == 0
 * means no limit.
 *
 * On success, *out_separator receives the hierarchy delimiter character
 * and TRUE is returned. FALSE + error otherwise (network failure, LOGIN
 * rejected, unparsable response, or a flat namespace with no
 * separator). */
gboolean sieve_imap_probe_hierarchy_separator_sync (const gchar  *host,
                                                    guint16       port,
                                                    gboolean      implicit_tls,
                                                    const gchar  *user,
                                                    const gchar  *password,
                                                    guint         timeout_seconds,
                                                    GCancellable *cancellable,
                                                    gchar        *out_separator,
                                                    GError      **error);

G_END_DECLS

#endif /* SIEVE_IMAP_PROBE_H */
