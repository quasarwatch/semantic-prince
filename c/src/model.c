/*
 * model.c - vocabulary model loader (port of src/spmodel.py).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/stat.h>

#include "model.h"
#include "npz.h"

static const char *path_base(const char *p)
{
  const char *s = strrchr(p, '/');

  return s ? s + 1 : p;
}

int sp_model_sig(const char *uni, const char *bi, char *out, size_t cap)
{
  struct stat su;
  struct stat sb;

  if (stat(uni, &su) != 0)
    return -1;

  if (stat(bi, &sb) != 0)
    return -1;

  /* json.dumps defaults: item separator ", ", ASCII basenames assumed */
  const int n = snprintf(out, cap,
                         "[[\"%s\", %lld, %lld], [\"%s\", %lld, %lld]]",
                         path_base(uni),
                         (long long) su.st_size, (long long) su.st_mtime,
                         path_base(bi),
                         (long long) sb.st_size, (long long) sb.st_mtime);

  return (n > 0 && (size_t) n < cap) ? 0 : -1;
}

static uint64_t fnv1a(const char *s, size_t n)
{
  uint64_t h = 14695981039346656037ull;

  for (size_t i = 0; i < n; i++)
  {
    h ^= (uint8_t) s[i];
    h *= 1099511628211ull;
  }

  return h;
}

static int widx_build(sp_model *m)
{
  uint32_t cap = 16;

  while (cap < m->K * 2)
    cap *= 2;

  m->widx = malloc(cap * sizeof(uint32_t));

  if (!m->widx)
    return -1;

  m->widx_cap = cap;

  for (uint32_t i = 0; i < cap; i++)
    m->widx[i] = SP_WIDX_EMPTY;

  for (uint32_t i = 0; i < m->K; i++)
  {
    uint32_t pos = (uint32_t) (fnv1a(m->words[i], m->wlens[i]) & (cap - 1));

    while (m->widx[pos] != SP_WIDX_EMPTY)
      pos = (pos + 1) & (cap - 1);

    m->widx[pos] = i;
  }

  return 0;
}

int64_t sp_model_find(const sp_model *m, const char *w, size_t len)
{
  if (!m->widx)
    return -1;

  uint32_t pos = (uint32_t) (fnv1a(w, len) & (m->widx_cap - 1));

  while (m->widx[pos] != SP_WIDX_EMPTY)
  {
    const uint32_t i = m->widx[pos];

    if (m->wlens[i] == len && !memcmp(m->words[i], w, len))
      return (int64_t) i;

    pos = (pos + 1) & (m->widx_cap - 1);
  }

  return -1;
}

static int compute_logu(sp_model *m)
{
  /* numpy sums the float64 counts with pairwise reduction; every count is
   * an integer < 2^53, so any summation order gives the exact same total */
  double s = 0.0;

  for (uint32_t i = 0; i < m->K; i++)
    s += (double) m->counts[i];

  m->logu = malloc(m->K * sizeof(double));

  if (!m->logu)
    return -1;

  for (uint32_t i = 0; i < m->K; i++)
    m->logu[i] = log((double) m->counts[i] / s);

  return 0;
}

static int split_words_blob(sp_model *m)
{
  /* words_blob is words joined by '\n' (no trailing separator) */
  m->words = malloc(m->K * sizeof(char *));
  m->wlens = malloc(m->K * sizeof(uint32_t));

  if (!m->words || !m->wlens)
    return -1;

  uint64_t pos = 0;

  for (uint32_t i = 0; i < m->K; i++)
  {
    uint64_t end = pos;

    while (end < m->words_blob_len && m->words_blob[end] != '\n')
      end++;

    m->words[i] = (char *) (m->words_blob + pos);
    m->wlens[i] = (uint32_t) (end - pos);

    pos = end + 1;
  }

  /* the blob must hold exactly K words: K-1 separators, no trailing one */
  return (pos == m->words_blob_len + 1) ? 0 : -1;
}

/* TSV count field: a positive decimal integer with optional surrounding
 * whitespace (python int() semantics).  Returns 0 on success; -1 on junk
 * or count <= 0 -- python raises on int() failures, and counts <= 0 make
 * log(counts/sum) NaN, which downstream selection turns into wild indexes. */
static int parse_count(const char *s, int64_t *out)
{
  char *end = NULL;
  const long long v = strtoll(s, &end, 10);

  if (end == s || v <= 0)
    return -1;

  while (*end == ' ' || *end == '\t' || *end == '\r'
      || *end == '\n' || *end == '\v' || *end == '\f')
    end++;

  if (*end)
    return -1;

  *out = (int64_t) v;

  return 0;
}

static int64_t *copy_i8(const npy_array_t *a)
{
  if (!a || a->ndim != 1 || strcmp(a->descr, "<i8"))
    return NULL;

  int64_t *r = malloc((size_t) a->data_len);

  if (r)
    memcpy(r, a->data, (size_t) a->data_len);

  return r;
}

static int32_t *copy_i4(const npy_array_t *a)
{
  if (!a || a->ndim != 1 || strcmp(a->descr, "<i4"))
    return NULL;

  int32_t *r = malloc((size_t) a->data_len);

  if (r)
    memcpy(r, a->data, (size_t) a->data_len);

  return r;
}

static uint8_t *copy_u1(const npy_array_t *a)
{
  if (!a || a->ndim != 1 || (strcmp(a->descr, "|u1") && strcmp(a->descr, "u1")))
    return NULL;

  uint8_t *r = malloc((size_t) a->data_len);

  if (r)
    memcpy(r, a->data, (size_t) a->data_len);

  return r;
}

/* decode a <U{n} UTF-32LE array into ASCII; -1 if undecodable for us */
static int decode_sig(const npy_array_t *a, char *out, size_t cap)
{
  if (!a || a->ndim != 1 || a->shape[0] != 1)
    return -1;

  if (a->descr[0] != '<' || a->descr[1] != 'U')
    return -1;

  const uint64_t n = a->elem_size / 4;

  if (n >= cap)
    return -1;

  size_t o = 0;

  for (uint64_t i = 0; i < n; i++)
  {
    const uint8_t *p = a->data + i * 4;

    const uint32_t cp = (uint32_t) p[0]
                      | ((uint32_t) p[1] << 8)
                      | ((uint32_t) p[2] << 16)
                      | ((uint32_t) p[3] << 24);

    if (cp == 0)
      break;                /* numpy pads fixed-width strings with NULs */

    if (cp > 127)
      return -1;

    out[o++] = (char) cp;
  }

  out[o] = 0;

  return 0;
}

static void cache_path(const char *uni, uint32_t K, int minlen, int maxlen,
                       char *out, size_t cap)
{
  char suffix[64];

  snprintf(suffix, sizeof(suffix), "/.cache_vocab_K%u_%d_%d.npz",
           K, minlen, maxlen);

  const char *s = strrchr(uni, '/');

  size_t dlen = s ? (size_t) (s - uni) : 1;  /* no slash -> "." */

  if (dlen + strlen(suffix) + 1 > cap)
  {
    out[0] = 0;
    return;
  }

  if (s)
    memcpy(out, uni, dlen);
  else
    out[0] = '.';

  strcpy(out + dlen, suffix);
}

/* cached arrays come from a file we did not write: check every index,
 * count and word length before they can reach a downstream indexer
 * (pairs/chains/targeted trust bi_i/bi_j < K and wlens in [minlen, maxlen]) */
static int cache_fields_sane(const sp_model *m, int minlen, int maxlen)
{
  for (uint32_t i = 0; i < m->K; i++)
  {
    if ((int64_t) m->wlens[i] < (int64_t) minlen
     || (int64_t) m->wlens[i] > (int64_t) maxlen)
      return 0;

    if (m->counts[i] <= 0)
      return 0;
  }

  for (uint64_t e = 0; e < m->E; e++)
  {
    if ((uint32_t) m->bi_i[e] >= m->K || (uint32_t) m->bi_j[e] >= m->K)
      return 0;

    if (m->bi_cnt[e] <= 0)
      return 0;
  }

  return 1;
}

static int load_npz(sp_model *m, const char *path, const char *sig,
                    int minlen, int maxlen)
{
  npz_t *z = npz_open(path);

  if (!z)
    return -1;

  int rc = -1;
  char cached_sig[512];

  const npy_array_t *a_sig  = npz_get(z, "sig");
  const npy_array_t *a_blob = npz_get(z, "words_blob");
  const npy_array_t *a_cnt  = npz_get(z, "counts");
  const npy_array_t *a_bi   = npz_get(z, "bi_i");
  const npy_array_t *a_bj   = npz_get(z, "bi_j");
  const npy_array_t *a_bc   = npz_get(z, "bi_cnt");

  if (decode_sig(a_sig, cached_sig, sizeof(cached_sig)) == 0
   && !strcmp(cached_sig, sig)
   && a_blob && a_cnt && a_bi && a_bj && a_bc)
  {
    m->K = (uint32_t) (a_cnt->data_len / 8);
    m->E = a_bc->data_len / 8;

    m->words_blob     = copy_u1(a_blob);
    m->words_blob_len = a_blob->data_len;
    m->counts         = copy_i8(a_cnt);
    m->bi_i           = copy_i4(a_bi);
    m->bi_j           = copy_i4(a_bj);
    m->bi_cnt         = copy_i8(a_bc);

    if (m->words_blob && m->counts && m->bi_i && m->bi_j && m->bi_cnt
     && a_bi->data_len == a_bj->data_len
     && a_bi->data_len / 4 == m->E
     && m->K > 0
     && split_words_blob(m) == 0
     && cache_fields_sane(m, minlen, maxlen)
     && compute_logu(m) == 0)
    {
      m->source = 0;
      rc = 0;
    }
    else
    {
      /* sig matches but the payload is unusable: rebuild from the TSVs,
       * same as python's "corrupt cache -> rebuild" fallback */
      fprintf(stderr, "spg: %s: corrupt model cache, ignoring\n",
              path_base(path));
    }
  }

  npz_close(z);

  return rc;
}

static int load_tsv_uni(sp_model *m, const char *uni, uint32_t K,
                        int minlen, int maxlen)
{
  FILE *f = fopen(uni, "r");

  if (!f)
    return -1;

  m->counts = malloc(K * sizeof(int64_t));

  size_t blob_cap = (size_t) K * 12;

  m->words_blob = malloc(blob_cap);

  if (!m->counts || !m->words_blob)
  {
    fclose(f);
    return -1;
  }

  m->words = malloc(K * sizeof(char *));
  m->wlens = malloc(K * sizeof(uint32_t));

  if (!m->words || !m->wlens)
  {
    fclose(f);
    return -1;
  }

  char  *line = NULL;
  size_t lcap = 0;

  m->K = 0;

  while (m->K < K && getline(&line, &lcap, f) > 0)
  {
    char *tab = strchr(line, '\t');

    if (!tab)
      continue;

    const size_t wlen = (size_t) (tab - line);

    /* compare in int64_t: casting a >2GB wlen to int would wrap the check */
    if ((int64_t) wlen < (int64_t) minlen || (int64_t) wlen > (int64_t) maxlen)
      continue;

    if (parse_count(tab + 1, &m->counts[m->K]) != 0)
    {
      fprintf(stderr, "spg: %s: malformed or non-positive count\n",
              path_base(uni));
      free(line);
      fclose(f);
      return -1;
    }

    if (wlen + 1 > blob_cap - m->words_blob_len)
    {
      /* one doubling may not be enough for a long word; grow until it fits */
      while (blob_cap - m->words_blob_len < wlen + 1)
      {
        if (blob_cap > SIZE_MAX / 2)
        {
          fprintf(stderr, "spg: %s: word too long\n", path_base(uni));
          free(line);
          fclose(f);
          return -1;
        }

        blob_cap *= 2;
      }

      uint8_t *nb = realloc(m->words_blob, blob_cap);

      if (!nb)
        break;

      m->words_blob = nb;
    }

    if (m->K > 0)
      m->words_blob[m->words_blob_len++] = '\n';

    memcpy(m->words_blob + m->words_blob_len, line, wlen);
    m->words_blob_len += wlen;

    m->K++;
  }

  free(line);
  fclose(f);

  if (m->K == 0)
    return -1;

  /* rebuild word pointers against the final blob */
  uint64_t pos = 0;

  for (uint32_t i = 0; i < m->K; i++)
  {
    uint64_t end = pos;

    while (end < m->words_blob_len && m->words_blob[end] != '\n')
      end++;

    m->words[i] = (char *) (m->words_blob + pos);
    m->wlens[i] = (uint32_t) (end - pos);

    pos = end + 1;
  }

  return 0;
}

static int load_tsv_bi(sp_model *m, const char *bi)
{
  FILE *f = fopen(bi, "r");

  if (!f)
    return -1;

  uint64_t cap = 1 << 20;

  m->bi_i   = malloc(cap * sizeof(int32_t));
  m->bi_j   = malloc(cap * sizeof(int32_t));
  m->bi_cnt = malloc(cap * sizeof(int64_t));

  if (!m->bi_i || !m->bi_j || !m->bi_cnt)
  {
    fclose(f);
    return -1;
  }

  char  *line = NULL;
  size_t lcap = 0;
  ssize_t n;

  m->E = 0;

  while ((n = getline(&line, &lcap, f)) > 0)
  {
    if (n > 0 && line[n - 1] == '\n')
      line[--n] = 0;

    char *tab = strrchr(line, '\t');   /* python: rsplit("\t", 1) */

    if (!tab)
      continue;

    *tab = 0;

    /* pair.split(" ") must give exactly 2 parts: exactly one space */
    const char *sp = strchr(line, ' ');

    if (!sp || strchr(sp + 1, ' '))
      continue;

    const int64_t i = sp_model_find(m, line, (size_t) (sp - line));
    const int64_t j = sp_model_find(m, sp + 1, strlen(sp + 1));

    if (i < 0 || j < 0)
      continue;

    int64_t cnt;

    if (parse_count(tab + 1, &cnt) != 0)
    {
      fprintf(stderr, "spg: %s: malformed or non-positive count\n",
              path_base(bi));
      free(line);
      fclose(f);
      return -1;
    }

    if (m->E == cap)
    {
      cap *= 2;

      int32_t *ni = realloc(m->bi_i,   cap * sizeof(int32_t));
      int32_t *nj = realloc(m->bi_j,   cap * sizeof(int32_t));
      int64_t *nc = realloc(m->bi_cnt, cap * sizeof(int64_t));

      if (!ni || !nj || !nc)
      {
        free(ni ? ni : m->bi_i);
        free(nj ? nj : m->bi_j);
        free(nc ? nc : m->bi_cnt);
        m->bi_i = m->bi_j = NULL;
        m->bi_cnt = NULL;
        break;
      }

      m->bi_i   = ni;
      m->bi_j   = nj;
      m->bi_cnt = nc;
    }

    m->bi_i[m->E]   = (int32_t) i;
    m->bi_j[m->E]   = (int32_t) j;
    m->bi_cnt[m->E] = cnt;
    m->E++;
  }

  free(line);
  fclose(f);

  return (m->bi_i && m->bi_j && m->bi_cnt) ? 0 : -1;
}

int sp_model_load(sp_model *m, const char *uni, const char *bi,
                  uint32_t K, int minlen, int maxlen)
{
  memset(m, 0, sizeof(*m));

  char sig[512];

  if (sp_model_sig(uni, bi, sig, sizeof(sig)) != 0)
    return -1;

  char cpath[4096];

  cache_path(uni, K, minlen, maxlen, cpath, sizeof(cpath));

  if (load_npz(m, cpath, sig, minlen, maxlen) == 0)
    goto out_widx;

  sp_model_free(m);
  memset(m, 0, sizeof(*m));

  if (load_tsv_uni(m, uni, K, minlen, maxlen) != 0)
    goto fail;

  if (widx_build(m) != 0)
    goto fail;

  if (load_tsv_bi(m, bi) != 0)
    goto fail;

  if (compute_logu(m) != 0)
    goto fail;

  m->source = 1;

  return 0;

fail:
  sp_model_free(m);

  return -1;

out_widx:
  if (widx_build(m) != 0)
  {
    sp_model_free(m);
    return -1;
  }

  return 0;
}

void sp_model_free(sp_model *m)
{
  free(m->words);
  free(m->wlens);
  free(m->words_blob);
  free(m->counts);
  free(m->logu);
  free(m->bi_i);
  free(m->bi_j);
  free(m->bi_cnt);
  free(m->widx);

  m->words      = NULL;
  m->wlens      = NULL;
  m->words_blob = NULL;
  m->counts     = NULL;
  m->logu       = NULL;
  m->bi_i       = NULL;
  m->bi_j       = NULL;
  m->bi_cnt     = NULL;
  m->widx       = NULL;
  m->K          = 0;
  m->E          = 0;
}
