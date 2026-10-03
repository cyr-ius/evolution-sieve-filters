/* sieve-managesieve-client-private.h
 *
 * Internal entry points of sieve-managesieve-client.c, for its unit tests
 * only (tests/test-managesieve-parser.c). Not part of the client's API:
 * nothing outside tests/ should include this.
 */

#ifndef SIEVE_MANAGESIEVE_CLIENT_PRIVATE_H
#define SIEVE_MANAGESIEVE_CLIENT_PRIVATE_H

#include "sieve-managesieve-client.h"

G_BEGIN_DECLS

/* Attaches the client to an already-open stream instead of connecting
 * (no TCP, no TLS), then reads the capability banner from it exactly as
 * sieve_managesieve_client_connect_sync() does. Lets the response parser
 * be fed canned server bytes (e.g. a GSimpleIOStream over memory
 * streams). Takes its own reference on `stream`. */
gboolean sieve_managesieve_client_attach_stream_for_testing (SieveManageSieveClient *self,
                                                             GIOStream              *stream,
                                                             GCancellable           *cancellable,
                                                             GError                **error);

G_END_DECLS

#endif /* SIEVE_MANAGESIEVE_CLIENT_PRIVATE_H */
