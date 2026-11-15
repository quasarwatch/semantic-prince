/*
 * struct.c - spg struct: fill skeleton templates (A = word runs, D = digit
 * runs, S = symbol runs), most probable first.  Port of src/genstruct.py
 * with bugfixes_v1/v2 semantics.
 *
 * Parity contract (results/lab2/c_port_plan.md 5.2): C always uses the
 * deterministic filler construction that genstruct.py reproduces under
 * SP_FILLER_FULLSORT=1:
 *   - bylen: word indexes of a length sorted by (count desc, index desc)
 *     (== idx[np.argsort(counts[idx], kind="stable")[::-1]][:TOP_SRC]);
 *   - pair sublattices: full sort by (score desc, flat index asc),
 *     top min(4*TOP_FILL, size) (== np.argsort(-flat, kind="stable")).
 * The python pickle filler cache is neither read nor written; fillers are
 * built natively (elementwise pjm on the 400x400 sublattices only, which is
 * bitwise the same as the full-matrix numpy computation per element).
 *
 * Replicated semantics:
 *   - budget mode: per-round quotas ceil(LIMIT * c/total) over live (c>0)
 *     templates; only really emitted candidates burn quota (got += emit());
 *     the candidate pulled before the quota check is discarded (python
 *     for-loop pulls next() before the body runs); exhausted templates leave
 *     the live set; all-zero counts -> 0 lines, exit 0 (H1).
 *   - global mode: heapq.merge(key=(s + log(c/tpl_total)), reverse=True);
 *     cpython 3.10 stores [key, -order] in a max-heap, so exact score ties
 *     leave in ascending template order (verified empirically; plan N14
 *     suspected the opposite).  Templates with c == 0 are skipped.
 *     NOTE (latent python quirk, replicated): the genexprs capture `off`
 *     late-bound, so every stream is shifted by the offset of the LAST
 *     included template -- a constant, hence the template prior has no
 *     effect on the merge order in the reference implementation.
 *   - HeapGen: lazy exact best-first k-way merge, incremental scores
 *     s' = (s - old) + new, seen-set of index tuples, heap order
 *     (-score, index tuple lexicographic).
 *   - GridGen: broadcast left-fold score grid, order (score desc, flat asc)
 *     (== np.argsort(-flat, kind="stable")), lazy build.
 *   - --no-dedup is filtered from any argv slot before parsing (B6).
 */

#define _GNU_SOURCE   /* qsort_r */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#include "model.h"
#include "struct.h"

#define TOP_SRC   400u
#define TOP_FILL  600u
#define GRID_MAX  2000000u
#define MAXL      16u

/* ------------------------------------------------------------------ */
/* utilities                                                           */

static void die(const char *msg)
{
  fprintf(stderr, "spg struct: %s\n", msg);
  exit(1);
}

static void *xmalloc(size_t n)
{
  void *p = malloc(n ? n : 1);

  if (!p)
    die("out of memory");

  return p;
}

static void *xrealloc(void *p, size_t n)
{
  void *q = realloc(p, n ? n : 1);

  if (!q)
    die("out of memory");

  return q;
}

static uint64_t fnv1a(const void *data, size_t n)
{
  const uint8_t *p = data;
  uint64_t h = 14695981039346656037ull;

  for (size_t i = 0; i < n; i++)
  {
    h ^= p[i];
    h *= 1099511628211ull;
  }

  return h;
}

/* ------------------------------------------------------------------ */
/* segment lists (filler / digit / symbol run values)                  */

typedef struct
{
  const uint8_t *p;
  uint32_t       len;
} sstr;

typedef struct
{
  sstr    *strs;
  double  *scores;
  uint32_t n;
  uint32_t maxlen;
} seglist;

/* ------------------------------------------------------------------ */
/* filler accumulator: bytes -> max score, first-insertion order kept  */
/* (python dict with `if key not in d or s > d[key]: d[key] = s`)      */

typedef struct
{
  uint64_t h;
  double   score;
  uint32_t seq;
  uint32_t off;
  uint32_t len;
} fent;

typedef struct
{
  int       present;
  fent     *ents;
  uint32_t  n, cap;
  uint32_t *tab;        /* ent index + 1, 0 = empty */
  uint32_t  tcap;
  uint8_t  *arena;
  uint64_t  alen, acap;
} fmap;

static void fmap_init(fmap *f)
{
  memset(f, 0, sizeof(*f));
  f->tcap = 4096;
  f->tab  = xmalloc(f->tcap * sizeof(uint32_t));
  memset(f->tab, 0, f->tcap * sizeof(uint32_t));
  f->cap   = 1024;
  f->ents  = xmalloc(f->cap * sizeof(fent));
  f->acap  = 1 << 16;
  f->arena = xmalloc(f->acap);
}

static void fmap_grow(fmap *f)
{
  uint32_t  ncap = f->tcap * 2;
  uint32_t *nt   = xmalloc(ncap * sizeof(uint32_t));

  memset(nt, 0, ncap * sizeof(uint32_t));

  for (uint32_t i = 0; i < f->tcap; i++)
  {
    uint32_t e = f->tab[i];

    if (e)
    {
      uint32_t pos = (uint32_t) (f->ents[e - 1].h & (ncap - 1));

      while (nt[pos])
        pos = (pos + 1) & (ncap - 1);

      nt[pos] = e;
    }
  }

  free(f->tab);
  f->tab  = nt;
  f->tcap = ncap;
}

static void fmap_add(fmap *f, const uint8_t *key, uint32_t len, double score)
{
  uint64_t h   = fnv1a(key, len);
  uint32_t pos = (uint32_t) (h & (f->tcap - 1));

  while (f->tab[pos])
  {
    fent *e = &f->ents[f->tab[pos] - 1];

    if (e->h == h && e->len == len && !memcmp(f->arena + e->off, key, len))
    {
      if (score > e->score)
        e->score = score;   /* position (seq) kept, as in a python dict */

      return;
    }

    pos = (pos + 1) & (f->tcap - 1);
  }

  if ((f->n + 1) * 4 > (uint32_t) f->tcap * 3)
  {
    fmap_grow(f);
    pos = (uint32_t) (h & (f->tcap - 1));

    while (f->tab[pos])
      pos = (pos + 1) & (f->tcap - 1);
  }

  if (f->n == f->cap)
  {
    f->cap *= 2;
    f->ents = xrealloc(f->ents, f->cap * sizeof(fent));
  }

  if (f->alen + len > f->acap)
  {
    while (f->alen + len > f->acap)
      f->acap *= 2;

    f->arena = xrealloc(f->arena, f->acap);
  }

  fent *e  = &f->ents[f->n];
  e->h     = h;
  e->score = score;
  e->seq   = f->n;
  e->off   = (uint32_t) f->alen;
  e->len   = len;

  memcpy(f->arena + f->alen, key, len);
  f->alen += len;

  f->tab[pos] = f->n + 1;
  f->n++;
}

/* ------------------------------------------------------------------ */
/* sort contexts (qsort_r)                                             */

static int cmp_bylen(const void *pa, const void *pb, void *ctx)
{
  const int64_t *counts = ctx;
  uint32_t a = *(const uint32_t *) pa;
  uint32_t b = *(const uint32_t *) pb;

  if (counts[a] != counts[b])
    return counts[a] > counts[b] ? -1 : 1;    /* count desc */

  return a > b ? -1 : (a < b ? 1 : 0);        /* index desc */
}

static int cmp_flat(const void *pa, const void *pb, void *ctx)
{
  const double *sc = ctx;
  uint32_t a = *(const uint32_t *) pa;
  uint32_t b = *(const uint32_t *) pb;

  if (sc[a] != sc[b])
    return sc[a] > sc[b] ? -1 : 1;            /* score desc */

  return a < b ? -1 : (a > b ? 1 : 0);        /* flat index asc */
}

static int cmp_fent(const void *pa, const void *pb, void *ctx)
{
  const fent *ents = ctx;
  uint32_t a = *(const uint32_t *) pa;
  uint32_t b = *(const uint32_t *) pb;

  if (ents[a].score != ents[b].score)
    return ents[a].score > ents[b].score ? -1 : 1;  /* score desc */

  return ents[a].seq < ents[b].seq ? -1
       : ents[a].seq > ents[b].seq ? 1 : 0;         /* insertion asc */
}

/* ------------------------------------------------------------------ */
/* bigram count lookup (i, j) -> cnt                                   */

typedef struct
{
  uint64_t *keys;       /* UINT64_MAX = empty */
  int64_t  *cnts;
  uint64_t  cap;
} bihash;

static void bihash_init(bihash *b, uint64_t n)
{
  uint64_t cap = 16;

  while (cap < n * 2)
    cap *= 2;

  b->cap  = cap;
  b->keys = xmalloc(cap * sizeof(uint64_t));
  b->cnts = xmalloc(cap * sizeof(int64_t));

  memset(b->keys, 0xff, cap * sizeof(uint64_t));
}

static void bihash_put(bihash *b, uint32_t i, uint32_t j, int64_t cnt)
{
  uint64_t key = ((uint64_t) i << 32) | j;
  uint64_t pos = (key * 11400714819323198485ull) & (b->cap - 1);

  while (b->keys[pos] != UINT64_MAX)
  {
    if (b->keys[pos] == key)
    {
      b->cnts[pos] = cnt;   /* last write wins, as M[i, j] = cnt */
      return;
    }

    pos = (pos + 1) & (b->cap - 1);
  }

  b->keys[pos] = key;
  b->cnts[pos] = cnt;
}

static int64_t bihash_get(const bihash *b, uint32_t i, uint32_t j)
{
  uint64_t key = ((uint64_t) i << 32) | j;
  uint64_t pos = (key * 11400714819323198485ull) & (b->cap - 1);

  while (b->keys[pos] != UINT64_MAX)
  {
    if (b->keys[pos] == key)
      return b->cnts[pos];

    pos = (pos + 1) & (b->cap - 1);
  }

  return 0;
}

/* ------------------------------------------------------------------ */
/* fillers                                                             */

static seglist g_fillers[MAXL + 1];
static int     g_fillers_present[MAXL + 1];

static void build_fillers(const sp_model *m, double alpha)
{
  const uint32_t K = m->K;

  /* exact integer total: all counts < 2^53, so the int64 sum equals the
   * numpy float64 pairwise sum bit for bit */
  int64_t csum = 0;

  for (uint32_t i = 0; i < K; i++)
    csum += m->counts[i];

  const double dsum = (double) csum;

  /* bylen[l]: top TOP_SRC word indexes of length l,
   * (count desc, index desc) == argsort(counts, stable)[::-1] */
  uint32_t *bylen[11];
  uint32_t  bylen_n[11];
  uint32_t *tmp = xmalloc((K ? K : 1) * sizeof(uint32_t));

  for (uint32_t l = 2; l <= 10; l++)
  {
    uint32_t n = 0;

    for (uint32_t i = 0; i < K; i++)
      if (m->wlens[i] == l)
        tmp[n++] = i;

    qsort_r(tmp, n, sizeof(uint32_t), cmp_bylen, (void *) m->counts);

    if (n > TOP_SRC)
      n = TOP_SRC;

    bylen[l] = xmalloc((n ? n : 1) * sizeof(uint32_t));
    memcpy(bylen[l], tmp, n * sizeof(uint32_t));
    bylen_n[l] = n;
  }

  free(tmp);

  bihash bh;

  bihash_init(&bh, m->E);

  for (uint64_t e = 0; e < m->E; e++)
    bihash_put(&bh, (uint32_t) m->bi_i[e], (uint32_t) m->bi_j[e],
               m->bi_cnt[e]);

  static fmap fm[MAXL + 1];

  for (uint32_t l = 0; l <= MAXL; l++)
    fmap_init(&fm[l]);

  /* singles, in word-index order */
  for (uint32_t i = 0; i < K; i++)
  {
    const uint32_t L = m->wlens[i];

    fm[L].present = 1;
    fmap_add(&fm[L], (const uint8_t *) m->words[i], m->wlens[i], m->logu[i]);
  }

  /* pairs over the length sublattices */
  for (uint32_t l1 = 2; l1 <= 10; l1++)
  {
    for (uint32_t l2 = 2; l2 <= 10; l2++)
    {
      const uint32_t L = l1 + l2;

      if (L > MAXL)
        continue;

      fm[L].present = 1;   /* setdefault happens even for empty lattices */

      const uint32_t n1 = bylen_n[l1];
      const uint32_t n2 = bylen_n[l2];

      if (!n1 || !n2)
        continue;

      const uint64_t sz = (uint64_t) n1 * n2;
      double        *sc = xmalloc(sz * sizeof(double));

      for (uint32_t x = 0; x < n1; x++)
      {
        const uint32_t wi = bylen[l1][x];
        const double   lw = m->logu[wi];
        const double   cf = (double) m->counts[wi];

        for (uint32_t y = 0; y < n2; y++)
        {
          const uint32_t wj  = bylen[l2][y];
          const int64_t  cnt = bihash_get(&bh, wi, wj);
          const double   pjm = alpha * ((double) cnt / cf)
                             + (1.0 - alpha) * ((double) m->counts[wj] / dsum);

          sc[(uint64_t) x * n2 + y] = lw + log(fmax(pjm, 1e-15));
        }
      }

      uint32_t *ord = xmalloc(sz * sizeof(uint32_t));

      for (uint64_t o = 0; o < sz; o++)
        ord[o] = (uint32_t) o;

      qsort_r(ord, sz, sizeof(uint32_t), cmp_flat, sc);

      const uint64_t keep = sz < (uint64_t) TOP_FILL * 4
                          ? sz : (uint64_t) TOP_FILL * 4;

      for (uint64_t t = 0; t < keep; t++)
      {
        const uint32_t o  = ord[t];
        const uint32_t wi = bylen[l1][o / n2];
        const uint32_t wj = bylen[l2][o % n2];

        if (wi == wj)
          continue;

        uint8_t key[32];

        memcpy(key, m->words[wi], m->wlens[wi]);
        memcpy(key + m->wlens[wi], m->words[wj], m->wlens[wj]);

        fmap_add(&fm[L], key, m->wlens[wi] + m->wlens[wj], sc[o]);
      }

      free(ord);
      free(sc);
    }
  }

  /* finalize: stable sort by score desc (insertion order on ties), top 600 */
  for (uint32_t L = 0; L <= MAXL; L++)
  {
    g_fillers_present[L] = fm[L].present;

    if (!fm[L].present)
      continue;

    uint32_t *ord = xmalloc((fm[L].n ? fm[L].n : 1) * sizeof(uint32_t));

    for (uint32_t i = 0; i < fm[L].n; i++)
      ord[i] = i;

    qsort_r(ord, fm[L].n, sizeof(uint32_t), cmp_fent, fm[L].ents);

    const uint32_t n = fm[L].n < TOP_FILL ? fm[L].n : TOP_FILL;
    seglist       *sl = &g_fillers[L];

    sl->n      = n;
    sl->strs   = xmalloc((n ? n : 1) * sizeof(sstr));
    sl->scores = xmalloc((n ? n : 1) * sizeof(double));
    sl->maxlen = 0;

    for (uint32_t i = 0; i < n; i++)
    {
      const fent *e = &fm[L].ents[ord[i]];

      sl->strs[i].p   = fm[L].arena + e->off;   /* arena outlives us */
      sl->strs[i].len = e->len;
      sl->scores[i]   = e->score;

      if (e->len > sl->maxlen)
        sl->maxlen = e->len;
    }

    free(ord);
    free(fm[L].tab);
    free(fm[L].ents);
  }

  free(bh.keys);
  free(bh.cnts);

  for (uint32_t l = 2; l <= 10; l++)
    free(bylen[l]);
}

/* ------------------------------------------------------------------ */
/* digit / symbol run values                                           */

typedef struct
{
  uint32_t  len;
  int       present;
  seglist   sl;
  /* accumulation during parse: v[].p holds an OFFSET into arena (fixed up
   * to a real pointer in finalize, after the arena's last realloc) */
  sstr     *v;
  int64_t  *c;
  uint32_t  n, cap;
  int64_t   total;
  uint8_t  *arena;
  uint64_t  alen, acap;
} runval;

typedef struct
{
  runval  *rv;
  uint32_t n, cap;
} runvals;

static runval *runval_find(runvals *r, uint32_t len);  /* fwd */

static void load_run_values(const char *path, double gamma, runvals *r)
{
  FILE *f = fopen(path, "r");

  if (!f)
    die("cannot open run-values file");

  char  *line = NULL;
  size_t lcap = 0;
  ssize_t n;

  while ((n = getline(&line, &lcap, f)) > 0)
  {
    if (n > 0 && line[n - 1] == '\n')
      line[--n] = 0;

    /* python: l, v, c = line.split("\t") -> exactly two tabs */
    char *t1 = strchr(line, '\t');
    char *t2 = t1 ? strchr(t1 + 1, '\t') : NULL;

    if (!t1 || !t2 || strchr(t2 + 1, '\t'))
      die("malformed digits/symbols line");

    *t1 = 0;
    *t2 = 0;

    char     *end = NULL;
    const long l = strtol(line, &end, 10);

    if (end == line)
      die("bad run length");

    const int64_t c = strtoll(t2 + 1, &end, 10);

    if (end == t2 + 1)
      die("bad run count");

    runval *rv = runval_find(r, (uint32_t) l);

    if (!rv)
    {
      if (r->n == r->cap)
      {
        r->cap = r->cap ? r->cap * 2 : 16;
        r->rv  = xrealloc(r->rv, r->cap * sizeof(runval));
      }

      rv = &r->rv[r->n++];
      memset(rv, 0, sizeof(*rv));
      rv->len     = (uint32_t) l;
      rv->present = 1;
      rv->cap     = 64;
      rv->v       = xmalloc(rv->cap * sizeof(sstr));
      rv->c       = xmalloc(rv->cap * sizeof(int64_t));
      rv->acap    = 1 << 12;
      rv->arena   = xmalloc(rv->acap);
    }

    if (rv->n == rv->cap)
    {
      rv->cap *= 2;
      rv->v = xrealloc(rv->v, rv->cap * sizeof(sstr));
      rv->c = xrealloc(rv->c, rv->cap * sizeof(int64_t));
    }

    const uint32_t vlen = (uint32_t) strlen(t1 + 1);

    if (rv->alen + vlen > rv->acap)
    {
      while (rv->alen + vlen > rv->acap)
        rv->acap *= 2;

      rv->arena = xrealloc(rv->arena, rv->acap);
    }

    memcpy(rv->arena + rv->alen, t1 + 1, vlen);
    rv->v[rv->n].p   = (const uint8_t *) (uintptr_t) rv->alen;  /* offset */
    rv->v[rv->n].len = vlen;
    rv->c[rv->n]     = c;
    rv->n++;
    rv->alen        += vlen;
    rv->total       += c;
  }

  free(line);
  fclose(f);

  /* scores: math.log(c / total) * GAMMA, file order kept */
  for (uint32_t k = 0; k < r->n; k++)
  {
    runval *rv = &r->rv[k];

    rv->sl.n      = rv->n;
    rv->sl.strs   = xmalloc((rv->n ? rv->n : 1) * sizeof(sstr));
    rv->sl.scores = xmalloc((rv->n ? rv->n : 1) * sizeof(double));
    rv->sl.maxlen = 0;

    for (uint32_t i = 0; i < rv->n; i++)
    {
      rv->sl.strs[i].p   = rv->arena + (uintptr_t) rv->v[i].p;
      rv->sl.strs[i].len = rv->v[i].len;
      rv->sl.scores[i] = log((double) rv->c[i] / (double) rv->total) * gamma;

      if (rv->v[i].len > rv->sl.maxlen)
        rv->sl.maxlen = rv->v[i].len;
    }

    free(rv->v);
    free(rv->c);
    rv->v = NULL;
    rv->c = NULL;
  }
}

static runval *runval_find(runvals *r, uint32_t len)
{
  for (uint32_t i = 0; i < r->n; i++)
    if (r->rv[i].len == len)
      return &r->rv[i];

  return NULL;
}

static const seglist *runval_get(const runvals *r, uint32_t len)
{
  for (uint32_t k = 0; k < r->n; k++)
    if (r->rv[k].len == len)
      return &r->rv[k].sl;

  return NULL;
}

/* ------------------------------------------------------------------ */
/* templates                                                           */

typedef struct
{
  int64_t   c;
  uint32_t  nsegs;
  seglist **segs;
} tpl;

/* regex ([ADS])(\d+): scan left to right, non-overlapping */
static const seglist *seg_lookup(char cls, uint32_t ln,
                                 const runvals *dig, const runvals *sym)
{
  if (cls == 'A')
  {
    if (ln <= MAXL && g_fillers_present[ln])
      return &g_fillers[ln];

    return NULL;
  }

  return runval_get(cls == 'D' ? dig : sym, ln);
}

static uint64_t load_templates(const char *path, int64_t ntpl_max,
                               const runvals *dig, const runvals *sym,
                               tpl **out)
{
  FILE *f = fopen(path, "r");

  if (!f)
    die("cannot open templates file");

  uint64_t ntpl = 0, cap = 16;
  tpl     *tpls = xmalloc(cap * sizeof(tpl));

  char  *line = NULL;
  size_t lcap = 0;
  ssize_t n;

  while ((n = getline(&line, &lcap, f)) > 0)
  {
    if (n > 0 && line[n - 1] == '\n')
      line[--n] = 0;

    /* python: t, c = line.split("\t") -> exactly one tab */
    char *tab = strchr(line, '\t');

    if (!tab || strchr(tab + 1, '\t'))
      die("malformed template line");

    const char  *t    = line;
    const size_t tlen = (size_t) (tab - line);

    seglist **segs  = xmalloc((tlen / 2 + 1) * sizeof(seglist *));
    uint32_t nsegs  = 0;
    int      ok     = 1;

    for (size_t i = 0; i < tlen;)
    {
      const char ch = t[i];

      if ((ch == 'A' || ch == 'D' || ch == 'S')
       && i + 1 < tlen && t[i + 1] >= '0' && t[i + 1] <= '9')
      {
        size_t j = i + 1;

        while (j < tlen && t[j] >= '0' && t[j] <= '9')
          j++;

        char     *end = NULL;
        const long ln = strtol(t + i + 1, &end, 10);
        const seglist *sl;

        (void) end;
        sl = seg_lookup(ch, ln < 0 ? UINT32_MAX : (uint32_t) ln, dig, sym);

        if (!sl)
        {
          ok = 0;
          break;
        }

        segs[nsegs++] = (seglist *) sl;
        i = j;
      }
      else
        i++;
    }

    if (ok)
    {
      char *end = NULL;

      const int64_t c = strtoll(tab + 1, &end, 10);

      if (end == tab + 1)
        die("bad template count");

      if (ntpl == cap)
      {
        cap  *= 2;
        tpls  = xrealloc(tpls, cap * sizeof(tpl));
      }

      tpls[ntpl].c     = c;
      tpls[ntpl].nsegs = nsegs;
      tpls[ntpl].segs  = segs;
      ntpl++;
    }
    else
      free(segs);

    if ((int64_t) ntpl == ntpl_max)
      break;
  }

  free(line);
  fclose(f);

  *out = tpls;

  return ntpl;
}

/* ------------------------------------------------------------------ */
/* per-template candidate streams (GridGen / HeapGen)                  */

typedef struct
{
  int       is_heap;
  uint32_t  nsegs;
  seglist **segs;
  uint64_t  space;          /* grid only: product of segment sizes */

  uint8_t  *cand;
  uint32_t  cand_cap;
  uint32_t  cand_len;

  uint32_t *scratch;        /* [nsegs] index scratch */

  /* grid state (lazy) */
  double   *g_sc;
  uint32_t *g_order;
  uint64_t  g_pos;
  int       g_ready;

  /* heap state */
  double   *h_neg;
  uint32_t *h_tup;
  uint64_t  h_n, h_cap;
  uint32_t *tups;           /* tuple arena, nsegs uint32 per tuple */
  uint64_t  tups_n, tups_cap;
  uint64_t *seen_h;         /* 0 = empty */
  uint32_t *seen_i;         /* tuple index + 1 */
  uint64_t  seen_cap, seen_n;
} gen;

static int heap_cmp(const gen *g, double aneg, uint32_t at,
                    double bneg, uint32_t bt)
{
  if (aneg != bneg)
    return aneg < bneg ? -1 : 1;

  const uint32_t *a = g->tups + (uint64_t) at * g->nsegs;
  const uint32_t *b = g->tups + (uint64_t) bt * g->nsegs;

  for (uint32_t k = 0; k < g->nsegs; k++)
    if (a[k] != b[k])
      return a[k] < b[k] ? -1 : 1;

  return 0;
}

static void heap_push(gen *g, double neg, uint32_t tno)
{
  if (g->h_n == g->h_cap)
  {
    g->h_cap *= 2;
    g->h_neg  = xrealloc(g->h_neg, g->h_cap * sizeof(double));
    g->h_tup  = xrealloc(g->h_tup, g->h_cap * sizeof(uint32_t));
  }

  uint64_t pos = g->h_n++;

  g->h_neg[pos] = neg;
  g->h_tup[pos] = tno;

  while (pos > 0)
  {
    const uint64_t par = (pos - 1) >> 1;

    if (heap_cmp(g, g->h_neg[pos], g->h_tup[pos],
                 g->h_neg[par], g->h_tup[par]) >= 0)
      break;

    double   tn = g->h_neg[pos];
    uint32_t tt = g->h_tup[pos];

    g->h_neg[pos] = g->h_neg[par];
    g->h_tup[pos] = g->h_tup[par];
    g->h_neg[par] = tn;
    g->h_tup[par] = tt;
    pos = par;
  }
}

static void heap_pop(gen *g, double *neg, uint32_t *tno)
{
  *neg = g->h_neg[0];
  *tno = g->h_tup[0];

  const double   ln = g->h_neg[--g->h_n];
  const uint32_t lt = g->h_tup[g->h_n];

  if (g->h_n == 0)
    return;

  uint64_t pos = 0;

  for (;;)
  {
    uint64_t child = 2 * pos + 1;

    if (child >= g->h_n)
      break;

    if (child + 1 < g->h_n
     && heap_cmp(g, g->h_neg[child + 1], g->h_tup[child + 1],
                 g->h_neg[child], g->h_tup[child]) < 0)
      child++;

    if (heap_cmp(g, ln, lt, g->h_neg[child], g->h_tup[child]) <= 0)
      break;

    g->h_neg[pos] = g->h_neg[child];
    g->h_tup[pos] = g->h_tup[child];
    pos = child;
  }

  g->h_neg[pos] = ln;
  g->h_tup[pos] = lt;
}

/* returns 1 and the new tuple index if the tuple was not seen before */
static int heap_offer_seen(gen *g, const uint32_t *tup, uint32_t *tno_out)
{
  uint64_t h = fnv1a(tup, g->nsegs * sizeof(uint32_t)) | 1;

  if ((g->seen_n + 1) * 10 > g->seen_cap * 7)
  {
    const uint64_t ncap   = g->seen_cap * 2;
    uint64_t      *nh     = xmalloc(ncap * sizeof(uint64_t));
    uint32_t      *ni     = xmalloc(ncap * sizeof(uint32_t));

    memset(nh, 0, ncap * sizeof(uint64_t));

    for (uint64_t i = 0; i < g->seen_cap; i++)
    {
      if (g->seen_h[i])
      {
        uint64_t pos = g->seen_h[i] & (ncap - 1);

        while (nh[pos])
          pos = (pos + 1) & (ncap - 1);

        nh[pos] = g->seen_h[i];
        ni[pos] = g->seen_i[i];
      }
    }

    free(g->seen_h);
    free(g->seen_i);
    g->seen_h   = nh;
    g->seen_i   = ni;
    g->seen_cap = ncap;
  }

  uint64_t pos = h & (g->seen_cap - 1);

  while (g->seen_h[pos])
  {
    if (g->seen_h[pos] == h)
    {
      const uint32_t *old = g->tups
                          + (uint64_t) (g->seen_i[pos] - 1) * g->nsegs;

      if (!memcmp(old, tup, g->nsegs * sizeof(uint32_t)))
        return 0;
    }

    pos = (pos + 1) & (g->seen_cap - 1);
  }

  if (g->tups_n == g->tups_cap)
  {
    g->tups_cap *= 2;
    g->tups      = xrealloc(g->tups,
                            g->tups_cap * g->nsegs * sizeof(uint32_t));
  }

  memcpy(g->tups + g->tups_n * g->nsegs, tup, g->nsegs * sizeof(uint32_t));

  g->seen_h[pos] = h;
  g->seen_i[pos] = (uint32_t) g->tups_n + 1;
  g->seen_n++;

  *tno_out = (uint32_t) g->tups_n;
  g->tups_n++;

  return 1;
}

static void gen_heap_init(gen *g)
{
  g->h_cap    = 1024;
  g->h_neg    = xmalloc(g->h_cap * sizeof(double));
  g->h_tup    = xmalloc(g->h_cap * sizeof(uint32_t));
  g->tups_cap = 1024;
  g->tups     = xmalloc(g->tups_cap * g->nsegs * sizeof(uint32_t));
  g->seen_cap = 1024;
  g->seen_h   = xmalloc(g->seen_cap * sizeof(uint64_t));
  g->seen_i   = xmalloc(g->seen_cap * sizeof(uint32_t));

  memset(g->seen_h, 0, g->seen_cap * sizeof(uint64_t));

  double s0 = 0.0;

  for (uint32_t k = 0; k < g->nsegs; k++)
    s0 += g->segs[k]->scores[0];

  memset(g->tups, 0, g->nsegs * sizeof(uint32_t));

  uint32_t tno;

  heap_offer_seen(g, g->tups, &tno);   /* root tuple, always new */
  heap_push(g, -s0, tno);
}

static gen *gen_new(seglist **segs, uint32_t nsegs)
{
  gen *g = xmalloc(sizeof(*g));

  memset(g, 0, sizeof(*g));

  g->nsegs   = nsegs;
  g->segs    = segs;
  g->scratch = xmalloc((nsegs ? nsegs : 1) * sizeof(uint32_t));

  uint64_t prod = 1;
  int      heap = 0;

  for (uint32_t k = 0; k < nsegs; k++)
  {
    prod *= segs[k]->n;

    if (prod > GRID_MAX)
    {
      heap = 1;
      break;
    }
  }

  g->is_heap = heap;
  g->space   = heap ? 0 : prod;

  uint32_t cap = 1;

  for (uint32_t k = 0; k < nsegs; k++)
    cap += segs[k]->maxlen;

  g->cand     = xmalloc(cap);
  g->cand_cap = cap;

  if (heap)
    gen_heap_init(g);

  return g;
}

static void gen_build_cand(gen *g, const uint32_t *idx)
{
  uint8_t *w = g->cand;

  for (uint32_t k = 0; k < g->nsegs; k++)
  {
    const sstr *s = &g->segs[k]->strs[idx[k]];

    memcpy(w, s->p, s->len);
    w += s->len;
  }

  g->cand_len = (uint32_t) (w - g->cand);
}

static void grid_build(gen *g)
{
  if (!g->nsegs)
    die("template with no segments");   /* python: IndexError in GridGen */

  const uint64_t space = g->space;

  g->g_sc = xmalloc((space ? space : 1) * sizeof(double));

  uint32_t *idx = g->scratch;

  memset(idx, 0, g->nsegs * sizeof(uint32_t));

  for (uint64_t o = 0; o < space; o++)
  {
    /* broadcast left fold: ((s0 + s1) + s2) + ... */
    double s = g->segs[0]->scores[idx[0]];

    for (uint32_t k = 1; k < g->nsegs; k++)
      s += g->segs[k]->scores[idx[k]];

    g->g_sc[o] = s;

    /* odometer, last dimension fastest (C order ravel) */
    uint32_t k = g->nsegs;

    while (k-- > 0)
    {
      if (++idx[k] < g->segs[k]->n)
        break;

      idx[k] = 0;
    }
  }

  g->g_order = xmalloc((space ? space : 1) * sizeof(uint32_t));

  for (uint64_t o = 0; o < space; o++)
    g->g_order[o] = (uint32_t) o;

  /* np.argsort(-flat, kind="stable") == (score desc, flat index asc) */
  qsort_r(g->g_order, space, sizeof(uint32_t), cmp_flat, g->g_sc);

  g->g_pos   = 0;
  g->g_ready = 1;
}

static int gen_next(gen *g, double *score)
{
  if (!g->is_heap)
  {
    if (!g->g_ready)
      grid_build(g);

    if (g->g_pos >= g->space)
      return 0;

    const uint64_t o = g->g_order[g->g_pos++];
    uint64_t       t = o;

    for (uint32_t k = g->nsegs; k-- > 0;)
    {
      g->scratch[k] = (uint32_t) (t % g->segs[k]->n);
      t            /= g->segs[k]->n;
    }

    gen_build_cand(g, g->scratch);
    *score = g->g_sc[o];

    return 1;
  }

  if (!g->h_n)
    return 0;

  double   neg;
  uint32_t tno;

  heap_pop(g, &neg, &tno);

  /* the tuple arena may be reallocated by offers below: copy the popped
   * tuple out first */
  memcpy(g->scratch, g->tups + (uint64_t) tno * g->nsegs,
         g->nsegs * sizeof(uint32_t));

  gen_build_cand(g, g->scratch);
  *score = -neg;

  for (uint32_t k = 0; k < g->nsegs; k++)
  {
    const uint32_t nk = g->scratch[k] + 1;

    if (nk >= g->segs[k]->n)
      continue;

    const uint32_t old = g->scratch[k];
    uint32_t       newtno;

    g->scratch[k] = nk;

    if (heap_offer_seen(g, g->scratch, &newtno))
    {
      /* s = -negscore - seg[idx] + seg[nk], in this order */
      const double s = (-neg - g->segs[k]->scores[old])
                     + g->segs[k]->scores[nk];

      heap_push(g, -s, newtno);
    }

    g->scratch[k] = old;
  }

  return 1;
}

/* ------------------------------------------------------------------ */
/* dedup set + output                                                  */

typedef struct
{
  uint64_t *h;          /* 0 = empty */
  uint32_t *off;
  uint32_t *len;
  uint64_t  cap, n;
  uint8_t  *arena;
  uint64_t  alen, acap;
} dset;

static int      g_dedup = 1;
static dset     g_ds;
static uint64_t g_emitted;

static void dset_init(dset *d, uint64_t limit)
{
  uint64_t cap = 1 << 16;

  while (cap * 3 < (limit > 0 ? (uint64_t) limit : 1) * 4)
    cap *= 2;

  d->cap  = cap;
  d->h    = xmalloc(cap * sizeof(uint64_t));
  d->off  = xmalloc(cap * sizeof(uint32_t));
  d->len  = xmalloc(cap * sizeof(uint32_t));
  d->acap = 1 << 20;
  d->arena = xmalloc(d->acap);

  memset(d->h, 0, cap * sizeof(uint64_t));
}

static int dset_add(dset *d, const uint8_t *key, uint32_t len)
{
  const uint64_t h = fnv1a(key, len) | 1;

  if ((d->n + 1) * 4 > d->cap * 3)
  {
    const uint64_t ncap  = d->cap * 2;
    uint64_t      *nh    = xmalloc(ncap * sizeof(uint64_t));
    uint32_t      *noff  = xmalloc(ncap * sizeof(uint32_t));
    uint32_t      *nlen  = xmalloc(ncap * sizeof(uint32_t));

    memset(nh, 0, ncap * sizeof(uint64_t));

    for (uint64_t i = 0; i < d->cap; i++)
    {
      if (d->h[i])
      {
        uint64_t pos = d->h[i] & (ncap - 1);

        while (nh[pos])
          pos = (pos + 1) & (ncap - 1);

        nh[pos]   = d->h[i];
        noff[pos] = d->off[i];
        nlen[pos] = d->len[i];
      }
    }

    free(d->h);
    free(d->off);
    free(d->len);
    d->h   = nh;
    d->off = noff;
    d->len = nlen;
    d->cap = ncap;
  }

  uint64_t pos = h & (d->cap - 1);

  while (d->h[pos])
  {
    if (d->h[pos] == h && d->len[pos] == len
     && !memcmp(d->arena + d->off[pos], key, len))
      return 0;

    pos = (pos + 1) & (d->cap - 1);
  }

  if (d->alen + len > d->acap)
  {
    while (d->alen + len > d->acap)
      d->acap *= 2;

    d->arena = xrealloc(d->arena, d->acap);
  }

  memcpy(d->arena + d->alen, key, len);

  d->h[pos]   = h;
  d->off[pos] = (uint32_t) d->alen;
  d->len[pos] = len;
  d->alen    += len;
  d->n++;

  return 1;
}

static uint8_t g_outbuf[1 << 23];
static size_t  g_outn;

static void out_flush(void)
{
  if (g_outn)
  {
    fwrite(g_outbuf, 1, g_outn, stdout);
    g_outn = 0;
  }
}

/* returns 1 when the candidate was really emitted (0 = dedup hit) */
static int emit(const uint8_t *cand, uint32_t len)
{
  if (g_dedup && !dset_add(&g_ds, cand, len))
    return 0;

  if (g_outn + len + 1 > sizeof(g_outbuf))
    out_flush();

  memcpy(g_outbuf + g_outn, cand, len);
  g_outn += len;
  g_outbuf[g_outn++] = '\n';
  g_emitted++;

  return 1;
}

/* ------------------------------------------------------------------ */
/* budget mode                                                         */

static void run_budget(const tpl *tpls, uint64_t ntpl, int64_t limit)
{
  gen **gens = xmalloc((ntpl ? ntpl : 1) * sizeof(gen *));

  for (uint64_t t = 0; t < ntpl; t++)
    gens[t] = gen_new(tpls[t].segs, tpls[t].nsegs);

  uint8_t  *alive = xmalloc((ntpl ? ntpl : 1));
  uint64_t *live  = xmalloc((ntpl ? ntpl : 1) * sizeof(uint64_t));
  uint64_t *snap  = xmalloc((ntpl ? ntpl : 1) * sizeof(uint64_t));
  uint64_t  nlive = 0;

  memset(alive, 0, ntpl ? ntpl : 1);

  /* zero-count templates get no quota share (H1) */
  for (uint64_t t = 0; t < ntpl; t++)
    if (tpls[t].c > 0)
    {
      alive[t]      = 1;
      live[nlive++] = t;
    }

  while (nlive && (int64_t) g_emitted < limit)
  {
    int64_t total = 0;

    for (uint64_t i = 0; i < nlive; i++)
      total += tpls[live[i]].c;

    const uint64_t nsnap = nlive;

    memcpy(snap, live, nlive * sizeof(uint64_t));

    for (uint64_t s = 0; s < nsnap; s++)
    {
      const uint64_t t = snap[s];

      if ((int64_t) g_emitted >= limit)
        break;

      /* ceil(LIMIT * (c / total)), float64 like python */
      const uint64_t budget = (uint64_t) ceil((double) limit
                            * ((double) tpls[t].c / (double) total));
      uint64_t got = 0;
      double   sc;

      for (;;)
      {
        if (!gen_next(gens[t], &sc))
          break;

        /* python pulls next() before checking the quota: the pulled
         * candidate is discarded on break */
        if (got >= budget || (int64_t) g_emitted >= limit)
          break;

        got += (uint64_t) emit(gens[t]->cand, gens[t]->cand_len);
      }

      if (got < budget && (int64_t) g_emitted < limit)
        alive[t] = 0;   /* generator ran dry: share redistributed */
    }

    nlive = 0;

    for (uint64_t t = 0; t < ntpl; t++)
      if (alive[t])
        live[nlive++] = t;
  }
}

/* ------------------------------------------------------------------ */
/* global mode: heapq.merge(key = s + log(c/tpl_total), reverse=True)  */

typedef struct
{
  double   key;
  int64_t  negord;    /* order * direction, direction = -1 (reverse) */
  uint64_t gi;
} mnode;

static int mnode_gt(const mnode *a, const mnode *b)
{
  if (a->key != b->key)
    return a->key > b->key;

  return a->negord > b->negord;
}

static void merge_push(mnode *h, uint64_t *hn, mnode v)
{
  uint64_t pos = (*hn)++;

  h[pos] = v;

  while (pos > 0)
  {
    const uint64_t par = (pos - 1) >> 1;

    if (!mnode_gt(&h[pos], &h[par]))
      break;

    mnode t = h[pos];

    h[pos] = h[par];
    h[par] = t;
    pos    = par;
  }
}

static void merge_siftdown(mnode *h, uint64_t hn, uint64_t pos)
{
  const mnode v = h[pos];

  for (;;)
  {
    uint64_t child = 2 * pos + 1;

    if (child >= hn)
      break;

    if (child + 1 < hn && mnode_gt(&h[child + 1], &h[child]))
      child++;

    if (!mnode_gt(&h[child], &v))
      break;

    h[pos] = h[child];
    pos    = child;
  }

  h[pos] = v;
}

static void run_global(const tpl *tpls, uint64_t ntpl, int64_t limit)
{
  int64_t tpl_total = 0;

  for (uint64_t t = 0; t < ntpl; t++)
    tpl_total += tpls[t].c;

  /* genstruct.py builds the per-template genexprs `(s + off, cand)` in a
   * loop but heapq.merge consumes them only afterwards: `off` is late-bound,
   * so EVERY stream is shifted by the offset of the LAST included template
   * (a constant shift; the template prior has no effect on the merge order).
   * Replicated literally for parity. */
  double   off_last = 0.0;
  gen    **gg  = xmalloc((ntpl ? ntpl : 1) * sizeof(gen *));
  mnode   *h   = xmalloc((ntpl ? ntpl : 1) * sizeof(mnode));
  uint64_t hn  = 0;
  uint64_t G   = 0;

  for (uint64_t t = 0; t < ntpl; t++)
  {
    if (tpls[t].c == 0 || tpl_total == 0)
      continue;   /* zero prior: never wins the merge, log(0) undefined */

    off_last = log((double) tpls[t].c / (double) tpl_total);
    gg[G]    = gen_new(tpls[t].segs, tpls[t].nsegs);
    G++;
  }

  /* python pulls the first candidate of every genexpr only when the merge
   * starts, i.e. after the loop: all keys use the final `off` */
  for (uint64_t g = 0; g < G; g++)
  {
    double s;

    if (gen_next(gg[g], &s))
    {
      mnode v;

      v.key    = s + off_last;
      v.negord = -(int64_t) g;
      v.gi     = g;

      merge_push(h, &hn, v);
    }
  }

  while (hn > 1)
  {
    const uint64_t gi = h[0].gi;

    if ((int64_t) g_emitted >= limit)
      break;

    emit(gg[gi]->cand, gg[gi]->cand_len);

    double s;

    if (gen_next(gg[gi], &s))
    {
      h[0].key = s + off_last;
      merge_siftdown(h, hn, 0);
    }
    else
    {
      /* heappop_max */
      h[0] = h[--hn];
      merge_siftdown(h, hn, 0);
    }
  }

  if (hn == 1)
  {
    gen *g = gg[h[0].gi];

    if ((int64_t) g_emitted < limit)
      emit(g->cand, g->cand_len);

    double s;

    while ((int64_t) g_emitted < limit && gen_next(g, &s))
      emit(g->cand, g->cand_len);
  }

  free(gg);
  free(h);
}

/* ------------------------------------------------------------------ */
/* entry point                                                         */

int cmd_struct(int argc, char **argv)
{
  /* filter --no-dedup from any slot before positional parsing (B6) */
  char **pos  = xmalloc((size_t) (argc + 1) * sizeof(char *));
  int    npos = 0;

  for (int i = 0; i < argc; i++)
  {
    if (!strcmp(argv[i], "--no-dedup"))
      g_dedup = 0;
    else
      pos[npos++] = argv[i];
  }

  if (npos < 7)
  {
    fprintf(stderr,
            "usage: spg struct UNI BI TPL DIG SYM K LIMIT [alpha]"
            " [budget|global] [ntpl] [--no-dedup]\n");
    return 2;
  }

  const char *uni  = pos[0];
  const char *bi   = pos[1];
  const char *tphp = pos[2];
  const char *digp = pos[3];
  const char *symp = pos[4];

  const uint32_t K    = (uint32_t) strtoul(pos[5], NULL, 10);
  const int64_t  limit = strtoll(pos[6], NULL, 10);
  const double   alpha = npos > 7 ? strtod(pos[7], NULL) : 0.7;
  const char    *mode  = npos > 8 ? pos[8] : "budget";
  const int64_t  ntpl  = npos > 9 ? strtoll(pos[9], NULL, 10) : 300;

  double gamma = 1.0;

  const char *ge = getenv("GENSTRUCT_GAMMA");

  if (ge)
    gamma = strtod(ge, NULL);

  sp_model m;

  if (sp_model_load(&m, uni, bi, K, 2, 10) != 0)
    die("failed to load vocabulary model");

  build_fillers(&m, alpha);

  runvals dig = { NULL, 0, 0 };
  runvals sym = { NULL, 0, 0 };

  load_run_values(digp, gamma, &dig);
  load_run_values(symp, gamma, &sym);

  tpl    *tpls = NULL;
  uint64_t ntpl_n = load_templates(tphp, ntpl, &dig, &sym, &tpls);

  if (g_dedup)
    dset_init(&g_ds, (uint64_t) (limit > 0 ? limit : 0));

  if (!strcmp(mode, "global"))
    run_global(tpls, ntpl_n, limit);
  else
    run_budget(tpls, ntpl_n, limit);

  out_flush();
  fprintf(stderr, "emitted: %llu\n", (unsigned long long) g_emitted);

  sp_model_free(&m);

  return 0;
}
