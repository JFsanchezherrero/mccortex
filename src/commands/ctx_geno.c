#include "global.h"
#include "commands.h"
#include "util.h"
#include "file_util.h"
#include "db_graph.h"
#include "graphs_load.h"
#include "seq_reader.h"
#include "gpath_checks.h"
#include "seq_reader.h"
#include "genotyping.h"

#include "htslib/vcf.h"
#include "htslib/faidx.h"

const char geno_usage[] =
"usage: "CMD" geno [options] <in.vcf> <in.ctx> [in2.ctx ...]\n"
"\n"
"  Genotype a VCF using cortex graphs. VCF must be sorted by position. \n"
"  VCF must be a file, not piped in. It is recommended to use uncleaned graphs.\n"
"\n"
"  -h, --help              This help message\n"
"  -q, --quiet             Silence status output normally printed to STDERR\n"
"  -f, --force             Overwrite output files\n"
"  -m, --memory <mem>      Memory to use\n"
"  -n, --nkmers <kmers>    Number of hash table entries (e.g. 1G ~ 1 billion)\n"
"  -o, --out <bub.txt.gz>  Output file [default: STDOUT]\n"
"  -r, --ref <ref.fa>      Reference file\n"
"\n";

static struct option longopts[] =
{
// General options
  {"help",         no_argument,       NULL, 'h'},
  {"out",          required_argument, NULL, 'o'},
  {"force",        no_argument,       NULL, 'f'},
  {"memory",       required_argument, NULL, 'm'},
  {"nkmers",       required_argument, NULL, 'n'},
  {"ref",          required_argument, NULL, 'r'},
  {NULL, 0, NULL, 0}
};

/**
 * Get coverage of ref and alt alleles from the de Bruijn graph in the given
 * colour.
 */
void bkey_get_covg(BinaryKmer bkey, uint64_t altref_bits,
                   GenoVar *gts, size_t ntgts,
                   int colour, const dBGraph *db_graph)
{
  (void)gts; (void)colour;

  size_t i;
  dBNode node = db_graph_find(db_graph, bkey);

  if(node.key != HASH_NOT_FOUND) {
    // Covg covg = colour >= 0 ? db_node_get_covg(db_graph, node.key, colour)
    //                         : db_node_sum_covg(db_graph, node.key);

    for(i = 0; i < ntgts; i++, altref_bits >>= 2) {
      // if((altref_bits & 3) == 1) { gts[i].refkmers++; gts[i].refsumcovg += covg; }
      // else if((altref_bits & 3) == 2) { gts[i].altkmers++; gts[i].altsumcovg += covg; }
      // else { /* ignore kmer in both ref/alt */ }
    }
  }
}

// return true if valid variant
static inline bool init_new_var(GenoVar *var, GenoVCF *parent, uint32_t aid)
{
  bcf1_t *v = &parent->v;
  uint32_t pos = v->pos, reflen, altlen;
  const char *ref = v->d.allele[0], *alt = v->d.allele[aid];
  reflen = strlen(ref);
  altlen = strlen(alt);

  // Left trim
  while(reflen && altlen && *ref == *alt) {
    ref++; alt++; pos++;
    reflen--; altlen--;
  }

  // Right trim
  while(reflen && altlen && ref[reflen-1] == alt[altlen-1]) {
    reflen--; altlen--;
  }

  if(!reflen && !altlen) return false;

  // Initialise
  var->parent = parent;
  var->ref = ref;
  var->alt = alt;
  var->pos = pos;
  var->reflen = reflen;
  var->altlen = altlen;
  var->aid = aid;

  // Increment number of children
  parent->nchildren++;

  return true;
}

static void vcf_list_populate(GenoVCFPtrList *vlist, size_t n)
{
  size_t i;
  for(i = 0; i < n; i++) {
    GenoVCF *v = ctx_calloc(1, sizeof(GenoVCF));
    genovcf_ptr_list_append(vlist, v);
  }
}

static void var_list_populate(GenoVarPtrList *alist, size_t n)
{
  size_t i;
  for(i = 0; i < n; i++) {
    GenoVar *v = ctx_calloc(1, sizeof(GenoVar));
    genovar_ptr_list_append(alist, v);
  }
}

void bcf_empty1(bcf1_t *v);
static void vcf_list_destroy(GenoVCFPtrList *vlist)
{
  size_t i;
  for(i = 0; i < genovcf_ptr_list_len(vlist); i++) {
    GenoVCF *v = genovcf_ptr_list_get(vlist, i);
    bcf_empty1(&v->v);
    ctx_free(v);
  }
}

static void var_list_destroy(GenoVarPtrList *alist)
{
  size_t i;
  for(i = 0; i < genovar_ptr_list_len(alist); i++) {
    GenoVar *a = genovar_ptr_list_get(alist, i);
    ctx_free(a);
  }
}

typedef struct {
  htsFile *vcffh;
  bcf_hdr_t *vcfhdr;
  // vpool are ready to be used
  // vwait are waiting to be printed
  GenoVCFPtrList vpool, vwait; // pool of vcf lines
  size_t nxtprint; // index (vidx) of next GenoVCF to be printed
  // alist are current alleles, anchrom are on next chromosome
  // apool is a memory pool
  GenoVarPtrList alist, anchrom, apool; // alleles
} VcfReader;

// Returns:
//  -1 if EOF / error
//  0 if read would have added var on another chrom
//  Otherwise number of variants added
//
static int vcf_reader_fetch(VcfReader *vr)
{
  // Check we have GenoVCF entries to read into
  if(genovcf_ptr_list_len(&vr->vpool) == 0) vcf_list_populate(&vr->vpool, 16);

  // If we already have alleles from a diff chrom to use, return them
  if(genovar_ptr_list_len(&vr->alist) == 0 && genovar_ptr_list_len(&vr->anchrom))
  {
    genovar_ptr_list_push(&vr->alist, genovar_ptr_list_getptr(&vr->anchrom, 0),
                                      genovar_ptr_list_len(&vr->anchrom));
    genovar_ptr_list_reset(&vr->anchrom);
    return genovar_ptr_list_len(&vr->alist);
  }

  while(1)
  {
    // Take vcf out of pool
    GenoVCF *ve;
    genovcf_ptr_list_pop(&vr->vpool, &ve, 1);
    bcf1_t *v = &ve->v;

    // Read VCF
    if(bcf_read(vr->vcffh, vr->vcfhdr, v) < 0) {
      // EOF
      genovcf_ptr_list_push(&vr->vpool, &ve, 1);
      return -1;
    }

    // Unpack all info
    bcf_unpack(v, BCF_UN_ALL);

    // Check we have enough vars to decompose
    size_t i, n = MAX2(v->n_allele, 16);
    if(genovar_ptr_list_len(&vr->apool) < n) var_list_populate(&vr->apool, n);

    size_t nadded = 0, nexisting_alleles = genovar_ptr_list_len(&vr->alist);
    bool diff_chroms = false, overlap = false;

    if(nexisting_alleles) {
      GenoVar *var = genovar_ptr_list_get(&vr->alist, nexisting_alleles-1);
      diff_chroms = (var->parent->v.rid != v->rid);
      int32_t var_end = var->parent->v.pos + strlen(var->parent->v.d.allele[0]);
      overlap = (!diff_chroms && var_end > v->pos);
    }

    ctx_assert2(!diff_chroms || vr->anchrom.end == 0, "Already read diff chrom");

    // Load alleles alist, using insert sort
    GenoVar *var;
    genovar_ptr_list_pop(&vr->apool, &var, 1);
    GenoVarPtrList *list = diff_chroms ? &vr->anchrom : &vr->alist;

    // i==0 is ref allele
    for(i = 1; i < v->n_allele; i++) {
      if(init_new_var(var, ve, i)) {
        genovar_ptr_list_push(list, &var, 1);
        genovar_ptr_list_pop(&vr->apool, &var, 1);
        nadded++;
      }
    }
    // Re-add unused var
    genovar_ptr_list_push(&vr->apool, &var, 1);

    if(nadded)
    {
      if(overlap || diff_chroms) {
        genovars_sort(genovar_ptr_list_getptr(list, 0),
                      genovar_ptr_list_len(list));
      } else {
        // Just sort the alleles we added to the end of alist
        genovars_sort(genovar_ptr_list_getptr(&vr->alist, nexisting_alleles),
                      genovar_ptr_list_len(&vr->alist) - nexisting_alleles);
      }

      return diff_chroms ? 0 : nadded;
    }
  }
}

static void vcf_reader_drop_var(VcfReader *vr, size_t idx)
{
  GenoVar *a = genovar_ptr_list_get(&vr->alist, idx);
  genovar_ptr_list_append(&vr->apool, a);
  a->parent->nchildren--;
  // Re-add parent vcf to pool if no longer used
  if(a->parent->nchildren == 0) {
    genovcf_ptr_list_append(&vr->vwait, a->parent);
  }
  // Instead of removing `a` from sorted array vr->alist, set it to NULL
  genovar_ptr_list_set(&vr->alist, idx, NULL);
}

// Remove NULL entries from vr->alist
static void vcf_reader_shrink_vars(VcfReader *vr)
{
  size_t i, j, len = genovar_ptr_list_len(&vr->alist);
  GenoVar *var;
  for(i = j = 0; i < len; i++) {
    var = genovar_ptr_list_get(&vr->alist, i);
    if(var != NULL) {
      genovar_ptr_list_set(&vr->alist, j, var);
      j++;
    }
  }
  genovar_ptr_list_pop(&vr->alist, NULL, i-j);
}

static int _genovcf_cmp(const void *aa, const void *bb)
{
  const GenoVCF *a = *(const GenoVCF*const*)aa, *b = *(const GenoVCF*const*)bb;
  return (long)a->vidx - (long)b->vidx;
}

static void vcf_reader_print_waiting(VcfReader *vr,
                                     htsFile *outfh, bcf_hdr_t *outhdr)
{
  // Sort waiting by vidx
  GenoVCF **vcfptr = genovcf_ptr_list_getptr(&vr->vwait, 0);
  size_t len = genovcf_ptr_list_len(&vr->vwait);
  qsort(vcfptr, len, sizeof(GenoVCF*), _genovcf_cmp);

  // print
  size_t start = vr->nxtprint, end = vr->nxtprint + len;
  while(vr->nxtprint < end && vr->nxtprint == (*vcfptr)->vidx)
  {
    if(bcf_write(outfh, outhdr, &(*vcfptr)->v) != 0) die("Cannot write record");
    vcfptr++;
    vr->nxtprint++;
  }
  // Take off those that were printed
  size_t nprinted = vr->nxtprint - start;
  genovcf_ptr_list_unshift(&vr->vwait, NULL, nprinted);
}


#define INIT_BUF_SIZE 128
uint32_t max_gt_vars = 8; // 2^8 = 256 possible haplotypes

static void genotype_vcf(htsFile *vcffh, bcf_hdr_t *vcfhdr,
                         htsFile *outfh, bcf_hdr_t *outhdr,
                         faidx_t *fai, const dBGraph *db_graph)
{
  (void)db_graph;
  VcfReader vr;
  memset(&vr, 0, sizeof(vr));
  vr.vcffh = vcffh;
  vr.vcfhdr = vcfhdr;

  Genotyper *gtyper = genotyper_init();

  genovcf_ptr_list_alloc(&vr.vpool, INIT_BUF_SIZE);
  genovar_ptr_list_alloc(&vr.alist, INIT_BUF_SIZE);
  genovar_ptr_list_alloc(&vr.apool, INIT_BUF_SIZE);

  vcf_list_populate(&vr.vpool, INIT_BUF_SIZE);
  var_list_populate(&vr.apool, INIT_BUF_SIZE);

  int32_t refid = -1;
  char *chrom = NULL;
  int chromlen = 0;

  int n;
  while((n = vcf_reader_fetch(&vr)) >= 0)
  {
    // Get ref chromosome
    GenoVar *first = genovar_ptr_list_get(&vr.alist, 0);
    bcf1_t *ve = &first->parent->v;
    if(refid != ve->rid) {
      free(chrom);
      chrom = fai_fetch(fai, bcf_seqname(vcfhdr, ve), &chromlen);
      if(chrom == NULL) die("Cannot find chrom '%s'", bcf_seqname(vcfhdr, ve));
      refid = ve->rid;
    }

    // TODO Pick some variants to genotype

    // Remove redundant ones
    size_t idx = 0;
    vcf_reader_drop_var(&vr, idx);

    // Shrink array if we dropped any vars
    vcf_reader_shrink_vars(&vr);

    vcf_reader_print_waiting(&vr, outfh, outhdr);
  }

  // TODO deal with remainder
  vcf_reader_print_waiting(&vr, outfh, outhdr);

  free(chrom);

  vcf_list_destroy(&vr.vpool);
  var_list_destroy(&vr.apool);
  var_list_destroy(&vr.alist);

  genovcf_ptr_list_dealloc(&vr.vpool);
  genovar_ptr_list_dealloc(&vr.alist);
  genovar_ptr_list_dealloc(&vr.apool);

  genotyper_destroy(gtyper);
}

/*
static inline bool read_vcf_entry(htsFile *vcf_file, bcf_hdr_t *vcfhdr,
                                  bcf1_t *v)
{
  int s = bcf_read(vcf_file, vcfhdr, v);
  if(s < 0) die("Cannot read");
  else if(s == 0) return false;
  else {
    bcf_unpack(v, BCF_UN_ALL);
    if(v->n_allele < 2) die("No alleles in VCF entry - is this possible?");
    return true;
  }
}

// How many vcf entries to read in
#define MAX_NVCF 100


static void genotype_vcf(htsFile *vcf_file, bcf_hdr_t *vcfhdr,
                         const dBGraph *db_graph)
{
  size_t i, j, nvcfs = 0, nvcf_size = MAX_NVCF;

  Genotyper *gtyper = genotyper_init();

  size_t *nvs = ctx_calloc(nvcf_size, sizeof(size_t));
  bcf1_t **vs = ctx_calloc(nvcf_size, sizeof(bcf1_t*));
  for(i = 0; i < nvcf_size; i++) vs[i] = bcf_init1();

  GenoVarList decomp;
  genovar_list_alloc(&decomp, 1024);

  GenoVarPtrBuffer var_ptrs;
  genovar_ptr_buf_alloc(&var_ptrs, 1024);

  const char *chrom = NULL;
  size_t chromlen = 0;

  while(1)
  {
    // Fill pool with variants
    for(; nvcfs < nvcf_size; nvcfs++)
      if(!read_vcf_entry(vcf_file, vcfhdr, vs[nvcfs])) break;

    if(!nvcfs) break;

    // decompose into simple events in decomp
    GenoVar gv;
    genovar_list_reset(&decomp);
    for(i = 0; i < nvcfs; i++) {
      for(j = 0; j < vs[i]->n_allele; j++) {
        if(init_new_var(&gv, vs[i], i, j))
          genovar_list_push(&decomp, &gv, 1);
      }
      nvs[i] = genovar_list_len(&decomp);
    }

    // Put list into var_ptrs and sort
    genovar_ptr_buf_reset(&var_ptrs);
    for(i = 0; i < genovar_list_len(&decomp); i++)
      genovar_ptr_buf_add(&var_ptrs, &decomp.b[i]);
    genovars_sort(var_ptrs.b, genovar_list_len(&decomp));

    // count kmers on same chrom within k dist of each other


    // Genotype a few, print them
    GenoKmer *klist = NULL;
    size_t nkmers = 0;
    nkmers = genotyping_get_kmers(gtyper,
                                  (const GenoVar *const*)genovar_ptr_buf_getptr(&var_ptrs, 0),
                                  genovar_ptr_buf_len(&var_ptrs),
                                  0, var_ptrs.len,
                                  chrom, chromlen,
                                  db_graph->kmer_size, &klist);

    // shift off a few vcf entries

  }

  // for(i = 0; i < nvcf_size; i++) bcf_destroy(vpool[i].v);
  for(i = 0; i < nvcf_size; i++) bcf_destroy(vs[i]);

  genovar_list_dealloc(&decomp);
  genovar_ptr_buf_dealloc(&var_ptrs);
  ctx_free(nvs);
  ctx_free(vs);
  genotyper_destroy(gtyper);
}
*/

int ctx_geno(int argc, char **argv)
{
  struct MemArgs memargs = MEM_ARGS_INIT;
  const char *out_path = NULL;

  char *ref_path = NULL;

  // Arg parsing
  char cmd[100];
  char shortopts[300];
  cmd_long_opts_to_short(longopts, shortopts, sizeof(shortopts));
  int c;
  size_t i;

  // silence error messages from getopt_long
  // opterr = 0;

  while((c = getopt_long_only(argc, argv, shortopts, longopts, NULL)) != -1) {
    cmd_get_longopt_str(longopts, c, cmd, sizeof(cmd));
    switch(c) {
      case 0: /* flag set */ break;
      case 'h': cmd_print_usage(NULL); break;
      case 'o': cmd_check(!out_path, cmd); out_path = optarg; break;
      case 'f': cmd_check(!futil_get_force(), cmd); futil_set_force(true); break;
      case 'm': cmd_mem_args_set_memory(&memargs, optarg); break;
      case 'n': cmd_mem_args_set_nkmers(&memargs, optarg); break;
      case 'r': cmd_check(!ref_path, cmd); ref_path = optarg; break;
        break;
      case ':': /* BADARG */
      case '?': /* BADCH getopt_long has already printed error */
        // cmd_print_usage(NULL);
        die("`"CMD" geno -h` for help. Bad option: %s", argv[optind-1]);
      default: abort();
    }
  }

  // Defaults for unset values
  if(out_path == NULL) out_path = "-";
  if(ref_path == NULL) cmd_print_usage("Require a reference (-r,--ref <ref.fa>)");
  if(optind+2 > argc) cmd_print_usage("Require VCF and graph files");

  // open ref
  // index fasta with: samtools faidx ref.fa
  faidx_t *fai = fai_load(ref_path);
  if(fai == NULL) die("Cannot load ref index: %s / %s.fai", ref_path, ref_path);

  // Open input VCF file
  const char *vcf_path = argv[optind++];
  htsFile *vcffh = hts_open(vcf_path, "r");
  bcf_hdr_t *vcfhdr = bcf_hdr_read(vcffh);

  //
  // Open graph files
  //
  const size_t num_gfiles = argc - optind;
  char **graph_paths = argv + optind;
  ctx_assert(num_gfiles > 0);

  GraphFileReader *gfiles = ctx_calloc(num_gfiles, sizeof(GraphFileReader));
  size_t ncols, ctx_max_kmers = 0, ctx_sum_kmers = 0;

  ncols = graph_files_open(graph_paths, gfiles, num_gfiles,
                           &ctx_max_kmers, &ctx_sum_kmers);

  // Check graph + paths are compatible
  graphs_gpaths_compatible(gfiles, num_gfiles, NULL, 0, -1);

  //
  // Decide on memory
  //
  size_t bits_per_kmer, kmers_in_hash, graph_mem;

  bits_per_kmer = sizeof(BinaryKmer)*8 + sizeof(Covg) * ncols;
  kmers_in_hash = cmd_get_kmers_in_hash(memargs.mem_to_use,
                                        memargs.mem_to_use_set,
                                        memargs.num_kmers,
                                        memargs.num_kmers_set,
                                        bits_per_kmer,
                                        -1, -1,
                                        true, &graph_mem);

  cmd_check_mem_limit(memargs.mem_to_use, graph_mem);

  //
  // Open output file
  //
  htsFile *outfh = hts_open(out_path, "w");

  // Allocate memory
  dBGraph db_graph;
  db_graph_alloc(&db_graph, gfiles[0].hdr.kmer_size, ncols, 1, kmers_in_hash,
                 DBG_ALLOC_COVGS);

  //
  // Load graphs
  //
  LoadingStats stats = LOAD_STATS_INIT_MACRO;

  GraphLoadingPrefs gprefs = {.db_graph = &db_graph,
                              .boolean_covgs = false,
                              .must_exist_in_graph = true,
                              .must_exist_in_edges = NULL,
                              .empty_colours = false};

  for(i = 0; i < num_gfiles; i++) {
    graph_load(&gfiles[i], gprefs, &stats);
    graph_file_close(&gfiles[i]);
    gprefs.empty_colours = false;
  }
  ctx_free(gfiles);

  hash_table_print_stats(&db_graph.ht);

  // Add samples to vcf header
  bcf_hdr_t *outhdr = bcf_hdr_dup(vcfhdr);
  for(i = 0; i < db_graph.num_of_cols; i++) {
    status("Name %zu: %s", i, db_graph.ginfo[i].sample_name.b);
    bcf_hdr_add_sample(outhdr, db_graph.ginfo[i].sample_name.b);
  }

  if(bcf_hdr_write(outfh, outhdr) != 0)
    die("Cannot write header to: %s", futil_outpath_str(out_path));

  genotype_vcf(vcffh, vcfhdr, outfh, outhdr, fai, &db_graph);

  /*
  // Genotype calls
  read_t *chrom;
  const char *chrom_name;
  GenoVarList vlist;
  genovar_list_alloc(&vlist, 256);

  bcf1_t *v = bcf_init1();

  Genotyper *gtyper = genotyper_init();

  const size_t kmer_size = db_graph.kmer_size;
  size_t tgtidx, ntgts;
  GenoVar *last;
  size_t end;

  if(!read_vars(&vlist, vcffh, vcfhdr, v)) warn("Empty VCF");
  else {
    tgtidx = 0;
    ntgts = 1;
    while(1)
    {
      last = genovar_list_getptr(&vlist, tgtidx);
      end = last->pos + last->reflen;
      while(last->pos <= end+kmer_size) {
        if(!read_vars(&vlist, vcffh, vcfhdr, v)) break;
        last = genovar_list_getptr(&vlist, genovar_list_len(&vlist)-1);
        end = last->pos + last->reflen;
      }

      // Genotype and print
      chrom_name = vcfhdr->id[BCF_DT_CTG][v->rid].key;
      chrom = seq_fetch_chrom(genome, chrom_name);

      GenoKmer *kmers = NULL;
      size_t nkmers = 0;

      // nkmers = genotyping_get_kmers(gtyper,
      //                               genovar_list_getptr(&vlist, 0),
      //                               genovar_list_len(&vlist),
      //                               tgtidx, ntgts,
      //                               chrom->seq.b, chrom->seq.end,
      //                               kmer_size, &kmers);

      int colour = -1;

      for(i = 0; i < nkmers; i++) {
        bkey_get_covg(kmers[i].bkey, kmers[i].arbits,
                      genovar_list_getptr(&vlist, tgtidx), ntgts,
                      colour, &db_graph);
      }

      // Set new tgt
      tgtidx += ntgts;
      ntgts = 1;
      if(tgtidx >= genovar_list_len(&vlist)) break; // done

      // DEV: shift off unwanted
    }
  }

  bcf_destroy(v);

  genotyper_destroy(gtyper);
  */

    // fprintf(stderr, "%s %i %u", chrom_name, v->pos+1, v->rlen);
    // fprintf(stderr, " %s", v->d.allele[0]);
    // fprintf(stderr, " %s", v->d.allele[1]);
    // for(i=2; i < v->n_allele; i++)
    //   fprintf(stderr, ",%s", v->d.allele[i]);
    // fprintf(stderr, "\n");

  status("  saved to: %s\n", out_path);

  bcf_hdr_destroy(vcfhdr);
  bcf_hdr_destroy(outhdr);
  hts_close(vcffh);
  hts_close(outfh);
  fai_destroy(fai);
  db_graph_dealloc(&db_graph);

  return EXIT_SUCCESS;
}
