/*
 * npz.c - minimal read-only npz (zip STORED) + npy v1/v2 parser.
 *
 * Layout used:
 *   zip: EOCD -> central directory -> local headers -> data
 *   npy: \x93NUMPY ver hlen header-dict payload
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "npz.h"

#define ZIP_LOC_SIG 0x04034b50u
#define ZIP_CEN_SIG 0x02014b50u
#define ZIP_EOC_SIG 0x06054b50u

static uint16_t rd16(const uint8_t *p)
{
  return (uint16_t) ((uint16_t) p[0] | ((uint16_t) p[1] << 8));
}

static uint32_t rd32(const uint8_t *p)
{
  return (uint32_t) p[0]
       | ((uint32_t) p[1] << 8)
       | ((uint32_t) p[2] << 16)
       | ((uint32_t) p[3] << 24);
}

uint64_t npy_nelem(const npy_array_t *a)
{
  uint64_t n = 1;

  for (int i = 0; i < a->ndim; i++)
    n *= a->shape[i];

  return n;
}

/* parse dtype size from descr: <i8 <i4 <f8 |u1 u1 <U75 <S10 */
static int dtype_size(const char *d, uint64_t *sz)
{
  const char *p = d;

  if (*p == '<' || *p == '|' || *p == '=')
    p++;
  else if (*p == '>')
    return -1;              /* big-endian unsupported */

  if (*p == 'i' || *p == 'u' || *p == 'f')
  {
    *sz = (uint64_t) atoi(p + 1);
    return (*sz > 0) ? 0 : -1;
  }

  if (*p == 'U')            /* UTF-32LE, 4 bytes per char */
  {
    *sz = 4u * (uint64_t) atoi(p + 1);
    return (*sz > 0) ? 0 : -1;
  }

  if (*p == 'S')            /* byte string */
  {
    *sz = (uint64_t) atoi(p + 1);
    return (*sz > 0) ? 0 : -1;
  }

  return -1;
}

/* extract a quoted value for key from the npy header dict literal */
static int hdr_str(const char *hdr, const char *key, char *out, size_t cap)
{
  const char *p = strstr(hdr, key);

  if (!p)
    return -1;

  p = strchr(p + strlen(key), ':');

  if (!p)
    return -1;

  p++;

  while (*p == ' ')
    p++;

  if (*p != '\'' && *p != '"')
    return -1;

  const char q = *p++;

  const char *e = strchr(p, q);

  if (!e || (size_t) (e - p) >= cap)
    return -1;

  memcpy(out, p, (size_t) (e - p));
  out[e - p] = 0;

  return 0;
}

static int hdr_fortran(const char *hdr, int *fortran)
{
  const char *p = strstr(hdr, "'fortran_order'");

  if (!p)
    return -1;

  p = strchr(p, ':');

  if (!p)
    return -1;

  p++;

  while (*p == ' ')
    p++;

  if (!strncmp(p, "True", 4))
    *fortran = 1;
  else if (!strncmp(p, "False", 5))
    *fortran = 0;
  else
    return -1;

  return 0;
}

static int hdr_shape(const char *hdr, npy_array_t *a)
{
  const char *p = strstr(hdr, "'shape'");

  if (!p)
    return -1;

  p = strchr(p, '(');

  if (!p)
    return -1;

  p++;

  a->ndim = 0;

  for (;;)
  {
    while (*p == ' ' || *p == ',')
      p++;

    if (*p == ')')
      break;

    if (*p < '0' || *p > '9')
      return -1;

    if (a->ndim >= NPY_MAX_DIM)
      return -1;

    char *end = NULL;

    a->shape[a->ndim++] = strtoull(p, &end, 10);

    if (end == p)
      return -1;

    p = end;
  }

  return 0;
}

static int npy_parse(const uint8_t *p, size_t avail, npy_array_t *a)
{
  if (avail < 10)
    return -1;

  if (memcmp(p, "\x93NUMPY", 6))
    return -1;

  const int major = p[6];

  uint64_t hlen;
  size_t   off;

  if (major == 1)
  {
    hlen = rd16(p + 8);
    off  = 10;
  }
  else if (major == 2 || major == 3)
  {
    if (avail < 12)
      return -1;

    hlen = rd32(p + 8);
    off  = 12;
  }
  else
  {
    return -1;
  }

  if (off + hlen > avail)
    return -1;

  /* header is an ASCII/latin1 python dict literal, NUL-copy it */
  char *hdr = malloc(hlen + 1);

  if (!hdr)
    return -1;

  memcpy(hdr, p + off, hlen);
  hdr[hlen] = 0;

  int rc = -1;

  if (hdr_str(hdr, "'descr'", a->descr, sizeof(a->descr)) == 0
   && hdr_fortran(hdr, &a->fortran) == 0
   && hdr_shape(hdr, a) == 0
   && dtype_size(a->descr, &a->elem_size) == 0)
  {
    a->data     = p + off + hlen;
    a->data_len = avail - off - hlen;

    if (npy_nelem(a) * a->elem_size == a->data_len)
      rc = 0;
  }

  free(hdr);

  return rc;
}

npz_t *npz_open(const char *path)
{
  FILE *f = fopen(path, "rb");

  if (!f)
    return NULL;

  npz_t *z = calloc(1, sizeof(*z));

  if (!z)
  {
    fclose(f);
    return NULL;
  }

  if (fseek(f, 0, SEEK_END) == 0)
  {
    const long sz = ftell(f);

    if (sz > 0)
    {
      z->buf = malloc((size_t) sz);

      if (z->buf)
      {
        z->len = (size_t) sz;
        rewind(f);

        if (fread(z->buf, 1, z->len, f) != z->len)
        {
          free(z->buf);
          z->buf = NULL;
          z->len = 0;
        }
      }
    }
  }

  fclose(f);

  if (!z->buf)
  {
    free(z);
    return NULL;
  }

  /* locate EOCD: scan the trailing 64K + 22 */
  const size_t scan = (z->len < 66000) ? z->len : 66000;
  const uint8_t *eoc = NULL;

  for (size_t i = z->len - scan; i + 22 <= z->len; i++)
  {
    if (rd32(z->buf + i) == ZIP_EOC_SIG)
      eoc = z->buf + i;     /* keep last match */
  }

  if (!eoc)
    goto fail;

  const uint16_t n_cd  = rd16(eoc + 10);
  const uint32_t cd_of = rd32(eoc + 16);

  if ((uint64_t) cd_of + (uint64_t) n_cd * 46 > z->len)
    goto fail;

  z->arr = calloc(n_cd, sizeof(npy_array_t));

  if (!z->arr)
    goto fail;

  const uint8_t *c = z->buf + cd_of;

  for (int n = 0; n < n_cd; n++)
  {
    if ((size_t) (c - z->buf) + 46 > z->len || rd32(c) != ZIP_CEN_SIG)
      goto fail;

    const uint16_t method = rd16(c + 10);
    const uint32_t csize  = rd32(c + 20);
    const uint32_t usize  = rd32(c + 24);
    const uint16_t nlen   = rd16(c + 28);
    const uint16_t elen   = rd16(c + 30);
    const uint16_t comlen = rd16(c + 32);
    const uint32_t lho    = rd32(c + 42);

    if (method != 0 || csize != usize)  /* STORED only */
      goto fail;

    if ((uint64_t) lho + 30 > z->len || rd32(z->buf + lho) != ZIP_LOC_SIG)
      goto fail;

    const uint8_t *lh     = z->buf + lho;
    const uint16_t l_nlen = rd16(lh + 26);
    const uint16_t l_elen = rd16(lh + 28);
    const uint64_t doff   = (uint64_t) lho + 30 + l_nlen + l_elen;

    if (doff + csize > z->len)
      goto fail;

    npy_array_t *a = &z->arr[z->n_arr];

    if (nlen >= sizeof(a->name))
      goto fail;

    memcpy(a->name, c + 46, nlen);
    a->name[nlen] = 0;

    /* strip the ".npy" suffix */
    const size_t nl = strlen(a->name);

    if (nl > 4 && !strcmp(a->name + nl - 4, ".npy"))
      a->name[nl - 4] = 0;

    if (npy_parse(z->buf + doff, csize, a) != 0)
      goto fail;

    z->n_arr++;

    c += 46 + nlen + elen + comlen;
  }

  return z;

fail:
  npz_close(z);
  return NULL;
}

void npz_close(npz_t *z)
{
  if (!z)
    return;

  free(z->arr);
  free(z->buf);
  free(z);
}

const npy_array_t *npz_get(const npz_t *z, const char *name)
{
  for (int i = 0; i < z->n_arr; i++)
    if (!strcmp(z->arr[i].name, name))
      return &z->arr[i];

  return NULL;
}
