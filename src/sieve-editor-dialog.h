/* sieve-editor-dialog.h
 *
 * GTK dialog dedicated to editing Sieve scripts: choosing an Evolution
 * mail account, connecting to the ManageSieve server with the profile
 * already configured for that account (the "Sieve Filters" page of the
 * account editor, see sieve-config-page), then editing the active
 * script visually (conditions/actions) or as raw text.
 *
 * It carries NO connection settings: server, port, TLS mode,
 * username, password, auto-connect and SRV discovery are all configured
 * per-account in the account editor. The password is fetched from the
 * keyring (libsecret), never entered here.
 */
#ifndef SIEVE_EDITOR_DIALOG_H
#define SIEVE_EDITOR_DIALOG_H

#include <gtk/gtk.h>
#include <shell/e-shell.h>

#include "sieve-model.h"   /* SieveRule */

G_BEGIN_DECLS

/* Builds and shows (non-modal) the dialog. Nothing is connected over the
 * network yet until the user clicks "Connect" (or, if the account's
 * profile arms it, on open). `shell` provides the source registry
 * (enumeration of mail accounts + the account's OAuth2 token / password
 * via EDS) and the mail session (the account's folder tree, offered in
 * the "fileinto" action); it may be NULL (the account menu is then
 * empty). */
GtkWidget *sieve_editor_dialog_new (GtkWindow *parent,
                                    EShell    *shell);

/* "Context menu" variant (right-click on a message -> Create -> Create a
 * Sieve Filter…). Like sieve_editor_dialog_new, but:
 *   - `prefer_account_uid` (an ESource UID, or NULL): this account is
 *     pre-selected in the drop-down menu and, if its ManageSieve profile
 *     is configured (Edit -> Accounts -> "Sieve Filters" tab), the
 *     connection is started right away -- without waiting for the
 *     profile's "auto-connect" checkbox.
 *   - `seed_rule` (ownership transferred, may be NULL): added as a new
 *     rule at the end of the script once it has loaded, then shown in
 *     the visual editor so the user can fill in its action. If no
 *     connection is possible (unknown account, no profile…), the rule
 *     is still shown. */
GtkWidget *sieve_editor_dialog_new_with_seed (GtkWindow   *parent,
                                              EShell      *shell,
                                              const gchar *prefer_account_uid,
                                              SieveRule   *seed_rule);

G_END_DECLS

#endif /* SIEVE_EDITOR_DIALOG_H */
