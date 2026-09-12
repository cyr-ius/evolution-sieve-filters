/* sieve-account.c — see sieve-account.h */

#include "sieve-account.h"

#include <string.h>

void
sieve_account_info_free (SieveAccountInfo *info)
{
  if (info == NULL)
    return;

  g_free (info->source_uid);
  g_free (info->display_name);
  g_free (info->host);
  g_free (info->user);
  g_free (info->identity_address);
  g_free (info);
}

/* Email address of the identity linked to the account ("Mail Account"
 * references a "Mail Identity" source via its identity-uid). NULL if
 * not found or empty: SRV auto-discovery will then fall back to the
 * host name. */
static gchar *
dup_identity_address (ESourceRegistry *registry,
                      ESource         *account_source)
{
  ESourceMailAccount *account_ext;
  gchar *identity_uid;
  ESource *identity_source;
  gchar *address = NULL;

  if (!e_source_has_extension (account_source, E_SOURCE_EXTENSION_MAIL_ACCOUNT))
    return NULL;

  account_ext = e_source_get_extension (account_source,
                                        E_SOURCE_EXTENSION_MAIL_ACCOUNT);
  identity_uid = e_source_mail_account_dup_identity_uid (account_ext);
  if (identity_uid == NULL || *identity_uid == '\0') {
    g_free (identity_uid);
    return NULL;
  }

  identity_source = e_source_registry_ref_source (registry, identity_uid);
  g_free (identity_uid);
  if (identity_source == NULL)
    return NULL;

  if (e_source_has_extension (identity_source, E_SOURCE_EXTENSION_MAIL_IDENTITY)) {
    ESourceMailIdentity *identity_ext =
      e_source_get_extension (identity_source, E_SOURCE_EXTENSION_MAIL_IDENTITY);
    address = e_source_mail_identity_dup_address (identity_ext);
    if (address != NULL && *address == '\0')
      g_clear_pointer (&address, g_free);
  }

  g_object_unref (identity_source);
  return address;
}

/* TRUE if the source carries an "Authentication" extension whose method
 * is OAuth2 (either the generic "OAuth2" keyword or a known EDS
 * provider alias: "Google", "Office365", "Yahoo"...). */
static gboolean
source_auth_is_oauth2 (ESource *source)
{
  ESourceAuthentication *auth_ext;
  g_autofree gchar *method = NULL;

  if (!e_source_has_extension (source, E_SOURCE_EXTENSION_AUTHENTICATION))
    return FALSE;

  auth_ext = e_source_get_extension (source, E_SOURCE_EXTENSION_AUTHENTICATION);
  method = e_source_authentication_dup_method (auth_ext);
  if (method == NULL || *method == '\0')
    return FALSE;

  if (g_ascii_strcasecmp (method, "OAuth2") == 0)
    return TRUE;

  return e_oauth2_services_is_oauth2_alias_static (method);
}

/* The OAuth2 authentication method may be carried by the account
 * itself OR by its parent "collection" source (GNOME Online Accounts
 * accounts, grouped accounts...). We walk up the parent chain, with a
 * depth guard. Exposed (sieve-account.h): the account config page
 * (sieve-config-page) uses it to disable the password field for
 * OAuth2 accounts. */
gboolean
sieve_account_source_uses_oauth2 (ESourceRegistry *registry,
                                  ESource         *account_source)
{
  ESource *current;
  gboolean result = FALSE;

  if (registry == NULL || account_source == NULL)
    return FALSE;

  current = g_object_ref (account_source);

  for (guint depth = 0; current != NULL && depth < 8; depth++) {
    const gchar *parent_uid;
    ESource *parent;

    if (source_auth_is_oauth2 (current)) {
      result = TRUE;
      break;
    }

    parent_uid = e_source_get_parent (current);
    if (parent_uid == NULL || *parent_uid == '\0')
      break;

    parent = e_source_registry_ref_source (registry, parent_uid);
    g_object_unref (current);
    current = parent;
  }

  g_clear_object (&current);
  return result;
}

GList *
sieve_account_list (ESourceRegistry *registry)
{
  GList *sources;
  GList *link;
  GList *result = NULL;

  if (registry == NULL)
    return NULL;

  sources = e_source_registry_list_enabled (registry,
                                            E_SOURCE_EXTENSION_MAIL_ACCOUNT);

  for (link = sources; link != NULL; link = link->next) {
    ESource *source = link->data;
    ESourceAuthentication *auth_ext;
    SieveAccountInfo *info;
    gchar *host;

    if (!e_source_has_extension (source, E_SOURCE_EXTENSION_AUTHENTICATION))
      continue;

    auth_ext = e_source_get_extension (source, E_SOURCE_EXTENSION_AUTHENTICATION);
    host = e_source_authentication_dup_host (auth_ext);
    if (host == NULL || *host == '\0') {
      /* Local "On This Computer" account or account with no server:
       * nothing to manage on the Sieve side. */
      g_free (host);
      continue;
    }

    info = g_new0 (SieveAccountInfo, 1);
    info->source_uid = g_strdup (e_source_get_uid (source));
    info->display_name = g_strdup (e_source_get_display_name (source));
    info->host = host; /* ownership transferred */
    info->user = e_source_authentication_dup_user (auth_ext);
    info->identity_address = dup_identity_address (registry, source);
    info->uses_oauth2 = sieve_account_source_uses_oauth2 (registry, source);

    result = g_list_prepend (result, info);
  }

  g_list_free_full (sources, g_object_unref);

  return g_list_reverse (result);
}

gchar *
sieve_account_dup_oauth2_token (ESourceRegistry *registry,
                                const gchar     *source_uid,
                                GCancellable    *cancellable,
                                gint            *out_expires_in,
                                GError         **error)
{
  ESource *account_source;
  ESource *cred_source;
  ESourceCredentialsProvider *provider;
  gchar *token = NULL;
  gint expires_in = 0;

  if (out_expires_in != NULL)
    *out_expires_in = 0;

  if (registry == NULL || source_uid == NULL || *source_uid == '\0') {
    g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                         "No associated Evolution account: cannot obtain "
                         "an OAuth2 token (use a password or select an "
                         "account).");
    return NULL;
  }

  account_source = e_source_registry_ref_source (registry, source_uid);
  if (account_source == NULL) {
    g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                         "Evolution account not found in the source "
                         "registry.");
    return NULL;
  }

  /* The token (and its refresh) may be attached to the parent
   * "collection" source: ask EDS which source actually carries the
   * credentials before requesting the token. */
  provider = e_source_credentials_provider_new (registry);
  cred_source = (provider != NULL)
                  ? e_source_credentials_provider_ref_credentials_source (provider,
                                                                         account_source)
                  : NULL;

  {
    ESource *token_source = (cred_source != NULL) ? cred_source : account_source;
    gboolean ok = e_source_get_oauth2_access_token_sync (token_source, cancellable,
                                                         &token, &expires_in, error);

    g_clear_object (&provider);
    g_clear_object (&cred_source);
    g_object_unref (account_source);

    if (!ok)
      /* e_source_get_oauth2_access_token_sync has already set an error;
       * leave it as-is: the caller presents it to the user. */
      return NULL;
  }

  if (token == NULL || *token == '\0') {
    g_clear_pointer (&token, g_free);
    g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                         "Evolution did not provide an OAuth2 token for "
                         "this account: re-authorize it in Edit -> "
                         "Accounts.");
    return NULL;
  }

  if (out_expires_in != NULL)
    *out_expires_in = expires_in;

  return token;
}

gchar *
sieve_account_dup_stored_password (ESourceRegistry *registry,
                                   const gchar     *source_uid,
                                   GCancellable    *cancellable,
                                   GError         **error)
{
  ESource *account_source;
  ESourceCredentialsProvider *provider;
  ENamedParameters *credentials = NULL;
  gchar *password = NULL;
  gboolean ok;

  if (registry == NULL || source_uid == NULL || *source_uid == '\0')
    return NULL;

  account_source = e_source_registry_ref_source (registry, source_uid);
  if (account_source == NULL)
    return NULL;

  /* lookup_sync itself resolves the "collection" source carrying the
   * credentials when needed. */
  provider = e_source_credentials_provider_new (registry);
  ok = e_source_credentials_provider_lookup_sync (provider, account_source,
                                                  cancellable, &credentials, error);
  g_clear_object (&provider);
  g_object_unref (account_source);

  if (!ok)
    return NULL;

  if (credentials != NULL) {
    const gchar *stored =
      e_named_parameters_get (credentials, E_SOURCE_CREDENTIAL_PASSWORD);
    if (stored != NULL && *stored != '\0')
      password = g_strdup (stored);
    e_named_parameters_free (credentials);
  }

  return password;
}
