#include "scanner.h"
#include <string.h>

void scanner_init(Scanner *scanner, const char *source) {
  scanner->start = source;
  scanner->current = source;
  scanner->line = 1;
}

static Token token_create(Scanner *scanner, TokenType type) {
  return (Token){.type = type,
                 .start = scanner->start,
                 .length = (int)(scanner->current - scanner->start),
                 .line_number = scanner->line,
                 .source = scanner->start};
}

static Token token_error(Scanner *scanner, const char *message) {
  return (Token){.type = TOKEN_ERROR,
                 .start = message,
                 .length = (int)strlen(message),
                 .line_number = scanner->line,
                 .source = scanner->start};
}

static bool is_at_end(Scanner *s) { return *s->current == '\0'; }

static char advance(Scanner *s) { return *s->current++; }

static char peek(Scanner *s) { return *s->current; }

static char peek_next(Scanner *scanner) {
  if (is_at_end(scanner)) {
    return '\0';
  }
  return scanner->current[1];
}

static char peek_prev(Scanner *scanner) {
  if (scanner->current == scanner->start) {
    return '\0';
  }
  return scanner->current[-1];
}

static bool match(Scanner *scanner, char expected) {
  if (!is_at_end(scanner) && *scanner->current == expected) {
    scanner->current++;
    return true;
  }
  return false;
}

static void skip_whitespace(Scanner *s) {
  while (true) {
    char c = *s->current;
    switch (c) {
    case ' ':
    case '\r':
    case '\t':
      advance(s);
      break;
    case '\n':
      s->line++;
      advance(s);
      break;
    case '/':
      if (peek_next(s) == '/') {
        // a comment goes until the end of the line.
        while (peek(s) != '\n' && !is_at_end(s)) {
          advance(s);
        }
      } else {
        return;
      }
      break;
    default:
      return;
    }
  }
}

static Token token_string(Scanner *s) {
  while ((peek(s) != '"' || peek_prev(s) == '\\') && !is_at_end(s)) {
    if (peek(s) == '\n') {
      s->line++;
    }
    advance(s);
  }

  if (is_at_end(s))
    return token_error(s, "unterminated string.");

  advance(s);
  return token_create(s, TOKEN_STRING);
}

static bool is_digit(char c) { return c >= '0' && c <= '9'; }

static Token token_number(Scanner *s) {
  while (is_digit(peek(s))) {
    advance(s);
  }

  if (peek(s) == '.' && is_digit(peek_next(s))) {
    advance(s);

    while (is_digit(peek(s))) {
      advance(s);
    }
  }

  return token_create(s, TOKEN_NUMBER);
}

static bool is_alphabet(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static TokenType check_keyword(Scanner *s, int start, int length, const char *rest,
                               TokenType type) {
  if (s->current - s->start == start + length && memcmp(s->start + start, rest, length) == 0) {
    return type;
  }
  return TOKEN_IDENTIFIER;
}

static TokenType identifier_type(Scanner *s) {
  switch (s->start[0]) {
  case 'a':
    if (s->current - s->start > 1) {
      switch (s->start[1]) {
      case 'n':
        return check_keyword(s, 2, 1, "d", TOKEN_AND);
      case 's':
        return check_keyword(s, 2, 0, "", TOKEN_AS);
      }
    }
    break;
  case 'b':
    return check_keyword(s, 1, 4, "reak", TOKEN_BREAK);
  case 'c':
    if (s->current - s->start > 1) {
      switch (s->start[1]) {
      // case 'l':
      //   return check_keyword(s, 2, 3, "ass", TOKEN_CLASS);
      case 'o':
        return check_keyword(s, 2, 6, "ntinue", TOKEN_CONTINUE);
      }
    }
    break;
  case 'e':
    return check_keyword(s, 1, 3, "lse", TOKEN_ELSE);
  case 'f':
    if (s->current - s->start > 1) {
      switch (s->start[1]) {
      case 'a':
        return check_keyword(s, 2, 3, "lse", TOKEN_FALSE);
      case 'o':
        return check_keyword(s, 2, 1, "r", TOKEN_FOR);
      case 'u':
        return check_keyword(s, 2, 1, "n", TOKEN_FUN);
      }
    }
    break;
  case 'i':
    if (s->current - s->start > 1) {
      switch (s->start[1]) {
      case 'f':
        return check_keyword(s, 2, 0, "", TOKEN_IF);
      case 'm':
        return check_keyword(s, 2, 2, "pl", TOKEN_IMPL);
      case 's':
        return check_keyword(s, 2, 0, "", TOKEN_IS);
      case 'n':
        return check_keyword(s, 2, 0, "", TOKEN_IN);
      }
    }
    break;
  case 'm':
    return check_keyword(s, 1, 4, "atch", TOKEN_MATCH);
  case 'n':
    return check_keyword(s, 1, 2, "il", TOKEN_NIL);
  case 'o':
    return check_keyword(s, 1, 1, "r", TOKEN_OR);
  // case 'p':
  //   return check_keyword(s, 1, 4, "rint", TOKEN_PRINT);
  case 'r':
    return check_keyword(s, 1, 5, "eturn", TOKEN_RETURN);
  // case 's':
  //   return check_keyword(s, 1, 4, "uper", TOKEN_SUPER);
  case 's':
    return check_keyword(s, 1, 5, "truct", TOKEN_STRUCT);
  case 't':
    if (s->current - s->start > 1) {
      switch (s->start[1]) {
      case 'h':
        return check_keyword(s, 2, 2, "is", TOKEN_THIS);
      case 'r': {
        if (s->current - s->start > 2) {
          switch (s->start[2]) {
          case 'a':
            return check_keyword(s, 3, 2, "it", TOKEN_TRAIT);
          case 'u':
            return check_keyword(s, 3, 1, "e", TOKEN_TRUE);
          }
        }
      }
      }
    }
    break;
  case 'v':
    if (s->current - s->start > 1) {
      if (s->start[1] == 'a') {
        if (s->current - s->start > 2) {
          if (s->start[2] == 'r') {
            // var...
            if (s->current - s->start > 3) {
              if (s->start[3] == 'i') {
                return check_keyword(s, 4, 3, "ant", TOKEN_VARIANT);
              }
            }
            return check_keyword(s, 3, 0, "", TOKEN_VAR);
          }
        }
      }
    }
  }
  return TOKEN_IDENTIFIER;
}

static Token token_identifier(Scanner *s) {
  while (is_alphabet(peek(s)) || is_digit(peek(s))) {
    advance(s);
  }

  return token_create(s, identifier_type(s));
}

Token scanner_next_token(Scanner *s) {
  skip_whitespace(s);

  s->start = s->current;

  if (is_at_end(s)) {
    return token_create(s, TOKEN_EOF);
  }

  char c = advance(s);

  if (is_alphabet(c)) {
    return token_identifier(s);
  }

  if (is_digit(c)) {
    return token_number(s);
  }

  switch (c) {
  case '\'':
    return token_create(s, TOKEN_APOSTROPHE);
  case '(':
    return token_create(s, TOKEN_LEFT_PAREN);
  case ')':
    return token_create(s, TOKEN_RIGHT_PAREN);
  case '{':
    return token_create(s, TOKEN_LEFT_BRACE);
  case '}':
    return token_create(s, TOKEN_RIGHT_BRACE);
  case ':':
    return token_create(s, TOKEN_COLON);
  case ';':
    return token_create(s, TOKEN_SEMICOLON);
  case ',':
    return token_create(s, TOKEN_COMMA);
  case '.':
    return token_create(s, TOKEN_DOT);
  case '-':
    if (match(s, '-')) {
      return token_create(s, TOKEN_MINUS_MINUS);
    }
    return token_create(s, TOKEN_MINUS);
  case '+':
    if (match(s, '+')) {
      return token_create(s, TOKEN_PLUS_PLUS);
    }
    return token_create(s, TOKEN_PLUS);
  case '/':
    return token_create(s, TOKEN_SLASH);
  case '*':
    return token_create(s, TOKEN_STAR);
  case '!':
    return token_create(s, match(s, '=') ? TOKEN_BANG_EQUAL : TOKEN_BANG);
  case '=':
    return token_create(s, match(s, '=')   ? TOKEN_EQUAL_EQUAL
                           : match(s, '>') ? TOKEN_FAT_ARROW
                                           : TOKEN_EQUAL);
  case '<':
    return token_create(s, match(s, '=') ? TOKEN_LESS_EQUAL : TOKEN_LESS);
  case '>':
    return token_create(s, match(s, '=') ? TOKEN_GREATER_EQUAL : TOKEN_GREATER);
  case '?':
    return token_create(s, TOKEN_QUESTION);
  case '"':
    return token_string(s);
  }

  return token_error(s, "unexpected character.");
}

Token scanner_peek_next_token(Scanner *s) {
  Scanner copy = *s;
  return scanner_next_token(&copy);
}
