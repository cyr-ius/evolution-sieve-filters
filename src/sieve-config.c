/* sieve-config.c — see sieve-config.h
 *
 * Implementation on top of GKeyFile. One group per account profile:
 *
 *   [state]
 *   last-account=<ESource UID>          (missing / "" => none / manual)
 *
 *   [manual]           <- dialog's "manual entry" profile
 *   [account <UID>]     <- one profile per Evolution account
 *   host=...  port=...  user=...  implicit-tls=...  auto-connect=...  remember-password=...
 *
 * Tolerant: a missing or corrupt file = default values, never a
 * failure on the read side (the dialog must be able to open in all
 * cases). Every write is a read-modify-write so as not to lose other
 * profiles.
 */

#include "sieve-config.h"

#include <errno.h>

#define SIEVE_CONFIG_SUBDIR    "evolution-sieve-filters"
#define SIEVE_CONFIG_BASENAME  "state.ini"
#define SIEVE_STATE_GROUP      "state"
#define SIEVE_MANUAL_GROUP     "manual"
#define SIEVE_LAST_ACCOUNT_KEY "last-account"
#define SIEVE_LEGACY_GROUP     "connection"   /* original single-profile format */

static gchar *
sieve_config_path (void)
{
  return g_build_filename (g_get_user_config_dir (),
                           SIEVE_CONFIG_SUBDIR,
                           SIEVE_CONFIG_BASENAME,
                           NULL);
}

/* GKeyFile group name for an account. account_uid NULL / "" =>
 * "manual" profile. ESource UIDs contain neither brackets nor
 * newlines: safe as a group name suffix. */
static gchar *
group_for_account (const gchar *account_uid)
{
  if (account_uid == NULL || *account_uid == '\0')
    return g_strdup (SIEVE_MANUAL_GROUP);
  return g_strdup_printf ("account %s", account_uid);
}

/* Best-effort migration from the old single-profile format
 * ([connection] with an account-uid key) to one profile per account +
 * [state].last-account. Done in memory on every load until a write
 * persists it; never overwrites a profile that already exists. */
static void
migrate_legacy_connection (GKeyFile *kf)
{
  g_autofree gchar *uid = NULL;
  g_autofree gchar *host = NULL;
  g_autofree gchar *user = NULL;
  g_autofree gchar *group = NULL;

  if (!g_key_file_has_group (kf, SIEVE_LEGACY_GROUP))
    return;

  uid  = g_key_file_get_string (kf, SIEVE_LEGACY_GROUP, "account-uid", NULL);
  host = g_key_file_get_string (kf, SIEVE_LEGACY_GROUP, "host", NULL);
  user = g_key_file_get_string (kf, SIEVE_LEGACY_GROUP, "user", NULL);
  group = group_for_account (uid);

  if (!g_key_file_has_group (kf, group)) {
    g_key_file_set_string (kf, group, "host", host != NULL ? host : "");
    g_key_file_set_string (kf, group, "user", user != NULL ? user : "");
    g_key_file_set_integer (kf, group, "port",
                            g_key_file_get_integer (kf, SIEVE_LEGACY_GROUP,
                                                    "port", NULL));
    g_key_file_set_boolean (kf, group, "implicit-tls",
                            g_key_file_get_boolean (kf, SIEVE_LEGACY_GROUP,
                                                    "implicit-tls", NULL));
    g_key_file_set_boolean (kf, group, "auto-connect",
      g_key_file_has_key (kf, SIEVE_LEGACY_GROUP, "auto-connect", NULL)
        ? g_key_file_get_boolean (kf, SIEVE_LEGACY_GROUP, "auto-connect", NULL)
        : TRUE);
    g_key_file_set_boolean (kf, group, "remember-password",
      g_key_file_has_key (kf, SIEVE_LEGACY_GROUP, "remember-password", NULL)
        ? g_key_file_get_boolean (kf, SIEVE_LEGACY_GROUP, "remember-password", NULL)
        : TRUE);
  }

  if (uid != NULL && *uid != '\0'
      && !g_key_file_has_key (kf, SIEVE_STATE_GROUP, SIEVE_LAST_ACCOUNT_KEY, NULL))
    g_key_file_set_string (kf, SIEVE_STATE_GROUP, SIEVE_LAST_ACCOUNT_KEY, uid);

  g_key_file_remove_group (kf, SIEVE_LEGACY_GROUP, NULL);
}

/* Loads the state file. Never fails: missing / unreadable => empty
 * GKeyFile. */
static GKeyFile *
load_state_file (void)
{
  GKeyFile *kf = g_key_file_new ();
  g_autofree gchar *path = sieve_config_path ();
  GError *error = NULL;

  if (!g_key_file_load_from_file (kf, path, G_KEY_FILE_NONE, &error)) {
    if (!g_error_matches (error, G_FILE_ERROR, G_FILE_ERROR_NOENT))
      g_debug ("sieve: unreadable state (%s): %s — starting fresh",
               path, error->message);
    g_clear_error (&error);
  }

  migrate_legacy_connection (kf);
  return kf;
}

static gboolean
save_state_file (GKeyFile *kf,
                 GError  **error)
{
  g_autofree gchar *path = sieve_config_path ();
  g_autofree gchar *dir = g_path_get_dirname (path);

  if (g_mkdir_with_parents (dir, 0700) != 0) {
    g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (errno),
                 "cannot create %s: %s", dir, g_strerror (errno));
    return FALSE;
  }

  return g_key_file_save_to_file (kf, path, error);
}

void
sieve_config_free (SieveConfig *config)
{
  if (config == NULL)
    return;

  g_free (config->account_uid);
  g_free (config->host);
  g_free (config->user);
  g_free (config);
}

/* Reads a string key, folding "" back to NULL (an empty key makes no
 * sense for a host/user and complicates testing on the caller's
 * side). */
static gchar *
dup_string_key (GKeyFile *kf, const gchar *group, const gchar *key)
{
  gchar *value = g_key_file_get_string (kf, group, key, NULL);

  if (value != NULL && *value == '\0')
    g_clear_pointer (&value, g_free);
  return value;
}

SieveConfig *
sieve_config_load_for_account (const gchar *account_uid)
{
  SieveConfig *config = g_new0 (SieveConfig, 1);
  g_autoptr (GKeyFile) kf = load_state_file ();
  g_autofree gchar *group = group_for_account (account_uid);

  /* Default: auto-connect armed (so the very first successful
   * connection becomes the default behavior). `remember_password` is
   * now always true (the keyring is the default mode, there's no more
   * "Remember" checkbox; removal goes through the "Forget" button) —
   * the key is kept for state-file compatibility. */
  config->account_uid = (account_uid != NULL && *account_uid != '\0')
                          ? g_strdup (account_uid) : NULL;
  config->auto_connect = TRUE;
  config->remember_password = TRUE;

  if (!g_key_file_has_group (kf, group))
    return config;

  config->host = dup_string_key (kf, group, "host");
  config->user = dup_string_key (kf, group, "user");

  {
    gint port = g_key_file_get_integer (kf, group, "port", NULL);
    config->port = (port > 0 && port <= G_MAXUINT16) ? (guint16) port : 0;
  }

  if (g_key_file_has_key (kf, group, "implicit-tls", NULL))
    config->implicit_tls =
      g_key_file_get_boolean (kf, group, "implicit-tls", NULL);

  if (g_key_file_has_key (kf, group, "auto-connect", NULL))
    config->auto_connect =
      g_key_file_get_boolean (kf, group, "auto-connect", NULL);

  if (g_key_file_has_key (kf, group, "remember-password", NULL))
    config->remember_password =
      g_key_file_get_boolean (kf, group, "remember-password", NULL);

  return config;
}

gboolean
sieve_config_save_for_account (const SieveConfig *config,
                               GError           **error)
{
  g_autoptr (GKeyFile) kf = NULL;
  g_autofree gchar *group = NULL;

  g_return_val_if_fail (config != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  kf = load_state_file ();   /* read-modify-write: preserves the other
                              * profiles and [state] */
  group = group_for_account (config->account_uid);

  g_key_file_set_string (kf, group, "host",
                         config->host != NULL ? config->host : "");
  g_key_file_set_integer (kf, group, "port", (gint) config->port);
  g_key_file_set_string (kf, group, "user",
                         config->user != NULL ? config->user : "");
  g_key_file_set_boolean (kf, group, "implicit-tls", config->implicit_tls);
  g_key_file_set_boolean (kf, group, "auto-connect", config->auto_connect);
  g_key_file_set_boolean (kf, group, "remember-password",
                          config->remember_password);

  return save_state_file (kf, error);
}

gchar *
sieve_config_dup_last_account (void)
{
  g_autoptr (GKeyFile) kf = load_state_file ();
  gchar *uid = g_key_file_get_string (kf, SIEVE_STATE_GROUP,
                                      SIEVE_LAST_ACCOUNT_KEY, NULL);

  if (uid != NULL && *uid == '\0')
    g_clear_pointer (&uid, g_free);
  return uid;
}

gboolean
sieve_config_set_last_account (const gchar *account_uid,
                               GError     **error)
{
  g_autoptr (GKeyFile) kf = NULL;

  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  kf = load_state_file ();
  g_key_file_set_string (kf, SIEVE_STATE_GROUP, SIEVE_LAST_ACCOUNT_KEY,
                         account_uid != NULL ? account_uid : "");

  return save_state_file (kf, error);
}
