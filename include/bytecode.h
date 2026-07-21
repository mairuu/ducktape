#pragma once

#include "allocator.h"
#include "ast.h"
#include "object.h"

#include <stdbool.h>

// on-disk form of a linked `Executable`. The image is the *runtime* projection
// of the program: only what the VM and `value_print` actually read off a
// FunDef/StructDef/EnumDef survives (names, arities, field shapes, chunks).
// Types, spans and modules are compile-time concerns and are dropped, so a
// loaded image has no AST behind it.
//
// Every multi-byte field is little-endian and written explicitly, so an image
// does not depend on the host's byte order or struct layout.

#define BC_MAGIC "DTBC"
#define BC_VERSION 1

// serialize the linked program to `path`. `entry` is the function `--run`
// would have started at; its slot is recorded in the header. `exe`'s chunks
// must already be compiled.
bool bc_write(const Executable *exe, const FunDef *entry, const char *path,
              Allocator *al);

// load an image written by `bc_write`, filling `exe`, initialising `heap`
// against it, and reporting the entry function.
//
// heap initialisation belongs here rather than to the caller because of the
// same ordering constraint linking has (`runtime.md` "Linking"): decoding a
// chunk interns its string constants, so the heap must already be rooted off
// the fully-sized tables before the first constant pool is read.
//
// every structural error is reported to stderr and returns false; a truncated
// or corrupt image must never be executed.
bool bc_load(const char *path, Allocator *al, Executable *exe, Heap *heap,
             bool gc_stress, FunDef **entry_out);

// does `path` start with the image magic? lets `--run` take either a source
// file or an image without a separate flag.
bool bc_is_image(const char *path);
