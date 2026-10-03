/* sieve-ast.h
 *
 * Generic RFC 5228 parser: turns a Sieve script into a syntax tree
 * (commands, tests, arguments) without interpreting it.
 *
 * Scope:
 *   - the full RFC 5228 grammar (§8.1 lexical tokens, §8.2 grammar):
 *     identifiers, tags, numbers with a K/M/G quantifier, quoted strings,
 *     multi-line strings ("text:" ... "."), string lists, "#" and
 *     "/" "*" ... "*" "/" comments, nested tests and blocks;
 *   - no knowledge of any command or test: "if", "vacation", "not"…
 *     are all just identifiers here. Nor of any convention layered on
 *     top of Sieve comments ("# rule:[name]", "if false # <test>"…):
 *     that stays in sieve-model.c, which can recover those comments
 *     through the byte offsets kept on every node (see below).
 *
 * Every node records its [start, end) byte offsets in the source, and
 * each command also the start of the whitespace/comments preceding it
 * (`trivia_start`), so the caller can copy any node back verbatim or
 * inspect the comments around it.
 *
 * Error handling, chosen to keep sieve_rule_set_parse()'s contract
 * ("only a lexically broken script fails"):
 *   - a LEXICAL error (unterminated quoted string, multi-line string or
 *     bracket comment, unclosed "{", invalid UTF-8) fails the whole
 *     parse: NULL + SIEVE_MODEL_ERROR_SYNTAX;
 *   - a GRAMMAR error inside a top-level command does not: that command
 *     is replaced by an "unparseable" node (name == NULL, `error` set)
 *     spanning up to its closing ";" or "}", and parsing resumes after
 *     it. Nesting (blocks + tests) is bounded by SIEVE_AST_MAX_DEPTH, so
 *     a hostile script can't exhaust the stack.
 *
 * Pure GLib, no GTK nor Evolution dependency: testable on its own
 * (see tests/test-sieve-ast.c).
 */

#ifndef SIEVE_AST_H
#define SIEVE_AST_H

#include <glib.h>

G_BEGIN_DECLS

/* Maximum nesting of blocks and tests combined. */
#define SIEVE_AST_MAX_DEPTH 64

typedef enum {
  SIEVE_AST_ARG_STRING,       /* "..." or text: ... . (value decoded) */
  SIEVE_AST_ARG_STRING_LIST,  /* [ "a", "b" ] */
  SIEVE_AST_ARG_NUMBER,       /* 100, 1K, 10M, 2G */
  SIEVE_AST_ARG_TAG           /* :contains */
} SieveAstArgKind;

typedef struct {
  SieveAstArgKind kind;
  gchar     *str;        /* STRING: decoded value; TAG: lowercased name,
                            without the ':'; NUMBER: source text ("1M") */
  GPtrArray *list;       /* STRING_LIST: decoded gchar* (at least one) */
  guint64    number;     /* NUMBER: value with the quantifier applied */
  gboolean   multiline;  /* STRING: came from a "text:" literal */
  gsize      start, end; /* byte offsets in the source */
} SieveAstArg;

typedef struct SieveAstTest SieveAstTest;

struct SieveAstTest {
  gchar     *name;       /* lowercased identifier */
  GPtrArray *args;       /* SieveAstArg* */
  GPtrArray *tests;      /* nested SieveAstTest*, NULL if none */
  gboolean   test_list;  /* TRUE: "( t1, t2 )"; FALSE: a single test */
  gsize      start, end;
};

typedef struct SieveAstCommand SieveAstCommand;

struct SieveAstCommand {
  gchar     *name;         /* lowercased identifier; NULL = unparseable
                              top-level span (see `error`) */
  GPtrArray *args;         /* SieveAstArg* */
  GPtrArray *tests;        /* test(s) of if/elsif…, NULL if none */
  gboolean   test_list;
  GPtrArray *block;        /* SieveAstCommand*, NULL if ended by ";" */
  gchar     *error;        /* name == NULL only: why the span failed to
                              parse ("line L, column C: ...") */
  gsize      trivia_start; /* start of the whitespace/comments before it */
  gsize      start, end;   /* end: just past the ";" or "}" */
};

typedef struct {
  const gchar *source;     /* borrowed: must outlive the AST */
  GPtrArray   *commands;   /* top level, SieveAstCommand* */
} SieveAst;

/* Parses `script` (NUL-terminated, UTF-8). Returns NULL + `error`
 * (SIEVE_MODEL_ERROR / SIEVE_MODEL_ERROR_SYNTAX) on a lexical error
 * only; see the overview above. `script` is borrowed by the result. */
SieveAst *sieve_ast_parse (const gchar *script, GError **error);
void      sieve_ast_free  (SieveAst *ast);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (SieveAst, sieve_ast_free)

G_END_DECLS

#endif /* SIEVE_AST_H */
