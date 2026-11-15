/* spg targeted - targeted attack mode.
 *
 * Exact port of src/targeted.py (bugfixes_v1/v2 state):
 *   - service variants (--service=NAME) are injected with the top word's
 *     count (boost) and go FIRST in the personal list (B5);
 *   - personal words (--personal=FILE) are read with errors="ignore",
 *     stripped (unicode whitespace), deduplicated keeping order
 *     (dict.fromkeys), optionally de-leet expanded (@/4->a, 0->o, 3->e,
 *     1/!->i, $/5->s, 7->t, 8->b; original AND normalized form);
 *   - injected words extend the vocabulary: K, logu and u are recomputed
 *     AFTER all injections;
 *   - test slice: password fully alpha (unicode str.isalpha), starts/ends
 *     with a personal word, remainder >= 3 CODEPOINTS; hits are registered
 *     for ALL splits, first pwid per hitkey wins (B4, setdefault);
 *   - candidate scores for personal*K*2 pairs (lm JM bigram / freq),
 *     stable argsort by -score (ties: ascending candidate index),
 *     cumulative unique hits at checkpoints 100K/1M/10M/50M;
 *   - --emit mode streams w+x / x+w candidates (stable sort by -score,
 *     first-occurrence byte dedup).
 *
 * Unicode: UTF-8 aware. isalpha/strip use tables generated from the
 * reference CPython (tools/gen_utables.py); input is decoded with
 * errors="ignore" semantics (maximal-subpart skipping, as CPython).
 * Files are read with universal-newline splitting (\n, \r\n, \r) like
 * python text mode.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#include "model.h"
#include "targeted.h"
#include "unicode_tables.h"

#define ALPHA 0.7

#define I64_EMPTY (-1)          /* hitkey/pwid are always >= 0 */
#define U32_EMPTY 0xffffffffu

#define CHECKPOINT_N 4
static const int64_t CHECKPOINTS[CHECKPOINT_N] =
  { 100000, 1000000, 10000000, 50000000 };

/* ------------------------------------------------------------------ */
/* UTF-8 with CPython errors="ignore" semantics                        */
/* ------------------------------------------------------------------ */

/* Decode one codepoint. Returns sequence length (1..4) and sets *cp,
 * or 0 for an invalid sequence and sets *skip to the number of bytes
 * CPython's "ignore" handler would drop (maximal subpart rule). */
static int utf8_decode(const uint8_t *s, size_t n, int32_t *cp, size_t *skip)
{
  const uint8_t b0 = s[0];

  if (b0 < 0x80)
  {
    *cp = b0;
    return 1;
  }

  int      need;
  uint32_t code, lo1 = 0x80, hi1 = 0xBF;

  if (b0 >= 0xC2 && b0 <= 0xDF)
  {
    need = 2;
    code = b0 & 0x1Fu;
  }
  else if (b0 >= 0xE0 && b0 <= 0xEF)
  {
    need = 3;
    code = b0 & 0x0Fu;
    if (b0 == 0xE0)
      lo1 = 0xA0;
    if (b0 == 0xED)
      hi1 = 0x9F;             /* exclude surrogates */
  }
  else if (b0 >= 0xF0 && b0 <= 0xF4)
  {
    need = 4;
    code = b0 & 0x07u;
    if (b0 == 0xF0)
      lo1 = 0x90;
    if (b0 == 0xF4)
      hi1 = 0x8F;
  }
  else
  {
    *skip = 1;                /* stray continuation, C0/C1, F5..FF */
    return 0;
  }

  for (int k = 1; k < need; k++)
  {
    if ((size_t) k >= n)
    {
      *skip = n;              /* truncated at end: drop the whole prefix */
      return 0;
    }

    const uint8_t b  = s[k];
    const uint8_t lo = (k == 1) ? (uint8_t) lo1 : 0x80;
    const uint8_t hi = (k == 1) ? (uint8_t) hi1 : 0xBF;

    if (b < lo || b > hi)
    {
      *skip = (size_t) k;     /* maximal valid prefix */
      return 0;
    }

    code = (code << 6) | (uint32_t) (b & 0x3Fu);
  }

  *cp = (int32_t) code;

  return need;
}

/* In-place errors="ignore" cleanup; returns the new (shorter) length. */
static size_t utf8_clean(uint8_t *s, size_t n)
{
  size_t r = 0, w = 0;

  while (r < n)
  {
    int32_t cp;
    size_t  skip;
    const int l = utf8_decode(s + r, n - r, &cp, &skip);

    if (l > 0)
    {
      memmove(s + w, s + r, (size_t) l);
      w += (size_t) l;
      r += (size_t) l;
    }
    else
      r += skip;
  }

  return w;
}

static int uni_isalpha(int32_t cp)
{
  uint32_t lo = 0, hi = SP_UCD_ALPHA_NRANGES;

  while (lo < hi)
  {
    const uint32_t mid = lo + (hi - lo) / 2;

    if ((uint32_t) cp < sp_ucd_alpha[mid].lo)
      hi = mid;
    else if ((uint32_t) cp > sp_ucd_alpha[mid].hi)
      lo = mid + 1;
    else
      return 1;
  }

  return 0;
}

static int uni_isspace(int32_t cp)
{
  uint32_t lo = 0, hi = SP_UCD_SPACE_N;

  while (lo < hi)
  {
    const uint32_t mid = lo + (hi - lo) / 2;

    if ((uint32_t) cp < sp_ucd_space[mid])
      hi = mid;
    else if ((uint32_t) cp > sp_ucd_space[mid])
      lo = mid + 1;
    else
      return 1;
  }

  return 0;
}

/* str.isalpha() over a clean UTF-8 string; also counts codepoints. */
static int utf8_alpha_cplen(const uint8_t *s, size_t n, size_t *cpn)
{
  size_t i = 0, c = 0;

  if (n == 0)
    return 0;

  while (i < n)
  {
    int32_t cp;
    size_t  skip;
    const int l = utf8_decode(s + i, n - i, &cp, &skip);

    if (l <= 0 || !uni_isalpha(cp))
      return 0;

    i += (size_t) l;
    c++;
  }

  *cpn = c;

  return 1;
}

/* codepoint count of a clean UTF-8 string */
static size_t utf8_cplen(const uint8_t *s, size_t n)
{
  size_t i = 0, c = 0;

  while (i < n)
  {
    int32_t cp;
    size_t  skip;
    const int l = utf8_decode(s + i, n - i, &cp, &skip);

    if (l > 0)
    {
      i += (size_t) l;
      c++;
    }
    else
      i += skip;              /* cannot happen on cleaned input */
  }

  return c;
}

/* str.strip() (unicode whitespace) on a clean UTF-8 string */
static void utf8_strip(const uint8_t *s, size_t n, size_t *off, size_t *len)
{
  size_t a = 0, b = n;

  while (a < b)
  {
    int32_t cp;
    size_t  skip;
    const int l = utf8_decode(s + a, b - a, &cp, &skip);

    if (l <= 0 || !uni_isspace(cp))
      break;

    a += (size_t) l;
  }

  while (b > a)
  {
    size_t start = b - 1;

    while (start > a && (s[start] & 0xC0) == 0x80)
      start--;

    int32_t cp;
    size_t  skip;
    const int l = utf8_decode(s + start, b - start, &cp, &skip);

    if (l <= 0 || !uni_isspace(cp))
      break;

    b = start;
  }

  *off = a;
  *len = b - a;
}

/* ------------------------------------------------------------------ */
/* small dynamic byte-string list (refs or owned copies)               */
/* ------------------------------------------------------------------ */

typedef struct
{
  const uint8_t **p;
  uint32_t       *l;
  uint32_t        n, cap;
  uint8_t       **own;          /* individually owned copies */
  uint32_t        n_own, cap_own;
} wlist;

static int wlist_grow(wlist *ws)
{
  if (ws->n < ws->cap)
    return 0;

  const uint32_t ncap = ws->cap ? ws->cap * 2 : 16;

  const uint8_t **np = realloc(ws->p, ncap * sizeof(uint8_t *));
  uint32_t       *nl = realloc(ws->l, ncap * sizeof(uint32_t));

  if (!np || !nl)
  {
    free(np ? np : ws->p);
    free(nl ? nl : ws->l);
    ws->p = NULL;
    ws->l = NULL;
    return -1;
  }

  ws->p = np;
  ws->l = nl;
  ws->cap = ncap;

  return 0;
}

static int wlist_push_ref(wlist *ws, const uint8_t *p, uint32_t l)
{
  if (wlist_grow(ws) != 0)
    return -1;

  ws->p[ws->n] = p;
  ws->l[ws->n] = l;
  ws->n++;

  return 0;
}

/* owned copy: individually allocated (pointers stay valid, no arena
 * realloc invalidation) */
static int wlist_push_own(wlist *ws, const uint8_t *p, uint32_t l)
{
  if (ws->n_own == ws->cap_own)
  {
    const uint32_t ncap = ws->cap_own ? ws->cap_own * 2 : 16;

    uint8_t **no = realloc(ws->own, ncap * sizeof(uint8_t *));

    if (!no)
      return -1;

    ws->own = no;
    ws->cap_own = ncap;
  }

  uint8_t *copy = malloc(l ? l : 1);

  if (!copy)
    return -1;

  memcpy(copy, p, l);
  ws->own[ws->n_own++] = copy;

  if (wlist_push_ref(ws, copy, l) != 0)
    return -1;

  return 0;
}

static void wlist_free(wlist *ws)
{
  for (uint32_t i = 0; i < ws->n_own; i++)
    free(ws->own[i]);

  free(ws->own);
  free(ws->p);
  free(ws->l);
  memset(ws, 0, sizeof(*ws));
}

/* ------------------------------------------------------------------ */
/* byte-string hash set (no ownership), insertion check only           */
/* ------------------------------------------------------------------ */

typedef struct
{
  const uint8_t **p;
  uint32_t       *l;
  uint64_t       *h;
  uint32_t        cap;          /* power of two */
  uint32_t        n;
} sset;

static uint64_t fnv1a(const uint8_t *p, size_t n)
{
  uint64_t h = 14695981039346656037ull;

  for (size_t i = 0; i < n; i++)
  {
    h ^= p[i];
    h *= 1099511628211ull;
  }

  return h;
}

static int sset_init(sset *s, uint32_t cap)
{
  if (cap < 16)
    cap = 16;

  /* mask-based probing requires a power-of-two capacity */
  cap--;
  cap |= cap >> 1;
  cap |= cap >> 2;
  cap |= cap >> 4;
  cap |= cap >> 8;
  cap |= cap >> 16;
  cap++;

  s->p = calloc(cap, sizeof(uint8_t *));
  s->l = malloc(cap * sizeof(uint32_t));
  s->h = malloc(cap * sizeof(uint64_t));

  if (!s->p || !s->l || !s->h)
    return -1;

  s->cap = cap;
  s->n = 0;

  return 0;
}

static void sset_free(sset *s)
{
  free(s->p);
  free(s->l);
  free(s->h);
  memset(s, 0, sizeof(*s));
}

static int sset_contains(const sset *s, const uint8_t *p, uint32_t l,
                         uint64_t h, uint32_t *slot)
{
  uint32_t i = (uint32_t) (h & (s->cap - 1));

  while (s->p[i])
  {
    if (s->h[i] == h && s->l[i] == l && !memcmp(s->p[i], p, l))
    {
      *slot = i;
      return 1;
    }

    i = (i + 1) & (s->cap - 1);
  }

  *slot = i;

  return 0;
}

static int sset_grow(sset *s)
{
  sset t;

  if (sset_init(&t, s->cap * 2) != 0)
    return -1;

  for (uint32_t i = 0; i < s->cap; i++)
  {
    if (!s->p[i])
      continue;

    uint32_t slot;

    sset_contains(&t, s->p[i], s->l[i], s->h[i], &slot);
    t.p[slot] = s->p[i];
    t.l[slot] = s->l[i];
    t.h[slot] = s->h[i];
    t.n++;
  }

  sset_free(s);
  *s = t;

  return 0;
}

/* returns 1 if newly inserted, 0 if already present, -1 on OOM */
static int sset_add(sset *s, const uint8_t *p, uint32_t l)
{
  if ((s->n + 1) * 10 > s->cap * 7 && sset_grow(s) != 0)
    return -1;

  const uint64_t h = fnv1a(p, l);
  uint32_t       slot;

  if (sset_contains(s, p, l, h, &slot))
    return 0;

  s->p[slot] = p;
  s->l[slot] = l;
  s->h[slot] = h;
  s->n++;

  return 1;
}

/* ------------------------------------------------------------------ */
/* int64 -> int64 map (hitkey -> pwid), setdefault semantics           */
/* ------------------------------------------------------------------ */

typedef struct
{
  int64_t *k;
  int64_t *v;
  int64_t  cap;                 /* power of two */
  int64_t  n;
} i64map;

static int i64map_init(i64map *m, int64_t cap)
{
  if (cap < 16)
    cap = 16;

  m->k = malloc((size_t) cap * sizeof(int64_t));
  m->v = malloc((size_t) cap * sizeof(int64_t));

  if (!m->k || !m->v)
    return -1;

  for (int64_t i = 0; i < cap; i++)
    m->k[i] = I64_EMPTY;

  m->cap = cap;
  m->n = 0;

  return 0;
}

static void i64map_free(i64map *m)
{
  free(m->k);
  free(m->v);
  m->k = NULL;
  m->v = NULL;
  m->cap = 0;
  m->n = 0;
}

static uint64_t i64_hash(int64_t x)
{
  uint64_t z = (uint64_t) x;

  z ^= z >> 33;
  z *= 0xff51afd7ed558ccdull;
  z ^= z >> 33;

  return z;
}

static int i64map_grow(i64map *m)
{
  i64map t;

  if (i64map_init(&t, m->cap * 2) != 0)
    return -1;

  for (int64_t i = 0; i < m->cap; i++)
  {
    if (m->k[i] == I64_EMPTY)
      continue;

    int64_t slot = (int64_t) (i64_hash(m->k[i]) & (uint64_t) (t.cap - 1));

    while (t.k[slot] != I64_EMPTY)
      slot = (slot + 1) & (t.cap - 1);

    t.k[slot] = m->k[i];
    t.v[slot] = m->v[i];
    t.n++;
  }

  i64map_free(m);
  *m = t;

  return 0;
}

static int i64map_setdefault(i64map *m, int64_t key, int64_t val)
{
  if ((m->n + 1) * 10 > m->cap * 7 && i64map_grow(m) != 0)
    return -1;

  int64_t slot = (int64_t) (i64_hash(key) & (uint64_t) (m->cap - 1));

  while (m->k[slot] != I64_EMPTY)
  {
    if (m->k[slot] == key)
      return 0;               /* existing entry kept */

    slot = (slot + 1) & (m->cap - 1);
  }

  m->k[slot] = key;
  m->v[slot] = val;
  m->n++;

  return 0;
}

/* returns 1 and *val when found */
static int i64map_get(const i64map *m, int64_t key, int64_t *val)
{
  int64_t slot = (int64_t) (i64_hash(key) & (uint64_t) (m->cap - 1));

  while (m->k[slot] != I64_EMPTY)
  {
    if (m->k[slot] == key)
    {
      *val = m->v[slot];
      return 1;
    }

    slot = (slot + 1) & (m->cap - 1);
  }

  return 0;
}

/* int64 set (hit pwids) */
typedef struct
{
  int64_t *k;
  int64_t  cap;
  int64_t  n;
} i64set;

static int i64set_init(i64set *s, int64_t cap)
{
  if (cap < 16)
    cap = 16;

  s->k = malloc((size_t) cap * sizeof(int64_t));

  if (!s->k)
    return -1;

  for (int64_t i = 0; i < cap; i++)
    s->k[i] = I64_EMPTY;

  s->cap = cap;
  s->n = 0;

  return 0;
}

static void i64set_free(i64set *s)
{
  free(s->k);
  s->k = NULL;
  s->cap = 0;
  s->n = 0;
}

static int i64set_grow(i64set *s)
{
  i64set t;

  if (i64set_init(&t, s->cap * 2) != 0)
    return -1;

  for (int64_t i = 0; i < s->cap; i++)
  {
    if (s->k[i] == I64_EMPTY)
      continue;

    int64_t slot = (int64_t) (i64_hash(s->k[i]) & (uint64_t) (t.cap - 1));

    while (t.k[slot] != I64_EMPTY)
      slot = (slot + 1) & (t.cap - 1);

    t.k[slot] = s->k[i];
    t.n++;
  }

  i64set_free(s);
  *s = t;

  return 0;
}

static int i64set_add(i64set *s, int64_t key)
{
  if ((s->n + 1) * 10 > s->cap * 7 && i64set_grow(s) != 0)
    return -1;

  int64_t slot = (int64_t) (i64_hash(key) & (uint64_t) (s->cap - 1));

  while (s->k[slot] != I64_EMPTY)
  {
    if (s->k[slot] == key)
      return 0;

    slot = (slot + 1) & (s->cap - 1);
  }

  s->k[slot] = key;
  s->n++;

  return 1;
}

/* ------------------------------------------------------------------ */
/* extendable vocabulary: model words + injected words                 */
/* ------------------------------------------------------------------ */

typedef struct
{
  const uint8_t **w;            /* byte strings (model blob or inj[]) */
  uint32_t       *wl;
  int64_t        *cnt;
  uint32_t        n, cap;
  uint32_t       *htab;         /* word -> index, U32_EMPTY */
  uint32_t        hcap;
  uint8_t       **inj;          /* individually owned injected words */
  uint32_t        n_inj, cap_inj;
} tvoc;

static int tvoc_htab_insert(tvoc *v, uint32_t idx)
{
  uint32_t i = (uint32_t) (fnv1a(v->w[idx], v->wl[idx]) & (v->hcap - 1));

  while (v->htab[i] != U32_EMPTY)
    i = (i + 1) & (v->hcap - 1);

  v->htab[i] = idx;

  return 0;
}

static int tvoc_htab_rebuild(tvoc *v, uint32_t hcap)
{
  free(v->htab);
  v->htab = malloc(hcap * sizeof(uint32_t));

  if (!v->htab)
    return -1;

  v->hcap = hcap;

  for (uint32_t i = 0; i < hcap; i++)
    v->htab[i] = U32_EMPTY;

  for (uint32_t i = 0; i < v->n; i++)
    tvoc_htab_insert(v, i);

  return 0;
}

static int tvoc_init(tvoc *v, const sp_model *m)
{
  memset(v, 0, sizeof(*v));

  v->cap = m->K + 64;
  v->w   = malloc(v->cap * sizeof(uint8_t *));
  v->wl  = malloc(v->cap * sizeof(uint32_t));
  v->cnt = malloc(v->cap * sizeof(int64_t));

  if (!v->w || !v->wl || !v->cnt)
    return -1;

  for (uint32_t i = 0; i < m->K; i++)
  {
    v->w[i]   = (const uint8_t *) m->words[i];
    v->wl[i]  = m->wlens[i];
    v->cnt[i] = m->counts[i];
  }

  v->n = m->K;

  uint32_t hcap = 16;

  while (hcap < m->K * 2)
    hcap *= 2;

  return tvoc_htab_rebuild(v, hcap);
}

static void tvoc_free(tvoc *v)
{
  for (uint32_t i = 0; i < v->n_inj; i++)
    free(v->inj[i]);

  free(v->inj);
  free(v->w);
  free(v->wl);
  free(v->cnt);
  free(v->htab);
  memset(v, 0, sizeof(*v));
}

static int64_t tvoc_find(const tvoc *v, const uint8_t *w, uint32_t len)
{
  uint32_t i = (uint32_t) (fnv1a(w, len) & (v->hcap - 1));

  while (v->htab[i] != U32_EMPTY)
  {
    const uint32_t idx = v->htab[i];

    if (v->wl[idx] == len && !memcmp(v->w[idx], w, len))
      return (int64_t) idx;

    i = (i + 1) & (v->hcap - 1);
  }

  return -1;
}

/* append a word known to be absent */
static int tvoc_add(tvoc *v, const uint8_t *w, uint32_t l, int64_t cnt)
{
  if (v->n == v->cap)
  {
    const uint32_t ncap = v->cap * 2;

    const uint8_t **nw = realloc(v->w,   ncap * sizeof(uint8_t *));
    uint32_t       *nl = realloc(v->wl,  ncap * sizeof(uint32_t));
    int64_t        *nc = realloc(v->cnt, ncap * sizeof(int64_t));

    if (!nw || !nl || !nc)
    {
      free(nw ? nw : v->w);
      free(nl ? nl : v->wl);
      free(nc ? nc : v->cnt);
      v->w = NULL;
      v->wl = NULL;
      v->cnt = NULL;
      return -1;
    }

    v->w = nw;
    v->wl = nl;
    v->cnt = nc;
    v->cap = ncap;
  }

  if (v->n_inj == v->cap_inj)
  {
    const uint32_t ncap = v->cap_inj ? v->cap_inj * 2 : 16;

    uint8_t **ni = realloc(v->inj, ncap * sizeof(uint8_t *));

    if (!ni)
      return -1;

    v->inj = ni;
    v->cap_inj = ncap;
  }

  uint8_t *copy = malloc(l ? l : 1);

  if (!copy)
    return -1;

  memcpy(copy, w, l);
  v->inj[v->n_inj++] = copy;

  const uint32_t idx = v->n;

  v->w[idx] = copy;
  v->wl[idx] = l;
  v->cnt[idx] = cnt;
  v->n++;

  if (v->n * 10 > v->hcap * 7)
  {
    if (tvoc_htab_rebuild(v, v->hcap * 2) != 0)
      return -1;
  }
  else
    tvoc_htab_insert(v, idx);

  return (int) idx;
}

/* inject(): find or append with TOPCNT; boost also upgrades existing */
static int tvoc_inject(tvoc *v, const wlist *ws, int boost, int64_t topcnt,
                       uint32_t *out, uint32_t *n_out)
{
  uint32_t k = 0;

  for (uint32_t i = 0; i < ws->n; i++)
  {
    int64_t idx = tvoc_find(v, ws->p[i], ws->l[i]);

    if (idx < 0)
    {
      idx = tvoc_add(v, ws->p[i], ws->l[i], topcnt);

      if (idx < 0)
        return -1;
    }
    else if (boost)
      v->cnt[idx] = topcnt;

    out[k++] = (uint32_t) idx;
  }

  *n_out = k;

  return 0;
}

/* ------------------------------------------------------------------ */
/* stable mergesort, descending key, ties keep original index order    */
/* (equals np.argsort(-x, kind="stable"))                              */
/* ------------------------------------------------------------------ */

static void isort_desc(uint32_t *a, uint32_t *t, const double *key, uint64_t n)
{
  uint32_t *src = a, *dst = t;

  for (uint64_t i = 0; i < n; i++)
    src[i] = (uint32_t) i;

  for (uint64_t w = 1; w < n; w <<= 1)
  {
    for (uint64_t lo = 0; lo < n; lo += 2 * w)
    {
      const uint64_t mid = (lo + w < n) ? lo + w : n;
      const uint64_t hi = (lo + 2 * w < n) ? lo + 2 * w : n;
      uint64_t i = lo, j = mid, k = lo;

      while (i < mid && j < hi)
      {
        if (key[src[i]] >= key[src[j]])
          dst[k++] = src[i++];
        else
          dst[k++] = src[j++];
      }

      while (i < mid)
        dst[k++] = src[i++];

      while (j < hi)
        dst[k++] = src[j++];
    }

    uint32_t *sw = src;
    src = dst;
    dst = sw;
  }

  if (src != a)
    memcpy(a, src, (size_t) n * sizeof(uint32_t));
}

/* emit-mode candidate */
typedef struct
{
  double   s;
  uint32_t a, b;                /* candidate = word a + word b */
} cand_t;

static void candsort_desc(cand_t *a, cand_t *t, uint64_t n)
{
  cand_t *src = a, *dst = t;

  for (uint64_t w = 1; w < n; w <<= 1)
  {
    for (uint64_t lo = 0; lo < n; lo += 2 * w)
    {
      const uint64_t mid = (lo + w < n) ? lo + w : n;
      const uint64_t hi = (lo + 2 * w < n) ? lo + 2 * w : n;
      uint64_t i = lo, j = mid, k = lo;

      while (i < mid && j < hi)
      {
        if (src[i].s >= src[j].s)
          dst[k++] = src[i++];
        else
          dst[k++] = src[j++];
      }

      while (i < mid)
        dst[k++] = src[i++];

      while (j < hi)
        dst[k++] = src[j++];
    }

    cand_t *sw = src;
    src = dst;
    dst = sw;
  }

  if (src != a)
    memcpy(a, src, (size_t) n * sizeof(cand_t));
}

/* ------------------------------------------------------------------ */
/* buffered stdout writer                                              */
/* ------------------------------------------------------------------ */

typedef struct
{
  uint8_t *buf;
  size_t   len, cap;
} obuf;

static int obuf_init(obuf *o, size_t cap)
{
  o->buf = malloc(cap);

  if (!o->buf)
    return -1;

  o->len = 0;
  o->cap = cap;

  return 0;
}

static int obuf_write(obuf *o, const void *p, size_t n)
{
  if (o->len + n > o->cap)
  {
    if (o->len && fwrite(o->buf, 1, o->len, stdout) != o->len)
      return -1;

    o->len = 0;

    if (n > o->cap)
      return fwrite(p, 1, n, stdout) == n ? 0 : -1;
  }

  memcpy(o->buf + o->len, p, n);
  o->len += n;

  return 0;
}

static int obuf_flush(obuf *o)
{
  if (o->len && fwrite(o->buf, 1, o->len, stdout) != o->len)
    return -1;

  o->len = 0;

  return fflush(stdout) == 0 ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* misc helpers                                                        */
/* ------------------------------------------------------------------ */

static uint8_t *read_all(const char *path, size_t *out_n)
{
  FILE *f = fopen(path, "rb");

  if (!f)
    return NULL;

  size_t   cap = 1u << 20, n = 0;
  uint8_t *b = malloc(cap);

  if (!b)
  {
    fclose(f);
    return NULL;
  }

  for (;;)
  {
    if (n == cap)
    {
      cap *= 2;

      uint8_t *nb = realloc(b, cap);

      if (!nb)
      {
        free(b);
        fclose(f);
        return NULL;
      }

      b = nb;
    }

    const size_t got = fread(b + n, 1, cap - n, f);

    n += got;

    if (got == 0)
      break;
  }

  fclose(f);
  *out_n = n;

  return b;
}

/* python universal-newline line iterator over a memory buffer:
 * lines end at \n, \r\n or \r; a final unterminated line is yielded */
static int next_line(const uint8_t *buf, size_t n, size_t *pos,
                     size_t *loff, size_t *llen)
{
  size_t p = *pos;

  if (p >= n)
    return 0;

  const size_t start = p;

  while (p < n && buf[p] != '\n' && buf[p] != '\r')
    p++;

  *loff = start;
  *llen = p - start;

  if (p < n)
  {
    if (buf[p] == '\r' && p + 1 < n && buf[p + 1] == '\n')
      p++;

    p++;
  }

  *pos = p;

  return 1;
}

/* CPython repr() of a byte string, ASCII-exact; non-ASCII bytes are
 * emitted as \xNN (python would show printable unicode chars raw --
 * documented divergence, CLI args and word lists are ASCII in practice) */
static void py_repr(FILE *f, const uint8_t *s, size_t n)
{
  int has_sq = 0, has_dq = 0;

  for (size_t i = 0; i < n; i++)
  {
    if (s[i] == '\'')
      has_sq = 1;
    else if (s[i] == '"')
      has_dq = 1;
  }

  const int q = (has_sq && !has_dq) ? '"' : '\'';

  fputc(q, f);

  for (size_t i = 0; i < n; i++)
  {
    const uint8_t b = s[i];

    if (b == (uint8_t) q || b == '\\')
      fprintf(f, "\\%c", b);
    else if (b == '\n')
      fputs("\\n", f);
    else if (b == '\r')
      fputs("\\r", f);
    else if (b == '\t')
      fputs("\\t", f);
    else if (b >= 0x20 && b < 0x7F)
      fputc(b, f);
    else
      fprintf(f, "\\x%02x", b);
  }

  fputc(q, f);
}

/* de-leet table: @/4->a, 0->o, 3->e, 1/!->i, $/5->s, 7->t, 8->b */
static uint8_t leet_map(uint8_t c)
{
  switch (c)
  {
  case '@':
  case '4':
    return 'a';
  case '0':
    return 'o';
  case '3':
    return 'e';
  case '1':
  case '!':
    return 'i';
  case '$':
  case '5':
    return 's';
  case '7':
    return 't';
  case '8':
    return 'b';
  default:
    return c;
  }
}

/* deleet_expand(): each word + its de-leet form (if different), deduped.
 * Byte-level translation is exact: leet chars are ASCII and never appear
 * inside multi-byte UTF-8 sequences. */
static int deleet_expand(const wlist *in, wlist *out)
{
  sset seen;

  if (sset_init(&seen, 64) != 0)
    return -1;

  int rc = -1;

  for (uint32_t i = 0; i < in->n; i++)
  {
    const uint8_t *w = in->p[i];
    const uint32_t l = in->l[i];

    const int r1 = sset_add(&seen, w, l);

    if (r1 < 0)
      goto out;

    if (r1 == 1 && wlist_push_own(out, w, l) != 0)
      goto out;

    uint8_t tmp[256];
    uint8_t *dl = tmp;
    int     changed = 0;

    if (l > sizeof(tmp))
    {
      dl = malloc(l);

      if (!dl)
        goto out;
    }

    for (uint32_t k = 0; k < l; k++)
    {
      dl[k] = leet_map(w[k]);

      if (dl[k] != w[k])
        changed = 1;
    }

    if (changed)
    {
      const int r2 = sset_add(&seen, dl, l);

      if (r2 < 0 || (r2 == 1 && wlist_push_own(out, dl, l) != 0))
      {
        if (dl != tmp)
          free(dl);
        goto out;
      }
    }

    if (dl != tmp)
      free(dl);
  }

  rc = 0;

out:
  sset_free(&seen);

  return rc;
}

/* service variants: known table, else [name, "my"+name]; dedup keeping
 * order; stable sort by length descending (longest first) */
static int service_variants(const char *arg, wlist *out)
{
  /* name.strip().lower() -- ASCII-level (documented; CLI arg) */
  size_t n = strlen(arg);
  size_t a = 0, b = n;

  while (a < b && (arg[a] == ' ' || arg[a] == '\t' || arg[a] == '\n'
                || arg[a] == '\r' || arg[a] == '\v' || arg[a] == '\f'))
    a++;

  while (b > a && (arg[b - 1] == ' ' || arg[b - 1] == '\t' || arg[b - 1] == '\n'
                || arg[b - 1] == '\r' || arg[b - 1] == '\v' || arg[b - 1] == '\f'))
    b--;

  char name[256];

  if (b - a >= sizeof(name))
    return -1;

  for (size_t i = a; i < b; i++)
  {
    char c = arg[i];

    if (c >= 'A' && c <= 'Z')
      c = (char) (c + ('a' - 'A'));

    name[i - a] = c;
  }

  const size_t nl = b - a;

  name[nl] = 0;

  static const char *li[] = { "linkedin", "linked", "lnkd", "mylinkedin" };

  if (!strcmp(name, "linkedin"))
  {
    for (size_t i = 0; i < 4; i++)
      if (wlist_push_own(out, (const uint8_t *) li[i], (uint32_t) strlen(li[i])) != 0)
        return -1;
  }
  else
  {
    char myname[300];

    if (nl + 2 >= sizeof(myname))
      return -1;

    memcpy(myname, "my", 2);
    memcpy(myname + 2, name, nl);

    if (wlist_push_own(out, (const uint8_t *) name, (uint32_t) nl) != 0
     || wlist_push_own(out, (const uint8_t *) myname, (uint32_t) (nl + 2)) != 0)
      return -1;
  }

  /* dedup keep order (dict.fromkeys) */
  wlist dd = { 0 };
  sset  seen;

  if (sset_init(&seen, 16) != 0)
    return -1;

  for (uint32_t i = 0; i < out->n; i++)
  {
    const int r = sset_add(&seen, out->p[i], out->l[i]);

    if (r < 0 || (r == 1 && wlist_push_ref(&dd, out->p[i], out->l[i]) != 0))
    {
      sset_free(&seen);
      wlist_free(&dd);
      return -1;
    }
  }

  sset_free(&seen);

  /* stable sort by length descending (insertion sort, tiny n) */
  for (uint32_t i = 1; i < dd.n; i++)
  {
    const uint8_t *p = dd.p[i];
    const uint32_t l = dd.l[i];
    uint32_t       j = i;

    while (j > 0 && dd.l[j - 1] < l)
    {
      dd.p[j] = dd.p[j - 1];
      dd.l[j] = dd.l[j - 1];
      j--;
    }

    dd.p[j] = p;
    dd.l[j] = l;
  }

  /* replace *out with the deduped/sorted view (refs into out's arena) */
  out->n = 0;

  for (uint32_t i = 0; i < dd.n; i++)
    if (wlist_push_ref(out, dd.p[i], dd.l[i]) != 0)
    {
      wlist_free(&dd);
      return -1;
    }

  free(dd.p);
  free(dd.l);

  return 0;
}

/* ------------------------------------------------------------------ */
/* per-word JM scores (shared by --emit and the slice scorer)          */
/* ------------------------------------------------------------------ */

typedef struct
{
  const sp_model *m;
  const double   *logu;
  const double   *u;
  const double   *out_mass;
  const uint64_t *roff;         /* CSR by bi_i: rows in file order */
  const uint32_t *rj;
  const int64_t  *rcnt;
  const uint64_t *coff;         /* CSC by bi_j */
  const uint32_t *ci;
  const int64_t  *ccnt;
  uint32_t        K;            /* post-injection vocab size */
  double         *cond_f;       /* scratch [K] */
  double         *cond_r;       /* scratch [K] */
} score_ctx;

static void word_scores(score_ctx *c, uint32_t wi, double *s_f, double *s_r)
{
  const uint32_t K = c->K;

  memset(c->cond_f, 0, K * sizeof(double));
  memset(c->cond_r, 0, K * sizeof(double));

  /* forward: P(x|w) row -- bi_cnt / max(out_mass[wi], 1e-15) */
  const double dn = fmax(c->out_mass[wi], 1e-15);

  for (uint64_t e = c->roff[wi]; e < c->roff[wi + 1]; e++)
    c->cond_f[c->rj[e]] += (double) c->rcnt[e] / dn;

  /* reverse: per-source normalization bi_cnt / max(out_mass[x], 1e-15) */
  for (uint64_t e = c->coff[wi]; e < c->coff[wi + 1]; e++)
  {
    const uint32_t x = c->ci[e];

    c->cond_r[x] += (double) c->ccnt[e] / fmax(c->out_mass[x], 1e-15);
  }

  const double oma = 1.0 - ALPHA;

  for (uint32_t x = 0; x < K; x++)
  {
    s_f[x] = c->logu[wi]
           + log(fmax(ALPHA * c->cond_f[x] + oma * c->u[x], 1e-15));
    s_r[x] = c->logu[x]
           + log(fmax(ALPHA * c->cond_r[x] + oma * c->u[wi], 1e-15));
  }
}

/* ------------------------------------------------------------------ */
/* --emit mode                                                         */
/* ------------------------------------------------------------------ */

typedef struct
{
  uint32_t *idx;                /* candidate index, U32_EMPTY */
  uint64_t *h;
  uint64_t  cap, n;
} cset;

static int cset_init(cset *s, uint64_t total)
{
  uint64_t cap = 16;

  while (cap * 7 < total * 10)
    cap *= 2;

  s->idx = malloc((size_t) cap * sizeof(uint32_t));
  s->h = malloc((size_t) cap * sizeof(uint64_t));

  if (!s->idx || !s->h)
    return -1;

  memset(s->idx, 0xFF, (size_t) cap * sizeof(uint32_t));

  s->cap = cap;
  s->n = 0;

  return 0;
}

static void cset_free(cset *s)
{
  free(s->idx);
  free(s->h);
  s->idx = NULL;
  s->h = NULL;
  s->cap = 0;
  s->n = 0;
}

static uint64_t cand_hash(const tvoc *v, const cand_t *c)
{
  uint64_t h = 14695981039346656037ull;

  for (uint32_t i = 0; i < v->wl[c->a]; i++)
  {
    h ^= v->w[c->a][i];
    h *= 1099511628211ull;
  }

  for (uint32_t i = 0; i < v->wl[c->b]; i++)
  {
    h ^= v->w[c->b][i];
    h *= 1099511628211ull;
  }

  return h;
}

static int cand_eq(const tvoc *v, const cand_t *x, const cand_t *y)
{
  const uint32_t lx = v->wl[x->a] + v->wl[x->b];
  const uint32_t ly = v->wl[y->a] + v->wl[y->b];

  if (lx != ly)
    return 0;

  /* compare the two concatenations without materializing them */
  const uint8_t *xs[2] = { v->w[x->a], v->w[x->b] };
  const uint8_t *ys[2] = { v->w[y->a], v->w[y->b] };
  const uint32_t xl[2] = { v->wl[x->a], v->wl[x->b] };
  const uint32_t yl[2] = { v->wl[y->a], v->wl[y->b] };
  uint32_t xseg = 0, xoff = 0, yseg = 0, yoff = 0;

  for (uint32_t done = 0; done < lx; done++)
  {
    while (xseg < 1 && xoff >= xl[xseg])
    {
      xseg++;
      xoff = 0;
    }

    while (yseg < 1 && yoff >= yl[yseg])
    {
      yseg++;
      yoff = 0;
    }

    if (xs[xseg][xoff] != ys[yseg][yoff])
      return 0;

    xoff++;
    yoff++;
  }

  return 1;
}

/* returns 1 if newly inserted */
static int cset_add(cset *s, const tvoc *v, const cand_t *cands, uint32_t ci,
                    uint64_t h)
{
  uint64_t i = h & (s->cap - 1);

  while (s->idx[i] != U32_EMPTY)
  {
    if (s->h[i] == h && cand_eq(v, &cands[s->idx[i]], &cands[ci]))
      return 0;

    i = (i + 1) & (s->cap - 1);
  }

  s->idx[i] = ci;
  s->h[i] = h;
  s->n++;

  return 1;
}

static int do_emit(const tvoc *v, score_ctx *sc, const char *emit_arg,
                   int deleet, const uint32_t *personal, uint32_t n_personal)
{
  wlist ew = { 0 };
  int    rc = 1;

  /* candidate word list: --emit=a,b,c or the personal words */
  if (emit_arg)
  {
    const uint8_t *p = (const uint8_t *) emit_arg;
    size_t         start = 0, i = 0;

    for (;;)
    {
      if (p[i] == ',' || p[i] == 0)
      {
        if (wlist_push_ref(&ew, p + start, (uint32_t) (i - start)) != 0)
          goto out;

        if (p[i] == 0)
          break;

        start = i + 1;
      }

      i++;
    }
  }
  else
  {
    for (uint32_t i = 0; i < n_personal; i++)
      if (wlist_push_ref(&ew, v->w[personal[i]], v->wl[personal[i]]) != 0)
        goto out;
  }

  wlist ew2 = { 0 };

  if (deleet)
  {
    if (deleet_expand(&ew, &ew2) != 0)
      goto out;
  }
  else
  {
    ew2.p = ew.p;
    ew2.l = ew.l;
    ew2.n = ew.n;
    ew2.cap = ew.cap;
    ew.p = NULL;
    ew.l = NULL;
    ew.cap = 0;
  }

  /* resolve words, count candidates */
  uint32_t *wi_list = malloc((size_t) ew2.n * sizeof(uint32_t));
  uint32_t n_valid = 0;

  if (!wi_list && ew2.n)
    goto out_ew2;

  for (uint32_t i = 0; i < ew2.n; i++)
  {
    const int64_t idx = tvoc_find(v, ew2.p[i], ew2.l[i]);

    if (idx < 0)
    {
      fprintf(stderr, "not in vocab: %.*s\n", (int) ew2.l[i],
              (const char *) ew2.p[i]);
      continue;
    }

    wi_list[n_valid++] = (uint32_t) idx;
  }

  const uint32_t K = sc->K;

  if (K > 0 && n_valid > 0)
  {
    const uint64_t total = (uint64_t) n_valid * 2u * (K - 1u);

    cand_t *cands = malloc((size_t) total * sizeof(cand_t));
    cand_t *tmp = malloc((size_t) total * sizeof(cand_t));
    double *s_f = malloc((size_t) K * sizeof(double));
    double *s_r = malloc((size_t) K * sizeof(double));

    if (!cands || !tmp || !s_f || !s_r)
    {
      free(cands);
      free(tmp);
      free(s_f);
      free(s_r);
      goto out_wi;
    }

    uint64_t nc = 0;

    for (uint32_t i = 0; i < n_valid; i++)
    {
      const uint32_t wi = wi_list[i];

      word_scores(sc, wi, s_f, s_r);

      for (uint32_t x = 0; x < K; x++)
      {
        if (x == wi)
          continue;

        cands[nc].s = s_f[x];
        cands[nc].a = wi;
        cands[nc].b = x;
        nc++;
        cands[nc].s = s_r[x];
        cands[nc].a = x;
        cands[nc].b = wi;
        nc++;
      }
    }

    candsort_desc(cands, tmp, nc);

    cset seen;

    if (cset_init(&seen, nc) != 0)
    {
      free(cands);
      free(tmp);
      free(s_f);
      free(s_r);
      goto out_wi;
    }

    obuf out_b;

    if (obuf_init(&out_b, 8u << 20) != 0)
    {
      cset_free(&seen);
      free(cands);
      free(tmp);
      free(s_f);
      free(s_r);
      goto out_wi;
    }

    int io_ok = 1;

    for (uint64_t i = 0; i < nc && io_ok; i++)
    {
      const uint64_t h = cand_hash(v, &cands[i]);

      if (!cset_add(&seen, v, cands, (uint32_t) i, h))
        continue;

      if (obuf_write(&out_b, v->w[cands[i].a], v->wl[cands[i].a]) != 0
       || obuf_write(&out_b, v->w[cands[i].b], v->wl[cands[i].b]) != 0
       || obuf_write(&out_b, "\n", 1) != 0)
        io_ok = 0;
    }

    if (io_ok && obuf_flush(&out_b) == 0)
      rc = 0;

    free(out_b.buf);
    cset_free(&seen);
    free(cands);
    free(tmp);
    free(s_f);
    free(s_r);
  }
  else
    rc = 0;

out_wi:
  free(wi_list);
out_ew2:
  wlist_free(&ew2);
out:
  wlist_free(&ew);

  return rc;
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

static void usage(void)
{
  fprintf(stderr,
    "usage: spg targeted UNI BI TEST [NP] [K] [--service=NAME]\n"
    "                      [--personal=FILE] [--deleet] [--emit[=w1,w2,...]]\n");
}

int spg_targeted(int argc, char **argv)
{
  /* argv[0..] are the targeted.py arguments (dispatcher passed argv+2) */
  const char *pos[8];
  int         npos = 0;

  const char *service = NULL;
  const char *pfile = NULL;
  const char *emit_arg = NULL;
  const char *f64dump = NULL;
  int         deleet = 0;
  int         have_emit = 0;

  for (int i = 0; i < argc; i++)
  {
    const char *a = argv[i];

    if (a[0] == '-' && a[1] == '-')
    {
      if (!strncmp(a, "--service=", 10))
      {
        if (!service)
          service = a + 10;
      }
      else if (!strncmp(a, "--personal=", 11))
      {
        if (!pfile)
          pfile = a + 11;
      }
      else if (!strncmp(a, "--f64dump=", 10))
      {
        if (!f64dump)
          f64dump = a + 10;
      }
      else if (!strcmp(a, "--deleet"))
        deleet = 1;
      else if (!strcmp(a, "--emit"))
      {
        if (!have_emit)
        {
          have_emit = 1;
          emit_arg = NULL;
        }
      }
      else if (!strncmp(a, "--emit=", 7))
      {
        if (!have_emit)
        {
          have_emit = 1;
          emit_arg = a + 7;
        }
      }
    }
    else if (npos < 8)
      pos[npos++] = a;
  }

  if (npos < 3)
  {
    usage();
    return 2;
  }

  const char *uni_path = pos[0];
  const char *bi_path = pos[1];
  const char *test_path = pos[2];
  const long  NP = (npos > 3) ? atol(pos[3]) : 200;
  const long  K_req = (npos > 4) ? atol(pos[4]) : 10000;

  int     rc = 1;
  sp_model m;

  if (sp_model_load(&m, uni_path, bi_path,
                    K_req > 0 ? (uint32_t) K_req : 0, 2, 10) != 0)
  {
    fprintf(stderr, "spg targeted: failed to load model\n");
    return 1;
  }

  tvoc v;

  if (tvoc_init(&v, &m) != 0)
  {
    sp_model_free(&m);
    return 1;
  }

  /* all cleanup-owned pointers, initialized for the shared exit path */
  double   *logu = NULL;
  double   *u = NULL;
  double   *out_mass = NULL;
  uint64_t *roff = NULL;
  uint64_t *coff = NULL;
  uint32_t *rj = NULL;
  uint32_t *ci2 = NULL;
  int64_t  *rcnt = NULL;
  int64_t  *ccnt = NULL;
  uint32_t *personal = NULL;

  const int64_t topcnt = v.cnt[0];

  uint32_t *service_idxs = NULL;
  uint32_t  n_service = 0;
  uint32_t *personal_idxs = NULL;
  uint32_t  n_personal_file = 0;
  int       have_personal_file = 0;

  /* --service: variants injected with boost, first in the personal list */
  if (service && *service)
  {
    wlist vs = { 0 };

    if (service_variants(service, &vs) != 0)
      goto out;

    service_idxs = malloc((size_t) (vs.n ? vs.n : 1) * sizeof(uint32_t));

    if (!service_idxs)
    {
      wlist_free(&vs);
      goto out;
    }

    if (tvoc_inject(&v, &vs, 1, topcnt, service_idxs, &n_service) != 0)
    {
      wlist_free(&vs);
      goto out;
    }

    fprintf(stderr, "service ");
    py_repr(stderr, (const uint8_t *) service, strlen(service));
    fprintf(stderr, " -> [");

    for (uint32_t i = 0; i < n_service; i++)
    {
      if (i)
        fprintf(stderr, ", ");

      py_repr(stderr, v.w[service_idxs[i]], v.wl[service_idxs[i]]);
    }

    fprintf(stderr, "]\n");

    wlist_free(&vs);
  }

  /* --personal: read, strip, dedup keep order, optional de-leet, inject */
  if (pfile)
  {
    size_t   fn = 0;
    uint8_t *fb = read_all(pfile, &fn);

    if (!fb)
    {
      fprintf(stderr, "spg targeted: cannot read personal file %s\n", pfile);
      goto out;
    }

    wlist raw = { 0 };
    size_t it = 0, loff, llen;

    while (next_line(fb, fn, &it, &loff, &llen))
    {
      const size_t cl = utf8_clean(fb + loff, llen);
      size_t soff, slen;

      utf8_strip(fb + loff, cl, &soff, &slen);

      if (slen == 0)
        continue;

      if (wlist_push_ref(&raw, fb + loff + soff, (uint32_t) slen) != 0)
      {
        wlist_free(&raw);
        free(fb);
        goto out;
      }
    }

    const uint64_t n_raw = raw.n;

    /* dict.fromkeys: dedup keeping order */
    wlist dd = { 0 };
    sset  seen;

    if (sset_init(&seen, 2 * (raw.n + 16)) != 0)
    {
      wlist_free(&raw);
      free(fb);
      goto out;
    }

    int oom = 0;

    for (uint32_t i = 0; i < raw.n && !oom; i++)
    {
      const int r = sset_add(&seen, raw.p[i], raw.l[i]);

      if (r < 0 || (r == 1 && wlist_push_ref(&dd, raw.p[i], raw.l[i]) != 0))
        oom = 1;
    }

    sset_free(&seen);
    wlist_free(&raw);

    if (oom)
    {
      wlist_free(&dd);
      free(fb);
      goto out;
    }

    wlist fin = { 0 };

    if (deleet)
    {
      if (deleet_expand(&dd, &fin) != 0)
      {
        wlist_free(&dd);
        free(fb);
        goto out;
      }

      wlist_free(&dd);
    }
    else
      fin = dd;

    fprintf(stderr, "personal file %s: %llu words%s -> %llu\n",
            pfile, (unsigned long long) n_raw,
            deleet ? " +deleet" : "", (unsigned long long) fin.n);

    personal_idxs = malloc((size_t) (fin.n ? fin.n : 1) * sizeof(uint32_t));

    if (!personal_idxs)
    {
      wlist_free(&fin);
      free(fb);
      goto out;
    }

    if (tvoc_inject(&v, &fin, 0, topcnt, personal_idxs, &n_personal_file) != 0)
    {
      wlist_free(&fin);
      free(fb);
      goto out;
    }

    have_personal_file = 1;

    wlist_free(&fin);
    free(fb);
  }

  const uint32_t K = v.n;       /* post-injection vocabulary size */

  /* logu / u recomputed over the extended counts */
  logu = malloc((size_t) K * sizeof(double));
  u = malloc((size_t) K * sizeof(double));

  if (!logu || !u)
    goto out;

  {
    double s = 0.0;

    for (uint32_t i = 0; i < K; i++)
      s += (double) v.cnt[i];

    for (uint32_t i = 0; i < K; i++)
    {
      logu[i] = log((double) v.cnt[i] / s);
      u[i] = exp(logu[i]);
    }
  }

  /* parity aid (plan 5.3): replace logu/u with the numpy-computed values
   * (numpy SIMD log/exp can differ from libm by 1 ulp). The sidecar is
   * validated against the freshly injected counts, so a stale or foreign
   * dump fails loudly. */
  if (f64dump)
  {
    size_t   dn = 0;
    uint8_t *db = read_all(f64dump, &dn);
    uint64_t dk = 0;
    int      bad = 0;

    if (!db)
    {
      fprintf(stderr, "spg targeted: cannot read f64 dump %s\n", f64dump);
      goto out;
    }

    if (dn >= 8)
      memcpy(&dk, db, 8);

    if (dk != K || dn != 8 + (size_t) K * 8 * 3)
      bad = 1;

    if (!bad && memcmp(db + 8, v.cnt, (size_t) K * 8) != 0)
      bad = 1;

    if (bad)
    {
      fprintf(stderr, "spg targeted: f64 dump %s does not match the "
                      "injected vocabulary\n", f64dump);
      free(db);
      goto out;
    }

    memcpy(logu, db + 8 + (size_t) K * 8, (size_t) K * 8);
    memcpy(u, db + 8 + (size_t) K * 16, (size_t) K * 8);
    free(db);
  }

  /* personal = service_idxs + remaining (file | top-NP), seen_svc filtered */
  uint32_t  P = 0;

  {
    uint64_t cap = (uint64_t) n_service
                 + (have_personal_file ? n_personal_file
                                       : (uint64_t) (NP > 0 ? NP : 0));

    personal = malloc((size_t) (cap ? cap : 1) * sizeof(uint32_t));

    if (!personal)
      goto out;

    for (uint32_t i = 0; i < n_service; i++)
      personal[P++] = service_idxs[i];

    if (have_personal_file)
    {
      for (uint32_t i = 0; i < n_personal_file; i++)
      {
        const uint32_t idx = personal_idxs[i];
        int            dup = 0;

        for (uint32_t j = 0; j < n_service; j++)
          if (service_idxs[j] == idx)
          {
            dup = 1;
            break;
          }

        if (!dup)
          personal[P++] = idx;
      }
    }
    else
    {
      int64_t lim = NP < (int64_t) K ? NP : (int64_t) K;

      if (lim < 0)
        lim = 0;

      for (int64_t i = 0; i < lim; i++)
      {
        int dup = 0;

        for (uint32_t j = 0; j < n_service; j++)
          if (service_idxs[j] == (uint32_t) i)
          {
            dup = 1;
            break;
          }

        if (!dup)
          personal[P++] = (uint32_t) i;
      }
    }
  }

  /* out_mass: scatter-sum of bigram counts per source word */
  out_mass = calloc(K, sizeof(double));

  if (!out_mass)
    goto out;

  for (uint64_t e = 0; e < m.E; e++)
    out_mass[m.bi_i[e]] += (double) m.bi_cnt[e];

  /* CSR by bi_i and CSC by bi_j, file order preserved within groups */
  roff = calloc((size_t) K + 1, sizeof(uint64_t));
  coff = calloc((size_t) K + 1, sizeof(uint64_t));

  if (!roff || !coff)
    goto out;

  for (uint64_t e = 0; e < m.E; e++)
  {
    roff[(uint32_t) m.bi_i[e] + 1]++;
    coff[(uint32_t) m.bi_j[e] + 1]++;
  }

  for (uint32_t i = 0; i < K; i++)
  {
    roff[i + 1] += roff[i];
    coff[i + 1] += coff[i];
  }

  rj = malloc((size_t) (m.E ? m.E : 1) * sizeof(uint32_t));
  ci2 = malloc((size_t) (m.E ? m.E : 1) * sizeof(uint32_t));
  rcnt = malloc((size_t) (m.E ? m.E : 1) * sizeof(int64_t));
  ccnt = malloc((size_t) (m.E ? m.E : 1) * sizeof(int64_t));

  if (!rj || !ci2 || !rcnt || !ccnt)
    goto out;

  {
    uint64_t *cur_r = malloc(((size_t) K + 1) * sizeof(uint64_t));
    uint64_t *cur_c = malloc(((size_t) K + 1) * sizeof(uint64_t));

    if (!cur_r || !cur_c)
    {
      free(cur_r);
      free(cur_c);
      goto out;
    }

    memcpy(cur_r, roff, ((size_t) K + 1) * sizeof(uint64_t));
    memcpy(cur_c, coff, ((size_t) K + 1) * sizeof(uint64_t));

    for (uint64_t e = 0; e < m.E; e++)
    {
      const uint64_t pr = cur_r[(uint32_t) m.bi_i[e]]++;
      const uint64_t pc = cur_c[(uint32_t) m.bi_j[e]]++;

      rj[pr] = (uint32_t) m.bi_j[e];
      rcnt[pr] = m.bi_cnt[e];
      ci2[pc] = (uint32_t) m.bi_i[e];
      ccnt[pc] = m.bi_cnt[e];
    }

    free(cur_r);
    free(cur_c);
  }

  {
    score_ctx sc;

    sc.m = &m;
    sc.logu = logu;
    sc.u = u;
    sc.out_mass = out_mass;
    sc.roff = roff;
    sc.rj = rj;
    sc.rcnt = rcnt;
    sc.coff = coff;
    sc.ci = ci2;
    sc.ccnt = ccnt;
    sc.K = K;
    sc.cond_f = malloc((size_t) K * sizeof(double));
    sc.cond_r = malloc((size_t) K * sizeof(double));

    if (!sc.cond_f || !sc.cond_r)
    {
      free(sc.cond_f);
      free(sc.cond_r);
      goto out;
    }

    if (have_emit)
    {
      rc = do_emit(&v, &sc, emit_arg, deleet, personal, P);
      free(sc.cond_f);
      free(sc.cond_r);
      goto out;
    }

    /* ---------------- test slice scan ---------------- */

    size_t   tn = 0;
    uint8_t *tb = read_all(test_path, &tn);

    if (!tb)
    {
      fprintf(stderr, "spg targeted: cannot read test file %s\n", test_path);
      free(sc.cond_f);
      free(sc.cond_r);
      goto out;
    }

    /* per-personal-word byte/codepoint lengths */
    size_t *pwcp = malloc((size_t) (P ? P : 1) * sizeof(size_t));

    if (!pwcp)
    {
      free(tb);
      free(sc.cond_f);
      free(sc.cond_r);
      goto out;
    }

    for (uint32_t i = 0; i < P; i++)
      pwcp[i] = utf8_cplen(v.w[personal[i]], v.wl[personal[i]]);

    i64map hit_pw;

    if (i64map_init(&hit_pw, 1024) != 0)
    {
      free(pwcp);
      free(tb);
      free(sc.cond_f);
      free(sc.cond_r);
      goto out;
    }

    int64_t slice_size = 0;
    int64_t pwid = 0;
    size_t  it = 0, loff, llen;

    while (next_line(tb, tn, &it, &loff, &llen))
    {
      uint8_t *pw = tb + loff;
      const size_t cl = utf8_clean(pw, llen);
      size_t cpn = 0;

      if (utf8_alpha_cplen(pw, cl, &cpn))
      {
        int qualified = 0;

        for (uint32_t pi = 0; pi < P; pi++)
        {
          const uint8_t *w = v.w[personal[pi]];
          const uint32_t wl = v.wl[personal[pi]];

          if (wl <= cl && cpn >= pwcp[pi] + 3 && !memcmp(pw, w, wl))
          {
            qualified = 1;

            const int64_t xi = tvoc_find(&v, pw + wl, (uint32_t) (cl - wl));

            if (xi >= 0)
              i64map_setdefault(&hit_pw,
                                ((int64_t) personal[pi] * K + xi) * 2 + 0,
                                pwid);
          }
        }

        for (uint32_t pi = 0; pi < P; pi++)
        {
          const uint8_t *w = v.w[personal[pi]];
          const uint32_t wl = v.wl[personal[pi]];

          if (wl <= cl && cpn >= pwcp[pi] + 3 && !memcmp(pw + cl - wl, w, wl))
          {
            qualified = 1;

            const int64_t xi = tvoc_find(&v, pw, (uint32_t) (cl - wl));

            if (xi >= 0)
              i64map_setdefault(&hit_pw,
                                ((int64_t) personal[pi] * K + xi) * 2 + 1,
                                pwid);
          }
        }

        if (qualified)
          slice_size++;
      }

      pwid++;
    }

    free(tb);

    fprintf(stderr, "personal words: %llu, test slice: %llu (reachable: %llu)\n",
            (unsigned long long) P, (unsigned long long) slice_size,
            (unsigned long long) hit_pw.n);

    /* ---------------- candidate scores ---------------- */

    const uint64_t n_cand = (uint64_t) P * K * 2u;

    if (n_cand > 0xFFFFFFFEull)
    {
      fprintf(stderr, "spg targeted: candidate space too large\n");
      free(pwcp);
      i64map_free(&hit_pw);
      free(sc.cond_f);
      free(sc.cond_r);
      goto out;
    }

    double   *scores = NULL;
    uint32_t *ord = NULL, *tmp = NULL;
    double   *s_f = malloc((size_t) (K ? K : 1) * sizeof(double));
    double   *s_r = malloc((size_t) (K ? K : 1) * sizeof(double));

    if (n_cand)
    {
      scores = malloc((size_t) n_cand * sizeof(double));
      ord = malloc((size_t) n_cand * sizeof(uint32_t));
      tmp = malloc((size_t) n_cand * sizeof(uint32_t));
    }

    if (!s_f || !s_r || (n_cand && (!scores || !ord || !tmp)))
    {
      free(s_f);
      free(s_r);
      free(scores);
      free(ord);
      free(tmp);
      free(pwcp);
      i64map_free(&hit_pw);
      free(sc.cond_f);
      free(sc.cond_r);
      goto out;
    }

    /* lm scores: interleaved s_f/s_r per personal word, x==w -> -inf */
    uint64_t p = 0;

    for (uint32_t pi = 0; pi < P; pi++)
    {
      const uint32_t wi = personal[pi];

      word_scores(&sc, wi, s_f, s_r);

      for (uint32_t x = 0; x < K; x++)
      {
        const double fv = (x == wi) ? -INFINITY : s_f[x];
        const double rv = (x == wi) ? -INFINITY : s_r[x];

        scores[p + 2u * x] = fv;
        scores[p + 2u * x + 1u] = rv;
      }

      p += 2u * (uint64_t) K;
    }

    i64set hits;

    if (i64set_init(&hits, 1024) != 0)
    {
      free(s_f);
      free(s_r);
      free(scores);
      free(ord);
      free(tmp);
      free(pwcp);
      i64map_free(&hit_pw);
      free(sc.cond_f);
      free(sc.cond_r);
      goto out;
    }

    for (int label_i = 0; label_i < 2; label_i++)
    {
      const char *label = label_i == 0 ? "lm" : "freq";

      if (label_i == 1)
      {
        /* freq scores: forward and reverse share logu[w] + logu[x] */
        uint64_t q = 0;

        for (uint32_t pi = 0; pi < P; pi++)
        {
          const uint32_t wi = personal[pi];

          for (uint32_t x = 0; x < K; x++)
          {
            const double bv = (x == wi) ? -INFINITY : logu[wi] + logu[x];

            scores[q + 2u * x] = bv;
            scores[q + 2u * x + 1u] = bv;
          }

          q += 2u * (uint64_t) K;
        }
      }

      isort_desc(ord, tmp, scores, n_cand);

      hits.n = 0;             /* reuse the set: re-init cheaply */
      for (int64_t i = 0; i < hits.cap; i++)
        hits.k[i] = I64_EMPTY;

      int ci = 0;

      for (uint64_t n = 1; n <= n_cand; n++)
      {
        const uint32_t o = ord[n - 1];
        const uint64_t blk = o / (2u * (uint64_t) K);
        const uint64_t rem = o % (2u * (uint64_t) K);
        const uint32_t xi = (uint32_t) (rem / 2u);
        const uint32_t d = (uint32_t) (rem % 2u);
        const int64_t  hk = ((int64_t) personal[blk] * K + xi) * 2 + d;
        int64_t        hpwid;

        if (i64map_get(&hit_pw, hk, &hpwid))
          i64set_add(&hits, hpwid);

        while (ci < CHECKPOINT_N && (int64_t) n == CHECKPOINTS[ci])
        {
          printf("%4s  %12llu -> %7llu cracked (%.2f%% of slice)\n",
                 label, (unsigned long long) n,
                 (unsigned long long) hits.n,
                 100.0 * (double) hits.n
                       / (double) (slice_size > 0 ? slice_size : 1));
          ci++;
        }
      }

      printf("%4s  final %llu -> %llu (%.2f%% of slice)\n",
             label, (unsigned long long) n_cand,
             (unsigned long long) hits.n,
             100.0 * (double) hits.n
                   / (double) (slice_size > 0 ? slice_size : 1));
    }

    i64set_free(&hits);
    free(s_f);
    free(s_r);
    free(scores);
    free(ord);
    free(tmp);
    free(pwcp);
    i64map_free(&hit_pw);
    free(sc.cond_f);
    free(sc.cond_r);

    rc = 0;
  }

out:
  free(out_mass);
  free(roff);
  free(coff);
  free(rj);
  free(ci2);
  free(rcnt);
  free(ccnt);
  free(personal);
  free(logu);
  free(u);
  free(service_idxs);
  free(personal_idxs);
  tvoc_free(&v);
  sp_model_free(&m);

  return rc;
}
