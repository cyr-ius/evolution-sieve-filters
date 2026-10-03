/* sieve-imap-probe-private.h
 *
 * Internal entry point of sieve-imap-probe.c, for its unit tests only
 * (tests/test-imap-probe-parser.c). Not part of the probe's API: nothing
 * outside tests/ should include this.
 */

#ifndef SIEVE_IMAP_PROBE_PRIVATE_H
#define SIEVE_IMAP_PROBE_PRIVATE_H

#include "sieve-imap-probe.h"

G_BEGIN_DECLS

/* Same as sieve_imap_probe_hierarchy_separator_sync(), but on an
 * already-open stream instead of connecting (no TCP, no TLS): reads the
 * greeting, then LOGIN / LIST "" "" / LOGOUT exactly as the real probe
 * does. Lets the response reader be fed canned server bytes (e.g. a
 * GSimpleIOStream over memory streams). Takes its own reference on
 * `stream`, and closes it before returning. */
gboolean sieve_imap_probe_hierarchy_separator_on_stream_for_testing (GIOStream    *stream,
                                                                     const gchar  *user,
                                                                     const gchar  *password,
                                                                     GCancellable *cancellable,
                                                                     gchar        *out_separator,
                                                                     GError      **error);

G_END_DECLS

#endif /* SIEVE_IMAP_PROBE_PRIVATE_H */
