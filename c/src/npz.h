/*
 * npz.h - minimal read-only npz (zip STORED) + npy v1/v2 parser.
 *
 * np.savez writes zip entries uncompressed (STORED, method 0), so no
 * inflate is needed. Only little-endian dtypes are accepted.
 */

#ifndef NPZ_H
#define NPZ_H

#include <stdint.h>
#include <stddef.h>

#define NPY_MAX_DIM 8

typedef struct
{
  char           name[128];    /* entry name without ".npy" suffix */
  char           descr[16];    /* "<i8", "<i4", "|u1", "<U75", ... */
  int            fortran;      /* fortran_order flag (must be 0 for us) */
  int            ndim;
  uint64_t       shape[NPY_MAX_DIM];
  uint64_t       elem_size;    /* bytes per element */
  const uint8_t *data;         /* payload, points into the npz buffer */
  uint64_t       data_len;     /* payload size in bytes */
} npy_array_t;

typedef struct
{
  uint8_t    *buf;
  size_t      len;
  npy_array_t *arr;
  int         n_arr;
} npz_t;

npz_t           *npz_open(const char *path);
void             npz_close(npz_t *z);

/* lookup by name with or without the ".npy" suffix */
const npy_array_t *npz_get(const npz_t *z, const char *name);

/* number of elements (product of shape) */
uint64_t         npy_nelem(const npy_array_t *a);

#endif
