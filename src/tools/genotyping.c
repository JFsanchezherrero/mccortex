#include "global.h"
#include "genotyping.h"

struct GenotyperStruct {
  StrBuf seq;
  khash_t(BkToBits) *h;
  GenoKmerBuffer kmer_buf;
};

Genotyper* genotyper_init()
{
  Genotyper *gt = ctx_calloc(1, sizeof(Genotyper));
  strbuf_alloc(&gt->seq, 1024);
  gt->h = kh_init(BkToBits);
  genokmer_buf_alloc(&gt->kmer_buf, 512);
  return gt;
}

void genotyper_destroy(Genotyper *gt)
{
  strbuf_dealloc(&gt->seq);
  kh_destroy(BkToBits, gt->h);
  genokmer_buf_dealloc(&gt->kmer_buf);
  ctx_free(gt);
}

int genovar_ptr_cmp(const GenoVar *a, const GenoVar *b)
{
  if(a->pos != b->pos) return a->pos < b->pos ? -1 : 1;
  if(a->reflen != b->reflen) return a->reflen < b->reflen ? -1 : 1;
  if(a->altlen != b->altlen) return a->altlen < b->altlen ? -1 : 1;
  return strncmp(a->alt, b->alt, a->altlen);
}

static inline int _genovar_ptr_cmp(const void *aa, const void *bb)
{
  const GenoVar *a = *(const GenoVar*const*)aa, *b = *(const GenoVar*const*)bb;
  return genovar_ptr_cmp(a, b);
}

void genovars_sort(GenoVar **vars, size_t nvars)
{
  qsort(vars, nvars, sizeof(*vars), _genovar_ptr_cmp);
}

static bool vars_compatible(const GenoVar *const*vars, size_t nvars,
                            uint64_t bits)
{
  ctx_assert(nvars < 64);
  size_t i, end = 0;
  uint64_t b;
  for(i = 0, b = 1; i < nvars; i++, b<<=1) {
    if(bits & b) {
      if(vars[i]->pos < end) return false;
      end = vars[i]->pos + vars[i]->reflen;
    }
  }
  return true;
}

// Generate the DNA string sequence of a haplotype
// Store it in parameter seq
static inline void assemble_haplotype_str(StrBuf *seq, const char *chrom,
                                          size_t regstart, size_t regend,
                                          const GenoVar *const*vars,
                                          size_t nvars, uint64_t bits)
{
  strbuf_reset(seq);
  uint64_t i, end = regstart;

  for(i = 0; i < nvars; i++, bits>>=1) {
    if(bits & 1) {
      ctx_assert(end >= vars[i]->pos);
      strbuf_append_strn(seq, chrom+end, vars[i]->pos-end);
      strbuf_append_strn(seq, vars[i]->alt, vars[i]->altlen);
      end = vars[i]->pos + vars[i]->reflen;
    }
  }
  strbuf_append_strn(seq, chrom+end, regend-end);
}

//                 arararararar r=ref, a=alt
// var:  543210    554433221100
// bits: 010110 -> 011001101001
static inline uint64_t varbits_to_altrefbits(uint64_t bits,
                                             size_t tgtidx,
                                             size_t ntgts)
{
  ctx_assert(ntgts <= 32);
  uint64_t i, r = 0;
  bits >>= tgtidx;
  for(i = 0; i < ntgts; i++, bits>>=1)
    r |= 1UL << (i*2 + (bits&1));
  return r;
}


/**
 * Get a list of kmers which support variants.
 *
 * @param typer     initialised memory to use
 * @param vars      variants to genotype and surrounding vars.
 *                  Required sort order: pos, reflen, altlen, alt
 * @param nvars     number of variants in `vars`
 * @param tgtidx    index in vars of first variant to type
 * @param ntgts     number of variants to type
 * @param chrom     reference chromosome
 * @param chromlen  length of reference chromosom
 * @param kmer_size kmer size to type at
 * @return number of kmers
 */
size_t genotyping_get_kmers(Genotyper *typer,
                            const GenoVar *const*vars, size_t nvars,
                            size_t tgtidx, size_t ntgts,
                            const char *chrom, size_t chromlen,
                            size_t kmer_size, GenoKmer **result)
{
  ctx_assert2(0 < nvars && nvars < 64, "nvars: %zu", nvars);
  ctx_assert2(tgtidx < nvars, "tgtidx:%zu >= nvars:%zu ??", tgtidx, nvars);
  ctx_assert2(ntgts <= 32, "Too many targets: %zu", ntgts);

  GenoKmerBuffer *gkbuf = &typer->kmer_buf;
  genokmer_buf_reset(gkbuf);

  const GenoVar *tgt = vars[tgtidx];

  long minpos = MIN2(vars[0]->pos, tgt->pos - kmer_size + 1);
  size_t i, regstart, regend;

  regstart = MAX2(minpos, 0);
  regend = regstart;
  for(i = 0; i < nvars; i++) regend = MAX2(regend, vars[i]->pos+vars[i]->reflen);
  ctx_assert(regend <= chromlen);
  regend = MAX2(regend, tgt->pos + tgt->reflen + kmer_size - 1);
  regend = MIN2(regend, chromlen);

  StrBuf *seq = &typer->seq;
  khash_t(BkToBits) *h = typer->h;
  BinaryKmer bkey;
  int hret;
  khiter_t k;

  // TODO: may be faster to clear at the end using list of entries
  kh_clear(BkToBits, h);

  // Start with ref haplotype (no variants)
  uint64_t bits = 0, limit = 1UL<<nvars, altref_bits;

  for(; bits < limit; bits++) {
    if(vars_compatible(vars, nvars, bits)) {
      // Construct haplotype
      assemble_haplotype_str(seq, chrom, regstart, regend,
                             vars, nvars, bits);

      altref_bits = varbits_to_altrefbits(bits, tgtidx, ntgts);

      // Covert to kmers, find/add them to the hash table, OR bits
      for(i = 0; i + kmer_size <= seq->end; i++) {
        bkey = binary_kmer_from_str(seq->b+i, kmer_size);
        bkey = binary_kmer_get_key(bkey, kmer_size);
        k = kh_put(BkToBits, h, bkey, &hret);
        if(hret < 0) die("khash table failed: out of memory?");
        if(hret > 0) kh_value(h, k) = 0; // initialise if not already in table
        kh_value(h, k) |= altref_bits;
      }
    }
  }

  size_t nkmers = kh_size(h);
  genokmer_buf_capacity(gkbuf, nkmers);

  for(i = 0, k = kh_begin(h); k != kh_end(h); ++k) {
    if(kh_exist(h, k)) {
      bkey = kh_key(h, k);
      altref_bits = kh_value(h, k);
      if(genotyping_refalt_uniq(altref_bits)) {
        gkbuf->b[i++] = (GenoKmer){.bkey = bkey, .arbits = altref_bits};
      }
    }
  }
  gkbuf->len = i;

  *result = gkbuf->b;
  return gkbuf->len;
}
