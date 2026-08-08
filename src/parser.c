#include "parser.h"

#include "allocator.h"
#include "ast.h"
#include "scanner.h"
#include "string_utils.h"

#include <assert.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>

// ── growable lists ───────────────────────────────────────────────────────────
//
// Every list this parser builds has one shape: accumulate an unknown number of
// items, then hand them to an AST node sized to exactly what arrived. Each was
// a fixed stack array with a cap and a "too many ..." diagnostic behind it, and
// a cap on how many arguments a call may pass, or fields a struct may declare,
// can only ever refuse a program that is otherwise correct. The grammar sets no
// such limit and neither does anything downstream, so the number was never a
// rule about the language — it was the size of a buffer, showing through.
//
// The stack buffer stays, because it was never the wrong part. An arena does
// not reuse: `arena_realloc_fn` bump-allocates a fresh block and abandons the
// old one, so a list that grew from the arena from the start would cost ~2n
// while doubling plus another n for the right-sized copy, where the stack
// version costs exactly n. The buffer is now a small-size optimisation that
// *spills* rather than a ceiling that reports: the common case allocates
// exactly what the node keeps, and the uncommon case merely allocates.
//
// The inline buffer is zeroed and so is every spilled extension, so a field no
// site happens to write (a `span` on an item kind that has none) reads as zero
// instead of as whatever the stack held.
#define PLIST(T, name, inline_cap)                                             \
  T name##_buf[inline_cap] = {0};                                              \
  T *name = name##_buf;                                                        \
  int name##_count = 0;                                                        \
  int name##_cap = (inline_cap)

// room for one more item, spilling to the arena when the inline buffer is full
#define PLIST_GROW(p, name)                                                    \
  do {                                                                         \
    if ((name##_count) == (name##_cap)) {                                      \
      int plist_cap = (name##_cap) * 2;                                        \
      size_t plist_used = sizeof(*(name)) * (size_t)(name##_count);            \
      void *plist_fresh =                                                      \
          al_alloc((p)->al, sizeof(*(name)) * (size_t)plist_cap);              \
      memcpy(plist_fresh, (name), plist_used);                                 \
      memset((char *)plist_fresh + plist_used, 0,                              \
             sizeof(*(name)) * (size_t)plist_cap - plist_used);                \
      (name) = plist_fresh;                                                    \
      (name##_cap) = plist_cap;                                                \
    }                                                                          \
  } while (0)

#define PLIST_PUSH(p, name, value)                                             \
  do {                                                                         \
    PLIST_GROW(p, name);                                                       \
    (name)[(name##_count)++] = (value);                                        \
  } while (0)

// the finished list, in the arena, sized to what it holds
#define PLIST_TAKE(p, name)                                                    \
  memcpy(al_alloc((p)->al, sizeof(*(name)) * (size_t)(name##_count)), (name),  \
         sizeof(*(name)) * (size_t)(name##_count))

// how many arguments and associated-type bindings one `dyn Trait<..>` is
// expected to carry — an inline size, not a limit. It used to be a limit, and
// the diagnostic behind it was answering a question the parser cannot answer:
// whether a binding list is total is decided against the *trait's* associated
// types, which resolve_dyn_type does by name, reporting the unknown ones and
// the duplicated ones. A trait with nine associated types is not a parse error.
#define MAX_ASSOC_BINDINGS 8

// temp helpers

static Span span_merge(Span a, Span b) {
  (void)b;
  return (Span){.line = a.line,
                .col = a.col,
                .line_end = b.line_end,
                .col_end = b.col_end};
}

// error reporting

static void error_at(Parser *p, Span span, const char *msg) {
  if (p->panic_mode) {
    return;
  }
  p->panic_mode = true;
  diag_error(p->diags, span, "%s", msg);
}

// `@allow("typo")`: the only parser error that has to name what it read, and
// the only one that does not enter panic mode. Every other error here is about
// a token that arrived where the grammar wanted another, and the parse cannot
// continue past it; this attribute is well-formed and merely names nothing, so
// the declaration behind it still reads — all that is lost is the policy.
static void error_at_lint(Parser *p, Span span, StringView name) {
  if (p->panic_mode) {
    return;
  }
  diag_error(p->diags, span, "unknown lint '" SV_FMT "'", SV_ARG(name));
  diag_note(p->diags, (Span){0}, "available: %s", diag_lint_names());
}

// token navigation

static Token *peek_tok(const Parser *p) { return &p->tokens[p->current]; }

static Token *peek_ahead(const Parser *p, int offset) {
  int idx = p->current + offset;
  if (idx >= p->count) {
    return &p->tokens[p->count - 1]; // EOF
  }
  return &p->tokens[idx];
}

static Token *previous_tok(const Parser *p) {
  return &p->tokens[p->current - 1];
}

static bool check_tok(const Parser *p, TokenType type) {
  return peek_tok(p)->type == type;
}

static bool is_at_end(const Parser *p) { return check_tok(p, TOKEN_EOF); }

static Token *current_tok(const Parser *p) {
  if (is_at_end(p)) {
    return previous_tok(p);
  }
  return peek_tok(p);
}

static Span token_span(const Token *t) {
  return (Span){.line = t->line,
                .col = t->col,
                .line_end = t->line,
                .col_end = t->col + t->lexeme.len};
}

static Span current_tok_span(const Parser *p) {
  if (is_at_end(p)) {
    Token *prev = previous_tok(p);
    return (Span){.line = prev->line,
                  .col = prev->col + 1,
                  .line_end = prev->line,
                  .col_end = prev->col + 1};
  }
  return token_span(peek_tok(p));
}

static Span previous_tok_span(const Parser *p) {
  if (p->current == 0) {
    return (Span){.line = 1, .col = 1, .line_end = 1, .col_end = 1};
  }
  return token_span(previous_tok(p));
}

static Token *advance_tok(Parser *p) {
  if (!is_at_end(p))
    p->current++;
  return previous_tok(p);
}

static bool match_tok(Parser *p, TokenType type) {
  if (!check_tok(p, type)) {
    return false;
  }
  advance_tok(p);
  return true;
}

/*
#define match_any(p, ...)                                                      \
  match_any_impl(p, (TokenType[]){__VA_ARGS__},                                \
                 sizeof((TokenType[]){__VA_ARGS__}) / sizeof(TokenType))
*/

// static bool match_any_impl(Parser *p, const TokenType *types, size_t count) {
//   for (size_t i = 0; i < count; i++) {
//     if (check(p, types[i])) {
//       advance_tok(p);
//       return true;
//     }
//   }

//   return false;
// }

static bool consume_tok(Parser *p, TokenType type, const char *msg) {
  if (check_tok(p, type)) {
    advance_tok(p);
    return true;
  }
  error_at(p, token_span(peek_tok(p)), msg);
  return false;
}

// synchronization

static void sync_to_stmt(Parser *p) {
  p->panic_mode = false;

  while (!is_at_end(p)) {
    // A semicolon ends the broken statement — consume it and stop.
    if (check_tok(p, TOKEN_SEMICOLON)) {
      advance_tok(p);
      return;
    }

    switch (peek_tok(p)->type) {
    case TOKEN_VAR:
    case TOKEN_RETURN:
    case TOKEN_BREAK:
    case TOKEN_CONTINUE:
    case TOKEN_IF:
    case TOKEN_FOR:
    case TOKEN_MATCH:
    // declaration keywords — we've left the block entirely
    case TOKEN_FUN:
    case TOKEN_STRUCT:
    case TOKEN_ENUM:
    case TOKEN_TRAIT:
    case TOKEN_IMPL:
    case TOKEN_PUB:
    case TOKEN_USE:
    case TOKEN_MOD:
    case TOKEN_RBRACE: // end of the enclosing block
    case TOKEN_WHILE:
    case TOKEN_LOOP:
      return;
    default:
      advance_tok(p);
    }
  }
}

static void sync_to_decl(Parser *p) {
  p->panic_mode = false;

  while (!is_at_end(p)) {
    switch (peek_tok(p)->type) {
    case TOKEN_FUN:
    case TOKEN_STRUCT:
    case TOKEN_ENUM:
    case TOKEN_TRAIT:
    case TOKEN_IMPL:
    case TOKEN_PUB:
    case TOKEN_USE:
    case TOKEN_MOD:
    case TOKEN_VAR:
      return;
    default:
      advance_tok(p);
    }
  }
}

// parsing

typedef enum {
  PATH_EXPR, // type args require turbo-fish
  PATH_TYPE, // type args can be written with or without '::'
  PATH_USE,  // stop before '{' for struct patterns
  PATH_BARE, // no type args: a following '<' belongs to the caller
} PathParseMode;

static bool parse_path(Parser *p, PathParseMode mode, Path *out);

// The `<..>` after a `dyn Trait`: the trait's own type arguments, then its
// associated-type bindings (`<int, Item = string>`). Writes each list into its
// out-parameter and returns false on a parse error.
static bool parse_dyn_args(Parser *p, TypeNode ***out_args, int *out_arg_count,
                           AssocBindingNode **out_bindings,
                           int *out_binding_count);

// a `var` binding is a pattern; the checker rejects the refutable ones.
static Pattern *parse_pattern(Parser *p);

static TypeNode *parse_type(Parser *p) {
  TypeNode *base = NULL;

  // `!` — code that does not come back. Returned straight, like `()`: neither
  // takes a suffix, and both are punctuation so that the spelling a signature
  // writes is the one a diagnostic prints back.
  if (match_tok(p, TOKEN_BANG)) {
    return ast_type_node(TYNODE_NEVER, token_span(previous_tok(p)), p->al);
  }

  if (match_tok(p, TOKEN_LPAREN)) {
    if (match_tok(p, TOKEN_RPAREN)) {
      // unit type
      return ast_type_node(
          TYNODE_UNIT, span_merge(previous_tok_span(p), previous_tok_span(p)),
          p->al);
    }

    // tuple type
    PLIST(TypeNode *, elem_types, 8);
    bool had_error = false;

    do {
      TypeNode *ty = parse_type(p);
      if (ty->kind == TYNODE_POISON) {
        had_error = true;
        break;
      }
      PLIST_PUSH(p, elem_types, ty);
    } while (match_tok(p, TOKEN_COMMA));

    if (had_error) {
      return ast_type_node(TYNODE_POISON, current_tok_span(p), p->al);
    }

    if (!consume_tok(p, TOKEN_RPAREN, "expected ')' after tuple type")) {
      return ast_type_node(TYNODE_POISON, current_tok_span(p), p->al);
    }

    if (elem_types_count == 1) {
      return elem_types[0];
    }

    base = ast_type_node(
        TYNODE_TUPLE, span_merge(token_span(peek_tok(p)), previous_tok_span(p)),
        p->al);
    base->as.tuple.elems = PLIST_TAKE(p, elem_types);
    base->as.tuple.count = elem_types_count;
  }

  if (match_tok(p, TOKEN_FUN)) {
    Token start = *previous_tok(p);
    consume_tok(p, TOKEN_LPAREN, "expected '(' after 'fun' in type");

    PLIST(TypeNode *, param_types, 8);
    bool had_error = false;

    if (!check_tok(p, TOKEN_RPAREN)) {
      do {
        TypeNode *ty = parse_type(p);
        if (ty->kind == TYNODE_POISON) {
          had_error = true;
          break;
        }
        PLIST_PUSH(p, param_types, ty);
      } while (match_tok(p, TOKEN_COMMA));
    }

    if (!consume_tok(p, TOKEN_RPAREN,
                     "expected ')' after function type parameters")) {
      had_error = true;
    }

    if (!match_tok(p, TOKEN_THIN_ARROW)) {
      error_at(p, current_tok_span(p),
               "expected '->' after function type parameters");
      had_error = true;
    }

    TypeNode *return_type = parse_type(p);
    if (return_type->kind == TYNODE_POISON) {
      had_error = true;
    }

    if (had_error) {
      return ast_type_node(TYNODE_POISON,
                           span_merge(token_span(&start), previous_tok_span(p)),
                           p->al);
    }

    base = ast_type_node(TYNODE_FUN,
                         span_merge(token_span(&start), previous_tok_span(p)),
                         p->al);

    base->as.fun.param_count = param_types_count;
    base->as.fun.param_types = PLIST_TAKE(p, param_types);
    base->as.fun.return_type = return_type;
  }

  if (match_tok(p, TOKEN_LBRACKET)) {
    Token start = *previous_tok(p);

    TypeNode *elem = parse_type(p);
    if (elem->kind == TYNODE_POISON) {
      return ast_type_node(TYNODE_POISON, current_tok_span(p), p->al);
    }

    if (!consume_tok(p, TOKEN_RBRACKET, "expected ']' after array type")) {
      return ast_type_node(TYNODE_POISON, current_tok_span(p), p->al);
    }

    base = ast_type_node(TYNODE_ARRAY,
                         span_merge(token_span(&start), previous_tok_span(p)),
                         p->al);
    base->as.array.elem = elem;
  }

  if (match_tok(p, TOKEN_SELF_TYPE)) {
    base = ast_type_node(TYNODE_SELF, token_span(previous_tok(p)), p->al);
  }

  // `dyn Trait` — a trait object. The keyword is required: a bare trait name
  // in type position stays the *bound* spelling (`impl Show for T`), so the
  // two dispatch strategies are told apart in the source, not inferred.
  //
  // `dyn Iterator<Item = int>` pins each associated type the trait declares,
  // and `dyn Into<int>` supplies the trait's own type parameter. The path is
  // parsed *bare* so the '<' lands here rather than in `parse_type_args`: what
  // follows is one bracket list holding both, and reading it as a plain
  // type-argument list would report "expected '>'" at the `=`.
  if (match_tok(p, TOKEN_DYN)) {
    Token start = *previous_tok(p);

    Path path = {0};
    if (!parse_path(p, PATH_BARE, &path)) {
      return ast_type_node(TYNODE_POISON, current_tok_span(p), p->al);
    }

    TypeNode **type_args = NULL;
    AssocBindingNode *bindings = NULL;
    int type_arg_count = 0, binding_count = 0;
    if (check_tok(p, TOKEN_LT) &&
        !parse_dyn_args(p, &type_args, &type_arg_count, &bindings,
                        &binding_count)) {
      return ast_type_node(TYNODE_POISON, current_tok_span(p), p->al);
    }

    base = ast_type_node(TYNODE_DYN,
                         span_merge(token_span(&start), previous_tok_span(p)),
                         p->al);
    base->as.dyn.path = path;
    base->as.dyn.type_args = type_args;
    base->as.dyn.type_arg_count = type_arg_count;
    base->as.dyn.bindings = bindings;
    base->as.dyn.binding_count = binding_count;
  }

  if (check_tok(p, TOKEN_IDENT)) {
    Token start = *current_tok(p);

    Path path = {0};
    if (!parse_path(p, PATH_TYPE, &path)) {
      return ast_type_node(TYNODE_POISON, current_tok_span(p), p->al);
    }

    base = ast_type_node(TYNODE_NAMED,
                         span_merge(token_span(&start), previous_tok_span(p)),
                         p->al);
    base->as.named.path = path;
  }

  if (match_tok(p, TOKEN_DOT)) {
    do {
      if (!consume_tok(p, TOKEN_IDENT,
                       "expected associated type name after '.'")) {
        return ast_type_node(TYNODE_POISON, current_tok_span(p), p->al);
      }
      Token name = *previous_tok(p);

      TypeNode *assoc = ast_type_node(
          TYNODE_ASSOC, span_merge(base->span, token_span(&name)), p->al);
      assoc->as.assoc.base = base;
      assoc->as.assoc.assoc_name = name.lexeme;
      base = assoc;
    } while (match_tok(p, TOKEN_DOT));
  }

  if (base) {
    return base;
  }

  error_at(p, current_tok_span(p), "expected type");
  return ast_type_node(TYNODE_POISON, current_tok_span(p), p->al);
}

static bool parse_dyn_args(Parser *p, TypeNode ***out_args, int *out_arg_count,
                           AssocBindingNode **out_bindings,
                           int *out_binding_count) {
  if (!consume_tok(p, TOKEN_LT, "expected '<' after the trait's name")) {
    return false;
  }

  PLIST(TypeNode *, args, MAX_ASSOC_BINDINGS);
  PLIST(AssocBindingNode, bindings, MAX_ASSOC_BINDINGS);

  do {
    // an entry is a binding when it is `Name =`, and a type argument
    // otherwise. Two tokens of lookahead decide it, and nothing else has to:
    // a type argument may be any type, including a bare name.
    if (check_tok(p, TOKEN_IDENT) && peek_ahead(p, 1)->type == TOKEN_EQ) {
      Token name = *current_tok(p);
      advance_tok(p);
      advance_tok(p);

      TypeNode *ty = parse_type(p);
      if (ty->kind == TYNODE_POISON) {
        return false;
      }
      PLIST_PUSH(
          p, bindings,
          ((AssocBindingNode){
              .name = name.lexeme,
              .type = ty,
              .span = span_merge(token_span(&name), previous_tok_span(p)),
          }));
      continue;
    }

    // a type argument after a binding would leave the two lists interleaved,
    // and the trait's parameters are positional — so order is not a style
    // question here.
    if (bindings_count > 0) {
      error_at(p, current_tok_span(p),
               "a trait's type arguments come before its associated-type "
               "bindings, as in 'dyn Trait<int, Item = string>'");
      return false;
    }

    TypeNode *ty = parse_type(p);
    if (ty->kind == TYNODE_POISON) {
      return false;
    }
    PLIST_PUSH(p, args, ty);
  } while (match_tok(p, TOKEN_COMMA));

  if (!consume_tok(p, TOKEN_GT, "expected '>' after the trait's arguments")) {
    return false;
  }

  *out_args = PLIST_TAKE(p, args);
  *out_arg_count = args_count;
  *out_bindings = PLIST_TAKE(p, bindings);
  *out_binding_count = bindings_count;
  return true;
}

static int parse_type_args(Parser *p, TypeNode ***out_args);

static bool parse_path(Parser *p, PathParseMode mode, Path *out) {
  PLIST(PathSegment, segments, 8);
  Token *start_tok = current_tok(p);

  if (check_tok(p, TOKEN_IDENT) || check_tok(p, TOKEN_SELF_TYPE)) {
    do {
      // a use path stops before the brace group or the glob that follows it:
      // `a::b::{X, Y}`, `a::E::*`. Neither is a segment.
      if (mode == PATH_USE &&
          (check_tok(p, TOKEN_LBRACE) || check_tok(p, TOKEN_STAR))) {
        break;
      }
      // 'Self' may only lead a path; later segments must be plain idents.
      if (segments_count == 0 && check_tok(p, TOKEN_SELF_TYPE)) {
        advance_tok(p);
      } else if (!consume_tok(p, TOKEN_IDENT, "expected identifier in path")) {
        return false;
      }
      PLIST_GROW(p, segments);
      PathSegment *segment = &segments[segments_count++];
      *segment = (PathSegment){
          .name = previous_tok(p)->lexeme,
          .type_arg_count = 0,
          .type_args = NULL,
      };

      if (mode == PATH_EXPR) {
        // expr requires '::' before type args
        if (check_tok(p, TOKEN_COLONCOLON) &&
            peek_ahead(p, 1)->type == TOKEN_LT) {
          advance_tok(p); // consume '::'
          segment->type_arg_count = parse_type_args(p, &segment->type_args);
          if (segment->type_arg_count < 0) {
            return false;
          }
        }
      } else if (mode != PATH_BARE) {
        // '::' is optional
        if (check_tok(p, TOKEN_COLONCOLON) &&
            peek_ahead(p, 1)->type == TOKEN_LT) {
          advance_tok(p);
        }

        if (check_tok(p, TOKEN_LT)) {
          segment->type_arg_count = parse_type_args(p, &segment->type_args);
          if (segment->type_arg_count < 0) {
            return false;
          }
        }
      }
    } while (match_tok(p, TOKEN_COLONCOLON));
  }

  assert(out);
  out->segments = PLIST_TAKE(p, segments);
  out->count = segments_count;
  out->span = span_merge(token_span(start_tok), previous_tok_span(p));

  return true;
}

static Expr *parse_assign(Parser *p);
static Expr *parse_range(Parser *p);
static Expr *parse_or(Parser *p);
static Expr *parse_and(Parser *p);
static Expr *parse_equality(Parser *p);
static Expr *parse_comparison(Parser *p);
static Expr *parse_bitor(Parser *p);
static Expr *parse_bitxor(Parser *p);
static Expr *parse_bitand(Parser *p);
static Expr *parse_shift(Parser *p);
static Expr *parse_addition(Parser *p);
static Expr *parse_multiply(Parser *p);
static Expr *parse_unary(Parser *p);
static Expr *parse_postfix(Parser *p);
static Expr *parse_primary(Parser *p);
static bool at_shift_assign(const Parser *p, TokenType *op, int *width);

static Expr *parse_expr(Parser *p) { return parse_assign(p); }

static Expr *parse_assign(Parser *p) {
  Expr *lhs = parse_range(p);
  if (lhs->kind == EXPR_POISON) {
    return lhs;
  }

  TokenType op;
  int width;
  if (at_shift_assign(p, &op, &width)) {
    // a run rather than a token, so it is consumed by width; every level
    // below has already declined it for exactly that reason.
    p->current += width;
  } else if (match_tok(p, TOKEN_EQ) || match_tok(p, TOKEN_PLUSEQ) ||
             match_tok(p, TOKEN_MINUSEQ) || match_tok(p, TOKEN_STAREQ) ||
             match_tok(p, TOKEN_SLASHEQ) || match_tok(p, TOKEN_PERCENTEQ) ||
             match_tok(p, TOKEN_AMPEQ) || match_tok(p, TOKEN_PIPEEQ) ||
             match_tok(p, TOKEN_CARETEQ)) {
    op = previous_tok(p)->type;
  } else {
    return lhs;
  }

  Expr *rhs = parse_assign(p);
  if (rhs->kind == EXPR_POISON) {
    return rhs;
  }

  Expr *e = ast_expr(EXPR_ASSIGN, span_merge(lhs->span, rhs->span), p->al);
  e->as.assign.target = lhs;
  e->as.assign.value = rhs;
  e->as.assign.op = op;
  return e;
}

static Expr *parse_range(Parser *p) {
  Expr *lhs = parse_or(p);
  if (lhs->kind == EXPR_POISON) {
    return lhs;
  }

  if (match_tok(p, TOKEN_DOTDOT) || match_tok(p, TOKEN_DOTDOTEQ)) {
    Token *op = previous_tok(p);
    Expr *rhs = parse_or(p);
    if (rhs->kind == EXPR_POISON) {
      return rhs;
    }

    Expr *e = ast_expr(EXPR_RANGE, span_merge(lhs->span, rhs->span), p->al);
    e->as.range.start = lhs;
    e->as.range.end = rhs;
    e->as.range.inclusive = op->type == TOKEN_DOTDOTEQ;
    return e;
  }

  return lhs;
}

static Expr *parse_binary_left(Parser *p, Expr *(*sub)(Parser *),
                               const TokenType *ops) {
  Expr *lhs = sub(p);
  while (true) {
    bool matched = false;
    for (int i = 0; ops[i]; i++) {
      // The one token that can look like an operator here and not be one: the
      // leading angle of a compound shift, since `a >>= b` opens with the same
      // `>` that `a > b` does. `parse_comparison` is the level this bites.
      if ((ops[i] == TOKEN_LT || ops[i] == TOKEN_GT) &&
          at_shift_assign(p, NULL, NULL)) {
        continue;
      }
      if (!match_tok(p, ops[i])) {
        continue;
      }
      Token *op = previous_tok(p);
      Expr *rhs = sub(p);
      Span span = span_merge(lhs->span, rhs->span);
      Expr *expr = ast_expr(EXPR_BINARY, span, p->al);
      expr->as.binary.left = lhs;
      expr->as.binary.right = rhs;
      expr->as.binary.op = op->type;
      lhs = expr;
      matched = true;
      break;
    }
    if (!matched) {
      break;
    }
  }
  return lhs;
}

static Expr *parse_or(Parser *p) {
  static const TokenType ops[] = {TOKEN_OR, 0};
  return parse_binary_left(p, parse_and, ops);
}

static Expr *parse_and(Parser *p) {
  static const TokenType ops[] = {TOKEN_AND, 0};
  return parse_binary_left(p, parse_equality, ops);
}

static Expr *parse_equality(Parser *p) {
  static const TokenType ops[] = {TOKEN_EQEQ, TOKEN_BANGEQ, 0};
  return parse_binary_left(p, parse_comparison, ops);
}

static Expr *parse_comparison(Parser *p) {
  static const TokenType ops[] = {TOKEN_LT, TOKEN_LTEQ, TOKEN_GT, TOKEN_GTEQ,
                                  0};
  return parse_binary_left(p, parse_bitor, ops);
}

// ── the bitwise levels
// ────────────────────────────────────────────────────────
//
// `|`, then `^`, then `&`, then the shifts, sitting between comparison and
// addition. That is Rust's ordering rather than C's, and the difference is the
// one that matters: C puts `&` *below* `==`, so `a & b == c` means
// `a & (b == c)` — the classic trap that has needed parentheses in C for fifty
// years. Here comparison is lower than every bitwise operator, so it reads the
// way it looks. Shifts bind tighter than `&` and looser than `+`, so
// `a + b << c` is `(a + b) << c`; that half *is* C's ordering and is the one
// part of it worth keeping, since a shift is a scaling step in the arithmetic
// around it.
static Expr *parse_bitor(Parser *p) {
  static const TokenType ops[] = {TOKEN_PIPE, 0};
  return parse_binary_left(p, parse_bitxor, ops);
}

static Expr *parse_bitxor(Parser *p) {
  static const TokenType ops[] = {TOKEN_CARET, 0};
  return parse_binary_left(p, parse_bitand, ops);
}

static Expr *parse_bitand(Parser *p) {
  static const TokenType ops[] = {TOKEN_AMP, 0};
  return parse_binary_left(p, parse_shift, ops);
}

// Whether the token at `i` is followed immediately by the next one, with no
// whitespace between. `col` is the column a token starts at and `lexeme.len`
// is how wide it is, so adjacency is arithmetic.
static bool tok_touches_next(const Parser *p, int i) {
  if (i + 1 >= p->count) {
    return false;
  }
  const Token *a = &p->tokens[i];
  const Token *b = &p->tokens[i + 1];
  return a->line == b->line && b->col == a->col + a->lexeme.len;
}

// Does a run of `n` copies of `type` start here, each touching the next?
static bool run_of(const Parser *p, TokenType type, int n) {
  for (int i = 0; i < n; i++) {
    if (p->current + i >= p->count || p->tokens[p->current + i].type != type) {
      return false;
    }
    if (i + 1 < n && !tok_touches_next(p, p->current + i)) {
      return false;
    }
  }
  return true;
}

// A run of `n` tokens whose first `n - 1` are `angle` and whose last is
// `angle_eq`, each touching the next. That is the shape a compound shift has:
// the scanner fuses `=` onto the angle it follows, so `>>=` arrives as
// `>` `>=` and `>>>=` as `>` `>` `>=`.
static bool run_then_eq(const Parser *p, TokenType angle, TokenType angle_eq,
                        int n) {
  if (!run_of(p, angle, n - 1)) {
    return false;
  }
  int last = p->current + n - 1;
  return last < p->count && p->tokens[last].type == angle_eq &&
         tok_touches_next(p, last - 1);
}

// `<<=`, `>>=` and `>>>=`. Their operand type is fixed and their place is a
// place, so nothing about them is ambiguous once the run is *seen* — the whole
// difficulty is that the run starts with a token two lower levels also want,
// and assignment is the lowest level of all, so it cannot simply match first.
// Hence a predicate the levels in between consult in order to decline.
static bool at_shift_assign(const Parser *p, TokenType *op, int *width) {
  TokenType found;
  int n;
  // These three are mutually exclusive, so unlike `parse_shift` below there is
  // no longest-match rule to get right: a run's last token is the `=`-carrying
  // one, which pins its length. The fused `=` removes exactly the ambiguity
  // `>>>` has with `>>`.
  if (run_then_eq(p, TOKEN_GT, TOKEN_GTEQ, 3)) {
    found = TOKEN_USHREQ;
    n = 3;
  } else if (run_then_eq(p, TOKEN_GT, TOKEN_GTEQ, 2)) {
    found = TOKEN_SHREQ;
    n = 2;
  } else if (run_then_eq(p, TOKEN_LT, TOKEN_LTEQ, 2)) {
    found = TOKEN_SHLEQ;
    n = 2;
  } else {
    return false;
  }
  if (op != NULL) {
    *op = found;
  }
  if (width != NULL) {
    *width = n;
  }
  return true;
}

// `<<`, `>>` and `>>>` are runs of adjacent angle brackets rather than tokens
// of their own. The scanner cannot fuse them: a nested generic closes with a
// run of `>` (`Map<K, Vec<V>>`), and `parse_type` consumes those one at a time,
// so a `>>` token would break every type that nests. Fusing here instead costs
// one adjacency test and leaves types untouched — and because this level sits
// *below* comparison, the run is claimed before `parse_comparison` ever sees a
// `>` to read as an operator.
//
// The adjacency test is what keeps `a > > b` from being a shift, and it is the
// only place in the grammar where whitespace changes a parse.
static Expr *parse_shift(Parser *p) {
  Expr *lhs = parse_addition(p);
  while (true) {
    TokenType op;
    int width;
    // `a >>>= b` opens with two adjacent `>` and so would fuse as a signed
    // shift here, leaving `>=` stranded. The compound run is the assignment's.
    if (at_shift_assign(p, NULL, NULL)) {
      break;
    }
    // `>>>` before `>>`: the longer run has to win, or every unsigned shift
    // would parse as a signed one followed by a stray `>`.
    if (run_of(p, TOKEN_GT, 3)) {
      op = TOKEN_USHR;
      width = 3;
    } else if (run_of(p, TOKEN_GT, 2)) {
      op = TOKEN_SHR;
      width = 2;
    } else if (run_of(p, TOKEN_LT, 2)) {
      op = TOKEN_SHL;
      width = 2;
    } else {
      break;
    }

    Span start = current_tok_span(p);
    p->current += width;
    Expr *rhs = parse_addition(p);
    Span span = span_merge(span_merge(lhs->span, start), rhs->span);
    Expr *expr = ast_expr(EXPR_BINARY, span, p->al);
    expr->as.binary.left = lhs;
    expr->as.binary.right = rhs;
    expr->as.binary.op = op;
    lhs = expr;
  }
  return lhs;
}

static Expr *parse_addition(Parser *p) {
  static const TokenType ops[] = {TOKEN_PLUS, TOKEN_MINUS, 0};
  return parse_binary_left(p, parse_multiply, ops);
}

static Expr *parse_multiply(Parser *p) {
  static const TokenType ops[] = {TOKEN_STAR, TOKEN_SLASH, TOKEN_PERCENT, 0};
  return parse_binary_left(p, parse_unary, ops);
}

static Expr *parse_unary(Parser *p) {
  if (match_tok(p, TOKEN_NOT) || match_tok(p, TOKEN_MINUS) ||
      match_tok(p, TOKEN_TILDE)) {
    Token *op = previous_tok(p);
    Expr *operand = parse_unary(p);
    Span span = span_merge(token_span(op), operand->span);
    Expr *expr = ast_expr(EXPR_UNARY, span, p->al);
    expr->as.unary.op = op->type;
    expr->as.unary.operand = operand;
    return expr;
  }
  return parse_postfix(p);
}

static Expr *parse_postfix(Parser *p) {
  Expr *base = parse_primary(p);
  while (true) {
    if (match_tok(p, TOKEN_LPAREN)) {
      PLIST(Expr *, args, 8);
      if (!check_tok(p, TOKEN_RPAREN)) {
        do {
          PLIST_PUSH(p, args, parse_expr(p));
        } while (match_tok(p, TOKEN_COMMA));
      }
      consume_tok(p, TOKEN_RPAREN, "expected ')' after arguments");
      Span span = span_merge(base->span, previous_tok_span(p));
      Expr *expr = ast_expr(EXPR_CALL, span, p->al);
      expr->as.call.callee = base;
      expr->as.call.args = PLIST_TAKE(p, args);
      expr->as.call.arg_count = args_count;
      base = expr;
    } else if (match_tok(p, TOKEN_DOT)) {
      if (match_tok(p, TOKEN_IDENT)) {
        Token name = *previous_tok(p);

        // A method call's type arguments are spelled with the turbofish, the
        // same `::<..>` a path already requires (`Point::<int>::new`). The
        // brackets are the only ones in the expression grammar that could have
        // been read as operators — `it.fold<int>(0, f)` against `self.at < n`
        // — and the `::` is what settles it before either side is parsed, so
        // the `<` after a field or method access is now unconditionally the
        // less-than operator and `a.b < c > (d)` is two comparisons.
        TypeNode **type_args = NULL;
        int type_arg_count = 0;
        Span start_span = current_tok_span(p);

        if (check_tok(p, TOKEN_COLONCOLON) &&
            peek_ahead(p, 1)->type == TOKEN_LT) {
          advance_tok(p); // consume '::'
          type_arg_count = parse_type_args(p, &type_args);
          if (type_arg_count <= 0) {
            return ast_expr(EXPR_POISON, current_tok_span(p), p->al);
          }
        }

        if (type_arg_count != 0 && !check_tok(p, TOKEN_LPAREN)) {
          error_at(p, span_merge(start_span, previous_tok_span(p)),
                   "unexpected type arguments");
        }

        if (match_tok(p, TOKEN_LPAREN)) {
          // method call: obj.method(args)
          //
          // This list is the one that had no check at all — a bare `tmp[64]`
          // written through by `tmp[argc++]`, so the 65th argument corrupted
          // the stack rather than reporting anything. Every sibling list in
          // this file at least refused; this one did not, and the distro's
          // stack protector was all that stood between it and the return
          // address.
          PLIST(Expr *, args, 8);
          if (!check_tok(p, TOKEN_RPAREN)) {
            do {
              PLIST_PUSH(p, args, parse_expr(p));
            } while (match_tok(p, TOKEN_COMMA) && !check_tok(p, TOKEN_RPAREN));
          }
          consume_tok(p, TOKEN_RPAREN, "expected ')' after arguments");
          Expr *expr =
              ast_expr(EXPR_METHOD_CALL,
                       span_merge(base->span, previous_tok_span(p)), p->al);

          expr->as.method_call.object = base;
          expr->as.method_call.method_name = name.lexeme;
          expr->as.method_call.args = PLIST_TAKE(p, args);
          expr->as.method_call.arg_count = args_count;
          expr->as.method_call.type_args = type_args;
          expr->as.method_call.type_arg_count = type_arg_count;
          expr->as.method_call.resolved_method = NULL;
          base = expr;
        } else {
          // field access: obj.field
          Expr *expr = ast_expr(
              EXPR_FIELD, span_merge(base->span, previous_tok_span(p)), p->al);
          expr->as.field.object = base;
          expr->as.field.ident.name = name.lexeme;
          expr->as.field.resolved_index = -1;
          base = expr;
        }
      } else if (match_tok(p, TOKEN_INT)) {
        // tuple struct constructor: Point.0, Point.1, etc.
        Token idx_tok = *previous_tok(p);
        int idx = atoi(idx_tok.lexeme.chars);
        Expr *expr = ast_expr(
            EXPR_FIELD, span_merge(base->span, previous_tok_span(p)), p->al);
        expr->as.field.object = base;
        expr->as.field.is_tuple = true;
        expr->as.field.ident.index = idx;
        expr->as.field.resolved_index = -1;
        base = expr;
      } else {
        error_at(p, current_tok_span(p),
                 "expected field name or tuple index after '.'");
        return ast_expr(EXPR_POISON, current_tok_span(p), p->al);
      }
    } else if (match_tok(p, TOKEN_LBRACKET)) {
      // Index: arr[i]
      Expr *idx = parse_expr(p);
      consume_tok(p, TOKEN_RBRACKET, "expected ']'");
      Expr *expr = ast_expr(
          EXPR_INDEX, span_merge(base->span, previous_tok_span(p)), p->al);
      expr->as.index.object = base;
      expr->as.index.index = idx;
      base = expr;

    } else if (match_tok(p, TOKEN_QUESTION)) {
      // Propagate: expr?
      Expr *expr = ast_expr(
          EXPR_PROPAGATE, span_merge(base->span, previous_tok_span(p)), p->al);
      expr->as.propagate.operand = base;
      base = expr;

    } else if (match_tok(p, TOKEN_AS)) {
      // cast: `expr as Type`, or the fallible `expr as? Type` — a downcast of
      // a trait object, whose result is an `Option`. No type starts with `?`,
      // so the two never have to be told apart by more than this peek.
      bool fallible = match_tok(p, TOKEN_QUESTION);
      TypeNode *target = parse_type(p);
      Expr *expr = ast_expr(
          EXPR_CAST, span_merge(base->span, previous_tok_span(p)), p->al);
      expr->as.cast.operand = base;
      expr->as.cast.target_type = target;
      expr->as.cast.fallible = fallible;
      base = expr;

    } else {
      break;
    }
  }
  return base;
}

static Stmt *parse_stmt(Parser *p);

static bool is_pure_stmt(Parser *p) {
  return check_tok(p, TOKEN_VAR) || check_tok(p, TOKEN_RETURN) ||
         check_tok(p, TOKEN_BREAK) || check_tok(p, TOKEN_CONTINUE) ||
         check_tok(p, TOKEN_IF) || check_tok(p, TOKEN_FOR) ||
         check_tok(p, TOKEN_MATCH) || check_tok(p, TOKEN_WHILE) ||
         check_tok(p, TOKEN_LOOP) || check_tok(p, TOKEN_LABEL);
}

// One token, and the caller decides what may legally follow it: a `:` and a
// loop where it declares a name, nothing at all where it uses one.
static LoopLabel parse_opt_label(Parser *p) {
  if (!check_tok(p, TOKEN_LABEL)) {
    return (LoopLabel){0};
  }
  Token *tok = peek_tok(p);
  advance_tok(p);
  return (LoopLabel){.name = tok->lexeme, .span = token_span(tok)};
}

static Expr *parse_block(Parser *p) {
  if (!consume_tok(p, TOKEN_LBRACE, "expected '{'")) {
    return ast_expr(EXPR_POISON, current_tok_span(p), p->al);
  };
  Span start_span = previous_tok_span(p);

  PLIST(Stmt *, stmts, 8);
  Expr *tail = NULL;
  bool block_had_error = false;

  while (!check_tok(p, TOKEN_RBRACE) && !is_at_end(p)) {
    if (tail) {
      // something came after the tail expr — error, but keep parsing
      error_at(p, current_tok_span(p), "expected '}' after block expression");
      block_had_error = true;
    }

    if (is_pure_stmt(p)) {
      Stmt *stmt = parse_stmt(p);
      if (stmt->kind == STMT_POISON) {
        block_had_error = true;
      } else if (stmt->kind == STMT_EXPR && check_tok(p, TOKEN_RBRACE) &&
                 (stmt->as.expr_stmt.expr->kind == EXPR_IF ||
                  stmt->as.expr_stmt.expr->kind == EXPR_MATCH ||
                  stmt->as.expr_stmt.expr->kind == EXPR_LOOP ||
                  stmt->as.expr_stmt.expr->kind == EXPR_BLOCK)) {
        // a value-bearing block expression just before `}` is the tail. A
        // `loop` is one of those and a `while`/`for` is not: only its breaks
        // leave it, so only it has something other than `()` to be. The only
        // block that can arrive here is a *labelled* one — an unlabelled `{`
        // is no statement head, so it reaches the tail through parse_expr.
        tail = stmt->as.expr_stmt.expr;
      } else {
        PLIST_PUSH(p, stmts, stmt);
      }
    } else {
      Expr *expr = parse_expr(p);
      if (expr->kind == EXPR_POISON) {
        advance_tok(p);
        block_had_error = true;
      } else if (match_tok(p, TOKEN_SEMICOLON)) {
        Stmt *stmt = ast_stmt(STMT_EXPR, expr->span, p->al);
        stmt->as.expr_stmt.expr = expr;
        PLIST_PUSH(p, stmts, stmt);
      } else {
        tail = expr;
      }
    }

    if (p->panic_mode) {
      sync_to_stmt(p);
    }
  }

  if (!consume_tok(p, TOKEN_RBRACE, "expected '}'")) {
    return ast_expr(EXPR_POISON, current_tok_span(p), p->al);
  }

  if (block_had_error) {
    return ast_expr(EXPR_POISON, span_merge(start_span, previous_tok_span(p)),
                    p->al);
  }

  Expr *expr =
      ast_expr(EXPR_BLOCK, span_merge(start_span, previous_tok_span(p)), p->al);
  expr->as.block.stmts = PLIST_TAKE(p, stmts);
  expr->as.block.stmt_count = stmts_count;
  expr->as.block.tail_expr = tail;
  return expr;
}

static int parse_type_args(Parser *p, TypeNode ***out_args) {
  PLIST(TypeNode *, args, 8);
  bool had_error = false;

  if (!consume_tok(p, TOKEN_LT, "expected '<' before type arguments")) {
    had_error = true;
  } else if (check_tok(p, TOKEN_GT)) {
    // `::<>` names nothing. Consume the bracket before reporting: leaving it
    // behind used to hand the `>` back to the expression grammar as an
    // operator, so the real complaint arrived several tokens later.
    Span open = previous_tok_span(p);
    advance_tok(p);
    error_at(p, span_merge(open, previous_tok_span(p)),
             "expected at least one type argument between '<' and '>'");
    had_error = true;
  } else {
    do {
      TypeNode *ty = parse_type(p);
      if (ty->kind == TYNODE_POISON) {
        had_error = true;
        break;
      }
      PLIST_PUSH(p, args, ty);
    } while (match_tok(p, TOKEN_COMMA));

    if (!consume_tok(p, TOKEN_GT, "expected '>' after type arguments")) {
      had_error = true;
    }
  }

  if (had_error) {
    return 0;
  }

  *out_args = PLIST_TAKE(p, args);
  return args_count;
}

// The `<..>` after a trait's name is the same bracket in a bound as in a
// `dyn`, and now the same parser: `Iterator<Item = I.Item>` carries the trait's
// type arguments and its associated-type bindings in one list. So the path is
// read in PATH_BARE mode — leaving the `<` to us — and the type arguments are
// written back into its last segment, where trait_ref_resolve already reads
// them. Reading it as a plain type-argument list would report "expected '>'" at
// the `=`, which is exactly why the `dyn` side got its own parser first.
static bool parse_trait_ref(Parser *p, TraitRef *out) {
  if (!check_tok(p, TOKEN_IDENT)) {
    error_at(p, current_tok_span(p), "expected trait name in trait bound");
    return false;
  }
  Token *start_tok = previous_tok(p);

  if (!parse_path(p, PATH_BARE, &out->path)) {
    return false;
  }

  out->bindings = NULL;
  out->binding_count = 0;

  // `Into::<int>` spells the arguments the way an expression path does; a
  // bound accepts both, as PATH_TYPE did before this took the brackets over.
  if (check_tok(p, TOKEN_COLONCOLON) && peek_ahead(p, 1)->type == TOKEN_LT) {
    advance_tok(p);
  }
  if (check_tok(p, TOKEN_LT) && out->path.count > 0) {
    TypeNode **type_args = NULL;
    AssocBindingNode *bindings = NULL;
    int type_arg_count = 0, binding_count = 0;
    if (!parse_dyn_args(p, &type_args, &type_arg_count, &bindings,
                        &binding_count)) {
      return false;
    }
    PathSegment *last = &out->path.segments[out->path.count - 1];
    last->type_args = type_args;
    last->type_arg_count = type_arg_count;

    out->bindings = bindings;
    out->binding_count = binding_count;
  }

  out->span = span_merge(token_span(start_tok), previous_tok_span(p));

  return true;
}

// `allow_self` says whether `Self` may head a predicate. It may in a trait
// method's clause, where `where Self.Item: Ord` is the whole point — and
// nowhere else: on a `fun` or an `impl`, `Self` names no type parameter, so
// the predicate would match nothing and be dropped without a word. Refusing it
// here is what keeps that from being a silent no-op.
static bool parse_bound_lhs(Parser *p, WhereLhs *out, bool allow_self) {
  if (check_tok(p, TOKEN_SELF_TYPE) && !allow_self) {
    error_at(p, current_tok_span(p),
             "'Self' may only be bound in a trait method's 'where' clause");
    return false;
  }
  // `Self` is a keyword rather than an identifier, so it needs its own match.
  if (!match_tok(p, TOKEN_SELF_TYPE) &&
      !consume_tok(p, TOKEN_IDENT,
                   "expected type parameter name in where clause predicate")) {
    return false;
  }
  Span start = previous_tok_span(p);
  bool had_error = false;

  PLIST(StringView, segments, 4);
  PLIST_PUSH(p, segments, previous_tok(p)->lexeme);

  if (check_tok(p, TOKEN_DOT)) {
    do {
      if (!consume_tok(p, TOKEN_DOT,
                       "expected '.' in where clause predicate")) {
        had_error = true;
        break;
      }
      if (!consume_tok(
              p, TOKEN_IDENT,
              "expected identifier after '.' in where clause predicate")) {
        had_error = true;
        break;
      }
      PLIST_PUSH(p, segments, previous_tok(p)->lexeme);
    } while (check_tok(p, TOKEN_DOT));
  }
  if (had_error) {
    return false;
  }
  out->segment_count = segments_count;
  out->segments = PLIST_TAKE(p, segments);
  // the whole path, so a diagnostic about `T.Item` underlines both segments.
  // Until milestone 52 nothing reachable read this span, so it went unset.
  out->span = span_merge(start, previous_tok_span(p));
  return true;
}

static bool parse_trait_bound(Parser *p, TraitBound *out) {
  PLIST(TraitRef, refs, 4);
  bool had_error = false;

  do {
    PLIST_GROW(p, refs);
    had_error = had_error || !parse_trait_ref(p, &refs[refs_count++]);
  } while (!had_error && match_tok(p, TOKEN_PLUS));

  if (had_error) {
    return false;
  }

  out->ref_count = refs_count;
  out->refs = PLIST_TAKE(p, refs);
  return !had_error;
}

static bool parse_where_clause(Parser *p, WhereClause **out, bool allow_self) {
  if (!consume_tok(p, TOKEN_WHERE, "expected 'where'")) {
    return false;
  }

  Token where_tok = *previous_tok(p);

  PLIST(WherePred, preds, 4);
  bool had_error = false;

  do {
    if (check_tok(p, TOKEN_LBRACE)) {
      break;
    }

    PLIST_GROW(p, preds);
    WherePred *pred = &preds[preds_count++];
    had_error = had_error || !parse_bound_lhs(p, &pred->lhs, allow_self);
    if (had_error) {
      break;
    }

    if (!consume_tok(p, TOKEN_COLON,
                     "expected ':' after type parameter in where clause")) {
      had_error = true;
      break;
    }

    had_error = had_error || !parse_trait_bound(p, &pred->bound);
  } while (!had_error && match_tok(p, TOKEN_COMMA));

  if (had_error) {
    while (!is_at_end(p) && !check_tok(p, TOKEN_LBRACE) &&
           !check_tok(p, TOKEN_SEMICOLON)) {
      advance_tok(p);
    }
    return false;
  }

  *out = al_alloc(p->al, sizeof(WhereClause));
  (*out)->pred_count = preds_count;
  (*out)->preds = PLIST_TAKE(p, preds);

  (*out)->span = span_merge(token_span(&where_tok), current_tok_span(p));

  return true;
}

static int parse_type_params(Parser *p, TypeParamNode **out) {
  if (!consume_tok(p, TOKEN_LT, "expected '<'"))
    return -1;

  PLIST(TypeParamNode, params, 4);
  bool had_error = false;

  do {
    if (!consume_tok(p, TOKEN_IDENT, "expected type parameter name")) {
      had_error = true;
      break;
    }

    Token name_tok = *previous_tok(p);
    PLIST_GROW(p, params);
    TypeParamNode *param = &params[params_count++];
    param->name = name_tok.lexeme;
    // param->span = token_span(&name_tok);

    if (match_tok(p, TOKEN_COLON)) {
      had_error = had_error || !parse_trait_bound(p, &param->inline_bound);
    } else {
      param->inline_bound = (TraitBound){.ref_count = 0, .refs = NULL};
    }
    // `<Rhs = Self>` — after the bounds, so `<T: Ord = int>` reads the way it
    // is written. Accepted here for every declaration and rejected in sema for
    // all but a trait, which is where the position is known rather than the
    // syntax.
    param->default_type = NULL;
    if (!had_error && match_tok(p, TOKEN_EQ)) {
      param->default_type = parse_type(p);
      had_error = had_error || param->default_type == NULL;
    }
    param->span = span_merge(token_span(&name_tok), current_tok_span(p));
  } while (!had_error && match_tok(p, TOKEN_COMMA));

  if (had_error) {
    while (!is_at_end(p) && !check_tok(p, TOKEN_GT)) {
      advance_tok(p);
    }
  }

  consume_tok(p, TOKEN_GT, "expected '>'");
  if (had_error) {
    return -1;
  }

  *out = PLIST_TAKE(p, params);
  return params_count;
}

static void sync_to_fun_body(Parser *p) {
  // p->panic_mode = false;
  while (!is_at_end(p)) {
    switch (peek_tok(p)->type) {
    case TOKEN_LBRACE:
    case TOKEN_FAT_ARROW:
      return;
    default:
      advance_tok(p);
    }
  }
}

static ClosureParam parse_closure_param(Parser *p) {
  ClosureParam param = {0};

  if (match_tok(p, TOKEN_SELF)) {
    param.is_self = true;
    param.span = previous_tok_span(p);
    return param;
  }

  if (!consume_tok(p, TOKEN_IDENT, "expected parameter name")) {
    param.span = current_tok_span(p);
    return param;
  }
  Token name_tok = *previous_tok(p);

  TypeNode *ty = NULL;
  if (match_tok(p, TOKEN_COLON)) {
    ty = parse_type(p);
    if (ty->kind == TYNODE_POISON) {
      param.span = token_span(&name_tok);
      return param;
    }
  }

  param.name = name_tok.lexeme;
  param.type_annotation = ty;
  param.span =
      span_merge(token_span(&name_tok), ty ? ty->span : token_span(&name_tok));
  return param;
}

static bool parse_closure_param_list(Parser *p, ClosureParam **out,
                                     int *count) {
  PLIST(ClosureParam, params, 8);
  bool had_error = false;

  if (!check_tok(p, TOKEN_PIPE)) {
    do {
      if (check_tok(p, TOKEN_PIPE))
        break;

      ClosureParam param = parse_closure_param(p);
      if (param.name.len == 0 && !param.is_self) {
        had_error = true;
        break;
      }
      PLIST_PUSH(p, params, param);

    } while (match_tok(p, TOKEN_COMMA));
  }

  *out = PLIST_TAKE(p, params);
  *count = params_count;
  return !had_error;
}

// closure: | params | ( -> type )? ( expr | { block } )
static Expr *parse_closure(Parser *p) {
  Token start = *previous_tok(p);
  bool had_error = false;

  ClosureParam *params = NULL;
  int param_count = 0;

  if (!parse_closure_param_list(p, &params, &param_count)) {
    had_error = true;
    sync_to_fun_body(p);
  }

  if (!consume_tok(p, TOKEN_PIPE, "expected '|' after closure parameters")) {
    had_error = true;
    sync_to_fun_body(p);
  }

  TypeNode *return_type = NULL;
  if (match_tok(p, TOKEN_THIN_ARROW)) {
    return_type = parse_type(p);
    if (return_type->kind == TYNODE_POISON) {
      had_error = true;
      sync_to_fun_body(p);
    }
  }

  Expr *body = NULL;
  if (check_tok(p, TOKEN_LBRACE)) {
    body = parse_block(p);
  } else {
    body = parse_expr(p);
  }

  assert(body && "function body should not be NULL");
  if (body->kind == EXPR_POISON) {
    had_error = true;
  }

  if (had_error) {
    Span error_span =
        span_merge(token_span(&start), body ? body->span : token_span(&start));
    return ast_expr(EXPR_POISON, error_span, p->al);
  }

  Expr *expr =
      ast_expr(EXPR_CLOSURE,
               span_merge(token_span(&start), previous_tok_span(p)), p->al);
  ExprClosure *closure = &expr->as.closure;
  closure->params = params;
  closure->param_count = param_count;
  closure->return_type_annotation = return_type;
  closure->body = body;
  return expr;
}

static Pattern *parse_pattern(Parser *p) {
  Pattern pattern = {0};
  Span start_span = current_tok_span(p);

  if (match_tok(p, TOKEN_UNDER)) {
    pattern.kind = PAT_WILDCARD;
    pattern.span = previous_tok_span(p);
  } else if (check_tok(p, TOKEN_INT) || check_tok(p, TOKEN_FLOAT) ||
             check_tok(p, TOKEN_STRING) || check_tok(p, TOKEN_CHAR) ||
             check_tok(p, TOKEN_TRUE) || check_tok(p, TOKEN_FALSE)) {
    pattern.kind = PAT_LITERAL;
    pattern.as.literal_expr = parse_primary(p);
    pattern.span = previous_tok_span(p);
  } else if (check_tok(p, TOKEN_IDENT)) {
    Path path = {0};
    if (!parse_path(p, PATH_TYPE, &path)) {
      return NULL;
    }

    if (match_tok(p, TOKEN_LPAREN)) {
      // variant pattern: Enum::Variant(...) or Enum::Variant
      PLIST(Pattern *, subpats, 8);
      bool had_error = false;

      do {
        if (check_tok(p, TOKEN_RPAREN)) {
          break;
        }
        Pattern *subpat = parse_pattern(p);
        if (!subpat) {
          had_error = true;
          break;
        }
        PLIST_PUSH(p, subpats, subpat);
      } while (match_tok(p, TOKEN_COMMA));

      if (!consume_tok(p, TOKEN_RPAREN, "expected ')' after tuple pattern")) {
        return NULL;
      }

      if (!had_error) {
        pattern.kind = PAT_VARIANT;
        pattern.span = span_merge(start_span, previous_tok_span(p));
        pattern.as.variant.path = path;
        pattern.as.variant.fields =
            al_alloc_zero(p->al, sizeof(FieldPat) * subpats_count);
        for (int i = 0; i < subpats_count; i++) {
          pattern.as.variant.fields[i].sub_pattern = subpats[i];
          pattern.as.variant.fields[i].ident.index = i;
        }
        pattern.as.variant.field_count = subpats_count;
      } else {
        return NULL;
      }
    } else if (match_tok(p, TOKEN_LBRACE)) {
      // struct pattern: Struct { field: pat, ... }
      PLIST(FieldPat, fields, 8);
      bool had_error = false;

      do {
        if (check_tok(p, TOKEN_RBRACE)) {
          break;
        }
        if (!consume_tok(p, TOKEN_IDENT,
                         "expected field name in struct pattern")) {
          had_error = true;
          break;
        }
        Span field_span = token_span(previous_tok(p));
        StringView field_name = previous_tok(p)->lexeme;

        Pattern *field_pat = NULL;
        if (match_tok(p, TOKEN_COLON)) {
          field_pat = parse_pattern(p);
          if (!field_pat) {
            had_error = true;
            break;
          }
        }
        PLIST_GROW(p, fields);
        fields[fields_count].ident.name = field_name;
        fields[fields_count].sub_pattern = field_pat;
        fields[fields_count].span = field_span;
        fields_count++;
      } while (match_tok(p, TOKEN_COMMA));

      if (!consume_tok(p, TOKEN_RBRACE, "expected '}' after struct pattern")) {
        return NULL;
      }

      if (!had_error) {
        pattern.kind = PAT_STRUCT;
        pattern.span = span_merge(start_span, previous_tok_span(p));
        pattern.as.struc.fields = PLIST_TAKE(p, fields);
        pattern.as.struc.field_count = fields_count;
        pattern.as.struc.path = path;
      } else {
        return NULL;
      }
    } else if (path.count == 1 && pattern.kind == 0) {
      // variable pattern: just an identifier
      pattern.kind = PAT_BIND;
      pattern.span = path.span;
      pattern.as.bind.name = path.segments[0].name;
    } else {
      pattern.kind = PAT_VARIANT;
      pattern.span = path.span;
      pattern.as.variant.path = path;
    }
  } else if (match_tok(p, TOKEN_LPAREN)) {
    PLIST(Pattern *, subpats, 8);
    bool had_error = false;

    if (!check_tok(p, TOKEN_RPAREN)) {
      do {
        Pattern *subpat = parse_pattern(p);
        if (!subpat) {
          had_error = true;
          break;
        }
        PLIST_PUSH(p, subpats, subpat);
      } while (match_tok(p, TOKEN_COMMA));
    }

    if (!consume_tok(p, TOKEN_RPAREN, "expected ')' after tuple pattern")) {
      return NULL;
    }

    if (!had_error) {
      pattern.kind = PAT_TUPLE;
      pattern.span = span_merge(start_span, previous_tok_span(p));
      pattern.as.tuple.elems = PLIST_TAKE(p, subpats);
      pattern.as.tuple.count = subpats_count;
    } else {
      return NULL;
    }
  } else {
    error_at(p, current_tok_span(p), "expected pattern");
    return NULL;
  }

  Pattern *patt = al_alloc_zero_for(p->al, Pattern);
  *patt = pattern;
  return patt;
}

static void sync_to_next_arm(Parser *p) {
  int depth = 0;
  while (!check_tok(p, TOKEN_EOF)) {
    TokenType t = current_tok(p)->type;
    if (t == TOKEN_LBRACE) {
      depth++;
      advance_tok(p);
    } else if (t == TOKEN_RBRACE) {
      if (depth == 0)
        return; // this is match's closing brace — stop
      depth--;
      advance_tok(p);
    } else if (t == TOKEN_COMMA && depth == 0) {
      return; // next arm starts after this comma
    } else {
      advance_tok(p);
    }
  }
}

static Expr *parse_match(Parser *p) {
  Token tok = *previous_tok(p);
  bool had_error = false;

  bool old_allow_struct_init = p->allow_struct_init;
  p->allow_struct_init = false;
  Expr *value = parse_expr(p);
  p->allow_struct_init = old_allow_struct_init;
  if (value->kind == EXPR_POISON) {
    had_error = true;
  }

  if (!consume_tok(p, TOKEN_LBRACE, "expected '{' after 'match' expression")) {
    had_error = true;
  }

  PLIST(MatchArm, arms, 8);

  do {
    Span arm_start_span = current_tok_span(p);
    if (check_tok(p, TOKEN_RBRACE)) {
      break;
    }

    PLIST_GROW(p, arms);
    MatchArm *arm = &arms[arms_count++];
    arm->pattern = parse_pattern(p);
    if (!arm->pattern) {
      had_error = true;
      sync_to_next_arm(p);
      continue;
    }

    if (match_tok(p, TOKEN_IF)) {
      arm->guard = parse_expr(p);
      if (arm->guard->kind == EXPR_POISON) {
        had_error = true;
        sync_to_next_arm(p);
        continue;
      }
    } else {
      arm->guard = NULL;
    }

    if (!consume_tok(p, TOKEN_FAT_ARROW,
                     "expected '=>' after match arm pattern")) {
      had_error = true;
      sync_to_next_arm(p);
      continue;
    }

    arm->body = parse_expr(p);
    if (arm->body->kind == EXPR_POISON) {
      had_error = true;
      sync_to_next_arm(p);
      continue;
    }

    arm->span = span_merge(arm_start_span, arm->body->span);
  } while (match_tok(p, TOKEN_COMMA));

  if (!consume_tok(p, TOKEN_RBRACE, "expected '}' after match arms")) {
    had_error = true;
  }

  if (had_error) {
    return ast_expr(EXPR_POISON, current_tok_span(p), p->al);
  }

  Expr *expr = ast_expr(
      EXPR_MATCH, span_merge(token_span(&tok), current_tok_span(p)), p->al);
  expr->as.match.subject = value;
  expr->as.match.arms = PLIST_TAKE(p, arms);
  expr->as.match.arm_count = arms_count;
  expr->as.match.enforce_exhaustiveness = true;
  return expr;
}

// ── Interpolation format specs (milestone 35) ────────────────────────────────
//
// A `:`-introduced spec after the value in `{v:>8}` / `{f:.3}` / `{f:>8.3}`:
// one alignment (`<` `>` `^`) with a width, an optional leading fill char, and
// an optional `.N` precision — nothing more, because it is only shorthand for
// the `pad_*` / `float` calls the checker desugars it to. One scanner quirk
// shapes the parse: `8.3` scans as one FLOAT token (a digit precedes the dot),
// while a bare `.3` scans as DOT then INT, so a width and a precision arrive
// fused in the first case and separate in the second.

static int64_t parse_int_lexeme(StringView lexeme) {
  char buf[32];
  int len = lexeme.len < 31 ? (int)lexeme.len : 31;
  memcpy(buf, lexeme.chars, len);
  buf[len] = '\0';
  return atoll(buf);
}

// `.N`, the current token being the `.`.
static bool parse_format_precision(Parser *p, FormatSpec *spec) {
  match_tok(p, TOKEN_DOT);
  if (!check_tok(p, TOKEN_INT)) {
    error_at(p, current_tok_span(p),
             "expected a number of decimal places after '.' in this format "
             "spec");
    return false;
  }
  spec->has_precision = true;
  spec->precision = parse_int_lexeme(peek_tok(p)->lexeme);
  match_tok(p, TOKEN_INT);
  return true;
}

// The whole spec, the current token being the `:`.
static bool parse_format_spec(Parser *p, FormatSpec *spec) {
  *spec = (FormatSpec){.present = true, .fill = ' '};
  match_tok(p, TOKEN_COLON);

  // `.N` alone: a precision with no field width (`{f:.3}`).
  if (check_tok(p, TOKEN_DOT)) {
    return parse_format_precision(p, spec);
  }

  // an optional fill character, written as a char literal (`{v:'-'^8}`). A
  // char literal tokenises unambiguously where a bare `*` would collide with
  // multiplication, and it is the char the value is padded with either way.
  if (check_tok(p, TOKEN_CHAR)) {
    uint32_t cp = 0;
    char_literal_value(peek_tok(p)->lexeme, &cp);
    spec->fill = cp;
    match_tok(p, TOKEN_CHAR);
  }

  // a required alignment...
  if (match_tok(p, TOKEN_GT)) {
    spec->align = FMT_ALIGN_START;
  } else if (match_tok(p, TOKEN_LT)) {
    spec->align = FMT_ALIGN_END;
  } else if (match_tok(p, TOKEN_CARET)) {
    spec->align = FMT_ALIGN_CENTER;
  } else {
    error_at(p, current_tok_span(p),
             "expected an alignment '<', '>' or '^' in this format spec");
    return false;
  }

  // ...and a required width, which arrives as an INT, or as a FLOAT whose
  // fractional part is a fused precision (`>8.3`).
  if (check_tok(p, TOKEN_INT)) {
    spec->width = parse_int_lexeme(peek_tok(p)->lexeme);
    match_tok(p, TOKEN_INT);
    if (check_tok(p, TOKEN_DOT)) { // a width and precision written apart
      return parse_format_precision(p, spec);
    }
  } else if (check_tok(p, TOKEN_FLOAT)) {
    StringView lex = peek_tok(p)->lexeme;
    int dot = 0;
    while (dot < (int)lex.len && lex.chars[dot] != '.')
      dot++;
    spec->width =
        parse_int_lexeme((StringView){.chars = lex.chars, .len = dot});
    spec->has_precision = true;
    spec->precision = parse_int_lexeme(
        (StringView){.chars = lex.chars + dot + 1, .len = lex.len - dot - 1});
    match_tok(p, TOKEN_FLOAT);
  } else {
    error_at(p, current_tok_span(p),
             "expected a field width after the alignment in this format spec");
    return false;
  }

  return true;
}

// the optional `var P =` prefix of an `if`/`while` header. `var` cannot start
// an expression, so seeing it here is unambiguous, and the pattern sits to the
// left of the `=` — which is what lets a struct pattern keep its braces in a
// header where a struct *literal* is disallowed. Returns NULL for a plain
// condition; `*failed` says which NULL it was.
static Pattern *parse_cond_binding(Parser *p, bool *failed) {
  *failed = false;
  if (!match_tok(p, TOKEN_VAR)) {
    return NULL;
  }
  Pattern *binding = parse_pattern(p);
  if (binding == NULL ||
      !consume_tok(p, TOKEN_EQ, "expected '=' after the pattern")) {
    *failed = true;
    return NULL;
  }
  return binding;
}

static Expr *parse_primary(Parser *p) {
  Token *t = peek_tok(p);

  // integer literal
  if (match_tok(p, TOKEN_INT)) {
    Expr *expr = ast_expr(EXPR_INT, token_span(t), p->al);
    char buf[32];
    int len = t->lexeme.len < 31 ? t->lexeme.len : 31;
    memcpy(buf, t->lexeme.chars, len);
    buf[len] = '\0';
    expr->as.int_val = atoll(buf);
    return expr;
  }

  // float literal
  if (match_tok(p, TOKEN_FLOAT)) {
    Expr *expr = ast_expr(EXPR_FLOAT, token_span(t), p->al);
    char buf[64];
    int len = t->lexeme.len < 63 ? t->lexeme.len : 63;
    memcpy(buf, t->lexeme.chars, len);
    buf[len] = '\0';
    expr->as.float_val = atof(buf);
    return expr;
  }

  if (match_tok(p, TOKEN_STRING)) {
    Expr *expr = ast_expr(EXPR_STRING, token_span(t), p->al);
    expr->as.string.value =
        (StringView){.len = t->lexeme.len - 2, .chars = t->lexeme.chars + 1};
    return expr;
  }

  // character literal. The scanner has already validated the lexeme and
  // reported anything wrong with it, so a failure here is one the compile is
  // failing over anyway and the value only has to be *a* char.
  if (match_tok(p, TOKEN_CHAR)) {
    Expr *expr = ast_expr(EXPR_CHAR, token_span(t), p->al);
    uint32_t cp = 0;
    char_literal_value(t->lexeme, &cp);
    expr->as.char_val = cp;
    return expr;
  }

  if (match_tok(p, TOKEN_INTERPOLATION)) {
    // an interpolation contributes up to two segments (the text before it and
    // the value), so the old 16 was really a limit of seven or eight `{}` in
    // one string — the tightest of the caps this milestone lifts, and the one
    // a person writing a message hits first.
    PLIST(InterpolSeg, segs, 8);
    segs_count = 2;

    segs[0].kind = ISEG_TEXT;
    segs[0].text =
        (StringView){.len = t->lexeme.len - 2, .chars = t->lexeme.chars + 1};

    segs[1].kind = ISEG_EXPR;
    segs[1].expr = parse_expr(p);

    if (segs[1].expr->kind == EXPR_POISON) {
      return ast_expr(EXPR_POISON, current_tok_span(p), p->al);
    }

    if (check_tok(p, TOKEN_COLON) && !parse_format_spec(p, &segs[1].spec)) {
      return ast_expr(EXPR_POISON, current_tok_span(p), p->al);
    }

    while (check_tok(p, TOKEN_STRING) || check_tok(p, TOKEN_INTERPOLATION)) {
      // terminal segment
      if (match_tok(p, TOKEN_STRING)) {
        if (previous_tok(p)->lexeme.len > 2) {
          PLIST_GROW(p, segs);
          segs[segs_count].kind = ISEG_TEXT;
          segs[segs_count].text =
              (StringView){.len = previous_tok(p)->lexeme.len - 2,
                           .chars = previous_tok(p)->lexeme.chars + 1};
          segs_count++;
        }
        break;
      }

      if (!consume_tok(
              p, TOKEN_INTERPOLATION,
              "expected interpolation segment in interpolated string")) {
        return ast_expr(EXPR_POISON, current_tok_span(p), p->al);
      }

      // text between the previous `}` and this `{`
      if (previous_tok(p)->lexeme.len > 2) {
        PLIST_GROW(p, segs);
        segs[segs_count].kind = ISEG_TEXT;
        segs[segs_count].text =
            (StringView){.len = previous_tok(p)->lexeme.len - 2,
                         .chars = previous_tok(p)->lexeme.chars + 1};
        segs_count++;
      }

      Expr *expr = parse_expr(p);
      if (expr->kind == EXPR_POISON) {
        return ast_expr(EXPR_POISON, current_tok_span(p), p->al);
      }

      PLIST_GROW(p, segs);
      segs[segs_count].kind = ISEG_EXPR;
      segs[segs_count].expr = expr;
      if (check_tok(p, TOKEN_COLON) &&
          !parse_format_spec(p, &segs[segs_count].spec)) {
        return ast_expr(EXPR_POISON, current_tok_span(p), p->al);
      }
      segs_count++;
    }

    Expr *expr = ast_expr(EXPR_INTERPOLATED, token_span(t), p->al);
    expr->as.interpolated.segs = PLIST_TAKE(p, segs);
    expr->as.interpolated.seg_count = segs_count;
    return expr;
  }

  // true / false
  if (match_tok(p, TOKEN_TRUE) || match_tok(p, TOKEN_FALSE)) {
    Expr *expr = ast_expr(EXPR_BOOL, token_span(t), p->al);
    expr->as.bool_val = t->type == TOKEN_TRUE;
    return expr;
  }

  // path / struct-init / variant
  if (check_tok(p, TOKEN_IDENT) || check_tok(p, TOKEN_SELF_TYPE)) {
    Path path = {0};
    if (!parse_path(p, PATH_EXPR, &path)) {
      return ast_expr(EXPR_POISON, current_tok_span(p), p->al);
    }

    // c-style init
    if (p->allow_struct_init && match_tok(p, TOKEN_LBRACE)) {

      PLIST(FieldInit, fields, 8);

      if (!check_tok(p, TOKEN_RBRACE)) {
        do {
          if (check_tok(p, TOKEN_RBRACE)) {
            break;
          }
          if (!consume_tok(p, TOKEN_IDENT, "expected field name")) {
            return ast_expr(EXPR_POISON, current_tok_span(p), p->al);
          }
          Token *field_tok = previous_tok(p);
          StringView field_name = field_tok->lexeme;

          if (!consume_tok(p, TOKEN_COLON, "expected ':' after field name")) {
            return ast_expr(EXPR_POISON, current_tok_span(p), p->al);
          }

          Expr *field_value = parse_expr(p);
          if (field_value->kind == EXPR_POISON) {
            return ast_expr(EXPR_POISON, current_tok_span(p), p->al);
          }

          PLIST_GROW(p, fields);
          fields[fields_count].ident.name = field_name;
          fields[fields_count].value = field_value;
          fields[fields_count].span =
              span_merge(token_span(field_tok), field_value->span);
          fields_count++;
        } while (match_tok(p, TOKEN_COMMA));
      }

      if (!consume_tok(p, TOKEN_RBRACE, "expected '}' after struct init")) {
        return ast_expr(EXPR_POISON,
                        span_merge(token_span(t), previous_tok_span(p)), p->al);
      }

      Expr *expr =
          ast_expr(EXPR_STRUCT_INIT,
                   span_merge(token_span(t), previous_tok_span(p)), p->al);
      expr->as.struct_init.path = path;
      span_merge(token_span(t), previous_tok_span(p));
      expr->as.struct_init.field_count = fields_count;
      expr->as.struct_init.fields = PLIST_TAKE(p, fields);
      return expr;
    }

    Expr *expr = ast_expr(
        EXPR_PATH, span_merge(token_span(t), previous_tok_span(p)), p->al);
    expr->as.path_expr.path = path;
    return expr;
  }

  // tuple / unit
  if (match_tok(p, TOKEN_LPAREN)) {
    Span start_span = previous_tok_span(p);

    if (match_tok(p, TOKEN_RPAREN)) {
      return ast_expr(EXPR_UNIT, span_merge(start_span, previous_tok_span(p)),
                      p->al);
    }

    PLIST(Expr *, elem_exprs, 8);
    bool had_error = false;
    bool force_tuple = false;
    bool old_allow_struct_init = p->allow_struct_init;
    p->allow_struct_init = true;

    if (!check_tok(p, TOKEN_RPAREN)) {
      do {
        if (check_tok(p, TOKEN_RPAREN)) {
          force_tuple = true;
          break;
        }
        Expr *e = parse_expr(p);
        if (e->kind == EXPR_POISON) {
          had_error = true;
          break;
        }
        PLIST_PUSH(p, elem_exprs, e);
      } while (match_tok(p, TOKEN_COMMA));
    }

    if (!consume_tok(p, TOKEN_RPAREN, "expected ')' after tuple expression")) {
      had_error = true;
    }

    p->allow_struct_init = old_allow_struct_init;

    if (had_error) {
      return ast_expr(EXPR_POISON, span_merge(start_span, current_tok_span(p)),
                      p->al);
    }

    if (elem_exprs_count == 1 && !force_tuple) {
      // just a grouping
      return elem_exprs[0];
    }

    Expr *expr = ast_expr(EXPR_TUPLE,
                          span_merge(start_span, previous_tok_span(p)), p->al);
    expr->as.tuple.elems = PLIST_TAKE(p, elem_exprs);
    expr->as.tuple.count = elem_exprs_count;
    return expr;
  }

  // closure
  if (match_tok(p, TOKEN_PIPE)) {
    return parse_closure(p);
  }

  // array literal
  if (match_tok(p, TOKEN_LBRACKET)) {
    Span start_span = previous_tok_span(p);

    // 16 elements was the cap a literal array of data hit first, and it is the
    // one with no workaround short of `push` in a loop.
    PLIST(Expr *, elem_exprs, 8);
    bool had_error = false;
    bool old_allow_struct_init = p->allow_struct_init;
    p->allow_struct_init = true;

    if (!check_tok(p, TOKEN_RBRACKET)) {
      do {
        if (check_tok(p, TOKEN_RBRACKET)) {
          break; // trailing comma
        }
        Expr *e = parse_expr(p);
        if (e->kind == EXPR_POISON) {
          had_error = true;
          break;
        }
        PLIST_PUSH(p, elem_exprs, e);
      } while (match_tok(p, TOKEN_COMMA));
    }

    if (!consume_tok(p, TOKEN_RBRACKET, "expected ']' after array literal")) {
      had_error = true;
    }

    p->allow_struct_init = old_allow_struct_init;

    if (had_error) {
      return ast_expr(EXPR_POISON, span_merge(start_span, current_tok_span(p)),
                      p->al);
    }

    Expr *expr = ast_expr(EXPR_ARRAY,
                          span_merge(start_span, previous_tok_span(p)), p->al);
    expr->as.array.elems = PLIST_TAKE(p, elem_exprs);
    expr->as.array.count = elem_exprs_count;
    return expr;
  }

  // match
  if (match_tok(p, TOKEN_MATCH)) {
    return parse_match(p);
  }

  // if
  if (match_tok(p, TOKEN_IF)) {
    bool old_allow_struct_init = p->allow_struct_init;

    p->allow_struct_init = false;
    bool binding_failed = false;
    Pattern *binding = parse_cond_binding(p, &binding_failed);
    if (binding_failed) {
      p->allow_struct_init = old_allow_struct_init;
      return ast_expr(EXPR_POISON, current_tok_span(p), p->al);
    }
    Expr *cond = parse_expr(p);
    p->allow_struct_init = old_allow_struct_init;

    if (cond->kind == EXPR_POISON) {
      return ast_expr(EXPR_POISON, current_tok_span(p), p->al);
    }

    // the restriction belongs to the *condition* — it is what makes the `{`
    // after it start the body rather than a struct literal. Inside the body
    // there is no ambiguity left, so parse_block runs with the flag restored
    // (which is what `for` above already did, and `if` did not).
    Expr *then_block = parse_block(p);
    if (then_block->kind == EXPR_POISON) {
      return ast_expr(EXPR_POISON, current_tok_span(p), p->al);
    }

    Expr *else_branch = NULL;
    if (match_tok(p, TOKEN_ELSE)) {
      if (check_tok(p, TOKEN_IF)) {
        else_branch = parse_expr(p);
      } else {
        else_branch = parse_block(p);
      }
    }

    if (else_branch && else_branch->kind == EXPR_POISON) {
      return ast_expr(EXPR_POISON, current_tok_span(p), p->al);
    }

    Expr *expr = ast_expr(
        EXPR_IF, span_merge(token_span(t), previous_tok_span(p)), p->al);
    expr->as.if_expr.condition = cond;
    expr->as.if_expr.binding = binding;
    expr->as.if_expr.then_block = then_block;
    expr->as.if_expr.else_branch = else_branch;
    return expr;
  }

  // A label declares a name for the loop it prefixes and can prefix nothing
  // else, so it is consumed here, ahead of the three loop forms, and whichever
  // one follows takes it. Refusing everything else at this point is what keeps
  // a label from ever being an expression in its own right.
  LoopLabel label = {0};
  if (check_tok(p, TOKEN_LABEL)) {
    label = parse_opt_label(p);
    if (!consume_tok(p, TOKEN_COLON, "expected ':' after a loop label")) {
      // the two spellings differ only in the closing quote, so a one-character
      // label with no `:` behind it is far more likely a character literal
      // that was never closed — which is the one place the fork misleads.
      if (label.name.len == 2) {
        diag_note(p->diags, label.span,
                  "a character literal needs its closing quote: " SV_FMT "'",
                  SV_ARG(label.name));
      }
      return ast_expr(EXPR_POISON, current_tok_span(p), p->al);
    }
    if (!check_tok(p, TOKEN_FOR) && !check_tok(p, TOKEN_WHILE) &&
        !check_tok(p, TOKEN_LOOP) && !check_tok(p, TOKEN_LBRACE)) {
      error_at(p, current_tok_span(p),
               "a label may only name a 'loop', a 'while', a 'for' or a block");
      return ast_expr(EXPR_POISON, current_tok_span(p), p->al);
    }
  }

  // for
  if (match_tok(p, TOKEN_FOR)) {
    if (!consume_tok(p, TOKEN_IDENT, "expected loop variable name")) {
      return ast_expr(EXPR_POISON, current_tok_span(p), p->al);
    }
    Token *var_tok = previous_tok(p);

    if (!consume_tok(p, TOKEN_IN, "expected 'in' after loop variable")) {
      return ast_expr(EXPR_POISON, current_tok_span(p), p->al);
    }

    bool old_allow_struct_init = p->allow_struct_init;
    p->allow_struct_init = false;
    Expr *iterable = parse_expr(p);
    if (iterable->kind == EXPR_POISON) {
      return ast_expr(EXPR_POISON, current_tok_span(p), p->al);
    }
    p->allow_struct_init = old_allow_struct_init;

    Expr *block = parse_block(p);
    if (block->kind == EXPR_POISON) {
      return ast_expr(EXPR_POISON, current_tok_span(p), p->al);
    }

    Expr *expr = ast_expr(
        EXPR_FOR, span_merge(token_span(previous_tok(p)), previous_tok_span(p)),
        p->al);
    expr->as.for_expr = (ExprFor){
        .label = label,
        .var_name = var_tok->lexeme,
        .var_span = token_span(var_tok),
        .iterable = iterable,
        .body = block,
    };
    return expr;
  }

  // while
  if (match_tok(p, TOKEN_WHILE)) {
    bool old_allow_struct_init = p->allow_struct_init;
    p->allow_struct_init = false;
    bool binding_failed = false;
    Pattern *binding = parse_cond_binding(p, &binding_failed);
    if (binding_failed) {
      p->allow_struct_init = old_allow_struct_init;
      return ast_expr(EXPR_POISON, current_tok_span(p), p->al);
    }
    Expr *cond = parse_expr(p);
    p->allow_struct_init = old_allow_struct_init;

    if (cond->kind == EXPR_POISON) {
      return ast_expr(EXPR_POISON, current_tok_span(p), p->al);
    }

    Expr *block = parse_block(p);
    if (block->kind == EXPR_POISON) {
      return ast_expr(EXPR_POISON, current_tok_span(p), p->al);
    }

    Expr *expr = ast_expr(
        EXPR_WHILE, span_merge(token_span(t), previous_tok_span(p)), p->al);
    expr->as.while_expr.label = label;
    expr->as.while_expr.condition = cond;
    expr->as.while_expr.binding = binding;
    expr->as.while_expr.body = block;
    return expr;
  }

  // loop — no header at all, which is the whole point of the keyword: there is
  // no condition to read, so nothing has to prove the loop ends.
  if (match_tok(p, TOKEN_LOOP)) {
    Expr *block = parse_block(p);
    if (block->kind == EXPR_POISON) {
      return ast_expr(EXPR_POISON, current_tok_span(p), p->al);
    }

    Expr *expr = ast_expr(
        EXPR_LOOP, span_merge(token_span(t), previous_tok_span(p)), p->al);
    expr->as.loop_expr.label = label;
    expr->as.loop_expr.body = block;
    return expr;
  }

  // block — labelled or not, which is the same node either way. The label was
  // consumed above, so an ordinary block simply arrives with an empty one.
  if (check_tok(p, TOKEN_LBRACE)) {
    Expr *block = parse_block(p);
    if (block->kind == EXPR_BLOCK) {
      block->as.block.label = label;
      block->span = span_merge(token_span(t), block->span);
    }
    return block;
  }

  if (check_tok(p, TOKEN_SELF)) {
    advance_tok(p);
    Expr *expr = ast_expr(EXPR_SELF, token_span(previous_tok(p)), p->al);
    return expr;
  }

  // nothing matched
  error_at(p, current_tok_span(p), "expected expression");
  return ast_expr(EXPR_POISON, current_tok_span(p), p->al);
}

static Decl *parse_var_decl(Parser *p, bool is_pub) {
  if (!consume_tok(p, TOKEN_VAR, "expected 'var'")) {
    return ast_decl(DECL_POISON, current_tok_span(p), p->al);
  }
  Token *var_tok = previous_tok(p);

  Pattern *binding = parse_pattern(p);
  if (binding == NULL) {
    return ast_decl(DECL_POISON, current_tok_span(p), p->al);
  }

  TypeNode *ann = NULL;
  if (match_tok(p, TOKEN_COLON)) {
    ann = parse_type(p);
    assert(ann && "type anotation should not be NULL");
    if (ann->kind == TYNODE_POISON) {
      return ast_decl(DECL_POISON, ann->span, p->al);
    }
  }

  if (!consume_tok(p, TOKEN_EQ, "expected '=' after variable declaration")) {
    return ast_decl(DECL_POISON, current_tok_span(p), p->al);
  }

  Expr *init = parse_expr(p);
  assert(init && "initializer should not be NULL");

  if (init->kind == EXPR_POISON) {
    return ast_decl(DECL_POISON, init->span, p->al);
  }

  if (!consume_tok(p, TOKEN_SEMICOLON, "expected ';'")) {
    return ast_decl(DECL_POISON, current_tok_span(p), p->al);
  };

  Span full = span_merge(token_span(var_tok), previous_tok_span(p));
  Decl *decl = ast_decl(DECL_VAR, full, p->al);

  decl->as.var_decl.binding = binding;
  decl->as.var_decl.initializer = init;
  decl->as.var_decl.type_annotation = ann;
  decl->is_pub = is_pub;

  return decl;
}

static Stmt *parse_var_stmt(Parser *p) {
  if (!consume_tok(p, TOKEN_VAR, "expected 'var'")) {
    return ast_stmt(STMT_POISON, current_tok_span(p), p->al);
  }
  Token *var_tok = previous_tok(p);

  Pattern *binding = parse_pattern(p);
  if (binding == NULL) {
    return ast_stmt(STMT_POISON, current_tok_span(p), p->al);
  }

  TypeNode *ann = NULL;
  if (match_tok(p, TOKEN_COLON)) {
    ann = parse_type(p);
    assert(ann && "type anotation should not be NULL");
    if (ann->kind == TYNODE_POISON) {
      return ast_stmt(STMT_POISON, ann->span, p->al);
    }
  }

  if (!consume_tok(p, TOKEN_EQ, "expected '=' after variable declaration")) {
    return ast_stmt(STMT_POISON, current_tok_span(p), p->al);
  }

  Expr *init = parse_expr(p);
  assert(init && "initializer should not be NULL");

  if (init->kind == EXPR_POISON) {
    return ast_stmt(STMT_POISON, init->span, p->al);
  }

  // `var P = e else { .. };`. No lookahead is needed to tell this `else` from
  // an `if`'s: an `if` initializer has already taken its own on the way out of
  // parse_expr, so whatever is left here belongs to the binding.
  Expr *else_block = NULL;
  if (match_tok(p, TOKEN_ELSE)) {
    else_block = parse_block(p);
    if (else_block->kind == EXPR_POISON) {
      return ast_stmt(STMT_POISON, else_block->span, p->al);
    }
  }

  if (!consume_tok(p, TOKEN_SEMICOLON, "expected ';'")) {
    return ast_stmt(STMT_POISON, current_tok_span(p), p->al);
  };

  Span full = span_merge(token_span(var_tok), previous_tok_span(p));
  Stmt *stmt = ast_stmt(STMT_VAR, full, p->al);

  stmt->as.var_stmt.binding = binding;
  stmt->as.var_stmt.initializer = init;
  stmt->as.var_stmt.type_annotation = ann;
  stmt->as.var_stmt.else_block = else_block;

  return stmt;
}

Stmt *parse_stmt(Parser *p) {
  Span start_span = current_tok_span(p);

  if (check_tok(p, TOKEN_VAR)) {
    Stmt *stmt = parse_var_stmt(p);
    if (stmt->kind == STMT_POISON) {
      return ast_stmt(STMT_POISON, span_merge(start_span, stmt->span), p->al);
    }
    return stmt;
  }

  if (match_tok(p, TOKEN_RETURN)) {
    Expr *value = NULL;
    if (!check_tok(p, TOKEN_SEMICOLON)) {
      value = parse_expr(p);
      if (value->kind == EXPR_POISON) {
        return ast_stmt(STMT_POISON, span_merge(start_span, value->span),
                        p->al);
      }
    }
    Stmt *stmt = ast_stmt(STMT_RETURN, token_span(previous_tok(p)), p->al);
    stmt->span = token_span(previous_tok(p));
    stmt->as.return_stmt.value = value;
    if (!consume_tok(p, TOKEN_SEMICOLON, "expected ';' after return value")) {
      return ast_stmt(STMT_POISON, span_merge(start_span, current_tok_span(p)),
                      p->al);
    }
    return stmt;
  }

  if (match_tok(p, TOKEN_BREAK)) {
    Span break_span = token_span(previous_tok(p));
    // still no ambiguity to resolve: a block is not an expression primary
    // here, and the scanner has already settled `'a'` against `'a`, so a label
    // and a value can be told apart by token type alone.
    LoopLabel label = parse_opt_label(p);
    Expr *value = NULL;
    if (!check_tok(p, TOKEN_SEMICOLON)) {
      value = parse_expr(p);
      if (value->kind == EXPR_POISON) {
        return ast_stmt(STMT_POISON, span_merge(start_span, value->span),
                        p->al);
      }
    }
    Stmt *stmt = ast_stmt(STMT_BREAK, break_span, p->al);
    stmt->as.break_stmt.label = label;
    stmt->as.break_stmt.value = value;
    if (!consume_tok(p, TOKEN_SEMICOLON, "expected ';' after 'break'")) {
      return ast_stmt(STMT_POISON, span_merge(start_span, current_tok_span(p)),
                      p->al);
    }
    return stmt;
  }

  if (match_tok(p, TOKEN_CONTINUE)) {
    Stmt *stmt = ast_stmt(STMT_CONTINUE, token_span(previous_tok(p)), p->al);
    stmt->as.continue_stmt.label = parse_opt_label(p);
    if (!consume_tok(p, TOKEN_SEMICOLON, "expected ';' after 'continue'")) {
      return ast_stmt(STMT_POISON, span_merge(start_span, current_tok_span(p)),
                      p->al);
    }
    return stmt;
  }

  // if (match_tok(p, TOKEN_SEMICOLON)) {
  //   Stmt *stmt = ast_stmt(STMT_EXPR, token_span(previous_tok(p)), p->al);
  //   stmt->as.expr_stmt.expr = ast_expr(EXPR_UNIT, stmt->span, p->al);
  //   return stmt;
  // }

  if (check_tok(p, TOKEN_MATCH) || check_tok(p, TOKEN_IF) ||
      check_tok(p, TOKEN_FOR) || check_tok(p, TOKEN_WHILE) ||
      check_tok(p, TOKEN_LOOP) || check_tok(p, TOKEN_LABEL)) {
    Expr *expr = parse_primary(p);
    if (expr->kind == EXPR_POISON) {
      return ast_stmt(STMT_POISON, span_merge(start_span, expr->span), p->al);
    }
    Stmt *stmt = ast_stmt(STMT_EXPR, span_merge(start_span, expr->span), p->al);
    stmt->as.expr_stmt.expr = expr;
    return stmt;
  }

  assert(false && "unrecognized statement");
  advance_tok(p);
  return ast_stmt(STMT_POISON, start_span, p->al);
}

static ParamDeclNode parse_param(Parser *p) {
  ParamDeclNode param = {0};

  if (match_tok(p, TOKEN_SELF)) {
    param.is_self = true;
    param.span = previous_tok_span(p);
    return param;
  }

  if (!consume_tok(p, TOKEN_IDENT, "expected parameter name")) {
    param.span = current_tok_span(p);
    return param;
  }
  Token name_tok = *previous_tok(p);

  if (!consume_tok(p, TOKEN_COLON, "expected ':' after parameter name")) {
    param.span = token_span(&name_tok);
    return param;
  }

  TypeNode *ty = parse_type(p);
  if (ty->kind == TYNODE_POISON) {
    param.span = token_span(&name_tok);
    return param;
  }

  param.name = name_tok.lexeme;
  param.type_annotation = ty;
  param.span = span_merge(token_span(&name_tok), ty->span);
  return param;
}

static bool parse_param_list(Parser *p, ParamDeclNode **out, int *count) {
  PLIST(ParamDeclNode, params, 8);
  bool had_error = false;

  // empty param list
  if (!check_tok(p, TOKEN_RPAREN)) {
    do {
      // trailing comma: fun f(a: int, b: int,) — stop before ')'
      if (check_tok(p, TOKEN_RPAREN))
        break;

      ParamDeclNode param = parse_param(p);
      if (param.type_annotation == NULL && !param.is_self) {
        had_error = true;
        break; // can't trust param list structure anymore
      }
      PLIST_PUSH(p, params, param);

    } while (match_tok(p, TOKEN_COMMA));
  }

  *out = PLIST_TAKE(p, params);
  *count = params_count;
  return !had_error;
}

// `@native("io_print")` / `@intrinsic("array_len")` / `@lang("display")` /
// `@allow("unused_variable")`, already past the '@'. Each takes exactly one
// string: the attribute surface is a registry key and nothing more, so there is
// no attribute *grammar* to grow here — a new tier is a new name in this
// switch. `@native`/`@intrinsic` are body attributes (the fun is bodyless);
// `@lang` is a marker (the definition keeps its body); `@allow` is policy over
// whatever the definition contains. `parse_attrs` sorts them apart, and the key
// itself is checked there or later — never here.
static AttrNode parse_attr(Parser *p) {
  Token at_tok = *previous_tok(p);
  AttrNode attr = {.kind = ATTR_NONE, .span = token_span(&at_tok)};

  if (!consume_tok(p, TOKEN_IDENT, "expected attribute name after '@'")) {
    return attr;
  }
  Token name_tok = *previous_tok(p);
  AttrKind kind = ATTR_NONE;
  if (sv_equal_cstr(name_tok.lexeme, "native")) {
    kind = ATTR_NATIVE;
  } else if (sv_equal_cstr(name_tok.lexeme, "intrinsic")) {
    kind = ATTR_INTRINSIC;
  } else if (sv_equal_cstr(name_tok.lexeme, "lang")) {
    kind = ATTR_LANG;
  } else if (sv_equal_cstr(name_tok.lexeme, "allow")) {
    kind = ATTR_ALLOW;
  } else {
    error_at(p, token_span(&name_tok),
             "unknown attribute; expected '@native', '@intrinsic', '@lang' or "
             "'@allow'");
    return attr;
  }

  if (!consume_tok(p, TOKEN_LPAREN, "expected '(' after attribute name") ||
      !consume_tok(p, TOKEN_STRING,
                   "expected a name string in the attribute")) {
    return attr;
  }
  Token key_tok = *previous_tok(p);
  if (!consume_tok(p, TOKEN_RPAREN, "expected ')'")) {
    return attr;
  }

  attr.kind = kind;
  // the lexeme keeps its quotes; the key is what sits between them.
  attr.name = (StringView){.chars = key_tok.lexeme.chars + 1,
                           .len = key_tok.lexeme.len - 2};
  attr.span = span_merge(token_span(&at_tok), previous_tok_span(p));
  return attr;
}

// Read the attributes in front of one declaration and sort them into the three
// slots they can fill: a *body* (`@native`/`@intrinsic`, fun-only, bodyless), a
// *marker* (`@lang`), and the `@allow` mask. Only the last may be written more
// than once, because it names a lint rather than the definition — one line per
// lint, since an attribute takes exactly one key. Returns false if an attribute
// was malformed, which leaves the caller to poison and resynchronise; a
// misplaced (rather than broken) attribute is the caller's to reject, since
// where each may sit depends on what follows.
static bool parse_attrs(Parser *p, AttrNode *body, AttrNode *lang,
                        unsigned *allow) {
  *body = (AttrNode){.kind = ATTR_NONE};
  *lang = (AttrNode){.kind = ATTR_NONE};
  *allow = 0;

  while (match_tok(p, TOKEN_AT)) {
    AttrNode a = parse_attr(p);
    if (a.kind == ATTR_NONE) {
      return false;
    }
    if (a.kind == ATTR_ALLOW) {
      int lint = diag_lint_from_name(a.name);
      if (lint < 0) {
        error_at_lint(p, a.span, a.name);
        continue; // the declaration is still readable; only the policy is lost
      }
      *allow |= LINT_BIT(lint);
    } else if (a.kind == ATTR_LANG) {
      if (lang->kind != ATTR_NONE) {
        error_at(p, a.span, "duplicate '@lang' attribute");
      }
      *lang = a;
    } else {
      if (body->kind != ATTR_NONE) {
        error_at(p, a.span,
                 "a definition has at most one '@native'/'@intrinsic'");
      }
      *body = a;
    }
  }
  return true;
}

static Decl *parse_fun_decl(Parser *p, bool is_pub, AttrNode attr) {
  if (!consume_tok(p, TOKEN_FUN, "expected 'fun'")) {
    return ast_decl(DECL_POISON, current_tok_span(p), p->al);
  }
  Token fun_tok = *previous_tok(p);
  bool had_error = false;

  StringView name_sv = {0};
  Span name_span = {0};
  if (!consume_tok(p, TOKEN_IDENT, "expected function name")) {
    had_error = true;
  } else {
    name_sv = previous_tok(p)->lexeme;
    name_span = token_span(previous_tok(p));
  }

  TypeParamNode *type_params = NULL;
  int type_param_count = 0;
  if (check_tok(p, TOKEN_LT)) {
    type_param_count = parse_type_params(p, &type_params);
    if (type_param_count < 0) {
      had_error = true;
    }
  }

  if (!consume_tok(p, TOKEN_LPAREN, "expected '(' after function name")) {
    had_error = true;
    sync_to_fun_body(p);
  }

  ParamDeclNode *params = NULL;
  int param_count = 0;

  if (!parse_param_list(p, &params, &param_count)) {
    had_error = true;
    sync_to_fun_body(p);
  }

  if (!consume_tok(p, TOKEN_RPAREN, "expected ')'")) {
    had_error = true;
    sync_to_fun_body(p);
  }

  TypeNode *return_type = NULL;
  if (match_tok(p, TOKEN_THIN_ARROW)) {
    return_type = parse_type(p);
    assert(return_type && "return type should not be NULL");
    if (return_type->kind == TYNODE_POISON) {
      had_error = true;
    }
  }

  WhereClause *where_clause = NULL;

  if (check_tok(p, TOKEN_WHERE)) {
    if (!parse_where_clause(p, &where_clause, /*allow_self=*/false)) {
      had_error = true;
    }
  }

  // an attribute *is* the body, so the two are exclusive in both directions:
  // a `@native` fun ends at the semicolon, and any other one must have a
  // block. Reporting both ways is what keeps "which one wins?" from being a
  // question the language has to answer.
  Expr *body = NULL;
  if (attr.kind != ATTR_NONE) {
    if (!consume_tok(p, TOKEN_SEMICOLON,
                     "expected ';' — an attributed function has no body")) {
      had_error = true;
    }
  } else if (match_tok(p, TOKEN_SEMICOLON)) {
    error_at(p, previous_tok_span(p),
             "a function without '@native' or '@intrinsic' needs a body");
    had_error = true;
  } else if (match_tok(p, TOKEN_FAT_ARROW)) {
    body = parse_expr(p);
    assert(body && "function body should not be NULL");
    if (body->kind == EXPR_POISON) {
      had_error = true;
    }
  } else {
    p->panic_mode = false;
    body = parse_block(p);
    assert(body && "function body should not be NULL");
    if (body->kind == EXPR_POISON) {
      had_error = true;
    }
  }

  if (had_error) {
    return ast_decl(DECL_POISON,
                    span_merge(token_span(&fun_tok), previous_tok_span(p)),
                    p->al);
  }

  Span full = span_merge(token_span(&fun_tok), previous_tok_span(p));
  Decl *decl = ast_decl(DECL_FUN, full, p->al);
  decl->name_span = name_span;
  decl->is_pub = is_pub;
  decl->as.fun_decl = (DeclFun){
      .name = name_sv,
      .return_type = return_type,
      .body = body,
      .param_count = param_count,
      .params = params,
      .where_clause = where_clause,
      .shorthand = body != NULL && body->kind != EXPR_BLOCK,
      .attr = attr,
      .type_param_count = type_param_count,
      .type_params = type_params,
  };

  return decl;
}

// `allow_pub` gates a per-field `pub`: a struct field carries its own
// visibility, but an enum variant's payload does not (a variant is as visible
// as its enum), so a `pub` there is rejected rather than silently ignored.
static bool parse_field_decl(Parser *p, FieldDeclNode **out, int *out_count,
                             bool *out_is_tuple, bool allow_pub) {
  PLIST(FieldDeclNode, fields, 8);
  bool is_tuple_fields = false;
  bool had_error = false;

  if (match_tok(p, TOKEN_LBRACE)) {
    if (!check_tok(p, TOKEN_RBRACE)) {
      do {
        if (check_tok(p, TOKEN_RBRACE)) {
          break; // trailing comma
        }

        bool field_pub = match_tok(p, TOKEN_PUB);
        if (field_pub && !allow_pub) {
          error_at(p, previous_tok_span(p),
                   "'pub' is not allowed on an enum variant's field");
          had_error = true;
          break;
        }

        if (!consume_tok(p, TOKEN_IDENT, "expected field name")) {
          had_error = true;
          break;
        }
        Token name_tok = *previous_tok(p);

        if (!consume_tok(p, TOKEN_COLON, "expected ':' after field name")) {
          had_error = true;
          break;
        }

        TypeNode *ty = parse_type(p);
        if (ty->kind == TYNODE_POISON) {
          had_error = true;
          break;
        }

        PLIST_GROW(p, fields);
        fields[fields_count].ident.name = name_tok.lexeme;
        fields[fields_count].type_annotation = ty;
        fields[fields_count].is_pub = field_pub;
        fields[fields_count].span = span_merge(token_span(&name_tok), ty->span);
        fields_count++;
      } while (match_tok(p, TOKEN_COMMA));
    }

    if (had_error) {
      // sync to '}'
      while (!is_at_end(p) && !check_tok(p, TOKEN_RBRACE)) {
        advance_tok(p);
      }
    }

    if (!consume_tok(p, TOKEN_RBRACE, "expected '}' after struct fields")) {
      had_error = true;
    }
  } else if (match_tok(p, TOKEN_LPAREN)) {
    is_tuple_fields = true;

    if (!check_tok(p, TOKEN_RPAREN)) {
      do {
        bool field_pub = match_tok(p, TOKEN_PUB);
        if (field_pub && !allow_pub) {
          error_at(p, previous_tok_span(p),
                   "'pub' is not allowed on an enum variant's field");
          had_error = true;
          break;
        }

        TypeNode *ty = parse_type(p);
        if (ty->kind == TYNODE_POISON) {
          had_error = true;
          break;
        }

        PLIST_GROW(p, fields);
        fields[fields_count].ident.index = fields_count;
        fields[fields_count].type_annotation = ty;
        fields[fields_count].is_pub = field_pub;
        fields[fields_count].span = ty->span;
        fields_count++;
      } while (match_tok(p, TOKEN_COMMA));
    }

    if (had_error) {
      // sync to ')'
      while (!is_at_end(p) && !check_tok(p, TOKEN_RPAREN)) {
        advance_tok(p);
      }
    }

    if (!consume_tok(p, TOKEN_RPAREN, "expected ')' after struct fields")) {
      had_error = true;
    }
  }
  // else {
  //   error_at(p, current_tok_span(p), "expected '{' or '(' after struct
  //   name"); had_error = true;
  // }

  if (had_error) {
    return false;
  }

  assert(out);
  assert(out_count);
  assert(out_is_tuple);
  *out = PLIST_TAKE(p, fields);
  *out_is_tuple = is_tuple_fields;
  *out_count = fields_count;
  return true;
}

static Decl *parse_struct_decl(Parser *p, bool is_pub) {
  if (!consume_tok(p, TOKEN_STRUCT, "expected 'struct'")) {
    return ast_decl(DECL_POISON, current_tok_span(p), p->al);
  }
  Token struct_tok = *previous_tok(p);
  bool had_error = false;

  StringView name_sv = {0};
  Span name_span = {0};
  if (!consume_tok(p, TOKEN_IDENT, "expected struct name")) {
    had_error = true;
  } else {
    name_sv = previous_tok(p)->lexeme;
    name_span = token_span(previous_tok(p));
  }

  if (had_error) {
    // sync to type params or fields
    while (!is_at_end(p) && !check_tok(p, TOKEN_LT) &&
           !check_tok(p, TOKEN_LBRACE) && !check_tok(p, TOKEN_LPAREN)) {
      advance_tok(p);
    }
  }

  TypeParamNode *type_params = NULL;
  int type_param_count = 0;
  if (check_tok(p, TOKEN_LT)) {
    type_param_count = parse_type_params(p, &type_params);
    if (type_param_count < 0) {
      had_error = true;
    }
  }

  if (had_error) {
    // sync to fields
    while (!is_at_end(p) && !check_tok(p, TOKEN_LBRACE) &&
           !check_tok(p, TOKEN_LPAREN)) {
      advance_tok(p);
    }
  }

  int field_count = 0;
  FieldDeclNode *fields = NULL;
  bool is_tuple_struct = false;
  had_error =
      !parse_field_decl(p, &fields, &field_count, &is_tuple_struct, true) ||
      had_error;

  if (field_count == 0) {
    consume_tok(p, TOKEN_SEMICOLON,
                "expected ';' after unit struct declaration");
  }

  if (had_error) {
    return ast_decl(DECL_POISON,
                    span_merge(token_span(&struct_tok), previous_tok_span(p)),
                    p->al);
  }

  Decl *decl = ast_decl(DECL_STRUCT, token_span(&struct_tok), p->al);
  decl->name_span = name_span;
  decl->is_pub = is_pub;
  decl->as.struct_decl = (DeclStruct){
      .name = name_sv,
      .type_param_count = type_param_count,
      .type_params = type_params,
      .fields = fields,
      .field_count = field_count,
      .is_tuple = is_tuple_struct,
  };
  return decl;
}

static Decl *parse_enum_decl(Parser *p, bool is_pub) {
  if (!consume_tok(p, TOKEN_ENUM, "expected 'enum'")) {
    return ast_decl(DECL_POISON, current_tok_span(p), p->al);
  }
  Token enum_tok = *previous_tok(p);
  bool had_error = false;

  StringView name_sv = {0};
  Span name_span = {0};
  if (!consume_tok(p, TOKEN_IDENT, "expected enum name")) {
    had_error = true;
  } else {
    name_sv = previous_tok(p)->lexeme;
    name_span = token_span(previous_tok(p));
  }

  if (had_error) {
    // sync to type params or variants
    while (!is_at_end(p) && !check_tok(p, TOKEN_LT) &&
           !check_tok(p, TOKEN_LBRACE)) {
      advance_tok(p);
    }
  }

  TypeParamNode *type_params = NULL;
  int type_param_count = 0;
  if (check_tok(p, TOKEN_LT)) {
    type_param_count = parse_type_params(p, &type_params);
    if (type_param_count < 0) {
      had_error = true;
    }
  }

  if (had_error) {
    // sync to variants
    while (!is_at_end(p) && !check_tok(p, TOKEN_LBRACE)) {
      advance_tok(p);
    }
  }

  PLIST(VariantDeclNode, variants, 8);

  if (match_tok(p, TOKEN_LBRACE)) {
    if (!check_tok(p, TOKEN_RBRACE)) {
      do {
        if (check_tok(p, TOKEN_RBRACE)) {
          break; // trailing comma
        }

        if (!consume_tok(p, TOKEN_IDENT, "expected variant name")) {
          had_error = true;
          break;
        }
        Token name_tok = *previous_tok(p);

        bool is_tuple = false;
        int field_count = 0;
        FieldDeclNode *fields = NULL;
        had_error =
            !parse_field_decl(p, &fields, &field_count, &is_tuple, false) ||
            had_error;

        if (had_error) {
          break;
        }

        PLIST_PUSH(
            p, variants,
            ((VariantDeclNode){
                .name = name_tok.lexeme,
                .fields = fields,
                .field_count = field_count,
                .is_tuple = is_tuple,
                .span = span_merge(token_span(&name_tok), previous_tok_span(p)),
            }));
      } while (match_tok(p, TOKEN_COMMA));
    }

    if (!consume_tok(p, TOKEN_RBRACE, "expected '}' after enum variants")) {
      had_error = true;
    }
  } else {
    error_at(p, current_tok_span(p), "expected '{' after enum name");
    had_error = true;
  }

  if (had_error) {
    return ast_decl(DECL_POISON,
                    span_merge(token_span(&enum_tok), previous_tok_span(p)),
                    p->al);
  }

  Decl *decl = ast_decl(DECL_ENUM, token_span(&enum_tok), p->al);
  decl->name_span = name_span;
  decl->is_pub = is_pub;
  DeclEnum *enum_decl = &decl->as.enum_decl;
  enum_decl->name = name_sv;
  enum_decl->type_param_count = type_param_count;
  enum_decl->type_params = type_params;
  enum_decl->variants = PLIST_TAKE(p, variants);
  enum_decl->variant_count = variants_count;
  return decl;
}

static Decl *parse_use_decl(Parser *p) {
  if (!consume_tok(p, TOKEN_USE, "expected 'use'")) {
    return ast_decl(DECL_POISON, current_tok_span(p), p->al);
  }
  Token use_tok = *previous_tok(p);

  UseTarget target = {0};
  Path path = {0};
  bool bare = false;

  if (!parse_path(p, PATH_USE, &path)) {
    error_at(p, current_tok_span(p), "expected path in use declaration");
    return ast_decl(DECL_POISON, token_span(&use_tok), p->al);
  }

  if (match_tok(p, TOKEN_STAR)) {
    // `use a::E::*;` — the enum's every variant, each under its own name. No
    // alias list to build and no `as` to apply, since the names are the enum's.
    target.is_glob = true;
    target.span = span_merge(path.span, previous_tok_span(p));
    if (check_tok(p, TOKEN_AS)) {
      error_at(p, current_tok_span(p), "cannot rename a glob import");
      return ast_decl(DECL_POISON, token_span(&use_tok), p->al);
    }
  } else if (match_tok(p, TOKEN_LBRACE)) {
    PLIST(UseAlias, aliases, 8);

    // item list: use foo::{...}
    if (!check_tok(p, TOKEN_RBRACE)) {
      do {
        if (match_tok(p, TOKEN_COMMA)) {
          break;
        }
        if (!consume_tok(p, TOKEN_IDENT, "expected identifier in use glob")) {
          return ast_decl(DECL_POISON, token_span(&use_tok), p->al);
        }
        PLIST_GROW(p, aliases);
        aliases[aliases_count].name = previous_tok(p)->lexeme;
        Span item_span = previous_tok_span(p);
        if (match_tok(p, TOKEN_AS)) {
          if (!consume_tok(p, TOKEN_IDENT, "expected identifier after 'as'")) {
            return ast_decl(DECL_POISON, token_span(&use_tok), p->al);
          }
          aliases[aliases_count].alias = previous_tok(p)->lexeme;
          item_span = span_merge(item_span, previous_tok_span(p));
        } else {
          aliases[aliases_count].alias = aliases[aliases_count].name;
        }
        aliases[aliases_count].span = item_span;
        aliases_count++;
      } while (match_tok(p, TOKEN_COMMA));
    }

    if (!consume_tok(p, TOKEN_RBRACE, "expected '}' after use glob")) {
      return ast_decl(DECL_POISON, token_span(&use_tok), p->al);
    }

    target.aliases = PLIST_TAKE(p, aliases);
    target.count = aliases_count;
  } else {
    // Bare form (`use a::b;`). The trailing name stays on `path`: it may be an
    // item of module `a` or the module `a::b` itself, and only file existence
    // decides — so `mod_collect_imports` disambiguates. The alias here is the
    // name whichever it turns out to be gets bound under.
    bare = true;
    StringView last = path.segments[path.count - 1].name;
    StringView alias_name = last;
    Span item_span = path.span;
    if (match_tok(p, TOKEN_AS)) {
      if (!consume_tok(p, TOKEN_IDENT, "expected identifier after 'as'")) {
        return ast_decl(DECL_POISON, token_span(&use_tok), p->al);
      }
      alias_name = previous_tok(p)->lexeme;
      item_span = span_merge(item_span, previous_tok_span(p));
    }

    target.aliases = al_alloc_zero(p->al, sizeof(UseAlias));
    target.aliases[0].name = last;
    target.aliases[0].alias = alias_name;
    target.aliases[0].span = item_span;
    target.count = 1;
  }

  if (!consume_tok(p, TOKEN_SEMICOLON, "expected ';' after use declaration")) {
    return ast_decl(DECL_POISON,
                    span_merge(token_span(&use_tok), previous_tok_span(p)),
                    p->al);
  }

  Decl *decl = ast_decl(DECL_USE, token_span(&use_tok), p->al);
  decl->as.use_decl.path = path;
  decl->as.use_decl.target = target;
  decl->as.use_decl.bare = bare;
  return decl;
}

// `mod x;` — a child module. One identifier and a semicolon: where the source
// lives is derived from the tree, so there is nothing else to write down.
static Decl *parse_mod_decl(Parser *p) {
  if (!consume_tok(p, TOKEN_MOD, "expected 'mod'")) {
    return ast_decl(DECL_POISON, current_tok_span(p), p->al);
  }
  Token mod_tok = *previous_tok(p);

  if (!consume_tok(p, TOKEN_IDENT, "expected module name after 'mod'")) {
    return ast_decl(DECL_POISON, token_span(&mod_tok), p->al);
  }
  StringView name = previous_tok(p)->lexeme;
  Span name_span = previous_tok_span(p);

  if (!consume_tok(p, TOKEN_SEMICOLON, "expected ';' after module name")) {
    return ast_decl(DECL_POISON,
                    span_merge(token_span(&mod_tok), previous_tok_span(p)),
                    p->al);
  }

  Decl *decl =
      ast_decl(DECL_MOD, span_merge(token_span(&mod_tok), name_span), p->al);
  decl->as.mod_decl.name = name;
  decl->as.mod_decl.name_span = name_span;
  return decl;
}

static Decl *parse_trait_decl(Parser *p, bool is_pub) {
  if (!consume_tok(p, TOKEN_TRAIT, "expected 'trait'")) {
    return ast_decl(DECL_POISON, current_tok_span(p), p->al);
  }
  Token trait_tok = *previous_tok(p);

  StringView name_sv = {0};
  Span name_span = {0};
  if (!consume_tok(p, TOKEN_IDENT, "expected trait name")) {
    return ast_decl(DECL_POISON, token_span(&trait_tok), p->al);
  } else {
    name_sv = previous_tok(p)->lexeme;
    name_span = token_span(previous_tok(p));
  }

  TypeParamNode *type_params = NULL;
  int type_param_count = 0;
  if (check_tok(p, TOKEN_LT)) {
    type_param_count = parse_type_params(p, &type_params);
    if (type_param_count < 0) {
      return ast_decl(DECL_POISON, token_span(&trait_tok), p->al);
    }
  }

  // supertraits, spelled exactly like a type parameter's bound because that is
  // what they are — a bound on `Self`, which has no `<..>` list to sit in.
  TraitBound supers = {.ref_count = 0, .refs = NULL};
  if (match_tok(p, TOKEN_COLON) && !parse_trait_bound(p, &supers)) {
    return ast_decl(DECL_POISON, token_span(&trait_tok), p->al);
  }

  // PLIST zeroes, which this relies on: an associated-type item fills in only
  // kind/name/span, so every method-only field has to start NULL rather than
  // hold whatever the stack had — nothing reads them for an assoc item, but
  // nothing should have to know that.
  PLIST(TraitItemNode, items, 16);
  bool had_error = false;

  if (!consume_tok(p, TOKEN_LBRACE, "expected '{' after trait name")) {
    had_error = true;
  }

  if (!check_tok(p, TOKEN_RBRACE)) {
    do {
      PLIST_GROW(p, items);

      if (match_tok(p, TOKEN_TYPE)) {
        Span start_span = previous_tok_span(p);
        // associated type

        if (!consume_tok(p, TOKEN_IDENT, "expected identifier after 'type'")) {
          had_error = true;
        }

        StringView assoc_type_name = previous_tok(p)->lexeme;

        if (!consume_tok(p, TOKEN_SEMICOLON,
                         "expected ';' after associated type")) {
          had_error = true;
        }

        items[items_count].kind = TRAIT_ITEM_ASSOC_TYPE;
        items[items_count].name = assoc_type_name;
        items[items_count].span = span_merge(start_span, previous_tok_span(p));
        items_count++;
      } else if (match_tok(p, TOKEN_FUN)) {
        Token fun_tok = *previous_tok(p);

        StringView name_sv = {0};
        if (!consume_tok(p, TOKEN_IDENT, "expected function name")) {
          had_error = true;
        } else {
          name_sv = previous_tok(p)->lexeme;
        }

        TypeParamNode *type_params = NULL;
        int type_param_count = 0;
        if (check_tok(p, TOKEN_LT)) {
          type_param_count = parse_type_params(p, &type_params);
          if (type_param_count < 0) {
            had_error = true;
          }
        }

        if (!consume_tok(p, TOKEN_LPAREN, "expected '(' after function name")) {
          had_error = true;
          sync_to_fun_body(p);
        }

        ParamDeclNode *params = NULL;
        int param_count = 0;

        if (!parse_param_list(p, &params, &param_count)) {
          had_error = true;
          sync_to_fun_body(p);
        }

        if (!consume_tok(p, TOKEN_RPAREN, "expected ')'")) {
          had_error = true;
          sync_to_fun_body(p);
        }

        TypeNode *return_type = NULL;
        if (match_tok(p, TOKEN_THIN_ARROW)) {
          return_type = parse_type(p);
          assert(return_type && "return type should not be NULL");
          if (return_type->kind == TYNODE_POISON) {
            had_error = true;
          }
        }

        // a `where` on a trait method's signature constrains `Self`'s
        // associated types (`where Self.Item: Ord`) or the method's own type
        // parameters — see resolve_self_assoc_bounds.
        WhereClause *where_clause = NULL;
        if (check_tok(p, TOKEN_WHERE)) {
          if (!parse_where_clause(p, &where_clause, /*allow_self=*/true)) {
            had_error = true;
          }
        }

        Expr *body = NULL;
        if (match_tok(p, TOKEN_SEMICOLON)) {
          // no default implementation
        } else if (match_tok(p, TOKEN_FAT_ARROW)) {
          body = parse_expr(p);
          assert(body && "function body should not be NULL");
          if (body->kind == EXPR_POISON) {
            had_error = true;
          }
          if (!consume_tok(
                  p, TOKEN_SEMICOLON,
                  "expected ';' after default method implementation")) {
            had_error = true;
          }
        } else {
          p->panic_mode = false;
          body = parse_block(p);
          assert(body && "function body should not be NULL");
          if (body->kind == EXPR_POISON) {
            had_error = true;
          }
        }
        if (!had_error) {
          items[items_count].kind = TRAIT_ITEM_METHOD;
          items[items_count].name = name_sv;
          items[items_count].span =
              span_merge(token_span(&fun_tok), previous_tok_span(p));
          items[items_count].type_param_count = type_param_count;
          items[items_count].type_params = type_params;
          items[items_count].param_count = param_count;
          items[items_count].params = params;
          items[items_count].return_type = return_type;
          items[items_count].where_clause = where_clause;
          items[items_count].default_body = body;
          items_count++;
        }
      } else {
        advance_tok(p);
        error_at(p, current_tok_span(p), "expected trait item");
        had_error = true;
      }
    } while (!check_tok(p, TOKEN_RBRACE));
  }

  if (!consume_tok(p, TOKEN_RBRACE, "expected '}' after trait body")) {
    had_error = true;
  }

  if (had_error) {
    return ast_decl(DECL_POISON,
                    span_merge(token_span(&trait_tok), previous_tok_span(p)),
                    p->al);
  }

  Decl *decl = ast_decl(DECL_TRAIT, token_span(&trait_tok), p->al);
  decl->name_span = name_span;
  decl->is_pub = is_pub;
  DeclTrait *trait_decl = &decl->as.trait_decl;
  trait_decl->name = name_sv;
  trait_decl->type_params = type_params;
  trait_decl->type_param_count = type_param_count;
  trait_decl->supers = supers;
  trait_decl->items = PLIST_TAKE(p, items);
  trait_decl->item_count = items_count;
  return decl;
}

static Decl *parse_impl_decl(Parser *p, bool is_pub) {
  if (!consume_tok(p, TOKEN_IMPL, "expected 'impl'")) {
    return ast_decl(DECL_POISON, current_tok_span(p), p->al);
  }
  Token impl_tok = *previous_tok(p);
  bool had_error = false;

  TypeParamNode *type_params = NULL;
  int type_param_count = 0;
  if (check_tok(p, TOKEN_LT)) {
    type_param_count = parse_type_params(p, &type_params);
    if (type_param_count < 0) {
      had_error = true;
    }
  }

  TypeNode *target = parse_type(p);
  TypeNode *trait = NULL;
  if (target->kind == TYNODE_POISON) {
    had_error = true;
  }

  if (match_tok(p, TOKEN_FOR)) {
    trait = target;
    target = parse_type(p);
    if (trait->kind == TYNODE_POISON) {
      had_error = true;
    }
  }

  PLIST(ImplItemNode, items, 16);

  WhereClause *where_clause = NULL;
  if (check_tok(p, TOKEN_WHERE)) {
    if (!parse_where_clause(p, &where_clause, /*allow_self=*/false)) {
      had_error = true;
    }
  }

  if (!consume_tok(p, TOKEN_LBRACE, "expected '{' after impl header")) {
    had_error = true;
  }

  if (!check_tok(p, TOKEN_RBRACE)) {
    do {
      if (match_tok(p, TOKEN_TYPE)) {
        Span start_span = previous_tok_span(p);

        if (!consume_tok(p, TOKEN_IDENT, "expected identifier after 'type'")) {
          had_error = true;
        }

        StringView assoc_type_name = previous_tok(p)->lexeme;

        if (!consume_tok(p, TOKEN_EQ,
                         "expected '=' after associated type name")) {
          had_error = true;
        }

        TypeNode *ty = parse_type(p);
        if (ty->kind == TYNODE_POISON) {
          had_error = true;
          break;
        }

        if (!consume_tok(p, TOKEN_SEMICOLON,
                         "expected ';' after associated type")) {
          had_error = true;
        }

        PLIST_GROW(p, items);
        items[items_count].kind = IMPL_ITEM_ASSOC_TYPE;
        items[items_count].name = assoc_type_name;
        items[items_count].assoc_type = ty;
        items[items_count].span = span_merge(start_span, ty->span);
        items_count++;
      } else if (check_tok(p, TOKEN_AT) || check_tok(p, TOKEN_FUN)) {
        // a method may carry `@native`/`@intrinsic` just like a top-level fun:
        // the attribute *is* its body, so a primitive's operation can be
        // spelled `s.len()` rather than as a free `string::len(s)`. `self` is
        // an ordinary parameter to the native, so nothing downstream of the
        // signature has to change. `@allow` sits here too, one scope inside the
        // impl's own.
        AttrNode attr, lang_attr;
        unsigned allow_mask;
        if (!parse_attrs(p, &attr, &lang_attr, &allow_mask)) {
          had_error = true;
          break;
        }
        if (lang_attr.kind != ATTR_NONE) {
          // a method is never a lang item — those are top-level.
          error_at(p, lang_attr.span,
                   "'@lang' can only mark a trait, enum, or function");
        }
        Decl *fun_decl = parse_fun_decl(p, false, attr);
        if (fun_decl->kind == DECL_POISON) {
          had_error = true;
        } else {
          fun_decl->allow_mask = allow_mask;
          PLIST_GROW(p, items);
          items[items_count].kind = IMPL_ITEM_METHOD;
          items[items_count].name = fun_decl->as.fun_decl.name;
          items[items_count].fun_decl = fun_decl;
          items[items_count].span = fun_decl->span;
          items_count++;
        }
      } else {
        advance_tok(p);
        error_at(p, previous_tok_span(p), "expected impl item");
        had_error = true;
        break;
      }
    } while (!check_tok(p, TOKEN_RBRACE));
  }

  if (!consume_tok(p, TOKEN_RBRACE, "expected '}' after impl body")) {
    had_error = true;
  }

  if (had_error) {
    return ast_decl(DECL_POISON,
                    span_merge(token_span(&impl_tok), previous_tok_span(p)),
                    p->al);
  }

  Decl *decl = ast_decl(DECL_IMPL, token_span(&impl_tok), p->al);
  decl->is_pub = is_pub;
  DeclImpl *impl_decl = &decl->as.impl_decl;
  impl_decl->self_type = target;
  impl_decl->trait_type = trait;
  impl_decl->type_params = type_params;
  impl_decl->type_param_count = type_param_count;
  impl_decl->where_clause = where_clause;
  impl_decl->items = PLIST_TAKE(p, items);
  impl_decl->item_count = items_count;
  return decl;
}

static Decl *parse_decl(Parser *p) {
  Decl *decl = NULL;

  // attributes precede `pub`; the decl parsers below only ever see the body
  // attribute, since the other two apply to whatever kind of decl follows.
  AttrNode attr;      // body attribute
  AttrNode lang_attr; // marker
  unsigned allow_mask;
  if (!parse_attrs(p, &attr, &lang_attr, &allow_mask)) {
    sync_to_decl(p);
    return ast_decl(DECL_POISON, previous_tok_span(p), p->al);
  }

  bool is_pub = match_tok(p, TOKEN_PUB);

  if (check_tok(p, TOKEN_USE)) {
    // `pub use` re-exports: the alias becomes an item of *this* module, so an
    // importer may name it without knowing where it originally came from.
    decl = parse_use_decl(p);
    decl->is_pub = is_pub;
  } else if (check_tok(p, TOKEN_MOD)) {
    // `pub mod` exports the child: a path from outside this module's subtree
    // may walk through it.
    decl = parse_mod_decl(p);
    decl->is_pub = is_pub;
  } else {
    if (check_tok(p, TOKEN_VAR)) {
      decl = parse_var_decl(p, is_pub);
    } else if (check_tok(p, TOKEN_FUN)) {
      decl = parse_fun_decl(p, is_pub, attr);
    } else if (check_tok(p, TOKEN_STRUCT)) {
      decl = parse_struct_decl(p, is_pub);
    } else if (check_tok(p, TOKEN_ENUM)) {
      decl = parse_enum_decl(p, is_pub);
    } else if (check_tok(p, TOKEN_TRAIT)) {
      decl = parse_trait_decl(p, is_pub);
    } else if (check_tok(p, TOKEN_IMPL)) {
      decl = parse_impl_decl(p, is_pub);
    } else {
      advance_tok(p);
      error_at(p, previous_tok_span(p), "expected declaration");
      decl = ast_decl(DECL_POISON, previous_tok_span(p), p->al);
    }
  }

  if (attr.kind != ATTR_NONE && decl->kind != DECL_FUN &&
      decl->kind != DECL_POISON) {
    error_at(p, attr.span, "only a function can be '@native' or '@intrinsic'");
  }

  // an allow needs no placement rule: it covers whatever the declaration
  // contains, and a declaration that contains nothing it names is silenced
  // about nothing.
  decl->allow_mask = allow_mask;

  // a marker may sit on the definitions a lang item can be — a trait, an enum,
  // or a function — and nowhere else.
  if (lang_attr.kind != ATTR_NONE && decl->kind != DECL_POISON) {
    if (decl->kind == DECL_FUN || decl->kind == DECL_TRAIT ||
        decl->kind == DECL_ENUM) {
      decl->lang_attr = lang_attr;
    } else {
      error_at(p, lang_attr.span,
               "'@lang' can only mark a trait, enum, or function");
    }
  }

  if (p->panic_mode) {
    sync_to_decl(p);
  }

  return decl;
}

// public API

void parser_init(Parser *p, Token *tokens, int count, Allocator *al,
                 DiagBag *diags) {
  p->tokens = tokens;
  p->count = count;
  p->current = 0;
  p->al = al;
  p->diags = diags;
  p->panic_mode = false;
  p->allow_struct_init = true;
}

Program *parser_parse(Parser *p) {
  // this was a 1024-entry stack array guarded by an `assert`, and an assert is
  // not a guard: the release build defines NDEBUG, so a module with a 1025th
  // declaration wrote past the array and took the compiler down with SIGSEGV
  // rather than a diagnostic. A cap on how much a file may contain has no
  // defensible value anyway, so it grows instead of reporting.
  PLIST(Decl *, decls, 64);

  while (!is_at_end(p)) {
    Decl *decl = parse_decl(p);
    assert(decl && "declaration should not be NULL");
    PLIST_PUSH(p, decls, decl);
  }

  Program *prog = al_alloc_for(p->al, Program);
  prog->decls = PLIST_TAKE(p, decls);
  prog->decl_count = decls_count;

  return prog;
}
