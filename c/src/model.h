/*
 * model.h - vocabulary model: words + unigram counts + filtered bigrams.
 *
 * Mirrors src/spmodel.py (load_vocab_model). Load priority:
 *   1. npz cache .cache_vocab_K{K}_{minlen}_{maxlen}.npz next to the
 *      unigram TSV, used only when its stored sig matches the current
 *      json.dumps signature of [uni, bi] (basename, size, int(mtime)).
 *   2. cold TSV parse (no cache is written by C).
 *
 * All arrays are little-endian native (x86-64 target).
 */

#ifndef MODEL_H
#define MODEL_H

#include <stdint.h>
#include <stddef.h>

typedef struct
{
  uint32_t  K;                /* vocabulary size */
  char    **words;            /* [K] pointers into words_blob (not NUL-term) */
  uint32_t *wlens;            /* [K] byte lengths */
  uint8_t  *words_blob;       /* words joined by '\n' (npz words_blob layout) */
  uint64_t  words_blob_len;
  int64_t  *counts;           /* [K] unigram counts */
  double   *logu;             /* [K] log(counts / counts.sum()) */
  int32_t  *bi_i;             /* [E] left word index */
  int32_t  *bi_j;             /* [E] right word index */
  int64_t  *bi_cnt;           /* [E] bigram counts */
  uint64_t  E;                /* number of in-vocab bigrams */

  uint32_t *widx;             /* open-addressing map word -> index */
  uint32_t  widx_cap;         /* power of two, slot value 0xffffffff = empty */

  int       source;           /* 0 = npz cache, 1 = tsv parse */
} sp_model;

#define SP_WIDX_EMPTY 0xffffffffu

/* json.dumps([[basename, size, int(mtime)], ...]) for [uni, bi] */
int      sp_model_sig(const char *uni, const char *bi, char *out, size_t cap);

int      sp_model_load(sp_model *m, const char *uni, const char *bi,
                       uint32_t K, int minlen, int maxlen);

void     sp_model_free(sp_model *m);

/* word -> index, or -1 */
int64_t  sp_model_find(const sp_model *m, const char *w, size_t len);

#endif
