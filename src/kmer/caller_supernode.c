#include "global.h"
#include "caller_supernode.h"
#include "db_graph.h"
#include "db_node.h"
#include "supernode.h"

#ifdef DEBUG
#define DEBUG_CALLER 1
#endif

// Orient supernode
static void supernode_naturalise(hkey_t *nlist, Orientation *olist, size_t len)
{
  // Sort supernode into forward orientation
  if(len == 1)
    olist[0] = FORWARD;
  else if(nlist[0] > nlist[len-1] ||
          (nlist[0] == nlist[len-1] && olist[0] > olist[len-1]))
  {
    supernode_reverse(nlist, olist, len);
  }
}

// Create a supernode starting at node/or.  Store in snode.
// Ensure snode->nodes and snode->orients point to valid memory before passing
// Returns 0 on failure, otherwise snode->num_of_nodes
size_t caller_supernode_create(hkey_t node, Orientation orient,
                               CallerSupernode *snode, const dBGraph *db_graph)
{
  assert(db_graph->num_edge_cols == 1);

  Nucleotide nuc;
  Edges union_edges;
  hkey_t first_node, last_node;
  Orientation first_or, last_or;
  BinaryKmer bkmer;

  #ifdef DEBUG_CALLER
    char tmpstr[MAX_KMER_SIZE+1];
    bkmer = db_node_bkmer(db_graph, node);
    binary_kmer_to_str(bkmer, db_graph->kmer_size, tmpstr);
    printf(" create %s:%i\n", tmpstr, (int)orient);
  #endif

  // extend path
  boolean iscycle;

  CallerNodeBuf *nbuf = snode->nbuf;

  if(nbuf->len + 1 >= nbuf->cap) {
    nbuf->cap *= 2;
    nbuf->nodes = realloc2(nbuf->nodes, nbuf->cap * sizeof(hkey_t));
    nbuf->orients = realloc2(nbuf->orients, nbuf->cap * sizeof(Orientation));
  }

  snode->nbuf_offset = nbuf->len;
  nbuf->nodes[snode->nbuf_offset] = node;
  nbuf->orients[snode->nbuf_offset] = orient;

  nbuf->len = supernode_extend(db_graph, &nbuf->nodes,
                               &nbuf->orients, snode->nbuf_offset,
                               &iscycle, &nbuf->cap, true);

  snode->num_of_nodes = nbuf->len - snode->nbuf_offset;

  hkey_t *nodes = snode_nodes(snode);
  Orientation *orients = snode_orients(snode);
  supernode_naturalise(nodes, orients, snode->num_of_nodes);

  snode->first_pathpos = NULL;
  snode->num_prev = 0;
  snode->num_next = 0;

  first_node = nodes[0];
  first_or = opposite_orientation(orients[0]);
  last_node = nodes[snode->num_of_nodes-1];
  last_or = orients[snode->num_of_nodes-1];

  // Prev nodes
  union_edges = db_node_edges(db_graph, first_node);
  union_edges = edges_with_orientation(union_edges, first_or);
  bkmer = db_node_bkmer(db_graph, first_node);

  for(nuc = 0; nuc < 4; nuc++) {
    if(edges_has_edge(union_edges, nuc, FORWARD)) {
      db_graph_next_node(db_graph, bkmer, nuc, first_or,
                         snode->prev_nodes + snode->num_prev,
                         snode->prev_orients + snode->num_prev);
      snode->num_prev++;
    }
  }

  // Next nodes
  union_edges = db_node_edges(db_graph, last_node);
  union_edges = edges_with_orientation(union_edges, last_or);
  bkmer = db_node_bkmer(db_graph, last_node);

  for(nuc = 0; nuc < 4; nuc++) {
    if(edges_has_edge(union_edges, nuc, FORWARD)) {
      db_graph_next_node(db_graph, bkmer, nuc, last_or,
                         snode->next_nodes + snode->num_next,
                         snode->next_orients + snode->num_next);
      snode->num_next++;
    }
  }

  #ifdef DEBUG_CALLER
    char tmpstr1[MAX_KMER_SIZE+1], tmpstr2[MAX_KMER_SIZE+1];
    BinaryKmer first_bkmer = db_node_bkmer(db_graph, first_node);
    BinaryKmer last_bkmer = db_node_bkmer(db_graph, last_node);
    binary_kmer_to_str(first_bkmer, db_graph->kmer_size, tmpstr1);
    binary_kmer_to_str(last_bkmer, db_graph->kmer_size, tmpstr2);
    printf("   ( [>%i] first:%s:%i; len:%zu last:%s:%i [%i<] )\n",
           snode->num_prev, tmpstr1, (int)first_or, snode->num_of_nodes,
           tmpstr2, (int)last_or, snode->num_next);
  #endif

  return snode->num_of_nodes;
}

uint64_t supernode_pathpos_hash(SupernodePathPos *spp)
{
  uint32_t hsh, len = spp->pos + 1;
  size_t snode_size = sizeof(CallerSupernode*) * len;
  size_t sorients_size = sizeof(SuperOrientation) * len;

#ifdef CITY_HASH
  // Use Google's CityHash
  hsh = CityHash32((char*)spp->path->supernodes, snode_size);
  hsh ^= CityHash32((char*)spp->path->superorients, sorients_size);
#else
  // Use Bob Jenkin's lookup3
  hsh = hashlittle(spp->path->supernodes, snode_size, 0);
  hsh = hashlittle(spp->path->superorients, sorients_size, hsh);
#endif

  return hsh;
}

// Two SupernodePathPos objects are equal if they describe the same path through
// the graph.  Sort by length (shortest first), then orientation and nodes
int cmp_snpath_pos(const void *p1, const void *p2)
{
  const SupernodePathPos * const pp1 = p1;
  const SupernodePathPos * const pp2 = p2;

  ptrdiff_t cmp = (ptrdiff_t)pp1->pos - pp2->pos;

  if(cmp != 0)
    return cmp;

  int i, len = pp1->pos;
  for(i = 0; i < len; i++)
  {
    cmp = pp1->path->superorients[i] - pp2->path->superorients[i];

    if(cmp != 0)
      return cmp;

    cmp = pp1->path->supernodes[i] - pp2->path->supernodes[i];

    if(cmp != 0)
      return (cmp > 0 ? 1 : -1);
  }

  return 0;
}