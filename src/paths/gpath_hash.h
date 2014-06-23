#ifndef GPATH_HASH_H_
#define GPATH_HASH_H_

#include "cortex_types.h"
#include "gpath_store.h"

// Packed structure is 10 bytes
// Do not use pointes to fields in this struct - they are not aligned
struct GPEntryStruct
{
  // 5 bytes each
  hkey_t hkey:40;
  pkey_t gpindex:40;
} __attribute((packed));

typedef struct GPEntryStruct GPEntry;

typedef struct
{
  GPathStore *const gpstore; // Add to this path store
  GPEntry *const table; // Using this table to remove duplicates
  const size_t num_of_buckets; // needs to store maximum of 1<<32
  const uint8_t bucket_size; // max value 255
  const uint64_t capacity, mask; // num_of_buckets * bucket_size
  uint8_t *const bucket_nitems; // number of items in each bucket
  uint8_t *const bktlocks; // always cast to volatile
  size_t num_entries;
} GPathHash;

void gpath_hash_alloc(GPathHash *phash, GPathStore *gpstore, size_t mem_in_bytes);
void gpath_hash_dealloc(GPathHash *phash);
void gpath_hash_reset(GPathHash *phash);

// Returns NULL if out of memory
// Thread Safe: uses bucket level locks
GPath* gpath_hash_find_or_insert_mt(GPathHash *restrict phash,
                                    hkey_t hkey, GPathNew newgpath,
                                    bool *found);

// Load all paths already in GPathStore into this hash table
// void gpath_hash_load_all(GPathHash *gphash, size_t nkmer_capacity);

#endif /* GPATH_HASH_H_ */
