#include "compiler.h"

#include <assert.h>
#include <complex.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "allocator.h"
#include "definition.h"
#include "opcode.h"
#include "proto.h"
#include "scanner.h"
#include "string_utils.h"

// ===== type definitions =====

typedef struct {
  Scanner scanner;

  Token current;
  Token previous;

  bool had_error;
  bool in_panic_mode;
} Parser;

typedef struct {
  Token name;
  int depth;
  bool is_captured;
} Local;

typedef struct {
  uint8_t index;
  bool is_local;
} Upvalue;

#define MAX_BREAK_JUMPS 16

typedef struct Loop {
  struct Loop *enclosing;
  int start_offset;
  int break_count;
  int break_jumps[MAX_BREAK_JUMPS];
  int scope_depth;
  Token label;
} Loop;

#define COMPILER_MAX_LOCALS 256

typedef struct Compiler {
  struct Compiler *enclosing;

  int scope_depth;
  int local_count;
  Local locals[COMPILER_MAX_LOCALS];
  Upvalue upvalues[COMPILER_MAX_LOCALS];

  Loop *current_loop;

  Proto *proto;
} Compiler;

typedef struct {
  const char *source;
  Parser parser;

  DefinitionRegistry def_reg;
  Compiler *current_compiler;

  Allocator *al;
} Context;

typedef struct {
  Token token;
  const char *label;
} ErrorLabel;

typedef struct {
  const char *message;
  ErrorLabel labels[4];
  int label_count;
} Diagnostic;

typedef struct {
  int index;
} ConstantRef;

typedef enum {
  REF_LOCAL,
  REF_UPVALUE,
  REF_DEFINITION,
  REF_GLOBAL,
} RefScope;

typedef struct {
  RefScope scope;
  union {
    int local_index;      // if scope == REF_LOCAL
    int upvalue_index;    // if scope == REF_UPVALUE
    int definition_index; // if scope == REF_DEFINITION
    ConstantRef global;   // if scope == REF_GLOBAL
  };
} VarRef;

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

// ===== constants & macros =====

static void declaration(Context *ctx);
static void declaration_var(Context *ctx);
static void statement(Context *ctx);
static void statement_block(Context *ctx);
static void expression(Context *ctx);
static void ctx_advance(Context *ctx);

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

#define ctx_emit_bytes(ctx, ...)                                                                   \
  do {                                                                                             \
    uint8_t codes[] = {__VA_ARGS__};                                                               \
    _ctx_emit_bytes(ctx, codes, sizeof(codes) / sizeof(codes[0]));                                 \
  } while (0)

// ===== utility functions =====

StringView sv_from_token(Token *token) {
  return (StringView){.chars = token->start, .length = token->length};
}

static bool identifier_equals(Token *a, Token *b) {
  if (a->length != b->length) {
    return false;
  }
  return strncmp(a->start, b->start, a->length) == 0;
}

static uint8_t compound_assignment_opcode(TokenType type) {
  switch (type) {
  case TOKEN_PLUS_EQUAL:
    return OP_ADD;
  case TOKEN_MINUS_EQUAL:
    return OP_SUBTRACT;
  case TOKEN_STAR_EQUAL:
    return OP_MULTIPLY;
  case TOKEN_SLASH_EQUAL:
    return OP_DIVIDE;
  default:
    assert(false && "not a compound assignment operator");
    return 0;
  }
}

// ===== core initialization =====

static void compiler_init(Compiler *compiler, Compiler *enclosing, Allocator *al) {
  compiler->enclosing = enclosing;

  compiler->scope_depth = 0;
  compiler->local_count = 0;
  compiler->current_loop = NULL;

  compiler->proto = al_alloc_for(al, Proto);
  proto_init(compiler->proto, al);

  Local *local = &compiler->locals[compiler->local_count++];
  local->name = (Token){.start = "", .length = 0, .line_number = 0};
  local->depth = 0;
  local->is_captured = false;
}

static void ctx_init(Context *ctx, const char *source, const DefinitionArray *prior_defs,
                     Allocator *al) {
  ctx->al = al;
  ctx->source = source;

  scanner_init(&ctx->parser.scanner, source);
  ctx->parser.had_error = false;
  ctx->parser.in_panic_mode = false;

  ctx->current_compiler = NULL;

  defreg_init(&ctx->def_reg, al);

  // seed definition registry with prior definitions from other modules
  int def_count = arr_count(prior_defs);
  for (int i = 0; i < def_count; i++) {
    Definition def = def_copy(&prior_defs->data[i], al);
    defreg_add(&ctx->def_reg, def, al);
  }
}

static void ctx_destroy(Context *ctx) { defreg_destroy(&ctx->def_reg, ctx->al); }

// ===== error handling =====

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

static void ctx_error_at(Context *ctx, Token *token, const char *fmt, ...) {
  char buf[256];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  DIAGNOSTIC(ctx, buf, {*token, ""});
}

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

// ===== parser token operations =====

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

static bool ctx_match_compound_assignment(Context *ctx) {
  return ctx_match(ctx, TOKEN_PLUS_EQUAL) || ctx_match(ctx, TOKEN_MINUS_EQUAL) ||
         ctx_match(ctx, TOKEN_STAR_EQUAL) || ctx_match(ctx, TOKEN_SLASH_EQUAL);
}

static bool ctx_consume(Context *ctx, TokenType type, const char *err_msg) {
  if (ctx_check(ctx, type)) {
    ctx_advance(ctx);
    return true;
  }
  ctx_error_at(ctx, &ctx->parser.current, err_msg);

  return false;
}

// ===== code emission =====

static void ctx_emit_byte(Context *ctx, uint8_t byte) {
  proto_write_code(ctx->current_compiler->proto, byte, ctx->parser.previous.line_number, ctx->al);
}

static void _ctx_emit_bytes(Context *ctx, uint8_t *bytes, int length) {
  for (int i = 0; i < length; i++) {
    ctx_emit_byte(ctx, bytes[i]);
  }
}

static void ctx_emit_return(Context *ctx) {
  if (arr_count(ctx->current_compiler->proto->code) > 0 &&
      arr_peek(ctx->current_compiler->proto->code) == OP_RETURN) {
    return; // already has a return at the end
  }

  // implicitly return nil
  ctx_emit_byte(ctx, OP_NIL);
  ctx_emit_byte(ctx, OP_RETURN);
}

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

static void ctx_emit_closure(Context *ctx, Compiler *compiler, ConstantRef ref) {
  Proto *proto = compiler->proto;

  if (ref.index > UINT8_MAX) {
    ctx_error_at(ctx, &ctx->parser.previous, "too many constants in one chunk");
    return;
  }
  ctx_emit_bytes(ctx, OP_CLOSURE, (uint8_t)ref.index);
  for (int i = 0; i < proto->upvalue_count; i++) {
    Upvalue *upvalue = &compiler->upvalues[i];
    ctx_emit_bytes(ctx, upvalue->is_local ? 1 : 0, upvalue->index);
  }
}

static void ctx_emit_number(Context *ctx, double number) {
  if (number == (int)number && number >= 0 && number <= UINT8_MAX) {
    ctx_emit_bytes(ctx, OP_BYTE, (uint8_t)number);
    return;
  }

  ConstantRef ref = ctx_make_constant(ctx, (RawConstant){.type = RAW_NUMBER, .as.number = number});
  ctx_emit_constant(ctx, ref);
}

static size_t ctx_code_offset(Context *ctx) {
  return arr_count(ctx->current_compiler->proto->code);
}

static int ctx_emit_jump(Context *ctx, uint8_t instruction) {
  ctx_emit_bytes(ctx, instruction, 0, 0);
  return ctx_code_offset(ctx) - 2;
}

static void ctx_patch_jump(Context *ctx, int offset) {
  int jump = ctx_code_offset(ctx) - offset - 2;

  if (jump > UINT16_MAX) {
    ctx_error_at(ctx, &ctx->parser.previous, "too much code to jump over.");
  }

  proto_patch_short(ctx->current_compiler->proto, offset, (uint16_t)jump);
}

static void ctx_emit_loop(Context *ctx, int loop_start) {
  ctx_emit_byte(ctx, OP_LOOP);

  int offset = ctx_code_offset(ctx) - loop_start + 2;
  if (offset > UINT16_MAX) {
    ctx_error_at(ctx, &ctx->parser.previous, "loop body too large.");
  }

  ctx_emit_bytes(ctx, (offset >> 8) & 0xff, offset & 0xff);
}

static void ctx_mark_patch_site(Context *ctx) {
  int offset = ctx_code_offset(ctx);
  proto_add_patch(ctx->current_compiler->proto, offset, ctx->al);
}

// ===== variable management =====

static void ctx_add_local(Context *ctx, Token *name) {
  if (ctx->current_compiler->local_count >= UINT8_MAX) {
    ctx_error_at(ctx, name, "too many local variables in function.");
    return;
  }

  Local *local = &ctx->current_compiler->locals[ctx->current_compiler->local_count++];
  local->name = *name;
  local->depth = -1;
}

static int ctx_add_upvalue(Context *ctx, Compiler *compiler, uint8_t index, bool is_local) {
  int i = compiler->proto->upvalue_count;
  for (int j = 0; j < i; j++) {
    Upvalue *upvalue = &compiler->upvalues[j];
    if (upvalue->index == index && upvalue->is_local == is_local) {
      return j; // already have this upvalue
    }
  }

  if (i == UINT8_MAX) {
    ctx_error_at(ctx, &ctx->parser.previous, "too many closure variables in function.");
    return -1;
  }

  compiler->upvalues[i].index = index;
  compiler->upvalues[i].is_local = is_local;
  return compiler->proto->upvalue_count++;
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
    if (local->depth != -1 && local->depth < ctx->current_compiler->scope_depth) {
      break; // out of current depth
    }

    if (identifier_equals(name, &local->name)) {
      DIAGNOSTIC(ctx, "already a variable with this name in this scope.",
                 {local->name, "first declared here"}, {ctx->parser.previous, "re-declared here"});
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

// mark all variables in the current scope as initialized
static void ctx_mark_initialized(Context *ctx) {
  Compiler *c = ctx->current_compiler;
  if (c->scope_depth == 0) {
    return;
  }
  for (int i = c->local_count - 1; i >= 0; i--) {
    Local *local = &c->locals[i];
    if (local->depth != -1 && local->depth < c->scope_depth) {
      break; // out of current depth
    }
    if (local->depth == -1) {
      local->depth = c->scope_depth;
    }
  }
}

static int ctx_resolve_local(Context *ctx, Compiler *compiler, Token *name) {
  for (int i = compiler->local_count - 1; i >= 0; i--) {
    Local *local = &compiler->locals[i];
    if (!identifier_equals(name, &local->name)) {
      continue; // keep looking
    }
    if (local->depth == -1) {
      ctx_error_at(ctx, name, "can't read local variable in its own initializer.");
    }
    return i;
  }
  return -1;
}

static int ctx_resolve_upvalue(Context *ctx, Compiler *compiler, Token *name) {
  if (compiler->enclosing == NULL) {
    return -1; // not found
  }

  int local_index = ctx_resolve_local(ctx, compiler->enclosing, name);
  if (local_index != -1) {
    compiler->enclosing->locals[local_index].is_captured = true;
    return ctx_add_upvalue(ctx, compiler, (uint8_t)local_index, true);
  }

  int upvalue_index = ctx_resolve_upvalue(ctx, compiler->enclosing, name);
  if (upvalue_index != -1) {
    return ctx_add_upvalue(ctx, compiler, (uint8_t)upvalue_index, false);
  }

  return -1; // not found
}

static int ctx_resolve_definition(Context *ctx, Token *name) {
  DefinitionEntry *def = defreg_find(&ctx->def_reg, sv_from_token(name));
  if (def == NULL) {
    return -1;
  }
  return def->index;
}

static VarRef ctx_resolve_variable(Context *ctx, Token name) {
  VarRef ref;
  Compiler *target = ctx->current_compiler;
  if ((ref.local_index = ctx_resolve_local(ctx, target, &name)) > -1) {
    ref.scope = REF_LOCAL;
  } else if ((ref.upvalue_index = ctx_resolve_upvalue(ctx, target, &name)) > -1) {
    ref.scope = REF_UPVALUE;
  } else if ((ref.definition_index = ctx_resolve_definition(ctx, &name)) > -1) {
    ref.scope = REF_DEFINITION;
  } else {
    ref.global = ctx_identifier_constant(ctx, &name);
    ref.scope = REF_GLOBAL;
  }
  return ref;
}

static void ctx_emit_get(Context *ctx, VarRef ref) {
  switch (ref.scope) {
  case REF_LOCAL:
    ctx_emit_bytes(ctx, OP_GET_LOCAL, (uint8_t)ref.local_index);
    break;
  case REF_UPVALUE:
    ctx_emit_bytes(ctx, OP_GET_UPVALUE, (uint8_t)ref.upvalue_index);
    break;
  case REF_DEFINITION:
    ctx_mark_patch_site(ctx);
    ctx_emit_bytes(ctx, OP_GET_DEFINITION, (uint8_t)ref.definition_index);
    break;
  case REF_GLOBAL:
    ctx_emit_bytes(ctx, OP_GET_GLOBAL, (uint8_t)ref.global.index);
    break;
  }
}

static void ctx_emit_set(Context *ctx, VarRef ref) {
  switch (ref.scope) {
  case REF_LOCAL:
    ctx_emit_bytes(ctx, OP_SET_LOCAL, (uint8_t)ref.local_index);
    break;
  case REF_UPVALUE:
    ctx_emit_bytes(ctx, OP_SET_UPVALUE, (uint8_t)ref.upvalue_index);
    break;
  case REF_DEFINITION:
    ctx_error_at(ctx, &ctx->parser.previous, "can't assign to a definition.");
    break;
  case REF_GLOBAL:
    ctx_emit_bytes(ctx, OP_SET_GLOBAL, (uint8_t)ref.global.index);
    break;
  }
}

static void ctx_named_variable(Context *ctx, Token name, bool can_assign) {
  VarRef ref = ctx_resolve_variable(ctx, name);
  if (can_assign && ctx_match(ctx, TOKEN_EQUAL)) {
    expression(ctx);
    ctx_emit_set(ctx, ref);
  } else if (can_assign && ctx_match_compound_assignment(ctx)) {
    TokenType operator = ctx->parser.previous.type;
    ctx_emit_get(ctx, ref);
    expression(ctx);
    ctx_emit_byte(ctx, compound_assignment_opcode(operator));
    ctx_emit_set(ctx, ref);
  } else {
    ctx_emit_get(ctx, ref);
  }
}

static void ctx_define_variable(Context *ctx, ConstantRef name) {
  if (ctx->current_compiler->scope_depth > 0) {
    ctx_mark_initialized(ctx);
    return;
  }
  ctx_emit_bytes(ctx, OP_DEFINE_GLOBAL, (uint8_t)name.index);
}

// ===== loop management =====

Loop *ctx_resolve_loop(Context *ctx, Token *label) {
  Loop *loop = ctx->current_compiler->current_loop;
  while (loop != NULL) {
    if (identifier_equals(label, &loop->label)) {
      return loop;
    }
    loop = loop->enclosing;
  }
  return NULL;
}

// ===== scope management =====

static void ctx_begin_compile(Context *ctx, Compiler *compiler) {
  compiler_init(compiler, ctx->current_compiler, ctx->al);
  ctx->current_compiler = compiler;

  if (ctx->parser.previous.type == TOKEN_IDENTIFIER) {
    ctx->current_compiler->proto->name = str_from_sv(sv_from_token(&ctx->parser.previous), ctx->al);
  }
}

static Proto *ctx_end_compile(Context *ctx) {
  ctx_emit_return(ctx);
  Compiler *compiler = ctx->current_compiler;
  ctx->current_compiler = compiler->enclosing;
  return compiler->proto;
}

static void ctx_begin_scope(Context *ctx) { ctx->current_compiler->scope_depth++; }

static void ctx_end_scope(Context *ctx) {
  assert(ctx->current_compiler->scope_depth > 0 && "scope underflow");
  Compiler *c = ctx->current_compiler;
  c->scope_depth--;

  // pop all variables in the current scope
  while (c->local_count > 0 && c->locals[c->local_count - 1].depth > c->scope_depth) {
    ctx_emit_byte(ctx, c->locals[c->local_count - 1].is_captured ? OP_CLOSE_UPVALUE : OP_POP);
    c->local_count--;
  }
}

static void ctx_begin_loop(Context *ctx, Loop *loop, int start, Token *label) {
  *loop = (Loop){
      .enclosing = ctx->current_compiler->current_loop,
      .start_offset = start,
      .break_count = 0,
      .scope_depth = ctx->current_compiler->scope_depth,
      .label = label ? *label : (Token){0},
  };
  ctx->current_compiler->current_loop = loop;
}

static void ctx_end_loop(Context *ctx) {
  assert(ctx->current_compiler->current_loop != NULL && "no loop to end");
  ctx->current_compiler->current_loop = ctx->current_compiler->current_loop->enclosing;
}

// ===== parameter & argument parsing =====

static int ctx_parse_parameter_list(Context *ctx) {
  ctx_consume(ctx, TOKEN_LEFT_PAREN, "expect '(' before parameter list.");

  int param_count = 0;

  if (!ctx_check(ctx, TOKEN_RIGHT_PAREN)) {
    do {
      if (param_count >= UINT8_MAX) {
        ctx_error_at(ctx, &ctx->parser.current, "can't have more than 255 parameters.");
      }
      ConstantRef param_name = ctx_parse_variable(ctx, "expect parameter name.");
      ctx_mark_initialized(ctx);
      ctx_define_variable(ctx, param_name);
      param_count++;
    } while (ctx_match(ctx, TOKEN_COMMA));
  }

  ctx_consume(ctx, TOKEN_RIGHT_PAREN, "expect ')' after parameter list.");
  return param_count;
}

int ctx_parse_argument_list(Context *ctx) {
  int arg_count = 0;
  if (!ctx_check(ctx, TOKEN_RIGHT_PAREN)) {
    do {
      expression(ctx);
      if (arg_count >= UINT8_MAX) {
        ctx_error_at(ctx, &ctx->parser.current, "can't have more than 255 arguments.");
      }
      arg_count++;
    } while (ctx_match(ctx, TOKEN_COMMA));
  }
  ctx_consume(ctx, TOKEN_RIGHT_PAREN, "expect ')' after arguments.");
  return arg_count;
}

static void ctx_compile_function(Context *ctx) {
  Compiler compiler;
  ctx_begin_compile(ctx, &compiler);

  ctx_begin_scope(ctx);

  compiler.proto->arity = ctx_parse_parameter_list(ctx);

  ctx_consume(ctx, TOKEN_LEFT_BRACE, "expect '{' before function body.");
  statement_block(ctx);

  Proto *proto = ctx_end_compile(ctx);
  ConstantRef function_ref =
      ctx_make_constant(ctx, (RawConstant){.type = RAW_FUNCTION, .as.function = proto});

  if (proto->upvalue_count == 0) {
    ctx_emit_constant(ctx, function_ref);
  } else {
    ctx_emit_closure(ctx, &compiler, function_ref);
  }
}

// ===== parser rules & precedence =====

static void grouping(Context *ctx, bool can_assign);
static void call(Context *ctx, bool can_assign);
static void number(Context *ctx, bool can_assign);
static void literal(Context *ctx, bool can_assign);
static void string(Context *ctx, bool can_assign);
static void binary(Context *ctx, bool can_assign);
static void unary(Context *ctx, bool can_assign);
static void variable(Context *ctx, bool can_assign);
static void and_(Context *ctx, bool can_assign);
static void or_(Context *ctx, bool can_assign);

static ParseRule rules[] = {
    [TOKEN_LEFT_PAREN] = {grouping, call, PREC_CALL}, //
    [TOKEN_RIGHT_PAREN] = {NULL, NULL, PREC_NONE},    //
    [TOKEN_LEFT_BRACE] = {NULL, NULL, PREC_NONE},     //
    [TOKEN_RIGHT_BRACE] = {NULL, NULL, PREC_NONE},    //
    [TOKEN_COLON] = {NULL, NULL, PREC_NONE},          //
    [TOKEN_COMMA] = {NULL, NULL, PREC_NONE},          //
    [TOKEN_DOT] = {NULL, NULL, PREC_NONE},            //
    [TOKEN_MINUS] = {unary, binary, PREC_TERM},       //
    [TOKEN_MINUS_MINUS] = {NULL, NULL, PREC_NONE},    //
    [TOKEN_MINUS_EQUAL] = {NULL, NULL, PREC_NONE},    //
    [TOKEN_PLUS] = {NULL, binary, PREC_TERM},         //
    [TOKEN_PLUS_PLUS] = {NULL, NULL, PREC_NONE},      //
    [TOKEN_PLUS_EQUAL] = {NULL, NULL, PREC_NONE},     //
    [TOKEN_QUESTION] = {NULL, NULL, PREC_NONE},       //
    [TOKEN_SEMICOLON] = {NULL, NULL, PREC_NONE},      //
    [TOKEN_SLASH] = {NULL, binary, PREC_FACTOR},      //
    [TOKEN_SLASH_EQUAL] = {NULL, NULL, PREC_NONE},    //
    [TOKEN_STAR] = {NULL, binary, PREC_FACTOR},       //
    [TOKEN_STAR_EQUAL] = {NULL, NULL, PREC_NONE},     //

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

    [TOKEN_AND] = {NULL, and_, PREC_AND},       //
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
    [TOKEN_OR] = {NULL, or_, PREC_OR},          //
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

// ===== expression parsing =====

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

// ===== statement parsing =====

static void statement_expression(Context *ctx) {
  expression(ctx);
  ctx_consume(ctx, TOKEN_SEMICOLON, "expect ';' after expression.");
  ctx_emit_byte(ctx, OP_POP);
}

static void statement_block(Context *ctx) {
  while (!ctx_check(ctx, TOKEN_RIGHT_BRACE) && !ctx_check(ctx, TOKEN_EOF)) {
    declaration(ctx);
  }

  ctx_consume(ctx, TOKEN_RIGHT_BRACE, "expect '}' after block.");
}

static void statement_if(Context *ctx) {
  expression(ctx);

  int else_jump = ctx_emit_jump(ctx, OP_JUMP_IF_FALSE);
  ctx_emit_byte(ctx, OP_POP); // pop condition result

  ctx_begin_scope(ctx);
  ctx_consume(ctx, TOKEN_LEFT_BRACE, "expect '{' after 'if' condition.");
  statement_block(ctx);
  ctx_end_scope(ctx);

  int exit_jump = ctx_emit_jump(ctx, OP_JUMP);

  ctx_patch_jump(ctx, else_jump);
  ctx_emit_byte(ctx, OP_POP);

  if (ctx_match(ctx, TOKEN_ELSE)) {
    if (ctx_match(ctx, TOKEN_IF)) {
      statement_if(ctx); // else if
    } else {
      ctx_begin_scope(ctx);
      ctx_consume(ctx, TOKEN_LEFT_BRACE, "expect '{' after 'else'.");
      statement_block(ctx);
      ctx_end_scope(ctx);
    }
  }

  ctx_patch_jump(ctx, exit_jump);
}

static void statement_for(Context *ctx) {
  ctx_begin_scope(ctx);

  Token loop_label = {0};

  if (ctx_match(ctx, TOKEN_APOSTROPHE)) {
    ctx_consume(ctx, TOKEN_IDENTIFIER, "expect identifier after loop label.");
    loop_label = ctx->parser.previous;
  }

  int loop_start = ctx_code_offset(ctx);
  int exit_jump = -1;
  bool has_condition = false;

  if (ctx_match(ctx, TOKEN_SEMICOLON)) {
    // no initializer
  } else if (ctx_check(ctx, TOKEN_LEFT_BRACE)) {
    // infinite loop
  } else if (ctx_match(ctx, TOKEN_VAR)) {
    declaration_var(ctx);
    ctx_match(ctx, TOKEN_SEMICOLON); // consume optional semicolon after var declaration
    loop_start = ctx_code_offset(ctx);
  } else {
    // ambiguous expression
    int expr_start = ctx_code_offset(ctx);
    expression(ctx);

    if (ctx_match(ctx, TOKEN_SEMICOLON)) {
      // it was initializer
      ctx_emit_byte(ctx, OP_POP); // pop initializer result
      loop_start = ctx_code_offset(ctx);
    } else {
      // it was condition
      loop_start = expr_start;
      has_condition = true;
    }
  }

  if (!has_condition && !ctx_check(ctx, TOKEN_LEFT_BRACE)) {
    if (ctx_match(ctx, TOKEN_SEMICOLON)) {
      // explicitly omit condition for ; ; {}
    } else {
      expression(ctx);
      has_condition = true;
      ctx_match(ctx, TOKEN_SEMICOLON); // consume optional trailing semicolon
    }
  }

  if (has_condition) {
    exit_jump = ctx_emit_jump(ctx, OP_JUMP_IF_FALSE);
    ctx_emit_byte(ctx, OP_POP); // pop condition result
  }

  // if we did not hit body's left brace, it means we have an increment clause
  if (!ctx_check(ctx, TOKEN_LEFT_BRACE)) {
    int body_jump = ctx_emit_jump(ctx, OP_JUMP);
    int increment_start = ctx_code_offset(ctx);

    expression(ctx);
    ctx_match(ctx, TOKEN_SEMICOLON); // optional
    ctx_emit_byte(ctx, OP_POP);

    ctx_emit_loop(ctx, loop_start);
    loop_start = increment_start;
    ctx_patch_jump(ctx, body_jump);
  }

  Loop loop;
  ctx_begin_loop(ctx, &loop, loop_start, &loop_label);

  ctx_match(ctx, TOKEN_SEMICOLON); // optional semicolon before body
  ctx_consume(ctx, TOKEN_LEFT_BRACE, "expect '{' before for body.");

  ctx_begin_scope(ctx);
  statement_block(ctx);
  ctx_end_scope(ctx);

  ctx_emit_loop(ctx, loop_start);
  if (exit_jump != -1) {
    ctx_patch_jump(ctx, exit_jump);
    ctx_emit_byte(ctx, OP_POP);
  }

  for (int i = 0; i < loop.break_count; i++) {
    ctx_patch_jump(ctx, loop.break_jumps[i]);
  }

  ctx_end_loop(ctx);

  ctx_end_scope(ctx);
}

static void statement_break(Context *ctx) {
  if (ctx->current_compiler->current_loop == NULL) {
    ctx_error_at(ctx, &ctx->parser.previous, "can't use 'break' outside of a loop.");
    return;
  }
  Loop *loop = ctx->current_compiler->current_loop;
  if (loop->break_count >= MAX_BREAK_JUMPS) {
    ctx_error_at(ctx, &ctx->parser.previous, "too many 'break' statements in one loop.");
    return;
  }

  if (ctx_match(ctx, TOKEN_IDENTIFIER)) {
    loop = ctx_resolve_loop(ctx, &ctx->parser.previous);
    if (loop == NULL) {
      ctx_error_at(ctx, &ctx->parser.previous, "no enclosing loop with label '%.*s'.",
                   ctx->parser.previous.length, ctx->parser.previous.start);
      return;
    }
  }

  ctx_consume(ctx, TOKEN_SEMICOLON, "expect ';' after 'break'.");

  // pop local variables until end of loop's scope
  const Compiler *c = ctx->current_compiler;
  for (int i = c->local_count - 1; i >= 0 && c->locals[i].depth > loop->scope_depth; i--) {
    ctx_emit_byte(ctx, c->locals[i].is_captured ? OP_CLOSE_UPVALUE : OP_POP);
  }

  loop->break_jumps[loop->break_count++] = ctx_emit_jump(ctx, OP_JUMP);
}

static void statement_continue(Context *ctx) {
  if (ctx->current_compiler->current_loop == NULL) {
    ctx_error_at(ctx, &ctx->parser.current, "can't use 'continue' outside of a loop.");
    return;
  }
  Loop *loop = ctx->current_compiler->current_loop;
  if (ctx_match(ctx, TOKEN_IDENTIFIER)) {
    loop = ctx_resolve_loop(ctx, &ctx->parser.previous);
    if (loop == NULL) {
      ctx_error_at(ctx, &ctx->parser.previous, "no enclosing loop with label '%.*s'.",
                   ctx->parser.previous.length, ctx->parser.previous.start);
      return;
    }
  }

  ctx_consume(ctx, TOKEN_SEMICOLON, "expect ';' after 'continue'.");

  // pop local variables until end of loop's scope
  const Compiler *c = ctx->current_compiler;
  for (int i = c->local_count - 1; i >= 0 && c->locals[i].depth > loop->scope_depth; i--) {
    ctx_emit_byte(ctx, c->locals[i].is_captured ? OP_CLOSE_UPVALUE : OP_POP);
  }

  ctx_emit_loop(ctx, loop->start_offset);
}

static void statement_return(Context *ctx) {
  if (ctx->current_compiler->enclosing == NULL) {
    ctx_error_at(ctx, &ctx->parser.previous, "can't return from top-level code.");
  }

  if (ctx_check(ctx, TOKEN_SEMICOLON)) {
    ctx_emit_return(ctx);
  } else {
    expression(ctx);
    ctx_emit_byte(ctx, OP_RETURN);
  }

  ctx_consume(ctx, TOKEN_SEMICOLON, "expect ';' after return value.");
}

static void statement(Context *ctx) {
  if (ctx_match(ctx, TOKEN_LEFT_BRACE)) {
    ctx_begin_scope(ctx);
    statement_block(ctx);
    ctx_end_scope(ctx);
  } else if (ctx_match(ctx, TOKEN_IF)) {
    statement_if(ctx);
  } else if (ctx_match(ctx, TOKEN_FOR)) {
    statement_for(ctx);
  } else if (ctx_match(ctx, TOKEN_BREAK)) {
    statement_break(ctx);
  } else if (ctx_match(ctx, TOKEN_CONTINUE)) {
    statement_continue(ctx);
  } else if (ctx_match(ctx, TOKEN_RETURN)) {
    statement_return(ctx);
  } else {
    statement_expression(ctx);
  }
}

static void declaration_var(Context *ctx) {
  ConstantRef name_const = ctx_parse_variable(ctx, "expect variable name.");
  Token name = ctx->parser.previous;

  if (ctx->current_compiler->scope_depth == 0) {
    DefinitionEntry *entry = defreg_find(&ctx->def_reg, sv_from_token(&name));
    if (entry != NULL) {
      Definition *def = &ctx->def_reg.defs->data[entry->index];
      ctx_error_at(ctx, &name, "'%.*s' is a %s definition and cannot be shadowed by a variable.",
                   name.length, name.start, def_type_name(def->type));
    }
  }

  if (ctx_match(ctx, TOKEN_EQUAL)) {
    expression(ctx);
  } else {
    ctx_emit_byte(ctx, OP_NIL);
  }
  ctx_consume(ctx, TOKEN_SEMICOLON, "expect ';' after variable declaration.");
  ctx_define_variable(ctx, name_const);
}

static void declaration_fun(Context *ctx) {
  ConstantRef name_const = ctx_parse_variable(ctx, "expect function name.");
  Token name = ctx->parser.previous;

  if (ctx->current_compiler->scope_depth == 0) {
    DefinitionEntry *entry = defreg_find(&ctx->def_reg, sv_from_token(&name));
    if (entry != NULL) {
      Definition *def = &ctx->def_reg.defs->data[entry->index];
      ctx_error_at(ctx, &name, "'%.*s' is a %s definition and cannot be shadowed by a function.",
                   name.length, name.start, def_type_name(def->type));
    }
  }

  ctx_mark_initialized(ctx); // make function name available in its own body for recursion
  ctx_compile_function(ctx);
  ctx_define_variable(ctx, name_const);
}

static void ctx_register_definition(Context *ctx, Token *name, Definition def) {
  if (!defreg_add(&ctx->def_reg, def, ctx->al)) {
    ctx_error_at(ctx, name, "definition with this name already exists.");
  }
}

static void declaration_struct(Context *ctx) {
  ctx_consume(ctx, TOKEN_IDENTIFIER, "expect struct name.");
  Token name = ctx->parser.previous;

  if (ctx->current_compiler->scope_depth > 0) {
    ctx_error_at(ctx, &name, "struct definitions are only allowed at the top level.");
  }

  Definition def = {0};
  def.type = DEFTYPE_STRUCT;
  def.name = str_from_sv(sv_from_token(&name), ctx->al);
  StructDef *struct_def = &def.as.struct_def;
  struct_def->field_names = arr_new();

  ctx_consume(ctx, TOKEN_LEFT_BRACE, "expect '{' before struct body.");

  if (!ctx_check(ctx, TOKEN_RIGHT_BRACE)) {
    do {
      ctx_consume(ctx, TOKEN_IDENTIFIER, "expect field name in struct body.");
      Token field_name = ctx->parser.previous;
      ctx_consume(ctx, TOKEN_SEMICOLON, "expect ';' after struct field declaration.");
      String field_str = str_from_sv(sv_from_token(&field_name), ctx->al);
      arr_push(struct_def->field_names, field_str, ctx->al);
    } while (!ctx_check(ctx, TOKEN_RIGHT_BRACE) && !ctx_check(ctx, TOKEN_EOF));
  }

  ctx_consume(ctx, TOKEN_RIGHT_BRACE, "expect '}' after struct body.");
  ctx_register_definition(ctx, &name, def);
}

static void declaration(Context *ctx) {
  if (ctx_match(ctx, TOKEN_VAR)) {
    declaration_var(ctx);
  } else if (ctx_match(ctx, TOKEN_FUN)) {
    declaration_fun(ctx);
  } else if (ctx_match(ctx, TOKEN_STRUCT)) {
    declaration_struct(ctx);
  } else {
    statement(ctx);
  }

  if (ctx->parser.in_panic_mode) {
    ctx_synchronize(ctx);
  }
}

// ===== parse functions =====

static void grouping(Context *ctx, bool can_assign) {
  (void)can_assign;

  expression(ctx);
  ctx_consume(ctx, TOKEN_RIGHT_PAREN, "expect ')' after expression.");
}

static void call(Context *ctx, bool can_assign) {
  (void)can_assign;
  int arg_count = ctx_parse_argument_list(ctx);
  ctx_emit_bytes(ctx, OP_CALL, (uint8_t)arg_count);
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
  ctx_named_variable(ctx, ctx->parser.previous, can_assign);
}

static void and_(Context *ctx, bool can_assign) {
  (void)can_assign;

  int end_jump = ctx_emit_jump(ctx, OP_JUMP_IF_FALSE);
  ctx_emit_byte(ctx, OP_POP);
  parse_precedence(ctx, PREC_AND);
  ctx_patch_jump(ctx, end_jump);
}

static void or_(Context *ctx, bool can_assign) {
  (void)can_assign;

  int else_jump = ctx_emit_jump(ctx, OP_JUMP_IF_FALSE);
  int end_jump = ctx_emit_jump(ctx, OP_JUMP);

  ctx_patch_jump(ctx, else_jump);
  ctx_emit_byte(ctx, OP_POP);

  parse_precedence(ctx, PREC_OR);
  ctx_patch_jump(ctx, end_jump);
}

// ===== public api =====

bool compile(const char *source, const DefinitionArray *prior_defs, Allocator *al,
             Module *out_module) {
  Context ctx;
  ctx_init(&ctx, source, prior_defs, al);

  Compiler root_compiler;
  ctx_begin_compile(&ctx, &root_compiler);

  ctx_advance(&ctx);

  while (!ctx_check(&ctx, TOKEN_EOF)) {
    declaration(&ctx);
  }

  Proto *proto = ctx_end_compile(&ctx);
  if (ctx.parser.had_error) {
    proto_destroy(proto, al);
    al_free_for(al, proto);
    ctx_destroy(&ctx);
    return false;
  }

  if (out_module != NULL) {
    out_module->proto = proto;
    out_module->def_reg = ctx.def_reg; // moved
    ctx.def_reg = (DefinitionRegistry){0};
  } else {
    proto_destroy(proto, al);
    al_free_for(al, proto);
  }

  ctx_destroy(&ctx);

  return true;
}