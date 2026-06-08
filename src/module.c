#include "module.h"
#include "allocator.h"
#include "diag.h"
#include "parser.h"
#include "scanner.h"
#include "string_utils.h"

#include <assert.h>
#include <stdio.h>

static String read_file(const char *path, Allocator *al);

bool mod_parse(Module *m, DiagBag *diags, Allocator *al) {
  m->source = read_file(m->file_path.chars, al);
  if (m->source.chars == NULL) {
    return false;
  }

  diag_clear(diags);
  Scanner scanner;
  scanner_init(&scanner, m->source.chars, diags);

  Token *tokens = NULL;
  int token_count = scanner_tokenise_all(&scanner, &tokens, al);
  assert(tokens != NULL);

  if (diag_has_errors(diags)) {
    return false;
  }

  Parser parser;
  parser_init(&parser, tokens, token_count, al, diags);
  m->ast = parser_parse(&parser);
  assert(m->ast != NULL);

  if (diag_has_errors(diags)) {
    return false;
  }

  return true;
}

static String read_file(const char *path, Allocator *al) {
  FILE *file = fopen(path, "rb");
  if (file == NULL) {
    fprintf(stderr, "could not open file '%s'.\n", path);
    return str_null();
  }

  fseek(file, 0L, SEEK_END);
  size_t file_size = ftell(file);
  rewind(file);

  char *buffer = al_alloc(al, sizeof(char) * (file_size + 1));
  if (buffer == NULL) {
    fprintf(stderr, "not enough memory to read '%s'.\n", path);
    return str_null();
  }

  size_t bytes_read = fread(buffer, sizeof(char), file_size, file);
  if (bytes_read < file_size) {
    fprintf(stderr, "could not read file '%s'.\n", path);
    al_free(al, buffer, sizeof(char) * (file_size + 1));
    return str_null();
  }

  buffer[bytes_read] = '\0';
  fclose(file);
  return str_create(buffer, (int)bytes_read, (int)file_size + 1);
}
