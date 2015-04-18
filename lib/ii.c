/* -*- c-basic-offset: 2 -*- */
/* Copyright(C) 2009-2015 Brazil

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License version 2.1 as published by the Free Software Foundation.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this library; if not, write to the Free Software
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
*/
#include "grn.h"
#include <stdio.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <math.h>

#ifdef WIN32
# include <io.h>
#endif /* WIN32 */

#include "grn_ii.h"
#include "grn_ctx_impl.h"
#include "grn_token_cursor.h"
#include "grn_pat.h"
#include "grn_db.h"
#include "grn_output.h"
#include "grn_scorer.h"
#include "grn_util.h"

#ifdef GRN_WITH_ONIGMO
# define GRN_II_SELECT_ENABLE_SEQUENTIAL_SEARCH
#endif

#ifdef GRN_II_SELECT_ENABLE_SEQUENTIAL_SEARCH
# include "grn_string.h"
# include <oniguruma.h>
#endif

#define MAX_PSEG                 0x20000
#define S_CHUNK                  (1 << GRN_II_W_CHUNK)
#define W_SEGMENT                18
#define S_SEGMENT                (1 << W_SEGMENT)
#define W_ARRAY_ELEMENT	         3
#define S_ARRAY_ELEMENT	         (1 << W_ARRAY_ELEMENT)
#define W_ARRAY                  (W_SEGMENT - W_ARRAY_ELEMENT)
#define ARRAY_MASK_IN_A_SEGMENT  ((1 << W_ARRAY) - 1)
#define NOT_ASSIGNED             0xffffffff

#define S_GARBAGE                (1<<12)

#define CHUNK_SPLIT              0x80000000
#define CHUNK_SPLIT_THRESHOLD    0x60000

#define MAX_N_ELEMENTS           5

#define LSEG(pos) ((pos) >> 16)
#define LPOS(pos) (((pos) & 0xffff) << 2)
#define SEG2POS(seg,pos) ((((uint32_t)(seg)) << 16) + (((uint32_t)(pos)) >> 2))

#define NEXT_ADDR(p) (((byte *)(p)) + sizeof(*(p)))

#ifndef S_IRUSR
# define S_IRUSR 0400
#endif /* S_IRUSR */
#ifndef S_IWUSR
# define S_IWUSR 0200
#endif /* S_IWUSR */

/* segment */

inline static uint32_t
segment_get(grn_ctx *ctx, grn_ii *ii)
{
  uint32_t pseg;
  if (ii->header->bgqtail == ((ii->header->bgqhead + 1) & (GRN_II_BGQSIZE - 1))) {
    pseg = ii->header->bgqbody[ii->header->bgqtail];
    ii->header->bgqtail = (ii->header->bgqtail + 1) & (GRN_II_BGQSIZE - 1);
  } else {
    pseg = ii->header->pnext;
#ifndef CUT_OFF_COMPATIBILITY
    if (!pseg) {
      int i;
      uint32_t pmax = 0;
      char *used = GRN_CALLOC(MAX_PSEG);
      if (!used) { return MAX_PSEG; }
      for (i = 0; i < GRN_II_MAX_LSEG; i++) {
        if ((pseg = ii->header->ainfo[i]) != NOT_ASSIGNED) {
          if (pseg > pmax) { pmax = pseg; }
          used[pseg] = 1;
        }
        if ((pseg = ii->header->binfo[i]) != NOT_ASSIGNED) {
          if (pseg > pmax) { pmax = pseg; }
          used[pseg] = 1;
        }
      }
      for (pseg = 0; pseg < MAX_PSEG && used[pseg]; pseg++) ;
      GRN_FREE(used);
      ii->header->pnext = pmax + 1;
    } else
#endif /* CUT_OFF_COMPATIBILITY */
    if (ii->header->pnext < MAX_PSEG) { ii->header->pnext++; }
  }
  return pseg;
}

inline static grn_rc
segment_get_clear(grn_ctx *ctx, grn_ii *ii, uint32_t *pseg)
{
  uint32_t seg = segment_get(ctx, ii);
  if (seg < MAX_PSEG) {
    void *p = NULL;
    GRN_IO_SEG_REF(ii->seg, seg, p);
    if (!p) { return GRN_NO_MEMORY_AVAILABLE; }
    memset(p, 0, S_SEGMENT);
    GRN_IO_SEG_UNREF(ii->seg, seg);
    *pseg = seg;
    return GRN_SUCCESS;
  } else {
    return GRN_NO_MEMORY_AVAILABLE;
  }
}

inline static grn_rc
buffer_segment_new(grn_ctx *ctx, grn_ii *ii, uint32_t *segno)
{
  uint32_t lseg, pseg;
  if (*segno < GRN_II_MAX_LSEG) {
    if (ii->header->binfo[*segno] != NOT_ASSIGNED) {
      return GRN_INVALID_ARGUMENT;
    }
    lseg = *segno;
  } else {
    for (lseg = 0; lseg < GRN_II_MAX_LSEG; lseg++) {
      if (ii->header->binfo[lseg] == NOT_ASSIGNED) { break; }
    }
    if (lseg == GRN_II_MAX_LSEG) { return GRN_NO_MEMORY_AVAILABLE; }
    *segno = lseg;
  }
  pseg = segment_get(ctx, ii);
  if (pseg < MAX_PSEG) {
    ii->header->binfo[lseg] = pseg;
    if (lseg >= ii->header->bmax) { ii->header->bmax = lseg + 1; }
    return GRN_SUCCESS;
  } else {
    return GRN_NO_MEMORY_AVAILABLE;
  }
}

static grn_rc
buffer_segment_reserve(grn_ctx *ctx, grn_ii *ii,
                       uint32_t *lseg0, uint32_t *pseg0,
                       uint32_t *lseg1, uint32_t *pseg1)
{
  uint32_t i = 0;
  for (;; i++) {
    if (i == GRN_II_MAX_LSEG) { return GRN_NO_MEMORY_AVAILABLE; }
    if (ii->header->binfo[i] == NOT_ASSIGNED) { break; }
  }
  *lseg0 = i++;
  for (;; i++) {
    if (i == GRN_II_MAX_LSEG) { return GRN_NO_MEMORY_AVAILABLE; }
    if (ii->header->binfo[i] == NOT_ASSIGNED) { break; }
  }
  *lseg1 = i;
  if ((*pseg0 = segment_get(ctx, ii)) == MAX_PSEG) { return GRN_NO_MEMORY_AVAILABLE; }
  if ((*pseg1 = segment_get(ctx, ii)) == MAX_PSEG) { return GRN_NO_MEMORY_AVAILABLE; }
  /*
  {
    uint32_t pseg;
    char *used = GRN_CALLOC(MAX_PSEG);
    if (!used) { return GRN_NO_MEMORY_AVAILABLE; }
    for (i = 0; i < GRN_II_MAX_LSEG; i++) {
      if ((pseg = ii->header->ainfo[i]) != NOT_ASSIGNED) { used[pseg] = 1; }
      if ((pseg = ii->header->binfo[i]) != NOT_ASSIGNED) { used[pseg] = 1; }
    }
    for (pseg = 0;; pseg++) {
      if (pseg == MAX_PSEG) { GRN_FREE(used); return GRN_NO_MEMORY_AVAILABLE; }
      if (!used[pseg]) { break; }
    }
    *pseg0 = pseg++;
    for (;; pseg++) {
      if (pseg == MAX_PSEG) { GRN_FREE(used); return GRN_NO_MEMORY_AVAILABLE; }
      if (!used[pseg]) { break; }
    }
    *pseg1 = pseg;
    GRN_FREE(used);
  }
  */
  return GRN_SUCCESS;
}

#define BGQENQUE(lseg) do {\
  if (ii->header->binfo[lseg] != NOT_ASSIGNED) {\
    ii->header->bgqbody[ii->header->bgqhead] = ii->header->binfo[lseg];\
    ii->header->bgqhead = (ii->header->bgqhead + 1) & (GRN_II_BGQSIZE - 1);\
    GRN_ASSERT(ii->header->bgqhead != ii->header->bgqtail);\
  }\
} while (0)

inline static void
buffer_segment_update(grn_ii *ii, uint32_t lseg, uint32_t pseg)
{
  BGQENQUE(lseg);
  // smb_wmb();
  ii->header->binfo[lseg] = pseg;
  if (lseg >= ii->header->bmax) { ii->header->bmax = lseg + 1; }
}

inline static void
buffer_segment_clear(grn_ii *ii, uint32_t lseg)
{
  BGQENQUE(lseg);
  // smb_wmb();
  ii->header->binfo[lseg] = NOT_ASSIGNED;
}

/* chunk */

#define HEADER_CHUNK_AT(ii,offset) \
  ((((ii)->header->chunks[((offset) >> 3)]) >> ((offset) & 7)) & 1)

#define HEADER_CHUNK_ON(ii,offset) \
  (((ii)->header->chunks[((offset) >> 3)]) |= (1 << ((offset) & 7)))

#define HEADER_CHUNK_OFF(ii,offset) \
  (((ii)->header->chunks[((offset) >> 3)]) &= ~(1 << ((offset) & 7)))

#define N_GARBAGES_TH 1

#define N_GARBAGES ((S_GARBAGE - (sizeof(uint32_t) * 4))/(sizeof(uint32_t)))

typedef struct {
  uint32_t head;
  uint32_t tail;
  uint32_t nrecs;
  uint32_t next;
  uint32_t recs[N_GARBAGES];
} grn_ii_ginfo;

#define WIN_MAP(chunk,ctx,iw,seg,pos,size,mode)\
  grn_io_win_map(chunk, ctx, iw,\
                 ((seg) >> GRN_II_N_CHUNK_VARIATION),\
                 (((seg) & ((1 << GRN_II_N_CHUNK_VARIATION) - 1)) << GRN_II_W_LEAST_CHUNK) + (pos),\
                 size,mode)
/*
static int new_histogram[32];
static int free_histogram[32];
*/
static grn_rc
chunk_new(grn_ctx *ctx, grn_ii *ii, uint32_t *res, uint32_t size)
{
  /*
  if (size) {
    int m, es = size - 1;
    GRN_BIT_SCAN_REV(es, m);
    m++;
    new_histogram[m]++;
  }
  */
  if (size > S_CHUNK) {
    int i, j;
    uint32_t n = (size + S_CHUNK - 1) >> GRN_II_W_CHUNK;
    for (i = 0, j = -1; i < GRN_II_MAX_CHUNK; i++) {
      if (HEADER_CHUNK_AT(ii, i)) {
        j = i;
      } else {
        if (i == j + n) {
          j++;
          *res = j << GRN_II_N_CHUNK_VARIATION;
          for (; j <= i; j++) { HEADER_CHUNK_ON(ii, j); }
          return GRN_SUCCESS;
        }
      }
    }
    GRN_LOG(ctx, GRN_LOG_CRIT, "index full. requested chunk_size=%d.", size);
    return GRN_NO_MEMORY_AVAILABLE;
  } else {
    uint32_t *vp;
    int m, aligned_size;
    if (size > (1 << GRN_II_W_LEAST_CHUNK)) {
      int es = size - 1;
      GRN_BIT_SCAN_REV(es, m);
      m++;
    } else {
      m = GRN_II_W_LEAST_CHUNK;
    }
    aligned_size = 1 << (m - GRN_II_W_LEAST_CHUNK);
    if (ii->header->ngarbages[m - GRN_II_W_LEAST_CHUNK] > N_GARBAGES_TH) {
      grn_ii_ginfo *ginfo;
      uint32_t *gseg;
      grn_io_win iw, iw_;
      iw_.addr = NULL;
      gseg = &ii->header->garbages[m - GRN_II_W_LEAST_CHUNK];
      while (*gseg != NOT_ASSIGNED) {
        ginfo = WIN_MAP(ii->chunk, ctx, &iw, *gseg, 0, S_GARBAGE, grn_io_rdwr);
        //GRN_IO_SEG_MAP2(ii->chunk, *gseg, ginfo);
        if (!ginfo) {
          if (iw_.addr) { grn_io_win_unmap(&iw_); }
          return GRN_NO_MEMORY_AVAILABLE;
        }
        if (ginfo->next != NOT_ASSIGNED || ginfo->nrecs > N_GARBAGES_TH) {
          *res = ginfo->recs[ginfo->tail];
          if (++ginfo->tail == N_GARBAGES) { ginfo->tail = 0; }
          ginfo->nrecs--;
          ii->header->ngarbages[m - GRN_II_W_LEAST_CHUNK]--;
          if (!ginfo->nrecs) {
            HEADER_CHUNK_OFF(ii, *gseg);
            *gseg = ginfo->next;
          }
          if (iw_.addr) { grn_io_win_unmap(&iw_); }
          grn_io_win_unmap(&iw);
          return GRN_SUCCESS;
        }
        if (iw_.addr) { grn_io_win_unmap(&iw_); }
        iw_ = iw;
        gseg = &ginfo->next;
      }
      if (iw_.addr) { grn_io_win_unmap(&iw_); }
    }
    vp = &ii->header->free_chunks[m - GRN_II_W_LEAST_CHUNK];
    if (*vp == NOT_ASSIGNED) {
      int i = 0;
      while (HEADER_CHUNK_AT(ii, i)) {
        if (++i >= GRN_II_MAX_CHUNK) { return GRN_NO_MEMORY_AVAILABLE; }
      }
      HEADER_CHUNK_ON(ii, i);
      *vp = i << GRN_II_N_CHUNK_VARIATION;
    }
    *res = *vp;
    *vp += 1 << (m - GRN_II_W_LEAST_CHUNK);
    if (!(*vp & ((1 << GRN_II_N_CHUNK_VARIATION) - 1))) {
      *vp = NOT_ASSIGNED;
    }
    return GRN_SUCCESS;
  }
}

static grn_rc
chunk_free(grn_ctx *ctx, grn_ii *ii, uint32_t offset, uint32_t dummy, uint32_t size)
{
  /*
  if (size) {
    int m, es = size - 1;
    GRN_BIT_SCAN_REV(es, m);
    m++;
    free_histogram[m]++;
  }
  */
  grn_io_win iw, iw_;
  grn_ii_ginfo *ginfo;
  uint32_t seg, m, *gseg;
  seg = offset >> GRN_II_N_CHUNK_VARIATION;
  if (size > S_CHUNK) {
    int n = (size + S_CHUNK - 1) >> GRN_II_W_CHUNK;
    for (; n--; seg++) { HEADER_CHUNK_OFF(ii, seg); }
    return GRN_SUCCESS;
  }
  if (size > (1 << GRN_II_W_LEAST_CHUNK)) {
    int es = size - 1;
    GRN_BIT_SCAN_REV(es, m);
    m++;
  } else {
    m = GRN_II_W_LEAST_CHUNK;
  }
  gseg = &ii->header->garbages[m - GRN_II_W_LEAST_CHUNK];
  iw_.addr = NULL;
  while (*gseg != NOT_ASSIGNED) {
    ginfo = WIN_MAP(ii->chunk, ctx, &iw, *gseg, 0, S_GARBAGE, grn_io_rdwr);
    // GRN_IO_SEG_MAP2(ii->chunk, *gseg, ginfo);
    if (!ginfo) {
      if (iw_.addr) { grn_io_win_unmap(&iw_); }
      return GRN_NO_MEMORY_AVAILABLE;
    }
    if (ginfo->nrecs < N_GARBAGES) { break; }
    if (iw_.addr) { grn_io_win_unmap(&iw_); }
    iw_ = iw;
    gseg = &ginfo->next;
  }
  if (*gseg == NOT_ASSIGNED) {
    grn_rc rc;
    if ((rc = chunk_new(ctx, ii, gseg, S_GARBAGE))) {
      if (iw_.addr) { grn_io_win_unmap(&iw_); }
      return rc;
    }
    ginfo = WIN_MAP(ii->chunk, ctx, &iw, *gseg, 0, S_GARBAGE, grn_io_rdwr);
    /*
    uint32_t i = 0;
    while (HEADER_CHUNK_AT(ii, i)) {
      if (++i >= GRN_II_MAX_CHUNK) { return GRN_NO_MEMORY_AVAILABLE; }
    }
    HEADER_CHUNK_ON(ii, i);
    *gseg = i;
    GRN_IO_SEG_MAP2(ii->chunk, *gseg, ginfo);
    */
    if (!ginfo) {
      if (iw_.addr) { grn_io_win_unmap(&iw_); }
      return GRN_NO_MEMORY_AVAILABLE;
    }
    ginfo->head = 0;
    ginfo->tail = 0;
    ginfo->nrecs = 0;
    ginfo->next = NOT_ASSIGNED;
  }
  if (iw_.addr) { grn_io_win_unmap(&iw_); }
  ginfo->recs[ginfo->head] = offset;
  if (++ginfo->head == N_GARBAGES) { ginfo->head = 0; }
  ginfo->nrecs++;
  grn_io_win_unmap(&iw);
  ii->header->ngarbages[m - GRN_II_W_LEAST_CHUNK]++;
  return GRN_SUCCESS;
}

#define UNIT_SIZE 0x80
#define UNIT_MASK (UNIT_SIZE - 1)

/* <generated> */
static uint8_t *
pack_1(uint32_t *p, uint8_t *rp)
{
  uint8_t v;
  v = *p++ << 7;
  v += *p++ << 6;
  v += *p++ << 5;
  v += *p++ << 4;
  v += *p++ << 3;
  v += *p++ << 2;
  v += *p++ << 1;
  *rp++ = v + *p++;
  return rp;
}
static uint8_t *
unpack_1(uint32_t *p, uint8_t *dp)
{
  *p++ = (*dp >> 7);
  *p++ = ((*dp >> 6) & 0x1);
  *p++ = ((*dp >> 5) & 0x1);
  *p++ = ((*dp >> 4) & 0x1);
  *p++ = ((*dp >> 3) & 0x1);
  *p++ = ((*dp >> 2) & 0x1);
  *p++ = ((*dp >> 1) & 0x1);
  *p++ = (*dp++ & 0x1);
  return dp;
}
static uint8_t *
pack_2(uint32_t *p, uint8_t *rp)
{
  uint8_t v;
  v = *p++ << 6;
  v += *p++ << 4;
  v += *p++ << 2;
  *rp++ = v + *p++;
  v = *p++ << 6;
  v += *p++ << 4;
  v += *p++ << 2;
  *rp++ = v + *p++;
  return rp;
}
static uint8_t *
unpack_2(uint32_t *p, uint8_t *dp)
{
  *p++ = (*dp >> 6);
  *p++ = ((*dp >> 4) & 0x3);
  *p++ = ((*dp >> 2) & 0x3);
  *p++ = (*dp++ & 0x3);
  *p++ = (*dp >> 6);
  *p++ = ((*dp >> 4) & 0x3);
  *p++ = ((*dp >> 2) & 0x3);
  *p++ = (*dp++ & 0x3);
  return dp;
}
static uint8_t *
pack_3(uint32_t *p, uint8_t *rp)
{
  uint8_t v;
  v = *p++ << 5;
  v += *p++ << 2;
  *rp++ = v + (*p >> 1); v = *p++ << 7;
  v += *p++ << 4;
  v += *p++ << 1;
  *rp++ = v + (*p >> 2); v = *p++ << 6;
  v += *p++ << 3;
  *rp++ = v + *p++;
  return rp;
}
static uint8_t *
unpack_3(uint32_t *p, uint8_t *dp)
{
  uint32_t v;
  *p++ = (*dp >> 5);
  *p++ = ((*dp >> 2) & 0x7);
  v = ((*dp++ << 1) & 0x7); *p++ = v + (*dp >> 7);
  *p++ = ((*dp >> 4) & 0x7);
  *p++ = ((*dp >> 1) & 0x7);
  v = ((*dp++ << 2) & 0x7); *p++ = v + (*dp >> 6);
  *p++ = ((*dp >> 3) & 0x7);
  *p++ = (*dp++ & 0x7);
  return dp;
}
static uint8_t *
pack_4(uint32_t *p, uint8_t *rp)
{
  uint8_t v;
  v = *p++ << 4;
  *rp++ = v + *p++;
  v = *p++ << 4;
  *rp++ = v + *p++;
  v = *p++ << 4;
  *rp++ = v + *p++;
  v = *p++ << 4;
  *rp++ = v + *p++;
  return rp;
}
static uint8_t *
unpack_4(uint32_t *p, uint8_t *dp)
{
  *p++ = (*dp >> 4);
  *p++ = (*dp++ & 0xf);
  *p++ = (*dp >> 4);
  *p++ = (*dp++ & 0xf);
  *p++ = (*dp >> 4);
  *p++ = (*dp++ & 0xf);
  *p++ = (*dp >> 4);
  *p++ = (*dp++ & 0xf);
  return dp;
}
static uint8_t *
pack_5(uint32_t *p, uint8_t *rp)
{
  uint8_t v;
  v = *p++ << 3;
  *rp++ = v + (*p >> 2); v = *p++ << 6;
  v += *p++ << 1;
  *rp++ = v + (*p >> 4); v = *p++ << 4;
  *rp++ = v + (*p >> 1); v = *p++ << 7;
  v += *p++ << 2;
  *rp++ = v + (*p >> 3); v = *p++ << 5;
  *rp++ = v + *p++;
  return rp;
}
static uint8_t *
unpack_5(uint32_t *p, uint8_t *dp)
{
  uint32_t v;
  *p++ = (*dp >> 3);
  v = ((*dp++ << 2) & 0x1f); *p++ = v + (*dp >> 6);
  *p++ = ((*dp >> 1) & 0x1f);
  v = ((*dp++ << 4) & 0x1f); *p++ = v + (*dp >> 4);
  v = ((*dp++ << 1) & 0x1f); *p++ = v + (*dp >> 7);
  *p++ = ((*dp >> 2) & 0x1f);
  v = ((*dp++ << 3) & 0x1f); *p++ = v + (*dp >> 5);
  *p++ = (*dp++ & 0x1f);
  return dp;
}
static uint8_t *
pack_6(uint32_t *p, uint8_t *rp)
{
  uint8_t v;
  v = *p++ << 2;
  *rp++ = v + (*p >> 4); v = *p++ << 4;
  *rp++ = v + (*p >> 2); v = *p++ << 6;
  *rp++ = v + *p++;
  v = *p++ << 2;
  *rp++ = v + (*p >> 4); v = *p++ << 4;
  *rp++ = v + (*p >> 2); v = *p++ << 6;
  *rp++ = v + *p++;
  return rp;
}
static uint8_t *
unpack_6(uint32_t *p, uint8_t *dp)
{
  uint32_t v;
  *p++ = (*dp >> 2);
  v = ((*dp++ << 4) & 0x3f); *p++ = v + (*dp >> 4);
  v = ((*dp++ << 2) & 0x3f); *p++ = v + (*dp >> 6);
  *p++ = (*dp++ & 0x3f);
  *p++ = (*dp >> 2);
  v = ((*dp++ << 4) & 0x3f); *p++ = v + (*dp >> 4);
  v = ((*dp++ << 2) & 0x3f); *p++ = v + (*dp >> 6);
  *p++ = (*dp++ & 0x3f);
  return dp;
}
static uint8_t *
pack_7(uint32_t *p, uint8_t *rp)
{
  uint8_t v;
  v = *p++ << 1;
  *rp++ = v + (*p >> 6); v = *p++ << 2;
  *rp++ = v + (*p >> 5); v = *p++ << 3;
  *rp++ = v + (*p >> 4); v = *p++ << 4;
  *rp++ = v + (*p >> 3); v = *p++ << 5;
  *rp++ = v + (*p >> 2); v = *p++ << 6;
  *rp++ = v + (*p >> 1); v = *p++ << 7;
  *rp++ = v + *p++;
  return rp;
}
static uint8_t *
unpack_7(uint32_t *p, uint8_t *dp)
{
  uint32_t v;
  *p++ = (*dp >> 1);
  v = ((*dp++ << 6) & 0x7f); *p++ = v + (*dp >> 2);
  v = ((*dp++ << 5) & 0x7f); *p++ = v + (*dp >> 3);
  v = ((*dp++ << 4) & 0x7f); *p++ = v + (*dp >> 4);
  v = ((*dp++ << 3) & 0x7f); *p++ = v + (*dp >> 5);
  v = ((*dp++ << 2) & 0x7f); *p++ = v + (*dp >> 6);
  v = ((*dp++ << 1) & 0x7f); *p++ = v + (*dp >> 7);
  *p++ = (*dp++ & 0x7f);
  return dp;
}
static uint8_t *
pack_8(uint32_t *p, uint8_t *rp)
{
  *rp++ = *p++;
  *rp++ = *p++;
  *rp++ = *p++;
  *rp++ = *p++;
  *rp++ = *p++;
  *rp++ = *p++;
  *rp++ = *p++;
  *rp++ = *p++;
  return rp;
}
static uint8_t *
unpack_8(uint32_t *p, uint8_t *dp)
{
  *p++ = *dp++;
  *p++ = *dp++;
  *p++ = *dp++;
  *p++ = *dp++;
  *p++ = *dp++;
  *p++ = *dp++;
  *p++ = *dp++;
  *p++ = *dp++;
  return dp;
}
static uint8_t *
pack_9(uint32_t *p, uint8_t *rp)
{
  uint8_t v;
  *rp++ = (*p >> 1); v = *p++ << 7;
  *rp++ = v + (*p >> 2); v = *p++ << 6;
  *rp++ = v + (*p >> 3); v = *p++ << 5;
  *rp++ = v + (*p >> 4); v = *p++ << 4;
  *rp++ = v + (*p >> 5); v = *p++ << 3;
  *rp++ = v + (*p >> 6); v = *p++ << 2;
  *rp++ = v + (*p >> 7); v = *p++ << 1;
  *rp++ = v + (*p >> 8); *rp++ = *p++;
  return rp;
}
static uint8_t *
unpack_9(uint32_t *p, uint8_t *dp)
{
  uint32_t v;
  v = *dp++ << 1; *p++ = v + (*dp >> 7);
  v = ((*dp++ << 2) & 0x1ff); *p++ = v + (*dp >> 6);
  v = ((*dp++ << 3) & 0x1ff); *p++ = v + (*dp >> 5);
  v = ((*dp++ << 4) & 0x1ff); *p++ = v + (*dp >> 4);
  v = ((*dp++ << 5) & 0x1ff); *p++ = v + (*dp >> 3);
  v = ((*dp++ << 6) & 0x1ff); *p++ = v + (*dp >> 2);
  v = ((*dp++ << 7) & 0x1ff); *p++ = v + (*dp >> 1);
  v = ((*dp++ << 8) & 0x1ff); *p++ = v + *dp++;
  return dp;
}
static uint8_t *
pack_10(uint32_t *p, uint8_t *rp)
{
  uint8_t v;
  *rp++ = (*p >> 2); v = *p++ << 6;
  *rp++ = v + (*p >> 4); v = *p++ << 4;
  *rp++ = v + (*p >> 6); v = *p++ << 2;
  *rp++ = v + (*p >> 8); *rp++ = *p++;
  *rp++ = (*p >> 2); v = *p++ << 6;
  *rp++ = v + (*p >> 4); v = *p++ << 4;
  *rp++ = v + (*p >> 6); v = *p++ << 2;
  *rp++ = v + (*p >> 8); *rp++ = *p++;
  return rp;
}
static uint8_t *
unpack_10(uint32_t *p, uint8_t *dp)
{
  uint32_t v;
  v = *dp++ << 2; *p++ = v + (*dp >> 6);
  v = ((*dp++ << 4) & 0x3ff); *p++ = v + (*dp >> 4);
  v = ((*dp++ << 6) & 0x3ff); *p++ = v + (*dp >> 2);
  v = ((*dp++ << 8) & 0x3ff); *p++ = v + *dp++;
  v = *dp++ << 2; *p++ = v + (*dp >> 6);
  v = ((*dp++ << 4) & 0x3ff); *p++ = v + (*dp >> 4);
  v = ((*dp++ << 6) & 0x3ff); *p++ = v + (*dp >> 2);
  v = ((*dp++ << 8) & 0x3ff); *p++ = v + *dp++;
  return dp;
}
static uint8_t *
pack_11(uint32_t *p, uint8_t *rp)
{
  uint8_t v;
  *rp++ = (*p >> 3); v = *p++ << 5;
  *rp++ = v + (*p >> 6); v = *p++ << 2;
  *rp++ = v + (*p >> 9); *rp++ = (*p >> 1); v = *p++ << 7;
  *rp++ = v + (*p >> 4); v = *p++ << 4;
  *rp++ = v + (*p >> 7); v = *p++ << 1;
  *rp++ = v + (*p >> 10); *rp++ = (*p >> 2); v = *p++ << 6;
  *rp++ = v + (*p >> 5); v = *p++ << 3;
  *rp++ = v + (*p >> 8); *rp++ = *p++;
  return rp;
}
static uint8_t *
unpack_11(uint32_t *p, uint8_t *dp)
{
  uint32_t v;
  v = *dp++ << 3; *p++ = v + (*dp >> 5);
  v = ((*dp++ << 6) & 0x7ff); *p++ = v + (*dp >> 2);
  v = ((*dp++ << 9) & 0x7ff); v += *dp++ << 1; *p++ = v + (*dp >> 7);
  v = ((*dp++ << 4) & 0x7ff); *p++ = v + (*dp >> 4);
  v = ((*dp++ << 7) & 0x7ff); *p++ = v + (*dp >> 1);
  v = ((*dp++ << 10) & 0x7ff); v += *dp++ << 2; *p++ = v + (*dp >> 6);
  v = ((*dp++ << 5) & 0x7ff); *p++ = v + (*dp >> 3);
  v = ((*dp++ << 8) & 0x7ff); *p++ = v + *dp++;
  return dp;
}
static uint8_t *
pack_12(uint32_t *p, uint8_t *rp)
{
  uint8_t v;
  *rp++ = (*p >> 4); v = *p++ << 4;
  *rp++ = v + (*p >> 8); *rp++ = *p++;
  *rp++ = (*p >> 4); v = *p++ << 4;
  *rp++ = v + (*p >> 8); *rp++ = *p++;
  *rp++ = (*p >> 4); v = *p++ << 4;
  *rp++ = v + (*p >> 8); *rp++ = *p++;
  *rp++ = (*p >> 4); v = *p++ << 4;
  *rp++ = v + (*p >> 8); *rp++ = *p++;
  return rp;
}
static uint8_t *
unpack_12(uint32_t *p, uint8_t *dp)
{
  uint32_t v;
  v = *dp++ << 4; *p++ = v + (*dp >> 4);
  v = ((*dp++ << 8) & 0xfff); *p++ = v + *dp++;
  v = *dp++ << 4; *p++ = v + (*dp >> 4);
  v = ((*dp++ << 8) & 0xfff); *p++ = v + *dp++;
  v = *dp++ << 4; *p++ = v + (*dp >> 4);
  v = ((*dp++ << 8) & 0xfff); *p++ = v + *dp++;
  v = *dp++ << 4; *p++ = v + (*dp >> 4);
  v = ((*dp++ << 8) & 0xfff); *p++ = v + *dp++;
  return dp;
}
static uint8_t *
pack_13(uint32_t *p, uint8_t *rp)
{
  uint8_t v;
  *rp++ = (*p >> 5); v = *p++ << 3;
  *rp++ = v + (*p >> 10); *rp++ = (*p >> 2); v = *p++ << 6;
  *rp++ = v + (*p >> 7); v = *p++ << 1;
  *rp++ = v + (*p >> 12); *rp++ = (*p >> 4); v = *p++ << 4;
  *rp++ = v + (*p >> 9); *rp++ = (*p >> 1); v = *p++ << 7;
  *rp++ = v + (*p >> 6); v = *p++ << 2;
  *rp++ = v + (*p >> 11); *rp++ = (*p >> 3); v = *p++ << 5;
  *rp++ = v + (*p >> 8); *rp++ = *p++;
  return rp;
}
static uint8_t *
unpack_13(uint32_t *p, uint8_t *dp)
{
  uint32_t v;
  v = *dp++ << 5; *p++ = v + (*dp >> 3);
  v = ((*dp++ << 10) & 0x1fff); v += *dp++ << 2; *p++ = v + (*dp >> 6);
  v = ((*dp++ << 7) & 0x1fff); *p++ = v + (*dp >> 1);
  v = ((*dp++ << 12) & 0x1fff); v += *dp++ << 4; *p++ = v + (*dp >> 4);
  v = ((*dp++ << 9) & 0x1fff); v += *dp++ << 1; *p++ = v + (*dp >> 7);
  v = ((*dp++ << 6) & 0x1fff); *p++ = v + (*dp >> 2);
  v = ((*dp++ << 11) & 0x1fff); v += *dp++ << 3; *p++ = v + (*dp >> 5);
  v = ((*dp++ << 8) & 0x1fff); *p++ = v + *dp++;
  return dp;
}
static uint8_t *
pack_14(uint32_t *p, uint8_t *rp)
{
  uint8_t v;
  *rp++ = (*p >> 6); v = *p++ << 2;
  *rp++ = v + (*p >> 12); *rp++ = (*p >> 4); v = *p++ << 4;
  *rp++ = v + (*p >> 10); *rp++ = (*p >> 2); v = *p++ << 6;
  *rp++ = v + (*p >> 8); *rp++ = *p++;
  *rp++ = (*p >> 6); v = *p++ << 2;
  *rp++ = v + (*p >> 12); *rp++ = (*p >> 4); v = *p++ << 4;
  *rp++ = v + (*p >> 10); *rp++ = (*p >> 2); v = *p++ << 6;
  *rp++ = v + (*p >> 8); *rp++ = *p++;
  return rp;
}
static uint8_t *
unpack_14(uint32_t *p, uint8_t *dp)
{
  uint32_t v;
  v = *dp++ << 6; *p++ = v + (*dp >> 2);
  v = ((*dp++ << 12) & 0x3fff); v += *dp++ << 4; *p++ = v + (*dp >> 4);
  v = ((*dp++ << 10) & 0x3fff); v += *dp++ << 2; *p++ = v + (*dp >> 6);
  v = ((*dp++ << 8) & 0x3fff); *p++ = v + *dp++;
  v = *dp++ << 6; *p++ = v + (*dp >> 2);
  v = ((*dp++ << 12) & 0x3fff); v += *dp++ << 4; *p++ = v + (*dp >> 4);
  v = ((*dp++ << 10) & 0x3fff); v += *dp++ << 2; *p++ = v + (*dp >> 6);
  v = ((*dp++ << 8) & 0x3fff); *p++ = v + *dp++;
  return dp;
}
static uint8_t *
pack_15(uint32_t *p, uint8_t *rp)
{
  uint8_t v;
  *rp++ = (*p >> 7); v = *p++ << 1;
  *rp++ = v + (*p >> 14); *rp++ = (*p >> 6); v = *p++ << 2;
  *rp++ = v + (*p >> 13); *rp++ = (*p >> 5); v = *p++ << 3;
  *rp++ = v + (*p >> 12); *rp++ = (*p >> 4); v = *p++ << 4;
  *rp++ = v + (*p >> 11); *rp++ = (*p >> 3); v = *p++ << 5;
  *rp++ = v + (*p >> 10); *rp++ = (*p >> 2); v = *p++ << 6;
  *rp++ = v + (*p >> 9); *rp++ = (*p >> 1); v = *p++ << 7;
  *rp++ = v + (*p >> 8); *rp++ = *p++;
  return rp;
}
static uint8_t *
unpack_15(uint32_t *p, uint8_t *dp)
{
  uint32_t v;
  v = *dp++ << 7; *p++ = v + (*dp >> 1);
  v = ((*dp++ << 14) & 0x7fff); v += *dp++ << 6; *p++ = v + (*dp >> 2);
  v = ((*dp++ << 13) & 0x7fff); v += *dp++ << 5; *p++ = v + (*dp >> 3);
  v = ((*dp++ << 12) & 0x7fff); v += *dp++ << 4; *p++ = v + (*dp >> 4);
  v = ((*dp++ << 11) & 0x7fff); v += *dp++ << 3; *p++ = v + (*dp >> 5);
  v = ((*dp++ << 10) & 0x7fff); v += *dp++ << 2; *p++ = v + (*dp >> 6);
  v = ((*dp++ << 9) & 0x7fff); v += *dp++ << 1; *p++ = v + (*dp >> 7);
  v = ((*dp++ << 8) & 0x7fff); *p++ = v + *dp++;
  return dp;
}
static uint8_t *
pack_16(uint32_t *p, uint8_t *rp)
{
  *rp++ = (*p >> 8); *rp++ = *p++;
  *rp++ = (*p >> 8); *rp++ = *p++;
  *rp++ = (*p >> 8); *rp++ = *p++;
  *rp++ = (*p >> 8); *rp++ = *p++;
  *rp++ = (*p >> 8); *rp++ = *p++;
  *rp++ = (*p >> 8); *rp++ = *p++;
  *rp++ = (*p >> 8); *rp++ = *p++;
  *rp++ = (*p >> 8); *rp++ = *p++;
  return rp;
}
static uint8_t *
unpack_16(uint32_t *p, uint8_t *dp)
{
  uint32_t v;
  v = *dp++ << 8; *p++ = v + *dp++;
  v = *dp++ << 8; *p++ = v + *dp++;
  v = *dp++ << 8; *p++ = v + *dp++;
  v = *dp++ << 8; *p++ = v + *dp++;
  v = *dp++ << 8; *p++ = v + *dp++;
  v = *dp++ << 8; *p++ = v + *dp++;
  v = *dp++ << 8; *p++ = v + *dp++;
  v = *dp++ << 8; *p++ = v + *dp++;
  return dp;
}
static uint8_t *
pack_17(uint32_t *p, uint8_t *rp)
{
  uint8_t v;
  *rp++ = (*p >> 9); *rp++ = (*p >> 1); v = *p++ << 7;
  *rp++ = v + (*p >> 10); *rp++ = (*p >> 2); v = *p++ << 6;
  *rp++ = v + (*p >> 11); *rp++ = (*p >> 3); v = *p++ << 5;
  *rp++ = v + (*p >> 12); *rp++ = (*p >> 4); v = *p++ << 4;
  *rp++ = v + (*p >> 13); *rp++ = (*p >> 5); v = *p++ << 3;
  *rp++ = v + (*p >> 14); *rp++ = (*p >> 6); v = *p++ << 2;
  *rp++ = v + (*p >> 15); *rp++ = (*p >> 7); v = *p++ << 1;
  *rp++ = v + (*p >> 16); *rp++ = (*p >> 8); *rp++ = *p++;
  return rp;
}
static uint8_t *
unpack_17(uint32_t *p, uint8_t *dp)
{
  uint32_t v;
  v = *dp++ << 9; v += *dp++ << 1; *p++ = v + (*dp >> 7);
  v = ((*dp++ << 10) & 0x1ffff); v += *dp++ << 2; *p++ = v + (*dp >> 6);
  v = ((*dp++ << 11) & 0x1ffff); v += *dp++ << 3; *p++ = v + (*dp >> 5);
  v = ((*dp++ << 12) & 0x1ffff); v += *dp++ << 4; *p++ = v + (*dp >> 4);
  v = ((*dp++ << 13) & 0x1ffff); v += *dp++ << 5; *p++ = v + (*dp >> 3);
  v = ((*dp++ << 14) & 0x1ffff); v += *dp++ << 6; *p++ = v + (*dp >> 2);
  v = ((*dp++ << 15) & 0x1ffff); v += *dp++ << 7; *p++ = v + (*dp >> 1);
  v = ((*dp++ << 16) & 0x1ffff); v += *dp++ << 8; *p++ = v + *dp++;
  return dp;
}
static uint8_t *
pack_18(uint32_t *p, uint8_t *rp)
{
  uint8_t v;
  *rp++ = (*p >> 10); *rp++ = (*p >> 2); v = *p++ << 6;
  *rp++ = v + (*p >> 12); *rp++ = (*p >> 4); v = *p++ << 4;
  *rp++ = v + (*p >> 14); *rp++ = (*p >> 6); v = *p++ << 2;
  *rp++ = v + (*p >> 16); *rp++ = (*p >> 8); *rp++ = *p++;
  *rp++ = (*p >> 10); *rp++ = (*p >> 2); v = *p++ << 6;
  *rp++ = v + (*p >> 12); *rp++ = (*p >> 4); v = *p++ << 4;
  *rp++ = v + (*p >> 14); *rp++ = (*p >> 6); v = *p++ << 2;
  *rp++ = v + (*p >> 16); *rp++ = (*p >> 8); *rp++ = *p++;
  return rp;
}
static uint8_t *
unpack_18(uint32_t *p, uint8_t *dp)
{
  uint32_t v;
  v = *dp++ << 10; v += *dp++ << 2; *p++ = v + (*dp >> 6);
  v = ((*dp++ << 12) & 0x3ffff); v += *dp++ << 4; *p++ = v + (*dp >> 4);
  v = ((*dp++ << 14) & 0x3ffff); v += *dp++ << 6; *p++ = v + (*dp >> 2);
  v = ((*dp++ << 16) & 0x3ffff); v += *dp++ << 8; *p++ = v + *dp++;
  v = *dp++ << 10; v += *dp++ << 2; *p++ = v + (*dp >> 6);
  v = ((*dp++ << 12) & 0x3ffff); v += *dp++ << 4; *p++ = v + (*dp >> 4);
  v = ((*dp++ << 14) & 0x3ffff); v += *dp++ << 6; *p++ = v + (*dp >> 2);
  v = ((*dp++ << 16) & 0x3ffff); v += *dp++ << 8; *p++ = v + *dp++;
  return dp;
}
static uint8_t *
pack_19(uint32_t *p, uint8_t *rp)
{
  uint8_t v;
  *rp++ = (*p >> 11); *rp++ = (*p >> 3); v = *p++ << 5;
  *rp++ = v + (*p >> 14); *rp++ = (*p >> 6); v = *p++ << 2;
  *rp++ = v + (*p >> 17); *rp++ = (*p >> 9); *rp++ = (*p >> 1); v = *p++ << 7;
  *rp++ = v + (*p >> 12); *rp++ = (*p >> 4); v = *p++ << 4;
  *rp++ = v + (*p >> 15); *rp++ = (*p >> 7); v = *p++ << 1;
  *rp++ = v + (*p >> 18); *rp++ = (*p >> 10); *rp++ = (*p >> 2); v = *p++ << 6;
  *rp++ = v + (*p >> 13); *rp++ = (*p >> 5); v = *p++ << 3;
  *rp++ = v + (*p >> 16); *rp++ = (*p >> 8); *rp++ = *p++;
  return rp;
}
static uint8_t *
unpack_19(uint32_t *p, uint8_t *dp)
{
  uint32_t v;
  v = *dp++ << 11; v += *dp++ << 3; *p++ = v + (*dp >> 5);
  v = ((*dp++ << 14) & 0x7ffff); v += *dp++ << 6; *p++ = v + (*dp >> 2);
  v = ((*dp++ << 17) & 0x7ffff); v += *dp++ << 9; v += *dp++ << 1; *p++ = v + (*dp >> 7);
  v = ((*dp++ << 12) & 0x7ffff); v += *dp++ << 4; *p++ = v + (*dp >> 4);
  v = ((*dp++ << 15) & 0x7ffff); v += *dp++ << 7; *p++ = v + (*dp >> 1);
  v = ((*dp++ << 18) & 0x7ffff); v += *dp++ << 10; v += *dp++ << 2; *p++ = v + (*dp >> 6);
  v = ((*dp++ << 13) & 0x7ffff); v += *dp++ << 5; *p++ = v + (*dp >> 3);
  v = ((*dp++ << 16) & 0x7ffff); v += *dp++ << 8; *p++ = v + *dp++;
  return dp;
}
static uint8_t *
pack_20(uint32_t *p, uint8_t *rp)
{
  uint8_t v;
  *rp++ = (*p >> 12); *rp++ = (*p >> 4); v = *p++ << 4;
  *rp++ = v + (*p >> 16); *rp++ = (*p >> 8); *rp++ = *p++;
  *rp++ = (*p >> 12); *rp++ = (*p >> 4); v = *p++ << 4;
  *rp++ = v + (*p >> 16); *rp++ = (*p >> 8); *rp++ = *p++;
  *rp++ = (*p >> 12); *rp++ = (*p >> 4); v = *p++ << 4;
  *rp++ = v + (*p >> 16); *rp++ = (*p >> 8); *rp++ = *p++;
  *rp++ = (*p >> 12); *rp++ = (*p >> 4); v = *p++ << 4;
  *rp++ = v + (*p >> 16); *rp++ = (*p >> 8); *rp++ = *p++;
  return rp;
}
static uint8_t *
unpack_20(uint32_t *p, uint8_t *dp)
{
  uint32_t v;
  v = *dp++ << 12; v += *dp++ << 4; *p++ = v + (*dp >> 4);
  v = ((*dp++ << 16) & 0xfffff); v += *dp++ << 8; *p++ = v + *dp++;
  v = *dp++ << 12; v += *dp++ << 4; *p++ = v + (*dp >> 4);
  v = ((*dp++ << 16) & 0xfffff); v += *dp++ << 8; *p++ = v + *dp++;
  v = *dp++ << 12; v += *dp++ << 4; *p++ = v + (*dp >> 4);
  v = ((*dp++ << 16) & 0xfffff); v += *dp++ << 8; *p++ = v + *dp++;
  v = *dp++ << 12; v += *dp++ << 4; *p++ = v + (*dp >> 4);
  v = ((*dp++ << 16) & 0xfffff); v += *dp++ << 8; *p++ = v + *dp++;
  return dp;
}
static uint8_t *
pack_21(uint32_t *p, uint8_t *rp)
{
  uint8_t v;
  *rp++ = (*p >> 13); *rp++ = (*p >> 5); v = *p++ << 3;
  *rp++ = v + (*p >> 18); *rp++ = (*p >> 10); *rp++ = (*p >> 2); v = *p++ << 6;
  *rp++ = v + (*p >> 15); *rp++ = (*p >> 7); v = *p++ << 1;
  *rp++ = v + (*p >> 20); *rp++ = (*p >> 12); *rp++ = (*p >> 4); v = *p++ << 4;
  *rp++ = v + (*p >> 17); *rp++ = (*p >> 9); *rp++ = (*p >> 1); v = *p++ << 7;
  *rp++ = v + (*p >> 14); *rp++ = (*p >> 6); v = *p++ << 2;
  *rp++ = v + (*p >> 19); *rp++ = (*p >> 11); *rp++ = (*p >> 3); v = *p++ << 5;
  *rp++ = v + (*p >> 16); *rp++ = (*p >> 8); *rp++ = *p++;
  return rp;
}
static uint8_t *
unpack_21(uint32_t *p, uint8_t *dp)
{
  uint32_t v;
  v = *dp++ << 13; v += *dp++ << 5; *p++ = v + (*dp >> 3);
  v = ((*dp++ << 18) & 0x1fffff); v += *dp++ << 10; v += *dp++ << 2; *p++ = v + (*dp >> 6);
  v = ((*dp++ << 15) & 0x1fffff); v += *dp++ << 7; *p++ = v + (*dp >> 1);
  v = ((*dp++ << 20) & 0x1fffff); v += *dp++ << 12; v += *dp++ << 4; *p++ = v + (*dp >> 4);
  v = ((*dp++ << 17) & 0x1fffff); v += *dp++ << 9; v += *dp++ << 1; *p++ = v + (*dp >> 7);
  v = ((*dp++ << 14) & 0x1fffff); v += *dp++ << 6; *p++ = v + (*dp >> 2);
  v = ((*dp++ << 19) & 0x1fffff); v += *dp++ << 11; v += *dp++ << 3; *p++ = v + (*dp >> 5);
  v = ((*dp++ << 16) & 0x1fffff); v += *dp++ << 8; *p++ = v + *dp++;
  return dp;
}
static uint8_t *
pack_22(uint32_t *p, uint8_t *rp)
{
  uint8_t v;
  *rp++ = (*p >> 14); *rp++ = (*p >> 6); v = *p++ << 2;
  *rp++ = v + (*p >> 20); *rp++ = (*p >> 12); *rp++ = (*p >> 4); v = *p++ << 4;
  *rp++ = v + (*p >> 18); *rp++ = (*p >> 10); *rp++ = (*p >> 2); v = *p++ << 6;
  *rp++ = v + (*p >> 16); *rp++ = (*p >> 8); *rp++ = *p++;
  *rp++ = (*p >> 14); *rp++ = (*p >> 6); v = *p++ << 2;
  *rp++ = v + (*p >> 20); *rp++ = (*p >> 12); *rp++ = (*p >> 4); v = *p++ << 4;
  *rp++ = v + (*p >> 18); *rp++ = (*p >> 10); *rp++ = (*p >> 2); v = *p++ << 6;
  *rp++ = v + (*p >> 16); *rp++ = (*p >> 8); *rp++ = *p++;
  return rp;
}
static uint8_t *
unpack_22(uint32_t *p, uint8_t *dp)
{
  uint32_t v;
  v = *dp++ << 14; v += *dp++ << 6; *p++ = v + (*dp >> 2);
  v = ((*dp++ << 20) & 0x3fffff); v += *dp++ << 12; v += *dp++ << 4; *p++ = v + (*dp >> 4);
  v = ((*dp++ << 18) & 0x3fffff); v += *dp++ << 10; v += *dp++ << 2; *p++ = v + (*dp >> 6);
  v = ((*dp++ << 16) & 0x3fffff); v += *dp++ << 8; *p++ = v + *dp++;
  v = *dp++ << 14; v += *dp++ << 6; *p++ = v + (*dp >> 2);
  v = ((*dp++ << 20) & 0x3fffff); v += *dp++ << 12; v += *dp++ << 4; *p++ = v + (*dp >> 4);
  v = ((*dp++ << 18) & 0x3fffff); v += *dp++ << 10; v += *dp++ << 2; *p++ = v + (*dp >> 6);
  v = ((*dp++ << 16) & 0x3fffff); v += *dp++ << 8; *p++ = v + *dp++;
  return dp;
}
static uint8_t *
pack_23(uint32_t *p, uint8_t *rp)
{
  uint8_t v;
  *rp++ = (*p >> 15); *rp++ = (*p >> 7); v = *p++ << 1;
  *rp++ = v + (*p >> 22); *rp++ = (*p >> 14); *rp++ = (*p >> 6); v = *p++ << 2;
  *rp++ = v + (*p >> 21); *rp++ = (*p >> 13); *rp++ = (*p >> 5); v = *p++ << 3;
  *rp++ = v + (*p >> 20); *rp++ = (*p >> 12); *rp++ = (*p >> 4); v = *p++ << 4;
  *rp++ = v + (*p >> 19); *rp++ = (*p >> 11); *rp++ = (*p >> 3); v = *p++ << 5;
  *rp++ = v + (*p >> 18); *rp++ = (*p >> 10); *rp++ = (*p >> 2); v = *p++ << 6;
  *rp++ = v + (*p >> 17); *rp++ = (*p >> 9); *rp++ = (*p >> 1); v = *p++ << 7;
  *rp++ = v + (*p >> 16); *rp++ = (*p >> 8); *rp++ = *p++;
  return rp;
}
static uint8_t *
unpack_23(uint32_t *p, uint8_t *dp)
{
  uint32_t v;
  v = *dp++ << 15; v += *dp++ << 7; *p++ = v + (*dp >> 1);
  v = ((*dp++ << 22) & 0x7fffff); v += *dp++ << 14; v += *dp++ << 6; *p++ = v + (*dp >> 2);
  v = ((*dp++ << 21) & 0x7fffff); v += *dp++ << 13; v += *dp++ << 5; *p++ = v + (*dp >> 3);
  v = ((*dp++ << 20) & 0x7fffff); v += *dp++ << 12; v += *dp++ << 4; *p++ = v + (*dp >> 4);
  v = ((*dp++ << 19) & 0x7fffff); v += *dp++ << 11; v += *dp++ << 3; *p++ = v + (*dp >> 5);
  v = ((*dp++ << 18) & 0x7fffff); v += *dp++ << 10; v += *dp++ << 2; *p++ = v + (*dp >> 6);
  v = ((*dp++ << 17) & 0x7fffff); v += *dp++ << 9; v += *dp++ << 1; *p++ = v + (*dp >> 7);
  v = ((*dp++ << 16) & 0x7fffff); v += *dp++ << 8; *p++ = v + *dp++;
  return dp;
}
static uint8_t *
pack_24(uint32_t *p, uint8_t *rp)
{
  *rp++ = (*p >> 16); *rp++ = (*p >> 8); *rp++ = *p++;
  *rp++ = (*p >> 16); *rp++ = (*p >> 8); *rp++ = *p++;
  *rp++ = (*p >> 16); *rp++ = (*p >> 8); *rp++ = *p++;
  *rp++ = (*p >> 16); *rp++ = (*p >> 8); *rp++ = *p++;
  *rp++ = (*p >> 16); *rp++ = (*p >> 8); *rp++ = *p++;
  *rp++ = (*p >> 16); *rp++ = (*p >> 8); *rp++ = *p++;
  *rp++ = (*p >> 16); *rp++ = (*p >> 8); *rp++ = *p++;
  *rp++ = (*p >> 16); *rp++ = (*p >> 8); *rp++ = *p++;
  return rp;
}
static uint8_t *
unpack_24(uint32_t *p, uint8_t *dp)
{
  uint32_t v;
  v = *dp++ << 16; v += *dp++ << 8; *p++ = v + *dp++;
  v = *dp++ << 16; v += *dp++ << 8; *p++ = v + *dp++;
  v = *dp++ << 16; v += *dp++ << 8; *p++ = v + *dp++;
  v = *dp++ << 16; v += *dp++ << 8; *p++ = v + *dp++;
  v = *dp++ << 16; v += *dp++ << 8; *p++ = v + *dp++;
  v = *dp++ << 16; v += *dp++ << 8; *p++ = v + *dp++;
  v = *dp++ << 16; v += *dp++ << 8; *p++ = v + *dp++;
  v = *dp++ << 16; v += *dp++ << 8; *p++ = v + *dp++;
  return dp;
}
static uint8_t *
pack_25(uint32_t *p, uint8_t *rp)
{
  uint8_t v;
  *rp++ = (*p >> 17); *rp++ = (*p >> 9); *rp++ = (*p >> 1); v = *p++ << 7;
  *rp++ = v + (*p >> 18); *rp++ = (*p >> 10); *rp++ = (*p >> 2); v = *p++ << 6;
  *rp++ = v + (*p >> 19); *rp++ = (*p >> 11); *rp++ = (*p >> 3); v = *p++ << 5;
  *rp++ = v + (*p >> 20); *rp++ = (*p >> 12); *rp++ = (*p >> 4); v = *p++ << 4;
  *rp++ = v + (*p >> 21); *rp++ = (*p >> 13); *rp++ = (*p >> 5); v = *p++ << 3;
  *rp++ = v + (*p >> 22); *rp++ = (*p >> 14); *rp++ = (*p >> 6); v = *p++ << 2;
  *rp++ = v + (*p >> 23); *rp++ = (*p >> 15); *rp++ = (*p >> 7); v = *p++ << 1;
  *rp++ = v + (*p >> 24); *rp++ = (*p >> 16); *rp++ = (*p >> 8); *rp++ = *p++;
  return rp;
}
static uint8_t *
unpack_25(uint32_t *p, uint8_t *dp)
{
  uint32_t v;
  v = *dp++ << 17; v += *dp++ << 9; v += *dp++ << 1; *p++ = v + (*dp >> 7);
  v = ((*dp++ << 18) & 0x1ffffff); v += *dp++ << 10; v += *dp++ << 2; *p++ = v + (*dp >> 6);
  v = ((*dp++ << 19) & 0x1ffffff); v += *dp++ << 11; v += *dp++ << 3; *p++ = v + (*dp >> 5);
  v = ((*dp++ << 20) & 0x1ffffff); v += *dp++ << 12; v += *dp++ << 4; *p++ = v + (*dp >> 4);
  v = ((*dp++ << 21) & 0x1ffffff); v += *dp++ << 13; v += *dp++ << 5; *p++ = v + (*dp >> 3);
  v = ((*dp++ << 22) & 0x1ffffff); v += *dp++ << 14; v += *dp++ << 6; *p++ = v + (*dp >> 2);
  v = ((*dp++ << 23) & 0x1ffffff); v += *dp++ << 15; v += *dp++ << 7; *p++ = v + (*dp >> 1);
  v = ((*dp++ << 24) & 0x1ffffff); v += *dp++ << 16; v += *dp++ << 8; *p++ = v + *dp++;
  return dp;
}
static uint8_t *
pack_26(uint32_t *p, uint8_t *rp)
{
  uint8_t v;
  *rp++ = (*p >> 18); *rp++ = (*p >> 10); *rp++ = (*p >> 2); v = *p++ << 6;
  *rp++ = v + (*p >> 20); *rp++ = (*p >> 12); *rp++ = (*p >> 4); v = *p++ << 4;
  *rp++ = v + (*p >> 22); *rp++ = (*p >> 14); *rp++ = (*p >> 6); v = *p++ << 2;
  *rp++ = v + (*p >> 24); *rp++ = (*p >> 16); *rp++ = (*p >> 8); *rp++ = *p++;
  *rp++ = (*p >> 18); *rp++ = (*p >> 10); *rp++ = (*p >> 2); v = *p++ << 6;
  *rp++ = v + (*p >> 20); *rp++ = (*p >> 12); *rp++ = (*p >> 4); v = *p++ << 4;
  *rp++ = v + (*p >> 22); *rp++ = (*p >> 14); *rp++ = (*p >> 6); v = *p++ << 2;
  *rp++ = v + (*p >> 24); *rp++ = (*p >> 16); *rp++ = (*p >> 8); *rp++ = *p++;
  return rp;
}
static uint8_t *
unpack_26(uint32_t *p, uint8_t *dp)
{
  uint32_t v;
  v = *dp++ << 18; v += *dp++ << 10; v += *dp++ << 2; *p++ = v + (*dp >> 6);
  v = ((*dp++ << 20) & 0x3ffffff); v += *dp++ << 12; v += *dp++ << 4; *p++ = v + (*dp >> 4);
  v = ((*dp++ << 22) & 0x3ffffff); v += *dp++ << 14; v += *dp++ << 6; *p++ = v + (*dp >> 2);
  v = ((*dp++ << 24) & 0x3ffffff); v += *dp++ << 16; v += *dp++ << 8; *p++ = v + *dp++;
  v = *dp++ << 18; v += *dp++ << 10; v += *dp++ << 2; *p++ = v + (*dp >> 6);
  v = ((*dp++ << 20) & 0x3ffffff); v += *dp++ << 12; v += *dp++ << 4; *p++ = v + (*dp >> 4);
  v = ((*dp++ << 22) & 0x3ffffff); v += *dp++ << 14; v += *dp++ << 6; *p++ = v + (*dp >> 2);
  v = ((*dp++ << 24) & 0x3ffffff); v += *dp++ << 16; v += *dp++ << 8; *p++ = v + *dp++;
  return dp;
}
static uint8_t *
pack_27(uint32_t *p, uint8_t *rp)
{
  uint8_t v;
  *rp++ = (*p >> 19); *rp++ = (*p >> 11); *rp++ = (*p >> 3); v = *p++ << 5;
  *rp++ = v + (*p >> 22); *rp++ = (*p >> 14); *rp++ = (*p >> 6); v = *p++ << 2;
  *rp++ = v + (*p >> 25); *rp++ = (*p >> 17); *rp++ = (*p >> 9); *rp++ = (*p >> 1); v = *p++ << 7;
  *rp++ = v + (*p >> 20); *rp++ = (*p >> 12); *rp++ = (*p >> 4); v = *p++ << 4;
  *rp++ = v + (*p >> 23); *rp++ = (*p >> 15); *rp++ = (*p >> 7); v = *p++ << 1;
  *rp++ = v + (*p >> 26); *rp++ = (*p >> 18); *rp++ = (*p >> 10); *rp++ = (*p >> 2); v = *p++ << 6;
  *rp++ = v + (*p >> 21); *rp++ = (*p >> 13); *rp++ = (*p >> 5); v = *p++ << 3;
  *rp++ = v + (*p >> 24); *rp++ = (*p >> 16); *rp++ = (*p >> 8); *rp++ = *p++;
  return rp;
}
static uint8_t *
unpack_27(uint32_t *p, uint8_t *dp)
{
  uint32_t v;
  v = *dp++ << 19; v += *dp++ << 11; v += *dp++ << 3; *p++ = v + (*dp >> 5);
  v = ((*dp++ << 22) & 0x7ffffff); v += *dp++ << 14; v += *dp++ << 6; *p++ = v + (*dp >> 2);
  v = ((*dp++ << 25) & 0x7ffffff); v += *dp++ << 17; v += *dp++ << 9; v += *dp++ << 1; *p++ = v + (*dp >> 7);
  v = ((*dp++ << 20) & 0x7ffffff); v += *dp++ << 12; v += *dp++ << 4; *p++ = v + (*dp >> 4);
  v = ((*dp++ << 23) & 0x7ffffff); v += *dp++ << 15; v += *dp++ << 7; *p++ = v + (*dp >> 1);
  v = ((*dp++ << 26) & 0x7ffffff); v += *dp++ << 18; v += *dp++ << 10; v += *dp++ << 2; *p++ = v + (*dp >> 6);
  v = ((*dp++ << 21) & 0x7ffffff); v += *dp++ << 13; v += *dp++ << 5; *p++ = v + (*dp >> 3);
  v = ((*dp++ << 24) & 0x7ffffff); v += *dp++ << 16; v += *dp++ << 8; *p++ = v + *dp++;
  return dp;
}
static uint8_t *
pack_28(uint32_t *p, uint8_t *rp)
{
  uint8_t v;
  *rp++ = (*p >> 20); *rp++ = (*p >> 12); *rp++ = (*p >> 4); v = *p++ << 4;
  *rp++ = v + (*p >> 24); *rp++ = (*p >> 16); *rp++ = (*p >> 8); *rp++ = *p++;
  *rp++ = (*p >> 20); *rp++ = (*p >> 12); *rp++ = (*p >> 4); v = *p++ << 4;
  *rp++ = v + (*p >> 24); *rp++ = (*p >> 16); *rp++ = (*p >> 8); *rp++ = *p++;
  *rp++ = (*p >> 20); *rp++ = (*p >> 12); *rp++ = (*p >> 4); v = *p++ << 4;
  *rp++ = v + (*p >> 24); *rp++ = (*p >> 16); *rp++ = (*p >> 8); *rp++ = *p++;
  *rp++ = (*p >> 20); *rp++ = (*p >> 12); *rp++ = (*p >> 4); v = *p++ << 4;
  *rp++ = v + (*p >> 24); *rp++ = (*p >> 16); *rp++ = (*p >> 8); *rp++ = *p++;
  return rp;
}
static uint8_t *
unpack_28(uint32_t *p, uint8_t *dp)
{
  uint32_t v;
  v = *dp++ << 20; v += *dp++ << 12; v += *dp++ << 4; *p++ = v + (*dp >> 4);
  v = ((*dp++ << 24) & 0xfffffff); v += *dp++ << 16; v += *dp++ << 8; *p++ = v + *dp++;
  v = *dp++ << 20; v += *dp++ << 12; v += *dp++ << 4; *p++ = v + (*dp >> 4);
  v = ((*dp++ << 24) & 0xfffffff); v += *dp++ << 16; v += *dp++ << 8; *p++ = v + *dp++;
  v = *dp++ << 20; v += *dp++ << 12; v += *dp++ << 4; *p++ = v + (*dp >> 4);
  v = ((*dp++ << 24) & 0xfffffff); v += *dp++ << 16; v += *dp++ << 8; *p++ = v + *dp++;
  v = *dp++ << 20; v += *dp++ << 12; v += *dp++ << 4; *p++ = v + (*dp >> 4);
  v = ((*dp++ << 24) & 0xfffffff); v += *dp++ << 16; v += *dp++ << 8; *p++ = v + *dp++;
  return dp;
}
static uint8_t *
pack_29(uint32_t *p, uint8_t *rp)
{
  uint8_t v;
  *rp++ = (*p >> 21); *rp++ = (*p >> 13); *rp++ = (*p >> 5); v = *p++ << 3;
  *rp++ = v + (*p >> 26); *rp++ = (*p >> 18); *rp++ = (*p >> 10); *rp++ = (*p >> 2); v = *p++ << 6;
  *rp++ = v + (*p >> 23); *rp++ = (*p >> 15); *rp++ = (*p >> 7); v = *p++ << 1;
  *rp++ = v + (*p >> 28); *rp++ = (*p >> 20); *rp++ = (*p >> 12); *rp++ = (*p >> 4); v = *p++ << 4;
  *rp++ = v + (*p >> 25); *rp++ = (*p >> 17); *rp++ = (*p >> 9); *rp++ = (*p >> 1); v = *p++ << 7;
  *rp++ = v + (*p >> 22); *rp++ = (*p >> 14); *rp++ = (*p >> 6); v = *p++ << 2;
  *rp++ = v + (*p >> 27); *rp++ = (*p >> 19); *rp++ = (*p >> 11); *rp++ = (*p >> 3); v = *p++ << 5;
  *rp++ = v + (*p >> 24); *rp++ = (*p >> 16); *rp++ = (*p >> 8); *rp++ = *p++;
  return rp;
}
static uint8_t *
unpack_29(uint32_t *p, uint8_t *dp)
{
  uint32_t v;
  v = *dp++ << 21; v += *dp++ << 13; v += *dp++ << 5; *p++ = v + (*dp >> 3);
  v = ((*dp++ << 26) & 0x1fffffff); v += *dp++ << 18; v += *dp++ << 10; v += *dp++ << 2; *p++ = v + (*dp >> 6);
  v = ((*dp++ << 23) & 0x1fffffff); v += *dp++ << 15; v += *dp++ << 7; *p++ = v + (*dp >> 1);
  v = ((*dp++ << 28) & 0x1fffffff); v += *dp++ << 20; v += *dp++ << 12; v += *dp++ << 4; *p++ = v + (*dp >> 4);
  v = ((*dp++ << 25) & 0x1fffffff); v += *dp++ << 17; v += *dp++ << 9; v += *dp++ << 1; *p++ = v + (*dp >> 7);
  v = ((*dp++ << 22) & 0x1fffffff); v += *dp++ << 14; v += *dp++ << 6; *p++ = v + (*dp >> 2);
  v = ((*dp++ << 27) & 0x1fffffff); v += *dp++ << 19; v += *dp++ << 11; v += *dp++ << 3; *p++ = v + (*dp >> 5);
  v = ((*dp++ << 24) & 0x1fffffff); v += *dp++ << 16; v += *dp++ << 8; *p++ = v + *dp++;
  return dp;
}
static uint8_t *
pack_30(uint32_t *p, uint8_t *rp)
{
  uint8_t v;
  *rp++ = (*p >> 22); *rp++ = (*p >> 14); *rp++ = (*p >> 6); v = *p++ << 2;
  *rp++ = v + (*p >> 28); *rp++ = (*p >> 20); *rp++ = (*p >> 12); *rp++ = (*p >> 4); v = *p++ << 4;
  *rp++ = v + (*p >> 26); *rp++ = (*p >> 18); *rp++ = (*p >> 10); *rp++ = (*p >> 2); v = *p++ << 6;
  *rp++ = v + (*p >> 24); *rp++ = (*p >> 16); *rp++ = (*p >> 8); *rp++ = *p++;
  *rp++ = (*p >> 22); *rp++ = (*p >> 14); *rp++ = (*p >> 6); v = *p++ << 2;
  *rp++ = v + (*p >> 28); *rp++ = (*p >> 20); *rp++ = (*p >> 12); *rp++ = (*p >> 4); v = *p++ << 4;
  *rp++ = v + (*p >> 26); *rp++ = (*p >> 18); *rp++ = (*p >> 10); *rp++ = (*p >> 2); v = *p++ << 6;
  *rp++ = v + (*p >> 24); *rp++ = (*p >> 16); *rp++ = (*p >> 8); *rp++ = *p++;
  return rp;
}
static uint8_t *
unpack_30(uint32_t *p, uint8_t *dp)
{
  uint32_t v;
  v = *dp++ << 22; v += *dp++ << 14; v += *dp++ << 6; *p++ = v + (*dp >> 2);
  v = ((*dp++ << 28) & 0x3fffffff); v += *dp++ << 20; v += *dp++ << 12; v += *dp++ << 4; *p++ = v + (*dp >> 4);
  v = ((*dp++ << 26) & 0x3fffffff); v += *dp++ << 18; v += *dp++ << 10; v += *dp++ << 2; *p++ = v + (*dp >> 6);
  v = ((*dp++ << 24) & 0x3fffffff); v += *dp++ << 16; v += *dp++ << 8; *p++ = v + *dp++;
  v = *dp++ << 22; v += *dp++ << 14; v += *dp++ << 6; *p++ = v + (*dp >> 2);
  v = ((*dp++ << 28) & 0x3fffffff); v += *dp++ << 20; v += *dp++ << 12; v += *dp++ << 4; *p++ = v + (*dp >> 4);
  v = ((*dp++ << 26) & 0x3fffffff); v += *dp++ << 18; v += *dp++ << 10; v += *dp++ << 2; *p++ = v + (*dp >> 6);
  v = ((*dp++ << 24) & 0x3fffffff); v += *dp++ << 16; v += *dp++ << 8; *p++ = v + *dp++;
  return dp;
}
static uint8_t *
pack_31(uint32_t *p, uint8_t *rp)
{
  uint8_t v;
  *rp++ = (*p >> 23); *rp++ = (*p >> 15); *rp++ = (*p >> 7); v = *p++ << 1;
  *rp++ = v + (*p >> 30); *rp++ = (*p >> 22); *rp++ = (*p >> 14); *rp++ = (*p >> 6); v = *p++ << 2;
  *rp++ = v + (*p >> 29); *rp++ = (*p >> 21); *rp++ = (*p >> 13); *rp++ = (*p >> 5); v = *p++ << 3;
  *rp++ = v + (*p >> 28); *rp++ = (*p >> 20); *rp++ = (*p >> 12); *rp++ = (*p >> 4); v = *p++ << 4;
  *rp++ = v + (*p >> 27); *rp++ = (*p >> 19); *rp++ = (*p >> 11); *rp++ = (*p >> 3); v = *p++ << 5;
  *rp++ = v + (*p >> 26); *rp++ = (*p >> 18); *rp++ = (*p >> 10); *rp++ = (*p >> 2); v = *p++ << 6;
  *rp++ = v + (*p >> 25); *rp++ = (*p >> 17); *rp++ = (*p >> 9); *rp++ = (*p >> 1); v = *p++ << 7;
  *rp++ = v + (*p >> 24); *rp++ = (*p >> 16); *rp++ = (*p >> 8); *rp++ = *p++;
  return rp;
}
static uint8_t *
unpack_31(uint32_t *p, uint8_t *dp)
{
  uint32_t v;
  v = *dp++ << 23; v += *dp++ << 15; v += *dp++ << 7; *p++ = v + (*dp >> 1);
  v = ((*dp++ << 30) & 0x7fffffff); v += *dp++ << 22; v += *dp++ << 14; v += *dp++ << 6; *p++ = v + (*dp >> 2);
  v = ((*dp++ << 29) & 0x7fffffff); v += *dp++ << 21; v += *dp++ << 13; v += *dp++ << 5; *p++ = v + (*dp >> 3);
  v = ((*dp++ << 28) & 0x7fffffff); v += *dp++ << 20; v += *dp++ << 12; v += *dp++ << 4; *p++ = v + (*dp >> 4);
  v = ((*dp++ << 27) & 0x7fffffff); v += *dp++ << 19; v += *dp++ << 11; v += *dp++ << 3; *p++ = v + (*dp >> 5);
  v = ((*dp++ << 26) & 0x7fffffff); v += *dp++ << 18; v += *dp++ << 10; v += *dp++ << 2; *p++ = v + (*dp >> 6);
  v = ((*dp++ << 25) & 0x7fffffff); v += *dp++ << 17; v += *dp++ << 9; v += *dp++ << 1; *p++ = v + (*dp >> 7);
  v = ((*dp++ << 24) & 0x7fffffff); v += *dp++ << 16; v += *dp++ << 8; *p++ = v + *dp++;
  return dp;
}
static uint8_t *
pack_32(uint32_t *p, uint8_t *rp)
{
  *rp++ = (*p >> 24); *rp++ = (*p >> 16); *rp++ = (*p >> 8); *rp++ = *p++;
  *rp++ = (*p >> 24); *rp++ = (*p >> 16); *rp++ = (*p >> 8); *rp++ = *p++;
  *rp++ = (*p >> 24); *rp++ = (*p >> 16); *rp++ = (*p >> 8); *rp++ = *p++;
  *rp++ = (*p >> 24); *rp++ = (*p >> 16); *rp++ = (*p >> 8); *rp++ = *p++;
  *rp++ = (*p >> 24); *rp++ = (*p >> 16); *rp++ = (*p >> 8); *rp++ = *p++;
  *rp++ = (*p >> 24); *rp++ = (*p >> 16); *rp++ = (*p >> 8); *rp++ = *p++;
  *rp++ = (*p >> 24); *rp++ = (*p >> 16); *rp++ = (*p >> 8); *rp++ = *p++;
  *rp++ = (*p >> 24); *rp++ = (*p >> 16); *rp++ = (*p >> 8); *rp++ = *p++;
  return rp;
}
static uint8_t *
unpack_32(uint32_t *p, uint8_t *dp)
{
  uint32_t v;
  v = *dp++ << 24; v += *dp++ << 16; v += *dp++ << 8; *p++ = v + *dp++;
  v = *dp++ << 24; v += *dp++ << 16; v += *dp++ << 8; *p++ = v + *dp++;
  v = *dp++ << 24; v += *dp++ << 16; v += *dp++ << 8; *p++ = v + *dp++;
  v = *dp++ << 24; v += *dp++ << 16; v += *dp++ << 8; *p++ = v + *dp++;
  v = *dp++ << 24; v += *dp++ << 16; v += *dp++ << 8; *p++ = v + *dp++;
  v = *dp++ << 24; v += *dp++ << 16; v += *dp++ << 8; *p++ = v + *dp++;
  v = *dp++ << 24; v += *dp++ << 16; v += *dp++ << 8; *p++ = v + *dp++;
  v = *dp++ << 24; v += *dp++ << 16; v += *dp++ << 8; *p++ = v + *dp++;
  return dp;
}
/* </generated> */

static uint8_t *
pack_(uint32_t *p, uint32_t i, int w, uint8_t *rp)
{
  while (i >= 8) {
    switch (w) {
    case  0 : break;
    case  1 : rp = pack_1(p, rp); break;
    case  2 : rp = pack_2(p, rp); break;
    case  3 : rp = pack_3(p, rp); break;
    case  4 : rp = pack_4(p, rp); break;
    case  5 : rp = pack_5(p, rp); break;
    case  6 : rp = pack_6(p, rp); break;
    case  7 : rp = pack_7(p, rp); break;
    case  8 : rp = pack_8(p, rp); break;
    case  9 : rp = pack_9(p, rp); break;
    case 10 : rp = pack_10(p, rp); break;
    case 11 : rp = pack_11(p, rp); break;
    case 12 : rp = pack_12(p, rp); break;
    case 13 : rp = pack_13(p, rp); break;
    case 14 : rp = pack_14(p, rp); break;
    case 15 : rp = pack_15(p, rp); break;
    case 16 : rp = pack_16(p, rp); break;
    case 17 : rp = pack_17(p, rp); break;
    case 18 : rp = pack_18(p, rp); break;
    case 19 : rp = pack_19(p, rp); break;
    case 20 : rp = pack_20(p, rp); break;
    case 21 : rp = pack_21(p, rp); break;
    case 22 : rp = pack_22(p, rp); break;
    case 23 : rp = pack_23(p, rp); break;
    case 24 : rp = pack_24(p, rp); break;
    case 25 : rp = pack_25(p, rp); break;
    case 26 : rp = pack_26(p, rp); break;
    case 27 : rp = pack_27(p, rp); break;
    case 28 : rp = pack_28(p, rp); break;
    case 29 : rp = pack_29(p, rp); break;
    case 30 : rp = pack_30(p, rp); break;
    case 31 : rp = pack_31(p, rp); break;
    case 32 : rp = pack_32(p, rp); break;
    }
    p += 8;
    i -= 8;
  }
  {
    int b;
    uint8_t v;
    uint32_t *pe = p + i;
    for (b = 8 - w, v = 0; p < pe;) {
      if (b > 0) {
        v += *p++ << b;
        b -= w;
      } else if (b < 0) {
        *rp++ = v + (*p >> -b);
        b += 8;
        v = 0;
      } else {
        *rp++ = v + *p++;
        b = 8 - w;
        v = 0;
      }
    }
    if (b + w != 8) { *rp++ = v; }
    return rp;
  }
}

static uint8_t *
pack(uint32_t *p, uint32_t i, uint8_t *freq, uint8_t *rp)
{
  int32_t k, w;
  uint8_t ebuf[UNIT_SIZE], *ep = ebuf;
  uint32_t s, *pe = p + i, r, th = i - (i >> 3);
  for (w = 0, s = 0; w <= 32; w++) {
    if ((s += freq[w]) >= th) { break; }
  }
  if (i == s) {
    *rp++ = w;
    return pack_(p, i, w, rp);
  }
  r = 1 << w;
  *rp++ = w + 0x80;
  *rp++ = i - s;
  if (r >= UNIT_SIZE) {
    uint32_t first, *last = &first;
    for (k = 0; p < pe; p++, k++) {
      if (*p >= r) {
        GRN_B_ENC(*p - r, ep);
        *last = k;
        last = p;
      }
    }
    *last = 0;
    *rp++ = (uint8_t) first;
  } else {
    for (k = 0; p < pe; p++, k++) {
      if (*p >= r) {
        *ep++ = k;
        GRN_B_ENC(*p - r, ep);
        *p = 0;
      }
    }
  }
  rp = pack_(p - i, i, w, rp);
  grn_memcpy(rp, ebuf, ep - ebuf);
  return rp + (ep - ebuf);
}

int
grn_p_enc(grn_ctx *ctx, uint32_t *data, uint32_t data_size, uint8_t **res)
{
  uint8_t *rp, freq[33];
  uint32_t j, *dp, *dpe, d, w, buf[UNIT_SIZE];
  *res = rp = GRN_MALLOC(data_size * sizeof(uint32_t) * 2);
  GRN_B_ENC(data_size, rp);
  memset(freq, 0, 33);
  for (j = 0, dp = data, dpe = dp + data_size; dp < dpe; j++, dp++) {
    if (j == UNIT_SIZE) {
      rp = pack(buf, j, freq, rp);
      memset(freq, 0, 33);
      j = 0;
    }
    if ((d = buf[j] = *dp)) {
      GRN_BIT_SCAN_REV(d, w);
      freq[w + 1]++;
    } else {
      freq[0]++;
    }
  }
  if (j) { rp = pack(buf, j, freq, rp); }
  return rp - *res;
}

#define USE_P_ENC (1<<0)
#define CUT_OFF   (1<<1)
#define ODD       (1<<2)

typedef struct {
  uint32_t *data;
  uint32_t data_size;
  uint32_t flags;
} datavec;

static grn_rc
datavec_reset(grn_ctx *ctx, datavec *dv, uint32_t dvlen,
              size_t unitsize, size_t totalsize)
{
  int i;
  if (!dv[0].data || dv[dvlen].data < dv[0].data + totalsize) {
    if (dv[0].data) { GRN_FREE(dv[0].data); }
    if (!(dv[0].data = GRN_MALLOC(totalsize * sizeof(uint32_t)))) {
      return GRN_NO_MEMORY_AVAILABLE;
    }
    dv[dvlen].data = dv[0].data + totalsize;
  }
  for (i = 1; i < dvlen; i++) {
    dv[i].data = dv[i - 1].data + unitsize;
  }
  return GRN_SUCCESS;
}

static grn_rc
datavec_init(grn_ctx *ctx, datavec *dv, uint32_t dvlen,
             size_t unitsize, size_t totalsize)
{
  int i;
  if (!totalsize) {
    memset(dv, 0, sizeof(datavec) * (dvlen + 1));
    return GRN_SUCCESS;
  }
  if (!(dv[0].data = GRN_MALLOC(totalsize * sizeof(uint32_t)))) {
    return GRN_NO_MEMORY_AVAILABLE;
  }
  dv[dvlen].data = dv[0].data + totalsize;
  for (i = 1; i < dvlen; i++) {
    dv[i].data = dv[i - 1].data + unitsize;
  }
  return GRN_SUCCESS;
}

static void
datavec_fin(grn_ctx *ctx, datavec *dv)
{
  if (dv[0].data) { GRN_FREE(dv[0].data); }
}

size_t
grn_p_encv(grn_ctx *ctx, datavec *dv, uint32_t dvlen, uint8_t *res)
{
  uint8_t *rp = res, freq[33];
  uint32_t pgap, usep, l, df, data_size, *dp, *dpe;
  if (!dvlen || !(df = dv[0].data_size)) { return 0; }
  for (usep = 0, data_size = 0, l = 0; l < dvlen; l++) {
    uint32_t dl = dv[l].data_size;
    if (dl < df || ((dl > df) && (l != dvlen - 1))) {
      /* invalid argument */
      return 0;
    }
    usep += (dv[l].flags & USE_P_ENC) << l;
    data_size += dl;
  }
  pgap = data_size - df * dvlen;
  if (!usep) {
    GRN_B_ENC((df << 1) + 1, rp);
    for (l = 0; l < dvlen; l++) {
      for (dp = dv[l].data, dpe = dp + dv[l].data_size; dp < dpe; dp++) {
        GRN_B_ENC(*dp, rp);
      }
    }
  } else {
    uint32_t buf[UNIT_SIZE];
    GRN_B_ENC((usep << 1), rp);
    GRN_B_ENC(df, rp);
    if (dv[dvlen - 1].flags & ODD) {
      GRN_B_ENC(pgap, rp);
    } else {
      GRN_ASSERT(!pgap);
    }
    for (l = 0; l < dvlen; l++) {
      dp = dv[l].data;
      dpe = dp + dv[l].data_size;
      if ((dv[l].flags & USE_P_ENC)) {
        uint32_t j = 0, d;
        memset(freq, 0, 33);
        while (dp < dpe) {
          if (j == UNIT_SIZE) {
            rp = pack(buf, j, freq, rp);
            memset(freq, 0, 33);
            j = 0;
          }
          if ((d = buf[j++] = *dp++)) {
            uint32_t w;
            GRN_BIT_SCAN_REV(d, w);
            freq[w + 1]++;
          } else {
            freq[0]++;
          }
        }
        if (j) { rp = pack(buf, j, freq, rp); }
      } else {
        while (dp < dpe) { GRN_B_ENC(*dp++, rp); }
      }
    }
  }
  return rp - res;
}

#define GRN_B_DEC_CHECK(v,p,pe) do { \
  uint8_t *_p = (uint8_t *)p; \
  uint32_t _v; \
  if (_p >= pe) { return 0; } \
  _v = *_p++; \
  switch (_v >> 4) { \
  case 0x08 : \
    if (_v == 0x8f) { \
      if (_p + sizeof(uint32_t) > pe) { return 0; } \
      grn_memcpy(&_v, _p, sizeof(uint32_t)); \
      _p += sizeof(uint32_t); \
    } \
    break; \
  case 0x09 : \
    if (_p + 3 > pe) { return 0; } \
    _v = (_v - 0x90) * 0x100 + *_p++; \
    _v = _v * 0x100 + *_p++; \
    _v = _v * 0x100 + *_p++ + 0x20408f; \
    break; \
  case 0x0a : \
  case 0x0b : \
    if (_p + 2 > pe) { return 0; } \
    _v = (_v - 0xa0) * 0x100 + *_p++; \
    _v = _v * 0x100 + *_p++ + 0x408f; \
    break; \
  case 0x0c : \
  case 0x0d : \
  case 0x0e : \
  case 0x0f : \
    if (_p + 1 > pe) { return 0; } \
    _v = (_v - 0xc0) * 0x100 + *_p++ + 0x8f; \
    break; \
  } \
  v = _v; \
  p = _p; \
} while (0)

static uint8_t *
unpack(uint8_t *dp, uint8_t *dpe, int i, uint32_t *rp)
{
  uint8_t ne = 0, k = 0, w = *dp++;
  uint32_t m, *p = rp;
  if (w & 0x80) {
    ne = *dp++;
    w -= 0x80;
    m = (1 << w) - 1;
    if (m >= UNIT_MASK) { k = *dp++; }
  } else {
    m = (1 << w) - 1;
  }
  if (w) {
    while (i >= 8) {
      if (dp + w > dpe) { return NULL; }
      switch (w) {
      case 1 : dp = unpack_1(p, dp); break;
      case 2 : dp = unpack_2(p, dp); break;
      case 3 : dp = unpack_3(p, dp); break;
      case 4 : dp = unpack_4(p, dp); break;
      case 5 : dp = unpack_5(p, dp); break;
      case 6 : dp = unpack_6(p, dp); break;
      case 7 : dp = unpack_7(p, dp); break;
      case 8 : dp = unpack_8(p, dp); break;
      case 9 : dp = unpack_9(p, dp); break;
      case 10 : dp = unpack_10(p, dp); break;
      case 11 : dp = unpack_11(p, dp); break;
      case 12 : dp = unpack_12(p, dp); break;
      case 13 : dp = unpack_13(p, dp); break;
      case 14 : dp = unpack_14(p, dp); break;
      case 15 : dp = unpack_15(p, dp); break;
      case 16 : dp = unpack_16(p, dp); break;
      case 17 : dp = unpack_17(p, dp); break;
      case 18 : dp = unpack_18(p, dp); break;
      case 19 : dp = unpack_19(p, dp); break;
      case 20 : dp = unpack_20(p, dp); break;
      case 21 : dp = unpack_21(p, dp); break;
      case 22 : dp = unpack_22(p, dp); break;
      case 23 : dp = unpack_23(p, dp); break;
      case 24 : dp = unpack_24(p, dp); break;
      case 25 : dp = unpack_25(p, dp); break;
      case 26 : dp = unpack_26(p, dp); break;
      case 27 : dp = unpack_27(p, dp); break;
      case 28 : dp = unpack_28(p, dp); break;
      case 29 : dp = unpack_29(p, dp); break;
      case 30 : dp = unpack_30(p, dp); break;
      case 31 : dp = unpack_31(p, dp); break;
      case 32 : dp = unpack_32(p, dp); break;
      }
      i -= 8;
      p += 8;
    }
    {
      int b;
      uint32_t v, *pe;
      for (b = 8 - w, v = 0, pe = p + i; p < pe && dp < dpe;) {
        if (b > 0) {
          *p++ = v + ((*dp >> b) & m);
          b -= w;
          v = 0;
        } else if (b < 0) {
          v += (*dp++ << -b) & m;
          b += 8;
        } else {
          *p++ = v + (*dp++ & m);
          b = 8 - w;
          v = 0;
        }
      }
      if (b + w != 8) { dp++; }
    }
  } else {
    memset(p, 0, sizeof(uint32_t) * i);
  }
  if (ne) {
    if (m >= UNIT_MASK) {
      uint32_t *pp;
      while (ne--) {
        pp = &rp[k];
        k = *pp;
        GRN_B_DEC_CHECK(*pp, dp, dpe);
        *pp += (m + 1);
      }
    } else {
      while (ne--) {
        k = *dp++;
        GRN_B_DEC_CHECK(rp[k], dp, dpe);
        rp[k] += (m + 1);
      }
    }
  }
  return dp;
}

int
grn_p_dec(grn_ctx *ctx, uint8_t *data, uint32_t data_size, uint32_t nreq, uint32_t **res)
{
  uint8_t *dp = data, *dpe = data + data_size;
  uint32_t rest, orig_size, *rp, *rpe;
  GRN_B_DEC(orig_size, dp);
  if (!orig_size) {
    if (!nreq || nreq > data_size) { nreq = data_size; }
    if ((*res = rp = GRN_MALLOC(nreq * 4))) {
      for (rpe = rp + nreq; dp < data + data_size && rp < rpe; rp++) {
        GRN_B_DEC(*rp, dp);
      }
    }
    return rp - *res;
  } else {
    if (!(*res = rp = GRN_MALLOC(orig_size * sizeof(uint32_t)))) {
      return 0;
    }
    if (!nreq || nreq > orig_size) { nreq = orig_size; }
    for (rest = nreq; rest >= UNIT_SIZE; rest -= UNIT_SIZE) {
      if (!(dp = unpack(dp, dpe, UNIT_SIZE, rp))) { return 0; }
      rp += UNIT_SIZE;
    }
    if (rest) { if (!(dp = unpack(dp, dpe, rest, rp))) { return 0; } }
    GRN_ASSERT(data + data_size == dp);
    return nreq;
  }
}

int
grn_p_decv(grn_ctx *ctx, uint8_t *data, uint32_t data_size, datavec *dv, uint32_t dvlen)
{
  size_t size;
  uint32_t df, l, i, *rp, nreq;
  uint8_t *dp = data, *dpe = data + data_size;
  if (!data_size) {
    dv[0].data_size = 0;
    return 0;
  }
  for (nreq = 0; nreq < dvlen; nreq++) {
    if (dv[nreq].flags & CUT_OFF) { break; }
  }
  if (!nreq) { return 0; }
  GRN_B_DEC_CHECK(df, dp, dpe);
  if ((df & 1)) {
    df >>= 1;
    size = nreq == dvlen ? data_size : df * nreq;
    if (dv[dvlen].data < dv[0].data + size) {
      if (dv[0].data) { GRN_FREE(dv[0].data); }
      if (!(rp = GRN_MALLOC(size * sizeof(uint32_t)))) { return 0; }
      dv[dvlen].data = rp + size;
    } else {
      rp = dv[0].data;
    }
    for (l = 0; l < dvlen; l++) {
      if (dv[l].flags & CUT_OFF) { break; }
      dv[l].data = rp;
      if (l < dvlen - 1) {
        for (i = 0; i < df; i++, rp++) { GRN_B_DEC_CHECK(*rp, dp, dpe); }
      } else {
        for (i = 0; dp < dpe; i++, rp++) { GRN_B_DEC_CHECK(*rp, dp, dpe); }
      }
      dv[l].data_size = i;
    }
  } else {
    uint32_t n, rest, usep = df >> 1;
    GRN_B_DEC_CHECK(df, dp, dpe);
    if (dv[dvlen -1].flags & ODD) {
      GRN_B_DEC_CHECK(rest, dp, dpe);
    } else {
      rest = 0;
    }
    size = df * nreq + (nreq == dvlen ? rest : 0);
    if (dv[dvlen].data < dv[0].data + size) {
      if (dv[0].data) { GRN_FREE(dv[0].data); }
      if (!(rp = GRN_MALLOC(size * sizeof(uint32_t)))) { return 0; }
      dv[dvlen].data = rp + size;
    } else {
      rp = dv[0].data;
    }
    for (l = 0; l < dvlen; l++) {
      if (dv[l].flags & CUT_OFF) { break; }
      dv[l].data = rp;
      dv[l].data_size = n = (l < dvlen - 1) ? df : df + rest;
      if (usep & (1 << l)) {
        for (; n >= UNIT_SIZE; n -= UNIT_SIZE) {
          if (!(dp = unpack(dp, dpe, UNIT_SIZE, rp))) { return 0; }
          rp += UNIT_SIZE;
        }
        if (n) {
          if (!(dp = unpack(dp, dpe, n, rp))) { return 0; }
          rp += n;
        }
        dv[l].flags |= USE_P_ENC;
      } else {
        for (; n; n--, rp++) {
          GRN_B_DEC_CHECK(*rp, dp, dpe);
        }
      }
    }
    GRN_ASSERT(dp == dpe);
    if (dp != dpe) {
      GRN_LOG(ctx, GRN_LOG_NOTICE, "data_size=%d, %" GRN_FMT_LLD,
              data_size, (long long int)(dpe - dp));
    }
  }
  return rp - dv[0].data;
}

int
grn_b_enc(grn_ctx *ctx, uint32_t *data, uint32_t data_size, uint8_t **res)
{
  uint8_t *rp;
  uint32_t *dp, i;
  *res = rp = GRN_MALLOC(data_size * sizeof(uint32_t) * 2);
  GRN_B_ENC(data_size, rp);
  for (i = data_size, dp = data; i; i--, dp++) {
    GRN_B_ENC(*dp, rp);
  }
  return rp - *res;
}

int
grn_b_dec(grn_ctx *ctx, uint8_t *data, uint32_t data_size, uint32_t **res)
{
  uint32_t i, *rp, orig_size;
  uint8_t *dp = data;
  GRN_B_DEC(orig_size, dp);
  *res = rp = GRN_MALLOC(orig_size * sizeof(uint32_t));
  for (i = orig_size; i; i--, rp++) {
    GRN_B_DEC(*rp, dp);
  }
  return orig_size;
}

/* buffer */

typedef struct {
  uint32_t tid;
  uint32_t size_in_chunk;
  uint32_t pos_in_chunk;
  uint16_t size_in_buffer;
  uint16_t pos_in_buffer;
} buffer_term;

typedef struct {
  uint16_t step;
  uint16_t jump;
} buffer_rec;

typedef struct {
  uint32_t chunk;
  uint32_t chunk_size;
  uint32_t buffer_free;
  uint16_t nterms;
  uint16_t nterms_void;
} buffer_header;

struct grn_ii_buffer {
  buffer_header header;
  buffer_term terms[(S_SEGMENT - sizeof(buffer_header))/sizeof(buffer_term)];
};

typedef struct grn_ii_buffer buffer;

inline static uint32_t
buffer_open(grn_ctx *ctx, grn_ii *ii, uint32_t pos, buffer_term **bt, buffer **b)
{
  byte *p = NULL;
  uint16_t lseg = (uint16_t) (LSEG(pos));
  uint32_t pseg = ii->header->binfo[lseg];
  if (pseg != NOT_ASSIGNED) {
    GRN_IO_SEG_REF(ii->seg, pseg, p);
    if (!p) { return NOT_ASSIGNED; }
    if (b) { *b = (buffer *)p; }
    if (bt) { *bt = (buffer_term *)(p + LPOS(pos)); }
  }
  return pseg;
}

inline static grn_rc
buffer_close(grn_ctx *ctx, grn_ii *ii, uint32_t pseg)
{
  if (pseg >= MAX_PSEG) {
    GRN_LOG(ctx, GRN_LOG_NOTICE, "invalid pseg buffer_close(%d)", pseg);
    return GRN_INVALID_ARGUMENT;
  }
  GRN_IO_SEG_UNREF(ii->seg, pseg);
  return GRN_SUCCESS;
}

inline static uint32_t
buffer_open_if_capable(grn_ctx *ctx, grn_ii *ii, int32_t seg, int size, buffer **b)
{
  uint32_t pseg, pos = SEG2POS(seg, 0);
  if ((pseg = buffer_open(ctx, ii, pos, NULL, b)) != NOT_ASSIGNED) {
    uint16_t nterms = (*b)->header.nterms - (*b)->header.nterms_void;
    if (!((nterms < 4096 ||
           (ii->header->total_chunk_size >> ((nterms >> 8) - 6))
           > (*b)->header.chunk_size) &&
          ((*b)->header.buffer_free >= size + sizeof(buffer_term)))) {
      buffer_close(ctx, ii, pseg);
      return NOT_ASSIGNED;
    }
  }
  return pseg;
}

typedef struct {
  uint32_t rid;
  uint32_t sid;
} docid;

#define BUFFER_REC_DEL(r)     ((r)->jump = 1)
#define BUFFER_REC_DELETED(r) ((r)->jump == 1)

#define BUFFER_REC_AT(b,pos)  ((buffer_rec *)(b) + (pos))
#define BUFFER_REC_POS(b,rec) ((uint16_t)((rec) - (buffer_rec *)(b)))

inline static void
buffer_term_dump(grn_ctx *ctx, grn_ii *ii, buffer *b, buffer_term *bt)
{
  int pos, rid, sid;
  uint8_t *p;
  buffer_rec *r;
  GRN_LOG(ctx, GRN_LOG_DEBUG,
          "b=(%x %u %u %u)", b->header.chunk, b->header.chunk_size, b->header.buffer_free, b->header.nterms);
  GRN_LOG(ctx, GRN_LOG_DEBUG,
          "bt=(%u %u %u %u %u)", bt->tid, bt->size_in_chunk, bt->pos_in_chunk, bt->size_in_buffer, bt->pos_in_buffer);
  for (pos = bt->pos_in_buffer; pos; pos = r->step) {
    r = BUFFER_REC_AT(b, pos);
    p = NEXT_ADDR(r);
    GRN_B_DEC(rid, p);
    if ((ii->header->flags & GRN_OBJ_WITH_SECTION)) {
      GRN_B_DEC(sid, p);
    } else {
      sid = 1;
    }
    GRN_LOG(ctx, GRN_LOG_DEBUG, "%d=(%d:%d),(%d:%d)", pos, r->jump, r->step, rid, sid);
  }
}

inline static grn_rc
check_jump(grn_ctx *ctx, grn_ii *ii, buffer *b, buffer_rec *r, int j)
{
  uint16_t i = BUFFER_REC_POS(b, r);
  uint8_t *p;
  buffer_rec *r2;
  docid id, id2;
  if (!j) { return GRN_SUCCESS; }
  p = NEXT_ADDR(r);
  GRN_B_DEC(id.rid, p);
  if ((ii->header->flags & GRN_OBJ_WITH_SECTION)) {
    GRN_B_DEC(id.sid, p);
  } else {
    id.sid = 1;
  }
  if (j == 1) {
    GRN_LOG(ctx, GRN_LOG_DEBUG, "deleting! %d(%d:%d)", i, id.rid, id.sid);
    return GRN_SUCCESS;
  }
  r2 = BUFFER_REC_AT(b, j);
  p = NEXT_ADDR(r2);
  GRN_B_DEC(id2.rid, p);
  if ((ii->header->flags & GRN_OBJ_WITH_SECTION)) {
    GRN_B_DEC(id2.sid, p);
  } else {
    id2.sid = 1;
  }
  if (r2->step == i) {
    GRN_LOG(ctx, GRN_LOG_EMERG, "cycle! %d(%d:%d)<->%d(%d:%d)", i, id.rid, id.sid, j, id2.rid, id2.sid);
    return GRN_FILE_CORRUPT;
  }
  if (id2.rid < id.rid || (id2.rid == id.rid && id2.sid <= id.sid)) {
    GRN_LOG(ctx, GRN_LOG_CRIT, "invalid jump! %d(%d:%d)(%d:%d)->%d(%d:%d)(%d:%d)", i, r->jump, r->step, id.rid, id.sid, j, r2->jump, r2->step, id2.rid, id2.sid);
    return GRN_FILE_CORRUPT;
  }
  return GRN_SUCCESS;
}

inline static grn_rc
set_jump_r(grn_ctx *ctx, grn_ii *ii, buffer *b, buffer_rec *from, int to)
{
  int i, j, max_jump = 100;
  buffer_rec *r, *r2;
  for (r = from, j = to; j > 1 && max_jump--; r = BUFFER_REC_AT(b, r->step)) {
    r2 = BUFFER_REC_AT(b, j);
    if (r == r2) { break; }
    if (BUFFER_REC_DELETED(r2)) { break; }
    if (j == (i = r->jump)) { break; }
    if (j == r->step) { break; }
    if (check_jump(ctx, ii, b, r, j)) {
      ERR(GRN_FILE_CORRUPT, "check_jump failed");
      return ctx->rc;
    }
    r->jump = j;
    j = i;
    if (!r->step) { return GRN_FILE_CORRUPT; }
  }
  return GRN_SUCCESS;
}

#define GET_NUM_BITS(x,n) do {\
  n = x;\
  n = (n & 0x55555555) + ((n >> 1) & 0x55555555);\
  n = (n & 0x33333333) + ((n >> 2) & 0x33333333);\
  n = (n & 0x0F0F0F0F) + ((n >> 4) & 0x0F0F0F0F);\
  n = (n & 0x00FF00FF) + ((n >> 8) & 0x00FF00FF);\
  n = (n & 0x0000FFFF) + ((n >>16) & 0x0000FFFF);\
} while (0)

inline static grn_rc
buffer_put(grn_ctx *ctx, grn_ii *ii, buffer *b, buffer_term *bt,
           buffer_rec *rnew, uint8_t *bs, grn_ii_updspec *u, int size)
{
  uint8_t *p;
  grn_rc rc = GRN_SUCCESS;
  docid id_curr = {0, 0}, id_start = {0, 0}, id_post = {0, 0};
  buffer_rec *r_curr, *r_start = NULL;
  uint16_t last = 0, *lastp = &bt->pos_in_buffer, pos = BUFFER_REC_POS(b, rnew);
  int vdelta = 0, delta, delta0 = 0, vhops = 0, nhops = 0, reset = 1;
  grn_memcpy(NEXT_ADDR(rnew), bs, size - sizeof(buffer_rec));
  for (;;) {
    if (!*lastp) {
      rnew->step = 0;
      rnew->jump = 0;
      // smb_wmb();
      *lastp = pos;
      if (bt->size_in_buffer++ > 1) {
        buffer_rec *rhead = BUFFER_REC_AT(b, bt->pos_in_buffer);
        rhead->jump = pos;
        if (!(bt->size_in_buffer & 1)) {
          int n;
          buffer_rec *r = BUFFER_REC_AT(b, rhead->step), *r2;
          GET_NUM_BITS(bt->size_in_buffer, n);
          while (n-- && (r->jump > 1)) {
            r2 = BUFFER_REC_AT(b, r->jump);
            if (BUFFER_REC_DELETED(r2)) { break; }
            r = r2;
          }
          if (r != rnew) { set_jump_r(ctx, ii, b, r, last); }
        }
      }
      break;
    }
    r_curr = BUFFER_REC_AT(b, *lastp);
    p = NEXT_ADDR(r_curr);
    GRN_B_DEC(id_curr.rid, p);
    if ((ii->header->flags & GRN_OBJ_WITH_SECTION)) {
      GRN_B_DEC(id_curr.sid, p);
    } else {
      id_curr.sid = 1;
    }
    if (id_curr.rid < id_post.rid ||
        (id_curr.rid == id_post.rid && id_curr.sid < id_post.sid)) {
      rc = GRN_FILE_CORRUPT;
      ERRSET(ctx, GRN_CRIT, rc, "loop found!!! (%d:%d)->(%d:%d)",
              id_post.rid, id_post.sid, id_curr.rid, id_curr.sid);
      buffer_term_dump(ctx, ii, b, bt);
      bt->pos_in_buffer = 0;
      bt->size_in_buffer = 0;
      lastp = &bt->pos_in_buffer;
      continue;
    }
    id_post.rid = id_curr.rid;
    id_post.sid = id_curr.sid;
    if (u->rid < id_curr.rid || (u->rid == id_curr.rid && u->sid <= id_curr.sid)) {
      uint16_t step = *lastp, jump = r_curr->jump;
      if (u->rid == id_curr.rid) {
        if (u->sid == 0) {
          while (id_curr.rid == u->rid) {
            BUFFER_REC_DEL(r_curr);
            if (!(step = r_curr->step)) { break; }
            r_curr = BUFFER_REC_AT(b, step);
            p = NEXT_ADDR(r_curr);
            GRN_B_DEC(id_curr.rid, p);
          if ((ii->header->flags & GRN_OBJ_WITH_SECTION)) {
            GRN_B_DEC(id_curr.sid, p);
          } else {
            id_curr.sid = 1;
          }
          }
        } else if (u->sid == id_curr.sid) {
          BUFFER_REC_DEL(r_curr);
          step = r_curr->step;
        }
      }
      rnew->step = step;
      rnew->jump = check_jump(ctx, ii, b, rnew, jump) ? 0 : jump;
      // smb_wmb();
      *lastp = pos;
      break;
    }

    if (reset) {
      r_start = r_curr;
      id_start.rid = id_curr.rid;
      id_start.sid = id_curr.sid;
      if (!(delta0 = u->rid - id_start.rid)) { delta0 = u->sid - id_start.sid; }
      nhops = 0;
      vhops = 1;
      vdelta = delta0 >> 1;
    } else {
      if (!(delta = id_curr.rid - id_start.rid)) { delta = id_curr.sid - id_start.sid; }
      if (vdelta < delta) {
        vdelta += (delta0 >> ++vhops);
        r_start = r_curr;
      }
      if (nhops > vhops) {
        set_jump_r(ctx, ii, b, r_start, *lastp);
      } else {
        nhops++;
      }
    }

    last = *lastp;
    lastp = &r_curr->step;
    reset = 0;
    {
      uint16_t posj = r_curr->jump;
      if (posj > 1) {
        buffer_rec *rj = BUFFER_REC_AT(b, posj);
        if (!BUFFER_REC_DELETED(rj)) {
          docid idj;
          p = NEXT_ADDR(rj);
          GRN_B_DEC(idj.rid, p);
          if ((ii->header->flags & GRN_OBJ_WITH_SECTION)) {
            GRN_B_DEC(idj.sid, p);
          } else {
            idj.sid = 1;
          }
          if (idj.rid < u->rid || (idj.rid == u->rid && idj.sid < u->sid)) {
            last = posj;
            lastp = &rj->step;
          } else {
            reset = 1;
          }
        }
      }
    }
  }
  return rc;
}

/* array */

inline static uint32_t *
array_at(grn_ctx *ctx, grn_ii *ii, uint32_t id)
{
  byte *p = NULL;
  uint32_t seg, pseg;
  if (id > GRN_ID_MAX) { return NULL; }
  seg = id >> W_ARRAY;
  if ((pseg = ii->header->ainfo[seg]) == NOT_ASSIGNED) { return NULL; }
  GRN_IO_SEG_REF(ii->seg, pseg, p);
  if (!p) { return NULL; }
  return (uint32_t *)(p + (id & ARRAY_MASK_IN_A_SEGMENT) * S_ARRAY_ELEMENT);
}

inline static uint32_t *
array_get(grn_ctx *ctx, grn_ii *ii, uint32_t id)
{
  byte *p = NULL;
  uint16_t seg;
  uint32_t pseg;
  if (id > GRN_ID_MAX) { return NULL; }
  seg = id >> W_ARRAY;
  if ((pseg = ii->header->ainfo[seg]) == NOT_ASSIGNED) {
    if (segment_get_clear(ctx, ii, &pseg)) { return NULL; }
    ii->header->ainfo[seg] = pseg;
    if (seg >= ii->header->amax) { ii->header->amax = seg + 1; }
  }
  GRN_IO_SEG_REF(ii->seg, pseg, p);
  if (!p) { return NULL; }
  return (uint32_t *)(p + (id & ARRAY_MASK_IN_A_SEGMENT) * S_ARRAY_ELEMENT);
}

inline static void
array_unref(grn_ii *ii, uint32_t id)
{
  GRN_IO_SEG_UNREF(ii->seg, ii->header->ainfo[id >> W_ARRAY]);
}

/* updspec */

grn_ii_updspec *
grn_ii_updspec_open(grn_ctx *ctx, uint32_t rid, uint32_t sid)
{
  grn_ii_updspec *u;
  if (!(u = GRN_MALLOC(sizeof(grn_ii_updspec)))) { return NULL; }
  u->rid = rid;
  u->sid = sid;
  u->weight = 0;
  u->tf = 0;
  u->atf = 0;
  u->pos = NULL;
  u->tail = NULL;
  //  u->vnodes = NULL;
  return u;
}

#define GRN_II_MAX_TF 0x1ffff

grn_rc
grn_ii_updspec_add(grn_ctx *ctx, grn_ii_updspec *u, int pos, int32_t weight)
{
  struct _grn_ii_pos *p;
  u->atf++;
  if (u->tf >= GRN_II_MAX_TF) { return GRN_SUCCESS; }
  if (!(p = GRN_MALLOC(sizeof(struct _grn_ii_pos)))) {
    return GRN_NO_MEMORY_AVAILABLE;
  }
  u->weight += weight;
  p->pos = pos;
  p->next = NULL;
  if (u->tail) {
    u->tail->next = p;
  } else {
    u->pos = p;
  }
  u->tail = p;
  u->tf++;
  return GRN_SUCCESS;
}

int
grn_ii_updspec_cmp(grn_ii_updspec *a, grn_ii_updspec *b)
{
  struct _grn_ii_pos *pa, *pb;
  if (a->rid != b->rid) { return a->rid - b->rid; }
  if (a->sid != b->sid) { return a->sid - b->sid; }
  if (a->weight != b->weight) { return a->weight - b->weight; }
  if (a->tf != b->tf) { return a->tf - b->tf; }
  for (pa = a->pos, pb = b->pos; pa && pb; pa = pa->next, pb = pb->next) {
    if (pa->pos != pb->pos) { return pa->pos - pb->pos; }
  }
  if (pa) { return 1; }
  if (pb) { return -1; }
  return 0;
}

grn_rc
grn_ii_updspec_close(grn_ctx *ctx, grn_ii_updspec *u)
{
  struct _grn_ii_pos *p = u->pos, *q;
  while (p) {
    q = p->next;
    GRN_FREE(p);
    p = q;
  }
  GRN_FREE(u);
  return GRN_SUCCESS;
}

inline static uint8_t *
encode_rec(grn_ctx *ctx, grn_ii *ii, grn_ii_updspec *u, unsigned int *size, int deletep)
{
  uint8_t *br, *p;
  struct _grn_ii_pos *pp;
  uint32_t lpos, tf, weight;
  if (deletep) {
    tf = 0;
    weight = 0;
  } else {
    tf = u->tf;
    weight = u->weight;
  }
  if (!(br = GRN_MALLOC((tf + 4) * 5))) {
    return NULL;
  }
  p = br;
  GRN_B_ENC(u->rid, p);
  if ((ii->header->flags & GRN_OBJ_WITH_SECTION)) {
    GRN_B_ENC(u->sid, p);
  } else {
    u->sid = 1;
  }
  GRN_B_ENC(tf, p);
  if ((ii->header->flags & GRN_OBJ_WITH_WEIGHT)) { GRN_B_ENC(weight, p); }
  if ((ii->header->flags & GRN_OBJ_WITH_POSITION)) {
    for (lpos = 0, pp = u->pos; pp && tf--; lpos = pp->pos, pp = pp->next) {
      GRN_B_ENC(pp->pos - lpos, p);
    }
  }
  while (((intptr_t)p & 0x03)) { *p++ = 0; }
  *size = (unsigned int) ((p - br) + sizeof(buffer_rec));
  return br;
}

typedef struct {
  grn_ii *ii;
  grn_hash *h;
} lexicon_deletable_arg;

#ifdef CASCADE_DELETE_LEXICON
static int
lexicon_deletable(grn_ctx *ctx, grn_obj *lexicon, grn_id tid, void *arg)
{
  uint32_t *a;
  grn_hash *h = ((lexicon_deletable_arg *)arg)->h;
  grn_ii *ii = ((lexicon_deletable_arg *)arg)->ii;
  if (!h) { return 0; }
  if ((a = array_at(ctx, ii, tid))) {
    if (a[0]) {
      array_unref(ii, tid);
      return 0;
    }
    array_unref(ii, tid);
  }
  {
    grn_ii_updspec **u;
    if (!grn_hash_get(ctx, h, &tid, sizeof(grn_id), (void **) &u)) {
      return (ERRP(ctx, GRN_ERROR)) ? 0 : 1;
    }
    if (!(*u)->tf || !(*u)->sid) { return 1; }
    return 0;
  }
}
#endif /* CASCADE_DELETE_LEXICON */

inline static void
lexicon_delete(grn_ctx *ctx, grn_ii *ii, uint32_t tid, grn_hash *h)
{
#ifdef CASCADE_DELETE_LEXICON
  lexicon_deletable_arg arg = {ii, h};
  grn_table_delete_optarg optarg = {0, lexicon_deletable, &arg};
  _grn_table_delete_by_id(ctx, ii->lexicon, tid, &optarg);
#endif /* CASCADE_DELETE_LEXICON */
}

typedef struct {
  grn_id rid;
  uint32_t sid;
  uint32_t tf;
  uint32_t weight;
  uint32_t flags;
} docinfo;

#define GETNEXTC() do {\
  if (sdf) {\
    uint32_t dgap = *srp++;\
    cid.rid += dgap;\
    if (dgap) { cid.sid = 0; }\
    snp += cid.tf;\
    cid.tf = 1 + *stp++;\
    if ((ii->header->flags & GRN_OBJ_WITH_WEIGHT)) { cid.weight = *sop++; }\
    if ((ii->header->flags & GRN_OBJ_WITH_SECTION)) {\
      cid.sid += 1 + *ssp++;\
    } else {\
      cid.sid = 1;\
    }\
    sdf--;\
  } else {\
    cid.rid = 0;\
  }\
} while (0)

#define PUTNEXT_(id) do {\
  uint32_t dgap = id.rid - lid.rid;\
  uint32_t sgap = (dgap ? id.sid : id.sid - lid.sid) - 1;\
  *ridp++ = dgap;\
  if ((ii->header->flags & GRN_OBJ_WITH_SECTION)) {\
    *sidp++ = sgap;\
  }\
  *tfp++ = id.tf - 1;\
  if ((ii->header->flags & GRN_OBJ_WITH_WEIGHT)) { *weightp++ = id.weight; }\
  lid.rid = id.rid;\
  lid.sid = id.sid;\
} while (0)

#define PUTNEXTC() do {\
  if (cid.rid) {\
    if (cid.tf) {\
      if (lid.rid > cid.rid || (lid.rid == cid.rid && lid.sid >= cid.sid)) {\
        GRN_LOG(ctx, GRN_LOG_CRIT, "brokenc!! (%d:%d) -> (%d:%d)", lid.rid, lid.sid, bid.rid, bid.sid);\
        rc = GRN_FILE_CORRUPT;\
        break;\
      }\
      PUTNEXT_(cid);\
      if ((ii->header->flags & GRN_OBJ_WITH_POSITION)) {\
        uint32_t i;\
        for (i = 0; i < cid.tf; i++) {\
          *posp++ = snp[i];\
          spos += snp[i];\
        }\
      }\
    } else {\
      GRN_LOG(ctx, GRN_LOG_CRIT, "invalid chunk(%d,%d)", bt->tid, cid.rid);\
      rc = GRN_FILE_CORRUPT;\
      break;\
    }\
  }\
  GETNEXTC();\
} while (0)

#define GETNEXTB() do {\
  if (nextb) {\
    uint32_t lrid = bid.rid, lsid = bid.sid;\
    buffer_rec *br = BUFFER_REC_AT(sb, nextb);\
    sbp = NEXT_ADDR(br);\
    GRN_B_DEC(bid.rid, sbp);\
    if ((ii->header->flags & GRN_OBJ_WITH_SECTION)) {\
      GRN_B_DEC(bid.sid, sbp);\
    } else {\
      bid.sid = 1;\
    }\
    if (lrid > bid.rid || (lrid == bid.rid && lsid >= bid.sid)) {\
      GRN_LOG(ctx, GRN_LOG_CRIT, "brokeng!! (%d:%d) -> (%d:%d)", lrid, lsid, bid.rid, bid.sid);\
      rc = GRN_FILE_CORRUPT;\
      break;\
    }\
    nextb = br->step;\
  } else {\
    bid.rid = 0;\
  }\
} while (0)

#define PUTNEXTB() do {\
  if (bid.rid && bid.sid) {\
    GRN_B_DEC(bid.tf, sbp);\
    if (bid.tf > 0) {\
      if (lid.rid > bid.rid || (lid.rid == bid.rid && lid.sid >= bid.sid)) {\
        GRN_LOG(ctx, GRN_LOG_CRIT, "brokenb!! (%d:%d) -> (%d:%d)", lid.rid, lid.sid, bid.rid, bid.sid);\
        rc = GRN_FILE_CORRUPT;\
        break;\
      }\
      if ((ii->header->flags & GRN_OBJ_WITH_WEIGHT)) { GRN_B_DEC(bid.weight, sbp); }\
      PUTNEXT_(bid);\
      if ((ii->header->flags & GRN_OBJ_WITH_POSITION)) {\
        while (bid.tf--) { GRN_B_DEC(*posp, sbp); spos += *posp++; }\
      }\
    }\
  }\
  GETNEXTB();\
} while (0)

#define MERGE_BC(cond) do {\
  if (bid.rid) {\
    if (cid.rid) {\
      if (cid.rid < bid.rid) {\
        PUTNEXTC();\
      } else {\
        if (bid.rid < cid.rid) {\
          PUTNEXTB();\
        } else {\
          if (bid.sid) {\
            if (cid.sid < bid.sid) {\
              PUTNEXTC();\
            } else {\
              if (bid.sid == cid.sid) { GETNEXTC(); }\
              PUTNEXTB();\
            }\
          } else {\
            GETNEXTC();\
          }\
        }\
      }\
    } else {\
      PUTNEXTB();\
    }\
  } else {\
    if (cid.rid) {\
      PUTNEXTC();\
    } else {\
      break;\
    }\
  }\
} while (cond)

typedef struct {
  uint32_t segno;
  uint32_t size;
  uint32_t dgap;
} chunk_info;

static grn_rc
chunk_flush(grn_ctx *ctx, grn_ii *ii, chunk_info *cinfo, uint8_t *enc, uint32_t encsize)
{
  grn_rc rc = GRN_SUCCESS;
  uint8_t *dc;
  uint32_t dcn;
  grn_io_win dw;
  if (encsize) {
    if (!(rc = chunk_new(ctx, ii, &dcn, encsize))) {
      if ((dc = WIN_MAP(ii->chunk, ctx, &dw, dcn, 0, encsize, grn_io_wronly))) {
        grn_memcpy(dc, enc, encsize);
        grn_io_win_unmap(&dw);
        cinfo->segno = dcn;
        cinfo->size = encsize;
      } else {
        chunk_free(ctx, ii, dcn, 0, encsize);
        rc = GRN_NO_MEMORY_AVAILABLE;
      }
    }
  } else {
    cinfo->segno = 0;
    cinfo->size = 0;
  }
  return rc;
}

static grn_rc
chunk_merge(grn_ctx *ctx, grn_ii *ii, buffer *sb, buffer_term *bt,
            chunk_info *cinfo, grn_id rid, datavec *dv,
            uint16_t *nextbp, uint8_t **sbpp, docinfo *bidp, int32_t *balance)
{
  grn_rc rc;
  grn_io_win sw;
  uint64_t spos = 0;
  uint32_t segno = cinfo->segno, size = cinfo->size, sdf = 0, ndf = 0;
  uint32_t *ridp = NULL, *sidp = NULL, *tfp, *weightp = NULL, *posp = NULL;
  docinfo cid = {0, 0, 0, 0, 0}, lid = {0, 0, 0, 0, 0}, bid = *bidp;
  uint8_t *scp = WIN_MAP(ii->chunk, ctx, &sw, segno, 0, size, grn_io_rdonly);
  if (scp) {
    uint16_t nextb = *nextbp;
    uint32_t snn = 0, *srp, *ssp = NULL, *stp, *sop = NULL, *snp;
    uint8_t *sbp = *sbpp;
    datavec rdv[MAX_N_ELEMENTS + 1];
    size_t bufsize = S_SEGMENT * ii->n_elements;
    datavec_init(ctx, rdv, ii->n_elements, 0, 0);
    if ((ii->header->flags & GRN_OBJ_WITH_POSITION)) {
      rdv[ii->n_elements - 1].flags = ODD;
    }
    bufsize += grn_p_decv(ctx, scp, cinfo->size, rdv, ii->n_elements);
    // (df in chunk list) = a[1] - sdf;
    {
      int j = 0;
      sdf = rdv[j].data_size;
      srp = rdv[j++].data;
      if ((ii->header->flags & GRN_OBJ_WITH_SECTION)) { ssp = rdv[j++].data; }
      stp = rdv[j++].data;
      if ((ii->header->flags & GRN_OBJ_WITH_WEIGHT)) { sop = rdv[j++].data; }
      snn = rdv[j].data_size;
      snp = rdv[j].data;
    }
    if (!(rc = datavec_reset(ctx, dv, ii->n_elements, sdf + S_SEGMENT, bufsize))) {
      {
        int j = 0;
        ridp = dv[j++].data;
        if ((ii->header->flags & GRN_OBJ_WITH_SECTION)) { sidp = dv[j++].data; }
        tfp = dv[j++].data;
        if ((ii->header->flags & GRN_OBJ_WITH_WEIGHT)) { weightp = dv[j++].data; }
        posp = dv[j].data;
      }
      GETNEXTC();
      MERGE_BC(bid.rid <= rid || cid.rid);
      *sbpp = sbp;
      *nextbp = nextb;
      *bidp = bid;
      GRN_ASSERT(posp < dv[ii->n_elements].data);
      ndf = ridp - dv[0].data;
    }
    datavec_fin(ctx, rdv);
    grn_io_win_unmap(&sw);
  } else {
    rc = GRN_NO_MEMORY_AVAILABLE;
  }
  if (!rc) {
    int j = 0;
    uint8_t *enc;
    uint32_t encsize;
    uint32_t np = posp - dv[ii->n_elements - 1].data;
    uint32_t f_s = (ndf < 3) ? 0 : USE_P_ENC;
    uint32_t f_d = ((ndf < 16) || (ndf <= (lid.rid >> 8))) ? 0 : USE_P_ENC;
    dv[j].data_size = ndf; dv[j++].flags = f_d;
    if ((ii->header->flags & GRN_OBJ_WITH_SECTION)) {
      dv[j].data_size = ndf; dv[j++].flags = f_s;
    }
    dv[j].data_size = ndf; dv[j++].flags = f_s;
    if ((ii->header->flags & GRN_OBJ_WITH_WEIGHT)) {
      dv[j].data_size = ndf; dv[j++].flags = f_s;
    }
    if ((ii->header->flags & GRN_OBJ_WITH_POSITION)) {
      uint32_t f_p = ((np < 32) || (np <= (spos >> 13))) ? 0 : USE_P_ENC;
      dv[j].data_size = np; dv[j].flags = f_p|ODD;
    }
    if ((enc = GRN_MALLOC((ndf * 4 + np) * 2))) {
      encsize = grn_p_encv(ctx, dv, ii->n_elements, enc);
      if (!(rc = chunk_flush(ctx, ii, cinfo, enc, encsize))) {
        chunk_free(ctx, ii, segno, 0, size);
      }
      GRN_FREE(enc);
    } else {
      rc = GRN_NO_MEMORY_AVAILABLE;
    }
  }
  *balance += (ndf - sdf);
  return rc;
}

static grn_rc
buffer_merge(grn_ctx *ctx, grn_ii *ii, uint32_t seg, grn_hash *h,
              buffer *sb, uint8_t *sc, buffer *db, uint8_t *dc)
{
  buffer_term *bt;
  grn_rc rc = GRN_SUCCESS;
  uint8_t *sbp = NULL, *dcp = dc;
  datavec dv[MAX_N_ELEMENTS + 1];
  datavec rdv[MAX_N_ELEMENTS + 1];
  uint16_t n = db->header.nterms, nterms_void = 0;
  size_t unitsize = (S_SEGMENT + sb->header.chunk_size / sb->header.nterms) * 2;
  // size_t unitsize = (S_SEGMENT + sb->header.chunk_size) * 2 + (1<<24);
  size_t totalsize = unitsize * ii->n_elements;
  //todo : realloc
  if ((rc = datavec_init(ctx, dv, ii->n_elements, unitsize, totalsize))) {
    return rc;
  }
  datavec_init(ctx, rdv, ii->n_elements, 0, 0);
  if ((ii->header->flags & GRN_OBJ_WITH_POSITION)) {
    rdv[ii->n_elements - 1].flags = ODD;
  }
  for (bt = db->terms; n; n--, bt++) {
    uint16_t nextb;
    uint64_t spos = 0;
    int32_t balance = 0;
    uint32_t *ridp, *sidp = NULL, *tfp, *weightp = NULL, *posp, nchunks = 0;
    uint32_t nvchunks = 0;
    chunk_info *cinfo = NULL;
    grn_id crid = GRN_ID_NIL;
    docinfo cid = {0, 0, 0, 0, 0}, lid = {0, 0, 0, 0, 0}, bid = {0, 0};
    uint32_t sdf = 0, snn = 0, ndf;
    uint32_t *srp = NULL, *ssp = NULL, *stp = NULL, *sop = NULL, *snp = NULL;
    if (!bt->tid) {
      nterms_void++;
      continue;
    }
    if (!bt->pos_in_buffer) {
      GRN_ASSERT(!bt->size_in_buffer);
      if (bt->size_in_chunk) {
        grn_memcpy(dcp, sc + bt->pos_in_chunk, bt->size_in_chunk);
        bt->pos_in_chunk = (uint32_t)(dcp - dc);
        dcp += bt->size_in_chunk;
      }
      continue;
    }
    nextb = bt->pos_in_buffer;
    GETNEXTB();
    if (sc && bt->size_in_chunk) {
      uint8_t *scp = sc + bt->pos_in_chunk;
      uint8_t *sce = scp + bt->size_in_chunk;
      size_t size = S_SEGMENT * ii->n_elements;
      if ((bt->tid & CHUNK_SPLIT)) {
        int i;
        GRN_B_DEC(nchunks, scp);
        if (!(cinfo = GRN_MALLOCN(chunk_info, nchunks + 1))) {
          datavec_fin(ctx, dv);
          datavec_fin(ctx, rdv);
          return GRN_NO_MEMORY_AVAILABLE;
        }
        for (i = 0; i < nchunks; i++) {
          GRN_B_DEC(cinfo[i].segno, scp);
          GRN_B_DEC(cinfo[i].size, scp);
          GRN_B_DEC(cinfo[i].dgap, scp);
          crid += cinfo[i].dgap;
          if (bid.rid <= crid) {
            rc = chunk_merge(ctx, ii, sb, bt, &cinfo[i], crid, dv,
                             &nextb, &sbp, &bid, &balance);
            if (rc) {
              datavec_fin(ctx, dv);
              datavec_fin(ctx, rdv);
              return rc;
            }
          }
          if (cinfo[i].size) {
            nvchunks++;
          } else {
            crid -= cinfo[i].dgap;
            cinfo[i + 1].dgap += cinfo[i].dgap;
          }
        }
      }
      if (sce > scp) {
        size += grn_p_decv(ctx, scp, sce - scp, rdv, ii->n_elements);
        {
          int j = 0;
          sdf = rdv[j].data_size;
          srp = rdv[j++].data;
          if ((ii->header->flags & GRN_OBJ_WITH_SECTION)) { ssp = rdv[j++].data; }
          stp = rdv[j++].data;
          if ((ii->header->flags & GRN_OBJ_WITH_WEIGHT)) { sop = rdv[j++].data; }
          snn = rdv[j].data_size;
          snp = rdv[j].data;
        }
        if ((rc = datavec_reset(ctx, dv, ii->n_elements, sdf + S_SEGMENT, size))) {
          datavec_fin(ctx, dv);
          datavec_fin(ctx, rdv);
          return rc;
        }
      }
    }
    {
      int j = 0;
      ridp =   dv[j++].data;
      if ((ii->header->flags & GRN_OBJ_WITH_SECTION)) { sidp = dv[j++].data; }
      tfp =    dv[j++].data;
      if ((ii->header->flags & GRN_OBJ_WITH_WEIGHT)) { weightp = dv[j++].data; }
      posp = dv[j].data;
    }
    GETNEXTC();
    MERGE_BC(1);
    GRN_ASSERT(posp < dv[ii->n_elements].data);
    ndf = ridp - dv[0].data;
    /*
    {
      grn_obj buf;
      uint32_t rid, sid, tf, i, pos, *pp;
      GRN_TEXT_INIT(&buf, 0);
      rid = 0;
      pp = dv[3].data;
      for (i = 0; i < ndf; i++) {
        GRN_BULK_REWIND(&buf);
        rid += dv[0].data[i];
        if (dv[0].data[i]) { sid = 0; }
        sid += dv[1].data[i] + 1;
        tf = dv[2].data[i] + 1;
        pos = 0;
        grn_text_itoa(ctx, &buf, rid);
        GRN_TEXT_PUTC(ctx, &buf, ':');
        grn_text_itoa(ctx, &buf, sid);
        GRN_TEXT_PUTC(ctx, &buf, ':');
        grn_text_itoa(ctx, &buf, tf);
        GRN_TEXT_PUTC(ctx, &buf, ':');
        while (tf--) {
          pos += *pp++;
          grn_text_itoa(ctx, &buf, pos);
          if (tf) { GRN_TEXT_PUTC(ctx, &buf, ','); }
        }
        GRN_TEXT_PUTC(ctx, &buf, '\0');
        GRN_LOG(ctx, GRN_LOG_NOTICE, "Posting:%s", GRN_TEXT_VALUE(&buf));
      }
      GRN_OBJ_FIN(ctx, &buf);
    }
    */
    {
      grn_id tid = bt->tid & GRN_ID_MAX;
      uint32_t *a = array_at(ctx, ii, tid);
      if (!a) {
        GRN_LOG(ctx, GRN_LOG_NOTICE, "array_entry not found tid=%d", tid);
        memset(bt, 0, sizeof(buffer_term));
        nterms_void++;
      } else {
        if (!ndf && !nvchunks) {
          a[0] = 0;
          a[1] = 0;
          lexicon_delete(ctx, ii, tid, h);
          memset(bt, 0, sizeof(buffer_term));
          nterms_void++;
        } else if ((ii->header->flags & GRN_OBJ_WITH_SECTION)
                   && !nvchunks && ndf == 1 && lid.rid < 0x100000 &&
                   lid.sid < 0x800 && lid.tf == 1 && lid.weight == 0) {
          a[0] = (lid.rid << 12) + (lid.sid << 1) + 1;
          a[1] = (ii->header->flags & GRN_OBJ_WITH_POSITION) ? posp[-1] : 0;
          memset(bt, 0, sizeof(buffer_term));
          nterms_void++;
        } else if (!(ii->header->flags & GRN_OBJ_WITH_SECTION)
                   && !nvchunks && ndf == 1 && lid.tf == 1 && lid.weight == 0) {
          a[0] = (lid.rid << 1) + 1;
          a[1] = (ii->header->flags & GRN_OBJ_WITH_POSITION) ? posp[-1] : 0;
          memset(bt, 0, sizeof(buffer_term));
          nterms_void++;
        } else {
          int j = 0;
          uint8_t *dcp0;
          uint32_t encsize;
          uint32_t f_s = (ndf < 3) ? 0 : USE_P_ENC;
          uint32_t f_d = ((ndf < 16) || (ndf <= (lid.rid >> 8))) ? 0 : USE_P_ENC;
          dv[j].data_size = ndf; dv[j++].flags = f_d;
          if ((ii->header->flags & GRN_OBJ_WITH_SECTION)) {
            dv[j].data_size = ndf; dv[j++].flags = f_s;
          }
          dv[j].data_size = ndf; dv[j++].flags = f_s;
          if ((ii->header->flags & GRN_OBJ_WITH_WEIGHT)) {
            dv[j].data_size = ndf; dv[j++].flags = f_s;
          }
          if ((ii->header->flags & GRN_OBJ_WITH_POSITION)) {
            uint32_t np = posp - dv[ii->n_elements - 1].data;
            uint32_t f_p = ((np < 32) || (np <= (spos >> 13))) ? 0 : USE_P_ENC;
            dv[j].data_size = np; dv[j].flags = f_p|ODD;
          }
          dcp0 = dcp;
          a[1] = (bt->size_in_chunk ? a[1] : 0) + (ndf - sdf) + balance;
          if (nvchunks) {
            int i;
            GRN_B_ENC(nvchunks, dcp);
            for (i = 0; i < nchunks; i++) {
              if (cinfo[i].size) {
                GRN_B_ENC(cinfo[i].segno, dcp);
                GRN_B_ENC(cinfo[i].size, dcp);
                GRN_B_ENC(cinfo[i].dgap, dcp);
              }
            }
          }
          encsize = grn_p_encv(ctx, dv, ii->n_elements, dcp);

          if (sb->header.chunk_size + S_SEGMENT <= (dcp - dc) + encsize) {
            int i;
#define BUF_SIZE 255
            char buf[BUF_SIZE], *bufp, *buf_end;
            buf_end = buf + BUF_SIZE;
#undef BUF_SIZE
            GRN_LOG(ctx, GRN_LOG_NOTICE,
                    "cs(%d)+(%d)=(%d)<=(%" GRN_FMT_LLD ")+(%d)=(%" GRN_FMT_LLD ")",
                    sb->header.chunk_size, S_SEGMENT, sb->header.chunk_size + S_SEGMENT,
                    (long long int)(dcp - dc), encsize, (long long int)((dcp - dc) + encsize));
            for (j = 0; j < ii->n_elements; j++) {
              GRN_LOG(ctx, GRN_LOG_NOTICE, "rdv[%d] data_size=%d, flags=%d",
                      j, rdv[j].data_size, rdv[j].flags);
              for (i = 0, bufp = buf; i < rdv[j].data_size;) {
                bufp += grn_snprintf(bufp,
                                     buf_end - bufp,
                                     buf_end - bufp,
                                     " %d", rdv[j].data[i]);
                i++;
                if (!(i % 32) || i == rdv[j].data_size) {
                  GRN_LOG(ctx, GRN_LOG_NOTICE, "rdv[%d].data[%d]%s", j, i, buf);
                  bufp = buf;
                }
              }
            }

            for (j = 0; j < ii->n_elements; j++) {
              GRN_LOG(ctx, GRN_LOG_NOTICE, "dv[%d] data_size=%d, flags=%d",
                      j, dv[j].data_size, dv[j].flags);
              for (i = 0, bufp = buf; i < dv[j].data_size;) {
                bufp += grn_snprintf(bufp,
                                     buf_end - bufp,
                                     buf_end - bufp,
                                     " %d", dv[j].data[i]);
                i++;
                if (!(i % 32) || i == dv[j].data_size) {
                  GRN_LOG(ctx, GRN_LOG_NOTICE, "dv[%d].data[%d]%s", j, i, buf);
                  bufp = buf;
                }
              }
            }

          }

          if (encsize > CHUNK_SPLIT_THRESHOLD &&
              (cinfo || (cinfo = GRN_MALLOCN(chunk_info, nchunks + 1))) &&
              !chunk_flush(ctx, ii, &cinfo[nchunks], dcp, encsize)) {
            int i;
            cinfo[nchunks].dgap = lid.rid - crid;
            nvchunks++;
            dcp = dcp0;
            GRN_B_ENC(nvchunks, dcp);
            for (i = 0; i <= nchunks; i++) {
              if (cinfo[i].size) {
                GRN_B_ENC(cinfo[i].segno, dcp);
                GRN_B_ENC(cinfo[i].size, dcp);
                GRN_B_ENC(cinfo[i].dgap, dcp);
              }
            }
            GRN_LOG(ctx, GRN_LOG_NOTICE, "split (%d) encsize=%d", tid, encsize);
            bt->tid |= CHUNK_SPLIT;
          } else {
            dcp += encsize;
            if (!nvchunks) {
              bt->tid &= ~CHUNK_SPLIT;
            }
          }
          bt->pos_in_chunk = (uint32_t)(dcp0 - dc);
          bt->size_in_chunk = (uint32_t)(dcp - dcp0);
          bt->size_in_buffer = 0;
          bt->pos_in_buffer = 0;
        }
        array_unref(ii, tid);
      }
    }
    if (cinfo) { GRN_FREE(cinfo); }
  }
  datavec_fin(ctx, rdv);
  datavec_fin(ctx, dv);
  db->header.chunk_size = (uint32_t)(dcp - dc);
  db->header.buffer_free =
    S_SEGMENT - sizeof(buffer_header) - db->header.nterms * sizeof(buffer_term);
  db->header.nterms_void = nterms_void;
  return rc;
}

static void
fake_map(grn_ctx *ctx, grn_io *io, grn_io_win *iw, void *addr, uint32_t seg, uint32_t size)
{
  iw->ctx = ctx;
  iw->diff = 0;
  iw->io = io;
  iw->mode = grn_io_wronly;
  iw->segment = ((seg) >> GRN_II_N_CHUNK_VARIATION);
  iw->offset = (((seg) & ((1 << GRN_II_N_CHUNK_VARIATION) - 1)) << GRN_II_W_LEAST_CHUNK);
  iw->size = size;
  iw->cached = 0;
  iw->addr = addr;
}

static grn_rc
buffer_flush(grn_ctx *ctx, grn_ii *ii, uint32_t seg, grn_hash *h)
{
  grn_rc rc;
  grn_io_win sw, dw;
  buffer *sb, *db = NULL;
  uint8_t *dc, *sc = NULL;
  uint32_t ds, pseg, scn, dcn = 0;
  if (ii->header->binfo[seg] == NOT_ASSIGNED) { return GRN_FILE_CORRUPT; }
  if ((ds = segment_get(ctx, ii)) == MAX_PSEG) { return GRN_NO_MEMORY_AVAILABLE; }
  pseg = buffer_open(ctx, ii, SEG2POS(seg, 0), NULL, &sb);
  if (pseg != NOT_ASSIGNED) {
    GRN_IO_SEG_REF(ii->seg, ds, db);
    if (db) {
      uint32_t actual_chunk_size = 0;
      uint32_t max_dest_chunk_size = sb->header.chunk_size + S_SEGMENT;
      if ((dc = GRN_MALLOC(max_dest_chunk_size * 2))) {
        if ((scn = sb->header.chunk) == NOT_ASSIGNED ||
            (sc = WIN_MAP(ii->chunk, ctx, &sw, scn, 0,
                          sb->header.chunk_size, grn_io_rdonly))) {
          uint16_t n = sb->header.nterms;
          memset(db, 0, S_SEGMENT);
          grn_memcpy(db->terms, sb->terms, n * sizeof(buffer_term));
          db->header.nterms = n;
          if (!(rc = buffer_merge(ctx, ii, seg, h, sb, sc, db, dc))) {
            actual_chunk_size = db->header.chunk_size;
            if (actual_chunk_size >= max_dest_chunk_size) {
              GRN_LOG(ctx, GRN_LOG_WARNING, "actual_chunk_size(%d) >= max_dest_chunk_size(%d)",
                      actual_chunk_size, max_dest_chunk_size);
            }
            if (!actual_chunk_size || !(rc = chunk_new(ctx, ii, &dcn, actual_chunk_size))) {
              db->header.chunk = actual_chunk_size ? dcn : NOT_ASSIGNED;
              fake_map(ctx, ii->chunk, &dw, dc, dcn, actual_chunk_size);
              if (!(rc = grn_io_win_unmap(&dw))) {
                buffer_segment_update(ii, seg, ds);
                ii->header->total_chunk_size += actual_chunk_size;
                if (scn != NOT_ASSIGNED) {
                  grn_io_win_unmap(&sw);
                  chunk_free(ctx, ii, scn, 0, sb->header.chunk_size);
                  ii->header->total_chunk_size -= sb->header.chunk_size;
                }
              } else {
                GRN_FREE(dc);
                if (actual_chunk_size) {
                  chunk_free(ctx, ii, dcn, 0, actual_chunk_size);
                }
                if (scn != NOT_ASSIGNED) { grn_io_win_unmap(&sw); }
              }
            } else {
              GRN_FREE(dc);
              if (scn != NOT_ASSIGNED) { grn_io_win_unmap(&sw); }
            }
          } else {
            GRN_FREE(dc);
            if (scn != NOT_ASSIGNED) { grn_io_win_unmap(&sw); }
          }
        } else {
          GRN_FREE(dc);
          rc = GRN_NO_MEMORY_AVAILABLE;
        }
      } else {
        rc = GRN_NO_MEMORY_AVAILABLE;
      }
      GRN_IO_SEG_UNREF(ii->seg, ds);
    } else {
      rc = GRN_NO_MEMORY_AVAILABLE;
    }
    buffer_close(ctx, ii, pseg);
  } else {
    rc = GRN_NO_MEMORY_AVAILABLE;
  }
  return rc;
}

void
grn_ii_buffer_check(grn_ctx *ctx, grn_ii *ii, uint32_t seg)
{
  grn_rc rc;
  grn_io_win sw;
  buffer *sb;
  uint8_t *sc = NULL;
  uint32_t pseg, scn, nterms_with_corrupt_chunk = 0, nterm_with_chunk = 0;
  uint32_t ndeleted_terms_with_value = 0;
  buffer_term *bt;
  uint8_t *sbp = NULL;
  datavec rdv[MAX_N_ELEMENTS + 1];
  uint16_t n;
  int nterms_void = 0;
  int size_in_buffer = 0;
  grn_obj buf;
  size_t lower_bound;
  int64_t nloops = 0, nviolations = 0;
  if (ii->header->binfo[seg] == NOT_ASSIGNED) {
    GRN_OUTPUT_BOOL(GRN_FALSE);
    return;
  }
  pseg = buffer_open(ctx, ii, SEG2POS(seg, 0), NULL, &sb);
  if (pseg == NOT_ASSIGNED) {
    GRN_OUTPUT_BOOL(GRN_FALSE);
    return;
  }
  lower_bound =
    (sb->header.buffer_free + sizeof(buffer_term) * sb->header.nterms)
    / sizeof(buffer_rec);
  datavec_init(ctx, rdv, ii->n_elements, 0, 0);
  if ((ii->header->flags & GRN_OBJ_WITH_POSITION)) {
    rdv[ii->n_elements - 1].flags = ODD;
  }
  GRN_OUTPUT_MAP_OPEN("BUFFER", -1);
  GRN_OUTPUT_CSTR("buffer id");
  GRN_OUTPUT_INT64(seg);
  if ((scn = sb->header.chunk) == NOT_ASSIGNED) {
    GRN_OUTPUT_CSTR("void chunk size");
    GRN_OUTPUT_INT64(sb->header.chunk_size);
  } else {
    if ((sc = WIN_MAP(ii->chunk, ctx, &sw, scn, 0, sb->header.chunk_size, grn_io_rdonly))) {
      GRN_OUTPUT_CSTR("chunk size");
      GRN_OUTPUT_INT64(sb->header.chunk_size);
    } else {
      GRN_OUTPUT_CSTR("unmappable chunk size");
      GRN_OUTPUT_INT64(sb->header.chunk_size);
    }
  }
  GRN_OUTPUT_CSTR("buffer term");
  GRN_OUTPUT_ARRAY_OPEN("TERMS", sb->header.nterms);

  GRN_OBJ_INIT(&buf, GRN_BULK, 0, ii->lexicon->header.domain);
  for (bt = sb->terms, n = sb->header.nterms; n; n--, bt++) {
    grn_id tid, tid_;
    char key[GRN_TABLE_MAX_KEY_SIZE];
    int key_size;
    uint16_t nextb;
    uint32_t nchunks = 0;
    chunk_info *cinfo = NULL;
    grn_id crid = GRN_ID_NIL;
    docinfo bid = {0, 0};
    uint32_t sdf = 0, snn = 0;
    uint32_t *srp = NULL, *ssp = NULL, *stp = NULL, *sop = NULL, *snp = NULL;
    if (!bt->tid && !bt->pos_in_buffer && !bt->size_in_buffer) {
      nterms_void++;
      continue;
    }
    GRN_OUTPUT_ARRAY_OPEN("TERM", -1);
    tid = (bt->tid & GRN_ID_MAX);
    key_size = grn_table_get_key(ctx, ii->lexicon, tid, key, GRN_TABLE_MAX_KEY_SIZE);
    tid_ = grn_table_get(ctx, ii->lexicon, key, key_size);
    GRN_TEXT_SET(ctx, &buf, key, key_size);
    GRN_OUTPUT_OBJ(&buf, NULL);
    GRN_OUTPUT_INT64(bt->tid);
    GRN_OUTPUT_INT64(tid_);
    nextb = bt->pos_in_buffer;
    size_in_buffer += bt->size_in_buffer;
    if (tid != tid_ && (bt->size_in_buffer || bt->size_in_chunk)) {
      ndeleted_terms_with_value++;
    }
    GETNEXTB();
    GRN_OUTPUT_INT64(bt->size_in_buffer);
    GRN_OUTPUT_INT64(bt->size_in_chunk);
    if (sc && bt->size_in_chunk) {
      uint8_t *scp = sc + bt->pos_in_chunk;
      uint8_t *sce = scp + bt->size_in_chunk;
      size_t size = S_SEGMENT * ii->n_elements;
      if ((bt->tid & CHUNK_SPLIT)) {
        int i;
        GRN_B_DEC(nchunks, scp);
        if (!(cinfo = GRN_MALLOCN(chunk_info, nchunks + 1))) {
          datavec_fin(ctx, rdv);
          GRN_OBJ_FIN(ctx, &buf);
          return;
        }
        for (i = 0; i < nchunks; i++) {
          GRN_B_DEC(cinfo[i].segno, scp);
          GRN_B_DEC(cinfo[i].size, scp);
          GRN_B_DEC(cinfo[i].dgap, scp);
          crid += cinfo[i].dgap;
        }
      }
      if (sce > scp) {
        size += grn_p_decv(ctx, scp, sce - scp, rdv, ii->n_elements);
        {
          int j = 0;
          sdf = rdv[j].data_size;
          GRN_OUTPUT_INT64(sdf);
          srp = rdv[j++].data;
          if ((ii->header->flags & GRN_OBJ_WITH_SECTION)) { ssp = rdv[j++].data; }
          if (sdf != rdv[j].data_size) {
            nterms_with_corrupt_chunk++;
          }
          stp = rdv[j++].data;
          if ((ii->header->flags & GRN_OBJ_WITH_WEIGHT)) { sop = rdv[j++].data; }
          GRN_OUTPUT_INT64(rdv[j].data_size);
          snn = rdv[j].data_size;
          snp = rdv[j].data;
        }
        nterm_with_chunk++;
      }
    }
    {
      uint16_t pos;
      grn_id rid, sid, rid_ = 0, sid_ = 0;
      uint8_t *p;
      buffer_rec *r;
      for (pos = bt->pos_in_buffer; pos; pos = r->step) {
        if (pos < lower_bound) {
          nviolations++;
        }
        r = BUFFER_REC_AT(sb, pos);
        p = NEXT_ADDR(r);
        GRN_B_DEC(rid, p);
        if ((ii->header->flags & GRN_OBJ_WITH_SECTION)) {
          GRN_B_DEC(sid, p);
        } else {
          sid = 1;
        }
        if (rid < rid_ || (rid == rid_ && sid < sid_)) {
          nloops++;
        }
        rid_ = rid;
        sid_ = sid;
      }
    }
    GRN_OUTPUT_ARRAY_CLOSE();
    if (cinfo) { GRN_FREE(cinfo); }
  }
  GRN_OBJ_FIN(ctx, &buf);

  GRN_OUTPUT_ARRAY_CLOSE();
  GRN_OUTPUT_CSTR("buffer free");
  GRN_OUTPUT_INT64(sb->header.buffer_free);
  GRN_OUTPUT_CSTR("size in buffer");
  GRN_OUTPUT_INT64(size_in_buffer);
  GRN_OUTPUT_CSTR("nterms");
  GRN_OUTPUT_INT64(sb->header.nterms);
  if (nterms_void != sb->header.nterms_void) {
    GRN_OUTPUT_CSTR("nterms void gap");
    GRN_OUTPUT_INT64(nterms_void - sb->header.nterms_void);
  }
  GRN_OUTPUT_CSTR("nterms with chunk");
  GRN_OUTPUT_INT64(nterm_with_chunk);
  if (nterms_with_corrupt_chunk) {
    GRN_OUTPUT_CSTR("nterms with corrupt chunk");
    GRN_OUTPUT_INT64(nterms_with_corrupt_chunk);
  }
  if (ndeleted_terms_with_value) {
    GRN_OUTPUT_CSTR("number of deleted terms with value");
    GRN_OUTPUT_INT64(ndeleted_terms_with_value);
  }
  if (nloops) {
    GRN_OUTPUT_CSTR("number of loops");
    GRN_OUTPUT_INT64(nloops);
  }
  if (nviolations) {
    GRN_OUTPUT_CSTR("number of violations");
    GRN_OUTPUT_INT64(nviolations);
  }
  GRN_OUTPUT_MAP_CLOSE();
  datavec_fin(ctx, rdv);
  if (sc) { grn_io_win_unmap(&sw); }
  buffer_close(ctx, ii, pseg);
}

typedef struct {
  buffer_term *bt;
  const char *key;
  uint32_t key_size;
} term_sort;

static int
term_compar(const void *t1, const void *t2)
{
  int r;
  const term_sort *x = (term_sort *)t1, *y = (term_sort *)t2;
  if (x->key_size > y->key_size) {
    return (r = memcmp(x->key, y->key, y->key_size)) ? r : x->key_size - y->key_size;
  } else {
    return (r = memcmp(x->key, y->key, x->key_size)) ? r : x->key_size - y->key_size;
  }
}

static grn_rc
term_split(grn_ctx *ctx, grn_obj *lexicon, buffer *sb, buffer *db0, buffer *db1)
{
  uint16_t i, n, *nt;
  buffer_term *bt;
  uint32_t s, th = (sb->header.chunk_size + sb->header.nterms) >> 1;
  term_sort *ts = GRN_MALLOC(sb->header.nterms * sizeof(term_sort));
  if (!ts) { return GRN_NO_MEMORY_AVAILABLE; }
  for (i = 0, n = sb->header.nterms, bt = sb->terms; n; bt++, n--) {
    if (bt->tid) {
      grn_id tid = bt->tid & GRN_ID_MAX;
      ts[i].key = _grn_table_key(ctx, lexicon, tid, &ts[i].key_size);
      ts[i].bt = bt;
      i++;
    }
  }
  qsort(ts, i, sizeof(term_sort), term_compar);
  memset(db0, 0, S_SEGMENT);
  bt = db0->terms;
  nt = &db0->header.nterms;
  for (s = 0; n + 1 < i && s <= th; n++, bt++) {
    grn_memcpy(bt, ts[n].bt, sizeof(buffer_term));
    (*nt)++;
    s += ts[n].bt->size_in_chunk + 1;
  }
  memset(db1, 0, S_SEGMENT);
  bt = db1->terms;
  nt = &db1->header.nterms;
  for (; n < i; n++, bt++) {
    grn_memcpy(bt, ts[n].bt, sizeof(buffer_term));
    (*nt)++;
  }
  GRN_FREE(ts);
  GRN_LOG(ctx, GRN_LOG_NOTICE, "d0=%d d1=%d", db0->header.nterms, db1->header.nterms);
  return GRN_SUCCESS;
}

static void
array_update(grn_ctx *ctx, grn_ii *ii, uint32_t dls, buffer *db)
{
  uint16_t n;
  buffer_term *bt;
  uint32_t *a, pos = SEG2POS(dls, sizeof(buffer_header));
  for (n = db->header.nterms, bt = db->terms; n; n--, bt++) {
    if (bt->tid) {
      grn_id tid = bt->tid & GRN_ID_MAX;
      if ((a = array_at(ctx, ii, tid))) {
        a[0] = pos;
        array_unref(ii, tid);
      } else {
        GRN_LOG(ctx, GRN_LOG_WARNING, "array_at failed (%d)", tid);
      }
    }
    pos += sizeof(buffer_term) >> 2;
  }
}

static grn_rc
buffer_split(grn_ctx *ctx, grn_ii *ii, uint32_t seg, grn_hash *h)
{
  grn_rc rc;
  grn_io_win sw, dw0, dw1;
  buffer *sb, *db0 = NULL, *db1 = NULL;
  uint8_t *sc = NULL, *dc0, *dc1;
  uint32_t dps0, dps1, dls0, dls1, sps, scn, dcn0 = 0, dcn1 = 0;
  if (ii->header->binfo[seg] == NOT_ASSIGNED) { return GRN_FILE_CORRUPT; }
  if ((rc = buffer_segment_reserve(ctx, ii, &dls0, &dps0, &dls1, &dps1))) {
    return rc;
  }
  sps = buffer_open(ctx, ii, SEG2POS(seg, 0), NULL, &sb);
  if (sps != NOT_ASSIGNED) {
    GRN_IO_SEG_REF(ii->seg, dps0, db0);
    if (db0) {
      GRN_IO_SEG_REF(ii->seg, dps1, db1);
      if (db1) {
        uint32_t actual_db0_chunk_size = 0;
        uint32_t actual_db1_chunk_size = 0;
        uint32_t max_dest_chunk_size = sb->header.chunk_size + S_SEGMENT;
        if ((dc0 = GRN_MALLOC(max_dest_chunk_size * 2))) {
          if ((dc1 = GRN_MALLOC(max_dest_chunk_size * 2))) {
            if ((scn = sb->header.chunk) == NOT_ASSIGNED ||
                (sc = WIN_MAP(ii->chunk, ctx, &sw, scn, 0,
                              sb->header.chunk_size, grn_io_rdonly))) {
              term_split(ctx, ii->lexicon, sb, db0, db1);
              if (!(rc = buffer_merge(ctx, ii, seg, h, sb, sc, db0, dc0))) {
                actual_db0_chunk_size = db0->header.chunk_size;
                if (actual_db0_chunk_size >= max_dest_chunk_size) {
                  GRN_LOG(ctx, GRN_LOG_WARNING,
                          "actual_db0_chunk_size(%d) >= max_dest_chunk_size(%d)",
                          actual_db0_chunk_size, max_dest_chunk_size);
                }
                if (!actual_db0_chunk_size ||
                    !(rc = chunk_new(ctx, ii, &dcn0, actual_db0_chunk_size))) {
                  db0->header.chunk = actual_db0_chunk_size ? dcn0 : NOT_ASSIGNED;
                  fake_map(ctx, ii->chunk, &dw0, dc0, dcn0, actual_db0_chunk_size);
                  if (!(rc = grn_io_win_unmap(&dw0))) {
                    if (!(rc = buffer_merge(ctx, ii, seg, h, sb, sc, db1, dc1))) {
                      actual_db1_chunk_size = db1->header.chunk_size;
                      if (actual_db1_chunk_size >= max_dest_chunk_size) {
                        GRN_LOG(ctx, GRN_LOG_WARNING,
                                "actual_db1_chunk_size(%d) >= max_dest_chunk_size(%d)",
                                actual_db1_chunk_size, max_dest_chunk_size);
                      }
                      if (!actual_db1_chunk_size ||
                          !(rc = chunk_new(ctx, ii, &dcn1, actual_db1_chunk_size))) {
                        fake_map(ctx, ii->chunk, &dw1, dc1, dcn1, actual_db1_chunk_size);
                        if (!(rc = grn_io_win_unmap(&dw1))) {
                          db1->header.chunk = actual_db1_chunk_size ? dcn1 : NOT_ASSIGNED;
                          buffer_segment_update(ii, dls0, dps0);
                          buffer_segment_update(ii, dls1, dps1);
                          array_update(ctx, ii, dls0, db0);
                          array_update(ctx, ii, dls1, db1);
                          buffer_segment_clear(ii, seg);
                          ii->header->total_chunk_size += actual_db0_chunk_size;
                          ii->header->total_chunk_size += actual_db1_chunk_size;
                          if (scn != NOT_ASSIGNED) {
                            grn_io_win_unmap(&sw);
                            chunk_free(ctx, ii, scn, 0, sb->header.chunk_size);
                            ii->header->total_chunk_size -= sb->header.chunk_size;
                          }
                        } else {
                          if (actual_db1_chunk_size) {
                            chunk_free(ctx, ii, dcn1, 0, actual_db1_chunk_size);
                          }
                          if (actual_db0_chunk_size) {
                            chunk_free(ctx, ii, dcn0, 0, actual_db0_chunk_size);
                          }
                          GRN_FREE(dc1);
                          if (scn != NOT_ASSIGNED) { grn_io_win_unmap(&sw); }
                        }
                      } else {
                        if (actual_db0_chunk_size) {
                          chunk_free(ctx, ii, dcn0, 0, actual_db0_chunk_size);
                        }
                        GRN_FREE(dc1);
                        if (scn != NOT_ASSIGNED) { grn_io_win_unmap(&sw); }
                      }
                    } else {
                      if (actual_db0_chunk_size) {
                        chunk_free(ctx, ii, dcn0, 0, actual_db0_chunk_size);
                      }
                      GRN_FREE(dc1);
                      if (scn != NOT_ASSIGNED) { grn_io_win_unmap(&sw); }
                    }
                  } else {
                    if (actual_db0_chunk_size) {
                      chunk_free(ctx, ii, dcn0, 0, actual_db0_chunk_size);
                    }
                    GRN_FREE(dc1);
                    GRN_FREE(dc0);
                    if (scn != NOT_ASSIGNED) { grn_io_win_unmap(&sw); }
                  }
                } else {
                  GRN_FREE(dc1);
                  GRN_FREE(dc0);
                  if (scn != NOT_ASSIGNED) { grn_io_win_unmap(&sw); }
                }
              } else {
                GRN_FREE(dc1);
                GRN_FREE(dc0);
                if (scn != NOT_ASSIGNED) { grn_io_win_unmap(&sw); }
              }
            } else {
              GRN_FREE(dc1);
              GRN_FREE(dc0);
              rc = GRN_NO_MEMORY_AVAILABLE;
            }
          } else {
            GRN_FREE(dc0);
            rc = GRN_NO_MEMORY_AVAILABLE;
          }
        } else {
          rc = GRN_NO_MEMORY_AVAILABLE;
        }
        GRN_IO_SEG_UNREF(ii->seg, dps1);
      } else {
        rc = GRN_NO_MEMORY_AVAILABLE;
      }
      GRN_IO_SEG_UNREF(ii->seg, dps0);
    } else {
      rc = GRN_NO_MEMORY_AVAILABLE;
    }
    buffer_close(ctx, ii, sps);
  } else {
    rc = GRN_NO_MEMORY_AVAILABLE;
  }
  return rc;
}

#define SCALE_FACTOR 2048
#define MAX_NTERMS   8192
#define SPLIT_COND  (b->header.nterms > 1024 ||\
                     (b->header.nterms > 1 &&\
                      b->header.chunk_size * 100 > ii->header->total_chunk_size))

inline static uint32_t
buffer_new(grn_ctx *ctx, grn_ii *ii, int size, uint32_t *pos,
           buffer_term **bt, buffer_rec **br, buffer **bp, grn_id id, grn_hash *h)
{
  buffer *b = NULL;
  grn_id tid;
  uint16_t offset;
  char key[GRN_TABLE_MAX_KEY_SIZE];
  // unsigned int key_size;
  // const char *key = _grn_table_key(ctx, ii->lexicon, id, &key_size);
  int key_size = grn_table_get_key(ctx, ii->lexicon, id, key, GRN_TABLE_MAX_KEY_SIZE);
  uint32_t *a, lseg = NOT_ASSIGNED, pseg = NOT_ASSIGNED;
  grn_table_cursor *tc = NULL;
  if (S_SEGMENT - sizeof(buffer_header) < size + sizeof(buffer_term)) {
    GRN_LOG(ctx, GRN_LOG_CRIT, "requested size(%d) is too large", size);
    return NOT_ASSIGNED;
  }
  if (ii->lexicon->header.type == GRN_TABLE_PAT_KEY) {
    if (ii->lexicon->header.flags & GRN_OBJ_KEY_VAR_SIZE) {
      tc = grn_table_cursor_open(ctx, ii->lexicon, key, key_size, NULL, 0, 0, -1,
                                 GRN_CURSOR_ASCENDING|GRN_CURSOR_GT);
    } else {
      tc = grn_table_cursor_open(ctx, ii->lexicon, NULL, 0, key, key_size, 0, -1,
                                 GRN_CURSOR_PREFIX);
    }
  } else {
    tc = grn_table_cursor_open(ctx, ii->lexicon, NULL, 0, NULL, 0, 0, -1,
                               GRN_CURSOR_ASCENDING);
  }
  if (tc) {
    while (lseg == NOT_ASSIGNED && (tid = grn_table_cursor_next(ctx, tc))) {
      if ((a = array_at(ctx, ii, tid))) {
        for (;;) {
          uint32_t pos = a[0];
          if (!pos || (pos & 1)) { break; }
          if ((pseg = buffer_open(ctx, ii, pos, NULL, &b)) == NOT_ASSIGNED) { break; }
          if (b->header.buffer_free >= size + sizeof(buffer_term)) {
            lseg = LSEG(pos);
            break;
          }
          buffer_close(ctx, ii, pseg);
          if (SPLIT_COND)
            /* ((S_SEGMENT - sizeof(buffer_header) + ii->header->bmax -
               b->header.nterms * sizeof(buffer_term)) * 4 <
               b->header.chunk_size) */
            {
              GRN_LOG(ctx, GRN_LOG_NOTICE,
                      "nterms=%d chunk=%d total=%" GRN_FMT_INT64U,
                      b->header.nterms,
                      b->header.chunk_size,
                      ii->header->total_chunk_size >> 10);
            if (buffer_split(ctx, ii, LSEG(pos), h)) { break; }
          } else {
            if (S_SEGMENT - sizeof(buffer_header)
                - b->header.nterms * sizeof(buffer_term)
                < size + sizeof(buffer_term)) {
              break;
            }
            if (buffer_flush(ctx, ii, LSEG(pos), h)) { break; }
          }
        }
        array_unref(ii, tid);
      }
    }
    grn_table_cursor_close(ctx, tc);
  }
  if (lseg == NOT_ASSIGNED) {
    if (buffer_segment_new(ctx, ii, &lseg) ||
        (pseg = buffer_open(ctx, ii, SEG2POS(lseg, 0), NULL, &b)) == NOT_ASSIGNED) {
      return NOT_ASSIGNED;
    }
    memset(b, 0, S_SEGMENT);
    b->header.buffer_free = S_SEGMENT - sizeof(buffer_header);
    b->header.chunk = NOT_ASSIGNED;
  }
  if (b->header.nterms_void) {
    for (offset = 0; offset < b->header.nterms; offset++) {
      if (!b->terms[offset].tid) { break; }
    }
    if (offset == b->header.nterms) {
      GRN_LOG(ctx, GRN_LOG_NOTICE, "inconsistent buffer(%d)", lseg);
      b->header.nterms_void = 0;
      b->header.nterms++;
      b->header.buffer_free -= size + sizeof(buffer_term);
    } else {
      b->header.nterms_void--;
      b->header.buffer_free -= size;
    }
  } else {
    offset = b->header.nterms++;
    b->header.buffer_free -= size + sizeof(buffer_term);
  }
  *pos = SEG2POS(lseg, (sizeof(buffer_header) + sizeof(buffer_term) * offset));
  *bt = &b->terms[offset];
  *br = (buffer_rec *)(((byte *)&b->terms[b->header.nterms]) + b->header.buffer_free);
  *bp = b;
  return pseg;
}

/* ii */

static grn_ii *
_grn_ii_create(grn_ctx *ctx, grn_ii *ii, const char *path, grn_obj *lexicon, uint32_t flags)
{
  int i;
  grn_io *seg, *chunk;
  char path2[PATH_MAX];
  struct grn_ii_header *header;
  grn_obj_flags lflags;
  grn_encoding encoding;
  grn_obj *tokenizer;
  /*
  for (i = 0; i < 32; i++) {
    new_histogram[i] = 0;
    free_histogram[i] = 0;
  }
  */
  if (grn_table_get_info(ctx, lexicon, &lflags, &encoding, &tokenizer,
                         NULL, NULL)) {
    return NULL;
  }
  if (path && strlen(path) + 6 >= PATH_MAX) { return NULL; }
  seg = grn_io_create(ctx, path, sizeof(struct grn_ii_header),
                      S_SEGMENT, MAX_PSEG, grn_io_auto, GRN_IO_EXPIRE_SEGMENT);
  if (!seg) { return NULL; }
  if (path) {
    strcpy(path2, path);
    strcat(path2, ".c");
    chunk = grn_io_create(ctx, path2, 0, S_CHUNK, GRN_II_MAX_CHUNK, grn_io_auto,
                          GRN_IO_EXPIRE_SEGMENT);
  } else {
    chunk = grn_io_create(ctx, NULL, 0, S_CHUNK, GRN_II_MAX_CHUNK, grn_io_auto, 0);
  }
  if (!chunk) {
    grn_io_close(ctx, seg);
    return NULL;
  }
  header = grn_io_header(seg);
  grn_io_set_type(seg, GRN_COLUMN_INDEX);
  for (i = 0; i < GRN_II_MAX_LSEG; i++) {
    header->ainfo[i] = NOT_ASSIGNED;
    header->binfo[i] = NOT_ASSIGNED;
  }
  for (i = 0; i <= GRN_II_N_CHUNK_VARIATION; i++) {
    header->free_chunks[i] = NOT_ASSIGNED;
    header->garbages[i] = NOT_ASSIGNED;
  }
  header->flags = flags;
  ii->seg = seg;
  ii->chunk = chunk;
  ii->lexicon = lexicon;
  ii->lflags = lflags;
  ii->encoding = encoding;
  ii->header = header;
  ii->n_elements = 2;
  if ((flags & GRN_OBJ_WITH_SECTION)) { ii->n_elements++; }
  if ((flags & GRN_OBJ_WITH_WEIGHT)) { ii->n_elements++; }
  if ((flags & GRN_OBJ_WITH_POSITION)) { ii->n_elements++; }
  return ii;
}

grn_ii *
grn_ii_create(grn_ctx *ctx, const char *path, grn_obj *lexicon, uint32_t flags)
{
  grn_ii *ii = NULL;
  if (!(ii = GRN_GMALLOC(sizeof(grn_ii)))) {
    return NULL;
  }
  GRN_DB_OBJ_SET_TYPE(ii, GRN_COLUMN_INDEX);
  if (!_grn_ii_create(ctx, ii, path, lexicon, flags)) {
    GRN_FREE(ii);
    return NULL;
  }
  return ii;
}

grn_rc
grn_ii_remove(grn_ctx *ctx, const char *path)
{
  grn_rc rc;
  char buffer[PATH_MAX];
  if (!path || strlen(path) > PATH_MAX - 4) { return GRN_INVALID_ARGUMENT; }
  if ((rc = grn_io_remove(ctx, path))) { goto exit; }
  snprintf(buffer, PATH_MAX, "%s.c", path);
  rc = grn_io_remove(ctx, buffer);
exit :
  return rc;
}

grn_rc
grn_ii_truncate(grn_ctx *ctx, grn_ii *ii)
{
  grn_rc rc;
  const char *io_segpath, *io_chunkpath;
  char *segpath, *chunkpath = NULL;
  grn_obj *lexicon;
  uint32_t flags;
  if ((io_segpath = grn_io_path(ii->seg)) && *io_segpath != '\0') {
    if (!(segpath = GRN_STRDUP(io_segpath))) {
      ERR(GRN_NO_MEMORY_AVAILABLE, "cannot duplicate path: <%s>", io_segpath);
      return GRN_NO_MEMORY_AVAILABLE;
    }
    if ((io_chunkpath = grn_io_path(ii->chunk)) && *io_chunkpath != '\0') {
      if (!(chunkpath = GRN_STRDUP(io_chunkpath))) {
        ERR(GRN_NO_MEMORY_AVAILABLE, "cannot duplicate path: <%s>", io_chunkpath);
        return GRN_NO_MEMORY_AVAILABLE;
      }
    } else {
      chunkpath = NULL;
    }
  } else {
    segpath = NULL;
  }
  lexicon = ii->lexicon;
  flags = ii->header->flags;
  if ((rc = grn_io_close(ctx, ii->seg))) { goto exit; }
  if ((rc = grn_io_close(ctx, ii->chunk))) { goto exit; }
  ii->seg = NULL;
  ii->chunk = NULL;
  if (segpath && (rc = grn_io_remove(ctx, segpath))) { goto exit; }
  if (chunkpath && (rc = grn_io_remove(ctx, chunkpath))) { goto exit; }
  if (!_grn_ii_create(ctx, ii, segpath, lexicon, flags)) {
    rc = GRN_UNKNOWN_ERROR;
  }
exit:
  if (segpath) { GRN_FREE(segpath); }
  if (chunkpath) { GRN_FREE(chunkpath); }
  return rc;
}

grn_ii *
grn_ii_open(grn_ctx *ctx, const char *path, grn_obj *lexicon)
{
  grn_io *seg, *chunk;
  grn_ii *ii;
  char path2[PATH_MAX];
  struct grn_ii_header *header;
  grn_obj_flags lflags;
  grn_encoding encoding;
  grn_obj *tokenizer;
  if (grn_table_get_info(ctx, lexicon, &lflags, &encoding, &tokenizer,
                         NULL, NULL)) {
    return NULL;
  }
  if (strlen(path) + 6 >= PATH_MAX) { return NULL; }
  strcpy(path2, path);
  strcat(path2, ".c");
  seg = grn_io_open(ctx, path, grn_io_auto);
  if (!seg) { return NULL; }
  chunk = grn_io_open(ctx, path2, grn_io_auto);
  if (!chunk) {
    grn_io_close(ctx, seg);
    return NULL;
  }
  header = grn_io_header(seg);
  if (grn_io_get_type(seg) != GRN_COLUMN_INDEX) {
    ERR(GRN_INVALID_FORMAT, "file type unmatch");
    grn_io_close(ctx, seg);
    grn_io_close(ctx, chunk);
    return NULL;
  }
  if (!(ii = GRN_GMALLOC(sizeof(grn_ii)))) {
    grn_io_close(ctx, seg);
    grn_io_close(ctx, chunk);
    return NULL;
  }
  GRN_DB_OBJ_SET_TYPE(ii, GRN_COLUMN_INDEX);
  ii->seg = seg;
  ii->chunk = chunk;
  ii->lexicon = lexicon;
  ii->lflags = lflags;
  ii->encoding = encoding;
  ii->header = header;
  ii->n_elements = 2;
  if ((header->flags & GRN_OBJ_WITH_SECTION)) { ii->n_elements++; }
  if ((header->flags & GRN_OBJ_WITH_WEIGHT)) { ii->n_elements++; }
  if ((header->flags & GRN_OBJ_WITH_POSITION)) { ii->n_elements++; }
  return ii;
}

grn_rc
grn_ii_close(grn_ctx *ctx, grn_ii *ii)
{
  grn_rc rc;
  if (!ii) { return GRN_INVALID_ARGUMENT; }
  if ((rc = grn_io_close(ctx, ii->seg))) { return rc; }
  if ((rc = grn_io_close(ctx, ii->chunk))) { return rc; }
  GRN_GFREE(ii);
  /*
  {
    int i;
    for (i = 0; i < 32; i++) {
      GRN_LOG(ctx, GRN_LOG_NOTICE, "new[%d]=%d free[%d]=%d",
              i, new_histogram[i],
              i, free_histogram[i]);
    }
  }
  */
  return rc;
}

grn_rc
grn_ii_info(grn_ctx *ctx, grn_ii *ii, uint64_t *seg_size, uint64_t *chunk_size)
{
  grn_rc rc;

  if (seg_size) {
    if ((rc = grn_io_size(ctx, ii->seg, seg_size))) {
      return rc;
    }
  }

  if (chunk_size) {
    if ((rc = grn_io_size(ctx, ii->chunk, chunk_size))) {
      return rc;
    }
  }

  return GRN_SUCCESS;
}

void
grn_ii_expire(grn_ctx *ctx, grn_ii *ii)
{
  /*
  grn_io_expire(ctx, ii->seg, 128, 1000000);
  */
  grn_io_expire(ctx, ii->chunk, 0, 1000000);
}

#define BIT11_01(x) ((x >> 1) & 0x7ff)
#define BIT31_12(x) (x >> 12)

grn_rc
grn_ii_update_one(grn_ctx *ctx, grn_ii *ii, grn_id tid, grn_ii_updspec *u, grn_hash *h)
{
  grn_rc rc = GRN_SUCCESS;
  buffer *b;
  uint8_t *bs;
  buffer_rec *br = NULL;
  buffer_term *bt;
  uint32_t pseg = 0, pos = 0, size, *a;
  if (!tid) { return rc; }
  if (!u->tf || !u->sid) { return grn_ii_delete_one(ctx, ii, tid, u, h); }
  if (u->sid > ii->header->smax) { ii->header->smax = u->sid; }
  if (!(a = array_get(ctx, ii, tid))) { return GRN_NO_MEMORY_AVAILABLE; }
  if (!(bs = encode_rec(ctx, ii, u, &size, 0))) {
    rc = GRN_NO_MEMORY_AVAILABLE; goto exit;
  }
  for (;;) {
    if (a[0]) {
      if (!(a[0] & 1)) {
        pos = a[0];
        if ((pseg = buffer_open(ctx, ii, pos, &bt, &b)) == NOT_ASSIGNED) {
          rc = GRN_NO_MEMORY_AVAILABLE;
          goto exit;
        }
        if (b->header.buffer_free < size) {
          int bfb = b->header.buffer_free;
          GRN_LOG(ctx, GRN_LOG_DEBUG, "flushing a[0]=%d seg=%d(%p) free=%d",
                  a[0], LSEG(a[0]), b, b->header.buffer_free);
          buffer_close(ctx, ii, pseg);
          if (SPLIT_COND)
            /*((S_SEGMENT - sizeof(buffer_header) + ii->header->bmax -
               b->header.nterms * sizeof(buffer_term)) * 4 <
               b->header.chunk_size)*/
            {
              GRN_LOG(ctx, GRN_LOG_NOTICE,
                      "nterms=%d chunk=%d total=%" GRN_FMT_INT64U,
                      b->header.nterms,
                      b->header.chunk_size,
                      ii->header->total_chunk_size >> 10);
            if ((rc = buffer_split(ctx, ii, LSEG(pos), h))) { goto exit; }
            continue;
          }
          if ((rc = buffer_flush(ctx, ii, LSEG(pos), h))) { goto exit; }
          if (a[0] != pos) {
            GRN_LOG(ctx, GRN_LOG_DEBUG, "grn_ii_update_one: a[0] changed %d->%d", a[0], pos);
            continue;
          }
          if ((pseg = buffer_open(ctx, ii, pos, &bt, &b)) == NOT_ASSIGNED) {
            GRN_LOG(ctx, GRN_LOG_CRIT, "buffer not found a[0]=%d", a[0]);
            rc = GRN_NO_MEMORY_AVAILABLE;
            goto exit;
          }
          GRN_LOG(ctx, GRN_LOG_DEBUG, "flushed  a[0]=%d seg=%d(%p) free=%d->%d nterms=%d v=%d",
                  a[0], LSEG(a[0]), b, bfb, b->header.buffer_free,
                  b->header.nterms, b->header.nterms_void);
          if (b->header.buffer_free < size) {
            buffer_close(ctx, ii, pseg);
            GRN_LOG(ctx, GRN_LOG_CRIT, "buffer(%d) is full (%d < %d) in grn_ii_update_one",
                    a[0], b->header.buffer_free, size);
            /* todo: direct merge */
            rc = GRN_NO_MEMORY_AVAILABLE;
            goto exit;
          }
        }
        b->header.buffer_free -= size;
        br = (buffer_rec *)(((byte *)&b->terms[b->header.nterms])
                            + b->header.buffer_free);
      } else {
        grn_ii_updspec u2;
        uint32_t size2 = 0, v = a[0];
        struct _grn_ii_pos pos2;
        pos2.pos = a[1];
        pos2.next = NULL;
        u2.pos = &pos2;
        if ((ii->header->flags & GRN_OBJ_WITH_SECTION)) {
          u2.rid = BIT31_12(v);
          u2.sid = BIT11_01(v);
        } else {
          u2.rid = v >> 1;
          u2.sid = 1;
        }
        u2.tf = 1;
        u2.weight = 0;
        if (u2.rid != u->rid || u2.sid != u->sid) {
          uint8_t *bs2 = encode_rec(ctx, ii, &u2, &size2, 0);
          if (!bs2) {
            GRN_LOG(ctx, GRN_LOG_ALERT, "encode_rec on grn_ii_update_one failed !");
            rc = GRN_NO_MEMORY_AVAILABLE;
            goto exit;
          }
          pseg = buffer_new(ctx, ii, size + size2, &pos, &bt, &br, &b, tid, h);
          if (pseg == NOT_ASSIGNED) {
            GRN_FREE(bs2);
            goto exit;
          }
          bt->tid = tid;
          bt->size_in_chunk = 0;
          bt->pos_in_chunk = 0;
          bt->size_in_buffer = 0;
          bt->pos_in_buffer = 0;
          if ((rc = buffer_put(ctx, ii, b, bt, br, bs2, &u2, size2))) {
            GRN_FREE(bs2);
            buffer_close(ctx, ii, pseg);
            goto exit;
          }
          br = (buffer_rec *)(((byte *)br) + size2);
          GRN_FREE(bs2);
        }
      }
    }
    break;
  }
  if (!br) {
    if (u->tf == 1 && u->weight == 0) {
      if ((ii->header->flags & GRN_OBJ_WITH_SECTION)) {
        if (u->rid < 0x100000 && u->sid < 0x800) {
          a[0] = (u->rid << 12) + (u->sid << 1) + 1;
          a[1] = u->pos->pos;
          goto exit;
        }
      } else {
        a[0] = (u->rid << 1) + 1;
        a[1] = u->pos->pos;
        goto exit;
      }
    }
    pseg = buffer_new(ctx, ii, size, &pos, &bt, &br, &b, tid, h);
    if (pseg == NOT_ASSIGNED) { goto exit; }
    bt->tid = tid;
    bt->size_in_chunk = 0;
    bt->pos_in_chunk = 0;
    bt->size_in_buffer = 0;
    bt->pos_in_buffer = 0;
  }
  rc = buffer_put(ctx, ii, b, bt, br, bs, u, size);
  buffer_close(ctx, ii, pseg);
  if (!a[0] || (a[0] & 1)) { a[0] = pos; }
exit :
  array_unref(ii, tid);
  if (bs) { GRN_FREE(bs); }
  if (u->tf != u->atf) {
    char term[GRN_TABLE_MAX_KEY_SIZE];
    int term_size;
    term_size = grn_table_get_key(ctx, ii->lexicon, tid,
                                  term, GRN_TABLE_MAX_KEY_SIZE);
    GRN_LOG(ctx, GRN_LOG_WARNING,
            "too many postings(%d). %d postings are discarded. "
            "term: <%d>(<%.*s>)",
            u->atf, u->atf - u->tf,
            tid, term_size, term);
  }
  grn_ii_expire(ctx, ii);
  return rc;
}

grn_rc
grn_ii_delete_one(grn_ctx *ctx, grn_ii *ii, grn_id tid, grn_ii_updspec *u, grn_hash *h)
{
  grn_rc rc = GRN_SUCCESS;
  buffer *b;
  uint8_t *bs = NULL;
  buffer_rec *br;
  buffer_term *bt;
  uint32_t pseg, size, *a;
  if (!tid) { return rc; }
  if (!(a = array_at(ctx, ii, tid))) { return GRN_INVALID_ARGUMENT; }
  for (;;) {
    if (!a[0]) { goto exit; }
    if (a[0] & 1) {
      if ((ii->header->flags & GRN_OBJ_WITH_SECTION)) {
        uint32_t rid = BIT31_12(a[0]);
        uint32_t sid = BIT11_01(a[0]);
        if (u->rid == rid && (!u->sid || u->sid == sid)) {
          a[0] = 0;
          lexicon_delete(ctx, ii, tid, h);
        }
      } else {
        uint32_t rid = a[0] >> 1;
        if (u->rid == rid) {
          a[0] = 0;
          lexicon_delete(ctx, ii, tid, h);
        }
      }
      goto exit;
    }
    if (!(bs = encode_rec(ctx, ii, u, &size, 1))) {
      rc = GRN_NO_MEMORY_AVAILABLE;
      goto exit;
    }
    if ((pseg = buffer_open(ctx, ii, a[0], &bt, &b)) == NOT_ASSIGNED) {
      rc = GRN_NO_MEMORY_AVAILABLE;
      goto exit;
    }
    if (b->header.buffer_free < size) {
      uint32_t _a = a[0];
      GRN_LOG(ctx, GRN_LOG_DEBUG, "flushing! b=%p free=%d, seg(%d)", b, b->header.buffer_free, LSEG(a[0]));
      buffer_close(ctx, ii, pseg);
      if ((rc = buffer_flush(ctx, ii, LSEG(a[0]), h))) { goto exit; }
      if (a[0] != _a) {
        GRN_LOG(ctx, GRN_LOG_DEBUG, "grn_ii_delete_one: a[0] changed %d->%d)", a[0], _a);
        continue;
      }
      if ((pseg = buffer_open(ctx, ii, a[0], &bt, &b)) == NOT_ASSIGNED) {
        rc = GRN_NO_MEMORY_AVAILABLE;
        goto exit;
      }
      GRN_LOG(ctx, GRN_LOG_DEBUG, "flushed!  b=%p free=%d, seg(%d)", b, b->header.buffer_free, LSEG(a[0]));
      if (b->header.buffer_free < size) {
        GRN_LOG(ctx, GRN_LOG_CRIT, "buffer(%d) is full (%d < %d) in grn_ii_delete_one",
                a[0], b->header.buffer_free, size);
        rc = GRN_NO_MEMORY_AVAILABLE;
        buffer_close(ctx, ii, pseg);
        goto exit;
      }
    }

    b->header.buffer_free -= size;
    br = (buffer_rec *)(((byte *)&b->terms[b->header.nterms]) + b->header.buffer_free);
    rc = buffer_put(ctx, ii, b, bt, br, bs, u, size);
    buffer_close(ctx, ii, pseg);
    break;
  }
exit :
  array_unref(ii, tid);
  if (bs) { GRN_FREE(bs); }
  return rc;
}

#define CHUNK_USED    1
#define BUFFER_USED   2
#define SOLE_DOC_USED 4
#define SOLE_POS_USED 8

struct _grn_ii_cursor {
  grn_db_obj obj;
  grn_ctx *ctx;
  grn_ii *ii;
  grn_id id;
  grn_ii_posting *post;

  grn_id min;
  grn_id max;
  grn_ii_posting pc;
  grn_ii_posting pb;

  uint32_t cdf;
  uint32_t *cdp;
  uint32_t *crp;
  uint32_t *csp;
  uint32_t *ctp;
  uint32_t *cwp;
  uint32_t *cpp;

  uint8_t *bp;

  int nelements;
  uint32_t nchunks;
  uint32_t curr_chunk;
  chunk_info *cinfo;
  grn_io_win iw;
  uint8_t *cp;
  uint8_t *cpe;
  datavec rdv[MAX_N_ELEMENTS + 1];

  struct grn_ii_buffer *buf;
  uint16_t stat;
  uint16_t nextb;
  uint32_t buffer_pseg;
  int flags;
  uint32_t *ppseg;
};

static int
buffer_is_reused(grn_ctx *ctx, grn_ii *ii, grn_ii_cursor *c)
{
  if (*c->ppseg != c->buffer_pseg) {
    uint32_t i;
    for (i = ii->header->bgqtail; i != ii->header->bgqhead; i = (i + 1) & (GRN_II_BGQSIZE - 1)) {
      if (ii->header->bgqbody[i] == c->buffer_pseg) { return 0; }
    }
    return 1;
  }
  return 0;
}

static int
chunk_is_reused(grn_ctx *ctx, grn_ii *ii, grn_ii_cursor *c, uint32_t offset, uint32_t size)
{
  if (*c->ppseg != c->buffer_pseg) {
    uint32_t i, m, gseg;
    if (size > S_CHUNK) { return 1; }
    if (size > (1 << GRN_II_W_LEAST_CHUNK)) {
      int es = size - 1;
      GRN_BIT_SCAN_REV(es, m);
      m++;
    } else {
      m = GRN_II_W_LEAST_CHUNK;
    }
    gseg = ii->header->garbages[m - GRN_II_W_LEAST_CHUNK];
    while (gseg != NOT_ASSIGNED) {
      grn_io_win iw;
      grn_ii_ginfo *ginfo = WIN_MAP(ii->chunk, ctx, &iw, gseg, 0, S_GARBAGE, grn_io_rdwr);
      if (!ginfo) { break; }
      for (i = 0; i < ginfo->nrecs; i++) {
        if (ginfo->recs[i] == offset) {
          grn_io_win_unmap(&iw);
          return 0;
        }
      }
      gseg = ginfo->next;
      grn_io_win_unmap(&iw);
    }
    return 1;
  }
  return 0;
}

#define GRN_II_CURSOR_CMP(c1,c2) \
  (((c1)->post->rid > (c2)->post->rid) || \
   (((c1)->post->rid == (c2)->post->rid) && \
    (((c1)->post->sid > (c2)->post->sid) || \
     (((c1)->post->sid == (c2)->post->sid) && \
      ((c1)->post->pos > (c2)->post->pos)))))

grn_ii_cursor *
grn_ii_cursor_open(grn_ctx *ctx, grn_ii *ii, grn_id tid,
                   grn_id min, grn_id max, int nelements, int flags)
{
  grn_ii_cursor *c  = NULL;
  uint32_t pos, *a;
  if (!(a = array_at(ctx, ii, tid))) { return NULL; }
  for (;;) {
    if (!(pos = a[0])) { goto exit; }
    if (!(c = GRN_MALLOC(sizeof(grn_ii_cursor)))) { goto exit; }
    memset(c, 0, sizeof(grn_ii_cursor));
    c->ctx = ctx;
    c->ii = ii;
    c->id = tid;
    c->min = min;
    c->max = max;
    c->nelements = nelements;
    c->flags = flags;
    if (pos & 1) {
      c->stat = 0;
      if ((ii->header->flags & GRN_OBJ_WITH_SECTION)) {
        c->pb.rid = BIT31_12(pos);
        c->pb.sid = BIT11_01(pos);
      } else {
        c->pb.rid = pos >> 1;
        c->pb.sid = 1;
      }
      c->pb.tf = 1;
      c->pb.weight = 0;
      c->pb.pos = a[1];
    } else {
      uint32_t chunk;
      buffer_term *bt;
      if ((c->buffer_pseg = buffer_open(ctx, ii, pos, &bt, &c->buf)) == NOT_ASSIGNED) {
        GRN_FREE(c);
        c = NULL;
        goto exit;
      }
      c->ppseg = &ii->header->binfo[LSEG(pos)];
      if (bt->size_in_chunk && (chunk = c->buf->header.chunk) != NOT_ASSIGNED) {
        if (!(c->cp = WIN_MAP(ii->chunk, ctx, &c->iw, chunk, bt->pos_in_chunk,
                              bt->size_in_chunk, grn_io_rdonly))) {
          buffer_close(ctx, ii, c->buffer_pseg);
          GRN_FREE(c);
          c = NULL;
          goto exit;
        }
        if (buffer_is_reused(ctx, ii, c)) {
          grn_ii_cursor_close(ctx, c);
          continue;
        }
        c->cpe = c->cp + bt->size_in_chunk;
        if ((bt->tid & CHUNK_SPLIT)) {
          int i;
          grn_id crid;
          GRN_B_DEC(c->nchunks, c->cp);
          if (chunk_is_reused(ctx, ii, c, chunk, c->buf->header.chunk_size)) {
            grn_ii_cursor_close(ctx, c);
            continue;
          }
          if (!(c->cinfo = GRN_MALLOCN(chunk_info, c->nchunks))) {
            buffer_close(ctx, ii, c->buffer_pseg);
            grn_io_win_unmap(&c->iw);
            GRN_FREE(c);
            c = NULL;
            goto exit;
          }
          for (i = 0, crid = GRN_ID_NIL; i < c->nchunks; i++) {
            GRN_B_DEC(c->cinfo[i].segno, c->cp);
            GRN_B_DEC(c->cinfo[i].size, c->cp);
            GRN_B_DEC(c->cinfo[i].dgap, c->cp);
            crid += c->cinfo[i].dgap;
            if (crid < min) { c->curr_chunk = i + 1; }
          }
          if (chunk_is_reused(ctx, ii, c, chunk, c->buf->header.chunk_size)) {
            grn_ii_cursor_close(ctx, c);
            continue;
          }
        }
        if ((ii->header->flags & GRN_OBJ_WITH_POSITION)) {
          c->rdv[ii->n_elements - 1].flags = ODD;
        }
      }
      c->nextb = bt->pos_in_buffer;
      c->stat = CHUNK_USED|BUFFER_USED;
    }
    if (pos == a[0]) { break; }
    grn_ii_cursor_close(ctx, c);
  }
exit :
  array_unref(ii, tid);
  return c;
}

static inline void
grn_ii_cursor_set_min(grn_ctx *ctx, grn_ii_cursor *c, grn_id min)
{
  char grn_ii_cursor_set_min_enable_env[GRN_ENV_BUFFER_SIZE];

  if (c->min >= min) {
    return;
  }

  grn_getenv("GRN_II_CURSOR_SET_MIN_ENABLE",
             grn_ii_cursor_set_min_enable_env,
             GRN_ENV_BUFFER_SIZE);
  if (grn_ii_cursor_set_min_enable_env[0]) {
    c->min = min;
    if (c->buf && c->pc.rid < c->min && c->curr_chunk < c->nchunks) {
      uint32_t i, skip_chunk = 0;
      grn_id rid;
      for (i = 0, rid = GRN_ID_NIL; i < c->nchunks; i++) {
        rid += c->cinfo[i].dgap;
        if (rid < c->min) {
          skip_chunk = i + 1;
        } else {
          rid -= c->cinfo[i].dgap;
          break;
        }
      }
      if (skip_chunk > c->curr_chunk) {
        c->pc.rid = rid;
        c->curr_chunk = skip_chunk;
        c->crp = c->cdp + c->cdf;
      }
    }
  }
}

grn_ii_posting *
grn_ii_cursor_next(grn_ctx *ctx, grn_ii_cursor *c)
{
  if (c->buf) {
    for (;;) {
      if (c->stat & CHUNK_USED) {
        for (;;) {
          if (c->crp < c->cdp + c->cdf) {
            uint32_t dgap = *c->crp++;
            c->pc.rid += dgap;
            if (dgap) { c->pc.sid = 0; }
            if ((c->ii->header->flags & GRN_OBJ_WITH_SECTION)) {
              c->pc.sid += 1 + *c->csp++;
            } else {
              c->pc.sid = 1;
            }
            c->cpp += c->pc.rest;
            c->pc.rest = c->pc.tf = 1 + *c->ctp++;
            if ((c->ii->header->flags & GRN_OBJ_WITH_WEIGHT)) {
              c->pc.weight = *c->cwp++;
            } else {
              c->pc.weight = 0;
            }
            c->pc.pos = 0;
            /*
            {
              static int count = 0;
              int tf = c->pc.tf, pos = 0, *pp = (int *)c->cpp;
              grn_obj buf;
              GRN_TEXT_INIT(&buf, 0);
              grn_text_itoa(ctx, &buf, c->pc.rid);
              GRN_TEXT_PUTC(ctx, &buf, ':');
              grn_text_itoa(ctx, &buf, c->pc.sid);
              GRN_TEXT_PUTC(ctx, &buf, ':');
              grn_text_itoa(ctx, &buf, c->pc.tf);
              GRN_TEXT_PUTC(ctx, &buf, '(');
              while (tf--) {
                pos += *pp++;
                count++;
                grn_text_itoa(ctx, &buf, pos);
                if (tf) { GRN_TEXT_PUTC(ctx, &buf, ':'); }
              }
              GRN_TEXT_PUTC(ctx, &buf, ')');
              GRN_TEXT_PUTC(ctx, &buf, '\0');
              GRN_LOG(ctx, GRN_LOG_NOTICE, "posting(%d):%s", count, GRN_TEXT_VALUE(&buf));
              GRN_OBJ_FIN(ctx, &buf);
            }
            */
          } else {
            if (c->curr_chunk <= c->nchunks) {
              if (c->curr_chunk == c->nchunks) {
                if (c->cp < c->cpe) {
                  grn_p_decv(ctx, c->cp, c->cpe - c->cp, c->rdv, c->ii->n_elements);
                } else {
                  c->pc.rid = 0;
                  break;
                }
              } else {
                uint8_t *cp;
                grn_io_win iw;
                uint32_t size = c->cinfo[c->curr_chunk].size;
                if (size && (cp = WIN_MAP(c->ii->chunk, ctx, &iw,
                                          c->cinfo[c->curr_chunk].segno, 0,
                                          size, grn_io_rdonly))) {
                  grn_p_decv(ctx, cp, size, c->rdv, c->ii->n_elements);
                  grn_io_win_unmap(&iw);
                  if (chunk_is_reused(ctx, c->ii, c,
                                      c->cinfo[c->curr_chunk].segno, size)) {
                    GRN_LOG(ctx, GRN_LOG_WARNING,
                            "chunk(%d) is reused by another thread",
                            c->cinfo[c->curr_chunk].segno);
                    c->pc.rid = 0;
                    break;
                  }
                } else {
                  c->pc.rid = 0;
                  break;
                }
              }
              {
                int j = 0;
                c->cdf = c->rdv[j].data_size;
                c->crp = c->cdp = c->rdv[j++].data;
                if ((c->ii->header->flags & GRN_OBJ_WITH_SECTION)) {
                  c->csp = c->rdv[j++].data;
                }
                c->ctp = c->rdv[j++].data;
                if ((c->ii->header->flags & GRN_OBJ_WITH_WEIGHT)) {
                  c->cwp = c->rdv[j++].data;
                }
                c->cpp = c->rdv[j].data;
              }
              c->pc.rid = 0;
              c->pc.sid = 0;
              c->pc.rest = 0;
              c->curr_chunk++;
              continue;
            } else {
              c->pc.rid = 0;
            }
          }
          break;
        }
      }
      if (c->stat & BUFFER_USED) {
        for (;;) {
          if (c->nextb) {
            uint32_t lrid = c->pb.rid, lsid = c->pb.sid; /* for check */
            buffer_rec *br = BUFFER_REC_AT(c->buf, c->nextb);
            if (buffer_is_reused(ctx, c->ii, c)) {
              GRN_LOG(ctx, GRN_LOG_NOTICE, "buffer reused(%d,%d)", c->buffer_pseg, *c->ppseg);
              // todo : rewind;
            }
            c->bp = NEXT_ADDR(br);
            GRN_B_DEC(c->pb.rid, c->bp);
            if ((c->ii->header->flags & GRN_OBJ_WITH_SECTION)) {
              GRN_B_DEC(c->pb.sid, c->bp);
            } else {
              c->pb.sid = 1;
            }
            if (lrid > c->pb.rid || (lrid == c->pb.rid && lsid >= c->pb.sid)) {
              ERR(GRN_FILE_CORRUPT, "brokend!! (%d:%d) -> (%d:%d) (%d->%d)", lrid, lsid, c->pb.rid, c->pb.sid, c->buffer_pseg, *c->ppseg);
            }
            if (c->pb.rid < c->min) {
              c->pb.rid = 0;
              if (br->jump > 0) {
                buffer_rec *jump_br = BUFFER_REC_AT(c->buf, br->jump);
                uint8_t *jump_bp;
                uint32_t jump_rid;
                jump_bp = NEXT_ADDR(jump_br);
                GRN_B_DEC(jump_rid, jump_bp);
                if (jump_rid < c->min) {
                  c->nextb = br->jump;
                } else {
                  c->nextb = br->step;
                }
              } else {
                c->nextb = br->step;
              }
              continue;
            }
            c->nextb = br->step;
            GRN_B_DEC(c->pb.tf, c->bp);
            if ((c->ii->header->flags & GRN_OBJ_WITH_WEIGHT)) {
              GRN_B_DEC(c->pb.weight, c->bp);
            } else {
              c->pb.weight = 0;
            }
            c->pb.rest = c->pb.tf;
            c->pb.pos = 0;
          } else {
            c->pb.rid = 0;
          }
          break;
        }
      }
      if (c->pb.rid) {
        if (c->pc.rid) {
          if (c->pc.rid < c->pb.rid) {
            c->stat = CHUNK_USED;
            if (c->pc.tf && c->pc.sid) { c->post = &c->pc; break; }
          } else {
            if (c->pb.rid < c->pc.rid) {
              c->stat = BUFFER_USED;
              if (c->pb.tf && c->pb.sid) { c->post = &c->pb; break; }
            } else {
              if (c->pb.sid) {
                if (c->pc.sid < c->pb.sid) {
                  c->stat = CHUNK_USED;
                  if (c->pc.tf && c->pc.sid) { c->post = &c->pc; break; }
                } else {
                  c->stat = BUFFER_USED;
                  if (c->pb.sid == c->pc.sid) { c->stat |= CHUNK_USED; }
                  if (c->pb.tf) { c->post = &c->pb; break; }
                }
              } else {
                c->stat = CHUNK_USED;
              }
            }
          }
        } else {
          c->stat = BUFFER_USED;
          if (c->pb.tf && c->pb.sid) { c->post = &c->pb; break; }
        }
      } else {
        if (c->pc.rid) {
          c->stat = CHUNK_USED;
          if (c->pc.tf && c->pc.sid) { c->post = &c->pc; break; }
        } else {
          c->post = NULL;
          return NULL;
        }
      }
    }
  } else {
    if (c->stat & SOLE_DOC_USED) {
      c->post = NULL;
      return NULL;
    } else {
      c->post = &c->pb;
      c->stat |= SOLE_DOC_USED;
      if (c->post->rid < c->min) {
        c->post = NULL;
        return NULL;
      }
    }
  }
  return c->post;
}

grn_ii_posting *
grn_ii_cursor_next_pos(grn_ctx *ctx, grn_ii_cursor *c)
{
  uint32_t gap;
  if ((c->ii->header->flags & GRN_OBJ_WITH_POSITION)) {
    if (c->nelements == c->ii->n_elements) {
      if (c->buf) {
        if (c->post == &c->pc) {
          if (c->pc.rest) {
            c->pc.rest--;
            c->pc.pos += *c->cpp++;
          } else {
            return NULL;
          }
        } else if (c->post == &c->pb) {
          if (buffer_is_reused(ctx, c->ii, c)) {
            GRN_LOG(ctx, GRN_LOG_NOTICE, "buffer reused(%d,%d)", c->buffer_pseg, *c->ppseg);
            // todo : rewind;
          }
          if (c->pb.rest) {
            c->pb.rest--;
            GRN_B_DEC(gap, c->bp);
            c->pb.pos += gap;
          } else {
            return NULL;
          }
        } else {
          return NULL;
        }
      } else {
        if (c->stat & SOLE_POS_USED) {
          return NULL;
        } else {
          c->stat |= SOLE_POS_USED;
        }
      }
    }
  } else {
    if (c->stat & SOLE_POS_USED) {
      return NULL;
    } else {
      c->stat |= SOLE_POS_USED;
    }
  }
  return c->post;
}

grn_rc
grn_ii_cursor_close(grn_ctx *ctx, grn_ii_cursor *c)
{
  if (!c) { return GRN_INVALID_ARGUMENT; }
  datavec_fin(ctx, c->rdv);
  if (c->cinfo) { GRN_FREE(c->cinfo); }
  if (c->buf) { buffer_close(ctx, c->ii, c->buffer_pseg); }
  if (c->cp) { grn_io_win_unmap(&c->iw); }
  GRN_FREE(c);
  return GRN_SUCCESS;
}

uint32_t
grn_ii_get_chunksize(grn_ctx *ctx, grn_ii *ii, grn_id tid)
{
  uint32_t res, pos, *a;
  a = array_at(ctx, ii, tid);
  if (!a) { return 0; }
  if ((pos = a[0])) {
    if (pos & 1) {
      res = 0;
    } else {
      buffer *buf;
      uint32_t pseg;
      buffer_term *bt;
      if ((pseg = buffer_open(ctx, ii, pos, &bt, &buf)) == NOT_ASSIGNED) {
        res = 0;
      } else {
        res = bt->size_in_chunk;
        buffer_close(ctx, ii, pseg);
      }
    }
  } else {
    res = 0;
  }
  array_unref(ii, tid);
  return res;
}

uint32_t
grn_ii_estimate_size(grn_ctx *ctx, grn_ii *ii, grn_id tid)
{
  uint32_t res, pos, *a;
  a = array_at(ctx, ii, tid);
  if (!a) { return 0; }
  if ((pos = a[0])) {
    if (pos & 1) {
      res = 1;
    } else {
      buffer *buf;
      uint32_t pseg;
      buffer_term *bt;
      if ((pseg = buffer_open(ctx, ii, pos, &bt, &buf)) == NOT_ASSIGNED) {
        res = 0;
      } else {
        res = a[1] + bt->size_in_buffer + 2;
        buffer_close(ctx, ii, pseg);
      }
    }
  } else {
    res = 0;
  }
  array_unref(ii, tid);
  return res;
}

int
grn_ii_entry_info(grn_ctx *ctx, grn_ii *ii, grn_id tid, unsigned int *a,
                   unsigned int *chunk, unsigned int *chunk_size, unsigned int *buffer_free,
                   unsigned int *nterms, unsigned int *nterms_void, unsigned int *bt_tid,
                   unsigned int *size_in_chunk, unsigned int *pos_in_chunk,
                   unsigned int *size_in_buffer, unsigned int *pos_in_buffer)
{
  buffer *b;
  buffer_term *bt;
  uint32_t pseg, *ap;
  ERRCLR(NULL);
  ap = array_at(ctx, ii, tid);
  if (!ap) { return 0; }
  a[0] = *ap;
  array_unref(ii, tid);
  if (!a[0]) { return 1; }
  if (a[0] & 1) { return 2; }
  if ((pseg = buffer_open(ctx, ii, a[0], &bt, &b)) == NOT_ASSIGNED) { return 3; }
  *chunk = b->header.chunk;
  *chunk_size = b->header.chunk_size;
  *buffer_free = b->header.buffer_free;
  *nterms = b->header.nterms;
  *bt_tid = bt->tid;
  *size_in_chunk = bt->size_in_chunk;
  *pos_in_chunk = bt->pos_in_chunk;
  *size_in_buffer = bt->size_in_buffer;
  *pos_in_buffer = bt->pos_in_buffer;
  buffer_close(ctx, ii, pseg);
  return 4;
}

const char *
grn_ii_path(grn_ii *ii)
{
  return grn_io_path(ii->seg);
}

uint32_t
grn_ii_max_section(grn_ii *ii)
{
  return ii->header->smax;
}

grn_obj *
grn_ii_lexicon(grn_ii *ii)
{
  return ii->lexicon;
}

/* private classes */

/* b-heap */

typedef struct {
  int n_entries;
  int n_bins;
  grn_ii_cursor **bins;
} cursor_heap;

static inline cursor_heap *
cursor_heap_open(grn_ctx *ctx, int max)
{
  cursor_heap *h = GRN_MALLOC(sizeof(cursor_heap));
  if (!h) { return NULL; }
  h->bins = GRN_MALLOC(sizeof(grn_ii_cursor *) * max);
  if (!h->bins) {
    GRN_FREE(h);
    return NULL;
  }
  h->n_entries = 0;
  h->n_bins = max;
  return h;
}

static inline grn_rc
cursor_heap_push(grn_ctx *ctx, cursor_heap *h, grn_ii *ii, grn_id tid, uint32_t offset2)
{
  int n, n2;
  grn_ii_cursor *c, *c2;
  if (h->n_entries >= h->n_bins) {
    int max = h->n_bins * 2;
    grn_ii_cursor **bins = GRN_REALLOC(h->bins, sizeof(grn_ii_cursor *) * max);
    GRN_LOG(ctx, GRN_LOG_DEBUG, "expanded cursor_heap to %d,%p", max, bins);
    if (!bins) { return GRN_NO_MEMORY_AVAILABLE; }
    h->n_bins = max;
    h->bins = bins;
  }
  {
    if (!(c = grn_ii_cursor_open(ctx, ii, tid, GRN_ID_NIL, GRN_ID_MAX,
                                 ii->n_elements, 0))) {
      GRN_LOG(ctx, GRN_LOG_ERROR, "cursor open failed");
      return ctx->rc;
    }
    if (!grn_ii_cursor_next(ctx, c)) {
      grn_ii_cursor_close(ctx, c);
      return GRN_END_OF_DATA;
    }
    if (!grn_ii_cursor_next_pos(ctx, c)) {
      GRN_LOG(ctx, GRN_LOG_ERROR, "invalid ii_cursor b");
      grn_ii_cursor_close(ctx, c);
      return GRN_END_OF_DATA;
    }
    n = h->n_entries++;
    while (n) {
      n2 = (n - 1) >> 1;
      c2 = h->bins[n2];
      if (GRN_II_CURSOR_CMP(c, c2)) { break; }
      h->bins[n] = c2;
      n = n2;
    }
    h->bins[n] = c;
  }
  return GRN_SUCCESS;
}

static inline grn_rc
cursor_heap_push2(cursor_heap *h)
{
  grn_rc rc = GRN_SUCCESS;
  return rc;
}

static inline grn_ii_cursor *
cursor_heap_min(cursor_heap *h)
{
  return h->n_entries ? h->bins[0] : NULL;
}

static inline void
cursor_heap_recalc_min(cursor_heap *h)
{
  int n = 0, n1, n2, m;
  if ((m = h->n_entries) > 1) {
    grn_ii_cursor *c = h->bins[0], *c1, *c2;
    for (;;) {
      n1 = n * 2 + 1;
      n2 = n1 + 1;
      c1 = n1 < m ? h->bins[n1] : NULL;
      c2 = n2 < m ? h->bins[n2] : NULL;
      if (c1 && GRN_II_CURSOR_CMP(c, c1)) {
        if (c2 && GRN_II_CURSOR_CMP(c, c2) && GRN_II_CURSOR_CMP(c1, c2)) {
          h->bins[n] = c2;
          n = n2;
        } else {
          h->bins[n] = c1;
          n = n1;
        }
      } else {
        if (c2 && GRN_II_CURSOR_CMP(c, c2)) {
          h->bins[n] = c2;
          n = n2;
        } else {
          h->bins[n] = c;
          break;
        }
      }
    }
  }
}

static inline void
cursor_heap_pop(grn_ctx *ctx, cursor_heap *h, grn_id min)
{
  if (h->n_entries) {
    grn_ii_cursor *c = h->bins[0];
    grn_ii_cursor_set_min(ctx, c, min);
    if (!grn_ii_cursor_next(ctx, c)) {
      grn_ii_cursor_close(ctx, c);
      h->bins[0] = h->bins[--h->n_entries];
    } else if (!grn_ii_cursor_next_pos(ctx, c)) {
      GRN_LOG(ctx, GRN_LOG_ERROR, "invalid ii_cursor c");
      grn_ii_cursor_close(ctx, c);
      h->bins[0] = h->bins[--h->n_entries];
    }
    if (h->n_entries > 1) { cursor_heap_recalc_min(h); }
  }
}

static inline void
cursor_heap_pop_pos(grn_ctx *ctx, cursor_heap *h)
{
  if (h->n_entries) {
    grn_ii_cursor *c = h->bins[0];
    if (!grn_ii_cursor_next_pos(ctx, c)) {
      if (!grn_ii_cursor_next(ctx, c)) {
        grn_ii_cursor_close(ctx, c);
        h->bins[0] = h->bins[--h->n_entries];
      } else if (!grn_ii_cursor_next_pos(ctx, c)) {
        GRN_LOG(ctx, GRN_LOG_ERROR, "invalid ii_cursor d");
        grn_ii_cursor_close(ctx, c);
        h->bins[0] = h->bins[--h->n_entries];
      }
    }
    if (h->n_entries > 1) { cursor_heap_recalc_min(h); }
  }
}

static inline void
cursor_heap_close(grn_ctx *ctx, cursor_heap *h)
{
  int i;
  if (!h) { return; }
  for (i = h->n_entries; i--;) { grn_ii_cursor_close(ctx, h->bins[i]); }
  GRN_FREE(h->bins);
  GRN_FREE(h);
}

/* update */
#ifdef USE_VGRAM

inline static grn_rc
index_add(grn_ctx *ctx, grn_id rid, grn_obj *lexicon, grn_ii *ii, grn_vgram *vgram,
          const char *value, size_t value_len)
{
  grn_hash *h;
  unsigned int token_flags = 0;
  grn_token_cursor *token_cursor;
  grn_ii_updspec **u;
  grn_id tid, *tp;
  grn_rc r, rc = GRN_SUCCESS;
  grn_vgram_buf *sbuf = NULL;
  if (!rid) { return GRN_INVALID_ARGUMENT; }
  if (!(token_cursor = grn_token_cursor_open(ctx, lexicon, value, value_len,
                                             GRN_TOKEN_ADD, token_flags))) {
    return GRN_NO_MEMORY_AVAILABLE;
  }
  if (vgram) { sbuf = grn_vgram_buf_open(value_len); }
  h = grn_hash_create(ctx, NULL, sizeof(grn_id), sizeof(grn_ii_updspec *), GRN_HASH_TINY);
  if (!h) {
    GRN_LOG(ctx, GRN_LOG_ALERT, "grn_hash_create on index_add failed !");
    grn_token_cursor_close(ctx, token_cursor);
    if (sbuf) { grn_vgram_buf_close(sbuf); }
    return GRN_NO_MEMORY_AVAILABLE;
  }
  while (!token_cursor->status) {
    (tid = grn_token_cursor_next(ctx, token_cursor));
    if (tid) {
      if (!grn_hash_add(ctx, h, &tid, sizeof(grn_id), (void **) &u, NULL)) { break; }
      if (!*u) {
        if (!(*u = grn_ii_updspec_open(ctx, rid, 1))) {
          GRN_LOG(ctx, GRN_LOG_ERROR, "grn_ii_updspec_open on index_add failed!");
          goto exit;
        }
      }
      if (grn_ii_updspec_add(ctx, *u, token_cursor->pos, 0)) {
        GRN_LOG(ctx, GRN_LOG_ERROR, "grn_ii_updspec_add on index_add failed!");
        goto exit;
      }
      if (sbuf) { grn_vgram_buf_add(sbuf, tid); }
    }
  }
  grn_token_cursor_close(ctx, token_cursor);
  // todo : support vgram
  //  if (sbuf) { grn_vgram_update(vgram, rid, sbuf, (grn_set *)h); }
  GRN_HASH_EACH(ctx, h, id, &tp, NULL, &u, {
    if ((r = grn_ii_update_one(ctx, ii, *tp, *u, h))) { rc = r; }
    grn_ii_updspec_close(ctx, *u);
  });
  grn_hash_close(ctx, h);
  if (sbuf) { grn_vgram_buf_close(sbuf); }
  return rc;
exit:
  grn_hash_close(ctx, h);
  grn_token_cursor_close(ctx, token_cursor);
  if (sbuf) { grn_vgram_buf_close(sbuf); }
  return GRN_NO_MEMORY_AVAILABLE;
}

inline static grn_rc
index_del(grn_ctx *ctx, grn_id rid, grn_obj *lexicon, grn_ii *ii, grn_vgram *vgram,
          const char *value, size_t value_len)
{
  grn_rc rc = GRN_SUCCESS;
  grn_hash *h;
  unsigned int token_flags = 0;
  grn_token_cursor *token_cursor;
  grn_ii_updspec **u;
  grn_id tid, *tp;
  if (!rid) { return GRN_INVALID_ARGUMENT; }
  if (!(token_cursor = grn_token_cursor_open(ctx, lexicon, value, value_len,
                                             GRN_TOKEN_DEL, token_flags))) {
    return GRN_NO_MEMORY_AVAILABLE;
  }
  h = grn_hash_create(ctx, NULL, sizeof(grn_id), sizeof(grn_ii_updspec *), GRN_HASH_TINY);
  if (!h) {
    GRN_LOG(ctx, GRN_LOG_ALERT, "grn_hash_create on index_del failed !");
    grn_token_cursor_close(ctx, token_cursor);
    return GRN_NO_MEMORY_AVAILABLE;
  }
  while (!token_cursor->status) {
    if ((tid = grn_token_cursor_next(ctx, token_cursor))) {
      if (!grn_hash_add(ctx, h, &tid, sizeof(grn_id), (void **) &u, NULL)) { break; }
      if (!*u) {
        if (!(*u = grn_ii_updspec_open(ctx, rid, 0))) {
          GRN_LOG(ctx, GRN_LOG_ALERT, "grn_ii_updspec_open on index_del failed !");
          grn_hash_close(ctx, h);
          grn_token_cursor_close(ctx, token_cursor);
          return GRN_NO_MEMORY_AVAILABLE;
        }
      }
    }
  }
  grn_token_cursor_close(ctx, token_cursor);
  GRN_HASH_EACH(ctx, h, id, &tp, NULL, &u, {
    if (*tp) {
      grn_rc r;
      r = grn_ii_delete_one(ctx, ii, *tp, *u, NULL);
      if (r) {
        rc = r;
      }
    }
    grn_ii_updspec_close(ctx, *u);
  });
  grn_hash_close(ctx, h);
  return rc;
}

grn_rc
grn_ii_upd(grn_ctx *ctx, grn_ii *ii, grn_id rid, grn_vgram *vgram,
            const char *oldvalue, unsigned int oldvalue_len,
            const char *newvalue, unsigned int newvalue_len)
{
  grn_rc rc;
  grn_obj *lexicon = ii->lexicon;
  if (!rid) { return GRN_INVALID_ARGUMENT; }
  if (oldvalue && *oldvalue) {
    if ((rc = index_del(ctx, rid, lexicon, ii, vgram, oldvalue, oldvalue_len))) {
      GRN_LOG(ctx, GRN_LOG_ERROR, "index_del on grn_ii_upd failed !");
      goto exit;
    }
  }
  if (newvalue && *newvalue) {
    rc = index_add(ctx, rid, lexicon, ii, vgram, newvalue, newvalue_len);
  }
exit :
  return rc;
}

grn_rc
grn_ii_update(grn_ctx *ctx, grn_ii *ii, grn_id rid, grn_vgram *vgram, unsigned int section,
              grn_values *oldvalues, grn_values *newvalues)
{
  int j;
  grn_value *v;
  unsigned int token_flags = 0;
  grn_token_cursor *token_cursor;
  grn_rc rc = GRN_SUCCESS;
  grn_hash *old, *new;
  grn_id tid, *tp;
  grn_ii_updspec **u, **un;
  grn_obj *lexicon = ii->lexicon;
  if (!lexicon || !ii || !rid) {
    GRN_LOG(ctx, GRN_LOG_WARNING, "grn_ii_update: invalid argument");
    return GRN_INVALID_ARGUMENT;
  }
  if (newvalues) {
    new = grn_hash_create(ctx, NULL, sizeof(grn_id), sizeof(grn_ii_updspec *), GRN_HASH_TINY);
    if (!new) {
      GRN_LOG(ctx, GRN_LOG_ALERT, "grn_hash_create on grn_ii_update failed !");
      rc = GRN_NO_MEMORY_AVAILABLE;
      goto exit;
    }
    for (j = newvalues->n_values, v = newvalues->values; j; j--, v++) {
      if ((token_cursor = grn_token_cursor_open(ctx, lexicon, v->str, v->str_len,
                                                GRN_TOKEN_ADD, token_flags))) {
        while (!token_cursor->status) {
          if ((tid = grn_token_cursor_next(ctx, token_cursor))) {
            if (!grn_hash_add(ctx, new, &tid, sizeof(grn_id), (void **) &u, NULL)) {
              break;
            }
            if (!*u) {
              if (!(*u = grn_ii_updspec_open(ctx, rid, section))) {
                GRN_LOG(ctx, GRN_LOG_ALERT, "grn_ii_updspec_open on grn_ii_update failed!");
                grn_token_cursor_close(ctx, token_cursor);
                grn_hash_close(ctx, new);
                rc = GRN_NO_MEMORY_AVAILABLE;
                goto exit;
              }
            }
            if (grn_ii_updspec_add(ctx, *u, token_cursor->pos, v->weight)) {
              GRN_LOG(ctx, GRN_LOG_ALERT, "grn_ii_updspec_add on grn_ii_update failed!");
              grn_token_cursor_close(ctx, token_cursor);
              grn_hash_close(ctx, new);
              rc = GRN_NO_MEMORY_AVAILABLE;
              goto exit;
            }
          }
        }
        grn_token_cursor_close(ctx, token_cursor);
      }
    }
    if (!GRN_HASH_SIZE(new)) {
      grn_hash_close(ctx, new);
      new = NULL;
    }
  } else {
    new = NULL;
  }
  if (oldvalues) {
    old = grn_hash_create(ctx, NULL, sizeof(grn_id), sizeof(grn_ii_updspec *), GRN_HASH_TINY);
    if (!old) {
      GRN_LOG(ctx, GRN_LOG_ALERT, "grn_hash_create(ctx, NULL, old) on grn_ii_update failed!");
      if (new) { grn_hash_close(ctx, new); }
      rc = GRN_NO_MEMORY_AVAILABLE;
      goto exit;
    }
    for (j = oldvalues->n_values, v = oldvalues->values; j; j--, v++) {
      if ((token_cursor = grn_token_cursor_open(ctx, lexicon, v->str, v->str_len,
                                                GRN_TOKEN_DEL, token_flags))) {
        while (!token_cursor->status) {
          if ((tid = grn_token_cursor_next(ctx, token_cursor))) {
            if (!grn_hash_add(ctx, old, &tid, sizeof(grn_id), (void **) &u, NULL)) {
              break;
            }
            if (!*u) {
              if (!(*u = grn_ii_updspec_open(ctx, rid, section))) {
                GRN_LOG(ctx, GRN_LOG_ALERT, "grn_ii_updspec_open on grn_ii_update failed!");
                grn_token_cursor_close(ctx, token_cursor);
                if (new) { grn_hash_close(ctx, new); };
                grn_hash_close(ctx, old);
                rc = GRN_NO_MEMORY_AVAILABLE;
                goto exit;
              }
            }
            if (grn_ii_updspec_add(ctx, *u, token_cursor->pos, v->weight)) {
              GRN_LOG(ctx, GRN_LOG_ALERT, "grn_ii_updspec_add on grn_ii_update failed!");
              grn_token_cursor_close(ctx, token_cursor);
              if (new) { grn_hash_close(ctx, new); };
              grn_hash_close(ctx, old);
              rc = GRN_NO_MEMORY_AVAILABLE;
              goto exit;
            }
          }
        }
        grn_token_cursor_close(ctx, token_cursor);
      }
    }
  } else {
    old = NULL;
  }
  if (old) {
    grn_id eid;
    GRN_HASH_EACH(ctx, old, id, &tp, NULL, &u, {
      if (new && (eid = grn_hash_get(ctx, new, tp, sizeof(grn_id), (void **) &un))) {
        if (!grn_ii_updspec_cmp(*u, *un)) {
          grn_ii_updspec_close(ctx, *un);
          grn_hash_delete_by_id(ctx, new, eid, NULL);
        }
      } else {
        grn_rc r;
        r = grn_ii_delete_one(ctx, ii, *tp, *u, new);
        if (r) {
          rc = r;
        }
      }
      grn_ii_updspec_close(ctx, *u);
    });
    grn_hash_close(ctx, old);
  }
  if (new) {
    GRN_HASH_EACH(ctx, new, id, &tp, NULL, &u, {
      grn_rc r;
      if ((r = grn_ii_update_one(ctx, ii, *tp, *u, new))) { rc = r; }
      grn_ii_updspec_close(ctx, *u);
    });
    grn_hash_close(ctx, new);
  } else {
    if (!section) {
      /* todo: delete key when all sections deleted */
    }
  }
exit :
  return rc;
}
#endif /* USE_VGRAM */

static grn_rc
grn_vector2updspecs(grn_ctx *ctx, grn_ii *ii, grn_id rid, unsigned int section,
                    grn_obj *in, grn_obj *out, grn_tokenize_mode mode,
                    grn_obj *posting)
{
  int j;
  grn_id tid;
  grn_section *v;
  grn_token_cursor *token_cursor;
  grn_ii_updspec **u;
  grn_hash *h = (grn_hash *)out;
  grn_obj *lexicon = ii->lexicon;
  if (in->u.v.body) {
    const char *head = GRN_BULK_HEAD(in->u.v.body);
    for (j = in->u.v.n_sections, v = in->u.v.sections; j; j--, v++) {
      unsigned int token_flags = 0;
      if (v->length &&
          (token_cursor = grn_token_cursor_open(ctx, lexicon, head + v->offset, v->length,
                                                mode, token_flags))) {
        while (!token_cursor->status) {
          if ((tid = grn_token_cursor_next(ctx, token_cursor))) {
            if (posting) { GRN_RECORD_PUT(ctx, posting, tid); }
            if (!grn_hash_add(ctx, h, &tid, sizeof(grn_id), (void **) &u, NULL)) {
              break;
            }
            if (!*u) {
              if (!(*u = grn_ii_updspec_open(ctx, rid, section))) {
                GRN_LOG(ctx, GRN_LOG_ALERT, "grn_ii_updspec_open on grn_ii_update failed!");
                grn_token_cursor_close(ctx, token_cursor);
                return GRN_NO_MEMORY_AVAILABLE;
              }
            }
            if (grn_ii_updspec_add(ctx, *u, token_cursor->pos, v->weight)) {
              GRN_LOG(ctx, GRN_LOG_ALERT, "grn_ii_updspec_add on grn_ii_update failed!");
              grn_token_cursor_close(ctx, token_cursor);
              return GRN_NO_MEMORY_AVAILABLE;
            }
          }
        }
        grn_token_cursor_close(ctx, token_cursor);
      }
    }
  }
  return GRN_SUCCESS;
}

static grn_rc
grn_uvector2updspecs_data(grn_ctx *ctx, grn_ii *ii, grn_id rid,
                          unsigned int section, grn_obj *in, grn_obj *out,
                          grn_tokenize_mode mode, grn_obj *posting)
{
  int i, n;
  grn_hash *h = (grn_hash *)out;
  grn_obj *lexicon = ii->lexicon;
  unsigned int element_size;

  n = grn_uvector_size(ctx, in);
  element_size = grn_uvector_element_size(ctx, in);
  for (i = 0; i < n; i++) {
    grn_token_cursor *token_cursor;
    unsigned int token_flags = 0;
    const char *element;

    element = GRN_BULK_HEAD(in) + (element_size * i);
    token_cursor = grn_token_cursor_open(ctx, lexicon,
                                         element, element_size,
                                         mode, token_flags);
    if (!token_cursor) {
      continue;
    }

    while (!token_cursor->status) {
      grn_id tid;
      if ((tid = grn_token_cursor_next(ctx, token_cursor))) {
        grn_ii_updspec **u;

        if (posting) { GRN_RECORD_PUT(ctx, posting, tid); }
        if (!grn_hash_add(ctx, h, &tid, sizeof(grn_id), (void **)&u, NULL)) {
          break;
        }
        if (!*u) {
          if (!(*u = grn_ii_updspec_open(ctx, rid, section))) {
            GRN_LOG(ctx, GRN_LOG_ALERT,
                    "grn_ii_updspec_open on grn_uvector2updspecs_data failed!");
            grn_token_cursor_close(ctx, token_cursor);
            return GRN_NO_MEMORY_AVAILABLE;
          }
        }
        if (grn_ii_updspec_add(ctx, *u, token_cursor->pos, 0)) {
          GRN_LOG(ctx, GRN_LOG_ALERT,
                  "grn_ii_updspec_add on grn_uvector2updspecs failed!");
          grn_token_cursor_close(ctx, token_cursor);
          return GRN_NO_MEMORY_AVAILABLE;
        }
      }
    }

    grn_token_cursor_close(ctx, token_cursor);
  }

  return GRN_SUCCESS;
}

static grn_rc
grn_uvector2updspecs_id(grn_ctx *ctx, grn_ii *ii, grn_id rid,
                        unsigned int section, grn_obj *in, grn_obj *out)
{
  int i, n;
  grn_ii_updspec **u;
  grn_hash *h = (grn_hash *)out;

  n = grn_vector_size(ctx, in);
  for (i = 0; i < n; i++) {
    grn_id id;
    unsigned int weight;

    id = grn_uvector_get_element(ctx, in, i, &weight);
    if (!grn_hash_add(ctx, h, &id, sizeof(grn_id), (void **)&u, NULL)) {
      break;
    }
    if (!*u) {
      if (!(*u = grn_ii_updspec_open(ctx, rid, section))) {
        GRN_LOG(ctx, GRN_LOG_ALERT, "grn_ii_updspec_open on grn_ii_update failed!");
        return GRN_NO_MEMORY_AVAILABLE;
      }
    }
    if (grn_ii_updspec_add(ctx, *u, i, weight)) {
      GRN_LOG(ctx, GRN_LOG_ALERT, "grn_ii_updspec_add on grn_ii_update failed!");
      return GRN_NO_MEMORY_AVAILABLE;
    }
  }
  return GRN_SUCCESS;
}

static grn_rc
grn_uvector2updspecs(grn_ctx *ctx, grn_ii *ii, grn_id rid,
                     unsigned int section, grn_obj *in, grn_obj *out,
                     grn_tokenize_mode mode, grn_obj *posting)
{
  if (in->header.domain < GRN_N_RESERVED_TYPES) {
    return grn_uvector2updspecs_data(ctx, ii, rid, section, in, out,
                                     mode, posting);
  } else {
    return grn_uvector2updspecs_id(ctx, ii, rid, section, in, out);
  }
}

grn_rc
grn_ii_column_update(grn_ctx *ctx, grn_ii *ii, grn_id rid, unsigned int section,
                     grn_obj *oldvalue, grn_obj *newvalue, grn_obj *posting)
{
  grn_id *tp;
  grn_bool do_grn_ii_updspec_cmp = GRN_TRUE;
  grn_rc rc = GRN_SUCCESS;
  grn_ii_updspec **u, **un;
  grn_obj *old_, *old = oldvalue, *new_, *new = newvalue, oldv, newv, buf, *post = NULL;
  if (!ii || !ii->lexicon || !rid) {
    ERR(GRN_INVALID_ARGUMENT, "grn_ii_column_update: invalid argument");
    return GRN_INVALID_ARGUMENT;
  }
  if (posting) {
    GRN_RECORD_INIT(&buf, GRN_OBJ_VECTOR, grn_obj_id(ctx, ii->lexicon));
    post = &buf;
  }
  if (grn_io_lock(ctx, ii->seg, grn_lock_timeout)) { return ctx->rc; }
  if (new) {
    unsigned char type = (ii->obj.header.domain == new->header.domain)
      ? GRN_UVECTOR
      : new->header.type;
    switch (type) {
    case GRN_BULK :
      {
        if (grn_bulk_is_zero(ctx, new)) {
          do_grn_ii_updspec_cmp = GRN_FALSE;
        }
        new_ = new;
        GRN_OBJ_INIT(&newv, GRN_VECTOR, GRN_OBJ_DO_SHALLOW_COPY, GRN_DB_TEXT);
        newv.u.v.body = new;
        new = &newv;
        grn_vector_delimit(ctx, new, 0, GRN_ID_NIL);
        if (new_ != newvalue) { grn_obj_close(ctx, new_); }
      }
      /* fallthru */
    case GRN_VECTOR :
      new_ = new;
      new = (grn_obj *)grn_hash_create(ctx, NULL, sizeof(grn_id),
                                       sizeof(grn_ii_updspec *),
                                       GRN_HASH_TINY);
      if (!new) {
        GRN_LOG(ctx, GRN_LOG_ALERT, "grn_hash_create on grn_ii_update failed !");
        rc = GRN_NO_MEMORY_AVAILABLE;
      } else {
        rc = grn_vector2updspecs(ctx, ii, rid, section, new_, new, GRN_TOKEN_ADD, post);
      }
      if (new_ != newvalue) { grn_obj_close(ctx, new_); }
      if (rc) { goto exit; }
      break;
    case GRN_UVECTOR :
      new_ = new;
      new = (grn_obj *)grn_hash_create(ctx, NULL, sizeof(grn_id),
                                       sizeof(grn_ii_updspec *),
                                       GRN_HASH_TINY);
      if (!new) {
        GRN_LOG(ctx, GRN_LOG_ALERT, "grn_hash_create on grn_ii_update failed !");
        rc = GRN_NO_MEMORY_AVAILABLE;
      } else {
        if (new_->header.type == GRN_UVECTOR) {
          rc = grn_uvector2updspecs(ctx, ii, rid, section, new_, new,
                                    GRN_TOKEN_ADD, post);
        } else {
          grn_obj uvector;
          unsigned int weight = 0;
          GRN_VALUE_FIX_SIZE_INIT(&uvector, GRN_OBJ_VECTOR, new_->header.domain);
          if (new_->header.impl_flags & GRN_OBJ_WITH_WEIGHT) {
            uvector.header.impl_flags |= GRN_OBJ_WITH_WEIGHT;
          }
          grn_uvector_add_element(ctx, &uvector, GRN_RECORD_VALUE(new_), weight);
          rc = grn_uvector2updspecs(ctx, ii, rid, section, &uvector, new,
                                    GRN_TOKEN_ADD, post);
          GRN_OBJ_FIN(ctx, &uvector);
        }
      }
      if (new_ != newvalue) { grn_obj_close(ctx, new_); }
      if (rc) { goto exit; }
      break;
    case GRN_TABLE_HASH_KEY :
      break;
    default :
      ERR(GRN_INVALID_ARGUMENT, "invalid object assigned as newvalue");
      goto exit;
    }
  }
  if (posting) {
    grn_ii_updspec *u_;
    uint32_t offset = 0;
    grn_id tid_ = 0, gap, tid, *tpe;
    grn_table_sort_optarg arg = {GRN_TABLE_SORT_ASC|
                                 GRN_TABLE_SORT_AS_NUMBER|
                                 GRN_TABLE_SORT_AS_UNSIGNED, NULL, NULL,0 };
    grn_array *sorted = grn_array_create(ctx, NULL, sizeof(grn_id), 0);
    grn_hash_sort(ctx, (grn_hash *)new, -1, sorted, &arg);
    GRN_TEXT_PUT(ctx, posting, ((grn_hash *)new)->n_entries, sizeof(uint32_t));
    GRN_ARRAY_EACH(ctx, sorted, 0, 0, id, &tp, {
      grn_hash_get_key(ctx, (grn_hash *)new, *tp, &tid, sizeof(grn_id));
      gap = tid - tid_;
      GRN_TEXT_PUT(ctx, posting, &gap, sizeof(grn_id));
      tid_ = tid;
    });
    GRN_ARRAY_EACH(ctx, sorted, 0, 0, id, &tp, {
      grn_hash_get_value(ctx, (grn_hash *)new, *tp, &u_);
      u_->offset = offset++;
      GRN_TEXT_PUT(ctx, posting, &u_->tf, sizeof(int32_t));
    });
    tpe = (grn_id *)GRN_BULK_CURR(post);
    for (tp = (grn_id *)GRN_BULK_HEAD(post); tp < tpe; tp++) {
      grn_hash_get(ctx, (grn_hash *)new, (void *)tp, sizeof(grn_id), (void **)&u);
      GRN_TEXT_PUT(ctx, posting, &(*u)->offset, sizeof(int32_t));
    }
    GRN_OBJ_FIN(ctx, post);
    grn_array_close(ctx, sorted);
  }

  if (old) {
    unsigned char type = (ii->obj.header.domain == old->header.domain)
      ? GRN_UVECTOR
      : old->header.type;
    switch (type) {
    case GRN_BULK :
      {
        //        const char *str = GRN_BULK_HEAD(old);
        //        unsigned int str_len = GRN_BULK_VSIZE(old);
        old_ = old;
        GRN_OBJ_INIT(&oldv, GRN_VECTOR, GRN_OBJ_DO_SHALLOW_COPY, GRN_DB_TEXT);
        oldv.u.v.body = old;
        old = &oldv;
        grn_vector_delimit(ctx, old, 0, GRN_ID_NIL);
        if (old_ != oldvalue) { grn_obj_close(ctx, old_); }
      }
      /* fallthru */
    case GRN_VECTOR :
      old_ = old;
      old = (grn_obj *)grn_hash_create(ctx, NULL, sizeof(grn_id),
                                       sizeof(grn_ii_updspec *),
                                       GRN_HASH_TINY);
      if (!old) {
        GRN_LOG(ctx, GRN_LOG_ALERT, "grn_hash_create(ctx, NULL, old) on grn_ii_update failed!");
        rc = GRN_NO_MEMORY_AVAILABLE;
      } else {
        rc = grn_vector2updspecs(ctx, ii, rid, section, old_, old, GRN_TOKEN_DEL, NULL);
      }
      if (old_ != oldvalue) { grn_obj_close(ctx, old_); }
      if (rc) { goto exit; }
      break;
    case GRN_UVECTOR :
      old_ = old;
      old = (grn_obj *)grn_hash_create(ctx, NULL, sizeof(grn_id),
                                       sizeof(grn_ii_updspec *),
                                       GRN_HASH_TINY);
      if (!old) {
        GRN_LOG(ctx, GRN_LOG_ALERT, "grn_hash_create(ctx, NULL, old) on grn_ii_update failed!");
        rc = GRN_NO_MEMORY_AVAILABLE;
      } else {
        if (old_->header.type == GRN_UVECTOR) {
          rc = grn_uvector2updspecs(ctx, ii, rid, section, old_, old,
                                    GRN_TOKEN_DEL, NULL);
        } else {
          grn_obj uvector;
          unsigned int weight = 0;
          GRN_VALUE_FIX_SIZE_INIT(&uvector, GRN_OBJ_VECTOR, old_->header.domain);
          if (old_->header.impl_flags & GRN_OBJ_WITH_WEIGHT) {
            uvector.header.impl_flags |= GRN_OBJ_WITH_WEIGHT;
          }
          grn_uvector_add_element(ctx, &uvector, GRN_RECORD_VALUE(old_), weight);
          rc = grn_uvector2updspecs(ctx, ii, rid, section, &uvector, old,
                                    GRN_TOKEN_DEL, NULL);
          GRN_OBJ_FIN(ctx, &uvector);
        }
      }
      if (old_ != oldvalue) { grn_obj_close(ctx, old_); }
      if (rc) { goto exit; }
      break;
    case GRN_TABLE_HASH_KEY :
      break;
    default :
      ERR(GRN_INVALID_ARGUMENT, "invalid object assigned as oldvalue");
      goto exit;
    }
  }

  if (old) {
    grn_id eid;
    grn_hash *o = (grn_hash *)old;
    grn_hash *n = (grn_hash *)new;
    GRN_HASH_EACH(ctx, o, id, &tp, NULL, &u, {
      if (n && (eid = grn_hash_get(ctx, n, tp, sizeof(grn_id), (void **) &un))) {
        if (do_grn_ii_updspec_cmp && !grn_ii_updspec_cmp(*u, *un)) {
          grn_ii_updspec_close(ctx, *un);
          grn_hash_delete_by_id(ctx, n, eid, NULL);
        }
      } else {
        grn_rc r;
        r = grn_ii_delete_one(ctx, ii, *tp, *u, n);
        if (r) {
          rc = r;
        }
      }
      grn_ii_updspec_close(ctx, *u);
    });
  }
  if (new) {
    grn_hash *n = (grn_hash *)new;
    GRN_HASH_EACH(ctx, n, id, &tp, NULL, &u, {
      grn_rc r;
      if ((r = grn_ii_update_one(ctx, ii, *tp, *u, n))) { rc = r; }
      grn_ii_updspec_close(ctx, *u);
    });
  } else {
    if (!section) {
      /* todo: delete key when all sections deleted */
    }
  }
exit :
  grn_io_unlock(ii->seg);
  if (old && old != oldvalue) { grn_obj_close(ctx, old); }
  if (new && new != newvalue) { grn_obj_close(ctx, new); }
  return ctx->rc;
}

/* token_info */

typedef struct {
  cursor_heap *cursors;
  int offset;
  int pos;
  int size;
  int ntoken;
  grn_ii_posting *p;
} token_info;

#define EX_NONE   0
#define EX_PREFIX 1
#define EX_SUFFIX 2
#define EX_BOTH   3

inline static void
token_info_expand_both(grn_ctx *ctx, grn_obj *lexicon, grn_ii *ii,
                       const char *key, unsigned int key_size, token_info *ti)
{
  int s = 0;
  grn_hash *h, *g;
  uint32_t *offset2;
  grn_hash_cursor *c;
  grn_id *tp, *tq;
  if ((h = grn_hash_create(ctx, NULL, sizeof(grn_id), 0, 0))) {
    grn_table_search(ctx, lexicon, key, key_size,
                     GRN_OP_PREFIX, (grn_obj *)h, GRN_OP_OR);
    if (GRN_HASH_SIZE(h)) {
      if ((ti->cursors = cursor_heap_open(ctx, GRN_HASH_SIZE(h) + 256))) {
        if ((c = grn_hash_cursor_open(ctx, h, NULL, 0, NULL, 0, 0, -1, 0))) {
          uint32_t key2_size;
          const char *key2;
          while (grn_hash_cursor_next(ctx, c)) {
            grn_hash_cursor_get_key(ctx, c, (void **) &tp);
            key2 = _grn_table_key(ctx, lexicon, *tp, &key2_size);
            if (!key2) { break; }
            if ((lexicon->header.type != GRN_TABLE_PAT_KEY) ||
                !(lexicon->header.flags & GRN_OBJ_KEY_WITH_SIS) ||
                key2_size <= 2) { // todo: refine
              if ((s = grn_ii_estimate_size(ctx, ii, *tp))) {
                cursor_heap_push(ctx, ti->cursors, ii, *tp, 0);
                ti->ntoken++;
                ti->size += s;
              }
            } else {
              if ((g = grn_hash_create(ctx, NULL, sizeof(grn_id), 0, GRN_HASH_TINY))) {
                grn_pat_suffix_search(ctx, (grn_pat *)lexicon, key2, key2_size, g);
                GRN_HASH_EACH(ctx, g, id, &tq, NULL, &offset2, {
                  if ((s = grn_ii_estimate_size(ctx, ii, *tq))) {
                    cursor_heap_push(ctx, ti->cursors, ii, *tq, /* *offset2 */ 0);
                    ti->ntoken++;
                    ti->size += s;
                  }
                });
                grn_hash_close(ctx, g);
              }
            }
          }
          grn_hash_cursor_close(ctx, c);
        }
      }
    }
    grn_hash_close(ctx, h);
  }
}

inline static grn_rc
token_info_close(grn_ctx *ctx, token_info *ti)
{
  cursor_heap_close(ctx, ti->cursors);
  GRN_FREE(ti);
  return GRN_SUCCESS;
}

inline static token_info *
token_info_open(grn_ctx *ctx, grn_obj *lexicon, grn_ii *ii,
                const char *key, unsigned int key_size, uint32_t offset, int mode)
{
  int s = 0;
  grn_hash *h;
  token_info *ti;
  grn_id tid;
  grn_id *tp;
  if (!key) { return NULL; }
  if (!(ti = GRN_MALLOC(sizeof(token_info)))) { return NULL; }
  ti->cursors = NULL;
  ti->size = 0;
  ti->ntoken = 0;
  ti->offset = offset;
  switch (mode) {
  case EX_BOTH :
    token_info_expand_both(ctx, lexicon, ii, key, key_size, ti);
    break;
  case EX_NONE :
    if ((tid = grn_table_get(ctx, lexicon, key, key_size)) &&
        (s = grn_ii_estimate_size(ctx, ii, tid)) &&
        (ti->cursors = cursor_heap_open(ctx, 1))) {
      cursor_heap_push(ctx, ti->cursors, ii, tid, 0);
      ti->ntoken++;
      ti->size = s;
    }
    break;
  case EX_PREFIX :
    if ((h = grn_hash_create(ctx, NULL, sizeof(grn_id), 0, 0))) {
      grn_table_search(ctx, lexicon, key, key_size,
                       GRN_OP_PREFIX, (grn_obj *)h, GRN_OP_OR);
      if (GRN_HASH_SIZE(h)) {
        if ((ti->cursors = cursor_heap_open(ctx, GRN_HASH_SIZE(h)))) {
          GRN_HASH_EACH(ctx, h, id, &tp, NULL, NULL, {
            if ((s = grn_ii_estimate_size(ctx, ii, *tp))) {
              cursor_heap_push(ctx, ti->cursors, ii, *tp, 0);
              ti->ntoken++;
              ti->size += s;
            }
          });
        }
      }
      grn_hash_close(ctx, h);
    }
    break;
  case EX_SUFFIX :
    if ((h = grn_hash_create(ctx, NULL, sizeof(grn_id), 0, 0))) {
      grn_table_search(ctx, lexicon, key, key_size,
                       GRN_OP_SUFFIX, (grn_obj *)h, GRN_OP_OR);
      if (GRN_HASH_SIZE(h)) {
        if ((ti->cursors = cursor_heap_open(ctx, GRN_HASH_SIZE(h)))) {
          uint32_t *offset2;
          GRN_HASH_EACH(ctx, h, id, &tp, NULL, &offset2, {
            if ((s = grn_ii_estimate_size(ctx, ii, *tp))) {
              cursor_heap_push(ctx, ti->cursors, ii, *tp, /* *offset2 */ 0);
              ti->ntoken++;
              ti->size += s;
            }
          });
        }
      }
      grn_hash_close(ctx, h);
    }
    break;
  }
  if (cursor_heap_push2(ti->cursors)) {
    token_info_close(ctx, ti);
    return NULL;
  }
  {
    grn_ii_cursor *ic;
    if (ti->cursors && (ic = cursor_heap_min(ti->cursors))) {
      grn_ii_posting *p = ic->post;
      ti->pos = p->pos - ti->offset;
      ti->p = p;
    } else {
      token_info_close(ctx, ti);
      ti = NULL;
    }
  }
  return ti;
}

static inline grn_rc
token_info_skip(grn_ctx *ctx, token_info *ti, uint32_t rid, uint32_t sid)
{
  grn_ii_cursor *c;
  grn_ii_posting *p;
  for (;;) {
    if (!(c = cursor_heap_min(ti->cursors))) { return GRN_END_OF_DATA; }
    p = c->post;
    if (p->rid > rid || (p->rid == rid && p->sid >= sid)) { break; }
    cursor_heap_pop(ctx, ti->cursors, rid);
  }
  ti->pos = p->pos - ti->offset;
  ti->p = p;
  return GRN_SUCCESS;
}

static inline grn_rc
token_info_skip_pos(grn_ctx *ctx, token_info *ti, uint32_t rid, uint32_t sid, uint32_t pos)
{
  grn_ii_cursor *c;
  grn_ii_posting *p;
  pos += ti->offset;
  for (;;) {
    if (!(c = cursor_heap_min(ti->cursors))) { return GRN_END_OF_DATA; }
    p = c->post;
    if (p->rid != rid || p->sid != sid || p->pos >= pos) { break; }
    cursor_heap_pop_pos(ctx, ti->cursors);
  }
  ti->pos = p->pos - ti->offset;
  ti->p = p;
  return GRN_SUCCESS;
}

inline static int
token_compare(const void *a, const void *b)
{
  const token_info *t1 = *((token_info **)a), *t2 = *((token_info **)b);
  return t1->size - t2->size;
}

inline static grn_rc
token_info_build(grn_ctx *ctx, grn_obj *lexicon, grn_ii *ii, const char *string, unsigned int string_len,
                 token_info **tis, uint32_t *n, grn_bool *only_skip_token,
                 grn_operator mode)
{
  token_info *ti;
  const char *key;
  uint32_t size;
  grn_rc rc = GRN_END_OF_DATA;
  unsigned int token_flags = GRN_TOKEN_CURSOR_ENABLE_TOKENIZED_DELIMITER;
  grn_token_cursor *token_cursor = grn_token_cursor_open(ctx, lexicon,
                                                         string, string_len,
                                                         GRN_TOKEN_GET,
                                                         token_flags);
  *only_skip_token = GRN_FALSE;
  if (!token_cursor) { return GRN_NO_MEMORY_AVAILABLE; }
  if (mode == GRN_OP_UNSPLIT) {
    if ((ti = token_info_open(ctx, lexicon, ii, (char *)token_cursor->orig, token_cursor->orig_blen, 0, EX_BOTH))) {
      tis[(*n)++] = ti;
      rc = GRN_SUCCESS;
    }
  } else {
    grn_id tid;
    int ef;
    switch (mode) {
    case GRN_OP_PREFIX :
      ef = EX_PREFIX;
      break;
    case GRN_OP_SUFFIX :
      ef = EX_SUFFIX;
      break;
    case GRN_OP_PARTIAL :
      ef = EX_BOTH;
      break;
    default :
      ef = EX_NONE;
      break;
    }
    tid = grn_token_cursor_next(ctx, token_cursor);
    if (token_cursor->force_prefix) { ef |= EX_PREFIX; }
    switch (token_cursor->status) {
    case GRN_TOKEN_CURSOR_DOING :
      key = _grn_table_key(ctx, lexicon, tid, &size);
      ti = token_info_open(ctx, lexicon, ii, key, size, token_cursor->pos, ef & EX_SUFFIX);
      break;
    case GRN_TOKEN_CURSOR_DONE :
      ti = token_info_open(ctx, lexicon, ii, (const char *)token_cursor->curr,
                           token_cursor->curr_size, 0, ef);
      /*
      key = _grn_table_key(ctx, lexicon, tid, &size);
      ti = token_info_open(ctx, lexicon, ii, token_cursor->curr, token_cursor->curr_size, token_cursor->pos, ef);
      ti = token_info_open(ctx, lexicon, ii, (char *)token_cursor->orig,
                           token_cursor->orig_blen, token_cursor->pos, ef);
      */
      break;
    case GRN_TOKEN_CURSOR_NOT_FOUND :
      ti = token_info_open(ctx, lexicon, ii, (char *)token_cursor->orig,
                           token_cursor->orig_blen, 0, ef);
      break;
    case GRN_TOKEN_CURSOR_DONE_SKIP :
      *only_skip_token = GRN_TRUE;
      goto exit;
    default :
      goto exit;
    }
    if (!ti) { goto exit ; }
    tis[(*n)++] = ti;
    while (token_cursor->status == GRN_TOKEN_CURSOR_DOING) {
      tid = grn_token_cursor_next(ctx, token_cursor);
      if (token_cursor->force_prefix) { ef |= EX_PREFIX; }
      switch (token_cursor->status) {
      case GRN_TOKEN_CURSOR_DONE_SKIP :
        continue;
      case GRN_TOKEN_CURSOR_DOING :
        key = _grn_table_key(ctx, lexicon, tid, &size);
        ti = token_info_open(ctx, lexicon, ii, key, size, token_cursor->pos, EX_NONE);
        break;
      case GRN_TOKEN_CURSOR_DONE :
        if (tid) {
          key = _grn_table_key(ctx, lexicon, tid, &size);
          ti = token_info_open(ctx, lexicon, ii, key, size, token_cursor->pos, ef & EX_PREFIX);
          break;
        } /* else fallthru */
      default :
        ti = token_info_open(ctx, lexicon, ii, (char *)token_cursor->curr,
                             token_cursor->curr_size, token_cursor->pos, ef & EX_PREFIX);
        break;
      }
      if (!ti) {
        goto exit;
      }
      tis[(*n)++] = ti;
    }
    rc = GRN_SUCCESS;
  }
exit :
  grn_token_cursor_close(ctx, token_cursor);
  return rc;
}

static void
token_info_clear_offset(token_info **tis, uint32_t n)
{
  token_info **tie;
  for (tie = tis + n; tis < tie; tis++) { (*tis)->offset = 0; }
}

/* select */

inline static void
res_add(grn_ctx *ctx, grn_hash *s, grn_rset_posinfo *pi, double score,
        grn_operator op)
{
  grn_rset_recinfo *ri;
  switch (op) {
  case GRN_OP_OR :
    if (grn_hash_add(ctx, s, pi, s->key_size, (void **)&ri, NULL)) {
      if (s->obj.header.flags & GRN_OBJ_WITH_SUBREC) {
        grn_table_add_subrec((grn_obj *)s, ri, score, pi, 1);
      }
    }
    break;
  case GRN_OP_AND :
    if (grn_hash_get(ctx, s, pi, s->key_size, (void **)&ri)) {
      if (s->obj.header.flags & GRN_OBJ_WITH_SUBREC) {
        ri->n_subrecs |= GRN_RSET_UTIL_BIT;
        grn_table_add_subrec((grn_obj *)s, ri, score, pi, 1);
      }
    }
    break;
  case GRN_OP_AND_NOT :
    {
      grn_id id;
      if ((id = grn_hash_get(ctx, s, pi, s->key_size, (void **)&ri))) {
        grn_hash_delete_by_id(ctx, s, id, NULL);
      }
    }
    break;
  case GRN_OP_ADJUST :
    if (grn_hash_get(ctx, s, pi, s->key_size, (void **)&ri)) {
      if (s->obj.header.flags & GRN_OBJ_WITH_SUBREC) {
        ri->score += score;
      }
    }
    break;
  default :
    break;
  }
}

grn_rc
grn_ii_posting_add(grn_ctx *ctx, grn_ii_posting *pos, grn_hash *s, grn_operator op)
{
  res_add(ctx, s, (grn_rset_posinfo *)(pos), (1 + pos->weight), op);
  return ctx->rc;
}

#ifdef USE_BHEAP

/* todo */

#else /* USE_BHEAP */

struct _btr_node {
  struct _btr_node *car;
  struct _btr_node *cdr;
  token_info *ti;
};

typedef struct _btr_node btr_node;

typedef struct {
  int n;
  token_info *min;
  token_info *max;
  btr_node *root;
  btr_node *nodes;
} btr;

inline static void
bt_zap(btr *bt)
{
  bt->n = 0;
  bt->min = NULL;
  bt->max = NULL;
  bt->root = NULL;
}

inline static btr *
bt_open(grn_ctx *ctx, int size)
{
  btr *bt = GRN_MALLOC(sizeof(btr));
  if (bt) {
    bt_zap(bt);
    if (!(bt->nodes = GRN_MALLOC(sizeof(btr_node) * size))) {
      GRN_FREE(bt);
      bt = NULL;
    }
  }
  return bt;
}

inline static void
bt_close(grn_ctx *ctx, btr *bt)
{
  if (!bt) { return; }
  GRN_FREE(bt->nodes);
  GRN_FREE(bt);
}

inline static void
bt_push(btr *bt, token_info *ti)
{
  int pos = ti->pos, minp = 1, maxp = 1;
  btr_node *node, *new, **last;
  new = bt->nodes + bt->n++;
  new->ti = ti;
  new->car = NULL;
  new->cdr = NULL;
  for (last = &bt->root; (node = *last);) {
    if (pos < node->ti->pos) {
      last = &node->car;
      maxp = 0;
    } else {
      last = &node->cdr;
      minp = 0;
    }
  }
  *last = new;
  if (minp) { bt->min = ti; }
  if (maxp) { bt->max = ti; }
}

inline static void
bt_pop(btr *bt)
{
  btr_node *node, *min, *newmin, **last;
  for (last = &bt->root; (min = *last) && min->car; last = &min->car) ;
  if (min) {
    int pos = min->ti->pos, minp = 1, maxp = 1;
    *last = min->cdr;
    min->cdr = NULL;
    for (last = &bt->root; (node = *last);) {
      if (pos < node->ti->pos) {
        last = &node->car;
        maxp = 0;
      } else {
        last = &node->cdr;
        minp = 0;
      }
    }
    *last = min;
    if (maxp) { bt->max = min->ti; }
    if (!minp) {
      for (newmin = bt->root; newmin->car; newmin = newmin->car) ;
      bt->min = newmin->ti;
    }
  }
}

#endif /* USE_BHEAP */

typedef enum {
  grn_wv_none = 0,
  grn_wv_static,
  grn_wv_dynamic,
  grn_wv_constant
} grn_wv_mode;

inline static double
get_weight(grn_ctx *ctx, grn_hash *s, grn_id rid, int sid,
           grn_wv_mode wvm, grn_select_optarg *optarg)
{
  switch (wvm) {
  case grn_wv_none :
    return 1;
  case grn_wv_static :
    return sid <= optarg->vector_size ? optarg->weight_vector[sid - 1] : 0;
  case grn_wv_dynamic :
    /* todo : support hash with keys
    if (s->keys) {
      uint32_t key_size;
      const char *key = _grn_table_key(ctx, s->keys, rid, &key_size);
      // todo : change grn_select_optarg
      return key ? optarg->func(s, key, key_size, sid, optarg->func_arg) : 0;
    }
    */
    /* todo : cast */
    return optarg->func(ctx, (void *)s, (void *)(intptr_t)rid, sid, optarg->func_arg);
  case grn_wv_constant :
    return optarg->vector_size;
  default :
    return 1;
  }
}

grn_rc
grn_ii_similar_search(grn_ctx *ctx, grn_ii *ii,
                      const char *string, unsigned int string_len,
                      grn_hash *s, grn_operator op, grn_select_optarg *optarg)
{
  int *w1, limit;
  grn_id tid, *tp, max_size;
  grn_rc rc = GRN_SUCCESS;
  grn_hash *h;
  grn_token_cursor *token_cursor;
  unsigned int token_flags = GRN_TOKEN_CURSOR_ENABLE_TOKENIZED_DELIMITER;
  grn_obj *lexicon = ii->lexicon;
  if (!lexicon || !ii || !string || !string_len || !s || !optarg) { return GRN_INVALID_ARGUMENT; }
  if (!(h = grn_hash_create(ctx, NULL, sizeof(grn_id), sizeof(int), 0))) {
    return GRN_NO_MEMORY_AVAILABLE;
  }
  if (!(token_cursor = grn_token_cursor_open(ctx, lexicon, string, string_len,
                                             GRN_TOKEN_GET, token_flags))) {
    grn_hash_close(ctx, h);
    return GRN_NO_MEMORY_AVAILABLE;
  }
  if (!(max_size = optarg->max_size)) { max_size = 1048576; }
  while (token_cursor->status != GRN_TOKEN_CURSOR_DONE &&
         token_cursor->status != GRN_TOKEN_CURSOR_DONE_SKIP) {
    if ((tid = grn_token_cursor_next(ctx, token_cursor))) {
      if (grn_hash_add(ctx, h, &tid, sizeof(grn_id), (void **)&w1, NULL)) { (*w1)++; }
    }
    if (tid && token_cursor->curr_size) {
      if (optarg->max_interval == GRN_OP_UNSPLIT) {
        grn_table_search(ctx, lexicon, token_cursor->curr, token_cursor->curr_size,
                         GRN_OP_PREFIX, (grn_obj *)h, GRN_OP_OR);
      }
      if (optarg->max_interval == GRN_OP_PARTIAL) {
        grn_table_search(ctx, lexicon, token_cursor->curr, token_cursor->curr_size,
                         GRN_OP_SUFFIX, (grn_obj *)h, GRN_OP_OR);
      }
    }
  }
  grn_token_cursor_close(ctx, token_cursor);
  {
    grn_hash_cursor *c = grn_hash_cursor_open(ctx, h, NULL, 0, NULL, 0, 0, -1, 0);
    if (!c) {
      GRN_LOG(ctx, GRN_LOG_ALERT, "grn_hash_cursor_open on grn_ii_similar_search failed !");
      grn_hash_close(ctx, h);
      return GRN_NO_MEMORY_AVAILABLE;
    }
    while (grn_hash_cursor_next(ctx, c)) {
      uint32_t es;
      grn_hash_cursor_get_key_value(ctx, c, (void **) &tp, NULL, (void **) &w1);
      if ((es = grn_ii_estimate_size(ctx, ii, *tp))) {
        *w1 += max_size / es;
      } else {
        grn_hash_cursor_delete(ctx, c, NULL);
      }
    }
    grn_hash_cursor_close(ctx, c);
  }
  limit = optarg->similarity_threshold
    ? (optarg->similarity_threshold > GRN_HASH_SIZE(h)
       ? GRN_HASH_SIZE(h)
       : optarg->similarity_threshold)
    : (GRN_HASH_SIZE(h) >> 3) + 1;
  if (GRN_HASH_SIZE(h)) {
    grn_id j, id;
    int w2, rep;
    grn_ii_cursor *c;
    grn_ii_posting *pos;
    grn_wv_mode wvm = grn_wv_none;
    grn_table_sort_optarg arg = {
      GRN_TABLE_SORT_DESC|GRN_TABLE_SORT_BY_VALUE|GRN_TABLE_SORT_AS_NUMBER,
      NULL,
      NULL,
      0
    };
    grn_array *sorted = grn_array_create(ctx, NULL, sizeof(grn_id), 0);
    if (!sorted) {
      GRN_LOG(ctx, GRN_LOG_ALERT, "grn_hash_sort on grn_ii_similar_search failed !");
      grn_hash_close(ctx, h);
      return GRN_NO_MEMORY_AVAILABLE;
    }
    grn_hash_sort(ctx, h, limit, sorted, &arg);
    /* todo support subrec
    rep = (s->record_unit == grn_rec_position || s->subrec_unit == grn_rec_position);
    */
    rep = 0;
    if (optarg->func) {
      wvm = grn_wv_dynamic;
    } else if (optarg->vector_size) {
      wvm = optarg->weight_vector ? grn_wv_static : grn_wv_constant;
    }
    for (j = 1; j <= limit; j++) {
      grn_array_get_value(ctx, sorted, j, &id);
      _grn_hash_get_key_value(ctx, h, id, (void **) &tp, (void **) &w1);
      if (!*tp || !(c = grn_ii_cursor_open(ctx, ii, *tp, GRN_ID_NIL, GRN_ID_MAX,
                                           rep
                                           ? ii->n_elements
                                           : ii->n_elements - 1, 0))) {
        GRN_LOG(ctx, GRN_LOG_ERROR, "cursor open failed (%d)", *tp);
        continue;
      }
      if (rep) {
        while (grn_ii_cursor_next(ctx, c)) {
          pos = c->post;
          if ((w2 = get_weight(ctx, s, pos->rid, pos->sid, wvm, optarg)) > 0) {
            while (grn_ii_cursor_next_pos(ctx, c)) {
              res_add(ctx, s, (grn_rset_posinfo *) pos, *w1 * w2 * (1 + pos->weight), op);
            }
          }
        }
      } else {
        while (grn_ii_cursor_next(ctx, c)) {
          pos = c->post;
          if ((w2 = get_weight(ctx, s, pos->rid, pos->sid, wvm, optarg)) > 0) {
            res_add(ctx, s, (grn_rset_posinfo *) pos, *w1 * w2 * (pos->tf + pos->weight), op);
          }
        }
      }
      grn_ii_cursor_close(ctx, c);
    }
    grn_array_close(ctx, sorted);
  }
  grn_hash_close(ctx, h);
  grn_ii_resolve_sel_and(ctx, s, op);
  //  grn_hash_cursor_clear(r);
  return rc;
}

#define TERM_EXTRACT_EACH_POST 0
#define TERM_EXTRACT_EACH_TERM 1

grn_rc
grn_ii_term_extract(grn_ctx *ctx, grn_ii *ii, const char *string,
                     unsigned int string_len, grn_hash *s,
                     grn_operator op, grn_select_optarg *optarg)
{
  grn_rset_posinfo pi;
  grn_id tid;
  const char *p, *pe;
  grn_obj *nstr;
  const char *normalized;
  unsigned int normalized_length_in_bytes;
  grn_ii_cursor *c;
  grn_ii_posting *pos;
  int skip, rep, policy;
  grn_rc rc = GRN_SUCCESS;
  grn_wv_mode wvm = grn_wv_none;
  if (!ii || !string || !string_len || !s || !optarg) {
    return GRN_INVALID_ARGUMENT;
  }
  if (!(nstr = grn_string_open(ctx, string, string_len, NULL, 0))) {
    return GRN_INVALID_ARGUMENT;
  }
  policy = optarg->max_interval;
  if (optarg->func) {
    wvm = grn_wv_dynamic;
  } else if (optarg->vector_size) {
    wvm = optarg->weight_vector ? grn_wv_static : grn_wv_constant;
  }
  /* todo support subrec
  if (policy == TERM_EXTRACT_EACH_POST) {
    if ((rc = grn_records_reopen(s, grn_rec_section, grn_rec_none, 0))) { goto exit; }
  }
  rep = (s->record_unit == grn_rec_position || s->subrec_unit == grn_rec_position);
  */
  rep = 0;
  grn_string_get_normalized(ctx, nstr, &normalized, &normalized_length_in_bytes,
                            NULL);
  for (p = normalized, pe = p + normalized_length_in_bytes; p < pe; p += skip) {
    if ((tid = grn_table_lcp_search(ctx, ii->lexicon, p, pe - p))) {
      if (policy == TERM_EXTRACT_EACH_POST) {
        if (!(skip = grn_table_get_key(ctx, ii->lexicon, tid, NULL, 0))) { break; }
      } else {
        if (!(skip = (int)grn_charlen(ctx, p, pe))) { break; }
      }
      if (!(c = grn_ii_cursor_open(ctx, ii, tid, GRN_ID_NIL, GRN_ID_MAX,
                                   rep
                                   ? ii->n_elements
                                   : ii->n_elements - 1, 0))) {
        GRN_LOG(ctx, GRN_LOG_ERROR, "cursor open failed (%d)", tid);
        continue;
      }
      if (rep) {
        while (grn_ii_cursor_next(ctx, c)) {
          pos = c->post;
          while (grn_ii_cursor_next_pos(ctx, c)) {
            res_add(ctx, s, (grn_rset_posinfo *) pos,
                    get_weight(ctx, s, pos->rid, pos->sid, wvm, optarg), op);
          }
        }
      } else {
        while (grn_ii_cursor_next(ctx, c)) {
          if (policy == TERM_EXTRACT_EACH_POST) {
            pi.rid = c->post->rid;
            pi.sid = p - normalized;
            res_add(ctx, s, &pi, pi.sid + 1, op);
          } else {
            pos = c->post;
            res_add(ctx, s, (grn_rset_posinfo *) pos,
                    get_weight(ctx, s, pos->rid, pos->sid, wvm, optarg), op);
          }
        }
      }
      grn_ii_cursor_close(ctx, c);
    } else {
      if (!(skip = (int)grn_charlen(ctx, p, pe))) {
        break;
      }
    }
  }
  grn_obj_close(ctx, nstr);
  return rc;
}

#ifdef GRN_II_SELECT_ENABLE_SEQUENTIAL_SEARCH
static grn_bool
grn_ii_select_sequential_search_should_use(grn_ctx *ctx,
                                           grn_ii *ii,
                                           const char *raw_query,
                                           unsigned int raw_query_len,
                                           grn_hash *result,
                                           grn_operator op,
                                           grn_wv_mode wvm,
                                           grn_select_optarg *optarg,
                                           token_info **token_infos,
                                           uint32_t n_token_infos,
                                           double too_many_index_match_ratio)
{
  int n_sources;

  if (too_many_index_match_ratio < 0.0) {
    return GRN_FALSE;
  }

  if (op != GRN_OP_AND) {
    return GRN_FALSE;
  }

  if (optarg->mode != GRN_OP_EXACT) {
    return GRN_FALSE;
  }

  n_sources = ii->obj.source_size / sizeof(grn_id);
  if (n_sources == 0) {
    return GRN_FALSE;
  }

  {
    uint32_t i;
    int n_existing_records;

    n_existing_records = GRN_HASH_SIZE(result);
    for (i = 0; i < n_token_infos; i++) {
      token_info *info = token_infos[i];
      if (n_existing_records <= (info->size * too_many_index_match_ratio)) {
        return GRN_TRUE;
      }
    }
    return GRN_FALSE;
  }
}

static void
grn_ii_select_sequential_search_body(grn_ctx *ctx,
                                     grn_ii *ii,
                                     grn_obj *normalizer,
                                     grn_encoding encoding,
                                     OnigRegex regex,
                                     grn_hash *result,
                                     grn_operator op,
                                     grn_wv_mode wvm,
                                     grn_select_optarg *optarg)
{
  int i, n_sources;
  grn_id *source_ids = ii->obj.source;
  grn_obj buffer;

  GRN_TEXT_INIT(&buffer, 0);
  n_sources = ii->obj.source_size / sizeof(grn_id);
  for (i = 0; i < n_sources; i++) {
    grn_id source_id = source_ids[i];
    grn_obj *source;
    char column_name[GRN_TABLE_MAX_KEY_SIZE];
    int column_name_size;
    grn_obj *accessor;

    source = grn_ctx_at(ctx, source_id);
    column_name_size = grn_column_name(ctx, source,
                                       column_name,
                                       GRN_TABLE_MAX_KEY_SIZE);
    accessor = grn_obj_column(ctx, (grn_obj *)result, column_name,
                              column_name_size);
    {
      grn_hash_cursor *cursor;
      grn_id id;
      cursor = grn_hash_cursor_open(ctx, result, NULL, 0, NULL, 0, 0, -1, 0);
      while ((id = grn_hash_cursor_next(ctx, cursor)) != GRN_ID_NIL) {
        OnigPosition position;
        grn_obj *value;
        const char *normalized_value;
        unsigned int normalized_value_length;

        GRN_BULK_REWIND(&buffer);
        grn_obj_get_value(ctx, accessor, id, &buffer);
        value = grn_string_open_(ctx,
                                 GRN_TEXT_VALUE(&buffer),
                                 GRN_TEXT_LEN(&buffer),
                                 normalizer, 0, encoding);
        grn_string_get_normalized(ctx, value,
                                  &normalized_value, &normalized_value_length,
                                  NULL);
        position = onig_search(regex,
                               normalized_value,
                               normalized_value + normalized_value_length,
                               normalized_value,
                               normalized_value + normalized_value_length,
                               NULL,
                               0);
        if (position != ONIG_MISMATCH) {
          grn_rset_posinfo info;
          double score;
          info.rid = id;
          info.sid = i + 1;
          info.pos = 0;
          score = get_weight(ctx, result, info.rid, info.sid, wvm, optarg);
          res_add(ctx, result, &info, score, op);
        }
        grn_obj_unlink(ctx, value);
      }
      grn_hash_cursor_close(ctx, cursor);
    }
    grn_obj_unlink(ctx, accessor);
  }
  grn_obj_unlink(ctx, &buffer);
}

static grn_bool
grn_ii_select_sequential_search(grn_ctx *ctx,
                                grn_ii *ii,
                                const char *raw_query,
                                unsigned int raw_query_len,
                                grn_hash *result,
                                grn_operator op,
                                grn_wv_mode wvm,
                                grn_select_optarg *optarg,
                                token_info **token_infos,
                                uint32_t n_token_infos)
{
  grn_bool processed = GRN_TRUE;

  {
    /* Disabled by default. */
    double too_many_index_match_ratio = -1;
    char too_many_index_match_ratio_env[GRN_ENV_BUFFER_SIZE];
    grn_getenv("GRN_II_SELECT_TOO_MANY_INDEX_MATCH_RATIO",
               too_many_index_match_ratio_env,
               GRN_ENV_BUFFER_SIZE);
    if (too_many_index_match_ratio_env[0]) {
      too_many_index_match_ratio = atof(too_many_index_match_ratio_env);
    }

    if (!grn_ii_select_sequential_search_should_use(ctx,
                                                    ii,
                                                    raw_query,
                                                    raw_query_len,
                                                    result,
                                                    op,
                                                    wvm,
                                                    optarg,
                                                    token_infos,
                                                    n_token_infos,
                                                    too_many_index_match_ratio)) {
      return GRN_FALSE;
    }
  }

  {
    grn_encoding encoding;
    grn_obj *normalizer;
    int nflags = 0;
    grn_obj *query;
    const char *normalized_query;
    unsigned int normalized_query_length;

    grn_table_get_info(ctx, ii->lexicon,
                       NULL, &encoding, NULL, &normalizer, NULL);
    query = grn_string_open_(ctx, raw_query, raw_query_len,
                             normalizer, nflags, encoding);
    grn_string_get_normalized(ctx, query,
                              &normalized_query, &normalized_query_length,
                              NULL);
    {
      OnigRegex regex;
      int onig_result;
      OnigErrorInfo error_info;
      onig_result = onig_new(&regex,
                             normalized_query,
                             normalized_query + normalized_query_length,
                             ONIG_OPTION_NONE,
                             ONIG_ENCODING_UTF8,
                             ONIG_SYNTAX_ASIS,
                             &error_info);
      if (onig_result == ONIG_NORMAL) {
        grn_ii_select_sequential_search_body(ctx, ii, normalizer, encoding,
                                             regex, result, op, wvm, optarg);
        onig_free(regex);
      } else {
        char message[ONIG_MAX_ERROR_MESSAGE_LEN];
        onig_error_code_to_str(message, onig_result, error_info);
        GRN_LOG(ctx, GRN_LOG_WARNING,
                "[ii][select][sequential] "
                "failed to create regular expression object: %s",
                message);
        processed = GRN_FALSE;
      }
    }
    grn_obj_unlink(ctx, query);
  }

  return processed;
}
#endif

grn_rc
grn_ii_select(grn_ctx *ctx, grn_ii *ii, const char *string, unsigned int string_len,
              grn_hash *s, grn_operator op, grn_select_optarg *optarg)
{
  btr *bt = NULL;
  grn_rc rc = GRN_SUCCESS;
  int rep, orp, weight, max_interval = 0;
  token_info *ti, **tis = NULL, **tip, **tie;
  uint32_t n = 0, rid, sid, nrid, nsid;
  grn_bool only_skip_token = GRN_FALSE;
  grn_operator mode = GRN_OP_EXACT;
  grn_wv_mode wvm = grn_wv_none;
  grn_obj *lexicon = ii->lexicon;
  grn_scorer_score_func *score_func = NULL;
  grn_scorer_matched_record record;

  if (!lexicon || !ii || !s) { return GRN_INVALID_ARGUMENT; }
  if (optarg) {
    mode = optarg->mode;
    if (optarg->func) {
      wvm = grn_wv_dynamic;
    } else if (optarg->vector_size) {
      wvm = optarg->weight_vector ? grn_wv_static : grn_wv_constant;
    }
  }
  if (mode == GRN_OP_SIMILAR) {
    return grn_ii_similar_search(ctx, ii, string, string_len, s, op, optarg);
  }
  if (mode == GRN_OP_TERM_EXTRACT) {
    return grn_ii_term_extract(ctx, ii, string, string_len, s, op, optarg);
  }
  /* todo : support subrec
  rep = (s->record_unit == grn_rec_position || s->subrec_unit == grn_rec_position);
  orp = (s->record_unit == grn_rec_position || op == GRN_OP_OR);
  */
  rep = 0;
  orp = op == GRN_OP_OR;
  if (!string_len) { goto exit; }
  if (!(tis = GRN_MALLOC(sizeof(token_info *) * string_len * 2))) {
    return GRN_NO_MEMORY_AVAILABLE;
  }
  if (token_info_build(ctx, lexicon, ii, string, string_len, tis, &n, &only_skip_token, mode) || !n) { goto exit; }
  switch (mode) {
  case GRN_OP_NEAR2 :
    token_info_clear_offset(tis, n);
    mode = GRN_OP_NEAR;
    /* fallthru */
  case GRN_OP_NEAR :
    if (!(bt = bt_open(ctx, n))) { rc = GRN_NO_MEMORY_AVAILABLE; goto exit; }
    max_interval = optarg->max_interval;
    break;
  default :
    break;
  }
  qsort(tis, n, sizeof(token_info *), token_compare);
  tie = tis + n;
  /*
  for (tip = tis; tip < tie; tip++) {
    ti = *tip;
    grn_log("o=%d n=%d s=%d r=%d", ti->offset, ti->ntoken, ti->size, ti->rid);
  }
  */
  GRN_LOG(ctx, GRN_LOG_INFO, "n=%d (%.*s)", n, string_len, string);
  /* todo : array as result
  if (n == 1 && (*tis)->cursors->n_entries == 1 && op == GRN_OP_OR
      && !GRN_HASH_SIZE(s) && !s->garbages
      && s->record_unit == grn_rec_document && !s->max_n_subrecs
      && grn_ii_max_section(ii) == 1) {
    grn_ii_cursor *c = (*tis)->cursors->bins[0];
    if ((rc = grn_hash_array_init(s, (*tis)->size + 32768))) { goto exit; }
    do {
      grn_rset_recinfo *ri;
      grn_ii_posting *p = c->post;
      if ((weight = get_weight(ctx, s, p->rid, p->sid, wvm, optarg))) {
        GRN_HASH_INT_ADD(s, p, ri);
        ri->score = (p->tf + p->score) * weight;
        ri->n_subrecs = 1;
      }
    } while (grn_ii_cursor_next(ctx, c));
    goto exit;
  }
  */
#ifdef GRN_II_SELECT_ENABLE_SEQUENTIAL_SEARCH
  if (grn_ii_select_sequential_search(ctx, ii, string, string_len,
                                      s, op, wvm, optarg, tis, n)) {
    goto exit;
  }
#endif

  if (optarg && optarg->scorer) {
    grn_proc *scorer = (grn_proc *)(optarg->scorer);
    score_func = scorer->callbacks.scorer.score;
    record.table = grn_ctx_at(ctx, s->obj.header.domain);
    record.lexicon = lexicon;
    record.id = GRN_ID_NIL;
    GRN_RECORD_INIT(&(record.terms), GRN_OBJ_VECTOR, lexicon->header.domain);
    GRN_UINT32_INIT(&(record.term_weights), GRN_OBJ_VECTOR);
    record.total_term_weights = 0;
    record.n_documents = grn_table_size(ctx, record.table);
    record.n_occurrences = 0;
    record.n_candidates = 0;
    record.n_tokens = 0;
    record.weight = 0;
    record.args_expr = optarg->scorer_args_expr;
    record.args_expr_offset = optarg->scorer_args_expr_offset;
  }

  for (;;) {
    rid = (*tis)->p->rid;
    sid = (*tis)->p->sid;
    for (tip = tis + 1, nrid = rid, nsid = sid + 1; tip < tie; tip++) {
      ti = *tip;
      if (token_info_skip(ctx, ti, rid, sid)) { goto exit; }
      if (ti->p->rid != rid || ti->p->sid != sid) {
        nrid = ti->p->rid;
        nsid = ti->p->sid;
        break;
      }
    }
    weight = get_weight(ctx, s, rid, sid, wvm, optarg);
    if (tip == tie && weight > 0) {
      grn_rset_posinfo pi = {rid, sid, 0};
      if (orp || grn_hash_get(ctx, s, &pi, s->key_size, NULL)) {
        int count = 0, noccur = 0, pos = 0, score = 0, tscore = 0, min, max;

        if (score_func) {
          GRN_BULK_REWIND(&(record.terms));
          GRN_BULK_REWIND(&(record.term_weights));
          record.n_candidates = 0;
          record.n_tokens = 0;
        }

#define SKIP_OR_BREAK(pos) {\
  if (token_info_skip_pos(ctx, ti, rid, sid, pos)) { break; }    \
  if (ti->p->rid != rid || ti->p->sid != sid) { \
    nrid = ti->p->rid; \
    nsid = ti->p->sid; \
    break; \
  } \
}
        if (n == 1 && !rep) {
          noccur = (*tis)->p->tf;
          tscore = (*tis)->p->weight;
          if (score_func) {
            GRN_RECORD_PUT(ctx, &(record.terms), (*tis)->cursors->bins[0]->id);
            GRN_UINT32_PUT(ctx, &(record.term_weights), tscore);
            record.n_occurrences = noccur;
            record.n_candidates = (*tis)->size;
            record.n_tokens = (*tis)->ntoken;
          }
        } else if (mode == GRN_OP_NEAR) {
          bt_zap(bt);
          for (tip = tis; tip < tie; tip++) {
            ti = *tip;
            SKIP_OR_BREAK(pos);
            bt_push(bt, ti);
          }
          if (tip == tie) {
            for (;;) {
              ti = bt->min; min = ti->pos; max = bt->max->pos;
              if (min > max) { exit(0); }
              if (max - min <= max_interval) {
                if (rep) { pi.pos = min; res_add(ctx, s, &pi, weight, op); }
                noccur++;
                if (ti->pos == max + 1) {
                  break;
                }
                SKIP_OR_BREAK(max + 1);
              } else {
                if (ti->pos == max - max_interval) {
                  break;
                }
                SKIP_OR_BREAK(max - max_interval);
              }
              bt_pop(bt);
            }
          }
        } else {
          for (tip = tis; ; tip++) {
            if (tip == tie) { tip = tis; }
            ti = *tip;
            SKIP_OR_BREAK(pos);
            if (ti->pos == pos) {
              score += ti->p->weight; count++;
            } else {
              score = ti->p->weight; count = 1; pos = ti->pos;
              if (noccur == 0 && score_func) {
                GRN_BULK_REWIND(&(record.terms));
                GRN_BULK_REWIND(&(record.term_weights));
                record.n_candidates = 0;
                record.n_tokens = 0;
              }
            }
            if (noccur == 0 && score_func) {
              GRN_RECORD_PUT(ctx, &(record.terms), ti->cursors->bins[0]->id);
              GRN_UINT32_PUT(ctx, &(record.term_weights), ti->p->weight);
              record.n_candidates += ti->size;
              record.n_tokens += ti->ntoken;
            }
            if (count == n) {
              if (rep) { pi.pos = pos; res_add(ctx, s, &pi, (score + 1) * weight, op); }
              tscore += score;
              score = 0; count = 0; pos++;
              noccur++;
            }
          }
        }
        if (noccur && !rep) {
          double record_score;
          if (score_func) {
            record.id = rid;
            record.weight = weight;
            record.n_occurrences = noccur;
            record.total_term_weights = tscore;
            record_score = score_func(ctx, &record) * weight;
          } else {
            record_score = (noccur + tscore) * weight;
          }
          res_add(ctx, s, &pi, record_score, op);
        }
#undef SKIP_OR_BREAK
      }
    }
    if (token_info_skip(ctx, *tis, nrid, nsid)) { goto exit; }
  }
exit :
  if (score_func) {
    GRN_OBJ_FIN(ctx, &(record.terms));
    GRN_OBJ_FIN(ctx, &(record.term_weights));
  }
  for (tip = tis; tip < tis + n; tip++) {
    if (*tip) { token_info_close(ctx, *tip); }
  }
  if (tis) { GRN_FREE(tis); }
  if (!only_skip_token) {
    grn_ii_resolve_sel_and(ctx, s, op);
  }
  //  grn_hash_cursor_clear(r);
  bt_close(ctx, bt);
#ifdef DEBUG
  {
    uint32_t segno = GRN_II_MAX_LSEG, nnref = 0;
    grn_io_mapinfo *info = ii->seg->maps;
    for (; segno; segno--, info++) { if (info->nref) { nnref++; } }
    GRN_LOG(ctx, GRN_LOG_INFO, "nnref=%d", nnref);
  }
#endif /* DEBUG */
  return rc;
}

uint32_t
grn_ii_estimate_size_for_query(grn_ctx *ctx, grn_ii *ii,
                               const char *query, unsigned int query_len,
                               grn_search_optarg *optarg)
{
  grn_rc rc;
  grn_obj *lexicon = ii->lexicon;
  token_info **tis = NULL;
  uint32_t i;
  uint32_t n_tis = 0;
  grn_bool only_skip_token = GRN_FALSE;
  grn_operator mode = GRN_OP_EXACT;
  double estimated_size = 0;

  if (query_len == 0) {
    return 0;
  }

  tis = GRN_MALLOC(sizeof(token_info *) * query_len * 2);
  if (!tis) {
    return 0;
  }

  if (optarg) {
    switch (optarg->mode) {
    case GRN_OP_NEAR :
    case GRN_OP_NEAR2 :
      mode = optarg->mode;
      break;
    case GRN_OP_SIMILAR :
      mode = optarg->mode;
      break;
    case GRN_OP_REGEXP :
      mode = optarg->mode;
      break;
    default :
      break;
    }
  }

  rc = token_info_build(ctx, lexicon, ii, query, query_len,
                        tis, &n_tis, &only_skip_token, mode);
  if (rc != GRN_SUCCESS) {
    goto exit;
  }

  for (i = 0; i < n_tis; i++) {
    token_info *ti = tis[i];
    double term_estimated_size;
    term_estimated_size = ((double)ti->size / ti->ntoken);
    if (i == 0) {
      estimated_size = term_estimated_size;
    } else {
      estimated_size = fmin(estimated_size, term_estimated_size);
    }
  }

exit :
  for (i = 0; i < n_tis; i++) {
    token_info *ti = tis[i];
    if (ti) {
      token_info_close(ctx, ti);
    }
  }
  if (tis) {
    GRN_FREE(tis);
  }

  return estimated_size;
}

uint32_t
grn_ii_estimate_size_for_lexicon_cursor(grn_ctx *ctx, grn_ii *ii,
                                        grn_table_cursor *lexicon_cursor)
{
  grn_id term_id;
  uint32_t estimated_size = 0;

  while ((term_id = grn_table_cursor_next(ctx, lexicon_cursor)) != GRN_ID_NIL) {
    uint32_t term_estimated_size;
    term_estimated_size = grn_ii_estimate_size(ctx, ii, term_id);
    estimated_size += term_estimated_size;
  }

  return estimated_size;
}

grn_rc
grn_ii_sel(grn_ctx *ctx, grn_ii *ii, const char *string, unsigned int string_len,
           grn_hash *s, grn_operator op, grn_search_optarg *optarg)
{
  ERRCLR(ctx);
  GRN_LOG(ctx, GRN_LOG_INFO, "grn_ii_sel > (%.*s)", string_len, string);
  {
    grn_select_optarg arg = {GRN_OP_EXACT, 0, 0, NULL, 0, NULL, NULL, 0, NULL};
    if (!s) { return GRN_INVALID_ARGUMENT; }
    if (optarg) {
      switch (optarg->mode) {
      case GRN_OP_NEAR :
      case GRN_OP_NEAR2 :
        arg.mode = optarg->mode;
        arg.max_interval = optarg->max_interval;
        break;
      case GRN_OP_SIMILAR :
        arg.mode = optarg->mode;
        arg.similarity_threshold = optarg->similarity_threshold;
        break;
      case GRN_OP_REGEXP :
        arg.mode = optarg->mode;
        break;
      default :
        break;
      }
      if (optarg->vector_size > 0) {
        arg.weight_vector = optarg->weight_vector;
        arg.vector_size = optarg->vector_size;
      }
      arg.scorer = optarg->scorer;
      arg.scorer_args_expr = optarg->scorer_args_expr;
      arg.scorer_args_expr_offset = optarg->scorer_args_expr_offset;
    }
    /* todo : support subrec
    grn_rset_init(ctx, s, grn_rec_document, 0, grn_rec_none, 0, 0);
    */
    if (grn_ii_select(ctx, ii, string, string_len, s, op, &arg)) {
      GRN_LOG(ctx, GRN_LOG_ERROR, "grn_ii_select on grn_ii_sel(1) failed !");
      return ctx->rc;
    }
    GRN_LOG(ctx, GRN_LOG_INFO, "exact: %d", GRN_HASH_SIZE(s));
    if (op == GRN_OP_OR) {
      if ((int64_t)GRN_HASH_SIZE(s) <= ctx->impl->match_escalation_threshold) {
        arg.mode = GRN_OP_UNSPLIT;
        if (grn_ii_select(ctx, ii, string, string_len, s, op, &arg)) {
          GRN_LOG(ctx, GRN_LOG_ERROR, "grn_ii_select on grn_ii_sel(2) failed !");
          return ctx->rc;
        }
        GRN_LOG(ctx, GRN_LOG_INFO, "unsplit: %d", GRN_HASH_SIZE(s));
      }
      if ((int64_t)GRN_HASH_SIZE(s) <= ctx->impl->match_escalation_threshold) {
        arg.mode = GRN_OP_PARTIAL;
        if (grn_ii_select(ctx, ii, string, string_len, s, op, &arg)) {
          GRN_LOG(ctx, GRN_LOG_ERROR, "grn_ii_select on grn_ii_sel(3) failed !");
          return ctx->rc;
        }
        GRN_LOG(ctx, GRN_LOG_INFO, "partial: %d", GRN_HASH_SIZE(s));
      }
    }
    GRN_LOG(ctx, GRN_LOG_INFO, "hits=%d", GRN_HASH_SIZE(s));
    return GRN_SUCCESS;
  }
}

grn_rc
grn_ii_at(grn_ctx *ctx, grn_ii *ii, grn_id id, grn_hash *s, grn_operator op)
{
  int rep = 0;
  grn_ii_cursor *c;
  grn_ii_posting *pos;
  if ((c = grn_ii_cursor_open(ctx, ii, id, GRN_ID_NIL, GRN_ID_MAX,
                              rep ? ii->n_elements : ii->n_elements - 1, 0))) {
    while ((pos = grn_ii_cursor_next(ctx, c))) {
      res_add(ctx, s, (grn_rset_posinfo *) pos, (1 + pos->weight), op);
    }
    grn_ii_cursor_close(ctx, c);
  }
  return ctx->rc;
}

void
grn_ii_resolve_sel_and(grn_ctx *ctx, grn_hash *s, grn_operator op)
{
  if (op == GRN_OP_AND
      && !(ctx->flags & GRN_CTX_TEMPORARY_DISABLE_II_RESOLVE_SEL_AND)) {
    grn_id eid;
    grn_rset_recinfo *ri;
    grn_hash_cursor *c = grn_hash_cursor_open(ctx, s, NULL, 0, NULL, 0, 0, -1, 0);
    if (c) {
      while ((eid = grn_hash_cursor_next(ctx, c))) {
        grn_hash_cursor_get_value(ctx, c, (void **) &ri);
        if ((ri->n_subrecs & GRN_RSET_UTIL_BIT)) {
          ri->n_subrecs &= ~GRN_RSET_UTIL_BIT;
        } else {
          grn_hash_delete_by_id(ctx, s, eid, NULL);
        }
      }
      grn_hash_cursor_close(ctx, c);
    }
  }
}

/* just for inspect */
static grn_ii_posting *
grn_ii_cursor_next_all(grn_ctx *ctx, grn_ii_cursor *c)
{
  if (c->buf) {
    for (;;) {
      if (c->stat & CHUNK_USED) {
        for (;;) {
          if (c->crp < c->cdp + c->cdf) {
            uint32_t dgap = *c->crp++;
            c->pc.rid += dgap;
            if (dgap) { c->pc.sid = 0; }
            if ((c->ii->header->flags & GRN_OBJ_WITH_SECTION)) {
              c->pc.sid += 1 + *c->csp++;
            } else {
              c->pc.sid = 1;
            }
            c->cpp += c->pc.rest;
            c->pc.rest = c->pc.tf = 1 + *c->ctp++;
            if ((c->ii->header->flags & GRN_OBJ_WITH_WEIGHT)) {
              c->pc.weight = *c->cwp++;
            } else {
              c->pc.weight = 0;
            }
            c->pc.pos = 0;
            /*
            {
              static int count = 0;
              int tf = c->pc.tf, pos = 0, *pp = (int *)c->cpp;
              grn_obj buf;
              GRN_TEXT_INIT(&buf, 0);
              grn_text_itoa(ctx, &buf, c->pc.rid);
              GRN_TEXT_PUTC(ctx, &buf, ':');
              grn_text_itoa(ctx, &buf, c->pc.sid);
              GRN_TEXT_PUTC(ctx, &buf, ':');
              grn_text_itoa(ctx, &buf, c->pc.tf);
              GRN_TEXT_PUTC(ctx, &buf, '(');
              while (tf--) {
                pos += *pp++;
                count++;
                grn_text_itoa(ctx, &buf, pos);
                if (tf) { GRN_TEXT_PUTC(ctx, &buf, ':'); }
              }
              GRN_TEXT_PUTC(ctx, &buf, ')');
              GRN_TEXT_PUTC(ctx, &buf, '\0');
              GRN_LOG(ctx, GRN_LOG_NOTICE, "posting(%d):%s", count, GRN_TEXT_VALUE(&buf));
              GRN_OBJ_FIN(ctx, &buf);
            }
            */
          } else {
            if (c->curr_chunk <= c->nchunks) {
              if (c->curr_chunk == c->nchunks) {
                if (c->cp < c->cpe) {
                  grn_p_decv(ctx, c->cp, c->cpe - c->cp, c->rdv, c->ii->n_elements);
                } else {
                  c->pc.rid = 0;
                  break;
                }
              } else {
                uint8_t *cp;
                grn_io_win iw;
                uint32_t size = c->cinfo[c->curr_chunk].size;
                if (size && (cp = WIN_MAP(c->ii->chunk, ctx, &iw,
                                          c->cinfo[c->curr_chunk].segno, 0,
                                          size, grn_io_rdonly))) {
                  grn_p_decv(ctx, cp, size, c->rdv, c->ii->n_elements);
                  grn_io_win_unmap(&iw);
                } else {
                  c->pc.rid = 0;
                  break;
                }
              }
              {
                int j = 0;
                c->cdf = c->rdv[j].data_size;
                c->crp = c->cdp = c->rdv[j++].data;
                if ((c->ii->header->flags & GRN_OBJ_WITH_SECTION)) {
                  c->csp = c->rdv[j++].data;
                }
                c->ctp = c->rdv[j++].data;
                if ((c->ii->header->flags & GRN_OBJ_WITH_WEIGHT)) {
                  c->cwp = c->rdv[j++].data;
                }
                c->cpp = c->rdv[j].data;
              }
              c->pc.rid = 0;
              c->pc.sid = 0;
              c->pc.rest = 0;
              c->curr_chunk++;
              continue;
            } else {
              c->pc.rid = 0;
            }
          }
          break;
        }
      }
      if (c->stat & BUFFER_USED) {
        if (c->nextb) {
          uint32_t lrid = c->pb.rid, lsid = c->pb.sid; /* for check */
          buffer_rec *br = BUFFER_REC_AT(c->buf, c->nextb);
          if (buffer_is_reused(ctx, c->ii, c)) {
            GRN_LOG(ctx, GRN_LOG_NOTICE, "buffer reused(%d,%d)", c->buffer_pseg, *c->ppseg);
            // todo : rewind;
          }
          c->bp = NEXT_ADDR(br);
          GRN_B_DEC(c->pb.rid, c->bp);
          if ((c->ii->header->flags & GRN_OBJ_WITH_SECTION)) {
            GRN_B_DEC(c->pb.sid, c->bp);
          } else {
            c->pb.sid = 1;
          }
          if (lrid > c->pb.rid || (lrid == c->pb.rid && lsid >= c->pb.sid)) {
            ERR(GRN_FILE_CORRUPT, "brokend!! (%d:%d) -> (%d:%d) (%d->%d)", lrid, lsid, c->pb.rid, c->pb.sid, c->buffer_pseg, *c->ppseg);
          }
          c->nextb = br->step;
          GRN_B_DEC(c->pb.tf, c->bp);
          if ((c->ii->header->flags & GRN_OBJ_WITH_WEIGHT)) {
            GRN_B_DEC(c->pb.weight, c->bp);
          } else {
            c->pb.weight = 0;
          }
          c->pb.rest = c->pb.tf;
          c->pb.pos = 0;
        } else {
          c->pb.rid = 0;
        }
      }
      if (c->pb.rid) {
        if (c->pc.rid) {
          if (c->pc.rid < c->pb.rid) {
            c->stat = CHUNK_USED;
            c->post = &c->pc;
            break;
          } else {
            if (c->pb.rid < c->pc.rid) {
              c->stat = BUFFER_USED;
              c->post = &c->pb;
              break;
            } else {
              if (c->pb.sid) {
                if (c->pc.sid < c->pb.sid) {
                  c->stat = CHUNK_USED;
                  c->post = &c->pc;
                  break;
                } else {
                  c->stat = BUFFER_USED;
                  if (c->pb.sid == c->pc.sid) { c->stat |= CHUNK_USED; }
                  c->post = &c->pb;
                  break;
                }
              } else {
                c->stat = CHUNK_USED;
              }
            }
          }
        } else {
          c->stat = BUFFER_USED;
          c->post = &c->pb;
          break;
        }
      } else {
        if (c->pc.rid) {
          c->stat = CHUNK_USED;
          c->post = &c->pc;
          break;
        } else {
          c->post = NULL;
          return NULL;
        }
      }
    }
  } else {
    if (c->stat & SOLE_DOC_USED) {
      c->post = NULL;
      return NULL;
    } else {
      c->post = &c->pb;
      c->stat |= SOLE_DOC_USED;
    }
  }
  return c->post;
}

void
grn_ii_cursor_inspect(grn_ctx *ctx, grn_ii_cursor *c, grn_obj *buf)
{
  grn_obj key_buf;
  char key[GRN_TABLE_MAX_KEY_SIZE];
  int key_size;
  int i = 0;

  GRN_TEXT_PUTS(ctx, buf, "  #<");
  key_size = grn_table_get_key(ctx, c->ii->lexicon, c->id,
                               key, GRN_TABLE_MAX_KEY_SIZE);
  GRN_OBJ_INIT(&key_buf, GRN_BULK, 0, c->ii->lexicon->header.domain);
  GRN_TEXT_SET(ctx, &key_buf, key, key_size);
  grn_inspect(ctx, buf, &key_buf);
  GRN_OBJ_FIN(ctx, &key_buf);

  GRN_TEXT_PUTS(ctx, buf, "\n    elements:[\n      ");
  while (grn_ii_cursor_next_all(ctx, c)) {
    grn_ii_posting *pos = c->post;
    if (i > 0) {
      GRN_TEXT_PUTS(ctx, buf, ",\n      ");
    }
    i++;
    GRN_TEXT_PUTS(ctx, buf, "{status:");
    if (pos->tf && pos->sid) {
      GRN_TEXT_PUTS(ctx, buf, "available");
    } else {
      GRN_TEXT_PUTS(ctx, buf, "garbage");
    }
    GRN_TEXT_PUTS(ctx, buf, ", rid:");
    grn_text_lltoa(ctx, buf, pos->rid);
    GRN_TEXT_PUTS(ctx, buf, ", sid:");
    grn_text_lltoa(ctx, buf, pos->sid);
    GRN_TEXT_PUTS(ctx, buf, ", pos:");
    grn_text_lltoa(ctx, buf, pos->pos);
    GRN_TEXT_PUTS(ctx, buf, ", tf:");
    grn_text_lltoa(ctx, buf, pos->tf);
    GRN_TEXT_PUTS(ctx, buf, ", weight:");
    grn_text_lltoa(ctx, buf, pos->weight);
    GRN_TEXT_PUTS(ctx, buf, ", rest:");
    grn_text_lltoa(ctx, buf, pos->rest);
    GRN_TEXT_PUTS(ctx, buf, "}");
  }
  GRN_TEXT_PUTS(ctx, buf, "\n    ]\n  >");
}

void
grn_ii_inspect_values(grn_ctx *ctx, grn_ii *ii, grn_obj *buf)
{
  grn_table_cursor *tc;
  GRN_TEXT_PUTS(ctx, buf, "[");
  if ((tc = grn_table_cursor_open(ctx, ii->lexicon, NULL, 0, NULL, 0, 0, -1,
                                  GRN_CURSOR_ASCENDING))) {
    int i = 0;
    grn_id tid;
    grn_ii_cursor *c;
    while ((tid = grn_table_cursor_next(ctx, tc))) {
      if (i > 0) {
        GRN_TEXT_PUTS(ctx, buf, ",");
      }
      i++;
      GRN_TEXT_PUTS(ctx, buf, "\n");
      if ((c = grn_ii_cursor_open(ctx, ii, tid, GRN_ID_NIL, GRN_ID_MAX,
                                  ii->n_elements,
                                  GRN_OBJ_WITH_POSITION|GRN_OBJ_WITH_SECTION))) {
        grn_ii_cursor_inspect(ctx, c, buf);
        grn_ii_cursor_close(ctx, c);
      }
    }
    grn_table_cursor_close(ctx, tc);
  }
  GRN_TEXT_PUTS(ctx, buf, "]");
}

/********************** buffered index builder ***********************/

const grn_id II_BUFFER_TYPE_MASK = 0xc0000000;
#define II_BUFFER_TYPE_RID         0x80000000
#define II_BUFFER_TYPE_WEIGHT      0x40000000
#define II_BUFFER_TYPE(id)          (((id) & II_BUFFER_TYPE_MASK))
#define II_BUFFER_PACK(value, type) ((value) | (type))
#define II_BUFFER_UNPACK(id, type)  ((id) & ~(type))
#ifdef II_BUFFER_ORDER_BY_ID
const int II_BUFFER_ORDER = GRN_CURSOR_BY_ID;
#else /* II_BUFFER_ORDER_BY_ID */
const int II_BUFFER_ORDER = GRN_CURSOR_BY_KEY;
#endif /* II_BUFFER_ORDER_BY_ID */
const uint16_t II_BUFFER_NTERMS_PER_BUFFER = 16380;
const uint32_t II_BUFFER_PACKED_BUF_SIZE = 0x4000000;
const char *TMPFILE_PATH = "grn_ii_buffer_tmp";
const uint32_t II_BUFFER_NCOUNTERS_MARGIN = 0x100000;
const size_t II_BUFFER_BLOCK_SIZE = 0x1000000;
const uint32_t II_BUFFER_BLOCK_READ_UNIT_SIZE = 0x200000;

typedef struct {
  uint32_t nrecs;
  uint32_t nposts;
  grn_id last_rid;
  uint32_t last_sid;
  uint32_t last_tf;
  uint32_t last_weight;
  uint32_t last_pos;
  uint32_t offset_rid;
  uint32_t offset_sid;
  uint32_t offset_tf;
  uint32_t offset_weight;
  uint32_t offset_pos;
} ii_buffer_counter;

typedef struct {
  off64_t head;
  off64_t tail;
  uint32_t nextsize;
  uint8_t *buffer;
  uint32_t buffersize;
  uint8_t *bufcur;
  uint32_t rest;
  grn_id tid;
  uint32_t nrecs;
  uint32_t nposts;
  grn_id *recs;
  uint32_t *tfs;
  uint32_t *posts;
} ii_buffer_block;

struct _grn_ii_buffer {
  grn_obj *lexicon;
  grn_obj *tmp_lexicon;
  ii_buffer_block *blocks;
  uint32_t nblocks;
  int tmpfd;
  char tmpfpath[PATH_MAX];
  uint64_t update_buffer_size;
  // stuff for parsing
  off64_t filepos;
  grn_id *block_buf;
  size_t block_buf_size;
  size_t block_pos;
  ii_buffer_counter *counters;
  uint32_t ncounters;
  size_t total_size;
  size_t curr_size;
  // stuff for merging
  grn_ii *ii;
  uint32_t lseg;
  uint32_t dseg;
  buffer *term_buffer;
  datavec data_vectors[MAX_N_ELEMENTS + 1];
  uint8_t *packed_buf;
  size_t packed_buf_size;
  size_t packed_len;
  size_t total_chunk_size;
};

static ii_buffer_block *
block_new(grn_ctx *ctx, grn_ii_buffer *ii_buffer)
{
  ii_buffer_block *block;
  if (!(ii_buffer->nblocks & 0x3ff)) {
    ii_buffer_block *blocks;
    if (!(blocks = GRN_REALLOC(ii_buffer->blocks,
                               (ii_buffer->nblocks + 0x400) *
                               sizeof(ii_buffer_block)))) {
      return NULL;
    }
    ii_buffer->blocks = blocks;
  }
  block = &ii_buffer->blocks[ii_buffer->nblocks];
  block->head = ii_buffer->filepos;
  block->rest = 0;
  block->buffer = NULL;
  block->buffersize = 0;
  return block;
}

static uint8_t *
allocate_outbuf(grn_ctx *ctx, grn_ii_buffer *ii_buffer)
{
  size_t bufsize = 0, bufsize_ = 0;
  uint32_t flags = ii_buffer->ii->header->flags;
  ii_buffer_counter *counter = ii_buffer->counters;
  grn_id tid, tid_max = grn_table_size(ctx, ii_buffer->tmp_lexicon);
  for (tid = 1; tid <= tid_max; counter++, tid++) {
    counter->offset_tf += GRN_B_ENC_SIZE(counter->last_tf - 1);
    counter->last_rid = 0;
    counter->last_tf = 0;
    bufsize += 5;
    bufsize += GRN_B_ENC_SIZE(counter->nrecs);
    bufsize += GRN_B_ENC_SIZE(counter->nposts);
    bufsize += counter->offset_rid;
    if ((flags & GRN_OBJ_WITH_SECTION)) {
      bufsize += counter->offset_sid;
    }
    bufsize += counter->offset_tf;
    if ((flags & GRN_OBJ_WITH_WEIGHT)) {
      bufsize += counter->offset_weight;
    }
    if ((flags & GRN_OBJ_WITH_POSITION)) {
      bufsize += counter->offset_pos;
    }
    if (bufsize_ + II_BUFFER_BLOCK_READ_UNIT_SIZE < bufsize) {
      bufsize += sizeof(uint32_t);
      bufsize_ = bufsize;
    }
  }
  GRN_LOG(ctx, GRN_LOG_INFO, "flushing:%d bufsize:%" GRN_FMT_SIZE,
          ii_buffer->nblocks, bufsize);
  return (uint8_t *)GRN_MALLOC(bufsize);
}

static size_t
encode_terms(grn_ctx *ctx, grn_ii_buffer *ii_buffer,
             uint8_t *outbuf, ii_buffer_block *block)
{
  grn_id tid;
  uint8_t *outbufp = outbuf;
  uint8_t *outbufp_ = outbuf;
  grn_table_cursor  *tc;
  uint8_t *pnext = (uint8_t *)&block->nextsize;
  uint32_t flags = ii_buffer->ii->header->flags;
  tc = grn_table_cursor_open(ctx, ii_buffer->tmp_lexicon,
                             NULL, 0, NULL, 0, 0, -1, II_BUFFER_ORDER);
  while ((tid = grn_table_cursor_next(ctx, tc)) != GRN_ID_NIL) {
    char key[GRN_TABLE_MAX_KEY_SIZE];
    int key_size = grn_table_get_key(ctx, ii_buffer->tmp_lexicon, tid,
                                     key, GRN_TABLE_MAX_KEY_SIZE);
    grn_id gtid = grn_table_add(ctx, ii_buffer->lexicon, key, key_size, NULL);
    ii_buffer_counter *counter = &ii_buffer->counters[tid - 1];
    if (counter->nrecs) {
      uint32_t offset_rid = counter->offset_rid;
      uint32_t offset_sid = counter->offset_sid;
      uint32_t offset_tf = counter->offset_tf;
      uint32_t offset_weight = counter->offset_weight;
      uint32_t offset_pos = counter->offset_pos;
      GRN_B_ENC(gtid, outbufp);
      GRN_B_ENC(counter->nrecs, outbufp);
      GRN_B_ENC(counter->nposts, outbufp);
      ii_buffer->total_size += counter->nrecs + counter->nposts;
      counter->offset_rid = outbufp - outbuf;
      outbufp += offset_rid;
      if ((flags & GRN_OBJ_WITH_SECTION)) {
        counter->offset_sid = outbufp - outbuf;
        outbufp += offset_sid;
      }
      counter->offset_tf = outbufp - outbuf;
      outbufp += offset_tf;
      if ((flags & GRN_OBJ_WITH_WEIGHT)) {
        counter->offset_weight = outbufp - outbuf;
        outbufp += offset_weight;
      }
      if ((flags & GRN_OBJ_WITH_POSITION)) {
        counter->offset_pos = outbufp - outbuf;
        outbufp += offset_pos;
      }
    }
    if (outbufp_ + II_BUFFER_BLOCK_READ_UNIT_SIZE < outbufp) {
      uint32_t size = outbufp - outbufp_ + sizeof(uint32_t);
      grn_memcpy(pnext, &size, sizeof(uint32_t));
      pnext = outbufp;
      outbufp += sizeof(uint32_t);
      outbufp_ = outbufp;
    }
  }
  grn_table_cursor_close(ctx, tc);
  if (outbufp_ < outbufp) {
    uint32_t size = outbufp - outbufp_;
    grn_memcpy(pnext, &size, sizeof(uint32_t));
  }
  return outbufp - outbuf;
}

static void
encode_postings(grn_ctx *ctx, grn_ii_buffer *ii_buffer, uint8_t *outbuf)
{
  grn_id rid = 0;
  unsigned int sid = 1;
  unsigned int weight = 0;
  uint32_t pos = 0;
  uint32_t rest;
  grn_id *bp = ii_buffer->block_buf;
  uint32_t flags = ii_buffer->ii->header->flags;
  for (rest = ii_buffer->block_pos; rest; bp++, rest--) {
    grn_id id = *bp;
    switch (II_BUFFER_TYPE(id)) {
    case II_BUFFER_TYPE_RID :
      rid = II_BUFFER_UNPACK(id, II_BUFFER_TYPE_RID);
      if ((flags & GRN_OBJ_WITH_SECTION) && rest) {
        sid = *++bp;
        rest--;
      }
      weight = 0;
      pos = 0;
      break;
    case II_BUFFER_TYPE_WEIGHT :
      weight = II_BUFFER_UNPACK(id, II_BUFFER_TYPE_WEIGHT);
      break;
    default :
      {
        ii_buffer_counter *counter = &ii_buffer->counters[id - 1];
        if (counter->last_rid == rid && counter->last_sid == sid) {
          counter->last_tf++;
          counter->last_weight += weight;
        } else {
          if (counter->last_tf) {
            uint8_t *p = outbuf + counter->offset_tf;
            GRN_B_ENC(counter->last_tf - 1, p);
            counter->offset_tf = p - outbuf;
            if (flags & GRN_OBJ_WITH_WEIGHT) {
              p = outbuf + counter->offset_weight;
              GRN_B_ENC(counter->last_weight, p);
              counter->offset_weight = p - outbuf;
            }
          }
          {
            uint8_t *p = outbuf + counter->offset_rid;
            GRN_B_ENC(rid - counter->last_rid, p);
            counter->offset_rid = p - outbuf;
          }
          if (flags & GRN_OBJ_WITH_SECTION) {
            uint8_t *p = outbuf + counter->offset_sid;
            if (counter->last_rid != rid) {
              GRN_B_ENC(sid - 1, p);
            } else {
              GRN_B_ENC(sid - counter->last_sid - 1, p);
            }
            counter->offset_sid = p - outbuf;
          }
          counter->last_rid = rid;
          counter->last_sid = sid;
          counter->last_tf = 1;
          counter->last_weight = weight;
          counter->last_pos = 0;
        }
        if ((flags & GRN_OBJ_WITH_POSITION) && rest) {
          uint8_t *p = outbuf + counter->offset_pos;
          pos = *++bp;
          rest--;
          GRN_B_ENC(pos - counter->last_pos, p);
          counter->offset_pos = p - outbuf;
          counter->last_pos = pos;
        }
      }
      break;
    }
  }
}

static void
encode_last_tf(grn_ctx *ctx, grn_ii_buffer *ii_buffer, uint8_t *outbuf)
{
  ii_buffer_counter *counter = ii_buffer->counters;
  grn_id tid, tid_max = grn_table_size(ctx, ii_buffer->tmp_lexicon);
  for (tid = 1; tid <= tid_max; counter++, tid++) {
    uint8_t *p = outbuf + counter->offset_tf;
    GRN_B_ENC(counter->last_tf - 1, p);
  }
  if ((ii_buffer->ii->header->flags & GRN_OBJ_WITH_WEIGHT)) {
    for (tid = 1; tid <= tid_max; counter++, tid++) {
      uint8_t *p = outbuf + counter->offset_weight;
      GRN_B_ENC(counter->last_weight, p);
    }
  }
}

static void
grn_ii_buffer_flush(grn_ctx *ctx, grn_ii_buffer *ii_buffer)
{
  size_t encsize;
  uint8_t *outbuf;
  ii_buffer_block *block;
  GRN_LOG(ctx, GRN_LOG_NOTICE, "flushing:%d npostings:%" GRN_FMT_SIZE,
          ii_buffer->nblocks, ii_buffer->block_pos);
  if (!(block = block_new(ctx, ii_buffer))) { return; }
  if (!(outbuf = allocate_outbuf(ctx, ii_buffer))) { return; }
  encsize = encode_terms(ctx, ii_buffer, outbuf, block);
  encode_postings(ctx, ii_buffer, outbuf);
  encode_last_tf(ctx, ii_buffer, outbuf);
  {
    ssize_t r = grn_write(ii_buffer->tmpfd, outbuf, encsize);
    if (r != encsize) {
      ERR(GRN_INPUT_OUTPUT_ERROR, "write returned %" GRN_FMT_LLD " != %" GRN_FMT_LLU,
          (long long int)r, (unsigned long long int)encsize);
      return;
    }
    ii_buffer->filepos += r;
    block->tail = ii_buffer->filepos;
  }
  GRN_FREE(outbuf);
  memset(ii_buffer->counters, 0,
         grn_table_size(ctx, ii_buffer->tmp_lexicon) *
         sizeof(ii_buffer_counter));
  grn_obj_close(ctx, ii_buffer->tmp_lexicon);
  GRN_LOG(ctx, GRN_LOG_NOTICE, "flushed: %d encsize:%" GRN_FMT_SIZE,
          ii_buffer->nblocks, encsize);
  ii_buffer->tmp_lexicon = NULL;
  ii_buffer->nblocks++;
  ii_buffer->block_pos = 0;
}

const uint32_t PAT_CACHE_SIZE = 1<<20;

static grn_obj *
get_tmp_lexicon(grn_ctx *ctx, grn_ii_buffer *ii_buffer)
{
  grn_obj *tmp_lexicon = ii_buffer->tmp_lexicon;
  if (!tmp_lexicon) {
    grn_obj *domain = grn_ctx_at(ctx, ii_buffer->lexicon->header.domain);
    grn_obj *range = grn_ctx_at(ctx, DB_OBJ(ii_buffer->lexicon)->range);
    grn_obj *tokenizer;
    grn_obj *normalizer;
    grn_obj *token_filters;
    grn_obj_flags flags;
    grn_table_get_info(ctx, ii_buffer->lexicon, &flags, NULL,
                       &tokenizer, &normalizer, &token_filters);
    flags &= ~GRN_OBJ_PERSISTENT;
    tmp_lexicon = grn_table_create(ctx, NULL, 0, NULL, flags, domain, range);
    if (tmp_lexicon) {
      ii_buffer->tmp_lexicon = tmp_lexicon;
      grn_obj_set_info(ctx, tmp_lexicon,
                       GRN_INFO_DEFAULT_TOKENIZER, tokenizer);
      grn_obj_set_info(ctx, tmp_lexicon,
                       GRN_INFO_NORMALIZER, normalizer);
      grn_obj_set_info(ctx, tmp_lexicon,
                       GRN_INFO_TOKEN_FILTERS, token_filters);
      if ((flags & GRN_OBJ_TABLE_TYPE_MASK) == GRN_OBJ_TABLE_PAT_KEY) {
        grn_pat_cache_enable(ctx, (grn_pat *)tmp_lexicon, PAT_CACHE_SIZE);
      }
    }
  }
  return tmp_lexicon;
}

static ii_buffer_counter *
get_buffer_counter(grn_ctx *ctx, grn_ii_buffer *ii_buffer,
                   grn_obj *tmp_lexicon, grn_id tid)
{
  if (tid > ii_buffer->ncounters) {
    ii_buffer_counter *counters;
    uint32_t ncounters =
      grn_table_size(ctx, tmp_lexicon) + II_BUFFER_NCOUNTERS_MARGIN;
    counters = GRN_REALLOC(ii_buffer->counters,
                           ncounters * sizeof(ii_buffer_counter));
    if (!counters) { return NULL; }
    memset(&counters[ii_buffer->ncounters], 0,
           (ncounters - ii_buffer->ncounters) * sizeof(ii_buffer_counter));
    ii_buffer->ncounters = ncounters;
    ii_buffer->counters = counters;
  }
  return &ii_buffer->counters[tid - 1];
}

static void
grn_ii_buffer_tokenize(grn_ctx *ctx, grn_ii_buffer *ii_buffer, grn_id rid,
                       unsigned int sid, unsigned int weight,
                       const char *value, uint32_t value_len)
{
  if (value_len) {
    grn_obj *tmp_lexicon;
    uint32_t est_len = value_len * 2 + 2;
    if (ii_buffer->block_buf_size < ii_buffer->block_pos + est_len) {
      grn_ii_buffer_flush(ctx, ii_buffer);
    }
    if (ii_buffer->block_buf_size < est_len) {
      grn_id *block_buf = (grn_id *)GRN_REALLOC(ii_buffer->block_buf,
                                                est_len * sizeof(grn_id));
      if (!block_buf) { return; }
      ii_buffer->block_buf = block_buf;
      ii_buffer->block_buf_size = est_len;
    }
    if ((tmp_lexicon = get_tmp_lexicon(ctx, ii_buffer))) {
      unsigned int token_flags = 0;
      grn_token_cursor *token_cursor;
      grn_id *buffer = ii_buffer->block_buf;
      uint32_t block_pos = ii_buffer->block_pos;
      uint32_t ii_flags = ii_buffer->ii->header->flags;
      buffer[block_pos++] = II_BUFFER_PACK(rid, II_BUFFER_TYPE_RID);
      if (ii_flags & GRN_OBJ_WITH_SECTION) {
        buffer[block_pos++] = sid;
      }
      if (weight) {
        buffer[block_pos++] = II_BUFFER_PACK(weight, II_BUFFER_TYPE_WEIGHT);
      }
      if ((token_cursor = grn_token_cursor_open(ctx, tmp_lexicon,
                                                value, value_len,
                                                GRN_TOKEN_ADD, token_flags))) {
        while (!token_cursor->status) {
          grn_id tid;
          if ((tid = grn_token_cursor_next(ctx, token_cursor))) {
            ii_buffer_counter *counter;
            counter = get_buffer_counter(ctx, ii_buffer, tmp_lexicon, tid);
            if (!counter) { return; }
            buffer[block_pos++] = tid;
            if (ii_flags & GRN_OBJ_WITH_POSITION) {
              buffer[block_pos++] = token_cursor->pos;
            }
            if (counter->last_rid != rid) {
              counter->offset_rid += GRN_B_ENC_SIZE(rid - counter->last_rid);
              counter->last_rid = rid;
              counter->offset_sid += GRN_B_ENC_SIZE(sid - 1);
              counter->last_sid = sid;
              if (counter->last_tf) {
                counter->offset_tf += GRN_B_ENC_SIZE(counter->last_tf - 1);
                counter->last_tf = 0;
                counter->offset_weight += GRN_B_ENC_SIZE(counter->last_weight);
                counter->last_weight = 0;
              }
              counter->last_pos = 0;
              counter->nrecs++;
            } else if (counter->last_sid != sid) {
              counter->offset_rid += GRN_B_ENC_SIZE(0);
              counter->offset_sid +=
                GRN_B_ENC_SIZE(sid - counter->last_sid - 1);
              counter->last_sid = sid;
              if (counter->last_tf) {
                counter->offset_tf += GRN_B_ENC_SIZE(counter->last_tf - 1);
                counter->last_tf = 0;
                counter->offset_weight += GRN_B_ENC_SIZE(counter->last_weight);
                counter->last_weight = 0;
              }
              counter->last_pos = 0;
              counter->nrecs++;
            }
            counter->offset_pos +=
              GRN_B_ENC_SIZE(token_cursor->pos - counter->last_pos);
            counter->last_pos = token_cursor->pos;
            counter->last_tf++;
            counter->last_weight += weight;
            counter->nposts++;
          }
        }
        grn_token_cursor_close(ctx, token_cursor);
      }
      ii_buffer->block_pos = block_pos;
    }
  }
}

static void
grn_ii_buffer_fetch(grn_ctx *ctx, grn_ii_buffer *ii_buffer,
                    ii_buffer_block *block)
{
  if (!block->rest) {
    if (block->head < block->tail) {
      size_t bytesize = block->nextsize;
      if (block->buffersize < block->nextsize) {
        void *r = GRN_REALLOC(block->buffer, bytesize);
        if (r) {
          block->buffer = (uint8_t *)r;
          block->buffersize = block->nextsize;
        } else {
          GRN_LOG(ctx, GRN_LOG_WARNING, "realloc: %" GRN_FMT_LLU,
                  (unsigned long long int)bytesize);
          return;
        }
      }
      {
        off64_t seeked_position;
        seeked_position = grn_lseek(ii_buffer->tmpfd, block->head, SEEK_SET);
        if (seeked_position != block->head) {
          ERRNO_ERR("grn_lseek");
          GRN_LOG(ctx, GRN_LOG_ERROR,
                  "failed to "
                  "grn_lseek(%" GRN_FMT_OFF64_T ") -> %" GRN_FMT_OFF64_T,
                  block->head,
                  seeked_position);
          return;
        }
      }
      if (grn_read(ii_buffer->tmpfd, block->buffer, bytesize) != bytesize) {
        SERR("read");
        return;
      }
      block->head += bytesize;
      block->bufcur = block->buffer;
      if (block->head >= block->tail) {
        if (block->head > block->tail) {
          GRN_LOG(ctx, GRN_LOG_WARNING,
                  "fetch error: %" GRN_FMT_INT64D " > %" GRN_FMT_INT64D,
                  block->head, block->tail);
        }
        block->rest = block->nextsize;
        block->nextsize = 0;
      } else {
        block->rest = block->nextsize - sizeof(uint32_t);
        grn_memcpy(&block->nextsize,
                   &block->buffer[block->rest], sizeof(uint32_t));
      }
    }
  }
  if (block->rest) {
    uint8_t *p = block->bufcur;
    GRN_B_DEC(block->tid, p);
    GRN_B_DEC(block->nrecs, p);
    GRN_B_DEC(block->nposts, p);
    block->rest -= (p - block->bufcur);
    block->bufcur = p;
  } else {
    block->tid = 0;
  }
}

static void
grn_ii_buffer_chunk_flush(grn_ctx *ctx, grn_ii_buffer *ii_buffer)
{
  grn_io_win io_win;
  uint32_t chunk_number;
  chunk_new(ctx, ii_buffer->ii, &chunk_number, ii_buffer->packed_len);
  GRN_LOG(ctx, GRN_LOG_INFO, "chunk:%d, packed_len:%" GRN_FMT_SIZE,
          chunk_number, ii_buffer->packed_len);
  fake_map(ctx, ii_buffer->ii->chunk, &io_win, ii_buffer->packed_buf,
           chunk_number, ii_buffer->packed_len);
  grn_io_win_unmap(&io_win);
  ii_buffer->term_buffer->header.chunk = chunk_number;
  ii_buffer->term_buffer->header.chunk_size = ii_buffer->packed_len;
  ii_buffer->term_buffer->header.buffer_free =
    S_SEGMENT - sizeof(buffer_header) -
    ii_buffer->term_buffer->header.nterms * sizeof(buffer_term);
  ii_buffer->term_buffer->header.nterms_void = 0;
  buffer_segment_update(ii_buffer->ii, ii_buffer->lseg, ii_buffer->dseg);
  ii_buffer->ii->header->total_chunk_size += ii_buffer->packed_len;
  ii_buffer->total_chunk_size += ii_buffer->packed_len;
  GRN_LOG(ctx, GRN_LOG_NOTICE,
          "nterms=%d chunk=%d total=%" GRN_FMT_INT64U "KB",
          ii_buffer->term_buffer->header.nterms,
          ii_buffer->term_buffer->header.chunk_size,
          ii_buffer->ii->header->total_chunk_size >> 10);
  ii_buffer->term_buffer = NULL;
  ii_buffer->packed_buf = NULL;
  ii_buffer->packed_len = 0;
  ii_buffer->packed_buf_size = 0;
  ii_buffer->curr_size = 0;
}

static size_t
merge_hit_blocks(grn_ctx *ctx, grn_ii_buffer *ii_buffer,
                 ii_buffer_block *hits[], int nhits)
{
  uint64_t nrecs = 0;
  uint64_t nposts = 0;
  size_t max_size;
  uint64_t flags = ii_buffer->ii->header->flags;
  int i;
  for (i = 0; i < nhits; i++) {
    ii_buffer_block *block = hits[i];
    nrecs += block->nrecs;
    nposts += block->nposts;
  }
  ii_buffer->curr_size += nrecs + nposts;
  max_size = nrecs * (ii_buffer->ii->n_elements);
  if (flags & GRN_OBJ_WITH_POSITION) { max_size += nposts - nrecs; }
  datavec_reset(ctx, ii_buffer->data_vectors,
                ii_buffer->ii->n_elements, nrecs, max_size);
  {
    int i;
    uint32_t lr = 0;
    uint64_t spos = 0;
    uint32_t *ridp, *sidp = NULL, *tfp, *weightp = NULL, *posp = NULL;
    {
      int j = 0;
      ridp = ii_buffer->data_vectors[j++].data;
      if (flags & GRN_OBJ_WITH_SECTION) {
        sidp = ii_buffer->data_vectors[j++].data;
      }
      tfp = ii_buffer->data_vectors[j++].data;
      if (flags & GRN_OBJ_WITH_WEIGHT) {
        weightp = ii_buffer->data_vectors[j++].data;
      }
      if (flags & GRN_OBJ_WITH_POSITION) {
        posp = ii_buffer->data_vectors[j++].data;
      }
    }
    for (i = 0; i < nhits; i++) {
      ii_buffer_block *block = hits[i];
      uint8_t *p = block->bufcur;
      uint32_t n = block->nrecs;
      if (n) {
        GRN_B_DEC(*ridp, p);
        *ridp -= lr;
        lr += *ridp++;
        while (--n) {
          GRN_B_DEC(*ridp, p);
          lr += *ridp++;
        }
      }
      if ((flags & GRN_OBJ_WITH_SECTION)) {
        for (n = block->nrecs; n; n--) {
          GRN_B_DEC(*sidp++, p);
        }
      }
      for (n = block->nrecs; n; n--) {
        GRN_B_DEC(*tfp++, p);
      }
      if ((flags & GRN_OBJ_WITH_WEIGHT)) {
        for (n = block->nrecs; n; n--) {
          GRN_B_DEC(*weightp++, p);
        }
      }
      if ((flags & GRN_OBJ_WITH_POSITION)) {
        for (n = block->nposts; n; n--) {
          GRN_B_DEC(*posp, p);
          spos += *posp++;
        }
      }
      block->rest -= (p - block->bufcur);
      block->bufcur = p;
      grn_ii_buffer_fetch(ctx, ii_buffer, block);
    }
    {
      int j = 0;
      uint32_t f_s = (nrecs < 3) ? 0 : USE_P_ENC;
      uint32_t f_d = ((nrecs < 16) || (nrecs <= (lr >> 8))) ? 0 : USE_P_ENC;
      ii_buffer->data_vectors[j].data_size = nrecs;
      ii_buffer->data_vectors[j++].flags = f_d;
      if ((flags & GRN_OBJ_WITH_SECTION)) {
        ii_buffer->data_vectors[j].data_size = nrecs;
        ii_buffer->data_vectors[j++].flags = f_s;
      }
      ii_buffer->data_vectors[j].data_size = nrecs;
      ii_buffer->data_vectors[j++].flags = f_s;
      if ((flags & GRN_OBJ_WITH_WEIGHT)) {
        ii_buffer->data_vectors[j].data_size = nrecs;
        ii_buffer->data_vectors[j++].flags = f_s;
      }
      if ((flags & GRN_OBJ_WITH_POSITION)) {
        uint32_t f_p = (((nposts < 32) ||
                         (nposts <= (spos >> 13))) ? 0 : USE_P_ENC);
        ii_buffer->data_vectors[j].data_size = nposts;
        ii_buffer->data_vectors[j++].flags = f_p|ODD;
      }
    }
  }
  return (max_size + ii_buffer->ii->n_elements) * 4;
}

static buffer *
get_term_buffer(grn_ctx *ctx, grn_ii_buffer *ii_buffer)
{
  if (!ii_buffer->term_buffer) {
    uint32_t lseg;
    void *term_buffer;
    for (lseg = 0; lseg < GRN_II_MAX_LSEG; lseg++) {
      if (ii_buffer->ii->header->binfo[lseg] == NOT_ASSIGNED) { break; }
    }
    if (lseg == GRN_II_MAX_LSEG) {
      ERR(GRN_NO_MEMORY_AVAILABLE, "segment allocate failed");
      return NULL;
    }
    ii_buffer->lseg = lseg;
    ii_buffer->dseg = segment_get(ctx, ii_buffer->ii);
    GRN_IO_SEG_REF(ii_buffer->ii->seg, ii_buffer->dseg, term_buffer);
    ii_buffer->term_buffer = (buffer *)term_buffer;
  }
  return ii_buffer->term_buffer;
}

static grn_bool
try_in_place_packing(grn_ctx *ctx, grn_ii_buffer *ii_buffer,
                     grn_id tid, ii_buffer_block *hits[], int nhits)
{
  if (nhits == 1 && hits[0]->nrecs == 1 && hits[0]->nposts == 1) {
    grn_id rid;
    uint32_t sid = 1, tf, pos = 0, weight = 0;
    ii_buffer_block *block = hits[0];
    uint8_t *p = block->bufcur;
    uint32_t flags = ii_buffer->ii->header->flags;
    GRN_B_DEC(rid, p);
    if (flags & GRN_OBJ_WITH_SECTION) {
      GRN_B_DEC(sid, p);
      sid++;
    }
    GRN_B_DEC(tf, p);
    if (tf != 0) { GRN_LOG(ctx, GRN_LOG_WARNING, "tf=%d", tf); }
    if (flags & GRN_OBJ_WITH_WEIGHT) { GRN_B_DEC(weight, p); }
    if (flags & GRN_OBJ_WITH_POSITION) { GRN_B_DEC(pos, p); }
    if (!weight) {
      if (flags & GRN_OBJ_WITH_SECTION) {
        if (rid < 0x100000 && sid < 0x800) {
          uint32_t *a = array_get(ctx, ii_buffer->ii, tid);
          a[0] = (rid << 12) + (sid << 1) + 1;
          a[1] = pos;
        } else {
          return GRN_FALSE;
        }
      } else {
        uint32_t *a = array_get(ctx, ii_buffer->ii, tid);
        a[0] = (rid << 1) + 1;
        a[1] = pos;
      }
      block->rest -= (p - block->bufcur);
      block->bufcur = p;
      grn_ii_buffer_fetch(ctx, ii_buffer, block);
      return GRN_TRUE;
    }
  }
  return GRN_FALSE;
}

static void
grn_ii_buffer_merge(grn_ctx *ctx, grn_ii_buffer *ii_buffer,
                    grn_id tid, ii_buffer_block *hits[], int nhits)
{
  if (!try_in_place_packing(ctx, ii_buffer, tid, hits, nhits)) {
    size_t max_size = merge_hit_blocks(ctx, ii_buffer, hits, nhits);
    if (ii_buffer->packed_buf &&
        ii_buffer->packed_buf_size < ii_buffer->packed_len + max_size) {
      grn_ii_buffer_chunk_flush(ctx, ii_buffer);
    }
    if (!ii_buffer->packed_buf) {
      size_t buf_size = (max_size > II_BUFFER_PACKED_BUF_SIZE)
        ? max_size : II_BUFFER_PACKED_BUF_SIZE;
      if ((ii_buffer->packed_buf = GRN_MALLOC(buf_size))) {
        ii_buffer->packed_buf_size = buf_size;
      }
    }
    {
      uint16_t nterm;
      size_t packed_len;
      buffer_term *bt;
      uint32_t *a = array_get(ctx, ii_buffer->ii, tid);
      buffer *term_buffer = get_term_buffer(ctx, ii_buffer);
      if (!term_buffer) { return; }
      nterm = term_buffer->header.nterms++;
      bt = &term_buffer->terms[nterm];
      a[0] = SEG2POS(ii_buffer->lseg,
                     (sizeof(buffer_header) + sizeof(buffer_term) * nterm));
      packed_len = grn_p_encv(ctx, ii_buffer->data_vectors,
                              ii_buffer->ii->n_elements,
                              ii_buffer->packed_buf +
                              ii_buffer->packed_len);
      a[1] = ii_buffer->data_vectors[0].data_size;
      bt->tid = tid;
      bt->size_in_buffer = 0;
      bt->pos_in_buffer = 0;
      bt->size_in_chunk = packed_len;
      bt->pos_in_chunk = ii_buffer->packed_len;
      ii_buffer->packed_len += packed_len;
      if (((ii_buffer->curr_size * ii_buffer->update_buffer_size) +
           (ii_buffer->total_size * term_buffer->header.nterms * 16)) >=
          (ii_buffer->total_size * II_BUFFER_NTERMS_PER_BUFFER * 16)) {
        grn_ii_buffer_chunk_flush(ctx, ii_buffer);
      }
    }
  }
}

grn_ii_buffer *
grn_ii_buffer_open(grn_ctx *ctx, grn_ii *ii,
                   long long unsigned int update_buffer_size)
{
  if (ii && ii->lexicon) {
    grn_ii_buffer *ii_buffer = GRN_MALLOCN(grn_ii_buffer, 1);
    if (ii_buffer) {
      ii_buffer->ii = ii;
      ii_buffer->lexicon = ii->lexicon;
      ii_buffer->tmp_lexicon = NULL;
      ii_buffer->nblocks = 0;
      ii_buffer->blocks = NULL;
      ii_buffer->ncounters = II_BUFFER_NCOUNTERS_MARGIN;
      ii_buffer->block_pos = 0;
      ii_buffer->filepos = 0;
      ii_buffer->curr_size = 0;
      ii_buffer->total_size = 0;
      ii_buffer->update_buffer_size = update_buffer_size;
      ii_buffer->counters = GRN_CALLOC(ii_buffer->ncounters *
                                       sizeof(ii_buffer_counter));
      ii_buffer->term_buffer = NULL;
      ii_buffer->packed_buf = NULL;
      ii_buffer->packed_len = 0;
      ii_buffer->packed_buf_size = 0;
      ii_buffer->total_chunk_size = 0;
      if (ii_buffer->counters) {
        ii_buffer->block_buf = GRN_MALLOCN(grn_id, II_BUFFER_BLOCK_SIZE);
        if (ii_buffer->block_buf) {
          snprintf(ii_buffer->tmpfpath, PATH_MAX,
                   "%sXXXXXX", grn_io_path(ii->seg));
          ii_buffer->block_buf_size = II_BUFFER_BLOCK_SIZE;
          ii_buffer->tmpfd = grn_mkstemp(ii_buffer->tmpfpath);
          if (ii_buffer->tmpfd != -1) {
            grn_obj_flags flags;
            grn_table_get_info(ctx, ii->lexicon, &flags, NULL, NULL, NULL, NULL);
            if ((flags & GRN_OBJ_TABLE_TYPE_MASK) == GRN_OBJ_TABLE_PAT_KEY) {
              grn_pat_cache_enable(ctx, (grn_pat *)ii->lexicon,
                                   PAT_CACHE_SIZE);
            }
            return ii_buffer;
          } else {
            SERR("mkostemp");
          }
          GRN_FREE(ii_buffer->block_buf);
        }
        GRN_FREE(ii_buffer->counters);
      }
      GRN_FREE(ii_buffer);
    }
  } else {
    ERR(GRN_INVALID_ARGUMENT, "ii or ii->lexicon is NULL");
  }
  return NULL;
}

grn_rc
grn_ii_buffer_append(grn_ctx *ctx, grn_ii_buffer *ii_buffer,
                     grn_id rid, unsigned int sid, grn_obj *value)
{
  grn_ii_buffer_tokenize(ctx, ii_buffer, rid, sid, 0,
                         GRN_TEXT_VALUE(value), GRN_TEXT_LEN(value));
  return ctx->rc;
}

grn_rc
grn_ii_buffer_commit(grn_ctx *ctx, grn_ii_buffer *ii_buffer)
{
  if (ii_buffer->block_pos) {
    grn_ii_buffer_flush(ctx, ii_buffer);
  }
  if (ii_buffer->tmpfd != -1) {
    grn_close(ii_buffer->tmpfd);
  }
  if (ii_buffer->block_buf) {
    GRN_FREE(ii_buffer->block_buf);
    ii_buffer->block_buf = NULL;
  }
  if (ii_buffer->counters) {
    GRN_FREE(ii_buffer->counters);
    ii_buffer->counters = NULL;
  }

  if (ii_buffer->update_buffer_size &&
      ii_buffer->update_buffer_size < 20) {
    if (ii_buffer->update_buffer_size < 10) {
      ii_buffer->update_buffer_size =
        ii_buffer->total_size >> (10 - ii_buffer->update_buffer_size);
    } else {
      ii_buffer->update_buffer_size =
        ii_buffer->total_size << (ii_buffer->update_buffer_size - 10);
    }
  }

  GRN_LOG(ctx, GRN_LOG_NOTICE,
          "nblocks=%d, update_buffer_size=%" GRN_FMT_INT64U,
          ii_buffer->nblocks, ii_buffer->update_buffer_size);

  datavec_init(ctx, ii_buffer->data_vectors, ii_buffer->ii->n_elements, 0, 0);
#ifdef WIN32
  ii_buffer->tmpfd = GRN_OPEN(ii_buffer->tmpfpath, O_RDONLY|O_BINARY);
#else /* WIN32 */
  ii_buffer->tmpfd = GRN_OPEN(ii_buffer->tmpfpath, O_RDONLY);
#endif /* WIN32 */
  if (ii_buffer->tmpfd == -1) {
    ERRNO_ERR("oepn");
    return ctx->rc;
  }
  {
    uint32_t i;
    for (i = 0; i < ii_buffer->nblocks; i++) {
      grn_ii_buffer_fetch(ctx, ii_buffer, &ii_buffer->blocks[i]);
    }
  }
  {
    ii_buffer_block **hits;
    if ((hits = GRN_MALLOCN(ii_buffer_block *, ii_buffer->nblocks))) {
      grn_id tid;
      grn_table_cursor *tc;
      tc = grn_table_cursor_open(ctx, ii_buffer->lexicon,
                                 NULL, 0, NULL, 0, 0, -1, II_BUFFER_ORDER);
      if (tc) {
        while ((tid = grn_table_cursor_next(ctx, tc)) != GRN_ID_NIL) {
          int nrests = 0;
          int nhits = 0;
          uint32_t i;
          for (i = 0; i < ii_buffer->nblocks; i++) {
            if (ii_buffer->blocks[i].tid == tid) {
              hits[nhits++] = &ii_buffer->blocks[i];
            }
            if (ii_buffer->blocks[i].tid) { nrests++; }
          }
          if (nhits) { grn_ii_buffer_merge(ctx, ii_buffer, tid, hits, nhits); }
          if (!nrests) { break; }
        }
        if (ii_buffer->packed_len) {
          grn_ii_buffer_chunk_flush(ctx, ii_buffer);
        }
        grn_table_cursor_close(ctx, tc);
      }
      GRN_FREE(hits);
    }
  }
  datavec_fin(ctx, ii_buffer->data_vectors);
  GRN_LOG(ctx, GRN_LOG_NOTICE,
          "tmpfile_size:%" GRN_FMT_INT64D " > total_chunk_size:%" GRN_FMT_SIZE,
          ii_buffer->filepos, ii_buffer->total_chunk_size);
  grn_close(ii_buffer->tmpfd);
  grn_unlink(ii_buffer->tmpfpath);
  ii_buffer->tmpfd = -1;
  return ctx->rc;
}

grn_rc
grn_ii_buffer_close(grn_ctx *ctx, grn_ii_buffer *ii_buffer)
{
  uint32_t i;
  grn_obj_flags flags;
  grn_table_get_info(ctx, ii_buffer->ii->lexicon, &flags, NULL, NULL, NULL, NULL);
  if ((flags & GRN_OBJ_TABLE_TYPE_MASK) == GRN_OBJ_TABLE_PAT_KEY) {
    grn_pat_cache_disable(ctx, (grn_pat *)ii_buffer->ii->lexicon);
  }
  if (ii_buffer->tmp_lexicon) {
    grn_obj_close(ctx, ii_buffer->tmp_lexicon);
  }
  if (ii_buffer->tmpfd != -1) {
    grn_close(ii_buffer->tmpfd);
    grn_unlink(ii_buffer->tmpfpath);
  }
  if (ii_buffer->block_buf) {
    GRN_FREE(ii_buffer->block_buf);
  }
  if (ii_buffer->counters) {
    GRN_FREE(ii_buffer->counters);
  }
  if (ii_buffer->blocks) {
    for (i = 0; i < ii_buffer->nblocks; i++) {
      if (ii_buffer->blocks[i].buffer) {
        GRN_FREE(ii_buffer->blocks[i].buffer);
      }
    }
    GRN_FREE(ii_buffer->blocks);
  }
  GRN_FREE(ii_buffer);
  return ctx->rc;
}

static void
grn_ii_buffer_parse(grn_ctx *ctx, grn_ii_buffer *ii_buffer,
                    grn_obj *target, int ncols, grn_obj **cols)
{
  grn_table_cursor  *tc;
  if ((tc = grn_table_cursor_open(ctx, target,
                                  NULL, 0, NULL, 0, 0, -1,
                                  GRN_CURSOR_BY_ID))) {
    grn_id rid;
    grn_obj rv;
    GRN_TEXT_INIT(&rv, 0);
    while ((rid = grn_table_cursor_next(ctx, tc)) != GRN_ID_NIL) {
      int sid;
      grn_obj **col;
      for (sid = 1, col = cols; sid <= ncols; sid++, col++) {
        grn_obj_reinit_for(ctx, &rv, *col);
        if (GRN_OBJ_TABLEP(*col)) {
          grn_table_get_key2(ctx, *col, rid, &rv);
        } else {
          grn_obj_get_value(ctx, *col, rid, &rv);
        }
        switch (rv.header.type) {
        case GRN_BULK :
          grn_ii_buffer_tokenize(ctx, ii_buffer, rid, sid, 0,
                                 GRN_TEXT_VALUE(&rv), GRN_TEXT_LEN(&rv));
          break;
        case GRN_VECTOR :
          if (rv.u.v.body) {
            int i;
            int n_sections = rv.u.v.n_sections;
            grn_section *sections = rv.u.v.sections;
            const char *head = GRN_BULK_HEAD(rv.u.v.body);
            for (i = 0; i < n_sections; i++) {
              grn_section *section = sections + i;
              if (section->length == 0) {
                continue;
              }
              grn_ii_buffer_tokenize(ctx, ii_buffer, rid,
                                     sid, section->weight,
                                     head + section->offset, section->length);
            }
          }
          break;
        default :
          ERR(GRN_INVALID_ARGUMENT, "[index] invalid object assigned as value");
          break;
        }
      }
    }
    GRN_OBJ_FIN(ctx, &rv);
    grn_table_cursor_close(ctx, tc);
  }
}

grn_rc
grn_ii_build(grn_ctx *ctx, grn_ii *ii, uint64_t sparsity)
{
  grn_ii_buffer *ii_buffer;

  {
    grn_obj *data_table;

    data_table = grn_ctx_at(ctx, DB_OBJ(ii)->range);
    if (!data_table) {
      return ctx->rc;
    }
    if (grn_table_size(ctx, data_table) == 0) {
      return ctx->rc;
    }
  }

  ii_buffer = grn_ii_buffer_open(ctx, ii, sparsity);
  if (ii_buffer) {
    grn_id *s = ii->obj.source;
    if ((ii->obj.source_size) && s) {
      int ncols = ii->obj.source_size / sizeof(grn_id);
      grn_obj **cols = GRN_MALLOCN(grn_obj *, ncols);
      if (cols) {
        int i;
        for (i = 0; i < ncols; i++) {
          if (!(cols[i] = grn_ctx_at(ctx, s[i]))) { break; }
        }
        if (i == ncols) {
          grn_obj *target = cols[0];
          if (!GRN_OBJ_TABLEP(target)) {
            target = grn_ctx_at(ctx, target->header.domain);
          }
          if (target) {
            grn_ii_buffer_parse(ctx, ii_buffer, target, ncols, cols);
            grn_ii_buffer_commit(ctx, ii_buffer);
          } else {
            ERR(GRN_INVALID_ARGUMENT, "failed to resolve the target");
          }
        } else {
          ERR(GRN_INVALID_ARGUMENT, "failed to resolve a column (%d)", i);
        }
        GRN_FREE(cols);
      }
    } else {
      ERR(GRN_INVALID_ARGUMENT, "ii->obj.source is void");
    }
    grn_ii_buffer_close(ctx, ii_buffer);
  }
  return ctx->rc;
}
