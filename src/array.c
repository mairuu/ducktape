#include "array.h"
#include "allocator.h"
#include <string.h>

void _arr_free(ArrayHeader **arr, size_t element_size, Allocator *al) {
  if (!*arr) {
    return;
  }

  size_t total_size = sizeof(ArrayHeader) + element_size * (*arr)->capacity;
  al_free(al, *arr, total_size);
  *arr = NULL;
}

bool _arr_resize(ArrayHeader **arr, size_t element_size, int new_capacity, Allocator *al) {
  size_t new_size = sizeof(ArrayHeader) + element_size * new_capacity;
  if (!*arr) {
    *arr = al_alloc(al, new_size);
    if (!*arr) {
      return false;
    }
    (*arr)->count = 0;
    (*arr)->capacity = new_capacity;
    return true;
  }
  size_t old_size = sizeof(ArrayHeader) + element_size * (*arr)->capacity;
  *arr = al_realloc(al, *arr, old_size, new_size);
  if (!*arr) {
    return false;
  }
  (*arr)->capacity = new_capacity;
  return true;
}

bool _arr_reserve(ArrayHeader **arr, size_t element_size, int expected_capacity, Allocator *al) {
  if (*arr && (*arr)->capacity >= expected_capacity) {
    return true;
  }
  int new_capacity = (*arr) ? (*arr)->capacity : ARRAY_INITIAL_CAPACITY;
  while (new_capacity < expected_capacity) {
    new_capacity *= ARRAY_GROWTH_FACTOR;
  }
  return _arr_resize(arr, element_size, new_capacity, al);
}

void *_arr_copy(const ArrayHeader *arr, size_t element_size, Allocator *al) {
  if (!arr) {
    return NULL;
  }
  size_t total_size = sizeof(ArrayHeader) + element_size * arr->capacity;
  ArrayHeader *new_arr = al_alloc(al, total_size);
  if (!new_arr) {
    return NULL;
  }
  new_arr->count = arr->count;
  new_arr->capacity = arr->capacity;
  memcpy((void *)(new_arr + 1), (const void *)(arr + 1), element_size * arr->count);
  return new_arr;
}