#pragma once

#include "allocator.h"
#include "diag.h"
#include "string_utils.h"

typedef enum {
  TOKEN_NONE = 0, // sentinel

  // Literals
  TOKEN_INT,
  TOKEN_FLOAT,
  TOKEN_STRING,
  TOKEN_CHAR,
  TOKEN_INTERPOLATION,
  TOKEN_TRUE,
  TOKEN_FALSE,

  // Identifiers / keywords
  TOKEN_IDENT,
  TOKEN_AND,
  TOKEN_AS,
  TOKEN_BREAK,
  TOKEN_CONTINUE,
  TOKEN_DYN,
  TOKEN_ELSE,
  TOKEN_ENUM,
  TOKEN_FOR,
  TOKEN_FUN,
  TOKEN_IF,
  TOKEN_IMPL,
  TOKEN_IN,
  TOKEN_LOOP,
  TOKEN_MATCH,
  TOKEN_NOT,
  TOKEN_OR,
  TOKEN_PUB,
  TOKEN_RETURN,
  TOKEN_SELF,
  TOKEN_SELF_TYPE, // self / Self
  TOKEN_STRUCT,
  TOKEN_TRAIT,
  TOKEN_TRUE_KW,
  TOKEN_FALSE_KW,
  TOKEN_TYPE,
  TOKEN_USE,
  TOKEN_VAR,
  TOKEN_WHERE,
  TOKEN_WHILE,

  TOKEN_LPAREN,   // (
  TOKEN_RPAREN,   // )
  TOKEN_LBRACE,   // {
  TOKEN_RBRACE,   // {
  TOKEN_LBRACKET, // [
  TOKEN_RBRACKET, // [
  TOKEN_COMMA,
  TOKEN_SEMICOLON,
  TOKEN_COLON,
  TOKEN_DOT,
  TOKEN_COLONCOLON, // ::
  TOKEN_THIN_ARROW, // ->
  TOKEN_FAT_ARROW,  // =>
  TOKEN_DOTDOT,
  TOKEN_DOTDOTEQ, // ..  ..=
  TOKEN_UNDER,    // _

  TOKEN_PLUS,
  TOKEN_MINUS,
  TOKEN_STAR,
  TOKEN_SLASH,
  TOKEN_PERCENT,
  TOKEN_EQ,
  TOKEN_PLUSEQ,
  TOKEN_MINUSEQ,
  TOKEN_STAREQ,
  TOKEN_SLASHEQ,
  TOKEN_PERCENTEQ,
  TOKEN_EQEQ,
  TOKEN_BANGEQ,
  TOKEN_LT,
  TOKEN_LTEQ,
  TOKEN_GT,
  TOKEN_GTEQ,
  TOKEN_QUESTION, // ?
  TOKEN_PIPE,     // |  (closure parameter delimiter, and bitwise or)
  TOKEN_AT,       // @  (attribute introducer)
  TOKEN_CARET,    // ^  (bitwise xor; centre alignment in a format spec)
  TOKEN_AMP,      // &  (bitwise and)
  TOKEN_TILDE,    // ~  (bitwise not)

  // The shifts are **synthesised by the parser and never produced by the
  // scanner**. `>` has to stay its own token because a nested generic closes
  // with a run of them (`Map<K, Vec<V>>`), and a scanner that fused `>>` would
  // break every such type. So the scanner emits single angle brackets and
  // `parse_shift` fuses adjacent ones, checking that no whitespace separates
  // them; these three names exist only to label the resulting AST node.
  TOKEN_SHL,  // <<
  TOKEN_SHR,  // >>   arithmetic: the sign bit is propagated
  TOKEN_USHR, // >>>  logical: zeros are shifted in

  // control
  TOKEN_EOF,
  TOKEN_ERROR, // unrecognised character; message stored in a side-buffer
} TokenType;

typedef struct {
  TokenType type;
  StringView lexeme;
  int line;
  int col;
} Token;

const char *token_type_to_string(TokenType type);

typedef struct {
  const char *source;  // full source string (NUL-terminated)
  const char *start;   // start of the current lexeme
  const char *current; // one past the last consumed character
  int line;
  int col; // column of `current`
  int brace_depth;
  int interp_braces[8];
  int interp_depth;
  DiagBag *diags;
} Scanner;

void scanner_init(Scanner *s, const char *source, DiagBag *diags);

Token scanner_next_token(Scanner *s);

// The value of a TOKEN_CHAR, decoded from its lexeme (quotes included).
// Returns NULL on success, or the message describing what is wrong with the
// literal — which the scanner reports and the parser can then assume away,
// so the syntax of a character literal is known in exactly one place.
const char *char_literal_value(StringView lexeme, uint32_t *out);

int scanner_tokenise_all(Scanner *s, Token **out_tokens, Allocator *al);