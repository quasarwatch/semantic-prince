/* spg evaluate - streaming guess-curve evaluator.
 *
 * Exact port of src/evaluate.py:
 *   - test set loaded from file, lines rstripped of \r\n (bugfix H7),
 *     blank / whitespace-only lines skipped (python `if l.strip()`);
 *   - candidates streamed from stdin, rstripped of \r\n, every line
 *     counts toward n (including empty ones);
 *   - unique hits counted via a dedup set;
 *   - checkpoints printed when n hits 100K / 1M / 10M / 100M exactly,
 *     before the limit break;
 *   - output format byte-compatible with the python version.
 *
 * No external dependencies: open-addressing hash set with FNV-1a.
 *
 * Build (standalone):  gcc -std=gnu99 -O2 -DSPG_EVALUATE_STANDALONE src/evaluate.c -o spg_eval
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "evaluate.h"

#define HSET_MIN_CAP 1024u
#define IO_BUF_CAP   (8u * 1024u * 1024u)

/* ------------------------------------------------------------------ */
/* hash set: open addressing, linear probing, FNV-1a 64-bit           */
/* ------------------------------------------------------------------ */

typedef struct
{
  char   **keys;   /* owned copies, NULL = empty slot */
  uint32_t *lens;
  uint64_t *hashes;
  size_t    cap;   /* power of two */
  size_t    size;
} hset;

static uint64_t fnv1a(const char *p, size_t n)
{
  uint64_t h = 1469598103934665603ULL;
  size_t i;

  for (i = 0; i < n; i++)
  {
    h ^= (unsigned char)p[i];
    h *= 1099511628211ULL;
  }
  return h;
}

static int hset_init(hset *s, size_t cap)
{
  if (cap < HSET_MIN_CAP)
    cap = HSET_MIN_CAP;
  s->keys   = calloc(cap, sizeof(char *));
  s->lens   = malloc(cap * sizeof(uint32_t));
  s->hashes = malloc(cap * sizeof(uint64_t));
  if (!s->keys || !s->lens || !s->hashes)
    return -1;
  s->cap  = cap;
  s->size = 0;
  return 0;
}

static void hset_free(hset *s)
{
  size_t i;

  for (i = 0; i < s->cap; i++)
    free(s->keys[i]);
  free(s->keys);
  free(s->lens);
  free(s->hashes);
}

/* slot of key, or first empty slot where it would go */
static size_t hset_slot(const hset *s, const char *key, uint32_t len, uint64_t h)
{
  size_t i = (size_t)h & (s->cap - 1);

  while (s->keys[i])
  {
    if (s->hashes[i] == h && s->lens[i] == len &&
        memcmp(s->keys[i], key, len) == 0)
      return i;
    i = (i + 1) & (s->cap - 1);
  }
  return i;
}

static int hset_grow(hset *s)
{
  hset t;
  size_t i;

  if (hset_init(&t, s->cap * 2) != 0)
    return -1;
  for (i = 0; i < s->cap; i++)
  {
    if (s->keys[i])
    {
      size_t j = (size_t)s->hashes[i] & (t.cap - 1);

      while (t.keys[j])
        j = (j + 1) & (t.cap - 1);
      t.keys[j]   = s->keys[i];
      t.lens[j]   = s->lens[i];
      t.hashes[j] = s->hashes[i];
      t.size++;
    }
  }
  free(s->keys);
  free(s->lens);
  free(s->hashes);
  *s = t;
  return 0;
}

/* insert a copy of key; returns 1 if newly inserted, 0 if already there */
static int hset_add(hset *s, const char *key, uint32_t len)
{
  uint64_t h = fnv1a(key, len);
  size_t i;
  char *copy;

  if ((s->size + 1) * 10 >= s->cap * 7)
  {
    if (hset_grow(s) != 0)
      return -1;
  }
  i = hset_slot(s, key, len, h);
  if (s->keys[i])
    return 0;
  copy = malloc(len + 1);
  if (!copy)
    return -1;
  memcpy(copy, key, len);
  copy[len] = '\0';
  s->keys[i]   = copy;
  s->lens[i]   = len;
  s->hashes[i] = h;
  s->size++;
  return 1;
}

static int hset_contains(const hset *s, const char *key, uint32_t len)
{
  uint64_t h = fnv1a(key, len);
  size_t i   = hset_slot(s, key, len, h);

  return s->keys[i] != NULL;
}

/* ------------------------------------------------------------------ */
/* buffered line reader: returns lines without the trailing '\n';     */
/* '\r' is kept (stripped later, python rstrip(b"\r\n") semantics).   */
/* Grows for lines longer than the buffer.                            */
/* ------------------------------------------------------------------ */

typedef struct
{
  FILE  *fp;
  char  *buf;
  size_t cap;
  size_t start;
  size_t end;
} reader;

static int reader_init(reader *r, FILE *fp)
{
  r->buf = malloc(IO_BUF_CAP);
  if (!r->buf)
    return -1;
  r->fp    = fp;
  r->cap   = IO_BUF_CAP;
  r->start = 0;
  r->end   = 0;
  return 0;
}

static void reader_free(reader *r)
{
  free(r->buf);
}

/* 1 = line returned, 0 = EOF, -1 = error */
static int reader_next(reader *r, char **ptr, size_t *len)
{
  for (;;)
  {
    if (r->start < r->end)
    {
      char *nl = memchr(r->buf + r->start, '\n', r->end - r->start);

      if (nl)
      {
        *ptr = r->buf + r->start;
        *len = (size_t)(nl - (r->buf + r->start));
        r->start = (size_t)(nl - r->buf) + 1;
        return 1;
      }
    }
    /* no full line in buffer: compact, maybe grow, refill */
    if (r->start > 0)
    {
      memmove(r->buf, r->buf + r->start, r->end - r->start);
      r->end -= r->start;
      r->start = 0;
    }
    if (r->end == r->cap)
    {
      size_t ncap = r->cap * 2;
      char *nbuf  = realloc(r->buf, ncap);

      if (!nbuf)
        return -1;
      r->buf = nbuf;
      r->cap = ncap;
    }
    {
      size_t got = fread(r->buf + r->end, 1, r->cap - r->end, r->fp);

      if (got == 0)
      {
        if (r->end > r->start)
        {
          /* final line without trailing newline (python yields it too) */
          *ptr = r->buf + r->start;
          *len = r->end - r->start;
          r->start = r->end;
          return 1;
        }
        return ferror(r->fp) ? -1 : 0;
      }
      r->end += got;
    }
  }
}

/* ------------------------------------------------------------------ */
/* helpers                                                            */
/* ------------------------------------------------------------------ */

/* python bytes.strip() whitespace: space \t \n \r \x0b \x0c */
static int is_py_space(unsigned char c)
{
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\x0b' || c == '\x0c';
}

/* rstrip(b"\r\n") */
static size_t rstrip_crlf(char *line, size_t len)
{
  while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == '\n'))
    len--;
  return len;
}

/* decimal with ',' thousands separators, like python f"{v:,}" */
static void fmt_comma(uint64_t v, char *out)
{
  char tmp[24];
  int i = 0, j = 0;

  do
  {
    tmp[i++] = (char)('0' + (v % 10));
    v /= 10;
  }
  while (v);
  while (i > 0)
  {
    out[j++] = tmp[--i];
    if (i > 0 && i % 3 == 0)
      out[j++] = ',';
  }
  out[j] = '\0';
}

/* right-align in width, like python f"{v:>{w},}" */
static void fputs_padded(const char *s, int width, FILE *fp)
{
  int len = (int)strlen(s);

  while (width > len)
  {
    fputc(' ', fp);
    width--;
  }
  fputs(s, fp);
}

/* ------------------------------------------------------------------ */
/* subcommand                                                         */
/* ------------------------------------------------------------------ */

int spg_evaluate(int argc, char **argv)
{
  static const uint64_t checkpoints[4] =
  {
    100000ULL, 1000000ULL, 10000000ULL, 100000000ULL
  };
  const char *test_path;
  uint64_t limit = 100000000ULL;
  uint64_t n = 0;
  int ci = 0;
  FILE *tfp;
  reader rd;
  hset testset, seen;
  char *line;
  size_t len;
  int rc = 1;

  if (argc < 2)
  {
    fprintf(stderr, "usage: spg evaluate test.txt [limit]\n");
    return 2;
  }
  test_path = argv[1];
  if (argc > 2)
  {
    char *endp = NULL;

    limit = strtoull(argv[2], &endp, 10);
    if (!endp || *endp != '\0')
    {
      fprintf(stderr, "spg evaluate: invalid limit: %s\n", argv[2]);
      return 2;
    }
  }

  tfp = fopen(test_path, "rb");
  if (!tfp)
  {
    fprintf(stderr, "spg evaluate: cannot open %s\n", test_path);
    return 1;
  }
  if (hset_init(&testset, HSET_MIN_CAP) != 0 || hset_init(&seen, HSET_MIN_CAP) != 0)
  {
    fprintf(stderr, "spg evaluate: out of memory\n");
    fclose(tfp);
    return 1;
  }
  if (reader_init(&rd, tfp) != 0)
  {
    fprintf(stderr, "spg evaluate: out of memory\n");
    fclose(tfp);
    return 1;
  }

  /* load test set: skip lines whose raw form is all whitespace,
     dedup by full rstripped string */
  for (;;)
  {
    int st = reader_next(&rd, &line, &len);
    size_t i, clen;
    int blank = 1;

    if (st <= 0)
      break;
    for (i = 0; i < len; i++)
    {
      if (!is_py_space((unsigned char)line[i]))
      {
        blank = 0;
        break;
      }
    }
    if (blank)
      continue;
    clen = rstrip_crlf(line, len);
    if (hset_add(&testset, line, (uint32_t)clen) < 0)
    {
      fprintf(stderr, "spg evaluate: out of memory\n");
      goto out;
    }
  }

  fprintf(stderr, "test set: %zu unique passwords\n", testset.size);

  /* stream candidates from stdin */
  reader_free(&rd);
  if (reader_init(&rd, stdin) != 0)
  {
    fprintf(stderr, "spg evaluate: out of memory\n");
    goto out;
  }
  for (;;)
  {
    int st = reader_next(&rd, &line, &len);
    size_t clen;
    char sn[24], sh[24];

    if (st < 0)
    {
      fprintf(stderr, "spg evaluate: read error\n");
      goto out;
    }
    if (st == 0)
      break;
    clen = rstrip_crlf(line, len);
    n++;
    if (hset_contains(&testset, line, (uint32_t)clen))
      hset_add(&seen, line, (uint32_t)clen);
    while (ci < 4 && n == checkpoints[ci])
    {
      double pct = testset.size ? 100.0 * (double)seen.size / (double)testset.size : 0.0;

      fmt_comma(n, sn);
      fmt_comma(seen.size, sh);
      fputs_padded(sn, 12, stdout);
      fputs(" guesses -> ", stdout);
      fputs_padded(sh, 8, stdout);
      fprintf(stdout, " cracked (%.2f%%)\n", pct);
      ci++;
    }
    if (n >= limit)
      break;
  }

  {
    char sn[24], sh[24];
    double pct = testset.size ? 100.0 * (double)seen.size / (double)testset.size : 0.0;

    fmt_comma(n, sn);
    fmt_comma(seen.size, sh);
    fprintf(stdout, "final: %s guesses -> %s cracked (%.2f%%)\n", sn, sh, pct);
  }
  rc = 0;

out:
  reader_free(&rd);
  fclose(tfp);
  hset_free(&testset);
  hset_free(&seen);
  return rc;
}

#ifdef SPG_EVALUATE_STANDALONE
int main(int argc, char **argv)
{
  return spg_evaluate(argc, argv);
}
#endif
