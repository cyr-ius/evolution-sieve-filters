/* test-sieve-srv.c
 *
 * Unit tests for src/sieve-srv.[ch] — no network.
 * Only sieve_srv_domain_from_identity() is tested here: the DNS
 * resolution itself (sieve_srv_lookup_sync) depends on a network and a
 * domain publishing the record, so it's out of scope for a hermetic
 * test.
 */

#include <glib.h>

#include "sieve-srv.h"

static void
check (const gchar *input, const gchar *expected)
{
  gchar *got = sieve_srv_domain_from_identity (input);

  g_assert_cmpstr (got, ==, expected);
  g_free (got);
}

static void
test_domain_from_email (void)
{
  check ("alice@example.tld", "example.tld");
  check ("Alice.Martin@mail.example.co.uk", "mail.example.co.uk");
  /* A single '@' is expected, but we cautiously take what follows the last one. */
  check ("weird\"@\"name@example.tld", "example.tld");
}

static void
test_domain_from_host (void)
{
  /* No '@': it's a hostname, kept as-is. */
  check ("imap.example.tld", "imap.example.tld");
  check ("  mail.example.tld  ", "mail.example.tld");
}

static void
test_domain_rejects_unusable (void)
{
  check (NULL, NULL);
  check ("", NULL);
  check ("   ", NULL);
  check ("alice@", NULL);           /* nothing after the @ */
  check ("localhost", NULL);        /* single label: no public SRV */
  check ("alice@localhost", NULL);  /* same on the domain side */
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/sieve-srv/domain/email", test_domain_from_email);
  g_test_add_func ("/sieve-srv/domain/host", test_domain_from_host);
  g_test_add_func ("/sieve-srv/domain/unusable", test_domain_rejects_unusable);

  return g_test_run ();
}
