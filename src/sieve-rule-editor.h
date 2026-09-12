/* sieve-rule-editor.h
 *
 * GTK "visual editor" widget for Sieve filters, in the style of
 * Evolution's local filter rules: a list of rules on the left, and for
 * the selected rule a criteria / actions builder on the right.
 *
 * Relies on SieveRuleSet (sieve-model.h) as the single source of
 * truth: every modification writes directly into the internal model
 * then emits the "changed" signal. The dialog (sieve-editor-dialog.c)
 * bridges to the "raw text" tab via
 * sieve_rule_set_to_script() / sieve_rule_set_parse().
 *
 * Depends on GTK, not on Evolution.
 */

#ifndef SIEVE_RULE_EDITOR_H
#define SIEVE_RULE_EDITOR_H

#include <gtk/gtk.h>

#include "sieve-model.h"

G_BEGIN_DECLS

#define SIEVE_TYPE_RULE_EDITOR (sieve_rule_editor_get_type ())
G_DECLARE_FINAL_TYPE (SieveRuleEditor, sieve_rule_editor, SIEVE, RULE_EDITOR, GtkBox)

GtkWidget *sieve_rule_editor_new (void);

/* Replaces the editor's contents with a copy of `set`
 * (NULL -> empty editor). Does not emit "changed". */
void sieve_rule_editor_set_rule_set (SieveRuleEditor    *self,
                                     const SieveRuleSet *set);

/* Rebuilds a SieveRuleSet from the current state
 * (ownership transferred to the caller). */
SieveRuleSet *sieve_rule_editor_dup_rule_set (SieveRuleEditor *self);

/* Selects the rule at index `index` (bounded; < 0 or out of range ->
 * no selection): updates the list, rebuilds the detail panel, scrolls
 * the row into view and gives it keyboard focus. Used by the dialog to
 * highlight a freshly added rule (e.g. seeded from a message). Does not
 * emit "changed". */
void sieve_rule_editor_select_rule (SieveRuleEditor *self, gint index);

/* Sets the account's mailbox list: full paths, "/" separated
 * (e.g. "INBOX/Lists"). The "fileinto" action then offers an editable
 * drop-down list of these folders instead of a plain text field; free
 * text entry remains possible (folder not yet created, offline...).
 * `mailbox_paths`: NULL-terminated array, copied; NULL or empty ->
 * falls back to a free text field. Rebuilds the detail panel; does not
 * emit "changed". */
void sieve_rule_editor_set_mailboxes (SieveRuleEditor     *self,
                                      const gchar * const *mailbox_paths);

/* Enables or disables the toolbar's "Reload" button (refresh icon,
 * next to "+" / "-"). A click emits the "refresh-requested" signal; it
 * is up to the container (the dialog) to reload the rules from the
 * server. The button is insensitive by default (no session open). */
void sieve_rule_editor_set_refresh_sensitive (SieveRuleEditor *self,
                                              gboolean         sensitive);

/* Returns the rule list's toolbar ("+" / "-" / "reload" buttons). The
 * editor creates it and manages the sensitivity of its buttons, but
 * does not place it itself: the container (the dialog) inserts it
 * wherever it sees fit, typically in a common action bar alongside its
 * own "Save" button. Ownership stays with the editor -- do not destroy
 * it; valid as long as the editor is alive. */
GtkWidget *sieve_rule_editor_get_toolbar (SieveRuleEditor *self);

G_END_DECLS

#endif /* SIEVE_RULE_EDITOR_H */
