/*
 * chains.c - "spg chains": 3-word chains w1w2w3 via beam over bigram seams.
 *
 * Port of src/genchains.py (post bugfixes_v1/v2 state):
 *   score(w1,w2,w3) = logu[w1] + logp[w1,w2] + logp3,
 *   logp  = log(max(alpha * M[i,j]/cf[i] + (1-alpha) * u[j], 1e-15))
 *           with M[i,j] = bigram count, u[j] = cf[j]/cf.sum()      (JM)
 *   logp3 = logp[w2,w3]                                  (lam = 0: Markov)
 *         = mx + log(lam*exp(logp[w2,k]-mx) + (1-lam)*exp(logp[w1,k]-mx)),
 *           mx = max(logp[w2,k], logp[w1,k])             (lam > 0: pseudo-trigram)
 *
 * Beam: top-B first seams (w1,w2), each extended by top-T continuations w3.
 * Candidates with w3 == w1 or w3 == w2, or outside the [lmin,lmax] length
 * band, are masked to -inf. Global top-limit cut with dedup BEFORE the
 * budget is spent: the shortfall is topped up by doubling (bugfix B2).
 *
 * Ported fixes (results/lab2/bugfixes_v1.md, bugfixes_v2.md):
 *   B2 - top-up of the selection depth: cut = min(cut + 2*(limit-emitted), N)
 *   B3 - T = min(T, K) after the model load (argpartition kth guard)
 *   H3 - validation: B >= 1, T >= 1, 0 <= lam <= 1 (clear error text, rc 1)
 *   H4 - the -inf break is checked INSIDE every 500K emission chunk, not
 *        only at chunk boundaries
 * The Python OOM guard (adaptive block size) is kept: BS = clamp(budget /
 * (K*8), 5000, 50000) with budget = 1.2e9 (lam = 0) resp. 0.6e9 (lam > 0).
 * In C the block bounds no buffer (rows of logp are read in place, the lam
 * path uses a single O(K) row buffer), so it is structural parity only;
 * real OOM protection is that every allocation is checked.
 *
 * Tie order (fixed here; see results/lab2/c_port_plan.md N2/N4):
 *   - final emission order: score desc, candidate index DESC -- exactly the
 *     semantics of np.argsort(sc, kind="stable")[::-1]. Candidate index
 *     c = q*T + t, where q is the seam position and t the continuation
 *     position in beam order.
 *   - the beam order itself (seam order, per-row continuation order) comes
 *     from np.argpartition in genchains.py and is UNSPECIFIED there; it only
 *     permutes equal-score candidates via c. This port fixes it canonically:
 *     seams by (score desc, flat idx asc), continuations by (score desc,
 *     col asc). tools/genchains_ref.py reproduces this canonical order for
 *     bitwise parity; against stock genchains.py the outputs agree as
 *     multisets of lines up to permutations inside equal-score groups.
 *
 * Numeric contract: every float op mirrors the numpy expression elementwise
 * (division, multiply, add, max, log/exp). All scores are strictly negative
 * or exactly -inf on real vocabularies (logu < 0 for K >= 2, logp <= 0 with
 * pjm < 1), so no +/-0.0 tie split and no NaN can occur; np.maximum vs the
 * ternary used below would diverge only on NaN pjm (zero unigram counts),
 * which the loader never produces.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <errno.h>

#include "chains.h"
#include "model.h"

#define CHUNK       500000u        /* emission chunk, mirrors genchains.py CH */
#define OUTBUF_CAP  (8u << 20)     /* stdout buffer, 8 MiB */
#define NEG_INF     (-INFINITY)

typedef struct
{
  double   s;                      /* score */
  uint32_t idx;                    /* flat seam idx / candidate idx / column */
} sel_rec;

/* ------------------------------ helpers ------------------------------ */

static void *xmalloc(size_t n)
{
  void *p = malloc(n ? n : 1);

  if (!p)
  {
    fprintf(stderr, "spg chains: out of memory (%llu bytes)\n",
            (unsigned long long) n);
    exit(1);
  }

  return p;
}

static void *xrealloc(void *p, size_t n)
{
  void *q = realloc(p, n ? n : 1);

  if (!q)
  {
    fprintf(stderr, "spg chains: out of memory (%llu bytes)\n",
            (unsigned long long) n);
    exit(1);
  }

  return q;
}

static uint64_t fnv1a64(const uint8_t *s, size_t n)
{
  uint64_t h = 14695981039346656037ull;

  for (size_t i = 0; i < n; i++)
  {
    h ^= s[i];
    h *= 1099511628211ull;
  }

  return h;
}

static int parse_i64(const char *s, int64_t *out)
{
  char     *end = NULL;
  long long v;

  errno = 0;
  v = strtoll(s, &end, 10);

  if (errno || end == s || *end != '\0')
    return -1;

  *out = (int64_t) v;

  return 0;
}

static int parse_dbl(const char *s, double *out)
{
  char   *end = NULL;
  double  v;

  errno = 0;
  v = strtod(s, &end);

  if (end == s || *end != '\0')
    return -1;

  *out = v;

  return 0;
}

/* repr()-style shortest round-trip float formatting, so the lam validation
 * message matches str(float) in genchains.py byte for byte */
static void fmt_float(char *out, size_t cap, double v)
{
  for (int p = 1; p <= 17; p++)
  {
    snprintf(out, cap, "%.*g", p, v);

    if (strtod(out, NULL) == v)
      break;
  }

  if (isfinite(v) && !strchr(out, '.') && !strchr(out, 'e'))
  {
    size_t l = strlen(out);

    if (l + 2 < cap)
      memcpy(out + l, ".0", 3);
  }
}

/* ------------------------- selection / sorting ------------------------ */

/* total order: score desc; ties -> idx asc (tie_desc = 0) or idx desc (1).
 * Scores are never NaN (see header), so this is a strict total order. */
static int rec_before(const sel_rec *a, const sel_rec *b, int tie_desc)
{
  if (a->s != b->s)
    return a->s > b->s;

  return tie_desc ? (a->idx > b->idx) : (a->idx < b->idx);
}

static void rec_sort(sel_rec *a, sel_rec *tmp, size_t n, int tie_desc)
{
  sel_rec *src = a;
  sel_rec *dst = tmp;

  if (n < 2)
    return;

  for (size_t w = 1; w < n; w *= 2)
  {
    for (size_t lo = 0; lo < n; lo += 2 * w)
    {
      size_t mid = (lo + w < n) ? lo + w : n;
      size_t hi  = (lo + 2 * w < n) ? lo + 2 * w : n;
      size_t p = lo, q = mid, o = lo;

      while (p < mid && q < hi)
      {
        if (rec_before(&src[p], &src[q], tie_desc))
          dst[o++] = src[p++];
        else
          dst[o++] = src[q++];
      }

      while (p < mid)
        dst[o++] = src[p++];

      while (q < hi)
        dst[o++] = src[q++];
    }

    sel_rec *swp = src;
    src = dst;
    dst = swp;
  }

  if (src != a)
    memcpy(a, src, n * sizeof(sel_rec));
}

/* value at rank k (0-based) of the array in DESCENDING order; 3-way
 * quickselect with median-of-3 pivot, duplicates-safe */
static double kth_desc(double *a, size_t n, size_t k)
{
  size_t lo = 0;
  size_t hi = n - 1;

  for (;;)
  {
    if (lo >= hi)
      return a[lo];

    size_t mid = lo + (hi - lo) / 2;
    double x = a[lo], y = a[mid], z = a[hi];
    double pv = (x < y) ? ((y < z) ? y : ((x < z) ? z : x))
                        : ((x < z) ? x : ((y < z) ? z : y));
    size_t lt = lo, i = lo, gt = hi;

    while (i <= gt)
    {
      if (a[i] > pv)
      {
        double t = a[lt];
        a[lt] = a[i];
        a[i] = t;
        lt++;
        i++;
      }
      else if (a[i] < pv)
      {
        double t = a[i];
        a[i] = a[gt];
        a[gt] = t;
        gt--;
      }
      else
        i++;
    }

    if (k < lt)
      hi = lt - 1;
    else if (k > gt)
      lo = gt + 1;
    else
      return pv;
  }
}

/* (va,ia) ranks above (vb,ib): higher value, ties -> smaller column */
static int rec_better(double sa, uint32_t ia, double sb, uint32_t ib)
{
  return (sa > sb) || (sa == sb && ia < ib);
}

/* top-T of row v[0..K) by (value desc, column asc), delivered sorted in that
 * order into out[0..T); tmp is scratch of the same size */
static void row_top_t(const double *v, uint32_t K, uint32_t T,
                      sel_rec *out, sel_rec *tmp)
{
  uint32_t wp = 0;

  for (uint32_t c = 0; c < T; c++)
  {
    out[c].s   = v[c];
    out[c].idx = c;
  }

  /* worst slot: lowest value; among equal values the largest column */
  for (uint32_t t = 1; t < T; t++)
    if (rec_better(out[wp].s, out[wp].idx, out[t].s, out[t].idx))
      wp = t;

  for (uint32_t c = T; c < K; c++)
  {
    if (rec_better(v[c], c, out[wp].s, out[wp].idx))
    {
      out[wp].s   = v[c];
      out[wp].idx = c;

      wp = 0;
      for (uint32_t t = 1; t < T; t++)
        if (rec_better(out[wp].s, out[wp].idx, out[t].s, out[t].idx))
          wp = t;
    }
  }

  rec_sort(out, tmp, T, 0);
}

/* ------------------------------ dedup set ----------------------------- */

typedef struct
{
  uint32_t *slots;       /* entry index + 1, 0 = empty */
  uint64_t  cap;         /* power of two */
  uint64_t  n;
  uint64_t *ehash;       /* full FNV-1a per entry */
  uint64_t *eoff;        /* (arena offset << 8) | len, len <= 30 */
  uint8_t  *arena;
  uint64_t  arena_len;
  uint64_t  arena_cap;
  uint64_t  ecount;
  uint64_t  ecap;
} dedup;

static void dedup_init(dedup *d, uint64_t expected)
{
  uint64_t cap = 1u << 14;

  while (cap * 7 < expected * 10)    /* load factor stays below 0.7 */
    cap *= 2;

  d->cap       = cap;
  d->n         = 0;
  d->slots     = xmalloc((size_t) cap * sizeof(uint32_t));
  d->ecap      = expected ? expected : 1;
  d->ecount    = 0;
  d->ehash     = xmalloc((size_t) d->ecap * sizeof(uint64_t));
  d->eoff      = xmalloc((size_t) d->ecap * sizeof(uint64_t));
  d->arena_cap = d->ecap * 24;
  d->arena_len = 0;
  d->arena     = xmalloc((size_t) d->arena_cap);

  memset(d->slots, 0, (size_t) cap * sizeof(uint32_t));
}

static void dedup_grow(dedup *d)
{
  uint64_t  ncap  = d->cap * 2;
  uint32_t *nslot = xmalloc((size_t) ncap * sizeof(uint32_t));

  memset(nslot, 0, (size_t) ncap * sizeof(uint32_t));

  for (uint64_t e = 0; e < d->ecount; e++)
  {
    uint64_t pos = d->ehash[e] & (ncap - 1);

    while (nslot[pos])
      pos = (pos + 1) & (ncap - 1);

    nslot[pos] = (uint32_t) (e + 1);
  }

  free(d->slots);
  d->slots = nslot;
  d->cap   = ncap;
}

/* 1 = new string (recorded), 0 = duplicate */
static int dedup_put(dedup *d, const uint8_t *s, uint32_t len)
{
  uint64_t h;
  uint64_t pos;

  if ((d->n + 1) * 10 >= d->cap * 7)
    dedup_grow(d);

  h   = fnv1a64(s, len);
  pos = h & (d->cap - 1);

  while (d->slots[pos])
  {
    uint64_t e  = d->slots[pos] - 1;
    uint64_t eo = d->eoff[e];

    if (d->ehash[e] == h
     && (uint32_t) (eo & 0xff) == len
     && !memcmp(d->arena + (eo >> 8), s, len))
      return 0;

    pos = (pos + 1) & (d->cap - 1);
  }

  if (d->ecount == d->ecap)
  {
    d->ecap  *= 2;
    d->ehash  = xrealloc(d->ehash, (size_t) d->ecap * sizeof(uint64_t));
    d->eoff   = xrealloc(d->eoff,  (size_t) d->ecap * sizeof(uint64_t));
  }

  if (d->arena_len + len > d->arena_cap)
  {
    while (d->arena_len + len > d->arena_cap)
      d->arena_cap *= 2;

    d->arena = xrealloc(d->arena, (size_t) d->arena_cap);
  }

  memcpy(d->arena + d->arena_len, s, len);
  d->ehash[d->ecount] = h;
  d->eoff[d->ecount]  = (d->arena_len << 8) | len;
  d->arena_len       += len;
  d->slots[pos]       = (uint32_t) (d->ecount + 1);
  d->ecount++;
  d->n++;

  return 1;
}

static void dedup_free(dedup *d)
{
  free(d->slots);
  free(d->ehash);
  free(d->eoff);
  free(d->arena);
}

/* ----------------------------- stdout buffer -------------------------- */

typedef struct
{
  uint8_t *buf;
  size_t   len;
} outw;

static void ow_flush(outw *o)
{
  if (o->len)
  {
    fwrite(o->buf, 1, o->len, stdout);
    o->len = 0;
  }
}

static void ow_line(outw *o, const uint8_t *s, size_t n)
{
  if (OUTBUF_CAP - o->len < n + 1)
  {
    ow_flush(o);

    /* candidates are 3 loader-bounded words (<= 30 bytes), so this cannot
     * trigger on a valid model; direct-write instead of overflowing the
     * buffer if that invariant is ever broken (defense in depth) */
    if (n + 1 > OUTBUF_CAP)
    {
      fwrite(s, 1, n, stdout);
      fputc('\n', stdout);
      return;
    }
  }

  memcpy(o->buf + o->len, s, n);
  o->len += n;
  o->buf[o->len++] = '\n';
}

/* -------------------------------- main -------------------------------- */

int cmd_chains(int argc, char **argv)
{
  if (argc < 4)
  {
    fprintf(stderr,
            "usage: spg chains UNI_TSV BI_TSV K LIMIT"
            " [alpha] [B] [T] [lmin] [lmax] [lam]\n");
    return 2;
  }

  const char *uni = argv[0];
  const char *bi  = argv[1];
  int64_t k64, limit, B, T;
  double  alpha = 0.7, lam = 0.0;
  int64_t lmin = 0, lmax = 127;

  if (parse_i64(argv[2], &k64) || parse_i64(argv[3], &limit)
   || (argc > 4 && parse_dbl(argv[4], &alpha))
   || (argc > 5 && parse_i64(argv[5], &B))
   || (argc > 6 && parse_i64(argv[6], &T))
   || (argc > 7 && parse_i64(argv[7], &lmin))
   || (argc > 8 && parse_i64(argv[8], &lmax))
   || (argc > 9 && parse_dbl(argv[9], &lam)))
  {
    fprintf(stderr, "spg chains: invalid numeric argument\n");
    return 2;
  }

  B = (argc > 5) ? B : 200000;
  T = (argc > 6) ? T : 20;

  if (k64 < 0 || k64 > UINT32_MAX)
  {
    fprintf(stderr, "spg chains: K out of range\n");
    return 2;
  }

  /* H3: validate before the model load, same messages as genchains.py */
  if (B <= 0)
  {
    fprintf(stderr, "error: B (beam width) must be >= 1, got %lld\n",
            (long long) B);
    return 1;
  }

  if (T <= 0)
  {
    fprintf(stderr,
            "error: T (continuations per seam) must be >= 1, got %lld\n",
            (long long) T);
    return 1;
  }

  if (!(lam >= 0.0 && lam <= 1.0))
  {
    char fb[64];

    fmt_float(fb, sizeof(fb), lam);
    fprintf(stderr, "error: lam must be in [0, 1], got %s\n", fb);
    return 1;
  }

  sp_model m;

  if (sp_model_load(&m, uni, bi, (uint32_t) k64, 2, 10) != 0)
  {
    fprintf(stderr, "spg chains: failed to load model\n");
    return 1;
  }

  const uint32_t K = m.K;

  if (K == 0)
  {
    fprintf(stderr, "spg chains: empty vocabulary\n");
    sp_model_free(&m);
    return 1;
  }

  /* B3: argpartition kth would go out of bounds for T > K */
  if (T > (int64_t) K)
    T = (int64_t) K;

  const uint32_t T32 = (uint32_t) T;
  const uint64_t KK  = (uint64_t) K * K;

  if (KK - 1 > UINT32_MAX)
  {
    fprintf(stderr, "spg chains: K too large (K*K must fit uint32)\n");
    sp_model_free(&m);
    return 1;
  }

  if (limit <= 0)
  {
    /* stock genchains.py emits nothing for limit <= 0 (the emission slice
     * is empty); skip the pipeline, keep the same observable output */
    fprintf(stderr, "emitted: 0\n");
    sp_model_free(&m);
    return 0;
  }

  const uint64_t lim = (uint64_t) limit;
  const uint64_t Bb  = ((uint64_t) B < KK) ? (uint64_t) B : KK;
  const uint64_t N   = Bb * T32;

  if (N > UINT32_MAX)
  {
    fprintf(stderr, "spg chains: beam too large (B*T must fit uint32)\n");
    sp_model_free(&m);
    return 1;
  }

  /* logp: buffer serves as M (counts scatter), then pjm, then log pjm */
  double *logp = xmalloc((size_t) KK * sizeof(double));

  memset(logp, 0, (size_t) KK * sizeof(double));

  for (uint64_t e = 0; e < m.E; e++)
    logp[(uint64_t) m.bi_i[e] * K + (uint32_t) m.bi_j[e]] = (double) m.bi_cnt[e];

  double total = 0.0;

  for (uint32_t i = 0; i < K; i++)
    total += (double) m.counts[i];   /* integers < 2^53: any order is exact */

  double *u = xmalloc(K * sizeof(double));

  for (uint32_t j = 0; j < K; j++)
    u[j] = (double) m.counts[j] / total;

  const double oma = 1.0 - alpha;

  for (uint32_t i = 0; i < K; i++)
  {
    double *row = logp + (size_t) i * K;
    double  cfi = (double) m.counts[i];

    for (uint32_t j = 0; j < K; j++)
    {
      double pjm = alpha * (row[j] / cfi) + oma * u[j];

      row[j] = log(pjm > 1e-15 ? pjm : 1e-15);
    }
  }

  free(u);

  /* first-seam scores S2 = logu[:,None] + logp, diagonal -inf */
  double *s2 = xmalloc((size_t) KK * sizeof(double));

  for (uint32_t i = 0; i < K; i++)
  {
    const double *row  = logp + (size_t) i * K;
    double       *orow = s2 + (size_t) i * K;
    double        lu   = m.logu[i];

    for (uint32_t j = 0; j < K; j++)
      orow[j] = lu + row[j];
  }

  for (uint32_t i = 0; i < K; i++)
    s2[(size_t) i * K + i] = NEG_INF;

  /* defense in depth (audit): a NaN or +inf score is only possible with a
   * corrupt model (e.g. non-positive unigram counts slipping past a lenient
   * loader) and breaks the selection invariants below -- the top-B fill can
   * come up short and leave `seams` uninitialized, crashing far away from
   * the cause.  Mask such cells like the diagonal. */
  for (uint64_t p = 0; p < KK; p++)
    if (isnan(s2[p]) || s2[p] == INFINITY)
      s2[p] = NEG_INF;

  /* top-Bb seams, canonical order (score desc, flat idx asc) */
  sel_rec *seams = xmalloc((size_t) Bb * sizeof(sel_rec));
  uint64_t ns = 0;

  if (Bb == KK)
  {
    for (uint64_t p = 0; p < KK; p++)
    {
      seams[p].s   = s2[p];
      seams[p].idx = (uint32_t) p;
    }

    ns = KK;
  }
  else
  {
    double *cpy = xmalloc((size_t) KK * sizeof(double));
    double  tau;
    uint64_t n_gt = 0;
    uint64_t need;

    memcpy(cpy, s2, (size_t) KK * sizeof(double));
    tau = kth_desc(cpy, (size_t) KK, (size_t) (Bb - 1));
    free(cpy);

    for (uint64_t p = 0; p < KK; p++)
      if (s2[p] > tau)
        n_gt++;

    need = Bb - n_gt;   /* tied slots, filled by ascending flat idx */

    for (uint64_t p = 0; p < KK; p++)
    {
      if (s2[p] > tau)
      {
        seams[ns].s   = s2[p];
        seams[ns].idx = (uint32_t) p;
        ns++;
      }
    }

    for (uint64_t p = 0; p < KK && need; p++)
    {
      if (s2[p] == tau)
      {
        seams[ns].s   = s2[p];
        seams[ns].idx = (uint32_t) p;
        ns++;
        need--;
      }
    }
  }

  free(s2);

  {
    sel_rec *stmp = xmalloc((size_t) Bb * sizeof(sel_rec));

    rec_sort(seams, stmp, (size_t) ns, 0);
    free(stmp);
  }

  /* candidates: sc[c] = score, kk[c] = w3 index, c = q*T + t in beam order */
  double   *sc = xmalloc((size_t) N * sizeof(double));
  int32_t  *kk = xmalloc((size_t) N * sizeof(int32_t));
  sel_rec  *rec  = xmalloc((size_t) T32 * sizeof(sel_rec));
  sel_rec  *rtmp = xmalloc((size_t) T32 * sizeof(sel_rec));
  double   *rowt = (lam > 0.0) ? xmalloc(K * sizeof(double)) : NULL;
  const int band = (lmin > 0 || lmax < 127);
  const double oma_l = 1.0 - lam;

  /* adaptive block size, same formula as genchains.py (see header) */
  const uint64_t budget = (lam > 0.0) ? 600000000ull : 1200000000ull;
  uint64_t BS = budget / ((uint64_t) K * 8u);

  if (BS < 5000)
    BS = 5000;

  if (BS > 50000)
    BS = 50000;

  for (uint64_t s0 = 0; s0 < Bb; s0 += BS)
  {
    const uint64_t e0 = (s0 + BS < Bb) ? s0 + BS : Bb;

    for (uint64_t q = s0; q < e0; q++)
    {
      const uint32_t f    = seams[q].idx;
      const uint32_t wi   = f / K;
      const uint32_t wj   = f % K;
      const double  *rowj = logp + (size_t) wj * K;
      const double  *vals = rowj;

      if (lam > 0.0)
      {
        const double *rowi = logp + (size_t) wi * K;

        for (uint32_t k = 0; k < K; k++)
        {
          double bj = rowj[k];
          double bi = rowi[k];
          double mx = (bj > bi) ? bj : bi;

          rowt[k] = mx + log(lam * exp(bj - mx) + oma_l * exp(bi - mx));
        }

        vals = rowt;
      }

      row_top_t(vals, K, T32, rec, rtmp);

      const uint64_t base = q * T32;
      const double   s1   = seams[q].s;
      const uint32_t li   = m.wlens[wi];
      const uint32_t lj   = m.wlens[wj];

      for (uint32_t t = 0; t < T32; t++)
      {
        const uint64_t c  = base + t;
        const uint32_t k  = rec[t].idx;
        double         sv = s1 + rec[t].s;

        if (k == wi || k == wj)
          sv = NEG_INF;
        else if (band)
        {
          const int64_t cl = (int64_t) li + lj + m.wlens[k];

          if (cl < lmin || cl > lmax)
            sv = NEG_INF;
        }

        /* defense in depth (audit): a NaN/+inf continuation score (corrupt
         * model; the lam path can make NaN from inf-inf) would break the
         * top-cut fill below and leave `ord` uninitialized -- mask it */
        if (isnan(sv) || sv == INFINITY)
          sv = NEG_INF;

        sc[c] = sv;
        kk[c] = (int32_t) k;
      }
    }
  }

  free(rowt);
  free(rec);
  free(rtmp);
  free(logp);

  /* global top-limit cut with dedup; top-up the shortfall (B2) */
  dedup dd;
  outw  ow;
  uint64_t emitted = 0;
  uint64_t start   = 0;
  uint64_t cut     = (lim < N) ? lim : N;
  double  *scopy   = NULL;
  uint8_t  cbuf[3 * 10 + 1];   /* loader keeps words within 2..10 bytes */

  dedup_init(&dd, (lim < N) ? lim : N);
  ow.buf = xmalloc(OUTBUF_CAP);
  ow.len = 0;

  while (start < N)
  {
    /* canonical selection: top-cut by (score desc, candidate idx desc) */
    sel_rec *ord  = xmalloc((size_t) cut * sizeof(sel_rec));
    sel_rec *otmp = xmalloc((size_t) cut * sizeof(sel_rec));

    if (cut == N)
    {
      for (uint64_t c = 0; c < N; c++)
      {
        ord[c].s   = sc[c];
        ord[c].idx = (uint32_t) c;
      }
    }
    else
    {
      double  tau;
      uint64_t n_gt = 0;
      uint64_t need;
      uint64_t o = 0;

      if (!scopy)
        scopy = xmalloc((size_t) N * sizeof(double));

      memcpy(scopy, sc, (size_t) N * sizeof(double));
      tau = kth_desc(scopy, (size_t) N, (size_t) (cut - 1));

      for (uint64_t c = 0; c < N; c++)
        if (sc[c] > tau)
          n_gt++;

      need = cut - n_gt;   /* tied slots, filled by DESCENDING candidate idx
                              (the reversed-stable-sort tie rule, N2) */

      for (uint64_t c = 0; c < N; c++)
      {
        if (sc[c] > tau)
        {
          ord[o].s   = sc[c];
          ord[o].idx = (uint32_t) c;
          o++;
        }
      }

      for (uint64_t c = N; c-- > 0 && need;)
      {
        if (sc[c] == tau)
        {
          ord[o].s   = sc[c];
          ord[o].idx = (uint32_t) c;
          o++;
          need--;
        }
      }
    }

    rec_sort(ord, otmp, (size_t) cut, 1);
    free(otmp);

    int dry = 0;

    for (uint64_t cs = start; cs < cut; cs += CHUNK)
    {
      uint64_t ce = (cs + CHUNK < cut) ? cs + CHUNK : cut;

      /* H4: first -inf INSIDE the chunk truncates it (masked candidates
       * form the suffix of the sorted stream, so everything past the
       * first -inf is masked too) */
      for (uint64_t r = cs; r < ce; r++)
      {
        if (ord[r].s == NEG_INF)
        {
          ce  = r;
          dry = 1;
          break;
        }
      }

      for (uint64_t r = cs; r < ce; r++)
      {
        const uint64_t c  = ord[r].idx;
        const uint32_t f  = seams[c / T32].idx;
        const uint32_t wi = f / K;
        const uint32_t wj = f % K;
        const uint32_t k  = (uint32_t) kk[c];
        uint8_t *p = cbuf;

        /* stock genchains.py keeps scanning the chunk after the limit is
         * hit (dedup-ing into `seen` but emitting nothing); that tail is
         * unobservable, so we simply stop */
        if (emitted >= lim)
          break;

        memcpy(p, m.words[wi], m.wlens[wi]);
        p += m.wlens[wi];
        memcpy(p, m.words[wj], m.wlens[wj]);
        p += m.wlens[wj];
        memcpy(p, m.words[k], m.wlens[k]);
        p += m.wlens[k];

        if (dedup_put(&dd, cbuf, (uint32_t) (p - cbuf)))
        {
          ow_line(&ow, cbuf, (size_t) (p - cbuf));
          emitted++;
        }
      }

      if (dry)
        break;
    }

    free(ord);
    start = cut;

    if (dry || emitted >= lim || cut == N)
      break;

    cut += 2 * (lim - emitted);   /* recover deduped slots (B2) */

    if (cut > N)
      cut = N;
  }

  ow_flush(&ow);
  fflush(stdout);

  fprintf(stderr, "emitted: %llu\n", (unsigned long long) emitted);

  free(scopy);
  free(ow.buf);
  dedup_free(&dd);
  free(sc);
  free(kk);
  free(seams);
  sp_model_free(&m);

  return 0;
}
