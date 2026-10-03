/* sieve-config.h
 *
 * Small preferences store for the Sieve plugin, kept in a GKeyFile
 * under $XDG_CONFIG_HOME/evolution-sieve-filters/state.ini.
 *
 * Since the split between "account settings" (a page in
 * Edit -> Accounts, see sieve-config-page) and "script editing" (the
 * Edit -> Sieve Filters dialog), connection settings are stored PER
 * ACCOUNT: one profile per ESource UID, plus a "manual" profile
 * (account_uid == NULL) for free-form entry in the dialog, plus a
 * "last account chosen" pointer read back when the dialog opens.
 *
 * Thin layer on top of GKeyFile, fully synchronous, no dependency on
 * GTK or Evolution — so it is testable on its own (see
 * tests/test-sieve-config). The password is NOT stored here: it
 * remains the keyring's responsibility (sieve-secret).
 */

#ifndef SIEVE_CONFIG_H
#define SIEVE_CONFIG_H

#include <glib.h>

G_BEGIN_DECLS

typedef struct {
  gchar   *account_uid;   /* ESource UID; NULL / "" = "manual" profile */
  gboolean saved;          /* TRUE if this profile exists in state.ini
                            * (written at least once by the account
                            * editor's page); set by
                            * sieve_config_load_for_account(), ignored
                            * by sieve_config_save_for_account(). Tells
                            * "never configured" apart from "configured
                            * with an empty host/user", which means
                            * "same as the receiving (IMAP) server" --
                            * see sieve_config_get_effective_host(). */
  gchar   *host;           /* stored ManageSieve host; NULL = the
                            * receiving server's (see `saved`) */
  guint16  port;           /* stored ManageSieve port (0 = not set) */
  gchar   *user;           /* stored login identifier; NULL = the
                            * receiving server's */
  gboolean implicit_tls;   /* TRUE = implicit TLS; FALSE = StartTLS */
  gboolean auto_connect;   /* connect automatically on open */
  gchar   *auth_mechanism; /* forced SASL mechanism (UPPERCASE name);
                            * NULL = automatic negotiation */
  gchar   *gssapi_hostname; /* GSSAPI service principal hostname override
                             * (NULL = use `host`); advanced field for a
                             * ManageSieve host that's a DNS alias (CNAME)
                             * not covered by the server's Kerberos keytab
                             * (issue #2) */
  gboolean gssapi_canonicalize_hostname; /* TRUE: resolve the connection
                             * host's DNS canonical name automatically for
                             * GSSAPI instead of requiring gssapi_hostname
                             * to be typed by hand; ignored if
                             * gssapi_hostname is set. Mirrors Evolution/
                             * Camel's own (unconditional) behavior for
                             * IMAP -- see sieve-sasl.h. TRUE by default
                             * (sieve_config_load_for_account(), like
                             * auto_connect / remember_password): matches
                             * what Evolution's own IMAP connector already
                             * does out of the box, so ManageSieve behaves
                             * the same way unless the "Kerberos hostname:"
                             * field is filled in (which always wins). */
  gboolean gssapi_fallback; /* forced GSSAPI only: fall back to the next
                             * working mechanism if it fails (e.g. expired
                             * ticket) — see SieveManageSieveAuth. TRUE by
                             * default (sieve_config_load_for_account()). */
  gchar    folder_separator; /* the account's REAL IMAP hierarchy
                              * separator (often '.' for Dovecot/
                              * Maildir++, sometimes '/'); 0 = not set,
                              * defaults to '/' (Camel's own canonical
                              * separator, a no-op translation) --
                              * see sieve_rule_set_translate_folder_separator()
                              * in sieve-model.h (issue #3) */
  gboolean remember_password; /* vestige: always true now (the keyring
                               * is the default mode; removal goes
                               * through the "Forget" button). Key kept
                               * for file compatibility. */
} SieveConfig;

/* Frees a SieveConfig (NULL tolerated). */
void sieve_config_free (SieveConfig *config);

/* config->folder_separator if set, '/' otherwise (the default: no
 * translation). Convenience so callers don't each repeat the same
 * "0 => '/'" fallback. */
static inline gchar
sieve_config_get_effective_folder_separator (const SieveConfig *config)
{
  return (config != NULL && config->folder_separator != '\0')
           ? config->folder_separator : '/';
}

/* config->host if set, `fallback` otherwise (the account's receiving
 * server host: an empty "Server" field means "same as IMAP"). NULL if
 * neither is set. Callers that must tell "never configured" apart
 * check config->saved first. */
static inline const gchar *
sieve_config_get_effective_host (const SieveConfig *config,
                                 const gchar       *fallback)
{
  if (config != NULL && config->host != NULL && *config->host != '\0')
    return config->host;
  return (fallback != NULL && *fallback != '\0') ? fallback : NULL;
}

/* Same for the login identifier. */
static inline const gchar *
sieve_config_get_effective_user (const SieveConfig *config,
                                 const gchar       *fallback)
{
  if (config != NULL && config->user != NULL && *config->user != '\0')
    return config->user;
  return (fallback != NULL && *fallback != '\0') ? fallback : NULL;
}

/* Loads the connection profile for the given account (account_uid
 * NULL / "" => "manual" profile). Never returns NULL: a missing
 * profile => default values (saved = FALSE, auto_connect = TRUE,
 * remember_password = TRUE, everything else empty/0), with
 * config->account_uid copied
 * from the argument ("" normalized to NULL). */
SieveConfig *sieve_config_load_for_account (const gchar *account_uid);

/* Writes the config->account_uid profile (NULL / "" => "manual"
 * profile). Read-modify-write: other profiles are preserved.
 * FALSE + *error on write failure. */
gboolean sieve_config_save_for_account (const SieveConfig *config,
                                        GError           **error);

/* UID of the last account selected in the dialog, or NULL if none /
 * "manual entry". Free with g_free(). */
gchar *sieve_config_dup_last_account (void);

/* Remembers the last selected account (account_uid NULL / "" =>
 * none). FALSE + *error on write failure. */
gboolean sieve_config_set_last_account (const gchar *account_uid,
                                        GError     **error);

G_END_DECLS

#endif /* SIEVE_CONFIG_H */
