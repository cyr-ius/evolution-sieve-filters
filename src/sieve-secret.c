/* sieve-secret.c — see sieve-secret.h
 *
 * Implementation on top of secret_password_{lookup,store,clear}_sync().
 * These functions talk to the Secret Service over D-Bus: with no service
 * available (no D-Bus session, no gnome-keyring / kwallet…), they return
 * a clean failure with GError — the caller decides what to do about it
 * (the UI falls back to manual entry).
 */

#include "sieve-secret.h"

#include <libsecret/secret.h>

/* Keyring schema for a ManageSieve password.
 *
 * SECRET_SCHEMA_DONT_MATCH_NAME: the entry is found by its attributes
 * (host/port/user) without requiring the schema name to match — more
 * robust if the name were to change. The attributes themselves must
 * match exactly. */
static const SecretSchema *
sieve_secret_get_schema (void)
{
  static const SecretSchema schema = {
    .name = "net.ipocus.evolution.SieveFilters",
    .flags = SECRET_SCHEMA_DONT_MATCH_NAME,
    .attributes = {
      { "protocol", SECRET_SCHEMA_ATTRIBUTE_STRING },
      { "host",     SECRET_SCHEMA_ATTRIBUTE_STRING },
      { "port",     SECRET_SCHEMA_ATTRIBUTE_INTEGER },
      { "user",     SECRET_SCHEMA_ATTRIBUTE_STRING },
      { "NULL", 0 },
    }
  };

  return &schema;
}

gchar *
sieve_secret_lookup_password_sync (const gchar  *host,
                                   guint16       port,
                                   const gchar  *user,
                                   GCancellable *cancellable,
                                   GError      **error)
{
  g_return_val_if_fail (host != NULL && *host != '\0', NULL);
  g_return_val_if_fail (user != NULL && *user != '\0', NULL);
  g_return_val_if_fail (error == NULL || *error == NULL, NULL);

  return secret_password_lookup_sync (sieve_secret_get_schema (),
                                      cancellable, error,
                                      "protocol", "managesieve",
                                      "host", host,
                                      "port", (gint) port,
                                      "user", user,
                                      NULL);
}

gboolean
sieve_secret_store_password_sync (const gchar  *host,
                                  guint16       port,
                                  const gchar  *user,
                                  const gchar  *password,
                                  GCancellable *cancellable,
                                  GError      **error)
{
  gchar *label;
  gboolean ok;

  g_return_val_if_fail (host != NULL && *host != '\0', FALSE);
  g_return_val_if_fail (user != NULL && *user != '\0', FALSE);
  g_return_val_if_fail (password != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  /* Label shown in keyring managers (seahorse…). */
  label = g_strdup_printf ("Sieve Filters — %s@%s:%u", user, host, port);

  ok = secret_password_store_sync (sieve_secret_get_schema (),
                                   SECRET_COLLECTION_DEFAULT, label, password,
                                   cancellable, error,
                                   "protocol", "managesieve",
                                   "host", host,
                                   "port", (gint) port,
                                   "user", user,
                                   NULL);

  g_free (label);
  return ok;
}

gboolean
sieve_secret_clear_password_sync (const gchar  *host,
                                  guint16       port,
                                  const gchar  *user,
                                  GCancellable *cancellable,
                                  GError      **error)
{
  g_return_val_if_fail (host != NULL && *host != '\0', FALSE);
  g_return_val_if_fail (user != NULL && *user != '\0', FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  return secret_password_clear_sync (sieve_secret_get_schema (),
                                     cancellable, error,
                                     "protocol", "managesieve",
                                     "host", host,
                                     "port", (gint) port,
                                     "user", user,
                                     NULL);
}

void
sieve_secret_password_free (gchar *password)
{
  if (password != NULL)
    secret_password_free (password);
}
