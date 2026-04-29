#pragma once

#include "allocator.h"
#include "value.h"

#define HASHTABLE_INITIAL_CAPACITY 8

typedef struct {
  Value key;
  Value value;
} Entry;

typedef struct {
  int count;
  int alive;
  int capacity;
  Entry *entries;
} HashTable;

void ht_init(HashTable *table);

void ht_destroy(HashTable *table, Allocator *al);

Value *ht_get(HashTable *table, Value key);

bool ht_put(HashTable *table, Value key, Value value, Allocator *al);

void ht_delete(HashTable *table, Value key);

typedef struct {
  Value *key;
  Value *value;
  HashTable *_table;
  int _index;
} HashTableIterator;

void hti_init(HashTableIterator *it, HashTable *table);

bool hti_next(HashTableIterator *it);
