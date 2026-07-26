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

// how many associated types one `dyn Trait<..>` may bind. A trait is free to
// declare more (its item cap is 64) — it just cannot then be a trait object,
// which is reported here rather than as an incomplete binding list, since the
// list is required to be total.
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
    case TOKEN_RBRACE: // end of the enclosing block
    case TOKEN_WHILE:
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
// associated-type bindings (`<Int, Item = String>`). Writes each list into its
// out-parameter and returns false on a parse error.
static bool parse_dyn_args(Parser *p, TypeNode **out_args, int *out_arg_count,
                           AssocBindingNode *out_bindings,
                           int *out_binding_count);

// a `var` binding is a pattern; the checker rejects the refutable ones.
static Pattern *parse_pattern(Parser *p);

static TypeNode *parse_type(Parser *p) {
  TypeNode *base = NULL;

  if (match_tok(p, TOKEN_LPAREN)) {
    if (match_tok(p, TOKEN_RPAREN)) {
      // unit type
      return ast_type_node(
          TYNODE_UNIT, span_merge(previous_tok_span(p), previous_tok_span(p)),
          p->al);
    }

    // tuple type
    TypeNode *elem_types[16];
    int elem_count = 0;
    bool had_error = false;

    do {
      if (elem_count >= 16) {
        error_at(p, current_tok_span(p), "too many types in tuple");
        had_error = true;
        break;
      }
      TypeNode *ty = parse_type(p);
      if (ty->kind == TYNODE_POISON) {
        had_error = true;
        break;
      }
      elem_types[elem_count++] = ty;
    } while (match_tok(p, TOKEN_COMMA));

    if (had_error) {
      return ast_type_node(TYNODE_POISON, current_tok_span(p), p->al);
    }

    if (!consume_tok(p, TOKEN_RPAREN, "expected ')' after tuple type")) {
      return ast_type_node(TYNODE_POISON, current_tok_span(p), p->al);
    }

    if (elem_count == 1) {
      return elem_types[0];
    }

    base = ast_type_node(
        TYNODE_TUPLE, span_merge(token_span(peek_tok(p)), previous_tok_span(p)),
        p->al);
    base->as.tuple.elems = al_alloc(p->al, sizeof(TypeNode *) * elem_count);
    memcpy(base->as.tuple.elems, elem_types, sizeof(TypeNode *) * elem_count);
    base->as.tuple.count = elem_count;
  }

  if (match_tok(p, TOKEN_FUN)) {
    Token start = *previous_tok(p);
    consume_tok(p, TOKEN_LPAREN, "expected '(' after 'fun' in type");

    TypeNode *param_types[16];
    int param_count = 0;
    bool had_error = false;

    if (!check_tok(p, TOKEN_RPAREN)) {
      do {
        if (param_count >= 16) {
          error_at(p, current_tok_span(p),
                   "too many parameters in function type");
          had_error = true;
          break;
        }
        TypeNode *ty = parse_type(p);
        if (ty->kind == TYNODE_POISON) {
          had_error = true;
          break;
        }
        param_types[param_count++] = ty;
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

    base->as.fun.param_count = param_count;
    base->as.fun.param_types =
        al_alloc(p->al, sizeof(TypeNode *) * param_count);
    memcpy(base->as.fun.param_types, param_types,
           sizeof(TypeNode *) * param_count);
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
  // `dyn Iterator<Item = Int>` pins each associated type the trait declares,
  // and `dyn Into<Int>` supplies the trait's own type parameter. The path is
  // parsed *bare* so the '<' lands here rather than in `parse_type_args`: what
  // follows is one bracket list holding both, and reading it as a plain
  // type-argument list would report "expected '>'" at the `=`.
  if (match_tok(p, TOKEN_DYN)) {
    Token start = *previous_tok(p);

    Path path = {0};
    if (!parse_path(p, PATH_BARE, &path)) {
      return ast_type_node(TYNODE_POISON, current_tok_span(p), p->al);
    }

    TypeNode *type_args[MAX_ASSOC_BINDINGS];
    AssocBindingNode bindings[MAX_ASSOC_BINDINGS];
    int type_arg_count = 0, binding_count = 0;
    if (check_tok(p, TOKEN_LT) && !parse_dyn_args(p, type_args, &type_arg_count,
                                                  bindings, &binding_count)) {
      return ast_type_node(TYNODE_POISON, current_tok_span(p), p->al);
    }

    base = ast_type_node(TYNODE_DYN,
                         span_merge(token_span(&start), previous_tok_span(p)),
                         p->al);
    base->as.dyn.path = path;
    base->as.dyn.type_args =
        al_alloc(p->al, sizeof(TypeNode *) * type_arg_count);
    memcpy(base->as.dyn.type_args, type_args,
           sizeof(TypeNode *) * type_arg_count);
    base->as.dyn.type_arg_count = type_arg_count;
    base->as.dyn.bindings =
        al_alloc(p->al, sizeof(AssocBindingNode) * binding_count);
    memcpy(base->as.dyn.bindings, bindings,
           sizeof(AssocBindingNode) * binding_count);
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

static bool parse_dyn_args(Parser *p, TypeNode **out_args, int *out_arg_count,
                           AssocBindingNode *out_bindings,
                           int *out_binding_count) {
  if (!consume_tok(p, TOKEN_LT, "expected '<' after the trait's name")) {
    return false;
  }

  *out_arg_count = 0;
  *out_binding_count = 0;
  do {
    if (*out_arg_count >= MAX_ASSOC_BINDINGS ||
        *out_binding_count >= MAX_ASSOC_BINDINGS) {
      error_at(p, current_tok_span(p), "too many type arguments");
      return false;
    }

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
      out_bindings[(*out_binding_count)++] = (AssocBindingNode){
          .name = name.lexeme,
          .type = ty,
          .span = span_merge(token_span(&name), previous_tok_span(p)),
      };
      continue;
    }

    // a type argument after a binding would leave the two lists interleaved,
    // and the trait's parameters are positional — so order is not a style
    // question here.
    if (*out_binding_count > 0) {
      error_at(p, current_tok_span(p),
               "a trait's type arguments come before its associated-type "
               "bindings, as in 'dyn Trait<Int, Item = String>'");
      return false;
    }

    TypeNode *ty = parse_type(p);
    if (ty->kind == TYNODE_POISON) {
      return false;
    }
    out_args[(*out_arg_count)++] = ty;
  } while (match_tok(p, TOKEN_COMMA));

  if (!consume_tok(p, TOKEN_GT, "expected '>' after the trait's arguments")) {
    return false;
  }
  return true;
}

static int parse_type_args(Parser *p, TypeNode ***out_args);

static bool parse_path(Parser *p, PathParseMode mode, Path *out) {
  PathSegment segments[8];
  int segment_count = 0;
  Token *start_tok = current_tok(p);

  if (check_tok(p, TOKEN_IDENT) || check_tok(p, TOKEN_SELF_TYPE)) {
    do {
      if (segment_count >= 8) {
        error_at(p, current_tok_span(p), "too many segments in path");
        return false;
      }
      if (mode == PATH_USE && check_tok(p, TOKEN_LBRACE)) {
        break;
      }
      // 'Self' may only lead a path; later segments must be plain idents.
      if (segment_count == 0 && check_tok(p, TOKEN_SELF_TYPE)) {
        advance_tok(p);
      } else if (!consume_tok(p, TOKEN_IDENT, "expected identifier in path")) {
        return false;
      }
      PathSegment *segment = &segments[segment_count++];
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
  out->segments = al_alloc(p->al, sizeof(PathSegment) * segment_count);
  memcpy(out->segments, segments, sizeof(PathSegment) * segment_count);
  out->count = segment_count;
  out->span = span_merge(token_span(start_tok), previous_tok_span(p));

  return true;
}

static Expr *parse_assign(Parser *p);
static Expr *parse_range(Parser *p);
static Expr *parse_or(Parser *p);
static Expr *parse_and(Parser *p);
static Expr *parse_equality(Parser *p);
static Expr *parse_comparison(Parser *p);
static Expr *parse_addition(Parser *p);
static Expr *parse_multiply(Parser *p);
static Expr *parse_unary(Parser *p);
static Expr *parse_postfix(Parser *p);
static Expr *parse_primary(Parser *p);

static Expr *parse_expr(Parser *p) { return parse_assign(p); }

static Expr *parse_assign(Parser *p) {
  Expr *lhs = parse_range(p);
  if (lhs->kind == EXPR_POISON) {
    return lhs;
  }

  if (match_tok(p, TOKEN_EQ) || match_tok(p, TOKEN_PLUSEQ) ||
      match_tok(p, TOKEN_MINUSEQ) || match_tok(p, TOKEN_STAREQ) ||
      match_tok(p, TOKEN_SLASHEQ)) {
    Token *op = previous_tok(p);
    Expr *rhs = parse_assign(p);
    if (rhs->kind == EXPR_POISON) {
      return rhs;
    }

    Expr *e = ast_expr(EXPR_ASSIGN, span_merge(lhs->span, rhs->span), p->al);
    e->as.assign.target = lhs;
    e->as.assign.value = rhs;
    e->as.assign.op = op->type;
    return e;
  }

  return lhs;
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
  return parse_binary_left(p, parse_addition, ops);
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
  if (match_tok(p, TOKEN_NOT) || match_tok(p, TOKEN_MINUS)) {
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
      Expr *args[16];
      int argc = 0;
      if (!check_tok(p, TOKEN_RPAREN)) {
        do {
          if (argc >= 16) {
            error_at(p, current_tok_span(p), "too many arguments");
            break;
          }
          args[argc++] = parse_expr(p);
        } while (match_tok(p, TOKEN_COMMA));
      }
      consume_tok(p, TOKEN_RPAREN, "expected ')' after arguments");
      Expr **arg_array = al_alloc(p->al, sizeof(Expr *) * argc);
      memcpy(arg_array, args, sizeof(Expr *) * argc);
      Span span = span_merge(base->span, previous_tok_span(p));
      Expr *expr = ast_expr(EXPR_CALL, span, p->al);
      expr->as.call.callee = base;
      expr->as.call.args = arg_array;
      expr->as.call.arg_count = argc;
      base = expr;
    } else if (match_tok(p, TOKEN_DOT)) {
      if (match_tok(p, TOKEN_IDENT)) {
        Token name = *previous_tok(p);

        TypeNode *type_args[8];
        int type_arg_count = 0;
        Span start_span = current_tok_span(p);

        if (match_tok(p, TOKEN_LT)) {
          bool had_error = false;

          if (!check_tok(p, TOKEN_GT)) {
            do {
              if (type_arg_count >= 8) {
                error_at(p, span_merge(start_span, previous_tok_span(p)),
                         "too many type arguments in method call");
                had_error = true;
                break;
              }
              TypeNode *ty = parse_type(p);
              if (ty->kind == TYNODE_POISON) {
                had_error = true;
                break;
              }
              type_args[type_arg_count++] = ty;
            } while (match_tok(p, TOKEN_COMMA));
          }

          consume_tok(p, TOKEN_GT, "expected '>' after type arguments");

          if (type_arg_count == 0) {
            error_at(p, current_tok_span(p),
                     "expected at least one type argument in type application");
            had_error = true;
          }

          if (had_error) {
            return ast_expr(EXPR_POISON, current_tok_span(p), p->al);
          }
        }

        if (type_arg_count != 0 && !check_tok(p, TOKEN_LPAREN)) {
          error_at(p, span_merge(start_span, previous_tok_span(p)),
                   "unexpected type arguments");
        }

        if (match_tok(p, TOKEN_LPAREN)) {
          // method call: obj.method(args)
          Expr **args = NULL;
          int argc = 0;
          if (!check_tok(p, TOKEN_RPAREN)) {
            Expr *tmp[64];
            argc = 0;
            do {
              tmp[argc++] = parse_expr(p);
            } while (match_tok(p, TOKEN_COMMA) && !check_tok(p, TOKEN_RPAREN));
            args = al_alloc(p->al, sizeof(Expr *) * argc);
            memcpy(args, tmp, sizeof(Expr *) * argc);
          }
          consume_tok(p, TOKEN_RPAREN, "expected ')' after arguments");
          Expr *expr =
              ast_expr(EXPR_METHOD_CALL,
                       span_merge(base->span, previous_tok_span(p)), p->al);

          expr->as.method_call.object = base;
          expr->as.method_call.method_name = name.lexeme;
          expr->as.method_call.args = args;
          expr->as.method_call.arg_count = argc;
          expr->as.method_call.type_args = NULL;
          expr->as.method_call.type_arg_count = 0;
          expr->as.method_call.resolved_method = NULL;
          if (type_arg_count > 0) {
            expr->as.method_call.type_args =
                al_alloc(p->al, sizeof(TypeNode *) * type_arg_count);
            memcpy(expr->as.method_call.type_args, type_args,
                   sizeof(TypeNode *) * type_arg_count);
            expr->as.method_call.type_arg_count = type_arg_count;
          }
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
      // cast: expr as type
      TypeNode *target = parse_type(p);
      Expr *expr = ast_expr(
          EXPR_CAST, span_merge(base->span, previous_tok_span(p)), p->al);
      expr->as.cast.operand = base;
      expr->as.cast.target_type = target;
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
         check_tok(p, TOKEN_MATCH) || check_tok(p, TOKEN_WHILE);
}

static Expr *parse_block(Parser *p) {
  if (!consume_tok(p, TOKEN_LBRACE, "expected '{'")) {
    return ast_expr(EXPR_POISON, current_tok_span(p), p->al);
  };
  Span start_span = previous_tok_span(p);

  // grown rather than fixed: a block's length is bounded by nothing but the
  // source, so a cap here could only ever be an abort on a valid program.
  Stmt **tmp_stmts = NULL;
  int stmt_count = 0;
  int stmt_cap = 0;
  Expr *tail = NULL;
  bool block_had_error = false;

#define PUSH_STMT(s)                                                           \
  do {                                                                         \
    if (stmt_count == stmt_cap) {                                              \
      int new_cap = stmt_cap == 0 ? 8 : stmt_cap * 2;                          \
      tmp_stmts = al_realloc(p->al, tmp_stmts, sizeof(Stmt *) * stmt_cap,      \
                             sizeof(Stmt *) * new_cap);                        \
      stmt_cap = new_cap;                                                      \
    }                                                                          \
    tmp_stmts[stmt_count++] = (s);                                             \
  } while (0)

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
                  stmt->as.expr_stmt.expr->kind == EXPR_MATCH)) {
        // a value-bearing block expression just before `}` is the tail
        tail = stmt->as.expr_stmt.expr;
      } else {
        PUSH_STMT(stmt);
      }
    } else {
      Expr *expr = parse_expr(p);
      if (expr->kind == EXPR_POISON) {
        advance_tok(p);
        block_had_error = true;
      } else if (match_tok(p, TOKEN_SEMICOLON)) {
        Stmt *stmt = ast_stmt(STMT_EXPR, expr->span, p->al);
        stmt->as.expr_stmt.expr = expr;
        PUSH_STMT(stmt);
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
  if (stmt_count > 0) {
    // right-size: the grown buffer is up to twice what the block needs
    expr->as.block.stmts = al_alloc(p->al, sizeof(Stmt *) * stmt_count);
    memcpy(expr->as.block.stmts, tmp_stmts, sizeof(Stmt *) * stmt_count);
  }
  expr->as.block.stmt_count = stmt_count;
  expr->as.block.tail_expr = tail;
  return expr;
#undef PUSH_STMT
}

static int parse_type_args(Parser *p, TypeNode ***out_args) {
  TypeNode *args[16];
  int count = 0;
  bool had_error = false;

  if (!consume_tok(p, TOKEN_LT, "expected '<' before type arguments")) {
    had_error = true;
  } else if (!check_tok(p, TOKEN_GT)) {
    do {
      if (count >= 16) {
        error_at(p, current_tok_span(p), "too many type arguments");
        had_error = true;
        break;
      }
      TypeNode *ty = parse_type(p);
      if (ty->kind == TYNODE_POISON) {
        had_error = true;
        break;
      }
      args[count++] = ty;
    } while (match_tok(p, TOKEN_COMMA));

    if (!consume_tok(p, TOKEN_GT, "expected '>' after type arguments")) {
      had_error = true;
    }
  }

  if (had_error) {
    return 0;
  }

  *out_args = al_alloc(p->al, sizeof(TypeNode *) * count);
  memcpy(*out_args, args, sizeof(TypeNode *) * count);
  return count;
}

static bool parse_trait_ref(Parser *p, TraitRef *out) {
  if (!check_tok(p, TOKEN_IDENT)) {
    error_at(p, current_tok_span(p), "expected trait name in trait bound");
    return false;
  }
  Token *start_tok = previous_tok(p);

  if (!parse_path(p, PATH_TYPE, &out->path)) {
    return false;
  }
  out->span = span_merge(token_span(start_tok), previous_tok_span(p));

  return true;
}

static bool parse_bound_lhs(Parser *p, WhereLhs *out) {
  if (!consume_tok(p, TOKEN_IDENT,
                   "expected type parameter name in where clause predicate")) {
    return false;
  }
  Span start = previous_tok_span(p);
  bool had_error = false;

  StringView segments[4];
  int segment_count = 1;
  segments[0] = previous_tok(p)->lexeme;

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
      if (segment_count >= 4) {
        error_at(p, current_tok_span(p),
                 "too many segments in type parameter path in where clause");
        had_error = true;
        break;
      }
      segments[segment_count++] = previous_tok(p)->lexeme;
    } while (check_tok(p, TOKEN_DOT));
  }
  if (had_error) {
    return false;
  }
  out->segment_count = segment_count;
  out->segments = al_alloc(p->al, sizeof(StringView) * segment_count);
  memcpy(out->segments, segments, sizeof(StringView) * segment_count);
  // the whole path, so a diagnostic about `T.Item` underlines both segments.
  // Until milestone 52 nothing reachable read this span, so it went unset.
  out->span = span_merge(start, previous_tok_span(p));
  return true;
}

static bool parse_trait_bound(Parser *p, TraitBound *out) {
  TraitRef refs[8];
  int ref_count = 0;
  bool had_error = false;

  do {
    if (ref_count >= 8) {
      error_at(p, current_tok_span(p), "too many trait bounds");
      had_error = true;
      break;
    }
    had_error = had_error || !parse_trait_ref(p, &refs[ref_count++]);
  } while (!had_error && match_tok(p, TOKEN_PLUS));

  if (had_error) {
    return false;
  }

  out->ref_count = ref_count;
  out->refs = al_alloc(p->al, sizeof(TraitRef) * ref_count);
  memcpy(out->refs, refs, sizeof(TraitRef) * ref_count);
  return !had_error;
}

static bool parse_where_clause(Parser *p, WhereClause **out) {
  if (!consume_tok(p, TOKEN_WHERE, "expected 'where'")) {
    return false;
  }

  Token where_tok = *previous_tok(p);

  WherePred preds[8];
  int pred_count = 0;
  bool had_error = false;

  do {
    if (check_tok(p, TOKEN_LBRACE)) {
      break;
    }
    if (pred_count >= 8) {
      error_at(p, current_tok_span(p), "too many predicates in where clause");
      had_error = true;
      break;
    }

    WherePred *pred = &preds[pred_count++];
    had_error = had_error || !parse_bound_lhs(p, &pred->lhs);
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
  (*out)->pred_count = pred_count;
  (*out)->preds = al_alloc(p->al, sizeof(WherePred) * pred_count);
  memcpy((*out)->preds, preds, sizeof(WherePred) * pred_count);

  (*out)->span = span_merge(token_span(&where_tok), current_tok_span(p));

  return true;
}

static int parse_type_params(Parser *p, TypeParamNode **out) {
  if (!consume_tok(p, TOKEN_LT, "expected '<'"))
    return -1;

  TypeParamNode tmp[8];
  int count = 0;
  bool had_error = false;

  do {
    if (count >= 8) {
      error_at(p, current_tok_span(p), "too many type parameters");
      had_error = true;
      break;
    }

    if (!consume_tok(p, TOKEN_IDENT, "expected type parameter name")) {
      had_error = true;
      break;
    }

    Token name_tok = *previous_tok(p);
    TypeParamNode *param = &tmp[count++];
    param->name = name_tok.lexeme;
    // param->span = token_span(&name_tok);

    if (match_tok(p, TOKEN_COLON)) {
      had_error = had_error || !parse_trait_bound(p, &param->inline_bound);
    } else {
      param->inline_bound = (TraitBound){.ref_count = 0, .refs = NULL};
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

  *out = al_alloc(p->al, sizeof(TypeParamNode) * count);
  memcpy(*out, tmp, sizeof(TypeParamNode) * count);
  return count;
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

static bool parse_closure_param_list(Parser *p, ClosureParam *out, int *count,
                                     int max) {
  *count = 0;
  bool had_error = false;

  if (check_tok(p, TOKEN_PIPE))
    return true;

  do {
    if (check_tok(p, TOKEN_PIPE))
      break;

    if (*count >= max) {
      error_at(p, current_tok_span(p), "too many parameters");
      had_error = true;
      break;
    }

    ClosureParam param = parse_closure_param(p);
    if (param.name.len == 0 && !param.is_self) {
      had_error = true;
      break;
    }
    out[(*count)++] = param;

  } while (match_tok(p, TOKEN_COMMA));

  return !had_error;
}

// closure: | params | ( -> type )? ( => expr | { block } )
static Expr *parse_closure(Parser *p) {
  Token start = *previous_tok(p);
  bool had_error = false;

  ClosureParam params[16];
  int param_count = 0;

  if (!check_tok(p, TOKEN_PIPE)) {
    if (!parse_closure_param_list(p, params, &param_count, 16)) {
      had_error = true;
      sync_to_fun_body(p);
    }
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
  } else if (consume_tok(p, TOKEN_FAT_ARROW,
                         "expected '=>' or '{' before closure body")) {
    body = parse_expr(p);
  } else {
    had_error = true;
    body = ast_expr(EXPR_POISON, current_tok_span(p), p->al);
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
  closure->params = al_alloc(p->al, sizeof(ClosureParam) * param_count);
  memcpy(closure->params, params, sizeof(ClosureParam) * param_count);
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
      Pattern *subpats[16];
      int subpat_count = 0;
      bool had_error = false;

      do {
        if (check_tok(p, TOKEN_RPAREN)) {
          break;
        }
        if (subpat_count >= 16) {
          error_at(p, current_tok_span(p), "too many subpatterns");
          had_error = true;
          break;
        }
        Pattern *subpat = parse_pattern(p);
        if (!subpat) {
          had_error = true;
          break;
        }
        subpats[subpat_count++] = subpat;
      } while (match_tok(p, TOKEN_COMMA));

      if (!consume_tok(p, TOKEN_RPAREN, "expected ')' after tuple pattern")) {
        return NULL;
      }

      if (!had_error) {
        pattern.kind = PAT_VARIANT;
        pattern.span = span_merge(start_span, previous_tok_span(p));
        pattern.as.variant.path = path;
        pattern.as.variant.fields =
            al_alloc_zero(p->al, sizeof(FieldPat) * subpat_count);
        for (int i = 0; i < subpat_count; i++) {
          pattern.as.variant.fields[i].sub_pattern = subpats[i];
          pattern.as.variant.fields[i].ident.index = i;
        }
        pattern.as.variant.field_count = subpat_count;
      } else {
        return NULL;
      }
    } else if (match_tok(p, TOKEN_LBRACE)) {
      // struct pattern: Struct { field: pat, ... }
      FieldPat fields[16];
      int field_count = 0;
      bool had_error = false;

      do {
        if (check_tok(p, TOKEN_RBRACE)) {
          break;
        }
        if (field_count >= 16) {
          error_at(p, current_tok_span(p), "too many fields in struct pattern");
          had_error = true;
          break;
        }
        if (!consume_tok(p, TOKEN_IDENT,
                         "expected field name in struct pattern")) {
          had_error = true;
          break;
        }
        Span field_span = token_span(previous_tok(p));
        StringView field_name = previous_tok(p)->lexeme;

        if (match_tok(p, TOKEN_COLON)) {
          Pattern *field_pat = parse_pattern(p);
          if (!field_pat) {
            had_error = true;
            break;
          }
          fields[field_count].ident.name = field_name;
          fields[field_count].sub_pattern = field_pat;
          fields[field_count].span = field_span;
          field_count++;
        } else {
          fields[field_count].ident.name = field_name;
          fields[field_count].sub_pattern = NULL;
          fields[field_count].span = field_span;
          field_count++;
        }
      } while (match_tok(p, TOKEN_COMMA));

      if (!consume_tok(p, TOKEN_RBRACE, "expected '}' after struct pattern")) {
        return NULL;
      }

      if (!had_error) {
        pattern.kind = PAT_STRUCT;
        pattern.span = span_merge(start_span, previous_tok_span(p));
        pattern.as.struc.fields =
            al_alloc(p->al, sizeof(FieldPat) * field_count);
        memcpy(pattern.as.struc.fields, fields, sizeof(FieldPat) * field_count);
        pattern.as.struc.field_count = field_count;
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
    Pattern *subpats[16];
    int subpat_count = 0;
    bool had_error = false;

    if (!check_tok(p, TOKEN_RPAREN)) {
      do {
        if (subpat_count >= 16) {
          error_at(p, current_tok_span(p), "too many subpatterns");
          had_error = true;
          break;
        }
        Pattern *subpat = parse_pattern(p);
        if (!subpat) {
          had_error = true;
          break;
        }
        subpats[subpat_count++] = subpat;
      } while (match_tok(p, TOKEN_COMMA));
    }

    if (!consume_tok(p, TOKEN_RPAREN, "expected ')' after tuple pattern")) {
      return NULL;
    }

    if (!had_error) {
      pattern.kind = PAT_TUPLE;
      pattern.span = span_merge(start_span, previous_tok_span(p));
      pattern.as.tuple.elems =
          al_alloc(p->al, sizeof(Pattern *) * subpat_count);
      memcpy(pattern.as.tuple.elems, subpats, sizeof(Pattern *) * subpat_count);
      pattern.as.tuple.count = subpat_count;
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

  MatchArm tmp_arms[16];
  int arm_count = 0;

  do {
    Span arm_start_span = current_tok_span(p);
    if (check_tok(p, TOKEN_RBRACE)) {
      break;
    }

    if (arm_count >= 16) {
      error_at(p, current_tok_span(p), "too many match arms");
      had_error = true;
      sync_to_next_arm(p);
      continue;
    }

    MatchArm *arm = &tmp_arms[arm_count++];
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
  expr->as.match.arms = al_alloc(p->al, sizeof(MatchArm) * arm_count);
  memcpy(expr->as.match.arms, tmp_arms, sizeof(MatchArm) * arm_count);
  expr->as.match.arm_count = arm_count;
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
  // multiplication, and it is the Char the value is padded with either way.
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
  // failing over anyway and the value only has to be *a* Char.
  if (match_tok(p, TOKEN_CHAR)) {
    Expr *expr = ast_expr(EXPR_CHAR, token_span(t), p->al);
    uint32_t cp = 0;
    char_literal_value(t->lexeme, &cp);
    expr->as.char_val = cp;
    return expr;
  }

  if (match_tok(p, TOKEN_INTERPOLATION)) {
    InterpolSeg segs[16] = {0};
    int seg_count = 2;

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
        if (seg_count >= 16) {
          error_at(p, current_tok_span(p),
                   "too many segments in interpolated string");
          return ast_expr(EXPR_POISON, current_tok_span(p), p->al);
        }
        if (previous_tok(p)->lexeme.len > 2) {
          segs[seg_count].kind = ISEG_TEXT;
          segs[seg_count].text =
              (StringView){.len = previous_tok(p)->lexeme.len - 2,
                           .chars = previous_tok(p)->lexeme.chars + 1};
          seg_count++;
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
        if (seg_count >= 16) {
          error_at(p, current_tok_span(p),
                   "too many segments in interpolated string");
          return ast_expr(EXPR_POISON, current_tok_span(p), p->al);
        }
        segs[seg_count].kind = ISEG_TEXT;
        segs[seg_count].text =
            (StringView){.len = previous_tok(p)->lexeme.len - 2,
                         .chars = previous_tok(p)->lexeme.chars + 1};
        seg_count++;
      }

      Expr *expr = parse_expr(p);
      if (expr->kind == EXPR_POISON) {
        return ast_expr(EXPR_POISON, current_tok_span(p), p->al);
      }

      if (seg_count >= 16) {
        error_at(p, current_tok_span(p),
                 "too many segments in interpolated string");
        return ast_expr(EXPR_POISON, current_tok_span(p), p->al);
      }
      segs[seg_count].kind = ISEG_EXPR;
      segs[seg_count].expr = expr;
      if (check_tok(p, TOKEN_COLON) &&
          !parse_format_spec(p, &segs[seg_count].spec)) {
        return ast_expr(EXPR_POISON, current_tok_span(p), p->al);
      }
      seg_count++;
    }

    Expr *expr = ast_expr(EXPR_INTERPOLATED, token_span(t), p->al);
    expr->as.interpolated.segs =
        al_alloc(p->al, sizeof(InterpolSeg) * seg_count);
    memcpy(expr->as.interpolated.segs, segs, sizeof(InterpolSeg) * seg_count);
    expr->as.interpolated.seg_count = seg_count;
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

      FieldInit fields[16];
      int field_count = 0;

      if (!check_tok(p, TOKEN_RBRACE)) {
        do {
          if (check_tok(p, TOKEN_RBRACE)) {
            break;
          }
          if (field_count >= 16) {
            error_at(p, current_tok_span(p), "too many fields in struct init");
            return ast_expr(EXPR_POISON, current_tok_span(p), p->al);
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

          fields[field_count].ident.name = field_name;
          fields[field_count].value = field_value;
          fields[field_count].span =
              span_merge(token_span(field_tok), field_value->span);
          field_count++;
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
      expr->as.struct_init.field_count = field_count;
      expr->as.struct_init.fields =
          al_alloc(p->al, sizeof(expr->as.struct_init.fields[0]) * field_count);
      memcpy(expr->as.struct_init.fields, fields,
             sizeof(expr->as.struct_init.fields[0]) * field_count);
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

    Expr *elem_exprs[16];
    int elem_count = 0;
    bool had_error = false;
    bool force_tuple = false;
    bool old_allow_struct_init = p->allow_struct_init;
    p->allow_struct_init = true;

    if (!check_tok(p, TOKEN_RPAREN)) {
      do {
        if (elem_count >= 16) {
          error_at(p, current_tok_span(p), "too many elements in tuple");
          had_error = true;
          break;
        }
        if (check_tok(p, TOKEN_RPAREN)) {
          force_tuple = true;
          break;
        }
        Expr *e = parse_expr(p);
        if (e->kind == EXPR_POISON) {
          had_error = true;
          break;
        }
        elem_exprs[elem_count++] = e;
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

    if (elem_count == 1 && !force_tuple) {
      // just a grouping
      return elem_exprs[0];
    }

    Expr *expr = ast_expr(EXPR_TUPLE,
                          span_merge(start_span, previous_tok_span(p)), p->al);
    expr->as.tuple.elems = al_alloc(p->al, sizeof(Expr *) * elem_count);
    memcpy(expr->as.tuple.elems, elem_exprs, sizeof(Expr *) * elem_count);
    expr->as.tuple.count = elem_count;
    return expr;
  }

  // closure
  if (match_tok(p, TOKEN_PIPE)) {
    return parse_closure(p);
  }

  // array literal
  if (match_tok(p, TOKEN_LBRACKET)) {
    Span start_span = previous_tok_span(p);

    Expr *elem_exprs[16];
    int elem_count = 0;
    bool had_error = false;
    bool old_allow_struct_init = p->allow_struct_init;
    p->allow_struct_init = true;

    if (!check_tok(p, TOKEN_RBRACKET)) {
      do {
        if (check_tok(p, TOKEN_RBRACKET)) {
          break; // trailing comma
        }
        if (elem_count >= 16) {
          error_at(p, current_tok_span(p), "too many elements in array");
          had_error = true;
          break;
        }
        Expr *e = parse_expr(p);
        if (e->kind == EXPR_POISON) {
          had_error = true;
          break;
        }
        elem_exprs[elem_count++] = e;
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
    expr->as.array.elems = al_alloc(p->al, sizeof(Expr *) * elem_count);
    memcpy(expr->as.array.elems, elem_exprs, sizeof(Expr *) * elem_count);
    expr->as.array.count = elem_count;
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
    expr->as.if_expr.then_block = then_block;
    expr->as.if_expr.else_branch = else_branch;
    return expr;
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
    expr->as.while_expr.condition = cond;
    expr->as.while_expr.body = block;
    return expr;
  }

  // block
  if (check_tok(p, TOKEN_LBRACE)) {
    return parse_block(p);
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

  if (!consume_tok(p, TOKEN_SEMICOLON, "expected ';'")) {
    return ast_stmt(STMT_POISON, current_tok_span(p), p->al);
  };

  Span full = span_merge(token_span(var_tok), previous_tok_span(p));
  Stmt *stmt = ast_stmt(STMT_VAR, full, p->al);

  stmt->as.var_stmt.binding = binding;
  stmt->as.var_stmt.initializer = init;
  stmt->as.var_stmt.type_annotation = ann;

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
    Stmt *stmt = ast_stmt(STMT_BREAK, token_span(previous_tok(p)), p->al);
    if (!consume_tok(p, TOKEN_SEMICOLON, "expected ';' after 'break'")) {
      return ast_stmt(STMT_POISON, span_merge(start_span, current_tok_span(p)),
                      p->al);
    }
    return stmt;
  }

  if (match_tok(p, TOKEN_CONTINUE)) {
    Stmt *stmt = ast_stmt(STMT_CONTINUE, token_span(previous_tok(p)), p->al);
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
      check_tok(p, TOKEN_FOR) || check_tok(p, TOKEN_WHILE)) {
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

static bool parse_param_list(Parser *p, ParamDeclNode *out, int *count,
                             int max) {
  *count = 0;
  bool had_error = false;

  // empty param list
  if (check_tok(p, TOKEN_RPAREN))
    return true;

  do {
    // trailing comma: fun f(a: Int, b: Int,) — stop before ')'
    if (check_tok(p, TOKEN_RPAREN))
      break;

    if (*count >= max) {
      error_at(p, current_tok_span(p), "too many parameters");
      had_error = true;
      break;
    }

    ParamDeclNode param = parse_param(p);
    if (param.type_annotation == NULL && !param.is_self) {
      had_error = true;
      break; // can't trust param list structure anymore
    }
    out[(*count)++] = param;

  } while (match_tok(p, TOKEN_COMMA));

  return !had_error;
}

// `@native("io_print")` / `@intrinsic("array_len")` / `@lang("display")`,
// already past the '@'. Each takes exactly one string: the attribute surface is
// a registry key and nothing more, so there is no attribute *grammar* to grow
// here — a new tier is a new name in this switch. `@native`/`@intrinsic` are
// body attributes (the fun is bodyless); `@lang` is a marker (the definition
// keeps its body). The caller (`parse_decl`) sorts them apart.
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
  } else {
    error_at(p, token_span(&name_tok),
             "unknown attribute; expected '@native', '@intrinsic' or '@lang'");
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

static Decl *parse_fun_decl(Parser *p, bool is_pub, AttrNode attr) {
  if (!consume_tok(p, TOKEN_FUN, "expected 'fun'")) {
    return ast_decl(DECL_POISON, current_tok_span(p), p->al);
  }
  Token fun_tok = *previous_tok(p);
  bool had_error = false;

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

  ParamDeclNode params[16];
  int param_count = 0;

  if (!check_tok(p, TOKEN_RPAREN)) {
    if (!parse_param_list(p, params, &param_count, 16)) {
      had_error = true;
      sync_to_fun_body(p);
    }
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
    if (!parse_where_clause(p, &where_clause)) {
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
  decl->is_pub = is_pub;
  decl->as.fun_decl = (DeclFun){
      .name = name_sv,
      .return_type = return_type,
      .body = body,
      .param_count = param_count,
      .params = al_alloc(p->al, sizeof(ParamDeclNode) * param_count),
      .where_clause = where_clause,
      .shorthand = body != NULL && body->kind != EXPR_BLOCK,
      .attr = attr,
      .type_param_count = type_param_count,
      .type_params = type_params,
  };
  memcpy(decl->as.fun_decl.params, params, sizeof(ParamDeclNode) * param_count);

  return decl;
}

// `allow_pub` gates a per-field `pub`: a struct field carries its own
// visibility, but an enum variant's payload does not (a variant is as visible
// as its enum), so a `pub` there is rejected rather than silently ignored.
static bool parse_field_decl(Parser *p, FieldDeclNode **out, int *out_count,
                             bool *out_is_tuple, bool allow_pub) {
  FieldDeclNode fields[16];
  int field_count = 0;
  bool is_tuple_fields = false;
  bool had_error = false;

  if (match_tok(p, TOKEN_LBRACE)) {
    if (!check_tok(p, TOKEN_RBRACE)) {
      do {
        if (check_tok(p, TOKEN_RBRACE)) {
          break; // trailing comma
        }

        if (field_count >= 16) {
          error_at(p, current_tok_span(p), "too many fields in struct");
          had_error = true;
          break;
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

        fields[field_count].ident.name = name_tok.lexeme;
        fields[field_count].type_annotation = ty;
        fields[field_count].is_pub = field_pub;
        fields[field_count].span = span_merge(token_span(&name_tok), ty->span);
        field_count++;
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
        if (field_count >= 16) {
          error_at(p, current_tok_span(p), "too many fields in struct");
          had_error = true;
          break;
        }

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

        fields[field_count].ident.index = field_count;
        fields[field_count].type_annotation = ty;
        fields[field_count].is_pub = field_pub;
        fields[field_count].span = ty->span;
        field_count++;
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
  *out = al_alloc(p->al, sizeof(FieldDeclNode) * field_count);
  memcpy(*out, fields, sizeof(FieldDeclNode) * field_count);
  *out_is_tuple = is_tuple_fields;
  *out_count = field_count;
  return true;
}

static Decl *parse_struct_decl(Parser *p, bool is_pub) {
  if (!consume_tok(p, TOKEN_STRUCT, "expected 'struct'")) {
    return ast_decl(DECL_POISON, current_tok_span(p), p->al);
  }
  Token struct_tok = *previous_tok(p);
  bool had_error = false;

  StringView name_sv = {0};
  if (!consume_tok(p, TOKEN_IDENT, "expected struct name")) {
    had_error = true;
  } else {
    name_sv = previous_tok(p)->lexeme;
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
  if (!consume_tok(p, TOKEN_IDENT, "expected enum name")) {
    had_error = true;
  } else {
    name_sv = previous_tok(p)->lexeme;
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

  VariantDeclNode variants[16];
  int variant_count = 0;

  if (match_tok(p, TOKEN_LBRACE)) {
    if (!check_tok(p, TOKEN_RBRACE)) {
      do {
        if (variant_count >= 16) {
          error_at(p, current_tok_span(p), "too many variants in enum");
          had_error = true;
          break;
        }

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

        variants[variant_count++] = (VariantDeclNode){
            .name = name_tok.lexeme,
            .fields = fields,
            .field_count = field_count,
            .is_tuple = is_tuple,
            .span = span_merge(token_span(&name_tok), previous_tok_span(p)),
        };
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
  decl->is_pub = is_pub;
  DeclEnum *enum_decl = &decl->as.enum_decl;
  enum_decl->name = name_sv;
  enum_decl->type_param_count = type_param_count;
  enum_decl->type_params = type_params;
  enum_decl->variants =
      al_alloc(p->al, sizeof(VariantDeclNode) * variant_count);
  memcpy(enum_decl->variants, variants,
         sizeof(VariantDeclNode) * variant_count);
  enum_decl->variant_count = variant_count;
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

  if (match_tok(p, TOKEN_LBRACE)) {
    UseAlias aliases[8];
    int alias_count = 0;

    // glob import: use foo::{...}
    if (!check_tok(p, TOKEN_RBRACE)) {
      do {
        if (match_tok(p, TOKEN_COMMA)) {
          break;
        }
        if (alias_count >= 8) {
          error_at(p, current_tok_span(p), "too many items in use glob");
          return ast_decl(DECL_POISON, token_span(&use_tok), p->al);
        }
        if (!consume_tok(p, TOKEN_IDENT, "expected identifier in use glob")) {
          return ast_decl(DECL_POISON, token_span(&use_tok), p->al);
        }
        aliases[alias_count].name = previous_tok(p)->lexeme;
        Span item_span = previous_tok_span(p);
        if (match_tok(p, TOKEN_AS)) {
          if (!consume_tok(p, TOKEN_IDENT, "expected identifier after 'as'")) {
            return ast_decl(DECL_POISON, token_span(&use_tok), p->al);
          }
          aliases[alias_count].alias = previous_tok(p)->lexeme;
          item_span = span_merge(item_span, previous_tok_span(p));
        } else {
          aliases[alias_count].alias = aliases[alias_count].name;
        }
        aliases[alias_count].span = item_span;
        alias_count++;
      } while (match_tok(p, TOKEN_COMMA));
    }

    if (!consume_tok(p, TOKEN_RBRACE, "expected '}' after use glob")) {
      return ast_decl(DECL_POISON, token_span(&use_tok), p->al);
    }

    target.aliases = al_alloc(p->al, sizeof(UseAlias) * alias_count);
    memcpy(target.aliases, aliases, sizeof(UseAlias) * alias_count);
    target.count = alias_count;
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

    target.aliases = al_alloc(p->al, sizeof(UseAlias));
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

static Decl *parse_trait_decl(Parser *p, bool is_pub) {
  if (!consume_tok(p, TOKEN_TRAIT, "expected 'trait'")) {
    return ast_decl(DECL_POISON, current_tok_span(p), p->al);
  }
  Token trait_tok = *previous_tok(p);

  StringView name_sv = {0};
  if (!consume_tok(p, TOKEN_IDENT, "expected trait name")) {
    return ast_decl(DECL_POISON, token_span(&trait_tok), p->al);
  } else {
    name_sv = previous_tok(p)->lexeme;
  }

  TypeParamNode *type_params = NULL;
  int type_param_count = 0;
  if (check_tok(p, TOKEN_LT)) {
    type_param_count = parse_type_params(p, &type_params);
    if (type_param_count < 0) {
      return ast_decl(DECL_POISON, token_span(&trait_tok), p->al);
    }
  }

  TraitItemNode items[64];
  int item_count = 0;
  bool had_error = false;

  if (!consume_tok(p, TOKEN_LBRACE, "expected '{' after trait name")) {
    had_error = true;
  }

  if (!check_tok(p, TOKEN_RBRACE)) {
    do {
      if (item_count >= 64) {
        error_at(p, current_tok_span(p), "too many items in trait");
        had_error = true;
        break;
      }

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

        items[item_count].kind = TRAIT_ITEM_ASSOC_TYPE;
        items[item_count].name = assoc_type_name;
        items[item_count].span = span_merge(start_span, previous_tok_span(p));
        item_count++;
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

        ParamDeclNode params[64];
        int param_count = 0;

        if (!check_tok(p, TOKEN_RPAREN)) {
          if (!parse_param_list(p, params, &param_count, 64)) {
            had_error = true;
            sync_to_fun_body(p);
          }
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
          items[item_count].kind = TRAIT_ITEM_METHOD;
          items[item_count].name = name_sv;
          items[item_count].span =
              span_merge(token_span(&fun_tok), previous_tok_span(p));
          items[item_count].type_param_count = type_param_count;
          items[item_count].type_params = type_params;
          items[item_count].param_count = param_count;
          items[item_count].params =
              al_alloc(p->al, sizeof(ParamDeclNode) * param_count);
          memcpy(items[item_count].params, params,
                 sizeof(ParamDeclNode) * param_count);
          items[item_count].return_type = return_type;
          items[item_count].default_body = body;
          item_count++;
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
  decl->is_pub = is_pub;
  DeclTrait *trait_decl = &decl->as.trait_decl;
  trait_decl->name = name_sv;
  trait_decl->type_params = type_params;
  trait_decl->type_param_count = type_param_count;
  trait_decl->items = al_alloc(p->al, sizeof(TraitItemNode) * item_count);
  memcpy(trait_decl->items, items, sizeof(TraitItemNode) * item_count);
  trait_decl->item_count = item_count;
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

  ImplItemNode items[16];
  int item_count = 0;

  WhereClause *where_clause = NULL;
  if (check_tok(p, TOKEN_WHERE)) {
    if (!parse_where_clause(p, &where_clause)) {
      had_error = true;
    }
  }

  if (!consume_tok(p, TOKEN_LBRACE, "expected '{' after impl header")) {
    had_error = true;
  }

  if (!check_tok(p, TOKEN_RBRACE)) {
    do {
      if (item_count >= 16) {
        error_at(p, current_tok_span(p), "too many items in impl");
        had_error = true;
        break;
      }

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

        items[item_count].kind = IMPL_ITEM_ASSOC_TYPE;
        items[item_count].name = assoc_type_name;
        items[item_count].assoc_type = ty;
        items[item_count].span = span_merge(start_span, ty->span);
        item_count++;
      } else if (check_tok(p, TOKEN_AT) || check_tok(p, TOKEN_FUN)) {
        // a method may carry `@native`/`@intrinsic` just like a top-level fun:
        // the attribute *is* its body, so a primitive's operation can be
        // spelled `s.len()` rather than as a free `string::len(s)`. `self` is
        // an ordinary parameter to the native, so nothing downstream of the
        // signature has to change.
        AttrNode attr = {.kind = ATTR_NONE};
        if (match_tok(p, TOKEN_AT)) {
          attr = parse_attr(p);
          if (attr.kind == ATTR_NONE) {
            had_error = true;
            break;
          }
          if (attr.kind == ATTR_LANG) {
            // a method is never a lang item — those are top-level.
            error_at(p, attr.span,
                     "'@lang' can only mark a trait, enum, or function");
            attr.kind = ATTR_NONE;
          }
        }
        Decl *fun_decl = parse_fun_decl(p, false, attr);
        if (fun_decl->kind == DECL_POISON) {
          had_error = true;
        } else {
          items[item_count].kind = IMPL_ITEM_METHOD;
          items[item_count].name = fun_decl->as.fun_decl.name;
          items[item_count].fun_decl = fun_decl;
          items[item_count].span = fun_decl->span;
          item_count++;
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
  impl_decl->items = al_alloc(p->al, sizeof(ImplItemNode) * item_count);
  memcpy(impl_decl->items, items, sizeof(ImplItemNode) * item_count);
  impl_decl->item_count = item_count;
  return decl;
}

static Decl *parse_decl(Parser *p) {
  Decl *decl = NULL;

  // attributes precede `pub`. There are two kinds and a decl may carry one of
  // each: a *body* attribute (`@native`/`@intrinsic`, fun-only, bodyless) and a
  // *marker* (`@lang`, on a bodied trait/enum/fun). `float` is both. Sort them
  // into two slots here so the decl parsers below only see the body attribute.
  AttrNode attr = {.kind = ATTR_NONE};      // body attribute
  AttrNode lang_attr = {.kind = ATTR_NONE}; // marker
  while (match_tok(p, TOKEN_AT)) {
    AttrNode a = parse_attr(p);
    if (a.kind == ATTR_NONE) {
      sync_to_decl(p);
      return ast_decl(DECL_POISON, previous_tok_span(p), p->al);
    }
    if (a.kind == ATTR_LANG) {
      if (lang_attr.kind != ATTR_NONE) {
        error_at(p, a.span, "duplicate '@lang' attribute");
      }
      lang_attr = a;
    } else {
      if (attr.kind != ATTR_NONE) {
        error_at(p, a.span,
                 "a definition has at most one '@native'/'@intrinsic'");
      }
      attr = a;
    }
  }

  bool is_pub = match_tok(p, TOKEN_PUB);

  if (check_tok(p, TOKEN_USE)) {
    // `pub use` re-exports: the alias becomes an item of *this* module, so an
    // importer may name it without knowing where it originally came from.
    decl = parse_use_decl(p);
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
  Decl *decls[1024];
  int count = 0;

  while (!is_at_end(p)) {
    Decl *decl = parse_decl(p);
    assert(decl && "declaration should not be NULL");
    assert(count < 1024 && "too many declarations");
    decls[count++] = decl;
  }

  Program *prog = al_alloc_for(p->al, Program);
  prog->decls = al_alloc(p->al, sizeof(Decl *) * count);
  prog->decl_count = count;
  memcpy(prog->decls, decls, sizeof(Decl *) * count);

  return prog;
}