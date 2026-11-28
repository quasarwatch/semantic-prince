/*
 * pairs.c - "spg pairs UNI BI K freq|pwbi LIMIT [alpha] [--no-dedup]".
 *
 * Port of src/genpairs2.py (post bugfixes_v1):
 *   freq: S[i,j] = logu[i] + logu[j]
 *   pwbi: S[i,j] = logu[i] + log(max(alpha*M[i,j]/cf[i] + (1-alpha)*u[j],
 *         1e-15)), JM smoothing, u[j] = cf[j]/cf.sum()
 *   diagonal = -inf (never selected: limit is clamped to space = K*K - K)
 *
 * Score order (plan N2; genpairs2.py fullsort branch
 * "np.argsort(flat, kind=\"stable\")[::-1]"): score DESC, ties by flat
 * index (i*K+j) DESC.  This is one of the two tie modes of the project;
 * GridGen/targeted use the other one (ties by index ASC) - do not mix up.
 * Cross-check on fixture rt/uni.tsv: the exact tie abxx (flat 4) /
 * xxab (flat 40) is emitted as xxab first.
 *
 * C reproduces the order with a stable LSD radix sort over a monotonic
 * uint64 score key (initial array order = ascending flat index, so after
 * the stable sort ties are index-ASC; scanning from the top gives score
 * DESC, ties index-DESC).  numpy's argpartition branch (cut < N/4) is not
 * deterministic on ties; bitwise parity runs use SP_FORCE_FULLSORT=1 on
 * the Python side (plan 5.2).
 *
 * Bugfix B2 (dedup before the limit cut + top-up of the selection depth):
 * Python grows "cut" and re-slices the same deterministic order; the union
 * of emissions equals the first "limit" unique strings of the fully sorted
 * stream.  C emits exactly that in a single top-down pass.
 *
 * The radix sort and the dedup set are generic candidates for the shared
 * topk.c / hashset.c modules of plan 4.1; kept local until chains lands.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "model.h"
#include "pairs.h"

#define OUT_CAP (8u << 20)        /* stdout buffer, 8 MiB */

/* double -> uint64 monotone map: ascending keys == ascending doubles
 * (NaN, never produced here, would sort above +inf, same end as numpy) */
static uint64_t dkey(double d)
{
  uint64_t b;

  memcpy(&b, &d, 8);

  return (b & 0x8000000000000000ull) ? ~b : (b | 0x8000000000000000ull);
}

/* stable LSD radix sort of (key, idx), 8 passes x 8 bits; idx follows key.
 * 0 on success; key[]/idx[] hold the sorted order on return (8 = even). */
static int radix_sort_pairs(uint64_t *key, uint32_t *idx, uint64_t n)
{
  uint64_t *tk = malloc((size_t) n * sizeof(*tk));
  uint32_t *ti = malloc((size_t) n * sizeof(*ti));

  if (!tk || !ti)
  {
    free(tk);
    free(ti);
    return -1;
  }

  uint64_t *sk = key;
  uint64_t *dk = tk;
  uint32_t *si = idx;
  uint32_t *di = ti;

  for (unsigned pass = 0; pass < 8; pass++)
  {
    const unsigned sh = pass * 8;
    uint64_t cnt[256] = {0};

    for (uint64_t p = 0; p < n; p++)
      cnt[(sk[p] >> sh) & 255]++;

    uint64_t sum = 0;

    for (unsigned b = 0; b < 256; b++)
    {
      const uint64_t c = cnt[b];
      cnt[b] = sum;
      sum += c;
    }

    for (uint64_t p = 0; p < n; p++)
    {
      const uint64_t b = (sk[p] >> sh) & 255;
      const uint64_t q = cnt[b]++;

      dk[q] = sk[p];
      di[q] = si[p];
    }

    uint64_t *xk = sk; sk = dk; dk = xk;
    uint32_t *xi = si; si = di; di = xi;
  }

  free(tk);
  free(ti);

  return 0;
}

/* dedup of glued candidate bytes (bugfix "dedup DO cut"): open addressing,
 * slot = (h32, first flat index); h32 == 0 marks empty (hash 0 remapped).
 * Capacity reserved upfront for `limit` entries, load <= 2/3, no rehash. */
typedef struct
{
  uint32_t *h;
  uint32_t *fi;
  uint64_t  mask;
} dedup_t;

static uint32_t dhash(const char *a, size_t la, const char *b, size_t lb)
{
  uint64_t h = 14695981039346656037ull;         /* FNV-1a over a ++ b */

  for (size_t p = 0; p < la; p++)
  {
    h ^= (uint8_t) a[p];
    h *= 1099511628211ull;
  }

  for (size_t p = 0; p < lb; p++)
  {
    h ^= (uint8_t) b[p];
    h *= 1099511628211ull;
  }

  h ^= h >> 32;

  if ((uint32_t) h == 0)
    h = 1;

  return (uint32_t) h;
}

static int dedup_init(dedup_t *d, uint64_t limit)
{
  uint64_t cap = 1u << 16;

  while (cap < limit + limit / 2)
    cap *= 2;

  d->h  = calloc((size_t) cap, sizeof(uint32_t));
  d->fi = malloc((size_t) cap * sizeof(uint32_t));

  if (!d->h || !d->fi)
  {
    free(d->h);
    free(d->fi);
    return -1;
  }

  d->mask = cap - 1;

  return 0;
}

/* glued-string equality: w[ai]++w[aj] == w[bi]++w[bj] as BYTES, regardless
 * of the split point (that is the whole point of the dedup: "ab"+"abc" and
 * "aba"+"bc" glue to the same candidate) */
static int cand_eq(const sp_model *m, uint32_t ai, uint32_t aj,
                   uint32_t bi, uint32_t bj)
{
  if (m->wlens[ai] + m->wlens[aj] != m->wlens[bi] + m->wlens[bj])
    return 0;

  const char *sa[2];
  const char *sb[2];
  uint32_t    la[2];
  uint32_t    lb[2];

  sa[0] = m->words[ai]; sa[1] = m->words[aj];
  sb[0] = m->words[bi]; sb[1] = m->words[bj];
  la[0] = m->wlens[ai]; la[1] = m->wlens[aj];
  lb[0] = m->wlens[bi]; lb[1] = m->wlens[bj];

  uint32_t xa = 0, oa = 0, xb = 0, ob = 0;

  while (xa < 2)
  {
    if (oa == la[xa]) { xa++; oa = 0; continue; }
    if (ob == lb[xb]) { xb++; ob = 0; continue; }

    if (sa[xa][oa] != sb[xb][ob])
      return 0;

    oa++;
    ob++;
  }

  return 1;                     /* equal totals -> both streams exhausted */
}

/* 1 if the glued string w[i]++w[j] was already emitted, else insert (fi) */
static int dedup_seen(dedup_t *d, const sp_model *m, uint32_t fi,
                      uint32_t i, uint32_t j, uint32_t h)
{
  uint64_t pos = h & d->mask;

  for (;;)
  {
    const uint32_t eh = d->h[pos];

    if (eh == 0)
    {
      d->h[pos]  = h;
      d->fi[pos] = fi;
      return 0;
    }

    if (eh == h)
    {
      const uint32_t efi = d->fi[pos];

      if (cand_eq(m, efi / m->K, efi % m->K, i, j))
        return 1;
    }

    pos = (pos + 1) & d->mask;
  }
}

static uint8_t out_buf[OUT_CAP];
static size_t  out_len;

static void emit_pair(const sp_model *m, uint32_t i, uint32_t j)
{
  const uint32_t l1 = m->wlens[i];
  const uint32_t l2 = m->wlens[j];

  if (out_len + l1 + l2 + 1 > OUT_CAP)
  {
    fwrite(out_buf, 1, out_len, stdout);
    out_len = 0;

    /* words are loader-bounded (wlens <= 10), so this cannot trigger on a
     * valid model; direct-write instead of overflowing out_buf if that
     * invariant is ever broken (defense in depth) */
    if ((uint64_t) l1 + l2 + 1 > OUT_CAP)
    {
      fwrite(m->words[i], 1, l1, stdout);
      fwrite(m->words[j], 1, l2, stdout);
      fputc('\n', stdout);
      return;
    }
  }

  memcpy(out_buf + out_len, m->words[i], l1);
  out_len += l1;
  memcpy(out_buf + out_len, m->words[j], l2);
  out_len += l2;
  out_buf[out_len++] = '\n';
}

int cmd_pairs(int argc, char **argv)
{
  if (argc < 5)
  {
    fprintf(stderr,
            "usage: spg pairs UNI_TSV BI_TSV K freq|pwbi LIMIT"
            " [alpha] [--no-dedup]\n");
    return 2;
  }

  const char *uni  = argv[0];
  const char *bi   = argv[1];
  const char *mode = argv[3];
  char       *end  = NULL;

  const long kreq = strtol(argv[2], &end, 10);

  if (!end || *end || kreq <= 0 || kreq > 65535)
  {
    fprintf(stderr, "spg pairs: bad K '%s'\n", argv[2]);
    return 2;
  }

  end = NULL;

  const long long lreq = strtoll(argv[4], &end, 10);

  if (!end || *end)
  {
    fprintf(stderr, "spg pairs: bad limit '%s'\n", argv[4]);
    return 2;
  }

  /* genpairs2.py: ALPHA = float(argv[6]) if argv[6] != "--no-dedup"
   * else 0.7; DEDUP = "--no-dedup" not in argv (any slot) */
  double alpha = 0.7;

  if (argc > 5 && strcmp(argv[5], "--no-dedup"))
  {
    end   = NULL;
    alpha = strtod(argv[5], &end);

    if (!end || *end)
    {
      fprintf(stderr, "spg pairs: bad alpha '%s'\n", argv[5]);
      return 2;
    }
  }

  int dedup = 1;

  for (int a = 0; a < argc; a++)
    if (!strcmp(argv[a], "--no-dedup"))
      dedup = 0;

  sp_model m;

  if (sp_model_load(&m, uni, bi, (uint32_t) kreq, 2, 10) != 0)
  {
    fprintf(stderr, "spg pairs: failed to load model\n");
    return 1;
  }

  const uint64_t K     = m.K;          /* shrinks to the loaded vocab size */
  const uint64_t N     = K * K;        /* K <= 65535 -> fits u32 indexing  */
  const uint64_t space = N - K;        /* non-diagonal pairs (N18)         */
  uint64_t       lim   = (lreq < 0) ? 0 : (uint64_t) lreq;

  if (lim > space)
    lim = space;

  uint64_t *key = malloc((size_t) N * sizeof(*key));
  uint32_t *idx = malloc((size_t) N * sizeof(*idx));

  if (!key || !idx)
  {
    fprintf(stderr, "spg pairs: out of memory (%llu cells)\n",
            (unsigned long long) N);
    free(key);
    free(idx);
    sp_model_free(&m);
    return 1;
  }

  const uint64_t kdiag = dkey(-INFINITY);   /* fill_diagonal(-inf) */

  /* plan 5.3 parity fallback: SP_SCORES_BIN=<file> with raw LE f64
   * logu[K] (+ logp[K*K] for pwbi) from tools/dump_scores.py.  numpy's
   * SIMD log and libm log differ by <=1 ulp on ~4e-6 of inputs (measured:
   * 1/5000 logu cells, 96/25M logp cells at K=5000); the dump makes the
   * run bitwise-identical to the Python reference.  Without it the output
   * may permute inside 1-ulp score groups (sets/metrics unchanged). */
  const char *dump   = getenv("SP_SCORES_BIN");
  double     *scores = m.logu;      /* logu actually used for scoring */
  double     *logp_d = NULL;        /* dumped logp (pwbi), or NULL    */

  if (dump)
  {
    FILE *df = fopen(dump, "rb");

    scores = malloc((size_t) K * sizeof(*scores));

    if (!df || !scores || fread(scores, sizeof(*scores), K, df) != K)
    {
      fprintf(stderr, "spg pairs: bad SP_SCORES_BIN '%s'\n", dump);
      free(scores);
      free(key);
      free(idx);
      sp_model_free(&m);
      return 1;
    }

    if (!strcmp(mode, "pwbi"))
    {
      logp_d = malloc((size_t) N * sizeof(*logp_d));

      if (!logp_d || fread(logp_d, sizeof(*logp_d), N, df) != N)
      {
        fprintf(stderr, "spg pairs: bad SP_SCORES_BIN '%s' (logp)\n", dump);
        free(logp_d);
        free(scores);
        free(key);
        free(idx);
        sp_model_free(&m);
        return 1;
      }
    }

    fclose(df);
  }

  if (!strcmp(mode, "pwbi"))
  {
    if (logp_d)
    {
      for (uint64_t i = 0; i < K; i++)
      {
        const double   lui = scores[i];
        const uint64_t row = i * K;

        for (uint64_t j = 0; j < K; j++)
          key[row + j] = (i == j) ? kdiag : dkey(lui + logp_d[row + j]);
      }

      free(logp_d);
    }
    else
    {
    /* dense count matrix M, reused in place as the key buffer (both 8B);
     * scatter is last-write-wins like numpy fancy-assign (bi pairs are
     * unique in pw_bi.tsv - documented assumption N6) */
    double *M = (double *) key;

    memset(M, 0, (size_t) N * sizeof(double));

    for (uint64_t e = 0; e < m.E; e++)
      M[(uint64_t) (uint32_t) m.bi_i[e] * K + (uint32_t) m.bi_j[e]] =
        (double) m.bi_cnt[e];

    /* cf.sum(): integer counts < 2^53 -> any order gives the exact total */
    double csum = 0.0;

    for (uint64_t i = 0; i < K; i++)
      csum += (double) m.counts[i];

    double *u = malloc((size_t) K * sizeof(*u));

    if (!u)
    {
      fprintf(stderr, "spg pairs: out of memory (u)\n");
      free(key);
      free(idx);
      sp_model_free(&m);
      return 1;
    }

    for (uint64_t j = 0; j < K; j++)
      u[j] = (double) m.counts[j] / csum;

    const double oma = 1.0 - alpha;

    for (uint64_t i = 0; i < K; i++)
    {
      const double   cfi = (double) m.counts[i];
      const double   lui = scores[i];
      const uint64_t row = i * K;

      for (uint64_t j = 0; j < K; j++)
      {
        /* op order identical to numpy: pcond = M/cf, then
         * alpha*pcond + (1-alpha)*u, then max(), then log, then logu+ */
        double pjm = alpha * (M[row + j] / cfi) + oma * u[j];

        if (pjm < 1e-15)
          pjm = 1e-15;

        key[row + j] = (i == j) ? kdiag : dkey(lui + log(pjm));
      }
    }

    free(u);
    }
  }
  else                                    /* freq (any non-"pwbi" mode) */
  {
    for (uint64_t i = 0; i < K; i++)
    {
      const double   lui = scores[i];
      const uint64_t row = i * K;

      for (uint64_t j = 0; j < K; j++)
        key[row + j] = (i == j) ? kdiag : dkey(lui + scores[j]);
    }
  }

  for (uint64_t p = 0; p < N; p++)
    idx[p] = (uint32_t) p;

  if (radix_sort_pairs(key, idx, N) != 0)
  {
    fprintf(stderr, "spg pairs: out of memory (sort)\n");
    free(key);
    free(idx);
    sp_model_free(&m);
    return 1;
  }

  free(key);                              /* only the order is needed now */

  dedup_t  dd;
  dedup_t *pdd = NULL;

  if (dedup && lim > 0)
  {
    if (dedup_init(&dd, lim) != 0)
    {
      fprintf(stderr, "spg pairs: out of memory (dedup)\n");
      free(idx);
      sp_model_free(&m);
      return 1;
    }

    pdd = &dd;
  }

  /* first "lim" unique candidates of the (score desc, idx desc) stream;
   * the K diagonal cells sort last, so the first "space" cells of the
   * reversed order are exactly the non-diagonal pairs */
  uint64_t emitted = 0;
  uint64_t scanned = 0;
  uint64_t p       = N;

  while (scanned < space && emitted < lim)
  {
    const uint32_t fi = idx[--p];
    const uint32_t i  = fi / (uint32_t) K;
    const uint32_t j  = fi % (uint32_t) K;

    scanned++;

    if (pdd)
    {
      const uint32_t h = dhash(m.words[i], m.wlens[i],
                               m.words[j], m.wlens[j]);

      if (dedup_seen(pdd, &m, fi, i, j, h))
        continue;
    }

    emit_pair(&m, i, j);
    emitted++;
  }

  if (out_len)
    fwrite(out_buf, 1, out_len, stdout);

  fflush(stdout);

  fprintf(stderr, "emitted: %llu\n", (unsigned long long) emitted);

  if (pdd)
  {
    free(dd.h);
    free(dd.fi);
  }

  if (scores != m.logu)
    free(scores);

  free(idx);
  sp_model_free(&m);

  return 0;
}
