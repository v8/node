// Copyright 2012 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// The reason we write our own hash map instead of using unordered_map in STL,
// is that STL containers use a mutex pool on debug build, which will lead to
// deadlock when we are using async signal handler.

#ifndef V8_BASE_HASHMAP_H_
#define V8_BASE_HASHMAP_H_

#include <stdlib.h>

#include "src/base/bits.h"
#include "src/base/hashmap-entry.h"
#include "src/base/logging.h"

namespace v8 {
namespace base {

class DefaultAllocationPolicy {
 public:
  V8_INLINE void* New(size_t size) { return malloc(size); }
  V8_INLINE static void Delete(void* p) { free(p); }
};

// Metaprogramming helper to allow pointer keys to be passed by value and
// non-pointer keys by const reference.
template <typename Key>
struct MatchFunHelper;

template <typename Key, typename Value, class AllocationPolicy>
class TemplateHashMapImpl {
 public:
  typedef typename MatchFunHelper<Key>::Fun MatchFun;
  typedef TemplateHashMapEntry<Key, Value> Entry;

  // The default capacity.  This is used by the call sites which want
  // to pass in a non-default AllocationPolicy but want to use the
  // default value of capacity specified by the implementation.
  static const uint32_t kDefaultHashMapCapacity = 8;

  // initial_capacity is the size of the initial hash map;
  // it must be a power of 2 (and thus must not be 0).
  TemplateHashMapImpl(MatchFun match,
                      uint32_t capacity = kDefaultHashMapCapacity,
                      AllocationPolicy allocator = AllocationPolicy());

  ~TemplateHashMapImpl();

  // If an entry with matching key is found, returns that entry.
  // Otherwise, nullptr is returned.
  Entry* Lookup(const Key& key, uint32_t hash) const;

  // If an entry with matching key is found, returns that entry.
  // If no matching entry is found, a new entry is inserted with
  // corresponding key, key hash, and default initialized value.
  Entry* LookupOrInsert(const Key& key, uint32_t hash,
                        AllocationPolicy allocator = AllocationPolicy());

  Entry* InsertNew(const Key& key, uint32_t hash,
                   AllocationPolicy allocator = AllocationPolicy());

  // Removes the entry with matching key.
  // It returns the value of the deleted entry
  // or null if there is no value for such key.
  Value Remove(const Key& key, uint32_t hash);

  // Empties the hash map (occupancy() == 0).
  void Clear();

  // The number of (non-empty) entries in the table.
  uint32_t occupancy() const { return occupancy_; }

  // The capacity of the table. The implementation
  // makes sure that occupancy is at most 80% of
  // the table capacity.
  uint32_t capacity() const { return capacity_; }

  // Iteration
  //
  // for (Entry* p = map.Start(); p != nullptr; p = map.Next(p)) {
  //   ...
  // }
  //
  // If entries are inserted during iteration, the effect of
  // calling Next() is undefined.
  Entry* Start() const;
  Entry* Next(Entry* entry) const;

  // Some match functions defined for convenience.
  // TODO(leszeks): This isn't really matching pointers, so the name doesn't
  // really make sense, but we should remove this entirely and template the map
  // on the matching function.
  static bool PointersMatch(typename MatchFunHelper<Key>::KeyRef key1,
                            typename MatchFunHelper<Key>::KeyRef key2) {
    return key1 == key2;
  }

 private:
  MatchFun match_;
  Entry* map_;
  uint32_t capacity_;
  uint32_t occupancy_;

  Entry* map_end() const { return map_ + capacity_; }
  Entry* Probe(const Key& key, uint32_t hash) const;
  Entry* FillEmptyEntry(Entry* entry, const Key& key, const Value& value,
                        uint32_t hash,
                        AllocationPolicy allocator = AllocationPolicy());
  void Initialize(uint32_t capacity, AllocationPolicy allocator);
  void Resize(AllocationPolicy allocator);
};

template <typename Key>
struct MatchFunHelper {
  typedef const Key& KeyRef;
  typedef bool (*Fun)(KeyRef key1, KeyRef key2);
};

template <typename Key>
struct MatchFunHelper<Key*> {
  typedef Key* KeyRef;
  typedef bool (*Fun)(KeyRef key1, KeyRef key2);
};

typedef TemplateHashMapImpl<void*, void*, DefaultAllocationPolicy> HashMap;

template <typename Key, typename Value, class AllocationPolicy>
TemplateHashMapImpl<Key, Value, AllocationPolicy>::TemplateHashMapImpl(
    MatchFun match, uint32_t initial_capacity, AllocationPolicy allocator) {
  match_ = match;
  Initialize(initial_capacity, allocator);
}

template <typename Key, typename Value, class AllocationPolicy>
TemplateHashMapImpl<Key, Value, AllocationPolicy>::~TemplateHashMapImpl() {
  AllocationPolicy::Delete(map_);
}

template <typename Key, typename Value, class AllocationPolicy>
typename TemplateHashMapImpl<Key, Value, AllocationPolicy>::Entry*
TemplateHashMapImpl<Key, Value, AllocationPolicy>::Lookup(const Key& key,
                                                          uint32_t hash) const {
  Entry* entry = Probe(key, hash);
  return entry->exists() ? entry : nullptr;
}

template <typename Key, typename Value, class AllocationPolicy>
typename TemplateHashMapImpl<Key, Value, AllocationPolicy>::Entry*
TemplateHashMapImpl<Key, Value, AllocationPolicy>::LookupOrInsert(
    const Key& key, uint32_t hash, AllocationPolicy allocator) {
  // Find a matching entry.
  Entry* entry = Probe(key, hash);
  if (entry->exists()) {
    return entry;
  }

  return FillEmptyEntry(entry, key, Value(), hash, allocator);
}

template <typename Key, typename Value, class AllocationPolicy>
typename TemplateHashMapImpl<Key, Value, AllocationPolicy>::Entry*
TemplateHashMapImpl<Key, Value, AllocationPolicy>::InsertNew(
    const Key& key, uint32_t hash, AllocationPolicy allocator) {
  Entry* entry = Probe(key, hash);
  return FillEmptyEntry(entry, key, Value(), hash, allocator);
}

template <typename Key, typename Value, class AllocationPolicy>
Value TemplateHashMapImpl<Key, Value, AllocationPolicy>::Remove(const Key& key,
                                                                uint32_t hash) {
  // Lookup the entry for the key to remove.
  Entry* p = Probe(key, hash);
  if (!p->exists()) {
    // Key not found nothing to remove.
    return nullptr;
  }

  Value value = p->value;
  // To remove an entry we need to ensure that it does not create an empty
  // entry that will cause the search for another entry to stop too soon. If all
  // the entries between the entry to remove and the next empty slot have their
  // initial position inside this interval, clearing the entry to remove will
  // not break the search. If, while searching for the next empty entry, an
  // entry is encountered which does not have its initial position between the
  // entry to remove and the position looked at, then this entry can be moved to
  // the place of the entry to remove without breaking the search for it. The
  // entry made vacant by this move is now the entry to remove and the process
  // starts over.
  // Algorithm from http://en.wikipedia.org/wiki/Open_addressing.

  // This guarantees loop termination as there is at least one empty entry so
  // eventually the removed entry will have an empty entry after it.
  DCHECK(occupancy_ < capacity_);

  // p is the candidate entry to clear. q is used to scan forwards.
  Entry* q = p;  // Start at the entry to remove.
  while (true) {
    // Move q to the next entry.
    q = q + 1;
    if (q == map_end()) {
      q = map_;
    }

    // All entries between p and q have their initial position between p and q
    // and the entry p can be cleared without breaking the search for these
    // entries.
    if (!q->exists()) {
      break;
    }

    // Find the initial position for the entry at position q.
    Entry* r = map_ + (q->hash & (capacity_ - 1));

    // If the entry at position q has its initial position outside the range
    // between p and q it can be moved forward to position p and will still be
    // found. There is now a new candidate entry for clearing.
    if ((q > p && (r <= p || r > q)) || (q < p && (r <= p && r > q))) {
      *p = *q;
      p = q;
    }
  }

  // Clear the entry which is allowed to en emptied.
  p->clear();
  occupancy_--;
  return value;
}

template <typename Key, typename Value, class AllocationPolicy>
void TemplateHashMapImpl<Key, Value, AllocationPolicy>::Clear() {
  // Mark all entries as empty.
  const Entry* end = map_end();
  for (Entry* entry = map_; entry < end; entry++) {
    entry->clear();
  }
  occupancy_ = 0;
}

template <typename Key, typename Value, class AllocationPolicy>
typename TemplateHashMapImpl<Key, Value, AllocationPolicy>::Entry*
TemplateHashMapImpl<Key, Value, AllocationPolicy>::Start() const {
  return Next(map_ - 1);
}

template <typename Key, typename Value, class AllocationPolicy>
typename TemplateHashMapImpl<Key, Value, AllocationPolicy>::Entry*
TemplateHashMapImpl<Key, Value, AllocationPolicy>::Next(Entry* entry) const {
  const Entry* end = map_end();
  DCHECK(map_ - 1 <= entry && entry < end);
  for (entry++; entry < end; entry++) {
    if (entry->exists()) {
      return entry;
    }
  }
  return nullptr;
}

template <typename Key, typename Value, class AllocationPolicy>
typename TemplateHashMapImpl<Key, Value, AllocationPolicy>::Entry*
TemplateHashMapImpl<Key, Value, AllocationPolicy>::Probe(const Key& key,
                                                         uint32_t hash) const {
  DCHECK(base::bits::IsPowerOfTwo32(capacity_));
  Entry* entry = map_ + (hash & (capacity_ - 1));
  const Entry* end = map_end();
  DCHECK(map_ <= entry && entry < end);

  DCHECK(occupancy_ < capacity_);  // Guarantees loop termination.
  while (entry->exists() && (hash != entry->hash || !match_(key, entry->key))) {
    entry++;
    if (entry >= end) {
      entry = map_;
    }
  }

  return entry;
}

template <typename Key, typename Value, class AllocationPolicy>
typename TemplateHashMapImpl<Key, Value, AllocationPolicy>::Entry*
TemplateHashMapImpl<Key, Value, AllocationPolicy>::FillEmptyEntry(
    Entry* entry, const Key& key, const Value& value, uint32_t hash,
    AllocationPolicy allocator) {
  DCHECK(!entry->exists());

  new (entry) Entry(key, value, hash);
  occupancy_++;

  // Grow the map if we reached >= 80% occupancy.
  if (occupancy_ + occupancy_ / 4 >= capacity_) {
    Resize(allocator);
    entry = Probe(key, hash);
  }

  return entry;
}

template <typename Key, typename Value, class AllocationPolicy>
void TemplateHashMapImpl<Key, Value, AllocationPolicy>::Initialize(
    uint32_t capacity, AllocationPolicy allocator) {
  DCHECK(base::bits::IsPowerOfTwo32(capacity));
  map_ = reinterpret_cast<Entry*>(allocator.New(capacity * sizeof(Entry)));
  if (map_ == nullptr) {
    FATAL("Out of memory: HashMap::Initialize");
    return;
  }
  capacity_ = capacity;
  Clear();
}

template <typename Key, typename Value, class AllocationPolicy>
void TemplateHashMapImpl<Key, Value, AllocationPolicy>::Resize(
    AllocationPolicy allocator) {
  Entry* map = map_;
  uint32_t n = occupancy_;

  // Allocate larger map.
  Initialize(capacity_ * 2, allocator);

  // Rehash all current entries.
  for (Entry* entry = map; n > 0; entry++) {
    if (entry->exists()) {
      Entry* new_entry = Probe(entry->key, entry->hash);
      new_entry = FillEmptyEntry(new_entry, entry->key, entry->value,
                                 entry->hash, allocator);
      n--;
    }
  }

  // Delete old map.
  AllocationPolicy::Delete(map);
}

// A hash map for pointer keys and values with an STL-like interface.
template <class Key, class Value, class AllocationPolicy>
class TemplateHashMap
    : private TemplateHashMapImpl<void*, void*, AllocationPolicy> {
 public:
  STATIC_ASSERT(sizeof(Key*) == sizeof(void*));    // NOLINT
  STATIC_ASSERT(sizeof(Value*) == sizeof(void*));  // NOLINT
  struct value_type {
    Key* first;
    Value* second;
  };

  class Iterator {
   public:
    Iterator& operator++() {
      entry_ = map_->Next(entry_);
      return *this;
    }

    value_type* operator->() { return reinterpret_cast<value_type*>(entry_); }
    bool operator!=(const Iterator& other) { return entry_ != other.entry_; }

   private:
    Iterator(const TemplateHashMapImpl<void*, void*, AllocationPolicy>* map,
             typename TemplateHashMapImpl<void*, void*,
                                          AllocationPolicy>::Entry* entry)
        : map_(map), entry_(entry) {}

    const TemplateHashMapImpl<void*, void*, AllocationPolicy>* map_;
    typename TemplateHashMapImpl<void*, void*, AllocationPolicy>::Entry* entry_;

    friend class TemplateHashMap;
  };

  TemplateHashMap(typename TemplateHashMapImpl<
                      void*, void*, AllocationPolicy>::MatchFun match,
                  AllocationPolicy allocator = AllocationPolicy())
      : TemplateHashMapImpl<void*, void*, AllocationPolicy>(
            match,
            TemplateHashMapImpl<void*, void*,
                                AllocationPolicy>::kDefaultHashMapCapacity,
            allocator) {}

  Iterator begin() const { return Iterator(this, this->Start()); }
  Iterator end() const { return Iterator(this, nullptr); }
  Iterator find(Key* key, bool insert = false,
                AllocationPolicy allocator = AllocationPolicy()) {
    if (insert) {
      return Iterator(this, this->LookupOrInsert(key, key->Hash(), allocator));
    }
    return Iterator(this, this->Lookup(key, key->Hash()));
  }
};

}  // namespace base
}  // namespace v8

#endif  // V8_BASE_HASHMAP_H_
