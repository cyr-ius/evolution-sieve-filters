/* sieve-model.c — see sieve-model.h for the overview. */

#include "sieve-model.h"

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
    }
    for (guint j = 0; j < r->actions->len; j++) {
      const SieveAction *a = g_ptr_array_index (r->actions, j);
      if (a->type == SIEVE_ACTION_FILEINTO)
        g_hash_table_add (req, g_strdup ("fileinto"));
      else if (a->type == SIEVE_ACTION_ADDFLAG)
        g_hash_table_add (req, g_strdup ("imap4flags"));
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
    for (guint i = 0; i < set->extra_requires->len; i++)
      g_hash_table_add (req, g_strdup (g_ptr_array_index (set->extra_requires, i)));
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

/* ---- Lexical analysis ----------------------------------------------------
 *
 * Minimal tokenizer: just enough to read back what
 * sieve_rule_set_to_script() produces, plus a few common variants
 * (a single unwrapped test, string lists, extra tags ignored). Nothing
 * more: anything outside this scope raises an error and the caller
 * stays in plain-text editing.
 */

typedef enum {
  TK_EOF,
  TK_LPAREN, TK_RPAREN, TK_LBRACE, TK_RBRACE, TK_LBRACKET, TK_RBRACKET,
  TK_COMMA, TK_SEMI,
  TK_STRING, TK_IDENT, TK_TAG,
  TK_RULE    /* "# rule:[name]" marker; val == name */
} TokKind;

typedef struct {
  const gchar *cur;
  const gchar *trivia_start; /* start of whitespace/comments preceding the token */
  const gchar *tok_start;    /* first character of the current token */
  TokKind      kind;
  gchar       *val;  /* STRING / IDENT / TAG / RULE */
} Lex;

static void
lex_clear (Lex *lx)
{
  g_clear_pointer (&lx->val, g_free);
}

static void
lex_syntax_error (GError **error, const gchar *msg)
{
  g_set_error_literal (error, SIEVE_MODEL_ERROR, SIEVE_MODEL_ERROR_SYNTAX, msg);
}

static gboolean
lex_advance (Lex *lx, GError **error)
{
  const gchar *p = lx->cur;

  lex_clear (lx);
  lx->trivia_start = p;

  for (;;) {
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
      p++;

    if (*p == '#') {
      const gchar *line_start = p;
      const gchar *eol = p;
      gchar *line, *trimmed;

      while (*eol != '\0' && *eol != '\n')
        eol++;
      line = g_strndup (p + 1, eol - (p + 1));
      trimmed = g_strstrip (line);

      if (g_str_has_prefix (trimmed, "rule:[")) {
        gchar *rb = strrchr (trimmed, ']');
        if (rb != NULL && rb >= trimmed + 6) {
          lx->kind = TK_RULE;
          lx->val = g_strndup (trimmed + 6, rb - (trimmed + 6));
          lx->tok_start = line_start;
          lx->cur = (*eol == '\n') ? eol + 1 : eol;
          g_free (line);
          return TRUE;
        }
      }
      g_free (line);
      p = (*eol == '\n') ? eol + 1 : eol;
      continue;
    }

    if (p[0] == '/' && p[1] == '*') {
      const gchar *e = strstr (p + 2, "*/");
      if (e == NULL) {
        lex_syntax_error (error, "unterminated \"/* ... */\" comment");
        return FALSE;
      }
      p = e + 2;
      continue;
    }
    break;
  }

  lx->cur = p;
  lx->tok_start = p;

  if (*p == '\0') {
    lx->kind = TK_EOF;
    return TRUE;
  }

  switch (*p) {
    case '(': lx->kind = TK_LPAREN;   lx->cur = p + 1; return TRUE;
    case ')': lx->kind = TK_RPAREN;   lx->cur = p + 1; return TRUE;
    case '{': lx->kind = TK_LBRACE;   lx->cur = p + 1; return TRUE;
    case '}': lx->kind = TK_RBRACE;   lx->cur = p + 1; return TRUE;
    case '[': lx->kind = TK_LBRACKET; lx->cur = p + 1; return TRUE;
    case ']': lx->kind = TK_RBRACKET; lx->cur = p + 1; return TRUE;
    case ',': lx->kind = TK_COMMA;    lx->cur = p + 1; return TRUE;
    case ';': lx->kind = TK_SEMI;     lx->cur = p + 1; return TRUE;
    default: break;
  }

  if (*p == '"') {
    GString *s = g_string_new (NULL);
    p++;
    while (*p != '\0' && *p != '"') {
      if (*p == '\\' && p[1] != '\0') {
        p++;
        g_string_append_c (s, *p++);
      } else {
        g_string_append_c (s, *p++);
      }
    }
    if (*p != '"') {
      g_string_free (s, TRUE);
      lex_syntax_error (error, "unterminated quoted string");
      return FALSE;
    }
    p++;
    lx->kind = TK_STRING;
    lx->val = g_string_free (s, FALSE);
    lx->cur = p;
    return TRUE;
  }

  if (*p == ':') {
    const gchar *st = ++p;
    while (g_ascii_isalnum (*p) || *p == '_')
      p++;
    if (p == st) {
      lex_syntax_error (error, "empty tag after \":\"");
      return FALSE;
    }
    lx->kind = TK_TAG;
    lx->val = g_strndup (st, p - st);
    lx->cur = p;
    return TRUE;
  }

  if (g_ascii_isalnum (*p) || *p == '_') {
    const gchar *st = p;
    while (g_ascii_isalnum (*p) || *p == '_' || *p == '.')
      p++;
    lx->kind = TK_IDENT;
    lx->val = g_strndup (st, p - st);
    lx->cur = p;
    return TRUE;
  }

  {
    gchar *msg = g_strdup_printf ("unexpected character \"%c\"", *p);
    lex_syntax_error (error, msg);
    g_free (msg);
  }
  return FALSE;
}

/* ---- Syntactic analysis --------------------------------------------------- */

static void
unsupported (GError **error, const gchar *msg)
{
  g_set_error_literal (error, SIEVE_MODEL_ERROR, SIEVE_MODEL_ERROR_UNSUPPORTED, msg);
}

/* Reads a string, or a singleton list "[\"a\"]" (semantically identical
 * to a bare string). A list with more than one entry means "matches any
 * of these values", which the model has no way to represent with a
 * single `value` field: the caller must treat that as unsupported
 * (falling back to an opaque rule) rather than silently keep only the
 * first entry and drop the rest. */
static gboolean
expect_string (Lex *lx, gchar **out, GError **error)
{
  *out = NULL;

  if (lx->kind == TK_LBRACKET) {
    guint count = 0;

    if (!lex_advance (lx, error))
      return FALSE;
    if (lx->kind != TK_STRING) {
      unsupported (error, "expected a string in the list");
      return FALSE;
    }
    *out = g_strdup (lx->val);
    count = 1;
    if (!lex_advance (lx, error))
      goto fail;
    while (lx->kind == TK_COMMA) {
      if (!lex_advance (lx, error))
        goto fail;
      if (lx->kind != TK_STRING) {
        unsupported (error, "expected a string in the list");
        goto fail;
      }
      count++;
      if (!lex_advance (lx, error))
        goto fail;
    }
    if (lx->kind != TK_RBRACKET) {
      unsupported (error, "expected \"]\" to close the list");
      goto fail;
    }
    if (count > 1) {
      unsupported (error, "a value list with more than one entry is not supported by the visual editor");
      goto fail;
    }
    return lex_advance (lx, error);
  }

  if (lx->kind != TK_STRING) {
    unsupported (error, "expected a string");
    return FALSE;
  }
  *out = g_strdup (lx->val);
  return lex_advance (lx, error);

fail:
  g_clear_pointer (out, g_free);
  return FALSE;
}

/* Like expect_string(), but for a test's *value* argument, where the
 * visual editor can represent a list of any length ("matches any of
 * these values"): every entry is kept, in order. Returns a newly
 * allocated GPtrArray of at least one gchar* on success, NULL + `error`
 * otherwise. */
static GPtrArray *
expect_string_list (Lex *lx, GError **error)
{
  GPtrArray *out = g_ptr_array_new_with_free_func (g_free);

  if (lx->kind == TK_LBRACKET) {
    if (!lex_advance (lx, error))
      goto fail;
    if (lx->kind != TK_STRING) {
      unsupported (error, "expected a string in the list");
      goto fail;
    }
    g_ptr_array_add (out, g_strdup (lx->val));
    if (!lex_advance (lx, error))
      goto fail;
    while (lx->kind == TK_COMMA) {
      if (!lex_advance (lx, error))
        goto fail;
      if (lx->kind != TK_STRING) {
        unsupported (error, "expected a string in the list");
        goto fail;
      }
      g_ptr_array_add (out, g_strdup (lx->val));
      if (!lex_advance (lx, error))
        goto fail;
    }
    if (lx->kind != TK_RBRACKET) {
      unsupported (error, "expected \"]\" to close the list");
      goto fail;
    }
    if (!lex_advance (lx, error))
      goto fail;
    return out;
  }

  if (lx->kind != TK_STRING) {
    unsupported (error, "expected a string");
    goto fail;
  }
  g_ptr_array_add (out, g_strdup (lx->val));
  if (!lex_advance (lx, error))
    goto fail;
  return out;

fail:
  g_ptr_array_unref (out);
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

/* Parses a single test and adds the corresponding condition to `rule`.
 * "true" is accepted without adding anything, since an empty condition
 * list is serialized back as "if true" (see sieve_rule_set_to_script()).
 * "false" has no such round-trip: an empty condition list can only mean
 * "true", so silently accepting "false" here would flip the rule's
 * logic on save. It's therefore left unsupported (falls back to an
 * opaque, verbatim-kept rule). */
static gboolean
parse_test (Lex *lx, SieveRule *rule, GError **error)
{
  gchar *name;
  gboolean ok = FALSE;
  SieveCondition *c = NULL;

  if (lx->kind != TK_IDENT) {
    unsupported (error, "expected a test name");
    return FALSE;
  }
  name = g_ascii_strdown (lx->val, -1);

  if (g_strcmp0 (name, "true") == 0) {
    ok = lex_advance (lx, error);
    goto out;
  }

  if (g_strcmp0 (name, "false") == 0) {
    unsupported (error, "test \"false\" is not supported by the visual editor");
    goto out;
  }

  if (g_strcmp0 (name, "header") == 0 || g_strcmp0 (name, "address") == 0 ||
      g_strcmp0 (name, "envelope") == 0) {
    gchar *hdr = NULL;
    GPtrArray *vals;

    c = sieve_condition_new ();
    if (!lex_advance (lx, error))
      goto out;
    while (lx->kind == TK_TAG) {          /* :contains / :is / :matches / :regex */
      SieveMatch m;
      if (!match_from_tag (lx->val, &m)) {
        gchar *msg = g_strdup_printf ("tag \":%s\" not supported by the visual editor", lx->val);
        unsupported (error, msg);
        g_free (msg);
        goto out;
      }
      c->match = m;
      if (!lex_advance (lx, error))
        goto out;
    }
    if (!expect_string (lx, &hdr, error))
      goto out;
    vals = expect_string_list (lx, error);
    if (vals == NULL) {
      g_free (hdr);
      goto out;
    }
    set_field_from_header (c, hdr);
    g_free (hdr);
    g_ptr_array_unref (c->values);
    c->values = vals;
    g_ptr_array_add (rule->conditions, g_steal_pointer (&c));
    ok = TRUE;
    goto out;
  }

  if (g_strcmp0 (name, "size") == 0) {
    c = sieve_condition_new ();
    c->field = SIEVE_FIELD_SIZE;
    c->match = SIEVE_MATCH_OVER;
    if (!lex_advance (lx, error))
      goto out;
    if (lx->kind == TK_TAG) {
      c->match = (g_ascii_strcasecmp (lx->val, "under") == 0)
                   ? SIEVE_MATCH_UNDER : SIEVE_MATCH_OVER;
      if (!lex_advance (lx, error))
        goto out;
    }
    if (lx->kind != TK_IDENT) {
      unsupported (error, "expected a size after \"size\" (e.g. 1M)");
      goto out;
    }
    sieve_condition_set_value (c, lx->val);
    if (!lex_advance (lx, error))
      goto out;
    g_ptr_array_add (rule->conditions, g_steal_pointer (&c));
    ok = TRUE;
    goto out;
  }

  if (g_strcmp0 (name, "body") == 0) {
    GPtrArray *vals;

    c = sieve_condition_new ();
    c->field = SIEVE_FIELD_BODY;
    if (!lex_advance (lx, error))
      goto out;
    while (lx->kind == TK_TAG) {          /* :text (no-op, always re-emitted) or
                                            * :contains / :is / :matches / :regex.
                                            * ":raw" changes semantics and isn't
                                            * representable: left as unsupported. */
      SieveMatch m;
      if (g_ascii_strcasecmp (lx->val, "text") == 0) {
        /* no-op */
      } else if (match_from_tag (lx->val, &m)) {
        c->match = m;
      } else {
        gchar *msg = g_strdup_printf ("tag \":%s\" not supported by the visual editor", lx->val);
        unsupported (error, msg);
        g_free (msg);
        goto out;
      }
      if (!lex_advance (lx, error))
        goto out;
    }
    vals = expect_string_list (lx, error);
    if (vals == NULL)
      goto out;
    g_ptr_array_unref (c->values);
    c->values = vals;
    g_ptr_array_add (rule->conditions, g_steal_pointer (&c));
    ok = TRUE;
    goto out;
  }

  {
    gchar *msg = g_strdup_printf ("test \"%s\" not supported by the visual editor", name);
    unsupported (error, msg);
    g_free (msg);
  }

out:
  g_clear_pointer (&c, sieve_condition_free);
  g_free (name);
  return ok;
}

static gboolean
parse_action (Lex *lx, SieveRule *rule, GError **error)
{
  gchar *name;
  gboolean ok = FALSE;
  SieveAction *a = NULL;

  if (lx->kind != TK_IDENT) {
    unsupported (error, "expected an action name");
    return FALSE;
  }
  name = g_ascii_strdown (lx->val, -1);
  if (!lex_advance (lx, error))
    goto out;

  if (g_strcmp0 (name, "keep") == 0) {
    a = sieve_action_new (SIEVE_ACTION_KEEP);
  } else if (g_strcmp0 (name, "discard") == 0) {
    a = sieve_action_new (SIEVE_ACTION_DISCARD);
  } else if (g_strcmp0 (name, "stop") == 0) {
    a = sieve_action_new (SIEVE_ACTION_STOP);
  } else if (g_strcmp0 (name, "fileinto") == 0 || g_strcmp0 (name, "redirect") == 0 ||
             g_strcmp0 (name, "addflag") == 0 || g_strcmp0 (name, "setflag") == 0) {
    SieveActionType t = (g_strcmp0 (name, "fileinto") == 0) ? SIEVE_ACTION_FILEINTO
                      : (g_strcmp0 (name, "redirect") == 0) ? SIEVE_ACTION_REDIRECT
                      : SIEVE_ACTION_ADDFLAG;
    gchar *arg = NULL;

    a = sieve_action_new (t);
    while (lx->kind == TK_TAG) {          /* :copy / :flags ... ignored */
      if (!lex_advance (lx, error))
        goto out;
    }
    if (!expect_string (lx, &arg, error))
      goto out;
    /* imap4flags: "addflag \"var\" \"\\Seen\"" — keep the last string. */
    while (lx->kind == TK_STRING) {
      g_free (arg);
      arg = g_strdup (lx->val);
      if (!lex_advance (lx, error)) {
        g_free (arg);
        goto out;
      }
    }
    a->arg = arg;
  } else {
    gchar *msg = g_strdup_printf ("action \"%s\" not supported by the visual editor", name);
    unsupported (error, msg);
    g_free (msg);
    goto out;
  }

  if (lx->kind != TK_SEMI) {
    unsupported (error, "expected \";\" after the action");
    goto out;
  }
  if (!lex_advance (lx, error))
    goto out;

  g_ptr_array_add (rule->actions, g_steal_pointer (&a));
  ok = TRUE;

out:
  g_clear_pointer (&a, sieve_action_free);
  g_free (name);
  return ok;
}

/* Parses "allof (test, ...)" / "anyof (test, ...)" / "true" / a single
 * unwrapped test, setting rule->mode and adding to rule->conditions.
 * Factored out of parse_if() so it can also be applied to the trailing
 * comment of a disabled rule ("if false # <this>") — see parse_if(). */
static gboolean
parse_test_expression (Lex *lx, SieveRule *rule, GError **error)
{
  if (lx->kind == TK_IDENT &&
      (g_ascii_strcasecmp (lx->val, "allof") == 0 ||
       g_ascii_strcasecmp (lx->val, "anyof") == 0)) {
    rule->mode = (g_ascii_strcasecmp (lx->val, "anyof") == 0)
                   ? SIEVE_MATCH_MODE_ANY : SIEVE_MATCH_MODE_ALL;
    if (!lex_advance (lx, error))
      return FALSE;
    if (lx->kind != TK_LPAREN) {
      unsupported (error, "expected \"(\" after allof/anyof");
      return FALSE;
    }
    if (!lex_advance (lx, error))
      return FALSE;
    for (;;) {
      if (!parse_test (lx, rule, error))
        return FALSE;
      if (lx->kind != TK_COMMA)
        break;
      if (!lex_advance (lx, error))
        return FALSE;
    }
    if (lx->kind != TK_RPAREN) {
      unsupported (error, "expected \")\" to close the test list");
      return FALSE;
    }
    return lex_advance (lx, error);
  }

  if (lx->kind == TK_IDENT && g_ascii_strcasecmp (lx->val, "true") == 0) {
    rule->mode = SIEVE_MATCH_MODE_ALL;
    return lex_advance (lx, error);
  }

  rule->mode = SIEVE_MATCH_MODE_ALL;
  return parse_test (lx, rule, error);
}

/* If, starting at `p`, only horizontal whitespace precedes a "#" before
 * the next newline (or end of string), returns a newly-allocated,
 * stripped copy of the comment's text; otherwise NULL. Used to recover
 * the original test of a disabled rule ("if false # <test>", see
 * parse_if()) — the same convention Roundcube's managesieve plugin uses
 * to disable a rule without deleting it. */
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

static gboolean
parse_if (Lex *lx, SieveRule *rule, GError **error)
{
  if (lx->kind != TK_IDENT || g_ascii_strcasecmp (lx->val, "if") != 0) {
    unsupported (error, "expected \"if\" after the rule marker");
    return FALSE;
  }
  if (!lex_advance (lx, error))
    return FALSE;

  if (lx->kind == TK_IDENT && g_ascii_strcasecmp (lx->val, "false") == 0) {
    g_autofree gchar *comment = extract_same_line_comment (lx->cur);

    rule->enabled = FALSE;
    rule->mode = SIEVE_MATCH_MODE_ALL;
    if (!lex_advance (lx, error)) /* also skips the trailing comment, if any */
      return FALSE;

    if (comment != NULL) {
      Lex inner = { 0 };
      gboolean inner_ok;

      inner.cur = comment;
      inner_ok = lex_advance (&inner, NULL) &&
                 parse_test_expression (&inner, rule, NULL) &&
                 inner.kind == TK_EOF;
      lex_clear (&inner);
      if (!inner_ok) {
        /* Not a recognizable test: don't silently drop it (it would be
         * lost for good on the next save). Fall back to opaque instead,
         * keeping the exact original text. */
        unsupported (error, "disabled rule's trailing comment is not a recognizable test");
        return FALSE;
      }
    }
  } else {
    rule->enabled = TRUE;
    if (!parse_test_expression (lx, rule, error))
      return FALSE;
  }

  if (lx->kind != TK_LBRACE) {
    unsupported (error, "expected \"{\" (action block)");
    return FALSE;
  }
  if (!lex_advance (lx, error))
    return FALSE;
  while (lx->kind != TK_RBRACE) {
    if (lx->kind == TK_EOF) {
      unsupported (error, "unterminated action block");
      return FALSE;
    }
    if (!parse_action (lx, rule, error))
      return FALSE;
  }
  return lex_advance (lx, error); /* consumes "}" */
}

/* ---- Tolerant segmentation ("opaque" rules) -----------------------------
 *
 * The top level of a Sieve script is a sequence of units: a
 * "# rule:[name]" followed by an `if ... { ... }` (structured, editable
 * rule), or anything else — a block from another tool (Nextcloud
 * Mail...), an `if` without a marker, a foreign simple command — which
 * is kept as-is in an opaque rule. Only a lexically broken script
 * (unclosed brace or string/comment) still makes the parser fail.
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

/* Starting from `p` (at the top level), returns the position just past
 * the next unit: the balanced "{ ... }" block, or the ";" of a simple
 * command. Skips quoted strings and comments (both "#" line comments
 * and block comments). `*ok` is set to FALSE if a brace, string or
 * comment is left open. */
static const gchar *
scan_toplevel_unit_end (const gchar *p, gboolean *ok)
{
  gint depth = 0;
  gboolean seen_brace = FALSE;

  *ok = TRUE;
  for (; *p != '\0'; p++) {
    if (*p == '"') {
      for (p++; *p != '\0' && *p != '"'; p++)
        if (*p == '\\' && p[1] != '\0')
          p++;
      if (*p == '\0') { *ok = FALSE; return p; }
      continue; /* p is on the closing '"'; the loop's p++ moves past it */
    }
    if (*p == '#') {
      while (*p != '\0' && *p != '\n')
        p++;
      if (*p == '\0')
        return p; /* comment runs to the end: end of the unit */
      continue;
    }
    if (p[0] == '/' && p[1] == '*') {
      const gchar *e = strstr (p + 2, "*/");
      if (e == NULL) { *ok = FALSE; return p; }
      p = e + 1; /* the loop's p++ moves past the '/' */
      continue;
    }
    if (*p == '{') { depth++; seen_brace = TRUE; continue; }
    if (*p == '}') {
      depth--;
      if (seen_brace && depth <= 0)
        return p + 1;
      continue;
    }
    if (*p == ';' && !seen_brace && depth == 0)
      return p + 1;
  }
  if (seen_brace && depth != 0)
    *ok = FALSE;
  return p;
}

/* After the end of a unit, absorbs comment lines that follow
 * IMMEDIATELY (with no blank line in between) and are not a
 * "# rule:[...]" marker: typically a closing banner (Nextcloud Mail
 * re-prints "### Nextcloud Mail: Filters ### DON'T EDIT ###" after the
 * block). */
static const gchar *
swallow_trailing_comments (const gchar *p)
{
  while (*p != '\0' && *p != '\n')
    p++;
  if (*p == '\n')
    p++;

  for (;;) {
    const gchar *q = p;

    while (*q == ' ' || *q == '\t')
      q++;
    if (*q != '#')
      break;
    {
      const gchar *r = q + 1;
      while (*r == ' ' || *r == '\t')
        r++;
      if (g_str_has_prefix (r, "rule:["))
        break;
    }
    while (*q != '\0' && *q != '\n')
      q++;
    if (*q == '\n')
      q++;
    p = q;
  }
  return p;
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

SieveRuleSet *
sieve_rule_set_parse (const gchar *script, GError **error)
{
  Lex lx = { 0 };
  SieveRuleSet *set = sieve_rule_set_new ();
  GError *local = NULL;
  GPtrArray *lead_req = g_ptr_array_new_with_free_func (g_free);

  lx.cur = script != NULL ? script : "";

  if (!lex_advance (&lx, &local))
    goto fail;

  /* Leading "require", only if it isn't preceded by any comment
   * (otherwise it belongs to a foreign block). Its extensions are kept
   * for serialization when opaque rules remain; otherwise the line is
   * ignored (recomputed). */
  if (lx.kind == TK_IDENT && g_ascii_strcasecmp (lx.val, "require") == 0 &&
      trivia_is_blank (lx.trivia_start, lx.tok_start)) {
    while (lx.kind != TK_SEMI && lx.kind != TK_EOF) {
      if (lx.kind == TK_STRING)
        g_ptr_array_add (lead_req, g_strdup (lx.val));
      if (!lex_advance (&lx, &local))
        goto fail;
    }
    if (lx.kind == TK_SEMI && !lex_advance (&lx, &local))
      goto fail;
  }

  while (lx.kind != TK_EOF) {
    const gchar *seg_start = lx.trivia_start;
    gboolean was_rule = (lx.kind == TK_RULE);
    gboolean structured_ok = FALSE;

    if (was_rule) {
      SieveRule *rule = sieve_rule_new (lx.val);
      GError *try_err = NULL;

      if (!lex_advance (&lx, &local)) {
        sieve_rule_free (rule);
        goto fail;
      }
      if (parse_if (&lx, rule, &try_err)) {
        g_ptr_array_add (set->rules, rule);
        structured_ok = TRUE;
      } else {
        g_clear_error (&try_err);
        sieve_rule_free (rule);
      }
    }

    if (structured_ok)
      continue;

    /* Unrepresentable unit: captured verbatim as an opaque rule. After a
     * `parse_if` failure, the lexer is in some intermediate state — we
     * rely only on `seg_start` (raw position) and `was_rule`. A foreign
     * token at the top level (outside a marker) is only captured if it
     * opens a plausible unit (TK_IDENT). */
    if (was_rule || lx.kind == TK_IDENT) {
      gboolean ok = FALSE;
      const gchar *end = scan_toplevel_unit_end (seg_start, &ok);
      gchar *raw, *name;

      if (!ok) {
        lex_syntax_error (&local,
                          "incomplete Sieve script (unclosed brace, "
                          "string or comment)");
        goto fail;
      }
      end = swallow_trailing_comments (end);

      raw = normalize_opaque_raw (seg_start, end - seg_start);
      name = opaque_rule_name (raw);
      g_ptr_array_add (set->rules, sieve_rule_new_opaque (name, raw));
      g_free (raw);
      g_free (name);

      lx.cur = end;
      if (!lex_advance (&lx, &local))
        goto fail;
      continue;
    }

    lex_syntax_error (&local, "unexpected token at the top level of the script");
    goto fail;
  }

  {
    gboolean has_opaque = FALSE;

    for (guint i = 0; i < set->rules->len; i++)
      if (((SieveRule *) g_ptr_array_index (set->rules, i))->opaque) {
        has_opaque = TRUE;
        break;
      }
    if (has_opaque && lead_req->len > 0)
      set->extra_requires = g_steal_pointer (&lead_req);
  }

  g_clear_pointer (&lead_req, g_ptr_array_unref);
  lex_clear (&lx);
  return set;

fail:
  g_clear_pointer (&lead_req, g_ptr_array_unref);
  lex_clear (&lx);
  sieve_rule_set_free (set);
  g_propagate_error (error, local);
  return NULL;
}

/* Explicit, per-rule "unlock" of an opaque rule: reattempts the same
 * "if allof/anyof(...) { actions }" grammar sieve_rule_set_parse() uses
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
  Lex lx = { 0 };
  SieveRule *result = NULL;
  GError *local = NULL;

  g_return_val_if_fail (rule != NULL && rule->opaque, NULL);

  lx.cur = rule->raw != NULL ? rule->raw : "";
  if (!lex_advance (&lx, &local))
    goto out;

  if (lx.kind == TK_RULE && !lex_advance (&lx, &local))
    goto out;

  result = sieve_rule_new (rule->name);
  if (!parse_if (&lx, result, &local)) {
    sieve_rule_free (result);
    result = NULL;
    goto out;
  }
  if (lx.kind != TK_EOF) {
    sieve_rule_free (result);
    result = NULL;
    unsupported (&local, "more than a single \"if\" block");
  }

out:
  lex_clear (&lx);
  if (result == NULL)
    g_propagate_error (error, local);
  else
    g_clear_error (&local);
  return result;
}
