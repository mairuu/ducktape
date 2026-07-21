# ducktape — VS Code syntax highlighting

Basic TextMate grammar for `.dt` files: comments, strings (with `{}`
interpolation and escapes), numbers, keywords, declaration names, type names,
call names, operators. No language server, no semantic analysis — the compiler
is still the only thing that understands the program.

## Install (local)

Symlink or copy this directory into your extensions folder and reload VS Code:

```sh
ln -s "$PWD/editors/vscode" ~/.vscode/extensions/ducktape
```

## Keeping it in sync

The keyword list mirrors the keyword tables in `src/scanner.c`; the grammar
otherwise follows `references/grammar.ebnf`. Adding a keyword to the scanner
means adding it to `syntaxes/ducktape.tmLanguage.json`.
