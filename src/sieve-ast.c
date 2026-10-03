/* sieve-ast.c — see sieve-ast.h for the overview. */

#include "sieve-ast.h"
#include "sieve-model.h" /* SIEVE_MODEL_ERROR */

#include <stdarg.h>
#include <string.h>

/* ---- Nodes ------------------------------------------------------------- */

static void
sieve_ast_arg_free (SieveAstArg *arg)
{
  if (arg == NULL)
    return;
  g_free (arg->str);
  g_clear_pointer (&arg->list, g_ptr_array_unref);
  g_free (arg);
}

static void
sieve_ast_test_free (SieveAstTest *test)
{
  if (test == NULL)
    return;
  g_free (test->name);
  g_clear_pointer (&test->args, g_ptr_array_unref);
  g_clear_pointer (&test->tests, g_ptr_array_unref);
  g_free (test);
}

static void
sieve_ast_command_free (SieveAstCommand *cmd)
{
  if (cmd == NULL)
    return;
  g_free (cmd->name);
  g_clear_pointer (&cmd->args, g_ptr_array_unref);
  g_clear_pointer (&cmd->tests, g_ptr_array_unref);
  g_clear_pointer (&cmd->block, g_ptr_array_unref);
  g_free (cmd->error);
  g_free (cmd);
}

static GPtrArray *
new_arg_array (void)
{
  return g_ptr_array_new_with_free_func ((GDestroyNotify) sieve_ast_arg_free);
}

static GPtrArray *
new_test_array (void)
{
  return g_ptr_array_new_with_free_func ((GDestroyNotify) sieve_ast_test_free);
}

static GPtrArray *
new_command_array (void)
{
  return g_ptr_array_new_with_free_func ((GDestroyNotify) sieve_ast_command_free);
}

void
sieve_ast_free (SieveAst *ast)
{
  if (ast == NULL)
    return;
  g_clear_pointer (&ast->commands, g_ptr_array_unref);
  g_free (ast);
}

/* ---- Error reporting ----------------------------------------------------- */

/* "line L, column C: <msg>"; columns count characters, not bytes. The
 * source is valid UTF-8 up to `offset` (checked before lexing). */
static gchar *
format_at (const gchar *src, gsize offset, const gchar *fmt, va_list ap)
{
  guint line = 1;
  gsize line_start = 0;
  g_autofree gchar *msg = g_strdup_vprintf (fmt, ap);

  for (gsize i = 0; i < offset; i++) {
    if (src[i] == '\n') {
      line++;
      line_start = i + 1;
    }
  }
  return g_strdup_printf ("line %u, column %ld: %s", line,
                          g_utf8_strlen (src + line_start, offset - line_start) + 1,
                          msg);
}

G_GNUC_PRINTF (4, 5)
static void
fatal_error (GError **error, const gchar *src, gsize offset, const gchar *fmt, ...)
{
  va_list ap;
  g_autofree gchar *msg = NULL;

  va_start (ap, fmt);
  msg = format_at (src, offset, fmt, ap);
  va_end (ap);
  g_set_error_literal (error, SIEVE_MODEL_ERROR, SIEVE_MODEL_ERROR_SYNTAX, msg);
}

/* ---- Lexical analysis (RFC 5228 §8.1) ---------------------------------- */

typedef enum {
  T_EOF,
  T_LPAREN, T_RPAREN, T_LBRACE, T_RBRACE, T_LBRACKET, T_RBRACKET,
  T_COMMA, T_SEMI,
  T_STRING,    /* quoted string */
  T_MULTILINE, /* text: ... . */
  T_NUMBER,
  T_IDENT,
  T_TAG,
  T_ERROR      /* malformed token: a grammar error, not a fatal one */
} TokKind;

typedef struct {
  const gchar *src;
  gsize        pos;          /* next read position */
  TokKind      kind;
  gsize        trivia_start; /* start of whitespace/comments before the token */
  gsize        start, end;   /* the token itself */
  gchar       *str;          /* STRING/MULTILINE: decoded value;
                                IDENT/TAG: lowercased name;
                                NUMBER: source text; ERROR: message */
  guint64      number;       /* NUMBER */
} Lexer;

static const gchar *
tok_describe (TokKind kind)
{
  switch (kind) {
    case T_EOF:       return "end of script";
    case T_LPAREN:    return "\"(\"";
    case T_RPAREN:    return "\")\"";
    case T_LBRACE:    return "\"{\"";
    case T_RBRACE:    return "\"}\"";
    case T_LBRACKET:  return "\"[\"";
    case T_RBRACKET:  return "\"]\"";
    case T_COMMA:     return "\",\"";
    case T_SEMI:      return "\";\"";
    case T_STRING:    return "a string";
    case T_MULTILINE: return "a multi-line string";
    case T_NUMBER:    return "a number";
    case T_IDENT:     return "an identifier";
    case T_TAG:       return "a tag";
    case T_ERROR:     return "an invalid token";
    default:          return "?";
  }
}

static gboolean
is_ident_start (gchar c)
{
  return g_ascii_isalpha (c) || c == '_';
}

static gboolean
is_ident_char (gchar c)
{
  return g_ascii_isalnum (c) || c == '_';
}

/* Body of a "text:" literal, `p` just past the ':'. RFC 5228 §2.4.2:
 * rest of the "text:" line blank or a hash comment, then lines up to
 * one made of a single "."; a leading "." is removed from every other
 * line (dot-stuffing). Line terminators (CRLF or bare LF) are kept as
 * found in the decoded value. */
static gboolean
lex_multiline (Lexer *lx, gsize p, GError **error)
{
  const gchar *src = lx->src;
  GString *s;

  while (src[p] == ' ' || src[p] == '\t')
    p++;
  if (src[p] == '#')
    while (src[p] != '\0' && src[p] != '\n')
      p++;
  if (src[p] == '\r' && src[p + 1] == '\n') {
    p += 2;
  } else if (src[p] == '\n') {
    p++;
  } else {
    fatal_error (error, src, p, "expected the end of the line after \"text:\"");
    return FALSE;
  }

  s = g_string_new (NULL);
  for (;;) {
    gsize ls = p, eol = p, content_end, next;

    if (src[p] == '\0') {
      g_string_free (s, TRUE);
      fatal_error (error, src, lx->start,
                   "unterminated multi-line string (missing \".\" line)");
      return FALSE;
    }
    while (src[eol] != '\0' && src[eol] != '\n')
      eol++;
    content_end = eol;
    if (content_end > ls && src[content_end - 1] == '\r')
      content_end--;
    next = (src[eol] == '\n') ? eol + 1 : eol;

    if (content_end - ls == 1 && src[ls] == '.') {
      p = next;
      break;
    }
    if (src[ls] == '.')
      ls++;
    g_string_append_len (s, src + ls, content_end - ls);
    g_string_append_len (s, src + content_end, next - content_end);
    p = next;
  }

  lx->kind = T_MULTILINE;
  lx->str = g_string_free (s, FALSE);
  lx->pos = p;
  return TRUE;
}

static void
lex_set_error (Lexer *lx, gsize end, const gchar *msg)
{
  lx->kind = T_ERROR;
  lx->str = g_strdup (msg);
  lx->pos = end;
}

/* Reads the next token into `lx`. Returns FALSE (with `error`) on a
 * fatal lexical error only; a malformed but bounded token (bad number,
 * stray character) yields T_ERROR instead, so the parser can recover. */
static gboolean
lex_next (Lexer *lx, GError **error)
{
  const gchar *src = lx->src;
  gsize p = lx->pos;

  g_clear_pointer (&lx->str, g_free);
  lx->number = 0;
  lx->trivia_start = p;

  for (;;) {
    gchar c = src[p];

    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
      p++;
    } else if (c == '#') {
      while (src[p] != '\0' && src[p] != '\n')
        p++;
    } else if (c == '/' && src[p + 1] == '*') {
      const gchar *e = strstr (src + p + 2, "*/");

      if (e == NULL) {
        fatal_error (error, src, p, "unterminated \"/* ... */\" comment");
        return FALSE;
      }
      p = (e - src) + 2;
    } else {
      break;
    }
  }

  lx->start = p;
  lx->pos = p + 1;

  switch (src[p]) {
    case '\0': lx->kind = T_EOF; lx->pos = p; break;
    case '(':  lx->kind = T_LPAREN;   break;
    case ')':  lx->kind = T_RPAREN;   break;
    case '{':  lx->kind = T_LBRACE;   break;
    case '}':  lx->kind = T_RBRACE;   break;
    case '[':  lx->kind = T_LBRACKET; break;
    case ']':  lx->kind = T_RBRACKET; break;
    case ',':  lx->kind = T_COMMA;    break;
    case ';':  lx->kind = T_SEMI;     break;

    case '"': {
      GString *s = g_string_new (NULL);

      /* RFC 5228 §2.4.2: "\" followed by any character stands for that
       * character (only \" and \\ are defined; others are tolerated). */
      for (p++; src[p] != '\0' && src[p] != '"'; p++) {
        if (src[p] == '\\' && src[p + 1] != '\0')
          p++;
        g_string_append_c (s, src[p]);
      }
      if (src[p] == '\0') {
        g_string_free (s, TRUE);
        fatal_error (error, src, lx->start, "unterminated quoted string");
        return FALSE;
      }
      lx->kind = T_STRING;
      lx->str = g_string_free (s, FALSE);
      lx->pos = p + 1;
      break;
    }

    case ':': {
      gsize q = p + 1;

      if (!is_ident_start (src[q])) {
        lex_set_error (lx, q, "expected a tag name after \":\"");
        break;
      }
      while (is_ident_char (src[q]))
        q++;
      lx->kind = T_TAG;
      lx->str = g_ascii_strdown (src + p + 1, q - (p + 1));
      lx->pos = q;
      break;
    }

    default:
      if (g_ascii_isdigit (src[p])) {
        guint64 v = 0;
        gboolean overflow = FALSE;
        gsize q = p;
        guint shift = 0;

        for (; g_ascii_isdigit (src[q]); q++) {
          guint d = src[q] - '0';

          if (v > (G_MAXUINT64 - d) / 10)
            overflow = TRUE;
          else
            v = v * 10 + d;
        }
        switch (src[q]) {
          case 'k': case 'K': shift = 10; q++; break;
          case 'm': case 'M': shift = 20; q++; break;
          case 'g': case 'G': shift = 30; q++; break;
          default: break;
        }
        if (is_ident_char (src[q])) {
          while (is_ident_char (src[q]))
            q++;
          lex_set_error (lx, q, "invalid number");
          break;
        }
        if (overflow || v > (G_MAXUINT64 >> shift)) {
          lex_set_error (lx, q, "number too large");
          break;
        }
        lx->kind = T_NUMBER;
        lx->number = v << shift;
        lx->str = g_strndup (src + p, q - p);
        lx->pos = q;
      } else if (is_ident_start (src[p])) {
        gsize q = p;

        while (is_ident_char (src[q]))
          q++;
        if (q - p == 4 && g_ascii_strncasecmp (src + p, "text", 4) == 0 &&
            src[q] == ':') {
          if (!lex_multiline (lx, q + 1, error))
            return FALSE;
          break;
        }
        lx->kind = T_IDENT;
        lx->str = g_ascii_strdown (src + p, q - p);
        lx->pos = q;
      } else {
        const gchar *next = g_utf8_next_char (src + p);
        g_autofree gchar *msg =
          g_strdup_printf ("unexpected character \"%.*s\"",
                           (gint) (next - (src + p)), src + p);

        lex_set_error (lx, next - src, msg);
      }
      break;
  }

  lx->end = lx->pos;
  return TRUE;
}

/* ---- Grammar (RFC 5228 §8.2) ------------------------------------------- */

typedef struct {
  Lexer  lx;
  guint  depth;
  gchar *err; /* first grammar error, "line L, column C: ..." */
} Parser;

G_GNUC_PRINTF (3, 4)
static void
grammar_error (Parser *ps, gsize offset, const gchar *fmt, ...)
{
  va_list ap;

  if (ps->err != NULL)
    return;
  va_start (ap, fmt);
  ps->err = format_at (ps->lx.src, offset, fmt, ap);
  va_end (ap);
}

static void
unexpected (Parser *ps, const gchar *expected)
{
  if (ps->lx.kind == T_ERROR)
    grammar_error (ps, ps->lx.start, "%s", ps->lx.str);
  else
    grammar_error (ps, ps->lx.start, "expected %s, got %s",
                   expected, tok_describe (ps->lx.kind));
}

static gboolean
enter (Parser *ps)
{
  if (++ps->depth > SIEVE_AST_MAX_DEPTH) {
    grammar_error (ps, ps->lx.start, "nesting deeper than %d levels",
                   SIEVE_AST_MAX_DEPTH);
    return FALSE;
  }
  return TRUE;
}

static gboolean parse_arguments (Parser *ps, GPtrArray *args,
                                 GPtrArray **tests, gboolean *test_list,
                                 gsize *end, GError **error);

static gboolean
is_string_token (TokKind kind)
{
  return kind == T_STRING || kind == T_MULTILINE;
}

/* string-list = "[" string *("," string) "]" ; the "[" is current. */
static SieveAstArg *
parse_string_list (Parser *ps, GError **error)
{
  SieveAstArg *arg = g_new0 (SieveAstArg, 1);

  arg->kind = SIEVE_AST_ARG_STRING_LIST;
  arg->list = g_ptr_array_new_with_free_func (g_free);
  arg->start = ps->lx.start;

  if (!lex_next (&ps->lx, error))
    goto fail;
  for (;;) {
    if (!is_string_token (ps->lx.kind)) {
      unexpected (ps, "a string");
      goto fail;
    }
    g_ptr_array_add (arg->list, g_steal_pointer (&ps->lx.str));
    if (!lex_next (&ps->lx, error))
      goto fail;
    if (ps->lx.kind != T_COMMA)
      break;
    if (!lex_next (&ps->lx, error))
      goto fail;
  }
  if (ps->lx.kind != T_RBRACKET) {
    unexpected (ps, "\",\" or \"]\"");
    goto fail;
  }
  arg->end = ps->lx.end;
  if (!lex_next (&ps->lx, error))
    goto fail;
  return arg;

fail:
  sieve_ast_arg_free (arg);
  return NULL;
}

/* test = identifier arguments */
static SieveAstTest *
parse_test (Parser *ps, GError **error)
{
  SieveAstTest *test = NULL;

  if (ps->lx.kind != T_IDENT) {
    unexpected (ps, "a test name");
    return NULL;
  }
  if (!enter (ps))
    return NULL;

  test = g_new0 (SieveAstTest, 1);
  test->args = new_arg_array ();
  test->name = g_steal_pointer (&ps->lx.str);
  test->start = ps->lx.start;
  test->end = ps->lx.end;

  if (!lex_next (&ps->lx, error) ||
      !parse_arguments (ps, test->args, &test->tests, &test->test_list,
                        &test->end, error)) {
    sieve_ast_test_free (test);
    test = NULL;
  }
  ps->depth--;
  return test;
}

/* test-list = "(" test *("," test) ")" ; the "(" is current. */
static GPtrArray *
parse_test_list (Parser *ps, gsize *end, GError **error)
{
  GPtrArray *tests = new_test_array ();

  if (!lex_next (&ps->lx, error))
    goto fail;
  for (;;) {
    SieveAstTest *t = parse_test (ps, error);

    if (t == NULL)
      goto fail;
    g_ptr_array_add (tests, t);
    if (ps->lx.kind != T_COMMA)
      break;
    if (!lex_next (&ps->lx, error))
      goto fail;
  }
  if (ps->lx.kind != T_RPAREN) {
    unexpected (ps, "\",\" or \")\"");
    goto fail;
  }
  *end = ps->lx.end;
  if (!lex_next (&ps->lx, error))
    goto fail;
  return tests;

fail:
  g_ptr_array_unref (tests);
  return NULL;
}

/* arguments = *argument [ test / test-list ]
 * argument  = string-list / number / tag
 * `*end` is moved past each argument/test consumed. */
static gboolean
parse_arguments (Parser *ps, GPtrArray *args, GPtrArray **tests,
                 gboolean *test_list, gsize *end, GError **error)
{
  for (;;) {
    SieveAstArg *arg;

    switch (ps->lx.kind) {
      case T_LBRACKET:
        arg = parse_string_list (ps, error);
        if (arg == NULL)
          return FALSE;
        g_ptr_array_add (args, arg);
        *end = arg->end;
        continue;

      case T_STRING:
      case T_MULTILINE:
      case T_NUMBER:
      case T_TAG:
        arg = g_new0 (SieveAstArg, 1);
        arg->kind = ps->lx.kind == T_NUMBER ? SIEVE_AST_ARG_NUMBER
                  : ps->lx.kind == T_TAG    ? SIEVE_AST_ARG_TAG
                  : SIEVE_AST_ARG_STRING;
        arg->multiline = (ps->lx.kind == T_MULTILINE);
        arg->str = g_steal_pointer (&ps->lx.str);
        arg->number = ps->lx.number;
        arg->start = ps->lx.start;
        arg->end = ps->lx.end;
        g_ptr_array_add (args, arg);
        *end = arg->end;
        if (!lex_next (&ps->lx, error))
          return FALSE;
        continue;

      default:
        break;
    }
    break;
  }

  if (ps->lx.kind == T_IDENT) {
    SieveAstTest *t = parse_test (ps, error);

    if (t == NULL)
      return FALSE;
    *tests = new_test_array ();
    g_ptr_array_add (*tests, t);
    *test_list = FALSE;
    *end = t->end;
  } else if (ps->lx.kind == T_LPAREN) {
    *tests = parse_test_list (ps, end, error);
    if (*tests == NULL)
      return FALSE;
    *test_list = TRUE;
  }
  return TRUE;
}

/* command = identifier arguments (";" / block)
 * block   = "{" commands "}" */
static SieveAstCommand *
parse_command (Parser *ps, GError **error)
{
  SieveAstCommand *cmd;

  if (ps->lx.kind != T_IDENT) {
    unexpected (ps, "a command name");
    return NULL;
  }

  cmd = g_new0 (SieveAstCommand, 1);
  cmd->args = new_arg_array ();
  cmd->name = g_steal_pointer (&ps->lx.str);
  cmd->trivia_start = ps->lx.trivia_start;
  cmd->start = ps->lx.start;
  cmd->end = ps->lx.end;

  if (!lex_next (&ps->lx, error) ||
      !parse_arguments (ps, cmd->args, &cmd->tests, &cmd->test_list,
                        &cmd->end, error))
    goto fail;

  if (ps->lx.kind == T_SEMI) {
    cmd->end = ps->lx.end;
    if (!lex_next (&ps->lx, error))
      goto fail;
    return cmd;
  }

  if (ps->lx.kind != T_LBRACE) {
    unexpected (ps, "\";\" or \"{\"");
    goto fail;
  }
  if (!enter (ps))
    goto fail;
  cmd->block = new_command_array ();
  if (!lex_next (&ps->lx, error))
    goto fail;
  while (ps->lx.kind != T_RBRACE) {
    SieveAstCommand *sub;

    if (ps->lx.kind == T_EOF) {
      unexpected (ps, "\"}\"");
      goto fail;
    }
    sub = parse_command (ps, error);
    if (sub == NULL)
      goto fail;
    g_ptr_array_add (cmd->block, sub);
  }
  ps->depth--;
  cmd->end = ps->lx.end;
  if (!lex_next (&ps->lx, error))
    goto fail;
  return cmd;

fail:
  sieve_ast_command_free (cmd);
  return NULL;
}

/* After a grammar error in a top-level command, re-lexes from its first
 * token up to the end of that unit: the ";" ending a simple command, or
 * the "}" closing its first block (or a stray "}"), whichever comes
 * first. Iterative, so it can't be defeated by deep nesting. Fails
 * (fatal) only if EOF is reached inside an unclosed "{". */
static gboolean
skip_unit (Parser *ps, gsize unit_start, gsize *end, GError **error)
{
  guint depth = 0;

  ps->lx.pos = unit_start;
  if (!lex_next (&ps->lx, error))
    return FALSE;

  *end = unit_start;
  for (;;) {
    TokKind kind = ps->lx.kind;

    if (kind == T_EOF) {
      if (depth > 0) {
        fatal_error (error, ps->lx.src, unit_start, "unclosed \"{\"");
        return FALSE;
      }
      return TRUE;
    }
    *end = ps->lx.end;
    if (!lex_next (&ps->lx, error))
      return FALSE;

    if (kind == T_LBRACE) {
      depth++;
    } else if (kind == T_RBRACE) {
      if (depth <= 1)
        return TRUE;
      depth--;
    } else if (kind == T_SEMI && depth == 0) {
      return TRUE;
    }
  }
}

SieveAst *
sieve_ast_parse (const gchar *script, GError **error)
{
  g_autoptr (SieveAst) ast = g_new0 (SieveAst, 1);
  Parser ps = { 0 };
  GError *local = NULL;
  const gchar *bad;

  if (script == NULL)
    script = "";

  ast->source = script;
  ast->commands = new_command_array ();

  if (!g_utf8_validate (script, -1, &bad)) {
    fatal_error (error, script, bad - script, "invalid UTF-8");
    return NULL;
  }

  ps.lx.src = script;
  if (!lex_next (&ps.lx, &local))
    goto fail;

  while (ps.lx.kind != T_EOF) {
    gsize trivia_start = ps.lx.trivia_start;
    gsize start = ps.lx.start;
    SieveAstCommand *cmd;

    ps.depth = 0;
    g_clear_pointer (&ps.err, g_free);

    cmd = parse_command (&ps, &local);
    if (cmd == NULL) {
      if (local != NULL)
        goto fail;

      cmd = g_new0 (SieveAstCommand, 1);
      cmd->error = g_steal_pointer (&ps.err);
      cmd->trivia_start = trivia_start;
      cmd->start = start;
      if (!skip_unit (&ps, start, &cmd->end, &local)) {
        sieve_ast_command_free (cmd);
        goto fail;
      }
    }
    g_ptr_array_add (ast->commands, cmd);
  }

  g_free (ps.err);
  g_free (ps.lx.str);
  return g_steal_pointer (&ast);

fail:
  g_free (ps.err);
  g_free (ps.lx.str);
  g_propagate_error (error, local);
  return NULL;
}
