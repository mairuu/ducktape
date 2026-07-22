# ducktape — VS Code syntax highlighting

Basic TextMate grammar for `.dt` files: comments, strings (with `{}`
interpolation and escapes), character literals, numbers, keywords, declaration
names, type names, call names, operators. No language server, no semantic
analysis — the compiler is still the only thing that understands the program.

A character literal is matched *before* a string, which is not a preference but
a requirement: `'"'` is a legal literal, so leaving the inner quote to the
string rule opens a string that runs to the next quote in the file. A literal
that is empty, holds more than one character, or uses an unknown escape is
scoped `invalid.illegal`, mirroring what the scanner rejects.

## Install (local)

Symlink or copy this directory into your extensions folder and reload VS Code:

```sh
ln -s "$PWD/editors/vscode" ~/.vscode/extensions/ducktape
```

## Keeping it in sync

The keyword list mirrors the keyword tables in `src/scanner.c`; the grammar
otherwise follows `references/grammar.ebnf` — in particular the `CHAR` and
`FLOAT` productions, whose escape set and exponent form the grammar reproduces.
Adding a keyword to the scanner, or a literal form to the grammar, means adding
it here too.
