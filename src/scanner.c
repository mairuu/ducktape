// ═══════════════════════════════════════════════════════════════════════════════
// scanner.c
// ═══════════════════════════════════════════════════════════════════════════════
#include "scanner.h"
#include "allocator.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static const char *token_type_string[] = {
    [TOKEN_NONE] = "NONE",

    // Literals
    [TOKEN_INT] = "INT",
    [TOKEN_FLOAT] = "FLOAT",
    [TOKEN_STRING] = "STRING",
    [TOKEN_INTERPOLATION] = "INTERPOLATION",
    [TOKEN_TRUE] = "TRUE",
    [TOKEN_FALSE] = "FALSE",

    // Identifiers / keywords
    [TOKEN_IDENT] = "IDENT",
    [TOKEN_AND] = "AND",
    [TOKEN_AS] = "AS",
    [TOKEN_BREAK] = "BREAK",
    [TOKEN_CONTINUE] = "CONTINUE",
    [TOKEN_ELSE] = "ELSE",
    [TOKEN_ENUM] = "ENUM",
    [TOKEN_FOR] = "FOR",
    [TOKEN_FUN] = "FUN",
    [TOKEN_IF] = "IF",
    [TOKEN_IMPL] = "IMPL",
    [TOKEN_IN] = "IN",
    [TOKEN_MATCH] = "MATCH",
    [TOKEN_NOT] = "NOT",
    [TOKEN_OR] = "OR",
    [TOKEN_PUB] = "PUB",
    [TOKEN_RETURN] = "RETURN",
    [TOKEN_SELF] = "SELF",
    [TOKEN_SELF_TYPE] = "SELF_TYPE",
    [TOKEN_STRUCT] = "STRUCT",
    [TOKEN_TRAIT] = "TRAIT",
    [TOKEN_TRUE_KW] = "TRUE_KW",
    [TOKEN_FALSE_KW] = "FALSE_KW",
    [TOKEN_TYPE] = "TYPE",
    [TOKEN_USE] = "USE",
    [TOKEN_VAR] = "VAR",

    [TOKEN_LPAREN] = "LPAREN",
    [TOKEN_RPAREN] = "RPAREN",
    [TOKEN_LBRACE] = "LBRACE",
    [TOKEN_RBRACE] = "RBRACE",
    [TOKEN_LBRACKET] = "LBRACKET",
    [TOKEN_RBRACKET] = "RBRACKET",
    [TOKEN_COMMA] = "COMMA",
    [TOKEN_SEMICOLON] = "SEMICOLON",
    [TOKEN_COLON] = "COLON",
    [TOKEN_DOT] = "DOT",
    [TOKEN_COLONCOLON] = "COLONCOLON", // ::
    [TOKEN_ARROW] = "ARROW",           // =>
    [TOKEN_DOTDOT] = "DOTDOT",
    [TOKEN_DOTDOTEQ] = "DOTDOTEQ", // ..  ..=
    [TOKEN_UNDER] = "UNDER",     // _

    [TOKEN_PLUS] = "PLUS",
    [TOKEN_MINUS] = "MINUS",
    [TOKEN_STAR] = "STAR",
    [TOKEN_SLASH] = "SLASH",
    [TOKEN_PERCENT] = "PERCENT",
    [TOKEN_EQ] = "EQ",
    [TOKEN_PLUSEQ] = "PLUSEQ",
    [TOKEN_MINUSEQ] = "MINUSEQ",
    [TOKEN_STAREQ] = "STAREQ",
    [TOKEN_SLASHEQ] = "SLASHEQ",
    [TOKEN_EQEQ] = "EQEQ",
    [TOKEN_BANGEQ] = "BANGEQ",
    [TOKEN_LT] = "LT",
    [TOKEN_LTEQ] = "LTEQ",
    [TOKEN_GT] = "GT",
    [TOKEN_GTEQ] = "GTEQ",
    [TOKEN_QUESTION] = "QUESTION", // ?

    // control
    [TOKEN_EOF] = "EOF",
    [TOKEN_ERROR] =
        "ERROR", // unrecognised character; message stored in a side-buffer
};

const char *token_type_to_string(TokenType type) {
  if (type < 0 ||
      type >= sizeof(token_type_string) / sizeof(token_type_string[0]))
    return "UNKNOWN";
  return token_type_string[type];
}

// ── Internal helpers ─────────────────────────────────────────────────────────

static bool is_at_end(const Scanner *s) { return *s->current == '\0'; }

static char peek(const Scanner *s) { return *s->current; }
static char peek_next(const Scanner *s) {
  return is_at_end(s) ? '\0' : s->current[1];
}

static char advance(Scanner *s) {
  char c = *s->current++;
  if (c == '\n') {
    s->line++;
    s->col = 1;
  } else {
    s->col++;
  }
  return c;
}

static bool match_char(Scanner *s, char expected) {
  if (is_at_end(s) || *s->current != expected)
    return false;
  advance(s);
  return true;
}

static Token make_token(const Scanner *s, TokenType type) {
  return (Token){
      .type = type,
      .lexeme =
          (StringView){.chars = s->start, .len = (int)(s->current - s->start)},
      .line = s->line,
      .col = s->col - (int)(s->current - s->start),
  };
}

static Token error_token(Scanner *s, const char *msg) {
  reporter_error(s->reporter, s->line, s->line, s->col, s->col, "%s", msg);
  return (Token){
      .type = TOKEN_ERROR,
      .lexeme = (StringView){.chars = msg, .len = (int)strlen(msg)},
      .line = s->line,
      .col = s->col,
  };
}

// ── Whitespace and comments ──────────────────────────────────────────────────

static void skip_whitespace(Scanner *s) {
  for (;;) {
    char c = peek(s);
    switch (c) {
    case ' ':
    case '\t':
    case '\r':
    case '\n':
      advance(s);
      break;
    case '#': // line comment
      while (!is_at_end(s) && peek(s) != '\n')
        advance(s);
      break;
    default:
      return;
    }
  }
}

// ── String scanning ──────────────────────────────────────────────────────────

static Token scan_string(Scanner *s) {
  while (!is_at_end(s) && peek(s) != '"') {
    if (peek(s) == '\\') {
      advance(s); // consume backslash
      char esc = advance(s);
      switch (esc) {
      case 'n':
      case 't':
      case 'r':
      case '"':
      case '\\':
      case '{':
        break;
      default:
        reporter_error(s->reporter, s->line, s->line, s->col - 1, s->col - 1,
                       "unknown escape", "");
      }
    } else if (peek(s) == '{') {
      advance(s); // Consume '{'

      if (s->interp_depth < 8) {
        s->interp_braces[s->interp_depth++] = s->brace_depth;
      } else {
        return error_token(s, "String interpolation is too deep.");
      }

      return make_token(s, TOKEN_INTERPOLATION);
    } else {
      advance(s);
    }
  }

  if (is_at_end(s))
    return error_token(s, "unterminated string");

  advance(s);
  return make_token(s, TOKEN_STRING);
}

// ── Number scanning ──────────────────────────────────────────────────────────

static Token scan_number(Scanner *s) {
  while (isdigit(peek(s)))
    advance(s);

  if (peek(s) == '.' && isdigit(peek_next(s))) {
    advance(s); // consume '.'
    while (isdigit(peek(s)))
      advance(s);
    return make_token(s, TOKEN_FLOAT);
  }
  return make_token(s, TOKEN_INT);
}

// ── Keyword table
// ─────────────────────────────────────────────────────────────

typedef struct {
  const char *word;
  TokenType type;
} Keyword;

// by length
static const Keyword kw2[] = { //
    {"as", TOKEN_AS},
    {"in", TOKEN_IN},
    {"if", TOKEN_IF},
    {"or", TOKEN_OR},
    {NULL, 0}};

static const Keyword kw3[] = { //
    {"and", TOKEN_AND}, {"for", TOKEN_FOR}, {"fun", TOKEN_FUN},
    {"not", TOKEN_NOT}, {"pub", TOKEN_PUB}, {"use", TOKEN_USE},
    {"var", TOKEN_VAR}, {NULL, 0}};

static const Keyword kw4[] = { //
    {"else", TOKEN_ELSE},      {"enum", TOKEN_ENUM},
    {"impl", TOKEN_IMPL},      {"self", TOKEN_SELF},
    {"Self", TOKEN_SELF_TYPE}, {"type", TOKEN_TYPE},
    {"true", TOKEN_TRUE},      {NULL, 0}};

static const Keyword kw5[] = { //
    {"break", TOKEN_BREAK},
    {"match", TOKEN_MATCH},
    {"trait", TOKEN_TRAIT},
    {"false", TOKEN_FALSE},
    {NULL, 0}};

static const Keyword kw6[] = { //
    {"return", TOKEN_RETURN},
    {"struct", TOKEN_STRUCT},
    {NULL, 0}};

static const Keyword kw8[] = { //
    {"continue", TOKEN_CONTINUE},
    {NULL, 0}};

static const Keyword *KEYWORDS[] = {NULL, NULL, kw2,  kw3, kw4,
                                    kw5,  kw6,  NULL, kw8};

static Token scan_identifier(Scanner *s) {
  while (isalnum(peek(s)) || peek(s) == '_')
    advance(s);

  int len = (int)(s->current - s->start);
  if (len < 2 || len > 8)
    return make_token(s, TOKEN_IDENT);

  const Keyword *kw_list = KEYWORDS[len];
  if (kw_list != NULL) {
    for (const Keyword *kw = kw_list; kw->word != NULL; kw++) {
      if (strncmp(s->start, kw->word, len) == 0)
        return make_token(s, kw->type);
    }
  }

  return make_token(s, TOKEN_IDENT);
}

void scanner_init(Scanner *s, const char *source, ErrorReporter *reporter) {
  s->source = source;
  s->start = source;
  s->current = source;
  s->line = 1;
  s->col = 1;
  s->reporter = reporter;
}

Token scanner_next_token(Scanner *s) {
  skip_whitespace(s);
  s->start = s->current;

  if (is_at_end(s))
    return make_token(s, TOKEN_EOF);

  char c = advance(s);

  if (isdigit(c))
    return scan_number(s);
  if (isalpha(c) || c == '_')
    return scan_identifier(s);
  if (c == '"')
    return scan_string(s);

  switch (c) {
  case '(':
    return make_token(s, TOKEN_LPAREN);
  case ')':
    return make_token(s, TOKEN_RPAREN);
  case '{':
    s->brace_depth++; 
    return make_token(s, TOKEN_LBRACE);

  case '}':
    if (s->interp_depth > 0 &&
        s->interp_braces[s->interp_depth - 1] == s->brace_depth) {

      s->interp_depth--;
      return scan_string(s);
    }
    s->brace_depth--;
    return make_token(s, TOKEN_RBRACE);
  case '[':
    return make_token(s, TOKEN_LBRACKET);
  case ']':
    return make_token(s, TOKEN_RBRACKET);
  case ',':
    return make_token(s, TOKEN_COMMA);
  case ';':
    return make_token(s, TOKEN_SEMICOLON);
  case '?':
    return make_token(s, TOKEN_QUESTION);
  case '%':
    return make_token(s, TOKEN_PERCENT);

  case ':':
    return make_token(s, match_char(s, ':') ? TOKEN_COLONCOLON : TOKEN_COLON);
  case '.':
    if (match_char(s, '.'))
      return make_token(s, match_char(s, '=') ? TOKEN_DOTDOTEQ : TOKEN_DOTDOT);
    return make_token(s, TOKEN_DOT);

  case '=':
    if (match_char(s, '>'))
      return make_token(s, TOKEN_ARROW);
    if (match_char(s, '='))
      return make_token(s, TOKEN_EQEQ);
    return make_token(s, TOKEN_EQ);
  case '_':
    return make_token(s, TOKEN_UNDER);

  case '!':
    return make_token(s, match_char(s, '=') ? TOKEN_BANGEQ : TOKEN_ERROR);
  case '<':
    return make_token(s, match_char(s, '=') ? TOKEN_LTEQ : TOKEN_LT);
  case '>':
    return make_token(s, match_char(s, '=') ? TOKEN_GTEQ : TOKEN_GT);
  case '+':
    return make_token(s, match_char(s, '=') ? TOKEN_PLUSEQ : TOKEN_PLUS);
  case '-':
    return make_token(s, match_char(s, '=') ? TOKEN_MINUSEQ : TOKEN_MINUS);
  case '*':
    return make_token(s, match_char(s, '=') ? TOKEN_STAREQ : TOKEN_STAR);
  case '/':
    return make_token(s, match_char(s, '=') ? TOKEN_SLASHEQ : TOKEN_SLASH);
  }

  return error_token(s, "unexpected character");
}

int scanner_tokenise_all(Scanner *s, Token **out_tokens, Allocator *al) {
  int cap = 256;
  Token *buf = malloc(sizeof(Token) * cap);
  int count = 0;

  Token t;
  do {
    t = scanner_next_token(s);
    if (count >= cap) {
      cap *= 2;
      buf = realloc(buf, sizeof(Token) * cap);
    }
    buf[count++] = t;
  } while (t.type != TOKEN_EOF);

  *out_tokens = al_alloc(al, sizeof(Token) * count);
  memcpy(*out_tokens, buf, sizeof(Token) * count);
  free(buf);
  return count;
}