/*
 * debug_dump.c - "spg dump UNI BI K": sha256 of every model array,
 * one "name hash" line per array, for the parity gate vs tools/dump_ref.py.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "model.h"
#include "sha256.h"

static void dump_line(const char *name, const void *data, size_t len)
{
  char hex[65];

  sha256_hex(data, len, hex);

  printf("%s %s\n", name, hex);
}

int cmd_dump(int argc, char **argv)
{
  if (argc < 3)
  {
    fprintf(stderr, "usage: spg dump UNI_TSV BI_TSV K [minlen maxlen]\n");
    return 2;
  }

  const char    *uni    = argv[0];
  const char    *bi     = argv[1];
  const uint32_t K      = (uint32_t) strtoul(argv[2], NULL, 10);
  const int      minlen = (argc > 3) ? atoi(argv[3]) : 2;
  const int      maxlen = (argc > 4) ? atoi(argv[4]) : 10;

  sp_model m;

  if (sp_model_load(&m, uni, bi, K, minlen, maxlen) != 0)
  {
    fprintf(stderr, "spg dump: failed to load model\n");
    return 1;
  }

  fprintf(stderr, "source: %s\n", m.source == 0 ? "npz" : "tsv");
  fprintf(stderr, "K: %u  E: %llu\n", m.K, (unsigned long long) m.E);

  char sig[512];

  if (sp_model_sig(uni, bi, sig, sizeof(sig)) != 0)
  {
    fprintf(stderr, "spg dump: sig failed\n");
    sp_model_free(&m);
    return 1;
  }

  dump_line("sig",        sig, strlen(sig));
  dump_line("words_blob", m.words_blob, m.words_blob_len);
  dump_line("counts",     m.counts, (size_t) m.K * 8);
  dump_line("bi_i",       m.bi_i,   (size_t) m.E * 4);
  dump_line("bi_j",       m.bi_j,   (size_t) m.E * 4);
  dump_line("bi_cnt",     m.bi_cnt, (size_t) m.E * 8);
  dump_line("logu",       m.logu,   (size_t) m.K * 8);

  sp_model_free(&m);

  return 0;
}
