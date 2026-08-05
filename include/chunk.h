#pragma once

#include "allocator.h"
#include "value.h"

#include <stdint.h>

// bytecode operations. operands are listed after each op.
//
// An operand is one byte when it indexes a *frame*, two when it indexes a
// table the *program* grows. That line is structural rather than a budget:
// the VM reserves STACK_HEADROOM (256) slots for one frame, so a u8 local or
// upvalue index *is* the frame size — widening it would address stack that
// cannot exist. A global, struct, enum, vtable or constant index has no such
// backing bound; it grows with the program, so it gets two bytes (u16, big-
// endian, like the jump offsets). An index bounded by one *declaration* — a
// field, a variant tag, a vtable method — stays one byte, since the parser
// already caps and diagnoses those counts.
typedef enum {
  OP_CONST, // u16 constant index
  OP_UNIT,
  OP_TRUE,
  OP_FALSE,
  OP_POP,
  OP_POPN,  // u8 count — pop n values
  OP_SLIDE, // u8 count — remove n values beneath the top value

  OP_GET_LOCAL,  // u8 frame slot
  OP_SET_LOCAL,  // u8 frame slot (value stays on the stack)
  OP_GET_GLOBAL, // u16 program function slot

  OP_CLOSURE,       // u16 const index (a VAL_FUN), u8 upvalue count, then that
                    // many (u8 is_local, u8 index) pairs — builds an ObjClosure
  OP_GET_UPVALUE,   // u8 upvalue index — push the captured variable's value
  OP_SET_UPVALUE,   // u8 upvalue index (value stays on the stack)
  OP_CLOSE_UPVALUE, // u8 frame slot — close every open upvalue at/above it

  OP_ADD,
  OP_SUB,
  OP_MUL,
  OP_DIV,
  OP_MOD,
  OP_NEG,
  OP_NOT,

  // int-only, and the reason the language has them at all: nothing above this
  // tier can express a bit. `OP_SHR` propagates the sign, `OP_USHR` shifts
  // zeros in; both refuse a count outside 0..63 rather than inheriting C's
  // undefined behaviour for one.
  OP_BIT_AND,
  OP_BIT_OR,
  OP_BIT_XOR,
  OP_BIT_NOT,
  OP_SHL,
  OP_SHR,
  OP_USHR,

  OP_EQ,
  OP_NEQ,
  OP_LT,
  OP_LTEQ,
  OP_GT,
  OP_GTEQ,

  OP_CAST_INT,   // float -> int (truncating); int is a no-op
  OP_CAST_FLOAT, // int -> float; float is a no-op

  OP_RANGE,       // u8 inclusive — pops end, start; pushes start..end
  OP_RANGE_START, // pops range; pushes start
  OP_RANGE_TEST,  // pops i, range; pushes i < end (or <= when inclusive)
  OP_RANGE_STOP,  // pops range; pushes the first int past the end — `end`, or
                  // `end + 1` when inclusive. This is where `a..b` and `a..=b`
                  // stop being two things: a half-open bound describes both,
                  // so anything holding one (`std::iter`'s RangeIter) needs no
                  // flag of its own. Saturates at INT64_MAX, so `..=` that far
                  // loses its last element rather than overflowing.

  OP_JUMP,          // u16 forward offset
  OP_JUMP_IF_FALSE, // u16 forward offset (does not pop the condition)
  OP_LOOP,          // u16 backward offset

  OP_CALL, // u8 argc — callee value sits beneath the args; the callee may be
           // a ducktape function (opens a frame) or a native (runs C and
           // pushes its result, no frame)
  OP_RETURN,

  OP_ARRAY,     // u8 count — pop count elems (left-to-right), push array
  OP_INDEX_GET, // pops index, array; pushes array[index] (bounds-checked)
  OP_INDEX_SET, // pops value, index, array; sets array[index] = value,
                // pushes value (bounds-checked)
  OP_LEN,       // pops array; pushes its length as int
  OP_INTERP,    // u8 seg count — pop count values, stringify + concat,
                // push the resulting string
  OP_DUP,       // u8 depth — push a copy of the value `depth` below the top

  OP_TUPLE,      // u8 count — pop count elems (left-to-right), push a tuple
  OP_STRUCT,     // u16 program struct slot — pop that struct's field_count
                 // elems (declaration order), push a struct instance
  OP_ENUM,       // u16 program enum slot, u8 variant tag — pop that variant's
                 // field_count elems (declaration order), push an instance.
                 // The tag stays one byte: it is an index within one enum's
                 // declaration, which the parser caps well below 256.
  OP_FIELD_GET,  // u8 index — pops a tuple/struct/enum instance, pushes its
                 // index-th field (no bounds check: indices are static)
  OP_FIELD_SET,  // u8 index — pops value then a tuple/struct/enum instance,
                 // writes the index-th field, pushes the value (assignment is
                 // an expression); the mirror of OP_FIELD_GET
  OP_MAKE_DYN,   // u16 vtable slot — pops a value, pushes it wrapped as a
                 // trait object carrying that vtable
  OP_DYN_METHOD, // u8 vtable method index — pops a trait object, pushes its
                 // method's function *then* the unwrapped receiver, so the
                 // ordinary OP_CALL below sees a normal callee-beneath-args
                 // stack and needs no dynamic-dispatch case of its own
  OP_DYN_IS,     // u16 vtable slot — peeks a trait object, pushes whether it
                 // carries *that* table. A vtable is memoised per (trait,
                 // concrete type), so one table is one type: this is the
                 // whole of `as?`'s runtime test, and the reason the VM needs
                 // no type tags to answer it. Peeks rather than pops so the
                 // value survives for OP_DYN_INNER on the matching branch.
  OP_DYN_INNER,  // pops a trait object, pushes the value inside it. Only ever
                 // emitted where an OP_DYN_IS just proved which type that is
  OP_DYN_UPCAST, // u16 upcast pair — pops a `dyn Sub`, pushes the same inner
                 // value carrying the `dyn Super` table. The table is found
                 // on the one the value already holds (VTable.upcasts): the
                 // site knows both traits and no concrete type, the table
                 // knows the concrete type and both traits
  OP_TAG,        // pops an enum instance, pushes its variant tag as int
  OP_MATCH_FAIL, // no arm matched a subject not statically proven exhaustive
                 // (checker doesn't enforce exhaustiveness yet); runtime error
} OpCode;

typedef struct Chunk {
  uint8_t *code;
  int count;
  int cap;

  Value *consts;
  int const_count;
  int const_cap;

  Allocator *al;
} Chunk;

// the widest value a u16 operand addresses, and so the size of every table
// one indexes: constants in a chunk, and the program-wide slot spaces in
// `Executable`. Slots run 0..SLOT_MAX-1.
#define SLOT_MAX 65536

void chunk_init(Chunk *c, Allocator *al);
void chunk_write(Chunk *c, uint8_t byte);
// big-endian, matching the jump offsets.
void chunk_write_u16(Chunk *c, uint16_t v);
// returns the constant's index, or -1 if the pool has outgrown the operand.
// The caller reports it: a chunk has no diagnostics of its own, and the limit
// is worth a message rather than the assert this used to be.
int chunk_add_const(Chunk *c, Value v);
