/* sieve-folder-separator.h -- what an account's IMAP hierarchy
 * separator may be (issue #3).
 *
 * Header-only and pure GLib, shared by sieve-model (fileinto path
 * translation), sieve-config (state.ini), sieve-imap-probe (server
 * answer) and sieve-config-page (user input).
 *
 * The separator is stored and substituted as ONE BYTE everywhere
 * (`gchar`), so it must be ONE ASCII character: a non-ASCII one ("é")
 * would be cut to its first UTF-8 byte (0xC3), and that lone byte,
 * written into a fileinto path, makes the whole generated script
 * invalid UTF-8 -- the editor's text buffer then refuses it and comes
 * up empty, and "Save" would overwrite the server's script with it.
 * It must also be printable (no control character, no space) and
 * neither '"' nor '\\', which would need escaping inside the Sieve
 * quoted-string the path is written into. A value from a user or from
 * a (possibly hostile) IMAP server that fails this check is rejected,
 * never truncated. */

#ifndef SIEVE_FOLDER_SEPARATOR_H
#define SIEVE_FOLDER_SEPARATOR_H

#include <glib.h>

G_BEGIN_DECLS

static inline gboolean
sieve_folder_separator_is_valid (gchar c)
{
  return g_ascii_isgraph (c) && c != '"' && c != '\\';
}

/* `text` must hold exactly one valid separator character. */
static inline gboolean
sieve_folder_separator_text_is_valid (const gchar *text)
{
  return text != NULL && text[0] != '\0' && text[1] == '\0'
         && sieve_folder_separator_is_valid (text[0]);
}

G_END_DECLS

#endif /* SIEVE_FOLDER_SEPARATOR_H */
