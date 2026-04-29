
#include "compiler.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "allocator.h"
#include "opcode.h"
#include "proto.h"
#include "scanner.h"
#include "string_utils.h"

StringView sv_from_token(Token *token) {
  return (StringView){.chars = token->start, .length = token->length};
}

static bool identifier_equals(Token *a, Token *b) {
  if (a->length != b->length) {
    return false;
  }
  return strncmp(a->start, b->start, a->length) == 0;
}

typedef struct {
  Scanner scanner;

  Token current;
  Token previous;

  bool had_error;
  bool in_panic_mode;
} Parser;

#define COMPILER_MAX_LOCALS 256

typedef struct {
  Token name;
  int depth;
} Local;

typedef struct Compiler {
  struct Compiler *enclosing;

  int scope_depth;
  int local_count;
  Local locals[COMPILER_MAX_LOCALS];

  Proto *proto;
} Compiler;

static void compiler_init(Compiler *compiler, Compiler *enclosing, Allocator *al) {
  compiler->enclosing = enclosing;

  compiler->scope_depth = 0;
  compiler->local_count = 0;

  Local *local = &compiler->locals[compiler->local_count++];
  local->name = (Token){.start = "", .length = 0, .line_number = 0};
  local->depth = 0;

  compiler->proto = al_alloc_for(al, Proto);
  proto_init(compiler->proto, al);
}

typedef struct {
  const char *source;
  Parser parser;

  Compiler *current_compiler;

  Allocator *al;
} Context;

static void ctx_init(Context *ctx, const char *source, Allocator *al) {
  ctx->al = al;
  ctx->source = source;

  scanner_init(&ctx->parser.scanner, source);
  ctx->parser.had_error = false;
  ctx->parser.in_panic_mode = false;

  ctx->current_compiler = NULL;
}

static void ctx_destroy(Context *ctx) { (void)ctx; }

typedef struct {
  Token token;
  const char *label;
} ErrorLabel;

typedef struct {
  const char *message;
  ErrorLabel labels[4];
  int label_count;
} Diagnostic;

static void ctx_print_diagnostic(Context *ctx, Diagnostic *diag) {
  if (ctx->parser.in_panic_mode) {
    return;
  }
  ctx->parser.in_panic_mode = true;
  ctx->parser.had_error = true;

  fprintf(stderr, "error: %s\n", diag->message);

  for (int i = 0; i < diag->label_count; i++) {
    ErrorLabel *lb = &diag->labels[i];
    Token *token = &lb->token;

    const char *line_start = token->source;
    while (line_start > ctx->source && line_start[-1] != '\n') {
      line_start--;
    }

    const char *line_end = token->source;
    while (*line_end != '\n' && *line_end != '\0') {
      line_end++;
    }

    fprintf(stderr, "     |\n");
    fprintf(stderr, "%4d | %.*s\n", token->line_number, (int)(line_end - line_start), line_start);
    fprintf(stderr, "     |%*c^ %s\n", (int)(token->source - line_start) + 1, ' ', lb->label);
  }
  fprintf(stderr, "\n");
}

#define DIAGNOSTIC(ctx, msg, ...)                                                                  \
  do {                                                                                             \
    ErrorLabel _labels[] = {__VA_ARGS__};                                                          \
    Diagnostic _diag = {                                                                           \
        .message = (msg),                                                                          \
        .label_count = sizeof(_labels) / sizeof(_labels[0]),                                       \
        .labels = {},                                                                              \
    };                                                                                             \
    memcpy(_diag.labels, _labels, sizeof(_labels));                                                \
    ctx_print_diagnostic((ctx), &_diag);                                                           \
  } while (0)

static void ctx_error_at(Context *ctx, Token *token, const char *err_msg) {
  DIAGNOSTIC(ctx, err_msg, {*token, ""});
}

static void ctx_advance(Context *ctx) {
  ctx->parser.previous = ctx->parser.current;

  while (true) {
    ctx->parser.current = scanner_next_token(&ctx->parser.scanner);
    if (ctx->parser.current.type != TOKEN_ERROR) {
      break;
    }

    printf("error: %.*s\n", ctx->parser.current.length, ctx->parser.current.start);
  }
}

static inline bool ctx_check(Context *ctx, TokenType type) {
  return ctx->parser.current.type == type;
}

static bool ctx_match(Context *ctx, TokenType type) {
  if (!ctx_check(ctx, type)) {
    return false;
  }

  ctx_advance(ctx);
  return true;
}

static bool ctx_consume(Context *ctx, TokenType type, const char *err_msg) {
  if (ctx_check(ctx, type)) {
    ctx_advance(ctx);
    return true;
  }
  ctx_error_at(ctx, &ctx->parser.current, err_msg);

  return false;
}

static void ctx_emit_byte(Context *ctx, uint8_t byte) {
  proto_write_code(ctx->current_compiler->proto, byte, ctx->parser.previous.line_number, ctx->al);
}

static void _ctx_emit_bytes(Context *ctx, uint8_t *bytes, int length) {
  for (int i = 0; i < length; i++) {
    ctx_emit_byte(ctx, bytes[i]);
  }
}

#define ctx_emit_bytes(ctx, ...)                                                                   \
  do {                                                                                             \
    uint8_t codes[] = {__VA_ARGS__};                                                               \
    _ctx_emit_bytes(ctx, codes, sizeof(codes) / sizeof(codes[0]));                                 \
  } while (0)

static void ctx_emit_return(Context *ctx) { ctx_emit_byte(ctx, OP_RETURN); }

typedef struct {
  int index;
} ConstantRef;

static ConstantRef ctx_make_constant(Context *ctx, RawConstant val) {
  int index = proto_write_constant(ctx->current_compiler->proto, val, ctx->al);
  return (ConstantRef){.index = index};
}

static void ctx_emit_constant(Context *ctx, ConstantRef ref) {
  if (ref.index > UINT8_MAX) {
    ctx_error_at(ctx, &ctx->parser.previous, "too many constants in one chunk");
    return;
  }
  ctx_emit_bytes(ctx, OP_CONSTANT, (uint8_t)ref.index);
}

static void ctx_emit_number(Context *ctx, double number) {
  ConstantRef ref = ctx_make_constant(ctx, (RawConstant){.type = RAW_NUMBER, .as.number = number});
  ctx_emit_constant(ctx, ref);
}

// -----------------------------------------------------------------------------

static void ctx_add_local(Context *ctx, Token *name) {
  if (ctx->current_compiler->local_count >= UINT8_MAX) {
    ctx_error_at(ctx, name, "too many local variables in function.");
    return;
  }

  Local *local =
      &ctx->current_compiler->locals[ctx->current_compiler->local_count++];
  local->name = *name;
  local->depth = -1;
}

static ConstantRef ctx_identifier_constant(Context *ctx, Token *name) {
  String str = str_from_sv(sv_from_token(name), ctx->al);
  RawConstant constant = {.type = RAW_STRING, .as.string = str};
  return ctx_make_constant(ctx, constant);
}

// declare a variable in the current scope
static void ctx_declare_variable(Context *ctx, Token *name) {
  if (ctx->current_compiler->scope_depth == 0) {
    return;
  }

  for (int i = ctx->current_compiler->local_count - 1; i >= 0; i--) {
    Local *local = &ctx->current_compiler->locals[i];
    if (local->depth != -1 &&
        local->depth < ctx->current_compiler->scope_depth) {
      break; // out of current depth
    }

    if (identifier_equals(name, &local->name)) {
      DIAGNOSTIC(ctx, "already a variable with this name in this scope.",
                 {local->name, "first declared here"},
                 {ctx->parser.previous, "re-declared here"});
    }
  }

  ctx_add_local(ctx, name);
}

static ConstantRef ctx_parse_variable(Context *ctx, const char *err_msg) {
  ctx_consume(ctx, TOKEN_IDENTIFIER, err_msg);
  ctx_declare_variable(ctx, &ctx->parser.previous);

  if (ctx->current_compiler->scope_depth > 0) {
    return (ConstantRef){.index = 0};
  }

  return ctx_identifier_constant(ctx, &ctx->parser.previous);
}

// define a global variable with the given name,
static void ctx_define_variable(Context *ctx, ConstantRef name) {
  if (ctx->current_compiler->scope_depth > 0) {
    return; // local variables are implicitly defined when declared
  }
  ctx_emit_bytes(ctx, OP_DEFINE_GLOBAL, (uint8_t)name.index);
}

static void ctx_emit_get_global(Context *ctx, ConstantRef name) {
  ctx_emit_bytes(ctx, OP_GET_GLOBAL, (uint8_t)name.index);
}

static void ctx_emit_set_global(Context *ctx, ConstantRef name) {
  ctx_emit_bytes(ctx, OP_SET_GLOBAL, (uint8_t)name.index);
}

// -----------------------------------------------------------------------------

static void ctx_begin_compile(Context *ctx, Compiler *compiler) {
  compiler_init(compiler, ctx->current_compiler, ctx->al);
  ctx->current_compiler = compiler;
}

static Proto *ctx_end_compile(Context *ctx) {
  ctx_emit_return(ctx);
  Compiler *compiler = ctx->current_compiler;
  ctx->current_compiler = compiler->enclosing;
  return compiler->proto;
}

typedef void (*ParseFunc)(Context *ctx, bool can_assign);

typedef enum {
  PREC_NONE,
  PREC_ASSIGNMENT, // =
  PREC_OR,         // or
  PREC_AND,        // and
  PREC_EQUALITY,   // == !=
  PREC_COMPARISON, // < > <= >=
  PREC_TERM,       // + -
  PREC_FACTOR,     // * /
  PREC_UNARY,      // ! -
  PREC_CALL,       // . ()
  PREC_PRIMARY
} Precedence;

typedef struct {
  ParseFunc prefix;
  ParseFunc infix;
  Precedence precedence;
} ParseRule;

static void grouping(Context *ctx, bool can_assign);
static void number(Context *ctx, bool can_assign);
static void literal(Context *ctx, bool can_assign);
static void string(Context *ctx, bool can_assign);
static void binary(Context *ctx, bool can_assign);
static void unary(Context *ctx, bool can_assign);
static void variable(Context *ctx, bool can_assign);

static ParseRule rules[] = {
    [TOKEN_LEFT_PAREN] = {grouping, NULL, PREC_NONE}, //
    [TOKEN_RIGHT_PAREN] = {NULL, NULL, PREC_NONE},    //
    [TOKEN_LEFT_BRACE] = {NULL, NULL, PREC_NONE},     //
    [TOKEN_RIGHT_BRACE] = {NULL, NULL, PREC_NONE},    //
    [TOKEN_COLON] = {NULL, NULL, PREC_NONE},          //
    [TOKEN_COMMA] = {NULL, NULL, PREC_NONE},          //
    [TOKEN_DOT] = {NULL, NULL, PREC_NONE},            //
    [TOKEN_MINUS] = {unary, binary, PREC_TERM},       //
    [TOKEN_MINUS_MINUS] = {NULL, NULL, PREC_NONE},    //
    [TOKEN_PLUS] = {NULL, binary, PREC_TERM},         //
    [TOKEN_PLUS_PLUS] = {NULL, NULL, PREC_NONE},      //
    [TOKEN_QUESTION] = {NULL, NULL, PREC_NONE},       //
    [TOKEN_SEMICOLON] = {NULL, NULL, PREC_NONE},      //
    [TOKEN_SLASH] = {NULL, binary, PREC_FACTOR},      //
    [TOKEN_STAR] = {NULL, binary, PREC_FACTOR},       //

    [TOKEN_BANG] = {unary, NULL, PREC_NONE},                 //
    [TOKEN_BANG_EQUAL] = {NULL, binary, PREC_COMPARISON},    //
    [TOKEN_EQUAL] = {NULL, NULL, PREC_NONE},                 //
    [TOKEN_EQUAL_EQUAL] = {NULL, binary, PREC_COMPARISON},   //
    [TOKEN_GREATER] = {NULL, binary, PREC_COMPARISON},       //
    [TOKEN_GREATER_EQUAL] = {NULL, binary, PREC_COMPARISON}, //
    [TOKEN_LESS] = {NULL, binary, PREC_COMPARISON},          //
    [TOKEN_LESS_EQUAL] = {NULL, binary, PREC_COMPARISON},    //
    [TOKEN_FAT_ARROW] = {NULL, NULL, PREC_NONE},             //

    [TOKEN_IDENTIFIER] = {variable, NULL, PREC_NONE}, //
    [TOKEN_STRING] = {string, NULL, PREC_NONE},       //
    [TOKEN_NUMBER] = {number, NULL, PREC_NONE},       //

    [TOKEN_AND] = {NULL, NULL, PREC_NONE},      //
    [TOKEN_AS] = {NULL, NULL, PREC_NONE},       //
    [TOKEN_BREAK] = {NULL, NULL, PREC_NONE},    //
    [TOKEN_CONTINUE] = {NULL, NULL, PREC_NONE}, //
    [TOKEN_ELSE] = {NULL, NULL, PREC_NONE},     //
    [TOKEN_FALSE] = {literal, NULL, PREC_NONE}, //
    [TOKEN_FOR] = {NULL, NULL, PREC_NONE},      //
    [TOKEN_FUN] = {NULL, NULL, PREC_NONE},      //
    [TOKEN_IF] = {NULL, NULL, PREC_NONE},       //
    [TOKEN_IN] = {NULL, NULL, PREC_NONE},       //
    [TOKEN_IS] = {NULL, NULL, PREC_NONE},       //
    [TOKEN_IMPL] = {NULL, NULL, PREC_NONE},     //
    [TOKEN_MATCH] = {NULL, NULL, PREC_NONE},    //
    [TOKEN_NIL] = {literal, NULL, PREC_NONE},   //
    [TOKEN_OR] = {NULL, NULL, PREC_NONE},       //
    [TOKEN_RETURN] = {NULL, NULL, PREC_NONE},   //
    [TOKEN_STRUCT] = {NULL, NULL, PREC_NONE},   //
    [TOKEN_THIS] = {NULL, NULL, PREC_NONE},     //
    [TOKEN_TRAIT] = {NULL, NULL, PREC_NONE},    //
    [TOKEN_TRUE] = {literal, NULL, PREC_NONE},  //
    [TOKEN_VAR] = {NULL, NULL, PREC_NONE},      //
    [TOKEN_VARIANT] = {NULL, NULL, PREC_NONE},  //

    [TOKEN_ERROR] = {NULL, NULL, PREC_NONE}, //
    [TOKEN_EOF] = {NULL, NULL, PREC_NONE}    //
};

static inline ParseRule *get_rule(TokenType type) { return &rules[type]; }

static void parse_precedence(Context *ctx, Precedence precedence) {
  ctx_advance(ctx);
  ParseFunc prefix_rule = get_rule(ctx->parser.previous.type)->prefix;
  if (prefix_rule == NULL) {
    ctx_error_at(ctx, &ctx->parser.previous, "expect expression.");
    return;
  }

  bool can_assign = precedence <= PREC_ASSIGNMENT;
  prefix_rule(ctx, can_assign);

  while (precedence <= get_rule(ctx->parser.current.type)->precedence) {
    ctx_advance(ctx);
    ParseFunc infix_rule = get_rule(ctx->parser.previous.type)->infix;
    infix_rule(ctx, can_assign);
  }

  if (can_assign && ctx_match(ctx, TOKEN_EQUAL)) {
    ctx_error_at(ctx, &ctx->parser.previous, "invalid assignment target.");
  }
}

static void expression(Context *ctx) { parse_precedence(ctx, PREC_ASSIGNMENT); }

static void statement_expression(Context *ctx) {
  expression(ctx);
  ctx_consume(ctx, TOKEN_SEMICOLON, "expect ';' after expression.");
  ctx_emit_byte(ctx, OP_POP);
}

static void statement(Context *ctx) { statement_expression(ctx); }

static void ctx_synchronize(Context *ctx) {
  ctx->parser.in_panic_mode = false;

  while (ctx->parser.current.type != TOKEN_EOF) {
    if (ctx->parser.current.type == TOKEN_SEMICOLON) {
      ctx_advance(ctx);
      return;
    }

    switch (ctx->parser.current.type) {
    case TOKEN_RIGHT_BRACE:
    case TOKEN_FUN:
    case TOKEN_VAR:
    case TOKEN_FOR:
    case TOKEN_IF:
    case TOKEN_RETURN:
    case TOKEN_STRUCT:
    case TOKEN_TRAIT:
    case TOKEN_IMPL:
    case TOKEN_VARIANT:
      return;
    default:
      break;
    }

    ctx_advance(ctx);
  }
}

static void declaration_var(Context *ctx) {
  ConstantRef name = ctx_parse_variable(ctx, "expect variable name.");
  if (ctx_match(ctx, TOKEN_EQUAL)) {
    expression(ctx);
  } else {
    ctx_emit_byte(ctx, OP_NIL);
  }
  ctx_consume(ctx, TOKEN_SEMICOLON, "expect ';' after variable declaration.");
  ctx_define_variable(ctx, name);
}

static void declaration(Context *ctx) {
  if (ctx_match(ctx, TOKEN_VAR)) {
    declaration_var(ctx);
  } else {
    statement(ctx);
  }

  if (ctx->parser.in_panic_mode) {
    ctx_synchronize(ctx);
  }
}

static void grouping(Context *ctx, bool can_assign) {
  (void)can_assign;

  expression(ctx);
  ctx_consume(ctx, TOKEN_RIGHT_PAREN, "expect ')' after expression.");
}

static void number(Context *ctx, bool can_assign) {
  (void)can_assign;

  double value = strtod(ctx->parser.previous.start, NULL);
  ctx_emit_number(ctx, value);
}

static void literal(Context *ctx, bool can_assign) {
  (void)can_assign;

  switch (ctx->parser.previous.type) {
  case TOKEN_FALSE:
    ctx_emit_byte(ctx, OP_FALSE);
    break;
  case TOKEN_NIL:
    ctx_emit_byte(ctx, OP_NIL);
    break;
  case TOKEN_TRUE:
    ctx_emit_byte(ctx, OP_TRUE);
    break;
  default:
    assert(false && "invalid literal");
    return; // unreachable
  }
}

static void string(Context *ctx, bool can_assign) {
  (void)can_assign;
  StringView sv = (StringView){.chars = ctx->parser.previous.start + 1,
                               .length = ctx->parser.previous.length - 2};
  String str = str_from_sv(sv, ctx->al);
  RawConstant constant = {.type = RAW_STRING, .as.string = str};
  ConstantRef ref = ctx_make_constant(ctx, constant);
  ctx_emit_constant(ctx, ref);
}

static void binary(Context *ctx, bool can_assign) {
  (void)can_assign;

  TokenType operator_type = ctx->parser.previous.type;
  const ParseRule *rule = get_rule(operator_type);
  parse_precedence(ctx, rule->precedence + 1);

  switch (operator_type) {
  case TOKEN_BANG_EQUAL:
    ctx_emit_bytes(ctx, OP_EQUAL, OP_NOT);
    break;
  case TOKEN_EQUAL_EQUAL:
    ctx_emit_byte(ctx, OP_EQUAL);
    break;
  case TOKEN_GREATER:
    ctx_emit_byte(ctx, OP_GREATER);
    break;
  case TOKEN_GREATER_EQUAL:
    ctx_emit_bytes(ctx, OP_LESS, OP_NOT);
    break;
  case TOKEN_LESS:
    ctx_emit_byte(ctx, OP_LESS);
    break;
  case TOKEN_LESS_EQUAL:
    ctx_emit_bytes(ctx, OP_GREATER, OP_NOT);
    break;
  case TOKEN_BANG:
    ctx_emit_byte(ctx, OP_NOT);
    break;
  case TOKEN_PLUS:
    ctx_emit_byte(ctx, OP_ADD);
    break;
  case TOKEN_MINUS:
    ctx_emit_byte(ctx, OP_SUBTRACT);
    break;
  case TOKEN_STAR:
    ctx_emit_byte(ctx, OP_MULTIPLY);
    break;
  case TOKEN_SLASH:
    ctx_emit_byte(ctx, OP_DIVIDE);
    break;
  default:
    assert(false && "invalid binary operator");
    return; // unreachable
  }
}

static void unary(Context *ctx, bool can_assign) {
  (void)can_assign;

  TokenType operator_type = ctx->parser.previous.type;
  parse_precedence(ctx, PREC_UNARY);

  switch (operator_type) {
  case TOKEN_BANG:
    ctx_emit_byte(ctx, OP_NOT);
    break;
  case TOKEN_MINUS:
    ctx_emit_byte(ctx, OP_NEGATE);
    break;
  default:
    assert(false && "invalid unary operator");
    return; // unreachable
  }
}

static void variable(Context *ctx, bool can_assign) {
  ConstantRef name = ctx_identifier_constant(ctx, &ctx->parser.previous);

  if (can_assign && ctx_match(ctx, TOKEN_EQUAL)) {
    expression(ctx);
    ctx_emit_set_global(ctx, name);
  } else {
    ctx_emit_get_global(ctx, name);
  }
}

// -----------------------------------------------------------------------------

Proto *compile(const char *source, Allocator *al) {
  Context ctx;
  ctx_init(&ctx, source, al);

  Compiler root_compiler;
  ctx_begin_compile(&ctx, &root_compiler);

  ctx_advance(&ctx);

  while (!ctx_check(&ctx, TOKEN_EOF)) {
    declaration(&ctx);
  }

  Proto *proto = ctx_end_compile(&ctx);

  ctx_destroy(&ctx);

  return proto;
}