/*
 *  GRUB  --  GRand Unified Bootloader
 *  Copyright (C) 2009  Free Software Foundation, Inc.
 *
 *  GRUB is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  GRUB is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with GRUB.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <grub/relocator.h>
#include <grub/relocator_private.h>
#include <grub/mm_private.h>
#include <grub/misc.h>
#include <grub/cache.h>

/* TODO: use more efficient data structures if necessary.  */
/* FIXME: check memory map.  */ 
/* FIXME: try to request memory from firmware.  */

struct grub_relocator
{
  struct grub_relocator_chunk *chunks;
  grub_addr_t postchunks;
  grub_addr_t highestaddr;
  grub_addr_t highestnonpostaddr;
  grub_size_t relocators_size;
};

struct grub_relocator_chunk
{
  struct grub_relocator_chunk *next;
  grub_addr_t src;
  grub_addr_t target;
  grub_size_t size;
  enum {CHUNK_TYPE_IN_REGION, CHUNK_TYPE_REGION_START} type;
  grub_addr_t host_start;
};

struct grub_relocator *
grub_relocator_new (void)
{
  struct grub_relocator *ret;

  grub_cpu_relocator_init ();

  ret = grub_zalloc (sizeof (struct grub_relocator));
  if (!ret)
    return NULL;
    
  ret->postchunks = ~(grub_addr_t) 0;
  ret->relocators_size = grub_relocator_jumper_size;
  grub_dprintf ("relocator", "relocators_size=%lu\n",
		(unsigned long) ret->relocators_size);
  return ret;
}

static grub_mm_header_t
get_best_header (struct grub_relocator *rel,
		 grub_addr_t start, grub_addr_t end, grub_addr_t align,
		 grub_size_t size,
		 grub_mm_region_t rb, grub_mm_header_t *prev,
		 grub_addr_t *best_addr, int from_low_priv, int collisioncheck)
{
  grub_mm_header_t h, hp;
  grub_mm_header_t hb = NULL, hbp = NULL;

  auto void try_addr (grub_addr_t allowable_start, grub_addr_t allowable_end);
  void try_addr (grub_addr_t allowable_start, grub_addr_t allowable_end)
  {
    if (from_low_priv)
      {
	grub_addr_t addr;

	addr = ALIGN_UP (allowable_start, align);

	if (addr < start)
	  addr = ALIGN_UP (start, align);

	if (collisioncheck)
	  while (1)
	    {
	      struct grub_relocator_chunk *chunk;
	      for (chunk = rel->chunks; chunk; chunk = chunk->next)
		if ((chunk->target <= addr
		     && addr < chunk->target + chunk->size)
		    || (chunk->target < addr + size
			&& addr + size < chunk->target + chunk->size)
		    || (addr <= chunk->target && chunk->target < addr + size)
		    || (addr < chunk->target + chunk->size
			&& chunk->target + chunk->size < addr + size))
		  {
		    grub_dprintf ("relocator",
				  "collision 0x%llx+0x%llx, 0x%llx+0x%llx\n",
				  (unsigned long long) chunk->target,
				  (unsigned long long) chunk->size,
				  (unsigned long long) addr,
				  (unsigned long long) size);
		    addr = ALIGN_UP (chunk->target + chunk->size, align);
		    break;
		  }
	      if (!chunk)
		break;
	    }

	if (allowable_end <= addr + size)
	  return;

	if (addr > end)
	  return;

	if (hb == NULL || *best_addr > addr)
	  {
	    hb = h;
	    hbp = hp;
	    *best_addr = addr;
	    grub_dprintf ("relocator", "picked %p/%lx\n", hb,
			  (unsigned long) addr);
	  }
      }
    else
      {
	grub_addr_t addr;

	addr = ALIGN_DOWN (allowable_end - size, align);

	if (addr > end)
	  addr = ALIGN_DOWN (end, align);

	if (collisioncheck)
	  while (1)
	    {
	      struct grub_relocator_chunk *chunk;
	      for (chunk = rel->chunks; chunk; chunk = chunk->next)
		if ((chunk->target <= addr
		     && addr < chunk->target + chunk->size)
		    || (chunk->target < addr + size
			&& addr + size < chunk->target + chunk->size)
		    || (addr <= chunk->target && chunk->target < addr + size)
		    || (addr < chunk->target + chunk->size
			&& chunk->target + chunk->size < addr + size))
		  {
		    addr = ALIGN_DOWN (chunk->target - size, align);
		    break;
		  }
	      if (!chunk)
		break;
	    }

	if (allowable_start > addr)
	  return;

	if (addr < start)
	  return;

	if (hb == NULL || *best_addr < addr)
	  {
	    hb = h;
	    hbp = hp;
	    *best_addr = addr;
	    grub_dprintf ("relocator", "picked %p/%lx\n", hb,
			  (unsigned long) addr);
	  }
      }
  }
  
  hp = rb->first;
  h = hp->next;
  grub_dprintf ("relocator", "alive\n");
  do
    {
      grub_addr_t allowable_start, allowable_end;
      allowable_start = (grub_addr_t) h;
      allowable_end = (grub_addr_t) (h + h->size);

      if (h->magic != GRUB_MM_FREE_MAGIC)
	grub_fatal ("free magic is broken at %p: 0x%x", h, h->magic);

      try_addr (allowable_start, allowable_end);

      if ((grub_addr_t) h == (grub_addr_t) (rb + 1))
	{
	  grub_dprintf ("relocator", "Trying region start 0x%llx\n",
			(unsigned long long) (allowable_start 
					      - sizeof (*rb) - rb->pre_size));
	  try_addr (allowable_start - sizeof (*rb) - rb->pre_size,
		    allowable_end - sizeof (*rb));
	}
      hp = h;
      h = hp->next;
    }
  while (hp && hp != rb->first);
  *prev = hbp;
  return hb;
}

static int
malloc_in_range (struct grub_relocator *rel,
		 grub_addr_t start, grub_addr_t end, grub_addr_t align,
		 grub_size_t size, struct grub_relocator_chunk *res,
		 int from_low_priv, int collisioncheck)
{
  grub_mm_region_t rb, rbp;
  grub_mm_header_t hb = NULL, hbp = NULL;
  grub_addr_t best_addr;

 again:

  rb = NULL, rbp = NULL;
  
  {
    grub_mm_region_t r, rp;
    for (rp = NULL, r = grub_mm_base; r; rp = r, r = r->next)
      {
	grub_dprintf ("relocator", "region %p. %d %d %d %d\n", r,
		      (grub_addr_t) r + r->size + sizeof (*r) >= start,
		      (grub_addr_t) r < end, r->size + sizeof (*r) >= size,
		      (rb == NULL || (from_low_priv ? rb > r : rb < r)));
	if ((grub_addr_t) r + r->size + sizeof (*r) >= start
	    && (grub_addr_t) r < end && r->size + sizeof (*r) >= size
	    && (rb == NULL || (from_low_priv ? rb > r : rb < r)))
	  {
	    rb = r;
	    rbp = rp;
	  }
      }
  }

  if (!rb)
    {
      grub_dprintf ("relocator", "no suitable region found\n");
      return 0;
    }

  grub_dprintf ("relocator", "trying region %p - %p\n", rb, rb + rb->size + 1);

  hb = get_best_header (rel, start, end, align, size, rb, &hbp, &best_addr,
			from_low_priv, collisioncheck);

  grub_dprintf ("relocator", "best header %p/%p/%lx\n", hb, hbp,
		(unsigned long) best_addr);

  if (!hb)
    {
      if (from_low_priv)
	start = (grub_addr_t) (rb + rb->size + sizeof (*rb));
      else
	end = (grub_addr_t) rb - 1;
      goto again;
    }

  /* Special case: relocating region start.  */
  if (best_addr < (grub_addr_t) hb)
    {
      grub_addr_t newreg_start, newreg_raw_start = best_addr + size;
      grub_addr_t newreg_size, newreg_presize;
      grub_mm_header_t new_header;

      res->src = best_addr;
      res->type = CHUNK_TYPE_REGION_START;
      res->host_start = (grub_addr_t) rb - rb->pre_size;

      newreg_start = ALIGN_UP (newreg_raw_start, GRUB_MM_ALIGN);
      newreg_presize = newreg_start - newreg_raw_start;
      newreg_size = rb->size - (newreg_start - (grub_addr_t) rb);
      if ((hb->size << GRUB_MM_ALIGN_LOG2) >= newreg_start
	  - (grub_addr_t) rb)
	{
	  grub_mm_header_t newhnext = hb->next;
	  grub_size_t newhsize = ((hb->size << GRUB_MM_ALIGN_LOG2)
				  - (newreg_start
				     - (grub_addr_t) rb)) >> GRUB_MM_ALIGN_LOG2;
	  new_header = (void *) (newreg_start + sizeof (*rb));
	  if (newhnext == hb)
	    newhnext = new_header;
	  new_header->next = newhnext;
	  new_header->size = newhsize;
	  new_header->magic = GRUB_MM_FREE_MAGIC;
	}
      else
	{
	  new_header = hb->next;
	  if (new_header == hb)
	    new_header = (void *) (newreg_start + sizeof (*rb));	    
	}
      {
	struct grub_mm_header *newregfirst = rb->first;
	struct grub_mm_region *newregnext = rb->next;
	struct grub_mm_region *newreg = (void *) newreg_start;
	hbp->next = new_header;
	if (newregfirst == hb)
	  newregfirst = new_header;
	newreg->first = newregfirst;
	newreg->next = newregnext;
	newreg->pre_size = newreg_presize;
	newreg->size = newreg_size;
	if (rbp)
	  rbp->next = newreg;
	else
	  grub_mm_base = newreg;
	{
	  grub_mm_header_t h = newreg->first, hp = NULL;
	  do
	    {
	      if ((void *) h < (void *) (newreg + 1))
		grub_fatal ("Failed to adjust memory region: %p, %p, %p, %p, %p",
			    newreg, newreg->first, h, hp, hb);
	      hp = h;
	      h = h->next;
	    }
	  while (h != newreg->first);
	}
      }
      return 1;
    }
  {
    struct grub_mm_header *foll = NULL;

    res->src = best_addr;
    res->type = CHUNK_TYPE_IN_REGION;
    
    if (ALIGN_UP (best_addr + size, GRUB_MM_ALIGN) + GRUB_MM_ALIGN
	<= (grub_addr_t) (hb + hb->size))
      {
	foll = (void *) ALIGN_UP (best_addr + size, GRUB_MM_ALIGN);
	foll->magic = GRUB_MM_FREE_MAGIC;
	foll->size = hb->size - (foll - hb);
      }

    if (best_addr - (grub_addr_t) hb >= sizeof (*hb))
      {
	hb->size = ((best_addr - (grub_addr_t) hb) >> GRUB_MM_ALIGN_LOG2);
	if (foll)
	  {
	    foll->next = hb;
	    hbp->next = foll;
	    if (rb->first == hb)
	      rb->first = foll;
	  }
      }
    else
      {
	if (foll)
	  foll->next = hb->next;
	else
	  foll = hb->next;
	
	hbp->next = foll;
	if (rb->first == hb)
	  rb->first = foll;
	if (rb->first == hb)
	  rb->first = (void *) (rb + 1);
      }
    return 1;
  }
}

static void
adjust_limits (struct grub_relocator *rel, 
	       grub_addr_t *min_addr, grub_addr_t *max_addr,
	       grub_addr_t in_min, grub_addr_t in_max)
{
  struct grub_relocator_chunk *chunk;

  *min_addr = 0;
  *max_addr = rel->postchunks;

  /* Keep chunks in memory in the same order as they'll be after relocation.  */
  for (chunk = rel->chunks; chunk; chunk = chunk->next)
    {
      if (chunk->target > in_max && chunk->src < *max_addr
	  && chunk->src < rel->postchunks)
	*max_addr = chunk->src;
      if (chunk->target + chunk->size <= in_min
	  && chunk->src + chunk->size > *min_addr
	  && chunk->src < rel->postchunks)
	*min_addr = chunk->src + chunk->size;
    }
}

grub_err_t
grub_relocator_alloc_chunk_addr (struct grub_relocator *rel, void **src,
				 grub_addr_t target, grub_size_t size)
{
  struct grub_relocator_chunk *chunk;
  grub_addr_t min_addr = 0, max_addr;

  if (target > ~size)
    return grub_error (GRUB_ERR_OUT_OF_RANGE, "address is out of range");

  adjust_limits (rel, &min_addr, &max_addr, target, target);

  for (chunk = rel->chunks; chunk; chunk = chunk->next)
    if ((chunk->target <= target && target < chunk->target + chunk->size)
	|| (target <= chunk->target && chunk->target < target + size))
      return grub_error (GRUB_ERR_BAD_ARGUMENT, "overlap detected");


  chunk = grub_malloc (sizeof (struct grub_relocator_chunk));
  if (!chunk)
    return grub_errno;

  grub_dprintf ("relocator",
		"min_addr = 0x%llx, max_addr = 0x%llx, target = 0x%llx\n",
		(unsigned long long) min_addr, (unsigned long long) max_addr,
		(unsigned long long) target);

  do
    {
      /* A trick to improve Linux allocation.  */
#if defined (__i386__) || defined (__x86_64__)
      if (target < 0x100000)
	if (malloc_in_range (rel, rel->highestnonpostaddr, ~(grub_addr_t)0, 1,
			     size, chunk, 0, 1))
	  {
	    if (rel->postchunks > chunk->src)
	      rel->postchunks = chunk->src;
	    break;
	  }
#endif
      if (malloc_in_range (rel, target, max_addr, 1, size, chunk, 1, 0))
	break;

      if (malloc_in_range (rel, min_addr, target, 1, size, chunk, 0, 0))
	break;

      if (malloc_in_range (rel, rel->highestnonpostaddr, ~(grub_addr_t)0, 1,
			   size, chunk, 0, 1))
	{
	  if (rel->postchunks > chunk->src)
	    rel->postchunks = chunk->src;
	  break;
	}

      grub_dprintf ("relocator", "not allocated\n");
      grub_free (chunk);
      return grub_error (GRUB_ERR_OUT_OF_MEMORY, "out of memory");
    }
  while (0);

  grub_dprintf ("relocator", "allocated 0x%llx/0x%llx\n",
		(unsigned long long) chunk->src, (unsigned long long) target);

  if (rel->highestaddr < target + size)
    rel->highestaddr = target + size;

  if (rel->highestaddr < chunk->src + size)
    rel->highestaddr = chunk->src + size;

  if (chunk->src < rel->postchunks)
    {
      if (rel->highestnonpostaddr < target + size)
	rel->highestnonpostaddr = target + size;
      
      if (rel->highestnonpostaddr < chunk->src + size)
	rel->highestnonpostaddr = chunk->src + size;  
    }

  grub_dprintf ("relocator", "relocators_size=%ld\n",
		(unsigned long) rel->relocators_size);

  if (chunk->src < target)
    rel->relocators_size += grub_relocator_backward_size;
  if (chunk->src > target)
    rel->relocators_size += grub_relocator_forward_size;

  grub_dprintf ("relocator", "relocators_size=%ld\n",
		(unsigned long) rel->relocators_size);

  chunk->target = target;
  chunk->size = size;
  chunk->next = rel->chunks;
  rel->chunks = chunk;
  grub_dprintf ("relocator", "cur = %p, next = %p\n", rel->chunks,
		rel->chunks->next);

  *src = (void *) chunk->src;
  return GRUB_ERR_NONE;
}

grub_err_t
grub_relocator_alloc_chunk_align (struct grub_relocator *rel, void **src,
				  grub_addr_t *target,
				  grub_addr_t min_addr, grub_addr_t max_addr,
				  grub_size_t size, grub_size_t align,
				  int preference)
{
  grub_addr_t min_addr2 = 0, max_addr2;
  struct grub_relocator_chunk *chunk;

  if (max_addr > ~size)
    max_addr = ~size;

#ifdef GRUB_MACHINE_PCBIOS
  if (min_addr < 0x1000)
    min_addr = 0x1000;
#endif

  grub_dprintf ("relocator", "chunks = %p\n", rel->chunks);

  chunk = grub_malloc (sizeof (struct grub_relocator_chunk));
  if (!chunk)
    return grub_errno;

  if (malloc_in_range (rel, min_addr, max_addr, align,
		       size, chunk,
		       preference != GRUB_RELOCATOR_PREFERENCE_HIGH, 1))
    {
      grub_dprintf ("relocator", "allocated 0x%llx/0x%llx\n",
		    (unsigned long long) chunk->src,
		    (unsigned long long) chunk->src);
      grub_dprintf ("relocator", "chunks = %p\n", rel->chunks);
      chunk->target = chunk->src;
      chunk->size = size;
      chunk->next = rel->chunks;
      rel->chunks = chunk;
      *src = (void *) chunk->src;
      *target = chunk->target;
      return GRUB_ERR_NONE;
    }

  adjust_limits (rel, &min_addr2, &max_addr2, min_addr, max_addr);
  grub_dprintf ("relocator", "Adjusted limits from %lx-%lx to %lx-%lx\n",
		(unsigned long) min_addr, (unsigned long) max_addr,
		(unsigned long) min_addr2, (unsigned long) max_addr2);

  do
    {
      if (malloc_in_range (rel, min_addr2, max_addr2, align,
			   size, chunk, 1, 1))
	break;

      if (malloc_in_range (rel, rel->highestnonpostaddr, ~(grub_addr_t)0, 1,
			   size, chunk, 0, 1))
	{
	  if (rel->postchunks > chunk->src)
	    rel->postchunks = chunk->src;
	  break;
	}

      return grub_error (GRUB_ERR_OUT_OF_MEMORY, "out of memory");
    }
  while (0);

  /* FIXME: check memory map.  */
  if (preference == GRUB_RELOCATOR_PREFERENCE_HIGH)
    chunk->target = ALIGN_DOWN (max_addr, align);
  else
    chunk->target = ALIGN_UP (min_addr, align);    
  while (1)
    {
      struct grub_relocator_chunk *chunk2;
      for (chunk2 = rel->chunks; chunk2; chunk2 = chunk2->next)
	if ((chunk2->target <= chunk->target
	     && chunk->target < chunk2->target + chunk2->size)
	    || (chunk2->target <= chunk->target + size
		&& chunk->target + size < chunk2->target + chunk2->size)
	    || (chunk->target <= chunk2->target && chunk2->target
		< chunk->target + size)
	    || (chunk->target <= chunk2->target + chunk2->size
		&& chunk2->target + chunk2->size < chunk->target + size))
	  {
	    if (preference == GRUB_RELOCATOR_PREFERENCE_HIGH)
	      chunk->target = ALIGN_DOWN (chunk2->target, align);
	    else
	      chunk->target = ALIGN_UP (chunk2->target + chunk2->size, align);
	    break;
	  }
      if (!chunk2)
	break;
    }

  if (chunk->src < chunk->target)
    rel->relocators_size += grub_relocator_backward_size;
  if (chunk->src > chunk->target)
    rel->relocators_size += grub_relocator_forward_size;

  chunk->size = size;
  chunk->next = rel->chunks;
  rel->chunks = chunk;
  grub_dprintf ("relocator", "cur = %p, next = %p\n", rel->chunks,
		rel->chunks->next);
  *src = (void *) chunk->src;
  *target = chunk->target;
  return GRUB_ERR_NONE;
}

void
grub_relocator_unload (struct grub_relocator *rel)
{
  struct grub_relocator_chunk *chunk, *next;
  if (!rel)
    return;
  for (chunk = rel->chunks; chunk; chunk = next)
    {
      switch (chunk->type)
	{
	case CHUNK_TYPE_REGION_START:
	  {
	    grub_mm_region_t r1, r2, *rp;
	    grub_mm_header_t h;
	    grub_size_t pre_size;
	    r1 = (grub_mm_region_t) ALIGN_UP (chunk->src + chunk->size,
					      GRUB_MM_ALIGN);
	    r2 = (grub_mm_region_t) ALIGN_UP (chunk->host_start, GRUB_MM_ALIGN);
	    for (rp = &grub_mm_base; *rp && *rp != r2; rp = &((*rp)->next));
	    if (!*rp)
	      grub_fatal ("Anomaly in region alocations detected. "
			  "Simultaneous relocators?");
	    pre_size = ALIGN_UP (chunk->host_start, GRUB_MM_ALIGN)
	      - chunk->host_start;
	    r2->first = r1->first;
	    r2->next = r1->next;
	    r2->pre_size = pre_size;
	    r2->size = r1->size + (r2 - r1) * sizeof (*r2);
	    *rp = r1;
	    h = (grub_mm_header_t) (r1 + 1);
	    h->next = h;
	    h->magic = GRUB_MM_ALLOC_MAGIC;
	    h->size = (r2 - r1);
	    grub_free (h + 1);
	    break;
	  }
	case CHUNK_TYPE_IN_REGION:
	  {
	    grub_mm_header_t h = (grub_mm_header_t) ALIGN_DOWN (chunk->src,
								GRUB_MM_ALIGN);
	    h->size = (chunk->src / GRUB_MM_ALIGN)
	      - ((chunk->src + chunk->size + GRUB_MM_ALIGN - 1) 
		 / GRUB_MM_ALIGN);
	    h->next = h;
	    h->magic = GRUB_MM_ALLOC_MAGIC;
	    grub_free (h + 1);
	    break;
	  }
	}
      next = chunk->next;
      grub_free (chunk);
    }
}

grub_err_t
grub_relocator_prepare_relocs (struct grub_relocator *rel, grub_addr_t addr,
			       grub_addr_t *relstart, grub_size_t *relsize)
{
  grub_addr_t rels;
  grub_addr_t rels0;
  struct grub_relocator_chunk *sorted;
  grub_size_t nchunks = 0;
  unsigned j;
  struct grub_relocator_chunk movers_chunk;

  grub_dprintf ("relocator", "Preparing relocs (size=%ld)\n",
		(unsigned long) rel->relocators_size);

  if (!malloc_in_range (rel, 0, ~(grub_addr_t)0 - rel->relocators_size + 1,
			grub_relocator_align,
			rel->relocators_size, &movers_chunk, 1, 1))
    return grub_error (GRUB_ERR_OUT_OF_MEMORY, "out of memory");
  rels = rels0 = movers_chunk.src;

  if (relsize)
    *relsize = rel->relocators_size;

  grub_dprintf ("relocator", "Relocs allocated\n");
  
  {
    unsigned i;
    grub_size_t count[257];
    struct grub_relocator_chunk *from, *to, *tmp;

    grub_memset (count, 0, sizeof (count));

    {
        struct grub_relocator_chunk *chunk;
	for (chunk = rel->chunks; chunk; chunk = chunk->next)
	  {
	    grub_dprintf ("relocator", "chunk %p->%p, 0x%lx\n", 
			  (void *) chunk->src, (void *) chunk->target,
			  (unsigned long) chunk->size);
	    nchunks++;
	    count[(chunk->src & 0xff) + 1]++;
	  }
    }
    from = grub_malloc (nchunks * sizeof (sorted[0]));
    to = grub_malloc (nchunks * sizeof (sorted[0]));
    if (!from || !to)
      {
	grub_free (from);
	grub_free (to);
	return grub_errno;
      }

    for (j = 0; j < 256; j++)
      count[j+1] += count[j];

    {
      struct grub_relocator_chunk *chunk;
      for (chunk = rel->chunks; chunk; chunk = chunk->next)
	from[count[chunk->src & 0xff]++] = *chunk;
    }

    for (i = 1; i < GRUB_CPU_SIZEOF_VOID_P; i++)
      {
	grub_memset (count, 0, sizeof (count));
	for (j = 0; j < nchunks; j++)
	  count[((from[j].src >> (8 * i)) & 0xff) + 1]++;
	for (j = 0; j < 256; j++)
	  count[j+1] += count[j];
	for (j = 0; j < nchunks; j++)
	  to[count[(from[j].src >> (8 * i)) & 0xff]++] = from[j];
	tmp = to;
	to = from;
	from = tmp;
      }
    sorted = from;
    grub_free (to);
  }

  for (j = 0; j < nchunks; j++)
    {
      grub_dprintf ("relocator", "sorted chunk %p->%p, 0x%lx\n", 
		    (void *) sorted[j].src, (void *) sorted[j].target,
		    (unsigned long) sorted[j].size);
      if (sorted[j].src < sorted[j].target)
	{
	  grub_cpu_relocator_backward ((void *) rels,
				       (void *) sorted[j].src,
				       (void *) sorted[j].target,
				       sorted[j].size);
	  rels += grub_relocator_backward_size;
	}
      if (sorted[j].src > sorted[j].target)
	{
	  grub_cpu_relocator_forward ((void *) rels,
				      (void *) sorted[j].src,
				      (void *) sorted[j].target,
				      sorted[j].size);
	  rels += grub_relocator_forward_size;
	}
      if (sorted[j].src == sorted[j].target)
	grub_arch_sync_caches ((void *) sorted[j].src, sorted[j].size);
    }
  grub_cpu_relocator_jumper ((void *) rels, addr);
  *relstart = rels0;
  grub_free (sorted);
  return GRUB_ERR_NONE;
}
