/* sieve-model.c — see sieve-model.h for the overview. */

#include "sieve-model.h"
#include "sieve-ast.h"

#include <stdarg.h>
#include <string.h>

G_DEFINE_QUARK (sieve-model-error-quark, sieve_model_error)

/* ---- Constructors / destructors ---------------------------------------- */

SieveCondition *
sieve_condition_new (void)
{
  SieveCondition *c = g_new0 (SieveCondition, 1);
  c->field = SIEVE_FIELD_FROM;
  c->match = SIEVE_MATCH_CONTAINS;
  c->values = g_ptr_array_new_with_free_func (g_free);
  g_ptr_array_add (c->values, g_strdup (""));
  return c;
}

void
sieve_condition_free (SieveCondition *cond)
{
  if (cond == NULL)
    return;
  g_free (cond->header_name);
  g_clear_pointer (&cond->values, g_ptr_array_unref);
  g_free (cond);
}

void
sieve_condition_set_value (SieveCondition *cond, const gchar *value)
{
  g_ptr_array_set_size (cond->values, 0);
  g_ptr_array_add (cond->values, g_strdup (value != NULL ? value : ""));
}

const gchar *
sieve_condition_get_value (const SieveCondition *cond)
{
  if (cond->values == NULL || cond->values->len == 0)
    return "";
  return g_ptr_array_index (cond->values, 0);
}

SieveAction *
sieve_action_new (SieveActionType type)
{
  SieveAction *a = g_new0 (SieveAction, 1);
  a->type = type;
  return a;
}

void
sieve_action_free (SieveAction *action)
{
  if (action == NULL)
    return;
  g_free (action->arg);
  g_free (action);
}

SieveRule *
sieve_rule_new (const gchar *name)
{
  SieveRule *r = g_new0 (SieveRule, 1);
  r->name = g_strdup (name != NULL ? name : "");
  r->mode = SIEVE_MATCH_MODE_ALL;
  r->enabled = TRUE;
  r->conditions = g_ptr_array_new_with_free_func ((GDestroyNotify) sieve_condition_free);
  r->actions = g_ptr_array_new_with_free_func ((GDestroyNotify) sieve_action_free);
  return r;
}

SieveRule *
sieve_rule_new_opaque (const gchar *name, const gchar *raw)
{
  SieveRule *r = sieve_rule_new (name);
  r->opaque = TRUE;
  r->raw = g_strdup (raw != NULL ? raw : "");
  return r;
}

void
sieve_rule_free (SieveRule *rule)
{
  if (rule == NULL)
    return;
  g_free (rule->name);
  g_free (rule->raw);
  g_ptr_array_unref (rule->conditions);
  g_ptr_array_unref (rule->actions);
  g_free (rule);
}

SieveRuleSet *
sieve_rule_set_new (void)
{
  SieveRuleSet *s = g_new0 (SieveRuleSet, 1);
  s->rules = g_ptr_array_new_with_free_func ((GDestroyNotify) sieve_rule_free);
  return s;
}

void
sieve_rule_set_free (SieveRuleSet *set)
{
  if (set == NULL)
    return;
  g_ptr_array_unref (set->rules);
  g_clear_pointer (&set->extra_requires, g_ptr_array_unref);
  g_free (set);
}

/* ---- Serialization ------------------------------------------------------ */

static const gchar *
field_header_name (SieveField field, const gchar *header_name)
{
  switch (field) {
    case SIEVE_FIELD_FROM:    return "from";
    case SIEVE_FIELD_TO:      return "to";
    case SIEVE_FIELD_CC:      return "cc";
    case SIEVE_FIELD_SUBJECT: return "subject";
    case SIEVE_FIELD_HEADER:
      return (header_name != NULL && *header_name != '\0') ? header_name : "x-header";
    default:                  return "from";
  }
}

static const gchar *
match_tag (SieveMatch match)
{
  switch (match) {
    case SIEVE_MATCH_IS:      return ":is";
    case SIEVE_MATCH_MATCHES: return ":matches";
    case SIEVE_MATCH_REGEX:   return ":regex";
    default:                  return ":contains";
  }
}

/* Appends `s` as a Sieve quoted string (RFC 5228 §2.4.2), escaping
 * "\" and "\"". */
static void
append_quoted (GString *out, const gchar *s)
{
  g_string_append_c (out, '"');
  for (const gchar *p = s != NULL ? s : ""; *p != '\0'; p++) {
    if (*p == '"' || *p == '\\')
      g_string_append_c (out, '\\');
    g_string_append_c (out, *p);
  }
  g_string_append_c (out, '"');
}

/* A rule name ends up in "# rule:[...]": strip out characters that
 * would break the marker or the line. */
static gchar *
sanitize_rule_name (const gchar *name)
{
  GString *out = g_string_new (NULL);
  for (const gchar *p = name != NULL ? name : ""; *p != '\0'; p++) {
    if (*p == '[' || *p == ']' || *p == '\r' || *p == '\n')
      g_string_append_c (out, ' ');
    else
      g_string_append_c (out, *p);
  }
  return g_string_free (out, FALSE);
}

/* Appends a single value as a bare quoted string, or several as a
 * Sieve string list ["a", "b", ...] ("matches any of these values"). */
static void
append_values (GString *out, const GPtrArray *values)
{
  if (values->len <= 1) {
    append_quoted (out, (values->len == 1) ? g_ptr_array_index (values, 0) : "");
    return;
  }

  g_string_append_c (out, '[');
  for (guint i = 0; i < values->len; i++) {
    if (i > 0)
      g_string_append (out, ", ");
    append_quoted (out, g_ptr_array_index (values, i));
  }
  g_string_append_c (out, ']');
}

static void
append_condition (GString *out, const SieveCondition *c)
{
  if (c->field == SIEVE_FIELD_SIZE) {
    const gchar *v = sieve_condition_get_value (c);
    g_string_append_printf (out, "size %s %s",
                            c->match == SIEVE_MATCH_UNDER ? ":under" : ":over",
                            (*v != '\0') ? v : "1M");
    return;
  }

  if (c->field == SIEVE_FIELD_BODY) {
    g_string_append_printf (out, "body :text %s ", match_tag (c->match));
    append_values (out, c->values);
    return;
  }

  g_string_append_printf (out, "header %s ", match_tag (c->match));
  append_quoted (out, field_header_name (c->field, c->header_name));
  g_string_append_c (out, ' ');
  append_values (out, c->values);
}

static void
append_action (GString *out, const SieveAction *a)
{
  switch (a->type) {
    case SIEVE_ACTION_KEEP:
      g_string_append (out, "\tkeep;\n");
      break;
    case SIEVE_ACTION_DISCARD:
      g_string_append (out, "\tdiscard;\n");
      break;
    case SIEVE_ACTION_STOP:
      g_string_append (out, "\tstop;\n");
      break;
    case SIEVE_ACTION_FILEINTO:
      g_string_append (out, "\tfileinto ");
      append_quoted (out, (a->arg != NULL && *a->arg != '\0') ? a->arg : "INBOX");
      g_string_append (out, ";\n");
      break;
    case SIEVE_ACTION_REDIRECT:
      g_string_append (out, "\tredirect ");
      append_quoted (out, a->arg != NULL ? a->arg : "");
      g_string_append (out, ";\n");
      break;
    case SIEVE_ACTION_ADDFLAG:
      g_string_append (out, "\taddflag ");
      append_quoted (out, (a->arg != NULL && *a->arg != '\0') ? a->arg : "\\Seen");
      g_string_append (out, ";\n");
      break;
    default:
      break;
  }
}

/* Extensions whose need collect_requires() can decide on its own from the
 * structured rules: recomputed on every serialization (so they disappear
 * when the last rule using them goes away), unlike any other extension
 * from the original "require" line, which is always kept — see
 * sieve_rule_set_to_script(). */
static const gchar *const managed_requires[] = {
  "body", "encoded-character", "fileinto", "imap4flags", "regex",
  "variables", NULL
};

static gboolean
is_managed_require (const gchar *ext)
{
  return g_strv_contains (managed_requires, ext);
}

/* Heuristic for strings typed in the visual editor: "${hex:..}" and
 * "${unicode:..}" are RFC 5228 §2.4.2.4 encoded characters
 * ("encoded-character"), any other "${name}" (identifier, match number
 * or namespaced "a.b") is an RFC 5229 variable reference ("variables"). */
static void
collect_string_requires (const gchar *s, GHashTable *req)
{
  if (s == NULL)
    return;

  for (const gchar *p = strstr (s, "${"); p != NULL; p = strstr (p + 2, "${")) {
    const gchar *q = p + 2;

    if (g_ascii_strncasecmp (q, "hex:", 4) == 0 ||
        g_ascii_strncasecmp (q, "unicode:", 8) == 0) {
      g_hash_table_add (req, g_strdup ("encoded-character"));
      continue;
    }
    if (!g_ascii_isalnum (*q) && *q != '_')
      continue;
    while (g_ascii_isalnum (*q) || *q == '_' || *q == '.')
      q++;
    if (*q == '}')
      g_hash_table_add (req, g_strdup ("variables"));
  }
}

static void
collect_requires (const SieveRuleSet *set, GHashTable *req)
{
  for (guint i = 0; i < set->rules->len; i++) {
    const SieveRule *r = g_ptr_array_index (set->rules, i);

    if (r->opaque)
      continue; /* extensions used by opaque text stay within its own body */

    for (guint j = 0; j < r->conditions->len; j++) {
      const SieveCondition *c = g_ptr_array_index (r->conditions, j);
      if (c->field == SIEVE_FIELD_BODY)
        g_hash_table_add (req, g_strdup ("body"));
      if (c->match == SIEVE_MATCH_REGEX)
        g_hash_table_add (req, g_strdup ("regex"));
      if (c->field == SIEVE_FIELD_HEADER)
        collect_string_requires (c->header_name, req);
      for (guint k = 0; k < c->values->len; k++)
        collect_string_requires (g_ptr_array_index (c->values, k), req);
    }
    for (guint j = 0; j < r->actions->len; j++) {
      const SieveAction *a = g_ptr_array_index (r->actions, j);
      if (a->type == SIEVE_ACTION_FILEINTO)
        g_hash_table_add (req, g_strdup ("fileinto"));
      else if (a->type == SIEVE_ACTION_ADDFLAG)
        g_hash_table_add (req, g_strdup ("imap4flags"));
      collect_string_requires (a->arg, req);
    }
  }
}

gchar *
sieve_rule_set_to_script (const SieveRuleSet *set)
{
  GString *out = g_string_new (NULL);
  GHashTable *req = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

  g_return_val_if_fail (set != NULL, g_string_free (out, FALSE));

  collect_requires (set, req);
  if (set->extra_requires != NULL) {
    gboolean has_opaque = FALSE;

    for (guint i = 0; i < set->rules->len; i++)
      if (((SieveRule *) g_ptr_array_index (set->rules, i))->opaque) {
        has_opaque = TRUE;
        break;
      }
    /* Opaque text isn't scanned: while some remains, the whole original
     * line is kept. Otherwise only the extensions collect_requires()
     * can't decide on are (envelope, copy, …: unknown to the model, but
     * possibly still relied upon — the server accepted them before). */
    for (guint i = 0; i < set->extra_requires->len; i++) {
      const gchar *ext = g_ptr_array_index (set->extra_requires, i);
      if (has_opaque || !is_managed_require (ext))
        g_hash_table_add (req, g_strdup (ext));
    }
  }
  if (g_hash_table_size (req) > 0) {
    GList *keys = g_hash_table_get_keys (req);
    keys = g_list_sort (keys, (GCompareFunc) g_strcmp0);
    g_string_append (out, "require [");
    for (GList *l = keys; l != NULL; l = l->next) {
      if (l != keys)
        g_string_append (out, ", ");
      append_quoted (out, l->data);
    }
    g_string_append (out, "];\n\n");
    g_list_free (keys);
  }
  g_hash_table_unref (req);

  for (guint i = 0; i < set->rules->len; i++) {
    const SieveRule *r = g_ptr_array_index (set->rules, i);
    gchar *name;

    if (i > 0)
      g_string_append_c (out, '\n');

    if (r->opaque) {
      /* Text from another tool (or hand-written): copied back byte for
       * byte. `raw` is normalized at parse time (ends with a single "\n"). */
      g_string_append (out, (r->raw != NULL) ? r->raw : "");
      continue;
    }

    name = sanitize_rule_name (r->name);
    g_string_append_printf (out, "# rule:[%s]\n", name);
    g_free (name);

    if (!r->enabled) {
      /* Disabled: the test is kept as a trailing comment (same
       * convention as Roundcube's managesieve plugin) so re-enabling
       * the rule doesn't lose it. */
      g_string_append (out, "if false");
      if (r->conditions->len > 0) {
        g_string_append_printf (out, " # %s (",
                                r->mode == SIEVE_MATCH_MODE_ANY ? "anyof" : "allof");
        for (guint j = 0; j < r->conditions->len; j++) {
          if (j > 0)
            g_string_append (out, ", ");
          append_condition (out, g_ptr_array_index (r->conditions, j));
        }
        g_string_append_c (out, ')');
      }
      g_string_append (out, "\n{\n");
    } else if (r->conditions->len == 0) {
      g_string_append (out, "if true\n{\n");
    } else {
      g_string_append_printf (out, "if %s (",
                              r->mode == SIEVE_MATCH_MODE_ANY ? "anyof" : "allof");
      for (guint j = 0; j < r->conditions->len; j++) {
        if (j > 0)
          g_string_append (out, ", ");
        append_condition (out, g_ptr_array_index (r->conditions, j));
      }
      g_string_append (out, ")\n{\n");
    }

    for (guint j = 0; j < r->actions->len; j++)
      append_action (out, g_ptr_array_index (r->actions, j));

    g_string_append (out, "}\n");
  }

  return g_string_free (out, FALSE);
}

/* ---- Syntactic analysis --------------------------------------------------
 *
 * The script is first parsed into a generic syntax tree (sieve-ast.h,
 * the full RFC 5228 grammar), then each top-level unit is mapped onto
 * the model when it has one of the shapes the visual editor can
 * represent. Every mapping failure below is SIEVE_MODEL_ERROR_UNSUPPORTED:
 * the caller falls back to an opaque rule (or, for sieve_rule_unlock(),
 * leaves the rule locked).
 */

G_GNUC_PRINTF (2, 3)
static void
unsupported (GError **error, const gchar *fmt, ...)
{
  va_list ap;

  va_start (ap, fmt);
  g_propagate_error (error, g_error_new_valist (SIEVE_MODEL_ERROR,
                                                SIEVE_MODEL_ERROR_UNSUPPORTED,
                                                fmt, ap));
  va_end (ap);
}

static const SieveAstArg *
arg_at (const GPtrArray *args, guint i)
{
  return g_ptr_array_index (args, i);
}

/* A single string: a bare string, or a one-entry list (semantically
 * identical). A list with more than one entry means "any of these
 * values", which only a test's value argument can represent (see
 * arg_string_list()): anywhere else it's unsupported rather than
 * silently reduced to its first entry. */
static const gchar *
arg_single_string (const SieveAstArg *arg, GError **error)
{
  if (arg->kind == SIEVE_AST_ARG_STRING)
    return arg->str;
  if (arg->kind == SIEVE_AST_ARG_STRING_LIST && arg->list->len == 1)
    return g_ptr_array_index (arg->list, 0);
  if (arg->kind == SIEVE_AST_ARG_STRING_LIST)
    unsupported (error, "a value list with more than one entry is not supported by the visual editor");
  else
    unsupported (error, "expected a string");
  return NULL;
}

/* A test's value argument, where the visual editor can represent a list
 * of any length: every entry is kept, in order. Newly allocated array of
 * at least one gchar*, or NULL + `error`. */
static GPtrArray *
arg_string_list (const SieveAstArg *arg, GError **error)
{
  GPtrArray *out;

  if (arg->kind == SIEVE_AST_ARG_STRING) {
    out = g_ptr_array_new_with_free_func (g_free);
    g_ptr_array_add (out, g_strdup (arg->str));
    return out;
  }
  if (arg->kind == SIEVE_AST_ARG_STRING_LIST) {
    out = g_ptr_array_new_full (arg->list->len, g_free);
    for (guint i = 0; i < arg->list->len; i++)
      g_ptr_array_add (out, g_strdup (g_ptr_array_index (arg->list, i)));
    return out;
  }
  unsupported (error, "expected a string or a string list");
  return NULL;
}

static gboolean
match_from_tag (const gchar *tag, SieveMatch *out)
{
  if (g_ascii_strcasecmp (tag, "contains") == 0) { *out = SIEVE_MATCH_CONTAINS; return TRUE; }
  if (g_ascii_strcasecmp (tag, "is") == 0)       { *out = SIEVE_MATCH_IS;       return TRUE; }
  if (g_ascii_strcasecmp (tag, "matches") == 0)  { *out = SIEVE_MATCH_MATCHES;  return TRUE; }
  if (g_ascii_strcasecmp (tag, "regex") == 0)    { *out = SIEVE_MATCH_REGEX;    return TRUE; }
  return FALSE;
}

static void
set_field_from_header (SieveCondition *c, const gchar *hdr)
{
  if (g_ascii_strcasecmp (hdr, "from") == 0)
    c->field = SIEVE_FIELD_FROM;
  else if (g_ascii_strcasecmp (hdr, "to") == 0)
    c->field = SIEVE_FIELD_TO;
  else if (g_ascii_strcasecmp (hdr, "cc") == 0)
    c->field = SIEVE_FIELD_CC;
  else if (g_ascii_strcasecmp (hdr, "subject") == 0)
    c->field = SIEVE_FIELD_SUBJECT;
  else {
    c->field = SIEVE_FIELD_HEADER;
    g_free (c->header_name);
    c->header_name = g_strdup (hdr);
  }
}

static gboolean
is_bare_test (const SieveAstTest *t, const gchar *name)
{
  return g_strcmp0 (t->name, name) == 0 && t->args->len == 0 && t->tests == NULL;
}

/* Maps a single test onto a condition. "true"/"false" are handled by
 * the callers (see conditions_from_expression()). */
static SieveCondition *
condition_from_test (const SieveAstTest *t, GError **error)
{
  const GPtrArray *args = t->args;
  SieveCondition *c = NULL;
  GPtrArray *vals;
  guint i;

  if (t->tests != NULL)
    goto unknown_test;

  /* Only "header": "address" / "envelope" compare a parsed address (or
   * the SMTP envelope), not the raw header — mapping them onto a
   * SieveCondition would change the test once re-serialized as
   * "header". */
  if (g_strcmp0 (t->name, "header") == 0) {
    const gchar *hdr;

    c = sieve_condition_new ();
    for (i = 0; i < args->len && arg_at (args, i)->kind == SIEVE_AST_ARG_TAG; i++) {
      if (!match_from_tag (arg_at (args, i)->str, &c->match)) {
        unsupported (error, "tag \":%s\" not supported by the visual editor",
                     arg_at (args, i)->str);
        goto fail;
      }
    }
    if (args->len - i != 2) {
      unsupported (error, "expected a header name and a value");
      goto fail;
    }
    hdr = arg_single_string (arg_at (args, i), error);
    if (hdr == NULL)
      goto fail;
    vals = arg_string_list (arg_at (args, i + 1), error);
    if (vals == NULL)
      goto fail;
    set_field_from_header (c, hdr);
    g_ptr_array_unref (c->values);
    c->values = vals;
    return c;
  }

  if (g_strcmp0 (t->name, "size") == 0) {
    c = sieve_condition_new ();
    c->field = SIEVE_FIELD_SIZE;
    c->match = SIEVE_MATCH_OVER;
    i = 0;
    if (i < args->len && arg_at (args, i)->kind == SIEVE_AST_ARG_TAG) {
      const gchar *tag = arg_at (args, i)->str;

      if (g_strcmp0 (tag, "under") == 0) {
        c->match = SIEVE_MATCH_UNDER;
      } else if (g_strcmp0 (tag, "over") != 0) {
        unsupported (error, "tag \":%s\" not supported by the visual editor", tag);
        goto fail;
      }
      i++;
    }
    if (args->len - i != 1 || arg_at (args, i)->kind != SIEVE_AST_ARG_NUMBER) {
      unsupported (error, "expected a size after \"size\" (e.g. 1M)");
      goto fail;
    }
    sieve_condition_set_value (c, arg_at (args, i)->str);
    return c;
  }

  if (g_strcmp0 (t->name, "body") == 0) {
    c = sieve_condition_new ();
    c->field = SIEVE_FIELD_BODY;
    /* :text (no-op, always re-emitted) or :contains / :is / :matches /
     * :regex. ":raw" changes semantics and isn't representable. */
    for (i = 0; i < args->len && arg_at (args, i)->kind == SIEVE_AST_ARG_TAG; i++) {
      const gchar *tag = arg_at (args, i)->str;

      if (g_strcmp0 (tag, "text") != 0 && !match_from_tag (tag, &c->match)) {
        unsupported (error, "tag \":%s\" not supported by the visual editor", tag);
        goto fail;
      }
    }
    if (args->len - i != 1) {
      unsupported (error, "expected a value after \"body\"");
      goto fail;
    }
    vals = arg_string_list (arg_at (args, i), error);
    if (vals == NULL)
      goto fail;
    g_ptr_array_unref (c->values);
    c->values = vals;
    return c;
  }

unknown_test:
  unsupported (error, "test \"%s\" not supported by the visual editor", t->name);
fail:
  g_clear_pointer (&c, sieve_condition_free);
  return NULL;
}

static gboolean
add_condition (const SieveAstTest *t, SieveRule *rule, GError **error)
{
  SieveCondition *c = condition_from_test (t, error);

  if (c == NULL)
    return FALSE;
  g_ptr_array_add (rule->conditions, c);
  return TRUE;
}

/* "allof (test, ...)" / "anyof (test, ...)" / "true" / a single
 * unwrapped test: sets rule->mode and adds to rule->conditions.
 * A whole "true" adds nothing, since an empty condition list is
 * serialized back as "if true" (see sieve_rule_set_to_script()); inside
 * a list it's unsupported, since dropping it would turn "anyof (true,
 * X)" (always true) into "X". "false" has no such
 * round trip: an empty condition list can only mean "true", so silently
 * accepting "false" here would flip the rule's logic on save. It's
 * therefore left unsupported (falls back to an opaque, verbatim-kept
 * rule) — except as a disabled rule's marker, see rule_from_if(). */
static gboolean
conditions_from_expression (const SieveAstTest *t, SieveRule *rule, GError **error)
{
  if (g_strcmp0 (t->name, "allof") == 0 || g_strcmp0 (t->name, "anyof") == 0) {
    if (t->args->len != 0 || !t->test_list) {
      unsupported (error, "expected \"(\" after allof/anyof");
      return FALSE;
    }
    rule->mode = (g_strcmp0 (t->name, "anyof") == 0)
                   ? SIEVE_MATCH_MODE_ANY : SIEVE_MATCH_MODE_ALL;
    for (guint i = 0; i < t->tests->len; i++)
      if (!add_condition (g_ptr_array_index (t->tests, i), rule, error))
        return FALSE;
    return TRUE;
  }

  rule->mode = SIEVE_MATCH_MODE_ALL;
  if (is_bare_test (t, "true"))
    return TRUE;
  return add_condition (t, rule, error);
}

static gboolean
action_from_command (const SieveAstCommand *cmd, SieveRule *rule, GError **error)
{
  const GPtrArray *args = cmd->args;
  SieveAction *a;
  const gchar *arg;

  if (cmd->tests != NULL || cmd->block != NULL) {
    unsupported (error, "expected \";\" after the action");
    return FALSE;
  }

  if (g_strcmp0 (cmd->name, "keep") == 0 || g_strcmp0 (cmd->name, "discard") == 0 ||
      g_strcmp0 (cmd->name, "stop") == 0) {
    if (args->len != 0) {
      unsupported (error, "expected \";\" after the action");
      return FALSE;
    }
    a = sieve_action_new (g_strcmp0 (cmd->name, "keep") == 0 ? SIEVE_ACTION_KEEP
                        : g_strcmp0 (cmd->name, "discard") == 0 ? SIEVE_ACTION_DISCARD
                        : SIEVE_ACTION_STOP);
    g_ptr_array_add (rule->actions, a);
    return TRUE;
  }

  /* Exactly one string argument, as serialized by append_action().
   * Anything more can't be represented and is left unsupported rather
   * than dropped: tags (":copy" keeps the implicit keep, ":flags" sets
   * the delivered message's flags), and imap4flags' leading variable
   * name ("addflag \"var\" \"\\Seen\"" acts on a variable, not on the
   * message). "setflag" isn't mapped onto "addflag" either: it replaces
   * the flags instead of adding to them. */
  if (g_strcmp0 (cmd->name, "fileinto") == 0 || g_strcmp0 (cmd->name, "redirect") == 0 ||
      g_strcmp0 (cmd->name, "addflag") == 0) {
    if (args->len > 0 && arg_at (args, 0)->kind == SIEVE_AST_ARG_TAG) {
      unsupported (error, "tag \":%s\" not supported by the visual editor",
                   arg_at (args, 0)->str);
      return FALSE;
    }
    if (args->len != 1) {
      unsupported (error, "expected a single string after \"%s\"", cmd->name);
      return FALSE;
    }
    arg = arg_single_string (arg_at (args, 0), error);
    if (arg == NULL)
      return FALSE;
    a = sieve_action_new (g_strcmp0 (cmd->name, "fileinto") == 0 ? SIEVE_ACTION_FILEINTO
                        : g_strcmp0 (cmd->name, "redirect") == 0 ? SIEVE_ACTION_REDIRECT
                        : SIEVE_ACTION_ADDFLAG);
    a->arg = g_strdup (arg);
    g_ptr_array_add (rule->actions, a);
    return TRUE;
  }

  unsupported (error, "action \"%s\" not supported by the visual editor", cmd->name);
  return FALSE;
}

/* If, starting at `p`, only horizontal whitespace precedes a "#" before
 * the next newline (or end of string), returns a newly-allocated,
 * stripped copy of the comment's text; otherwise NULL. Used to recover
 * the original test of a disabled rule ("if false # <test>", see
 * rule_from_if()) — the same convention Roundcube's managesieve plugin
 * uses to disable a rule without deleting it. */
static gchar *
extract_same_line_comment (const gchar *p)
{
  const gchar *q = p;
  const gchar *start, *end;

  while (*q == ' ' || *q == '\t')
    q++;
  if (*q != '#')
    return NULL;
  start = q + 1;
  end = start;
  while (*end != '\0' && *end != '\n')
    end++;
  return g_strstrip (g_strndup (start, end - start));
}

/* Conditions of a disabled rule, from its "if false # <test>" comment:
 * parsed as the test of a throwaway "if <test> {}". */
static gboolean
conditions_from_comment (const gchar *comment, SieveRule *rule)
{
  g_autofree gchar *wrapped = g_strconcat ("if ", comment, "\n{}", NULL);
  g_autoptr (SieveAst) ast = sieve_ast_parse (wrapped, NULL);
  const SieveAstCommand *cmd;

  if (ast == NULL || ast->commands->len != 1)
    return FALSE;
  cmd = g_ptr_array_index (ast->commands, 0);
  if (g_strcmp0 (cmd->name, "if") != 0 || cmd->args->len != 0 ||
      cmd->tests == NULL || cmd->test_list || cmd->block == NULL ||
      cmd->block->len != 0)
    return FALSE;
  return conditions_from_expression (g_ptr_array_index (cmd->tests, 0), rule, NULL);
}

/* Maps "if <test> { <actions> }" onto `rule`. */
static gboolean
rule_from_if (const SieveAst *ast, const SieveAstCommand *cmd, SieveRule *rule,
              GError **error)
{
  const SieveAstTest *test;

  if (g_strcmp0 (cmd->name, "if") != 0) {
    unsupported (error, "expected \"if\" after the rule marker");
    return FALSE;
  }
  if (cmd->args->len != 0 || cmd->tests == NULL || cmd->test_list ||
      cmd->block == NULL) {
    unsupported (error, "expected a test and an action block after \"if\"");
    return FALSE;
  }
  test = g_ptr_array_index (cmd->tests, 0);

  if (is_bare_test (test, "false")) {
    g_autofree gchar *comment = extract_same_line_comment (ast->source + test->end);

    rule->enabled = FALSE;
    rule->mode = SIEVE_MATCH_MODE_ALL;
    /* Not a recognizable test: don't silently drop it (it would be lost
     * for good on the next save). Fall back to opaque instead, keeping
     * the exact original text. */
    if (comment != NULL && !conditions_from_comment (comment, rule)) {
      unsupported (error, "disabled rule's trailing comment is not a recognizable test");
      return FALSE;
    }
  } else {
    rule->enabled = TRUE;
    if (!conditions_from_expression (test, rule, error))
      return FALSE;
  }

  for (guint i = 0; i < cmd->block->len; i++)
    if (!action_from_command (g_ptr_array_index (cmd->block, i), rule, error))
      return FALSE;
  return TRUE;
}

/* ---- Tolerant segmentation ("opaque" rules) -----------------------------
 *
 * The top level of a Sieve script is a sequence of units: a
 * "# rule:[name]" followed by an `if ... { ... }` (structured, editable
 * rule), or anything else — a block from another tool (Nextcloud
 * Mail...), an `if` without a marker or with elsif/else branches, a
 * foreign simple command, a command the parser couldn't make sense of —
 * which is kept as-is in an opaque rule. Only a lexically broken script
 * (unclosed brace, string or comment) still makes the parser fail.
 */

/* TRUE if the [from, to) range contains only whitespace. */
static gboolean
trivia_is_blank (const gchar *from, const gchar *to)
{
  for (const gchar *p = from; p < to; p++)
    if (*p != ' ' && *p != '\t' && *p != '\r' && *p != '\n')
      return FALSE;
  return TRUE;
}

/* If the "#" comment at `hash` is a "# rule:[name]" marker, returns its
 * name (newly allocated), otherwise NULL. */
static gchar *
rule_marker_name (const gchar *hash, const gchar *limit)
{
  const gchar *eol = hash;
  g_autofree gchar *line = NULL;
  gchar *t, *rb;

  while (eol < limit && *eol != '\0' && *eol != '\n')
    eol++;
  line = g_strndup (hash + 1, eol - (hash + 1));
  t = g_strstrip (line);
  if (!g_str_has_prefix (t, "rule:["))
    return NULL;
  rb = strrchr (t, ']');
  if (rb == NULL || rb < t + 6)
    return NULL;
  return g_strndup (t + 6, rb - (t + 6));
}

/* Counts the "# rule:[name]" markers among the comments in [from, to)
 * (the whitespace/comments preceding a command); `*name` receives the
 * last one's name. */
static guint
find_rule_markers (const gchar *from, const gchar *to, gchar **name)
{
  const gchar *p = from;
  guint count = 0;

  while (p < to) {
    if (*p == '#') {
      gchar *found = rule_marker_name (p, to);

      if (found != NULL) {
        count++;
        g_free (*name);
        *name = found;
      }
      while (p < to && *p != '\n')
        p++;
    } else if (p[0] == '/' && p + 1 < to && p[1] == '*') {
      const gchar *e = g_strstr_len (p + 2, to - (p + 2), "*/");
      p = (e != NULL) ? e + 2 : to;
    } else {
      p++;
    }
  }
  return count;
}

/* After the end of an opaque unit (`end`), absorbs the rest of its last
 * line if that's only a comment, then comment lines that follow
 * IMMEDIATELY (with no blank line in between) and are not a
 * "# rule:[...]" marker: typically a closing banner (Nextcloud Mail
 * re-prints "### Nextcloud Mail: Filters ### DON'T EDIT ###" after the
 * block). Code sharing the unit's last line is never absorbed. */
static gsize
absorb_trailing_comments (const gchar *src, gsize end)
{
  const gchar *p = src + end;

  while (*p == ' ' || *p == '\t' || *p == '\r')
    p++;
  if (*p == '#') {
    g_autofree gchar *marker = rule_marker_name (p, p + strlen (p));

    if (marker != NULL)
      return end;
    while (*p != '\0' && *p != '\n')
      p++;
  }
  if (*p == '\0')
    return p - src;
  if (*p != '\n')
    return end;
  p++;

  for (;;) {
    const gchar *q = p;

    while (*q == ' ' || *q == '\t')
      q++;
    if (*q != '#')
      break;
    {
      g_autofree gchar *marker = rule_marker_name (q, q + strlen (q));

      if (marker != NULL)
        break;
    }
    while (*q != '\0' && *q != '\n')
      q++;
    if (*q == '\n')
      q++;
    p = q;
  }
  return p - src;
}

/* Copies [p, p+len), strips leading and trailing whitespace, guarantees
 * a single trailing "\n": canonical form of an opaque rule, stable
 * across round trips. */
static gchar *
normalize_opaque_raw (const gchar *p, gsize len)
{
  gchar *s = g_strndup (p, len);
  gchar *out;

  g_strstrip (s); /* in place */
  out = g_strconcat (s, "\n", NULL);
  g_free (s);
  return out;
}

/* Display name of an opaque rule: "# rule:[X]" if present, otherwise the
 * first usable "# text" line (skipping "##..." and "# FILTER:...",
 * Nextcloud's JSON noise), otherwise "(imported rule)". */
static gchar *
opaque_rule_name (const gchar *raw)
{
  const gchar *p = raw;
  gchar *fallback = NULL;

  while (*p != '\0') {
    const gchar *eol = p;
    gchar *line, *t;

    while (*eol != '\0' && *eol != '\n')
      eol++;
    line = g_strndup (p, eol - p);
    t = g_strstrip (line);

    if (*t == '#') {
      const gchar *c = t + 1;

      while (*c == ' ' || *c == '\t')
        c++;
      if (g_str_has_prefix (c, "rule:[")) {
        const gchar *rb = strrchr (c, ']');
        if (rb != NULL && rb > c + 6) {
          gchar *name = g_strndup (c + 6, rb - (c + 6));
          g_free (line);
          g_free (fallback);
          return g_strstrip (name);
        }
      }
      if (fallback == NULL && *c != '\0' && *c != '#' &&
          !g_str_has_prefix (c, "FILTER:"))
        fallback = g_strdup (c);
    } else if (*t != '\0') {
      g_free (line);
      break; /* first line of code: no more header to expect */
    }
    g_free (line);
    p = (*eol == '\n') ? eol + 1 : eol;
  }

  return (fallback != NULL) ? fallback : g_strdup ("(imported rule)");
}

static gboolean
is_if_branch (const SieveAstCommand *cmd)
{
  return g_strcmp0 (cmd->name, "elsif") == 0 || g_strcmp0 (cmd->name, "else") == 0;
}

SieveRuleSet *
sieve_rule_set_parse (const gchar *script, GError **error)
{
  g_autoptr (SieveAst) ast = sieve_ast_parse (script, error);
  SieveRuleSet *set;
  const gchar *src;
  gsize consumed = 0; /* source before this offset already belongs to a rule */
  guint i = 0;

  if (ast == NULL)
    return NULL;
  src = ast->source;
  set = sieve_rule_set_new ();

  /* Leading "require", only if it isn't preceded by any comment
   * (otherwise it belongs to a foreign block). Its extensions are kept
   * in `extra_requires`; sieve_rule_set_to_script() decides which of
   * them are re-emitted. */
  if (ast->commands->len > 0) {
    const SieveAstCommand *first = g_ptr_array_index (ast->commands, 0);

    if (g_strcmp0 (first->name, "require") == 0 &&
        trivia_is_blank (src + first->trivia_start, src + first->start)) {
      GPtrArray *req = g_ptr_array_new_with_free_func (g_free);

      for (guint j = 0; j < first->args->len; j++) {
        const SieveAstArg *arg = arg_at (first->args, j);

        if (arg->kind == SIEVE_AST_ARG_STRING)
          g_ptr_array_add (req, g_strdup (arg->str));
        else if (arg->kind == SIEVE_AST_ARG_STRING_LIST)
          for (guint k = 0; k < arg->list->len; k++)
            g_ptr_array_add (req, g_strdup (g_ptr_array_index (arg->list, k)));
      }
      if (req->len > 0)
        set->extra_requires = req;
      else
        g_ptr_array_unref (req);
      consumed = first->end;
      i = 1;
    }
  }

  while (i < ast->commands->len) {
    const SieveAstCommand *cmd = g_ptr_array_index (ast->commands, i);
    gsize seg_start = MAX (cmd->trivia_start, consumed);
    g_autofree gchar *marker = NULL;
    guint n_markers = find_rule_markers (src + seg_start, src + cmd->start, &marker);
    guint last = i;
    gsize end;
    gchar *raw, *name;

    /* An if's elsif/else branches belong to the same unit. */
    while (last + 1 < ast->commands->len &&
           is_if_branch (g_ptr_array_index (ast->commands, last + 1)))
      last++;

    if (n_markers == 1 && last == i) {
      SieveRule *rule = sieve_rule_new (marker);

      if (rule_from_if (ast, cmd, rule, NULL)) {
        g_ptr_array_add (set->rules, rule);
        consumed = cmd->end;
        i++;
        continue;
      }
      sieve_rule_free (rule);
    }

    /* Unrepresentable unit: captured verbatim as an opaque rule. */
    end = absorb_trailing_comments (
      src, ((const SieveAstCommand *) g_ptr_array_index (ast->commands, last))->end);
    raw = normalize_opaque_raw (src + seg_start, end - seg_start);
    name = opaque_rule_name (raw);
    g_ptr_array_add (set->rules, sieve_rule_new_opaque (name, raw));
    g_free (raw);
    g_free (name);
    consumed = end;
    i = last + 1;
  }

  return set;
}

/* Explicit, per-rule "unlock" of an opaque rule: applies the same
 * "if allof/anyof(...) { actions }" mapping sieve_rule_set_parse() uses
 * for a marked rule, but without requiring a leading "# rule:[name]"
 * marker — unlike the tolerant whole-script parse, this is triggered
 * on demand for one specific rule (visual editor's "Unlock" button),
 * typically after the user has fixed up the rule's text in the "Raw
 * text" tab, so relaxing the marker requirement here doesn't risk
 * silently reinterpreting foreign blocks (Nextcloud Mail, Roundcube)
 * elsewhere in the script.
 *
 * On success, returns a newly allocated, non-opaque SieveRule (the
 * caller replaces `rule` with it) named after `rule->name` (itself
 * already "# rule:[name]" or a best-effort fallback, see
 * opaque_rule_name()). On failure, returns NULL + `error` and `rule`
 * is left untouched: still opaque, exact original text preserved. */
SieveRule *
sieve_rule_unlock (const SieveRule *rule, GError **error)
{
  g_autoptr (SieveAst) ast = NULL;
  const SieveAstCommand *cmd;
  SieveRule *result;

  g_return_val_if_fail (rule != NULL && rule->opaque, NULL);

  ast = sieve_ast_parse (rule->raw != NULL ? rule->raw : "", error);
  if (ast == NULL)
    return NULL;

  if (ast->commands->len == 0) {
    unsupported (error, "expected \"if\"");
    return NULL;
  }
  cmd = g_ptr_array_index (ast->commands, 0);
  if (cmd->name == NULL) {
    unsupported (error, "%s", cmd->error);
    return NULL;
  }
  if (ast->commands->len > 1) {
    unsupported (error, "more than a single \"if\" block");
    return NULL;
  }

  result = sieve_rule_new (rule->name);
  if (!rule_from_if (ast, cmd, result, error)) {
    sieve_rule_free (result);
    return NULL;
  }
  return result;
}

void
sieve_rule_set_translate_folder_separator (SieveRuleSet *set,
                                           gchar         real_separator,
                                           gboolean      to_real)
{
  g_return_if_fail (set != NULL);

  if (real_separator == '/' || real_separator == '\0')
    return;

  for (guint i = 0; i < set->rules->len; i++) {
    SieveRule *rule = g_ptr_array_index (set->rules, i);

    if (rule->opaque)
      continue;

    for (guint j = 0; j < rule->actions->len; j++) {
      SieveAction *action = g_ptr_array_index (rule->actions, j);

      if (action->type != SIEVE_ACTION_FILEINTO || action->arg == NULL)
        continue;

      if (to_real) {
        g_strdelimit (action->arg, "/", real_separator);
      } else {
        gchar from[2] = { real_separator, '\0' };
        g_strdelimit (action->arg, from, '/');
      }
    }
  }
}
