/* sieve-config-page.h
 *
 * "Sieve Filters" page added to Evolution's account editor
 * (Edit -> Accounts -> an account). It carries ONLY the account's
 * ManageSieve connection settings (host, port, login, TLS mode) — rule
 * editing stays in the Edit -> Sieve Filters dialog.
 *
 * Technically: an EMailConfigPage (implemented on a GtkScrolledWindow,
 * like Evolution's native pages — the interface requires GtkBin) plus
 * an EExtension on E_TYPE_MAIL_CONFIG_NOTEBOOK that inserts it for
 * accounts whose receiving server is of type IMAP. Values are stored
 * by sieve-config (the "account <UID>" profile), read back by the
 * dialog.
 *
 * DEPENDS on Evolution (evolution-mail + libedataserver): built only
 * with the module.
 */

#ifndef SIEVE_CONFIG_PAGE_H
#define SIEVE_CONFIG_PAGE_H

#include <glib-object.h>

G_BEGIN_DECLS

/* Registers the dynamic types (page + notebook extension).
 * Call from e_module_load(). */
void sieve_config_page_types_register (GTypeModule *type_module);

G_END_DECLS

#endif /* SIEVE_CONFIG_PAGE_H */
