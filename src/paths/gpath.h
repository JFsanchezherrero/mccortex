#ifndef GPATH_H_
#define GPATH_H_

// 20 bytes per path
typedef struct GPathStruct GPath;

#define GPATH_MAX_KMERS (UINT32_MAX>>1)
#define GPATH_MAX_JUNCS UINT16_MAX
#define GPATH_MAX_SEEN UINT8_MAX

// 8+8+2=18bytes
struct GPathStruct
{
  uint8_t *seq;
  uint16_t num_juncs:15, orient:1;
  // only used for threading with single colour
  // uint32_t num_kmers;
  // uint8_t num_seen;
  GPath *next;
}
__attribute__((packed));

#define gpath_get_colset(gp,ncols) ((gp)->seq - (((ncols)+7)/8))
#define gpath_has_colour(gp,ncols,col) bitset_get(gpath_get_colset(gp,ncols),col)
#define gpath_set_colour(gp,ncols,col) bitset_set(gpath_get_colset(gp,ncols),col)
#define gpath_wipe_colset(gp,ncols) memset(gpath_get_colset(gp,ncols), 0, ((ncols)+7)/8)

// Compare by orient, sequence, number of junctions, number of kmers
int gpath_cmp(const GPath *a, const GPath *b);

static inline int gpath_cmp_void(const void *a, const void *b) {
  return gpath_cmp(*(const GPath**)a, *(const GPath**)b);
}

// Get number of bits set
size_t gpath_colset_bits_set(const GPath *gpath, size_t ncols);

// Copy colour set bits from src to dst
void gpath_colset_or_mt(GPath *dst_gp, const GPath *src_gp, size_t ncols);

// Remove from `set0` bits that are set in `set1`
// Returns 0 if no colours remain in src path, 1 otherwise
uint8_t gpath_colset_rm_intersect(const GPath *dst_gp, GPath *src_gp, size_t ncols);

#endif /* GPATH_H_ */
