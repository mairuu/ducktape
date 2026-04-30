#include "hash_table.h"

#include <assert.h>
#include <limits.h>
#include <stdint.h>
#include <string.h>

#include "value.h"

void ht_init(HashTable *table) {
    *table = (HashTable){0};
}

void ht_destroy(HashTable *table, Allocator *al) {
  if (table->capacity > 0) {
    al_free(al, table->entries, sizeof(*(table->entries)) * table->capacity);
  }
  *table = (HashTable){0};
}

#define TOMBSTONE_VALUE (void *)-1

static inline bool is_tombstone(Entry *entry) {
  return IS_EMPTY(entry->key) && AS_OBJECT(entry->value) == TOMBSTONE_VALUE;
}

static inline void mark_tombstone(Entry *entry) {
  entry->key = EMPTY_VAL;
  entry->value = OBJECT_VAL(TOMBSTONE_VALUE);
}

static inline bool is_empty(Entry *entry) {
  return IS_EMPTY(entry->key) && IS_NIL(entry->value);
}

static inline void mark_empty(Entry *entry) {
  entry->key = EMPTY_VAL;
  entry->value = NIL_VAL;
}

static Entry *get(HashTable *table, Value key) {
  assert(table != NULL);
  uint32_t hash = val_hash(key);

  for (int i = 0; i < table->capacity; i++) {
    // uint32_t index = (hash + i) % table->capacity;
    uint32_t index = (hash + i) & (table->capacity - 1);
    Entry *entry = &table->entries[index];

    if (is_empty(entry)) {
      return NULL; // found empty slot, key not present
    }

    if (val_equal(entry->key, key)) {
      return entry; // found exact match
    }

    // it's either a tombstone or a different key, keep probing
  }

  return NULL;
}

Value *ht_get(HashTable *table, Value key) {
  assert(table != NULL);
  Entry *entry = get(table, key);
  if (entry == NULL) {
    return NULL;
  }
  return &entry->value;
}

#define HIGH_WATER_MARK 0.75
#define LOW_WATER_MARK 0.5

static void _ht_alloc(HashTable *table, int capacity, Allocator *al) {
  table->count = 0;
  table->entries = al_alloc(al, sizeof(Entry) * capacity);
  table->capacity = capacity;
  // fill new entries with empty values
  for (int i = 0; i < capacity; i++) {
    mark_empty(&table->entries[i]);
  }
}

static void _ht_grow_table(HashTable *table, Allocator *al) {
  int key_count = 0;
  for (int i = 0; i < table->capacity; i++) {
    Entry *entry = &table->entries[i];
    if (!is_empty(entry) && !is_tombstone(entry)) {
      key_count++;
    }
  }

  int new_capacity = table->capacity;
  while (key_count >= new_capacity * LOW_WATER_MARK) {
    new_capacity *= 2;
  }
  assert(new_capacity > 0);

  HashTable new_table;
  _ht_alloc(&new_table, new_capacity, al);

  for (int i = 0; i < table->capacity; i++) {
    Entry *entry = &table->entries[i];
    if (is_empty(entry) || is_tombstone(entry)) {
      continue;
    }
    ht_put(&new_table, entry->key, entry->value, al);
  }
  ht_destroy(table, al);

  assert(new_table.count == key_count);
  *table = new_table;
}

static Entry *get_or_insert(HashTable *table, Value key, Allocator *al) {
  if (!table->entries) {
    _ht_alloc(table, HASHTABLE_INITIAL_CAPACITY, al);
  } else if (table->count >= table->capacity * HIGH_WATER_MARK) {
    _ht_grow_table(table, al);
  }

  uint32_t hash = val_hash(key);

  for (int i = 0; i < table->capacity; i++) {
    uint32_t index = (hash + i) & (table->capacity - 1);
    Entry *entry = &table->entries[index];

    if (!is_empty(entry) && val_equal(entry->key, key)) {
      return entry; // found exact match
    }

    if (is_tombstone(entry)) {
      table->alive++; // resurrecting
      entry->key = key;
      return entry; // reuse tombstone slot
    }

    if (is_empty(entry)) {
      table->count++;
      table->alive++;
      entry->key = key;
      return entry; // found empty slot
    }
  }

  assert(false && "hash table is full, should have been resized");
  return NULL; // unreachable.
}

bool ht_put(HashTable *table, Value key, Value value, Allocator *al) {
  Entry *entry = get_or_insert(table, key, al);
  if (entry == NULL) {
    return false;
  }

  entry->value = value;
  return true;
}

void ht_delete(HashTable *table, Value key) {
  assert(table != NULL);
  Entry *entry = get(table, key);
  if (entry == NULL) {
    return;
  }
  mark_tombstone(entry);
  table->alive--;
}

void hti_init(HashTableIterator *iter, HashTable *table) {
  assert(table != NULL);
  iter->key = NULL;
  iter->value = NULL;
  iter->_table = table;
  iter->_index = -1;
}

bool hti_next(HashTableIterator *iter) {
  if (iter->_table == NULL) {
    return false;
  }

  while (++iter->_index < iter->_table->capacity) {
    Entry *entry = &iter->_table->entries[iter->_index];
    if (is_empty(entry) || is_tombstone(entry)) {
      continue;
    }

    iter->key = &entry->key;
    iter->value = &entry->value;
    return true;
  }

  return false;
}
