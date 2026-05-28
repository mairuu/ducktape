#include "parser.h"

#include "allocator.h"
#include "ast.h"
#include "error_reporter.h"
#include "scanner.h"
#include "string_utils.h"

#include <assert.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>

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
  reporter_error(p->reporter, span.line, span.line_end, span.col, span.col_end,
                 msg, "");
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

static BindingPat parse_binding(Parser *p) {
  if (match_tok(p, TOKEN_IDENT)) {
    Token name_tok = *previous_tok(p);

    if (match_tok(p, TOKEN_LBRACE)) {
      StringView field_names[16];
      int field_count = 0;

      if (!check_tok(p, TOKEN_RBRACE)) {
        do {
          if (field_count >= 16) {
            error_at(p, current_tok_span(p),
                     "too many fields in struct pattern");
            return (BindingPat){.kind = BIND_POISON,
                                .span = current_tok_span(p)};
          }
          if (check_tok(p, TOKEN_RBRACE)) {
            break;
          }
          if (!consume_tok(p, TOKEN_IDENT,
                           "expected field name in struct pattern")) {
            return (BindingPat){.kind = BIND_POISON,
                                .span = current_tok_span(p)};
          }
          field_names[field_count++] = previous_tok(p)->lexeme;
        } while (match_tok(p, TOKEN_COMMA));

        if (!consume_tok(p, TOKEN_RBRACE,
                         "expected '}' after struct pattern fields")) {
          return (BindingPat){.kind = BIND_POISON, .span = current_tok_span(p)};
        }
      }

      BindingPat b = (BindingPat){
          .kind = BIND_STRUCT,
          .span = span_merge(token_span(&name_tok), previous_tok_span(p)),
          .as.struc.struct_name = name_tok.lexeme,
          .as.struc.field_names =
              al_alloc(p->al, sizeof(StringView) * field_count),
          .as.struc.field_count = field_count,
      };
      memcpy(b.as.struc.field_names, field_names,
             sizeof(StringView) * field_count);
      return b;
    }

    return (BindingPat){
        .kind = BIND_IDENT,
        .span = token_span(&name_tok),
        .as.ident = name_tok.lexeme,
    };
  }

  if (match_tok(p, TOKEN_LPAREN)) {
    // tuple pattern
    StringView names[16];
    int count = 0;

    Token *start_tok = previous_tok(p);

    if (!check_tok(p, TOKEN_RPAREN)) {
      do {
        if (count >= 16) {
          error_at(p, current_tok_span(p),
                   "too many elements in tuple pattern");
          return (BindingPat){.kind = BIND_POISON, .span = current_tok_span(p)};
        }
        if (check_tok(p, TOKEN_RPAREN)) {
          break;
        }
        if (!consume_tok(p, TOKEN_IDENT,
                         "expected identifier in tuple pattern")) {
          return (BindingPat){.kind = BIND_POISON, .span = current_tok_span(p)};
        }
        names[count++] = previous_tok(p)->lexeme;
      } while (match_tok(p, TOKEN_COMMA));
    }

    if (!consume_tok(p, TOKEN_RPAREN, "expected ')' after tuple pattern")) {
      return (BindingPat){.kind = BIND_POISON, .span = current_tok_span(p)};
    }

    BindingPat b = (BindingPat){
        .kind = BIND_TUPLE,
        .span = span_merge(token_span(start_tok), previous_tok_span(p)),
        .as.tuple.names = al_alloc(p->al, sizeof(StringView) * count),
        .as.tuple.count = count,
    };
    memcpy(b.as.tuple.names, names, sizeof(StringView) * count);
    return b;
  }

  error_at(p, current_tok_span(p), "expected identifier in binding pattern");
  return (BindingPat){.kind = BIND_POISON, .span = current_tok_span(p)};
}

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

    if (!match_tok(p, TOKEN_COLON)) {
      error_at(p, current_tok_span(p),
               "expected ':' after function type parameters");
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

  if (match_tok(p, TOKEN_SELF_TYPE)) {
    base = ast_type_node(TYNODE_SELF, token_span(previous_tok(p)), p->al);
  }

  if (match_tok(p, TOKEN_IDENT)) {
    Token start = *previous_tok(p);

    StringView segments[16];
    int segment_count = 1;
    segments[0] = previous_tok(p)->lexeme;

    while (match_tok(p, TOKEN_COLONCOLON)) {
      if (!match_tok(p, TOKEN_IDENT)) {
        error_at(p, current_tok_span(p), "expected identifier after '::'");
        return ast_type_node(
            TYNODE_POISON, span_merge(token_span(&start), current_tok_span(p)),
            p->al);
      }
      if (segment_count >= 16) {
        Span error_span = span_merge(token_span(&start), current_tok_span(p));
        error_at(p, error_span, "too many segments in type path");
        return ast_type_node(TYNODE_POISON, error_span, p->al);
      }
      segments[segment_count++] = previous_tok(p)->lexeme;
    }

    TypeNode *ty_params[16];
    int ty_param_count = 0;

    if (match_tok(p, TOKEN_LT)) {
      if (!check_tok(p, TOKEN_GT)) {
        do {
          if (ty_param_count >= 16) {
            error_at(p, current_tok_span(p),
                     "too many type arguments in type application");
            return ast_type_node(
                TYNODE_POISON,
                span_merge(token_span(&start), current_tok_span(p)), p->al);
          }
          TypeNode *ty = parse_type(p);
          if (ty->kind == TYNODE_POISON) {
            return ast_type_node(
                TYNODE_POISON,
                span_merge(token_span(&start), current_tok_span(p)), p->al);
          }
          ty_params[ty_param_count++] = ty;
        } while (match_tok(p, TOKEN_COMMA));
      }

      if (!consume_tok(p, TOKEN_GT, "expected '>' after type arguments")) {
        return ast_type_node(
            TYNODE_POISON, span_merge(token_span(&start), current_tok_span(p)),
            p->al);
      }
    }

    base = ast_type_node(TYNODE_NAMED,
                         span_merge(token_span(&start), previous_tok_span(p)),
                         p->al);
    base->as.named.path.segments =
        al_alloc(p->al, sizeof(StringView) * segment_count);
    memcpy(base->as.named.path.segments, segments,
           sizeof(StringView) * segment_count);
    base->as.named.path.count = segment_count;
    base->as.named.path.span =
        span_merge(token_span(&start), previous_tok_span(p));
    base->as.named.type_arg_count = ty_param_count;
    if (ty_param_count > 0) {
      base->as.named.type_args =
          al_alloc(p->al, sizeof(TypeNode *) * ty_param_count);
      memcpy(base->as.named.type_args, ty_params,
             sizeof(TypeNode *) * ty_param_count);
    } else {
      base->as.named.type_args = NULL;
    }
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
          base = expr;
        } else {
          // field access: obj.field
          Expr *expr = ast_expr(
              EXPR_FIELD, span_merge(base->span, previous_tok_span(p)), p->al);
          expr->as.field.object = base;
          expr->as.field.field_name = name.lexeme;
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
        expr->as.field.is_tuple_field = true;
        expr->as.field.tuple_index = idx;
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
         check_tok(p, TOKEN_BREAK) || check_tok(p, TOKEN_CONTINUE);
}

static Expr *parse_block(Parser *p) {
  if (!consume_tok(p, TOKEN_LBRACE, "expected '{'")) {
    return ast_expr(EXPR_POISON, current_tok_span(p), p->al);
  };
  Span start_span = previous_tok_span(p);

  Stmt *tmp_stmts[256];
  int stmt_count = 0;
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
      } else {
        assert(stmt_count < 256 && "too many statements in block");
        tmp_stmts[stmt_count++] = stmt;
      }
    } else {
      Expr *expr = parse_expr(p);
      if (expr->kind == EXPR_POISON) {
        advance_tok(p);
        block_had_error = true;
      } else if (match_tok(p, TOKEN_SEMICOLON)) {
        Stmt *stmt = ast_stmt(STMT_EXPR, expr->span, p->al);
        stmt->as.expr_stmt.expr = expr;
        assert(stmt_count < 256 && "too many statements in block");
        tmp_stmts[stmt_count++] = stmt;
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
  expr->as.block.stmts = al_alloc(p->al, sizeof(Stmt *) * stmt_count);
  memcpy(expr->as.block.stmts, tmp_stmts, sizeof(Stmt *) * stmt_count);
  expr->as.block.stmt_count = stmt_count;
  expr->as.block.tail_expr = tail;
  return expr;
}

static int parse_type_params(Parser *p, TypeParamNode **out) {
  if (!consume_tok(p, TOKEN_LT, "expected '<'"))
    return -1;

  TypeParamNode tmp[32];
  int count = 0;
  bool had_error = true;

  do {
    if (count >= 32) {
      error_at(p, current_tok_span(p), "too many type parameters");
      had_error = false;
      break;
    }

    if (!consume_tok(p, TOKEN_IDENT, "expected type parameter name")) {
      had_error = false;
      break;
    }
    Token name_tok = *previous_tok(p);

    StringView bounds[8];
    int bound_count = 0;

    if (match_tok(p, TOKEN_COLON)) {
      do {
        if (!consume_tok(p, TOKEN_IDENT, "expected trait bound")) {
          had_error = false;
          break;
        }
        if (bound_count < 32) {
          bounds[bound_count++] = previous_tok(p)->lexeme;
        }
      } while (had_error && match_tok(p, TOKEN_PLUS));
    }

    if (!had_error)
      break;

    TypeParamNode *param = &tmp[count++];
    param->name = name_tok.lexeme;
    param->span = token_span(&name_tok);
    param->bound_count = bound_count;
    param->bounds = al_alloc(p->al, sizeof(StringView) * bound_count);
    memcpy(param->bounds, bounds, sizeof(StringView) * bound_count);

  } while (had_error && match_tok(p, TOKEN_COMMA));

  if (!had_error) {
    while (!is_at_end(p) && !check_tok(p, TOKEN_GT)) {
      advance_tok(p);
    }
  }

  consume_tok(p, TOKEN_GT, "expected '>'");
  if (!had_error) {
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
    case TOKEN_ARROW:
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

  if (check_tok(p, TOKEN_RPAREN))
    return true;

  do {
    if (check_tok(p, TOKEN_RPAREN))
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

static Expr *parse_closure(Parser *p) {
  Token fun_tok = *previous_tok(p);
  bool had_error = false;

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

  ClosureParam params[64];
  int param_count = 0;

  if (!check_tok(p, TOKEN_RPAREN)) {
    if (!parse_closure_param_list(p, params, &param_count, 64)) {
      had_error = true;
      sync_to_fun_body(p);
    }
  }

  if (!consume_tok(p, TOKEN_RPAREN, "expected ')'")) {
    had_error = true;
    sync_to_fun_body(p);
  }

  TypeNode *return_type = NULL;
  if (match_tok(p, TOKEN_COLON)) {
    return_type = parse_type(p);
    assert(return_type && "return type should not be NULL");
    if (return_type->kind == TYNODE_POISON) {
      had_error = true;
      sync_to_fun_body(p);
    }
  }

  Expr *body = NULL;
  if (match_tok(p, TOKEN_ARROW)) {
    body = parse_expr(p);
    assert(body && "function body should not be NULL");
    if (body->kind == EXPR_POISON) {
      had_error = true;
    }
  } else {
    bool was_panic = p->panic_mode;
    p->panic_mode = false;
    body = parse_block(p);
    p->panic_mode = was_panic;
    assert(body && "function body should not be NULL");
    if (body->kind == EXPR_POISON) {
      had_error = true;
    }
  }

  if (had_error) {
    Span error_span = span_merge(token_span(&fun_tok),
                                 body ? body->span : token_span(&fun_tok));
    return ast_expr(EXPR_POISON, error_span, p->al);
  }

  Expr *expr = ast_expr(EXPR_CLOSURE, token_span(&fun_tok), p->al);
  expr->as.closure.type_params = type_params;
  expr->as.closure.type_param_count = type_param_count;
  expr->as.closure.params = al_alloc(p->al, sizeof(ClosureParam) * param_count);
  memcpy(expr->as.closure.params, params, sizeof(ClosureParam) * param_count);
  expr->as.closure.param_count = param_count;
  expr->as.closure.return_type_annotation = return_type;
  expr->as.closure.body = body;
  return expr;
}

static Pattern *parse_pattern(Parser *p) {
  Pattern pattern = {0};
  Span start_span = current_tok_span(p);

  if (match_tok(p, TOKEN_UNDER)) {
    pattern.kind = PAT_WILDCARD;
    pattern.span = previous_tok_span(p);
  } else if (check_tok(p, TOKEN_INT) || check_tok(p, TOKEN_FLOAT) ||
             check_tok(p, TOKEN_STRING) || check_tok(p, TOKEN_TRUE) ||
             check_tok(p, TOKEN_FALSE)) {
    pattern.kind = PAT_LITERAL;
    pattern.as.literal_expr = parse_primary(p);
    pattern.span = previous_tok_span(p);
  } else if (match_tok(p, TOKEN_IDENT)) {
    StringView segments[16];
    int segment_count = 1;
    segments[0] = previous_tok(p)->lexeme;
    Span start_path_span = token_span(previous_tok(p));

    if (match_tok(p, TOKEN_COLONCOLON)) {
      do {
        if (segment_count >= 16) {
          error_at(p, current_tok_span(p), "too many segments in path");
          return NULL;
        }
        if (!match_tok(p, TOKEN_IDENT)) {
          error_at(p, current_tok_span(p), "expected identifier after '::'");
          return NULL;
        }
        segments[segment_count++] = previous_tok(p)->lexeme;
      } while (match_tok(p, TOKEN_COLONCOLON));
    }

    Span path_span = span_merge(start_path_span, previous_tok_span(p));

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
        pattern.as.variant.path.segments =
            al_alloc(p->al, sizeof(StringView) * segment_count);
        memcpy(pattern.as.variant.path.segments, segments,
               sizeof(StringView) * segment_count);
        pattern.as.variant.path.count = segment_count;
        pattern.as.variant.path.span = path_span;
        pattern.as.variant.payloads =
            al_alloc(p->al, sizeof(Pattern *) * subpat_count);
        memcpy(pattern.as.variant.payloads, subpats,
               sizeof(Pattern *) * subpat_count);
        pattern.as.variant.payload_count = subpat_count;
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
          fields[field_count].field_name = field_name;
          fields[field_count].sub_pattern = field_pat;
          fields[field_count].span = field_span;
          field_count++;
        } else {
          fields[field_count].field_name = field_name;
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
        pattern.as.struc.path.segments =
            al_alloc(p->al, sizeof(StringView) * segment_count);
        memcpy(pattern.as.struc.path.segments, segments,
               sizeof(StringView) * segment_count);
        pattern.as.struc.path.count = segment_count;
        pattern.as.struc.path.span = path_span;
      } else {
        return NULL;
      }
    } else if (segment_count == 1 && pattern.kind == 0) {
      // variable pattern: just an identifier
      pattern.kind = PAT_BIND;
      pattern.span = path_span;
      pattern.as.bind.name = segments[0];
    } else {
      // path without tuple or struct syntax is not a valid pattern
      error_at(p, path_span, "unexpected path in pattern");
      return NULL;
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

  MatchArm tmp_arms[64];
  int arm_count = 0;

  do {
    Span arm_start_span = current_tok_span(p);
    if (check_tok(p, TOKEN_RBRACE)) {
      break;
    }

    if (arm_count >= 64) {
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

    if (!consume_tok(p, TOKEN_ARROW, "expected '=>' after match arm pattern")) {
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

  if (match_tok(p, TOKEN_INTERPOLATION)) {
    InterpolSeg segs[16];
    int seg_count = 2;

    segs[0].kind = ISEG_TEXT;
    segs[0].text =
        (StringView){.len = t->lexeme.len - 2, .chars = t->lexeme.chars + 1};

    segs[1].kind = ISEG_EXPR;
    segs[1].expr = parse_expr(p);

    if (segs[1].expr->kind == EXPR_POISON) {
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

      Expr *expr = parse_expr(p);
      if (expr->kind == EXPR_POISON) {
        return ast_expr(EXPR_POISON, current_tok_span(p), p->al);
      }

      segs[seg_count].kind = ISEG_EXPR;
      segs[seg_count].expr = expr;
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
  if (match_tok(p, TOKEN_IDENT)) {
    StringView segments[16];
    int segment_count = 1;
    segments[0] = previous_tok(p)->lexeme;

    if (match_tok(p, TOKEN_COLONCOLON)) {
      do {
        if (segment_count >= 16) {
          error_at(p, current_tok_span(p), "too many segments in path");
          return ast_expr(EXPR_POISON, current_tok_span(p), p->al);
        }
        if (!match_tok(p, TOKEN_IDENT)) {
          error_at(p, current_tok_span(p), "expected identifier after '::'");
          return ast_expr(EXPR_POISON, current_tok_span(p), p->al);
        }
        segments[segment_count++] = previous_tok(p)->lexeme;
      } while (match_tok(p, TOKEN_COLONCOLON));
    }

    // typeArgs
    TypeNode *ty_args[16];
    int ty_arg_count = 0;
    if (p->allow_struct_init && match_tok(p, TOKEN_LT)) {
      bool had_error = false;

      if (!check_tok(p, TOKEN_GT)) {
        do {
          if (ty_arg_count >= 16) {
            error_at(p, current_tok_span(p),
                     "too many type arguments in type application");
            had_error = true;
            break;
          }
          TypeNode *ty = parse_type(p);
          if (ty->kind == TYNODE_POISON) {
            had_error = true;
            break;
          }
          ty_args[ty_arg_count++] = ty;
        } while (match_tok(p, TOKEN_COMMA));
      }

      consume_tok(p, TOKEN_GT, "expected '>' after type arguments");

      if (ty_arg_count == 0) {
        error_at(p, current_tok_span(p),
                 "expected at least one type argument in type application");
        had_error = true;
      }

      if (had_error) {
        return ast_expr(EXPR_POISON, current_tok_span(p), p->al);
      }
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

          fields[field_count].name = field_name;
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
      expr->as.struct_init.path.segments =
          al_alloc(p->al, sizeof(StringView) * segment_count);
      memcpy(expr->as.struct_init.path.segments, segments,
             sizeof(StringView) * segment_count);
      expr->as.struct_init.path.count = segment_count;
      expr->as.struct_init.path.span =
          span_merge(token_span(t), previous_tok_span(p));
      expr->as.struct_init.field_count = field_count;
      expr->as.struct_init.fields =
          al_alloc(p->al, sizeof(expr->as.struct_init.fields[0]) * field_count);
      memcpy(expr->as.struct_init.fields, fields,
             sizeof(expr->as.struct_init.fields[0]) * field_count);
      expr->as.struct_init.type_arg_count = ty_arg_count;
      if (ty_arg_count > 0) {
        expr->as.struct_init.type_args =
            al_alloc(p->al, sizeof(TypeNode *) * ty_arg_count);
        memcpy(expr->as.struct_init.type_args, ty_args,
               sizeof(TypeNode *) * ty_arg_count);
      } else {
        expr->as.struct_init.type_args = NULL;
      }
      return expr;
    }

    if (segment_count == 1 && ty_arg_count == 0) {
      Expr *expr = ast_expr(EXPR_VAR, token_span(t), p->al);
      expr->as.var.name = segments[0];
      return expr;
    }

    Expr *expr = ast_expr(
        EXPR_PATH, span_merge(token_span(t), previous_tok_span(p)), p->al);
    expr->as.path_expr.path.segments =
        al_alloc(p->al, sizeof(StringView) * segment_count);
    memcpy(expr->as.path_expr.path.segments, segments,
           sizeof(StringView) * segment_count);
    expr->as.path_expr.path.count = segment_count;
    expr->as.path_expr.path.span =
        span_merge(token_span(t), previous_tok_span(p));
    expr->as.path_expr.type_arg_count = ty_arg_count;
    if (ty_arg_count > 0) {
      expr->as.path_expr.type_args =
          al_alloc(p->al, sizeof(TypeNode *) * ty_arg_count);
      memcpy(expr->as.path_expr.type_args, ty_args,
             sizeof(TypeNode *) * ty_arg_count);
    } else {
      expr->as.path_expr.type_args = NULL;
    }
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
  if (match_tok(p, TOKEN_FUN)) {
    return parse_closure(p);
  }

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

    p->allow_struct_init = false;
    Expr *then_block = parse_block(p);
    p->allow_struct_init = old_allow_struct_init;

    if (then_block->kind == EXPR_POISON) {
      return ast_expr(EXPR_POISON, current_tok_span(p), p->al);
    }

    p->allow_struct_init = false;
    Expr *else_branch = NULL;
    p->allow_struct_init = old_allow_struct_init;

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
    if (peek_ahead(p, 1)->type == TOKEN_IN) {
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
          EXPR_FOR,
          span_merge(token_span(previous_tok(p)), previous_tok_span(p)), p->al);
      expr->as.for_expr.var_name = var_tok->lexeme;
      expr->as.for_expr.iterable = iterable;
      expr->as.for_expr.body = block;
      expr->as.for_expr.is_while = false;
      return expr;
    }

    Expr *cond = NULL;
    if (!check_tok(p, TOKEN_LBRACE)) {
      bool old_allow_struct_init = p->allow_struct_init;
      p->allow_struct_init = false;
      cond = parse_expr(p);
      p->allow_struct_init = old_allow_struct_init;
      if (cond->kind == EXPR_POISON) {
        return ast_expr(EXPR_POISON, current_tok_span(p), p->al);
      }
    } else {
      cond = ast_expr(EXPR_BOOL, token_span(peek_tok(p)), p->al);
      cond->as.bool_val = true;
    }

    Expr *block = parse_block(p);
    if (block->kind == EXPR_POISON) {
      return ast_expr(EXPR_POISON, current_tok_span(p), p->al);
    }

    Expr *expr = ast_expr(
        EXPR_FOR, span_merge(token_span(previous_tok(p)), previous_tok_span(p)),
        p->al);
    expr->as.for_expr.condition = cond;
    expr->as.for_expr.body = block;
    expr->as.for_expr.is_while = true;
    return expr;
  }

  // block
  if (check_tok(p, TOKEN_LBRACE)) {
    return parse_block(p);
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

  BindingPat binding = parse_binding(p);
  if (binding.kind == BIND_POISON) {
    return ast_decl(DECL_POISON, binding.span, p->al);
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

  BindingPat binding = parse_binding(p);
  if (binding.kind == BIND_POISON) {
    return ast_stmt(STMT_POISON, binding.span, p->al);
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

  // desugar to unit expression statement
  // if (match_tok(p, TOKEN_SEMICOLON)) {
  //   Stmt *stmt= ast_stmt(STMT_EXPR, token_span(previous_tok(p)), p->al);
  //   stmt->as.expr_stmt.expr = ast_expr(EXPR_UNIT,
  //   token_span(previous_tok(p)), p->al); return stmt;
  // }

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

static Decl *parse_fun_decl(Parser *p, bool is_pub) {
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
  if (match_tok(p, TOKEN_COLON)) {
    return_type = parse_type(p);
    assert(return_type && "return type should not be NULL");
    if (return_type->kind == TYNODE_POISON) {
      had_error = true;
    }
  }

  Expr *body = NULL;
  if (match_tok(p, TOKEN_ARROW)) {
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
  decl->as.fun_decl.name = name_sv;
  decl->as.fun_decl.return_type = return_type;
  decl->as.fun_decl.body = body;
  decl->as.fun_decl.param_count = param_count;
  decl->as.fun_decl.params =
      al_alloc(p->al, sizeof(ParamDeclNode) * param_count);
  memcpy(decl->as.fun_decl.params, params, sizeof(ParamDeclNode) * param_count);
  decl->as.fun_decl.shorthand = body->kind != EXPR_BLOCK;
  decl->as.fun_decl.type_param_count = type_param_count;
  decl->as.fun_decl.type_params = type_params;

  return decl;
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

  FieldDeclNode fields[64];
  int field_count = 0;
  bool is_tuple_struct = false;

  if (match_tok(p, TOKEN_LBRACE)) {
    if (!check_tok(p, TOKEN_RBRACE)) {
      do {
        if (check_tok(p, TOKEN_RBRACE)) {
          break; // trailing comma
        }

        if (field_count >= 64) {
          error_at(p, current_tok_span(p), "too many fields in struct");
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

        fields[field_count].name = name_tok.lexeme;
        fields[field_count].type_annotation = ty;
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
    is_tuple_struct = true;

    if (!check_tok(p, TOKEN_RPAREN)) {
      do {
        if (field_count >= 64) {
          error_at(p, current_tok_span(p), "too many fields in struct");
          had_error = true;
          break;
        }

        TypeNode *ty = parse_type(p);
        if (ty->kind == TYNODE_POISON) {
          had_error = true;
          break;
        }

        fields[field_count].type_annotation = ty;
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
  } else {
    error_at(p, current_tok_span(p), "expected '{' or '(' after struct name");
    had_error = true;
  }

  if (had_error) {
    return ast_decl(DECL_POISON,
                    span_merge(token_span(&struct_tok), previous_tok_span(p)),
                    p->al);
  }

  Decl *decl = ast_decl(DECL_STRUCT, token_span(&struct_tok), p->al);
  decl->is_pub = is_pub;
  decl->as.struct_decl.name = name_sv;
  decl->as.struct_decl.type_param_count = type_param_count;
  decl->as.struct_decl.type_params = type_params;
  decl->as.struct_decl.is_tuple_struct = is_tuple_struct;

  if (is_tuple_struct) {
    decl->as.struct_decl.fields = NULL;
    decl->as.struct_decl.field_count = 0;
    decl->as.struct_decl.tuple_types =
        al_alloc(p->al, sizeof(TypeNode *) * field_count);
    for (int i = 0; i < field_count; i++) {
      decl->as.struct_decl.tuple_types[i] = fields[i].type_annotation;
    }
    decl->as.struct_decl.tuple_type_count = field_count;
  } else {
    decl->as.struct_decl.fields =
        al_alloc(p->al, sizeof(FieldDeclNode) * field_count);
    memcpy(decl->as.struct_decl.fields, fields,
           sizeof(FieldDeclNode) * field_count);
    decl->as.struct_decl.field_count = field_count;
    decl->as.struct_decl.tuple_types = NULL;
    decl->as.struct_decl.tuple_type_count = 0;
  }

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

  VariantDeclNode variants[64];
  int variant_count = 0;

  if (match_tok(p, TOKEN_LBRACE)) {
    if (!check_tok(p, TOKEN_RBRACE)) {
      do {
        if (variant_count >= 64) {
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

        TypeNode *payload[16];
        int payload_count = 0;
        if (match_tok(p, TOKEN_LPAREN)) {
          if (!check_tok(p, TOKEN_RPAREN)) {
            do {
              if (payload_count >= 16) {
                error_at(p, current_tok_span(p), "too many fields in variant");
                had_error = true;
                break;
              }
              TypeNode *ty = parse_type(p);
              if (ty->kind == TYNODE_POISON) {
                had_error = true;
                break;
              }
              payload[payload_count++] = ty;
            } while (match_tok(p, TOKEN_COMMA));
          }

          if (had_error) {
            // sync to ')'
            while (!is_at_end(p) && !check_tok(p, TOKEN_RPAREN)) {
              advance_tok(p);
            }
          }

          if (!consume_tok(p, TOKEN_RPAREN,
                           "expected ')' after variant payload")) {
            had_error = true;
          }
        }

        variants[variant_count].name = name_tok.lexeme;
        variants[variant_count].payload_types =
            al_alloc(p->al, sizeof(TypeNode *) * payload_count);
        memcpy(variants[variant_count].payload_types, payload,
               sizeof(TypeNode *) * payload_count);
        variants[variant_count].payload_count = payload_count;
        variants[variant_count].span = token_span(&name_tok);
        variant_count++;
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
  decl->as.enum_decl.name = name_sv;
  decl->as.enum_decl.type_param_count = type_param_count;
  decl->as.enum_decl.type_params = type_params;
  decl->as.enum_decl.variants =
      al_alloc(p->al, sizeof(VariantDeclNode) * variant_count);
  memcpy(decl->as.enum_decl.variants, variants,
         sizeof(VariantDeclNode) * variant_count);
  decl->as.enum_decl.variant_count = variant_count;
  return decl;
}

static Decl *parse_use_decl(Parser *p) {
  if (!consume_tok(p, TOKEN_USE, "expected 'use'")) {
    return ast_decl(DECL_POISON, current_tok_span(p), p->al);
  }
  Token use_tok = *previous_tok(p);

  StringView segments[16];
  int segment_count = 0;

  UseTarget target = {0};

  do {
    if (segment_count >= 16) {
      error_at(p, current_tok_span(p), "too many segments in use path");
      return ast_decl(DECL_POISON, token_span(&use_tok), p->al);
    }
    if (check_tok(p, TOKEN_LBRACE)) {
      break;
    }
    if (!consume_tok(p, TOKEN_IDENT, "expected identifier in use path")) {
      return ast_decl(DECL_POISON, token_span(&use_tok), p->al);
    }
    segments[segment_count++] = previous_tok(p)->lexeme;
  } while (match_tok(p, TOKEN_COLONCOLON));

  if (match_tok(p, TOKEN_LBRACE)) {
    UseAlias aliases[16];
    int alias_count = 0;

    // glob import: use foo::{...}
    if (!check_tok(p, TOKEN_RBRACE)) {
      do {
        if (match_tok(p, TOKEN_COMMA)) {
          break;
        }
        if (!consume_tok(p, TOKEN_IDENT, "expected identifier in use glob")) {
          return ast_decl(DECL_POISON, token_span(&use_tok), p->al);
        }
        aliases[alias_count].name = previous_tok(p)->lexeme;
        if (match_tok(p, TOKEN_AS)) {
          if (!consume_tok(p, TOKEN_IDENT, "expected identifier after 'as'")) {
            return ast_decl(DECL_POISON, token_span(&use_tok), p->al);
          }
          aliases[alias_count].alias = previous_tok(p)->lexeme;
        } else {
          aliases[alias_count].alias = aliases[alias_count].name;
        }
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
    StringView alias_name = segments[segment_count - 1];
    if (match_tok(p, TOKEN_AS)) {
      if (!consume_tok(p, TOKEN_IDENT, "expected identifier after 'as'")) {
        return ast_decl(DECL_POISON, token_span(&use_tok), p->al);
      }
      alias_name = previous_tok(p)->lexeme;
    }

    target.aliases = al_alloc(p->al, sizeof(UseAlias));
    target.aliases[0].name = segments[segment_count - 1];
    target.aliases[0].alias = alias_name;
    target.count = 1;
    segment_count--;
  }

  if (!consume_tok(p, TOKEN_SEMICOLON, "expected ';' after use declaration")) {
    return ast_decl(DECL_POISON,
                    span_merge(token_span(&use_tok), previous_tok_span(p)),
                    p->al);
  }

  Decl *decl = ast_decl(DECL_USE, token_span(&use_tok), p->al);
  decl->as.use_decl.path.segments =
      al_alloc(p->al, sizeof(StringView) * segment_count);
  memcpy(decl->as.use_decl.path.segments, segments,
         sizeof(StringView) * segment_count);
  decl->as.use_decl.path.count = segment_count;
  decl->as.use_decl.target = target;
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
        if (match_tok(p, TOKEN_COLON)) {
          return_type = parse_type(p);
          assert(return_type && "return type should not be NULL");
          if (return_type->kind == TYNODE_POISON) {
            had_error = true;
          }
        }

        Expr *body = NULL;
        if (match_tok(p, TOKEN_SEMICOLON)) {
          // no default implementation
        } else if (match_tok(p, TOKEN_ARROW)) {
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
  decl->as.trait_decl.name = name_sv;
  decl->as.trait_decl.type_params = type_params;
  decl->as.trait_decl.type_param_count = type_param_count;
  decl->as.trait_decl.items =
      al_alloc(p->al, sizeof(TraitItemNode) * item_count);
  memcpy(decl->as.trait_decl.items, items, sizeof(TraitItemNode) * item_count);
  decl->as.trait_decl.item_count = item_count;
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

  ImplItemNode items[64];
  int item_count = 0;

  if (!consume_tok(p, TOKEN_LBRACE, "expected '{' after impl header")) {
    had_error = true;
  }

  if (!check_tok(p, TOKEN_RBRACE)) {
    do {
      if (item_count >= 64) {
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
      } else if (check_tok(p, TOKEN_FUN)) {
        Decl *fun_decl = parse_fun_decl(p, false);
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
  decl->as.impl_decl.self_type = target;
  decl->as.impl_decl.trait_type = trait;
  decl->as.impl_decl.type_params = type_params;
  decl->as.impl_decl.type_param_count = type_param_count;
  decl->as.impl_decl.items = al_alloc(p->al, sizeof(ImplItemNode) * item_count);
  memcpy(decl->as.impl_decl.items, items, sizeof(ImplItemNode) * item_count);
  decl->as.impl_decl.item_count = item_count;
  return decl;
}

static Decl *parse_decl(Parser *p) {
  Decl *decl = NULL;

  if (check_tok(p, TOKEN_USE)) {
    decl = parse_use_decl(p);
  } else {
    bool is_pub = match_tok(p, TOKEN_PUB);
    if (check_tok(p, TOKEN_VAR)) {
      decl = parse_var_decl(p, is_pub);
    } else if (check_tok(p, TOKEN_FUN)) {
      decl = parse_fun_decl(p, is_pub);
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

  if (p->panic_mode) {
    sync_to_decl(p);
  }

  return decl;
}

// public API

void parser_init(Parser *p, Token *tokens, int count, Allocator *al,
                 ErrorReporter *reporter) {
  p->tokens = tokens;
  p->count = count;
  p->current = 0;
  p->al = al;
  p->reporter = reporter;
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