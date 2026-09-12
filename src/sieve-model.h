/* sieve-model.h
 *
 * Data model for a "visual" Sieve filter editor, in the style of
 * Evolution's local filtering rules (see README.md, item 5 of
 * "What's missing").
 *
 * Scope deliberately kept narrow for a usable first cut:
 *   - an ordered rule set (SieveRuleSet)
 *   - each rule: a name, a match mode (allof / anyof), a list of
 *     conditions (SieveCondition) and a list of actions (SieveAction)
 *   - conditions: From / To / Cc / Subject / generic header / size /
 *     body, with :contains / :is / :matches (and :over / :under for
 *     size)
 *   - actions: keep / discard / fileinto / redirect / addflag / stop
 *
 * This module is pure GLib: no dependency on GTK or Evolution, so it
 * is testable on its own (see tests/test-sieve-model.c).
 *
 * (De)serialization:
 *   - sieve_rule_set_to_script() produces a complete Sieve script, with
 *     the "require" line computed from the extensions actually used,
 *     and a "# rule:[name]" marker before each rule (same convention as
 *     Roundcube's ManageSieve plugin, which leaves the door open to
 *     interoperability).
 *   - sieve_rule_set_parse() reads back a script produced under these
 *     conventions. Any construct the visual editor cannot represent
 *     (not / exists / nested anyof, vacation, unknown actions, blocks
 *     from other tools such as Nextcloud Mail, an `if` without a
 *     marker, hand-written scripts…) is kept as-is in an "opaque" rule
 *     (SieveRule.opaque == TRUE, exact source text in SieveRule.raw):
 *     the visual editor shows it locked, and only the plain-text tab
 *     can modify it. The parser now returns NULL only for a lexically
 *     broken script (unclosed brace or string). An empty script, or one
 *     reduced to comments, yields an empty rule set rather than an
 *     error.
 *   - Round-trip: sieve_rule_set_to_script() copies opaque rules back
 *     byte for byte; the script -> model -> script round trip is stable
 *     (see tests/test-sieve-model.c).
 */

#ifndef SIEVE_MODEL_H
#define SIEVE_MODEL_H

#include <glib.h>

G_BEGIN_DECLS

#define SIEVE_MODEL_ERROR (sieve_model_error_quark ())
GQuark sieve_model_error_quark (void);

typedef enum {
  SIEVE_MODEL_ERROR_UNSUPPORTED, /* construct outside the visual editor's scope */
  SIEVE_MODEL_ERROR_SYNTAX       /* lexically malformed script */
} SieveModelError;

typedef enum {
  SIEVE_FIELD_FROM,
  SIEVE_FIELD_TO,
  SIEVE_FIELD_CC,
  SIEVE_FIELD_SUBJECT,
  SIEVE_FIELD_HEADER,  /* generic header: see SieveCondition.header_name */
  SIEVE_FIELD_SIZE,
  SIEVE_FIELD_BODY
} SieveField;

typedef enum {
  SIEVE_MATCH_CONTAINS,
  SIEVE_MATCH_IS,
  SIEVE_MATCH_MATCHES,
  SIEVE_MATCH_OVER,   /* SIEVE_FIELD_SIZE only */
  SIEVE_MATCH_UNDER   /* SIEVE_FIELD_SIZE only */
} SieveMatch;

typedef struct {
  SieveField field;
  gchar     *header_name; /* relevant if field == SIEVE_FIELD_HEADER */
  SieveMatch match;
  gchar     *value;       /* search pattern, or "1M" / "500K" for size */
} SieveCondition;

typedef enum {
  SIEVE_ACTION_KEEP,
  SIEVE_ACTION_DISCARD,
  SIEVE_ACTION_FILEINTO,  /* arg = folder name */
  SIEVE_ACTION_REDIRECT,  /* arg = email address */
  SIEVE_ACTION_ADDFLAG,   /* arg = IMAP flag, e.g. "\Seen" */
  SIEVE_ACTION_STOP
} SieveActionType;

typedef struct {
  SieveActionType type;
  gchar          *arg;    /* NULL for keep / discard / stop */
} SieveAction;

typedef enum {
  SIEVE_MATCH_MODE_ALL,   /* allof: all conditions */
  SIEVE_MATCH_MODE_ANY    /* anyof: at least one condition */
} SieveMatchMode;

typedef struct {
  gchar         *name;
  SieveMatchMode mode;
  GPtrArray     *conditions; /* elements: SieveCondition* */
  GPtrArray     *actions;    /* elements: SieveAction*    */
  gboolean       opaque;     /* TRUE: rule that cannot be represented, kept
                                verbatim. In that case mode/conditions/actions
                                are ignored (the arrays stay allocated but
                                empty) and only `raw` is authoritative.
                                `name` is still filled in (best effort) for
                                display purposes. */
  gchar         *raw;        /* exact source text if opaque, NULL otherwise */
} SieveRule;

typedef struct {
  GPtrArray *rules;          /* elements: SieveRule* */
  GPtrArray *extra_requires; /* extension names (gchar*) from the original
                                "require" line, preserved when opaque rules
                                are present; NULL otherwise. */
} SieveRuleSet;

SieveCondition *sieve_condition_new  (void);
void            sieve_condition_free (SieveCondition *cond);

SieveAction    *sieve_action_new  (SieveActionType type);
void            sieve_action_free (SieveAction *action);

SieveRule      *sieve_rule_new  (const gchar *name);

/* "Opaque" rule: `raw` is the exact Sieve text to re-emit as-is.
 * `name` is used only for display (the editor's rule list). */
SieveRule      *sieve_rule_new_opaque (const gchar *name, const gchar *raw);

void            sieve_rule_free (SieveRule *rule);

SieveRuleSet   *sieve_rule_set_new  (void);
void            sieve_rule_set_free (SieveRuleSet *set);

/* Serializes `set` into a complete Sieve script (string to be freed). */
gchar          *sieve_rule_set_to_script (const SieveRuleSet *set);

/* Rebuilds a model from a script following the conventions of
 * sieve_rule_set_to_script(). Returns NULL + `error` if the script falls
 * outside the visual editor's scope. */
SieveRuleSet   *sieve_rule_set_parse (const gchar *script, GError **error);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (SieveRuleSet, sieve_rule_set_free)

G_END_DECLS

#endif /* SIEVE_MODEL_H */
