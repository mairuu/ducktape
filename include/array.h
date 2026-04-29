#pragma once

#include "allocator.h"

#include <stddef.h>
#include <stdint.h>

typedef struct {
  int count;
  int capacity;
} ArrayHeader;

#define DEFINE_ARRAY(T, Name)                                                                      \
  typedef struct {                                                                                 \
    ArrayHeader header;                                                                            \
    T data[];                                                                                      \
  } Name

DEFINE_ARRAY(int, IntArray);
DEFINE_ARRAY(uint8_t, ByteArray);

#define ARRAY_INITIAL_CAPACITY 4

#define ARRAY_GROWTH_FACTOR 2

#define arr_elm_type(arr) typeof(arr->data[0])

#define arr_new() NULL

// #define arr_init

#define arr_free(arr, al) _arr_free((ArrayHeader **)arr, sizeof(arr_elm_type(*arr)), al)

// #define arr_destroy

// reserve at least expected_capacity elements
#define arr_reserve(arr, expected_capacity, al)                                                    \
  _arr_reserve((ArrayHeader **)arr, sizeof(arr_elm_type(*arr)), expected_capacity, al)

#define arr_copy(arr, al) _arr_copy((const ArrayHeader *)arr, sizeof(arr_elm_type(arr)), al)

#define arr_count(arr) ((arr) ? (arr)->header.count : 0)

#define arr_capacity(arr) ((arr) ? (arr)->header.capacity : 0)

#define arr_push(arr, elm, al)                                                                     \
  do {                                                                                             \
    if (!arr || arr->header.count >= arr->header.capacity) {                                       \
      size_t new_capacity =                                                                        \
          arr ? arr->header.capacity * ARRAY_GROWTH_FACTOR : ARRAY_INITIAL_CAPACITY;               \
      if (!_arr_resize((ArrayHeader **)&arr, sizeof(arr_elm_type(arr)), new_capacity, al)) {       \
        break;                                                                                     \
      }                                                                                            \
    }                                                                                              \
    arr->data[arr->header.count++] = elm;                                                          \
  } while (0)

// caller must ensure arr is not empty and arr->header.count > 0
#define arr_pop(arr) (arr)->data[--(arr)->header.count]

void _arr_free(ArrayHeader **arr, size_t element_size, Allocator *al);

bool _arr_resize(ArrayHeader **arr, size_t element_size, int new_capacity, Allocator *al);

bool _arr_reserve(ArrayHeader **arr, size_t element_size, int expected_capacity, Allocator *al);

void *_arr_copy(const ArrayHeader *arr, size_t element_size, Allocator *al);