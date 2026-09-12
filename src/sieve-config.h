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
  gchar   *host;           /* stored ManageSieve host */
  guint16  port;           /* stored ManageSieve port (0 = not set) */
  gchar   *user;           /* stored login identifier */
  gboolean implicit_tls;   /* TRUE = implicit TLS; FALSE = StartTLS */
  gboolean auto_connect;   /* connect automatically on open */
  gboolean remember_password; /* vestige: always true now (the keyring
                               * is the default mode; removal goes
                               * through the "Forget" button). Key kept
                               * for file compatibility. */
} SieveConfig;

/* Frees a SieveConfig (NULL tolerated). */
void sieve_config_free (SieveConfig *config);

/* Loads the connection profile for the given account (account_uid
 * NULL / "" => "manual" profile). Never returns NULL: a missing
 * profile => default values (auto_connect = TRUE, remember_password =
 * TRUE, everything else empty/0), with config->account_uid copied
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
