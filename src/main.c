#include "allocator.h"
#include "arena.h"
#include "ast.h"
#include "error_reporter.h"
#include "parser.h"
#include "scanner.h"
#include "string_utils.h"
#include "type_checker.h"

#include <stdio.h>
#include <stdlib.h>

static String read_file(const char *path, Allocator *al);

static ErrorReporter reporter;

int main(int argc, char *argv[]) {
  Allocator heap_al = heap_allocator_create();

  Arena arena;
  arena_init(&arena, &heap_al);
  Allocator arena_al = arena_allocator_create(&arena);

  if (argc < 2) {
    fprintf(stderr, "usage: %s <file>\n", argv[0]);
    return 1;
  }

  String source = read_file(argv[1], &arena_al);
  if (source.chars == NULL) {
    return 1;
  }

  reporter_init(&reporter, argv[1], source.chars);

  Scanner scanner;
  scanner_init(&scanner, source.chars, &reporter);

  Token *tokens = NULL;
  int token_count = scanner_tokenise_all(&scanner, &tokens, &arena_al);

  // for (int i = 0; i < token_count; i++) {
  //   Token *t = &tokens[i];
  //   fprintf(stdout, "%03d:%-2d %s '%.*s'\n", t->line, t->col,
  //           token_type_to_string(t->type), t->lexeme.len, t->lexeme.chars);
  // }

  if (reporter.had_error) {
    reporter_print_all(&reporter, stdout);
    str_destroy(&source, &arena_al);
    return 1;
  }

  Parser parser;
  parser_init(&parser, tokens, token_count, &heap_al, &reporter);

  Program *program = parser_parse(&parser);
  if (reporter.had_error) {
    reporter_print_all(&reporter, stdout);
    return 1;
  }

  TypeChecker tc;
  tc_init(&tc, &reporter, &arena_al);
  tc_check_program(&tc, program);
  tc_destroy(&tc);

  if (reporter.had_error) {
    reporter_print_all(&reporter, stdout);
    return 1;
  }

  dump_program(program, 0);

  arena_reset(&arena);

  return 0;
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
