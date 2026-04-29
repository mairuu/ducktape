#pragma once

typedef struct {
  const char *start;
  const char *current;
  int line;
} Scanner;

typedef enum {
  TOKEN_APOSTROPHE,
  TOKEN_LEFT_PAREN,
  TOKEN_RIGHT_PAREN,
  TOKEN_LEFT_BRACE,
  TOKEN_RIGHT_BRACE,
  TOKEN_COLON,
  TOKEN_COMMA,
  TOKEN_DOT,
  TOKEN_MINUS,
  TOKEN_MINUS_MINUS,
  TOKEN_MINUS_EQUAL,
  TOKEN_PLUS,
  TOKEN_PLUS_PLUS,
  TOKEN_PLUS_EQUAL,
  TOKEN_QUESTION,
  TOKEN_SEMICOLON,
  TOKEN_SLASH,
  TOKEN_SLASH_EQUAL,
  TOKEN_STAR,
  TOKEN_STAR_EQUAL,

  TOKEN_BANG,
  TOKEN_BANG_EQUAL,
  TOKEN_EQUAL,
  TOKEN_EQUAL_EQUAL,
  TOKEN_GREATER,
  TOKEN_GREATER_EQUAL,
  TOKEN_LESS,
  TOKEN_LESS_EQUAL,
  TOKEN_FAT_ARROW, // =>

  TOKEN_IDENTIFIER,
  TOKEN_STRING,
  TOKEN_NUMBER,

  TOKEN_AND,
  TOKEN_AS,
  TOKEN_BREAK,
  TOKEN_CONTINUE,
  TOKEN_ELSE,
  TOKEN_FALSE,
  TOKEN_FOR,
  TOKEN_FUN,
  TOKEN_IF,
  TOKEN_IN,
  TOKEN_IS,
  TOKEN_IMPL,
  TOKEN_MATCH,
  TOKEN_NIL,
  TOKEN_OR,
  TOKEN_RETURN,
  TOKEN_STRUCT,
  TOKEN_THIS,
  TOKEN_TRAIT,
  TOKEN_TRUE,
  TOKEN_VAR,
  TOKEN_VARIANT,

  TOKEN_ERROR,
  TOKEN_EOF
} TokenType;

typedef struct {
  const char *source; // pointer to the start of the token in the source,
                      // used for error reporting
  const char *start;
  TokenType type;
  int length;
  int line_number;
} Token;

void scanner_init(Scanner *scanner, const char *source);

Token scanner_next_token(Scanner *scanner);