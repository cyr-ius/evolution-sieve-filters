/* sieve-srv.c — see sieve-srv.h */

#include "sieve-srv.h"

#include <string.h>

gchar *
sieve_srv_domain_from_identity (const gchar *email_or_host)
{
  const gchar *at;
  const gchar *start;
  gchar *trimmed;
  gchar *result;

  if (email_or_host == NULL)
    return NULL;

  /* Trimmed copy: we tolerate whitespace around the value as it comes
   * out of a GtkEntry or an account setting. */
  trimmed = g_strstrip (g_strdup (email_or_host));

  if (*trimmed == '\0') {
    g_free (trimmed);
    return NULL;
  }

  /* Email address: keep what follows the last '@'. Otherwise treat the
   * value as a hostname, usable as-is. */
  at = strrchr (trimmed, '@');
  start = (at != NULL) ? at + 1 : trimmed;

  if (*start == '\0' || strchr (start, '.') == NULL) {
    /* "alice@" or a single label ("localhost"): nothing usable for a
     * public SRV query. */
    g_free (trimmed);
    return NULL;
  }

  result = g_strdup (start);
  g_free (trimmed);
  return result;
}

gboolean
sieve_srv_lookup_sync (const gchar   *domain,
                       gchar        **out_host,
                       guint16       *out_port,
                       GCancellable  *cancellable,
                       GError       **error)
{
  GResolver *resolver;
  GList *targets;
  GError *local_error = NULL;
  GSrvTarget *best;

  g_return_val_if_fail (domain != NULL && *domain != '\0', FALSE);
  g_return_val_if_fail (out_host != NULL, FALSE);
  g_return_val_if_fail (out_port != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  resolver = g_resolver_get_default ();
  targets = g_resolver_lookup_service (resolver, "sieve", "tcp", domain,
                                       cancellable, &local_error);
  g_object_unref (resolver);

  if (targets == NULL) {
    /* No record (or root target "." = "no service here", which GLib
     * also reports as NOT_FOUND): a normal absence, we don't propagate
     * an error — the caller will fall back to the IMAP host. */
    if (g_error_matches (local_error, G_RESOLVER_ERROR,
                         G_RESOLVER_ERROR_NOT_FOUND)) {
      g_clear_error (&local_error);
      return FALSE;
    }
    /* Network / resolver failure, or cancellation: to be reported. */
    g_propagate_error (error, local_error);
    return FALSE;
  }

  /* g_resolver_lookup_service() already sorts by priority then weight:
   * the first target is the best one. */
  best = targets->data;
  *out_host = g_strdup (g_srv_target_get_hostname (best));
  *out_port = g_srv_target_get_port (best);

  g_resolver_free_targets (targets);
  return TRUE;
}
