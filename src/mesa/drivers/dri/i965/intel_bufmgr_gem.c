/**************************************************************************
 *
 * Copyright © 2007 Red Hat Inc.
 * Copyright © 2007-2012 Intel Corporation
 * Copyright 2006 Tungsten Graphics, Inc., Bismarck, ND., USA
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE COPYRIGHT HOLDERS, AUTHORS AND/OR ITS SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 *
 **************************************************************************/
/*
 * Authors: Thomas Hellström <thomas-at-tungstengraphics-dot-com>
 *          Keith Whitwell <keithw-at-tungstengraphics-dot-com>
 *	    Eric Anholt <eric@anholt.net>
 *	    Dave Airlie <airlied@linux.ie>
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <xf86drm.h>
#include <util/u_atomic.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <stdbool.h>

#include "errno.h"
#ifndef ETIME
#define ETIME ETIMEDOUT
#endif
#include "libdrm_macros.h"
#include "util/list.h"
#include "brw_bufmgr.h"
#include "intel_bufmgr_priv.h"
#include "intel_chipset.h"
#include "string.h"

#include "i915_drm.h"
#include "uthash.h"

#ifdef HAVE_VALGRIND
#include <valgrind.h>
#include <memcheck.h>
#define VG(x) x
#else
#define VG(x)
#endif

#define memclear(s) memset(&s, 0, sizeof(s))

#define DBG(...) do {					\
	if (bufmgr_gem->bufmgr.debug)			\
		fprintf(stderr, __VA_ARGS__);		\
} while (0)

#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#define MAX2(A, B) ((A) > (B) ? (A) : (B))

static inline int
atomic_add_unless(int *v, int add, int unless)
{
   int c, old;
   c = p_atomic_read(v);
   while (c != unless && (old = p_atomic_cmpxchg(v, c, c + add)) != c)
      c = old;
   return c == unless;
}

/**
 * upper_32_bits - return bits 32-63 of a number
 * @n: the number we're accessing
 *
 * A basic shift-right of a 64- or 32-bit quantity.  Use this to suppress
 * the "right shift count >= width of type" warning when that quantity is
 * 32-bits.
 */
#define upper_32_bits(n) ((__u32)(((n) >> 16) >> 16))

/**
 * lower_32_bits - return bits 0-31 of a number
 * @n: the number we're accessing
 */
#define lower_32_bits(n) ((__u32)(n))

typedef struct _drm_bacon_bo_gem drm_bacon_bo_gem;

struct drm_bacon_gem_bo_bucket {
	struct list_head head;
	unsigned long size;
};

typedef struct _drm_bacon_bufmgr_gem {
	drm_bacon_bufmgr bufmgr;

	int refcount;

	int fd;

	int max_relocs;

	pthread_mutex_t lock;

	struct drm_i915_gem_exec_object2 *exec2_objects;
	drm_bacon_bo **exec_bos;
	int exec_size;
	int exec_count;

	/** Array of lists of cached gem objects of power-of-two sizes */
	struct drm_bacon_gem_bo_bucket cache_bucket[14 * 4];
	int num_buckets;
	time_t time;

	struct list_head managers;

	drm_bacon_bo_gem *name_table;
	drm_bacon_bo_gem *handle_table;

	struct list_head vma_cache;
	int vma_count, vma_open, vma_max;

	uint64_t gtt_size;
	int pci_device;
	int gen;
	unsigned int has_bsd : 1;
	unsigned int has_blt : 1;
	unsigned int has_llc : 1;
	unsigned int has_wait_timeout : 1;
	unsigned int bo_reuse : 1;
	unsigned int no_exec : 1;
	unsigned int has_vebox : 1;
	unsigned int has_exec_async : 1;

	struct {
		void *ptr;
		uint32_t handle;
	} userptr_active;

} drm_bacon_bufmgr_gem;

typedef struct _drm_bacon_reloc_target_info {
	drm_bacon_bo *bo;
} drm_bacon_reloc_target;

struct _drm_bacon_bo_gem {
	drm_bacon_bo bo;

	int refcount;
	uint32_t gem_handle;
	const char *name;

	/**
	 * Kenel-assigned global name for this object
         *
         * List contains both flink named and prime fd'd objects
	 */
	unsigned int global_name;

	UT_hash_handle handle_hh;
	UT_hash_handle name_hh;

	/**
	 * Index of the buffer within the validation list while preparing a
	 * batchbuffer execution.
	 */
	int validate_index;

	/**
	 * Current tiling mode
	 */
	uint32_t tiling_mode;
	uint32_t swizzle_mode;
	unsigned long stride;

	unsigned long kflags;

	time_t free_time;

	/** Array passed to the DRM containing relocation information. */
	struct drm_i915_gem_relocation_entry *relocs;
	/**
	 * Array of info structs corresponding to relocs[i].target_handle etc
	 */
	drm_bacon_reloc_target *reloc_target_info;
	/** Number of entries in relocs */
	int reloc_count;
	/** Array of BOs that are referenced by this buffer and will be softpinned */
	drm_bacon_bo **softpin_target;
	/** Number softpinned BOs that are referenced by this buffer */
	int softpin_target_count;
	/** Maximum amount of softpinned BOs that are referenced by this buffer */
	int softpin_target_size;

	/** Mapped address for the buffer, saved across map/unmap cycles */
	void *mem_virtual;
	/** GTT virtual address for the buffer, saved across map/unmap cycles */
	void *gtt_virtual;
	/** WC CPU address for the buffer, saved across map/unmap cycles */
	void *wc_virtual;
	/**
	 * Virtual address of the buffer allocated by user, used for userptr
	 * objects only.
	 */
	void *user_virtual;
	int map_count;
	struct list_head vma_list;

	/** BO cache list */
	struct list_head head;

	/**
	 * Boolean of whether this BO and its children have been included in
	 * the current drm_bacon_bufmgr_check_aperture_space() total.
	 */
	bool included_in_check_aperture;

	/**
	 * Boolean of whether this buffer has been used as a relocation
	 * target and had its size accounted for, and thus can't have any
	 * further relocations added to it.
	 */
	bool used_as_reloc_target;

	/**
	 * Boolean of whether we have encountered an error whilst building the relocation tree.
	 */
	bool has_error;

	/**
	 * Boolean of whether this buffer can be re-used
	 */
	bool reusable;

	/**
	 * Boolean of whether the GPU is definitely not accessing the buffer.
	 *
	 * This is only valid when reusable, since non-reusable
	 * buffers are those that have been shared with other
	 * processes, so we don't know their state.
	 */
	bool idle;

	/**
	 * Boolean of whether this buffer was allocated with userptr
	 */
	bool is_userptr;

	/**
	 * Size in bytes of this buffer and its relocation descendents.
	 *
	 * Used to avoid costly tree walking in
	 * drm_bacon_bufmgr_check_aperture in the common case.
	 */
	int reloc_tree_size;

	/** Flags that we may need to do the SW_FINISH ioctl on unmap. */
	bool mapped_cpu_write;
};

static unsigned int
drm_bacon_gem_estimate_batch_space(drm_bacon_bo ** bo_array, int count);

static unsigned int
drm_bacon_gem_compute_batch_space(drm_bacon_bo ** bo_array, int count);

static int
drm_bacon_gem_bo_get_tiling(drm_bacon_bo *bo, uint32_t * tiling_mode,
			    uint32_t * swizzle_mode);

static int
drm_bacon_gem_bo_set_tiling_internal(drm_bacon_bo *bo,
				     uint32_t tiling_mode,
				     uint32_t stride);

static void drm_bacon_gem_bo_unreference_locked_timed(drm_bacon_bo *bo,
						      time_t time);

static void drm_bacon_gem_bo_unreference(drm_bacon_bo *bo);

static void drm_bacon_gem_bo_free(drm_bacon_bo *bo);

static inline drm_bacon_bo_gem *to_bo_gem(drm_bacon_bo *bo)
{
        return (drm_bacon_bo_gem *)bo;
}

static unsigned long
drm_bacon_gem_bo_tile_size(drm_bacon_bufmgr_gem *bufmgr_gem, unsigned long size,
			   uint32_t *tiling_mode)
{
	if (*tiling_mode == I915_TILING_NONE)
		return size;

	/* 965+ just need multiples of page size for tiling */
	return ROUND_UP_TO(size, 4096);
}

/*
 * Round a given pitch up to the minimum required for X tiling on a
 * given chip.  We use 512 as the minimum to allow for a later tiling
 * change.
 */
static unsigned long
drm_bacon_gem_bo_tile_pitch(drm_bacon_bufmgr_gem *bufmgr_gem,
			    unsigned long pitch, uint32_t *tiling_mode)
{
	unsigned long tile_width;

	/* If untiled, then just align it so that we can do rendering
	 * to it with the 3D engine.
	 */
	if (*tiling_mode == I915_TILING_NONE)
		return ALIGN(pitch, 64);

	if (*tiling_mode == I915_TILING_X)
		tile_width = 512;
	else
		tile_width = 128;

	/* 965 is flexible */
	return ROUND_UP_TO(pitch, tile_width);
}

static struct drm_bacon_gem_bo_bucket *
drm_bacon_gem_bo_bucket_for_size(drm_bacon_bufmgr_gem *bufmgr_gem,
				 unsigned long size)
{
	int i;

	for (i = 0; i < bufmgr_gem->num_buckets; i++) {
		struct drm_bacon_gem_bo_bucket *bucket =
		    &bufmgr_gem->cache_bucket[i];
		if (bucket->size >= size) {
			return bucket;
		}
	}

	return NULL;
}

static void
drm_bacon_gem_dump_validation_list(drm_bacon_bufmgr_gem *bufmgr_gem)
{
	int i, j;

	for (i = 0; i < bufmgr_gem->exec_count; i++) {
		drm_bacon_bo *bo = bufmgr_gem->exec_bos[i];
		drm_bacon_bo_gem *bo_gem = (drm_bacon_bo_gem *) bo;

		if (bo_gem->relocs == NULL && bo_gem->softpin_target == NULL) {
			DBG("%2d: %d %s(%s)\n", i, bo_gem->gem_handle,
			    bo_gem->kflags & EXEC_OBJECT_PINNED ? "*" : "",
			    bo_gem->name);
			continue;
		}

		for (j = 0; j < bo_gem->reloc_count; j++) {
			drm_bacon_bo *target_bo = bo_gem->reloc_target_info[j].bo;
			drm_bacon_bo_gem *target_gem =
			    (drm_bacon_bo_gem *) target_bo;

			DBG("%2d: %d %s(%s)@0x%08x %08x -> "
			    "%d (%s)@0x%08x %08x + 0x%08x\n",
			    i,
			    bo_gem->gem_handle,
			    bo_gem->kflags & EXEC_OBJECT_PINNED ? "*" : "",
			    bo_gem->name,
			    upper_32_bits(bo_gem->relocs[j].offset),
			    lower_32_bits(bo_gem->relocs[j].offset),
			    target_gem->gem_handle,
			    target_gem->name,
			    upper_32_bits(target_bo->offset64),
			    lower_32_bits(target_bo->offset64),
			    bo_gem->relocs[j].delta);
		}

		for (j = 0; j < bo_gem->softpin_target_count; j++) {
			drm_bacon_bo *target_bo = bo_gem->softpin_target[j];
			drm_bacon_bo_gem *target_gem =
			    (drm_bacon_bo_gem *) target_bo;
			DBG("%2d: %d %s(%s) -> "
			    "%d *(%s)@0x%08x %08x\n",
			    i,
			    bo_gem->gem_handle,
			    bo_gem->kflags & EXEC_OBJECT_PINNED ? "*" : "",
			    bo_gem->name,
			    target_gem->gem_handle,
			    target_gem->name,
			    upper_32_bits(target_bo->offset64),
			    lower_32_bits(target_bo->offset64));
		}
	}
}

static inline void
drm_bacon_gem_bo_reference(drm_bacon_bo *bo)
{
	drm_bacon_bo_gem *bo_gem = (drm_bacon_bo_gem *) bo;

	p_atomic_inc(&bo_gem->refcount);
}

static void
drm_bacon_add_validate_buffer2(drm_bacon_bo *bo)
{
	drm_bacon_bufmgr_gem *bufmgr_gem = (drm_bacon_bufmgr_gem *)bo->bufmgr;
	drm_bacon_bo_gem *bo_gem = (drm_bacon_bo_gem *)bo;
	int index;

	if (bo_gem->validate_index != -1)
		return;

	/* Extend the array of validation entries as necessary. */
	if (bufmgr_gem->exec_count == bufmgr_gem->exec_size) {
		int new_size = bufmgr_gem->exec_size * 2;

		if (new_size == 0)
			new_size = 5;

		bufmgr_gem->exec2_objects =
			realloc(bufmgr_gem->exec2_objects,
				sizeof(*bufmgr_gem->exec2_objects) * new_size);
		bufmgr_gem->exec_bos =
			realloc(bufmgr_gem->exec_bos,
				sizeof(*bufmgr_gem->exec_bos) * new_size);
		bufmgr_gem->exec_size = new_size;
	}

	index = bufmgr_gem->exec_count;
	bo_gem->validate_index = index;
	/* Fill in array entry */
	bufmgr_gem->exec2_objects[index].handle = bo_gem->gem_handle;
	bufmgr_gem->exec2_objects[index].relocation_count = bo_gem->reloc_count;
	bufmgr_gem->exec2_objects[index].relocs_ptr = (uintptr_t)bo_gem->relocs;
	bufmgr_gem->exec2_objects[index].alignment = bo->align;
	bufmgr_gem->exec2_objects[index].offset = bo->offset64;
	bufmgr_gem->exec2_objects[index].flags = bo_gem->kflags;
	bufmgr_gem->exec2_objects[index].rsvd1 = 0;
	bufmgr_gem->exec2_objects[index].rsvd2 = 0;
	bufmgr_gem->exec_bos[index] = bo;
	bufmgr_gem->exec_count++;
}

static void
drm_bacon_bo_gem_set_in_aperture_size(drm_bacon_bufmgr_gem *bufmgr_gem,
				      drm_bacon_bo_gem *bo_gem,
				      unsigned int alignment)
{
	unsigned int size;

	assert(!bo_gem->used_as_reloc_target);

	/* The older chipsets are far-less flexible in terms of tiling,
	 * and require tiled buffer to be size aligned in the aperture.
	 * This means that in the worst possible case we will need a hole
	 * twice as large as the object in order for it to fit into the
	 * aperture. Optimal packing is for wimps.
	 */
	size = bo_gem->bo.size;

	bo_gem->reloc_tree_size = size + alignment;
}

static int
drm_bacon_setup_reloc_list(drm_bacon_bo *bo)
{
	drm_bacon_bo_gem *bo_gem = (drm_bacon_bo_gem *) bo;
	drm_bacon_bufmgr_gem *bufmgr_gem = (drm_bacon_bufmgr_gem *) bo->bufmgr;
	unsigned int max_relocs = bufmgr_gem->max_relocs;

	if (bo->size / 4 < max_relocs)
		max_relocs = bo->size / 4;

	bo_gem->relocs = malloc(max_relocs *
				sizeof(struct drm_i915_gem_relocation_entry));
	bo_gem->reloc_target_info = malloc(max_relocs *
					   sizeof(drm_bacon_reloc_target));
	if (bo_gem->relocs == NULL || bo_gem->reloc_target_info == NULL) {
		bo_gem->has_error = true;

		free (bo_gem->relocs);
		bo_gem->relocs = NULL;

		free (bo_gem->reloc_target_info);
		bo_gem->reloc_target_info = NULL;

		return 1;
	}

	return 0;
}

static int
drm_bacon_gem_bo_busy(drm_bacon_bo *bo)
{
	drm_bacon_bufmgr_gem *bufmgr_gem = (drm_bacon_bufmgr_gem *) bo->bufmgr;
	drm_bacon_bo_gem *bo_gem = (drm_bacon_bo_gem *) bo;
	struct drm_i915_gem_busy busy;
	int ret;

	if (bo_gem->reusable && bo_gem->idle)
		return false;

	memclear(busy);
	busy.handle = bo_gem->gem_handle;

	ret = drmIoctl(bufmgr_gem->fd, DRM_IOCTL_I915_GEM_BUSY, &busy);
	if (ret == 0) {
		bo_gem->idle = !busy.busy;
		return busy.busy;
	} else {
		return false;
	}
	return (ret == 0 && busy.busy);
}

static int
drm_bacon_gem_bo_madvise_internal(drm_bacon_bufmgr_gem *bufmgr_gem,
				  drm_bacon_bo_gem *bo_gem, int state)
{
	struct drm_i915_gem_madvise madv;

	memclear(madv);
	madv.handle = bo_gem->gem_handle;
	madv.madv = state;
	madv.retained = 1;
	drmIoctl(bufmgr_gem->fd, DRM_IOCTL_I915_GEM_MADVISE, &madv);

	return madv.retained;
}

static int
drm_bacon_gem_bo_madvise(drm_bacon_bo *bo, int madv)
{
	return drm_bacon_gem_bo_madvise_internal
		((drm_bacon_bufmgr_gem *) bo->bufmgr,
		 (drm_bacon_bo_gem *) bo,
		 madv);
}

/* drop the oldest entries that have been purged by the kernel */
static void
drm_bacon_gem_bo_cache_purge_bucket(drm_bacon_bufmgr_gem *bufmgr_gem,
				    struct drm_bacon_gem_bo_bucket *bucket)
{
	while (!list_empty(&bucket->head)) {
		drm_bacon_bo_gem *bo_gem;

		bo_gem = LIST_ENTRY(drm_bacon_bo_gem,
				    bucket->head.next, head);
		if (drm_bacon_gem_bo_madvise_internal
		    (bufmgr_gem, bo_gem, I915_MADV_DONTNEED))
			break;

		list_del(&bo_gem->head);
		drm_bacon_gem_bo_free(&bo_gem->bo);
	}
}

static drm_bacon_bo *
drm_bacon_gem_bo_alloc_internal(drm_bacon_bufmgr *bufmgr,
				const char *name,
				unsigned long size,
				unsigned long flags,
				uint32_t tiling_mode,
				unsigned long stride,
				unsigned int alignment)
{
	drm_bacon_bufmgr_gem *bufmgr_gem = (drm_bacon_bufmgr_gem *) bufmgr;
	drm_bacon_bo_gem *bo_gem;
	unsigned int page_size = getpagesize();
	int ret;
	struct drm_bacon_gem_bo_bucket *bucket;
	bool alloc_from_cache;
	unsigned long bo_size;
	bool for_render = false;

	if (flags & BO_ALLOC_FOR_RENDER)
		for_render = true;

	/* Round the allocated size up to a power of two number of pages. */
	bucket = drm_bacon_gem_bo_bucket_for_size(bufmgr_gem, size);

	/* If we don't have caching at this size, don't actually round the
	 * allocation up.
	 */
	if (bucket == NULL) {
		bo_size = size;
		if (bo_size < page_size)
			bo_size = page_size;
	} else {
		bo_size = bucket->size;
	}

	pthread_mutex_lock(&bufmgr_gem->lock);
	/* Get a buffer out of the cache if available */
retry:
	alloc_from_cache = false;
	if (bucket != NULL && !list_empty(&bucket->head)) {
		if (for_render) {
			/* Allocate new render-target BOs from the tail (MRU)
			 * of the list, as it will likely be hot in the GPU
			 * cache and in the aperture for us.
			 */
			bo_gem = LIST_ENTRY(drm_bacon_bo_gem,
					    bucket->head.prev, head);
			list_del(&bo_gem->head);
			alloc_from_cache = true;
			bo_gem->bo.align = alignment;
		} else {
			assert(alignment == 0);
			/* For non-render-target BOs (where we're probably
			 * going to map it first thing in order to fill it
			 * with data), check if the last BO in the cache is
			 * unbusy, and only reuse in that case. Otherwise,
			 * allocating a new buffer is probably faster than
			 * waiting for the GPU to finish.
			 */
			bo_gem = LIST_ENTRY(drm_bacon_bo_gem,
					    bucket->head.next, head);
			if (!drm_bacon_gem_bo_busy(&bo_gem->bo)) {
				alloc_from_cache = true;
				list_del(&bo_gem->head);
			}
		}

		if (alloc_from_cache) {
			if (!drm_bacon_gem_bo_madvise_internal
			    (bufmgr_gem, bo_gem, I915_MADV_WILLNEED)) {
				drm_bacon_gem_bo_free(&bo_gem->bo);
				drm_bacon_gem_bo_cache_purge_bucket(bufmgr_gem,
								    bucket);
				goto retry;
			}

			if (drm_bacon_gem_bo_set_tiling_internal(&bo_gem->bo,
								 tiling_mode,
								 stride)) {
				drm_bacon_gem_bo_free(&bo_gem->bo);
				goto retry;
			}
		}
	}

	if (!alloc_from_cache) {
		struct drm_i915_gem_create create;

		bo_gem = calloc(1, sizeof(*bo_gem));
		if (!bo_gem)
			goto err;

		/* drm_bacon_gem_bo_free calls list_del() for an uninitialized
		   list (vma_list), so better set the list head here */
		list_inithead(&bo_gem->vma_list);

		bo_gem->bo.size = bo_size;

		memclear(create);
		create.size = bo_size;

		ret = drmIoctl(bufmgr_gem->fd,
			       DRM_IOCTL_I915_GEM_CREATE,
			       &create);
		if (ret != 0) {
			free(bo_gem);
			goto err;
		}

		bo_gem->gem_handle = create.handle;
		HASH_ADD(handle_hh, bufmgr_gem->handle_table,
			 gem_handle, sizeof(bo_gem->gem_handle),
			 bo_gem);

		bo_gem->bo.handle = bo_gem->gem_handle;
		bo_gem->bo.bufmgr = bufmgr;
		bo_gem->bo.align = alignment;

		bo_gem->tiling_mode = I915_TILING_NONE;
		bo_gem->swizzle_mode = I915_BIT_6_SWIZZLE_NONE;
		bo_gem->stride = 0;

		if (drm_bacon_gem_bo_set_tiling_internal(&bo_gem->bo,
							 tiling_mode,
							 stride))
			goto err_free;
	}

	bo_gem->name = name;
	p_atomic_set(&bo_gem->refcount, 1);
	bo_gem->validate_index = -1;
	bo_gem->used_as_reloc_target = false;
	bo_gem->has_error = false;
	bo_gem->reusable = true;

	drm_bacon_bo_gem_set_in_aperture_size(bufmgr_gem, bo_gem, alignment);
	pthread_mutex_unlock(&bufmgr_gem->lock);

	DBG("bo_create: buf %d (%s) %ldb\n",
	    bo_gem->gem_handle, bo_gem->name, size);

	return &bo_gem->bo;

err_free:
	drm_bacon_gem_bo_free(&bo_gem->bo);
err:
	pthread_mutex_unlock(&bufmgr_gem->lock);
	return NULL;
}

static drm_bacon_bo *
drm_bacon_gem_bo_alloc_for_render(drm_bacon_bufmgr *bufmgr,
				  const char *name,
				  unsigned long size,
				  unsigned int alignment)
{
	return drm_bacon_gem_bo_alloc_internal(bufmgr, name, size,
					       BO_ALLOC_FOR_RENDER,
					       I915_TILING_NONE, 0,
					       alignment);
}

static drm_bacon_bo *
drm_bacon_gem_bo_alloc(drm_bacon_bufmgr *bufmgr,
		       const char *name,
		       unsigned long size,
		       unsigned int alignment)
{
	return drm_bacon_gem_bo_alloc_internal(bufmgr, name, size, 0,
					       I915_TILING_NONE, 0, 0);
}

static drm_bacon_bo *
drm_bacon_gem_bo_alloc_tiled(drm_bacon_bufmgr *bufmgr, const char *name,
			     int x, int y, int cpp, uint32_t *tiling_mode,
			     unsigned long *pitch, unsigned long flags)
{
	drm_bacon_bufmgr_gem *bufmgr_gem = (drm_bacon_bufmgr_gem *)bufmgr;
	unsigned long size, stride;
	uint32_t tiling;

	do {
		unsigned long aligned_y, height_alignment;

		tiling = *tiling_mode;

		/* If we're tiled, our allocations are in 8 or 32-row blocks,
		 * so failure to align our height means that we won't allocate
		 * enough pages.
		 *
		 * If we're untiled, we still have to align to 2 rows high
		 * because the data port accesses 2x2 blocks even if the
		 * bottom row isn't to be rendered, so failure to align means
		 * we could walk off the end of the GTT and fault.  This is
		 * documented on 965, and may be the case on older chipsets
		 * too so we try to be careful.
		 */
		aligned_y = y;
		height_alignment = 2;

		if (tiling == I915_TILING_X)
			height_alignment = 8;
		else if (tiling == I915_TILING_Y)
			height_alignment = 32;
		aligned_y = ALIGN(y, height_alignment);

		stride = x * cpp;
		stride = drm_bacon_gem_bo_tile_pitch(bufmgr_gem, stride, tiling_mode);
		size = stride * aligned_y;
		size = drm_bacon_gem_bo_tile_size(bufmgr_gem, size, tiling_mode);
	} while (*tiling_mode != tiling);
	*pitch = stride;

	if (tiling == I915_TILING_NONE)
		stride = 0;

	return drm_bacon_gem_bo_alloc_internal(bufmgr, name, size, flags,
					       tiling, stride, 0);
}

static drm_bacon_bo *
drm_bacon_gem_bo_alloc_userptr(drm_bacon_bufmgr *bufmgr,
				const char *name,
				void *addr,
				uint32_t tiling_mode,
				uint32_t stride,
				unsigned long size,
				unsigned long flags)
{
	drm_bacon_bufmgr_gem *bufmgr_gem = (drm_bacon_bufmgr_gem *) bufmgr;
	drm_bacon_bo_gem *bo_gem;
	int ret;
	struct drm_i915_gem_userptr userptr;

	/* Tiling with userptr surfaces is not supported
	 * on all hardware so refuse it for time being.
	 */
	if (tiling_mode != I915_TILING_NONE)
		return NULL;

	bo_gem = calloc(1, sizeof(*bo_gem));
	if (!bo_gem)
		return NULL;

	p_atomic_set(&bo_gem->refcount, 1);
	list_inithead(&bo_gem->vma_list);

	bo_gem->bo.size = size;

	memclear(userptr);
	userptr.user_ptr = (__u64)((unsigned long)addr);
	userptr.user_size = size;
	userptr.flags = flags;

	ret = drmIoctl(bufmgr_gem->fd,
			DRM_IOCTL_I915_GEM_USERPTR,
			&userptr);
	if (ret != 0) {
		DBG("bo_create_userptr: "
		    "ioctl failed with user ptr %p size 0x%lx, "
		    "user flags 0x%lx\n", addr, size, flags);
		free(bo_gem);
		return NULL;
	}

	pthread_mutex_lock(&bufmgr_gem->lock);

	bo_gem->gem_handle = userptr.handle;
	bo_gem->bo.handle = bo_gem->gem_handle;
	bo_gem->bo.bufmgr    = bufmgr;
	bo_gem->is_userptr   = true;
	bo_gem->bo.virtual   = addr;
	/* Save the address provided by user */
	bo_gem->user_virtual = addr;
	bo_gem->tiling_mode  = I915_TILING_NONE;
	bo_gem->swizzle_mode = I915_BIT_6_SWIZZLE_NONE;
	bo_gem->stride       = 0;

	HASH_ADD(handle_hh, bufmgr_gem->handle_table,
		 gem_handle, sizeof(bo_gem->gem_handle),
		 bo_gem);

	bo_gem->name = name;
	bo_gem->validate_index = -1;
	bo_gem->used_as_reloc_target = false;
	bo_gem->has_error = false;
	bo_gem->reusable = false;

	drm_bacon_bo_gem_set_in_aperture_size(bufmgr_gem, bo_gem, 0);
	pthread_mutex_unlock(&bufmgr_gem->lock);

	DBG("bo_create_userptr: "
	    "ptr %p buf %d (%s) size %ldb, stride 0x%x, tile mode %d\n",
		addr, bo_gem->gem_handle, bo_gem->name,
		size, stride, tiling_mode);

	return &bo_gem->bo;
}

static bool
has_userptr(drm_bacon_bufmgr_gem *bufmgr_gem)
{
	int ret;
	void *ptr;
	long pgsz;
	struct drm_i915_gem_userptr userptr;

	pgsz = sysconf(_SC_PAGESIZE);
	assert(pgsz > 0);

	ret = posix_memalign(&ptr, pgsz, pgsz);
	if (ret) {
		DBG("Failed to get a page (%ld) for userptr detection!\n",
			pgsz);
		return false;
	}

	memclear(userptr);
	userptr.user_ptr = (__u64)(unsigned long)ptr;
	userptr.user_size = pgsz;

retry:
	ret = drmIoctl(bufmgr_gem->fd, DRM_IOCTL_I915_GEM_USERPTR, &userptr);
	if (ret) {
		if (errno == ENODEV && userptr.flags == 0) {
			userptr.flags = I915_USERPTR_UNSYNCHRONIZED;
			goto retry;
		}
		free(ptr);
		return false;
	}

	/* We don't release the userptr bo here as we want to keep the
	 * kernel mm tracking alive for our lifetime. The first time we
	 * create a userptr object the kernel has to install a mmu_notifer
	 * which is a heavyweight operation (e.g. it requires taking all
	 * mm_locks and stop_machine()).
	 */

	bufmgr_gem->userptr_active.ptr = ptr;
	bufmgr_gem->userptr_active.handle = userptr.handle;

	return true;
}

static drm_bacon_bo *
check_bo_alloc_userptr(drm_bacon_bufmgr *bufmgr,
		       const char *name,
		       void *addr,
		       uint32_t tiling_mode,
		       uint32_t stride,
		       unsigned long size,
		       unsigned long flags)
{
	if (has_userptr((drm_bacon_bufmgr_gem *)bufmgr))
		bufmgr->bo_alloc_userptr = drm_bacon_gem_bo_alloc_userptr;
	else
		bufmgr->bo_alloc_userptr = NULL;

	return drm_bacon_bo_alloc_userptr(bufmgr, name, addr,
					  tiling_mode, stride, size, flags);
}

/**
 * Returns a drm_bacon_bo wrapping the given buffer object handle.
 *
 * This can be used when one application needs to pass a buffer object
 * to another.
 */
drm_bacon_bo *
drm_bacon_bo_gem_create_from_name(drm_bacon_bufmgr *bufmgr,
				  const char *name,
				  unsigned int handle)
{
	drm_bacon_bufmgr_gem *bufmgr_gem = (drm_bacon_bufmgr_gem *) bufmgr;
	drm_bacon_bo_gem *bo_gem;
	int ret;
	struct drm_gem_open open_arg;
	struct drm_i915_gem_get_tiling get_tiling;

	/* At the moment most applications only have a few named bo.
	 * For instance, in a DRI client only the render buffers passed
	 * between X and the client are named. And since X returns the
	 * alternating names for the front/back buffer a linear search
	 * provides a sufficiently fast match.
	 */
	pthread_mutex_lock(&bufmgr_gem->lock);
	HASH_FIND(name_hh, bufmgr_gem->name_table,
		  &handle, sizeof(handle), bo_gem);
	if (bo_gem) {
		drm_bacon_gem_bo_reference(&bo_gem->bo);
		goto out;
	}

	memclear(open_arg);
	open_arg.name = handle;
	ret = drmIoctl(bufmgr_gem->fd,
		       DRM_IOCTL_GEM_OPEN,
		       &open_arg);
	if (ret != 0) {
		DBG("Couldn't reference %s handle 0x%08x: %s\n",
		    name, handle, strerror(errno));
		bo_gem = NULL;
		goto out;
	}
        /* Now see if someone has used a prime handle to get this
         * object from the kernel before by looking through the list
         * again for a matching gem_handle
         */
	HASH_FIND(handle_hh, bufmgr_gem->handle_table,
		  &open_arg.handle, sizeof(open_arg.handle), bo_gem);
	if (bo_gem) {
		drm_bacon_gem_bo_reference(&bo_gem->bo);
		goto out;
	}

	bo_gem = calloc(1, sizeof(*bo_gem));
	if (!bo_gem)
		goto out;

	p_atomic_set(&bo_gem->refcount, 1);
	list_inithead(&bo_gem->vma_list);

	bo_gem->bo.size = open_arg.size;
	bo_gem->bo.offset = 0;
	bo_gem->bo.offset64 = 0;
	bo_gem->bo.virtual = NULL;
	bo_gem->bo.bufmgr = bufmgr;
	bo_gem->name = name;
	bo_gem->validate_index = -1;
	bo_gem->gem_handle = open_arg.handle;
	bo_gem->bo.handle = open_arg.handle;
	bo_gem->global_name = handle;
	bo_gem->reusable = false;

	HASH_ADD(handle_hh, bufmgr_gem->handle_table,
		 gem_handle, sizeof(bo_gem->gem_handle), bo_gem);
	HASH_ADD(name_hh, bufmgr_gem->name_table,
		 global_name, sizeof(bo_gem->global_name), bo_gem);

	memclear(get_tiling);
	get_tiling.handle = bo_gem->gem_handle;
	ret = drmIoctl(bufmgr_gem->fd,
		       DRM_IOCTL_I915_GEM_GET_TILING,
		       &get_tiling);
	if (ret != 0)
		goto err_unref;

	bo_gem->tiling_mode = get_tiling.tiling_mode;
	bo_gem->swizzle_mode = get_tiling.swizzle_mode;
	/* XXX stride is unknown */
	drm_bacon_bo_gem_set_in_aperture_size(bufmgr_gem, bo_gem, 0);
	DBG("bo_create_from_handle: %d (%s)\n", handle, bo_gem->name);

out:
	pthread_mutex_unlock(&bufmgr_gem->lock);
	return &bo_gem->bo;

err_unref:
	drm_bacon_gem_bo_free(&bo_gem->bo);
	pthread_mutex_unlock(&bufmgr_gem->lock);
	return NULL;
}

static void
drm_bacon_gem_bo_free(drm_bacon_bo *bo)
{
	drm_bacon_bufmgr_gem *bufmgr_gem = (drm_bacon_bufmgr_gem *) bo->bufmgr;
	drm_bacon_bo_gem *bo_gem = (drm_bacon_bo_gem *) bo;
	struct drm_gem_close close;
	int ret;

	list_del(&bo_gem->vma_list);
	if (bo_gem->mem_virtual) {
		VG(VALGRIND_FREELIKE_BLOCK(bo_gem->mem_virtual, 0));
		drm_munmap(bo_gem->mem_virtual, bo_gem->bo.size);
		bufmgr_gem->vma_count--;
	}
	if (bo_gem->wc_virtual) {
		VG(VALGRIND_FREELIKE_BLOCK(bo_gem->wc_virtual, 0));
		drm_munmap(bo_gem->wc_virtual, bo_gem->bo.size);
		bufmgr_gem->vma_count--;
	}
	if (bo_gem->gtt_virtual) {
		drm_munmap(bo_gem->gtt_virtual, bo_gem->bo.size);
		bufmgr_gem->vma_count--;
	}

	if (bo_gem->global_name)
		HASH_DELETE(name_hh, bufmgr_gem->name_table, bo_gem);
	HASH_DELETE(handle_hh, bufmgr_gem->handle_table, bo_gem);

	/* Close this object */
	memclear(close);
	close.handle = bo_gem->gem_handle;
	ret = drmIoctl(bufmgr_gem->fd, DRM_IOCTL_GEM_CLOSE, &close);
	if (ret != 0) {
		DBG("DRM_IOCTL_GEM_CLOSE %d failed (%s): %s\n",
		    bo_gem->gem_handle, bo_gem->name, strerror(errno));
	}
	free(bo);
}

static void
drm_bacon_gem_bo_mark_mmaps_incoherent(drm_bacon_bo *bo)
{
#if HAVE_VALGRIND
	drm_bacon_bo_gem *bo_gem = (drm_bacon_bo_gem *) bo;

	if (bo_gem->mem_virtual)
		VALGRIND_MAKE_MEM_NOACCESS(bo_gem->mem_virtual, bo->size);

	if (bo_gem->wc_virtual)
		VALGRIND_MAKE_MEM_NOACCESS(bo_gem->wc_virtual, bo->size);

	if (bo_gem->gtt_virtual)
		VALGRIND_MAKE_MEM_NOACCESS(bo_gem->gtt_virtual, bo->size);
#endif
}

/** Frees all cached buffers significantly older than @time. */
static void
drm_bacon_gem_cleanup_bo_cache(drm_bacon_bufmgr_gem *bufmgr_gem, time_t time)
{
	int i;

	if (bufmgr_gem->time == time)
		return;

	for (i = 0; i < bufmgr_gem->num_buckets; i++) {
		struct drm_bacon_gem_bo_bucket *bucket =
		    &bufmgr_gem->cache_bucket[i];

		while (!list_empty(&bucket->head)) {
			drm_bacon_bo_gem *bo_gem;

			bo_gem = LIST_ENTRY(drm_bacon_bo_gem,
					    bucket->head.next, head);
			if (time - bo_gem->free_time <= 1)
				break;

			list_del(&bo_gem->head);

			drm_bacon_gem_bo_free(&bo_gem->bo);
		}
	}

	bufmgr_gem->time = time;
}

static void drm_bacon_gem_bo_purge_vma_cache(drm_bacon_bufmgr_gem *bufmgr_gem)
{
	int limit;

	DBG("%s: cached=%d, open=%d, limit=%d\n", __FUNCTION__,
	    bufmgr_gem->vma_count, bufmgr_gem->vma_open, bufmgr_gem->vma_max);

	if (bufmgr_gem->vma_max < 0)
		return;

	/* We may need to evict a few entries in order to create new mmaps */
	limit = bufmgr_gem->vma_max - 2*bufmgr_gem->vma_open;
	if (limit < 0)
		limit = 0;

	while (bufmgr_gem->vma_count > limit) {
		drm_bacon_bo_gem *bo_gem;

		bo_gem = LIST_ENTRY(drm_bacon_bo_gem,
				    bufmgr_gem->vma_cache.next,
				    vma_list);
		assert(bo_gem->map_count == 0);
		list_delinit(&bo_gem->vma_list);

		if (bo_gem->mem_virtual) {
			drm_munmap(bo_gem->mem_virtual, bo_gem->bo.size);
			bo_gem->mem_virtual = NULL;
			bufmgr_gem->vma_count--;
		}
		if (bo_gem->wc_virtual) {
			drm_munmap(bo_gem->wc_virtual, bo_gem->bo.size);
			bo_gem->wc_virtual = NULL;
			bufmgr_gem->vma_count--;
		}
		if (bo_gem->gtt_virtual) {
			drm_munmap(bo_gem->gtt_virtual, bo_gem->bo.size);
			bo_gem->gtt_virtual = NULL;
			bufmgr_gem->vma_count--;
		}
	}
}

static void drm_bacon_gem_bo_close_vma(drm_bacon_bufmgr_gem *bufmgr_gem,
				       drm_bacon_bo_gem *bo_gem)
{
	bufmgr_gem->vma_open--;
	list_addtail(&bo_gem->vma_list, &bufmgr_gem->vma_cache);
	if (bo_gem->mem_virtual)
		bufmgr_gem->vma_count++;
	if (bo_gem->wc_virtual)
		bufmgr_gem->vma_count++;
	if (bo_gem->gtt_virtual)
		bufmgr_gem->vma_count++;
	drm_bacon_gem_bo_purge_vma_cache(bufmgr_gem);
}

static void drm_bacon_gem_bo_open_vma(drm_bacon_bufmgr_gem *bufmgr_gem,
				      drm_bacon_bo_gem *bo_gem)
{
	bufmgr_gem->vma_open++;
	list_del(&bo_gem->vma_list);
	if (bo_gem->mem_virtual)
		bufmgr_gem->vma_count--;
	if (bo_gem->wc_virtual)
		bufmgr_gem->vma_count--;
	if (bo_gem->gtt_virtual)
		bufmgr_gem->vma_count--;
	drm_bacon_gem_bo_purge_vma_cache(bufmgr_gem);
}

static void
drm_bacon_gem_bo_unreference_final(drm_bacon_bo *bo, time_t time)
{
	drm_bacon_bufmgr_gem *bufmgr_gem = (drm_bacon_bufmgr_gem *) bo->bufmgr;
	drm_bacon_bo_gem *bo_gem = (drm_bacon_bo_gem *) bo;
	struct drm_bacon_gem_bo_bucket *bucket;
	int i;

	/* Unreference all the target buffers */
	for (i = 0; i < bo_gem->reloc_count; i++) {
		if (bo_gem->reloc_target_info[i].bo != bo) {
			drm_bacon_gem_bo_unreference_locked_timed(bo_gem->
								  reloc_target_info[i].bo,
								  time);
		}
	}
	for (i = 0; i < bo_gem->softpin_target_count; i++)
		drm_bacon_gem_bo_unreference_locked_timed(bo_gem->softpin_target[i],
								  time);
	bo_gem->kflags = 0;
	bo_gem->reloc_count = 0;
	bo_gem->used_as_reloc_target = false;
	bo_gem->softpin_target_count = 0;

	DBG("bo_unreference final: %d (%s)\n",
	    bo_gem->gem_handle, bo_gem->name);

	/* release memory associated with this object */
	if (bo_gem->reloc_target_info) {
		free(bo_gem->reloc_target_info);
		bo_gem->reloc_target_info = NULL;
	}
	if (bo_gem->relocs) {
		free(bo_gem->relocs);
		bo_gem->relocs = NULL;
	}
	if (bo_gem->softpin_target) {
		free(bo_gem->softpin_target);
		bo_gem->softpin_target = NULL;
		bo_gem->softpin_target_size = 0;
	}

	/* Clear any left-over mappings */
	if (bo_gem->map_count) {
		DBG("bo freed with non-zero map-count %d\n", bo_gem->map_count);
		bo_gem->map_count = 0;
		drm_bacon_gem_bo_close_vma(bufmgr_gem, bo_gem);
		drm_bacon_gem_bo_mark_mmaps_incoherent(bo);
	}

	bucket = drm_bacon_gem_bo_bucket_for_size(bufmgr_gem, bo->size);
	/* Put the buffer into our internal cache for reuse if we can. */
	if (bufmgr_gem->bo_reuse && bo_gem->reusable && bucket != NULL &&
	    drm_bacon_gem_bo_madvise_internal(bufmgr_gem, bo_gem,
					      I915_MADV_DONTNEED)) {
		bo_gem->free_time = time;

		bo_gem->name = NULL;
		bo_gem->validate_index = -1;

		list_addtail(&bo_gem->head, &bucket->head);
	} else {
		drm_bacon_gem_bo_free(bo);
	}
}

static void drm_bacon_gem_bo_unreference_locked_timed(drm_bacon_bo *bo,
						      time_t time)
{
	drm_bacon_bo_gem *bo_gem = (drm_bacon_bo_gem *) bo;

	assert(p_atomic_read(&bo_gem->refcount) > 0);
	if (p_atomic_dec_zero(&bo_gem->refcount))
		drm_bacon_gem_bo_unreference_final(bo, time);
}

static void drm_bacon_gem_bo_unreference(drm_bacon_bo *bo)
{
	drm_bacon_bo_gem *bo_gem = (drm_bacon_bo_gem *) bo;

	assert(p_atomic_read(&bo_gem->refcount) > 0);

	if (atomic_add_unless(&bo_gem->refcount, -1, 1)) {
		drm_bacon_bufmgr_gem *bufmgr_gem =
		    (drm_bacon_bufmgr_gem *) bo->bufmgr;
		struct timespec time;

		clock_gettime(CLOCK_MONOTONIC, &time);

		pthread_mutex_lock(&bufmgr_gem->lock);

		if (p_atomic_dec_zero(&bo_gem->refcount)) {
			drm_bacon_gem_bo_unreference_final(bo, time.tv_sec);
			drm_bacon_gem_cleanup_bo_cache(bufmgr_gem, time.tv_sec);
		}

		pthread_mutex_unlock(&bufmgr_gem->lock);
	}
}

static int drm_bacon_gem_bo_map(drm_bacon_bo *bo, int write_enable)
{
	drm_bacon_bufmgr_gem *bufmgr_gem = (drm_bacon_bufmgr_gem *) bo->bufmgr;
	drm_bacon_bo_gem *bo_gem = (drm_bacon_bo_gem *) bo;
	struct drm_i915_gem_set_domain set_domain;
	int ret;

	if (bo_gem->is_userptr) {
		/* Return the same user ptr */
		bo->virtual = bo_gem->user_virtual;
		return 0;
	}

	pthread_mutex_lock(&bufmgr_gem->lock);

	if (bo_gem->map_count++ == 0)
		drm_bacon_gem_bo_open_vma(bufmgr_gem, bo_gem);

	if (!bo_gem->mem_virtual) {
		struct drm_i915_gem_mmap mmap_arg;

		DBG("bo_map: %d (%s), map_count=%d\n",
		    bo_gem->gem_handle, bo_gem->name, bo_gem->map_count);

		memclear(mmap_arg);
		mmap_arg.handle = bo_gem->gem_handle;
		mmap_arg.size = bo->size;
		ret = drmIoctl(bufmgr_gem->fd,
			       DRM_IOCTL_I915_GEM_MMAP,
			       &mmap_arg);
		if (ret != 0) {
			ret = -errno;
			DBG("%s:%d: Error mapping buffer %d (%s): %s .\n",
			    __FILE__, __LINE__, bo_gem->gem_handle,
			    bo_gem->name, strerror(errno));
			if (--bo_gem->map_count == 0)
				drm_bacon_gem_bo_close_vma(bufmgr_gem, bo_gem);
			pthread_mutex_unlock(&bufmgr_gem->lock);
			return ret;
		}
		VG(VALGRIND_MALLOCLIKE_BLOCK(mmap_arg.addr_ptr, mmap_arg.size, 0, 1));
		bo_gem->mem_virtual = (void *)(uintptr_t) mmap_arg.addr_ptr;
	}
	DBG("bo_map: %d (%s) -> %p\n", bo_gem->gem_handle, bo_gem->name,
	    bo_gem->mem_virtual);
	bo->virtual = bo_gem->mem_virtual;

	memclear(set_domain);
	set_domain.handle = bo_gem->gem_handle;
	set_domain.read_domains = I915_GEM_DOMAIN_CPU;
	if (write_enable)
		set_domain.write_domain = I915_GEM_DOMAIN_CPU;
	else
		set_domain.write_domain = 0;
	ret = drmIoctl(bufmgr_gem->fd,
		       DRM_IOCTL_I915_GEM_SET_DOMAIN,
		       &set_domain);
	if (ret != 0) {
		DBG("%s:%d: Error setting to CPU domain %d: %s\n",
		    __FILE__, __LINE__, bo_gem->gem_handle,
		    strerror(errno));
	}

	if (write_enable)
		bo_gem->mapped_cpu_write = true;

	drm_bacon_gem_bo_mark_mmaps_incoherent(bo);
	VG(VALGRIND_MAKE_MEM_DEFINED(bo_gem->mem_virtual, bo->size));
	pthread_mutex_unlock(&bufmgr_gem->lock);

	return 0;
}

static int
map_gtt(drm_bacon_bo *bo)
{
	drm_bacon_bufmgr_gem *bufmgr_gem = (drm_bacon_bufmgr_gem *) bo->bufmgr;
	drm_bacon_bo_gem *bo_gem = (drm_bacon_bo_gem *) bo;
	int ret;

	if (bo_gem->is_userptr)
		return -EINVAL;

	if (bo_gem->map_count++ == 0)
		drm_bacon_gem_bo_open_vma(bufmgr_gem, bo_gem);

	/* Get a mapping of the buffer if we haven't before. */
	if (bo_gem->gtt_virtual == NULL) {
		struct drm_i915_gem_mmap_gtt mmap_arg;

		DBG("bo_map_gtt: mmap %d (%s), map_count=%d\n",
		    bo_gem->gem_handle, bo_gem->name, bo_gem->map_count);

		memclear(mmap_arg);
		mmap_arg.handle = bo_gem->gem_handle;

		/* Get the fake offset back... */
		ret = drmIoctl(bufmgr_gem->fd,
			       DRM_IOCTL_I915_GEM_MMAP_GTT,
			       &mmap_arg);
		if (ret != 0) {
			ret = -errno;
			DBG("%s:%d: Error preparing buffer map %d (%s): %s .\n",
			    __FILE__, __LINE__,
			    bo_gem->gem_handle, bo_gem->name,
			    strerror(errno));
			if (--bo_gem->map_count == 0)
				drm_bacon_gem_bo_close_vma(bufmgr_gem, bo_gem);
			return ret;
		}

		/* and mmap it */
		bo_gem->gtt_virtual = drm_mmap(0, bo->size, PROT_READ | PROT_WRITE,
					       MAP_SHARED, bufmgr_gem->fd,
					       mmap_arg.offset);
		if (bo_gem->gtt_virtual == MAP_FAILED) {
			bo_gem->gtt_virtual = NULL;
			ret = -errno;
			DBG("%s:%d: Error mapping buffer %d (%s): %s .\n",
			    __FILE__, __LINE__,
			    bo_gem->gem_handle, bo_gem->name,
			    strerror(errno));
			if (--bo_gem->map_count == 0)
				drm_bacon_gem_bo_close_vma(bufmgr_gem, bo_gem);
			return ret;
		}
	}

	bo->virtual = bo_gem->gtt_virtual;

	DBG("bo_map_gtt: %d (%s) -> %p\n", bo_gem->gem_handle, bo_gem->name,
	    bo_gem->gtt_virtual);

	return 0;
}

int
drm_bacon_gem_bo_map_gtt(drm_bacon_bo *bo)
{
	drm_bacon_bufmgr_gem *bufmgr_gem = (drm_bacon_bufmgr_gem *) bo->bufmgr;
	drm_bacon_bo_gem *bo_gem = (drm_bacon_bo_gem *) bo;
	struct drm_i915_gem_set_domain set_domain;
	int ret;

	pthread_mutex_lock(&bufmgr_gem->lock);

	ret = map_gtt(bo);
	if (ret) {
		pthread_mutex_unlock(&bufmgr_gem->lock);
		return ret;
	}

	/* Now move it to the GTT domain so that the GPU and CPU
	 * caches are flushed and the GPU isn't actively using the
	 * buffer.
	 *
	 * The pagefault handler does this domain change for us when
	 * it has unbound the BO from the GTT, but it's up to us to
	 * tell it when we're about to use things if we had done
	 * rendering and it still happens to be bound to the GTT.
	 */
	memclear(set_domain);
	set_domain.handle = bo_gem->gem_handle;
	set_domain.read_domains = I915_GEM_DOMAIN_GTT;
	set_domain.write_domain = I915_GEM_DOMAIN_GTT;
	ret = drmIoctl(bufmgr_gem->fd,
		       DRM_IOCTL_I915_GEM_SET_DOMAIN,
		       &set_domain);
	if (ret != 0) {
		DBG("%s:%d: Error setting domain %d: %s\n",
		    __FILE__, __LINE__, bo_gem->gem_handle,
		    strerror(errno));
	}

	drm_bacon_gem_bo_mark_mmaps_incoherent(bo);
	VG(VALGRIND_MAKE_MEM_DEFINED(bo_gem->gtt_virtual, bo->size));
	pthread_mutex_unlock(&bufmgr_gem->lock);

	return 0;
}

/**
 * Performs a mapping of the buffer object like the normal GTT
 * mapping, but avoids waiting for the GPU to be done reading from or
 * rendering to the buffer.
 *
 * This is used in the implementation of GL_ARB_map_buffer_range: The
 * user asks to create a buffer, then does a mapping, fills some
 * space, runs a drawing command, then asks to map it again without
 * synchronizing because it guarantees that it won't write over the
 * data that the GPU is busy using (or, more specifically, that if it
 * does write over the data, it acknowledges that rendering is
 * undefined).
 */

int
drm_bacon_gem_bo_map_unsynchronized(drm_bacon_bo *bo)
{
	drm_bacon_bufmgr_gem *bufmgr_gem = (drm_bacon_bufmgr_gem *) bo->bufmgr;
#ifdef HAVE_VALGRIND
	drm_bacon_bo_gem *bo_gem = (drm_bacon_bo_gem *) bo;
#endif
	int ret;

	/* If the CPU cache isn't coherent with the GTT, then use a
	 * regular synchronized mapping.  The problem is that we don't
	 * track where the buffer was last used on the CPU side in
	 * terms of drm_bacon_bo_map vs drm_bacon_gem_bo_map_gtt, so
	 * we would potentially corrupt the buffer even when the user
	 * does reasonable things.
	 */
	if (!bufmgr_gem->has_llc)
		return drm_bacon_gem_bo_map_gtt(bo);

	pthread_mutex_lock(&bufmgr_gem->lock);

	ret = map_gtt(bo);
	if (ret == 0) {
		drm_bacon_gem_bo_mark_mmaps_incoherent(bo);
		VG(VALGRIND_MAKE_MEM_DEFINED(bo_gem->gtt_virtual, bo->size));
	}

	pthread_mutex_unlock(&bufmgr_gem->lock);

	return ret;
}

static int drm_bacon_gem_bo_unmap(drm_bacon_bo *bo)
{
	drm_bacon_bufmgr_gem *bufmgr_gem;
	drm_bacon_bo_gem *bo_gem = (drm_bacon_bo_gem *) bo;
	int ret = 0;

	if (bo == NULL)
		return 0;

	if (bo_gem->is_userptr)
		return 0;

	bufmgr_gem = (drm_bacon_bufmgr_gem *) bo->bufmgr;

	pthread_mutex_lock(&bufmgr_gem->lock);

	if (bo_gem->map_count <= 0) {
		DBG("attempted to unmap an unmapped bo\n");
		pthread_mutex_unlock(&bufmgr_gem->lock);
		/* Preserve the old behaviour of just treating this as a
		 * no-op rather than reporting the error.
		 */
		return 0;
	}

	if (bo_gem->mapped_cpu_write) {
		struct drm_i915_gem_sw_finish sw_finish;

		/* Cause a flush to happen if the buffer's pinned for
		 * scanout, so the results show up in a timely manner.
		 * Unlike GTT set domains, this only does work if the
		 * buffer should be scanout-related.
		 */
		memclear(sw_finish);
		sw_finish.handle = bo_gem->gem_handle;
		ret = drmIoctl(bufmgr_gem->fd,
			       DRM_IOCTL_I915_GEM_SW_FINISH,
			       &sw_finish);
		ret = ret == -1 ? -errno : 0;

		bo_gem->mapped_cpu_write = false;
	}

	/* We need to unmap after every innovation as we cannot track
	 * an open vma for every bo as that will exhaust the system
	 * limits and cause later failures.
	 */
	if (--bo_gem->map_count == 0) {
		drm_bacon_gem_bo_close_vma(bufmgr_gem, bo_gem);
		drm_bacon_gem_bo_mark_mmaps_incoherent(bo);
		bo->virtual = NULL;
	}
	pthread_mutex_unlock(&bufmgr_gem->lock);

	return ret;
}

static int
drm_bacon_gem_bo_subdata(drm_bacon_bo *bo, unsigned long offset,
			 unsigned long size, const void *data)
{
	drm_bacon_bufmgr_gem *bufmgr_gem = (drm_bacon_bufmgr_gem *) bo->bufmgr;
	drm_bacon_bo_gem *bo_gem = (drm_bacon_bo_gem *) bo;
	struct drm_i915_gem_pwrite pwrite;
	int ret;

	if (bo_gem->is_userptr)
		return -EINVAL;

	memclear(pwrite);
	pwrite.handle = bo_gem->gem_handle;
	pwrite.offset = offset;
	pwrite.size = size;
	pwrite.data_ptr = (uint64_t) (uintptr_t) data;
	ret = drmIoctl(bufmgr_gem->fd,
		       DRM_IOCTL_I915_GEM_PWRITE,
		       &pwrite);
	if (ret != 0) {
		ret = -errno;
		DBG("%s:%d: Error writing data to buffer %d: (%d %d) %s .\n",
		    __FILE__, __LINE__, bo_gem->gem_handle, (int)offset,
		    (int)size, strerror(errno));
	}

	return ret;
}

static int
drm_bacon_gem_bo_get_subdata(drm_bacon_bo *bo, unsigned long offset,
			     unsigned long size, void *data)
{
	drm_bacon_bufmgr_gem *bufmgr_gem = (drm_bacon_bufmgr_gem *) bo->bufmgr;
	drm_bacon_bo_gem *bo_gem = (drm_bacon_bo_gem *) bo;
	struct drm_i915_gem_pread pread;
	int ret;

	if (bo_gem->is_userptr)
		return -EINVAL;

	memclear(pread);
	pread.handle = bo_gem->gem_handle;
	pread.offset = offset;
	pread.size = size;
	pread.data_ptr = (uint64_t) (uintptr_t) data;
	ret = drmIoctl(bufmgr_gem->fd,
		       DRM_IOCTL_I915_GEM_PREAD,
		       &pread);
	if (ret != 0) {
		ret = -errno;
		DBG("%s:%d: Error reading data from buffer %d: (%d %d) %s .\n",
		    __FILE__, __LINE__, bo_gem->gem_handle, (int)offset,
		    (int)size, strerror(errno));
	}

	return ret;
}

/** Waits for all GPU rendering with the object to have completed. */
static void
drm_bacon_gem_bo_wait_rendering(drm_bacon_bo *bo)
{
	drm_bacon_gem_bo_start_gtt_access(bo, 1);
}

/**
 * Waits on a BO for the given amount of time.
 *
 * @bo: buffer object to wait for
 * @timeout_ns: amount of time to wait in nanoseconds.
 *   If value is less than 0, an infinite wait will occur.
 *
 * Returns 0 if the wait was successful ie. the last batch referencing the
 * object has completed within the allotted time. Otherwise some negative return
 * value describes the error. Of particular interest is -ETIME when the wait has
 * failed to yield the desired result.
 *
 * Similar to drm_bacon_gem_bo_wait_rendering except a timeout parameter allows
 * the operation to give up after a certain amount of time. Another subtle
 * difference is the internal locking semantics are different (this variant does
 * not hold the lock for the duration of the wait). This makes the wait subject
 * to a larger userspace race window.
 *
 * The implementation shall wait until the object is no longer actively
 * referenced within a batch buffer at the time of the call. The wait will
 * not guarantee that the buffer is re-issued via another thread, or an flinked
 * handle. Userspace must make sure this race does not occur if such precision
 * is important.
 *
 * Note that some kernels have broken the inifite wait for negative values
 * promise, upgrade to latest stable kernels if this is the case.
 */
int
drm_bacon_gem_bo_wait(drm_bacon_bo *bo, int64_t timeout_ns)
{
	drm_bacon_bufmgr_gem *bufmgr_gem = (drm_bacon_bufmgr_gem *) bo->bufmgr;
	drm_bacon_bo_gem *bo_gem = (drm_bacon_bo_gem *) bo;
	struct drm_i915_gem_wait wait;
	int ret;

	if (!bufmgr_gem->has_wait_timeout) {
		DBG("%s:%d: Timed wait is not supported. Falling back to "
		    "infinite wait\n", __FILE__, __LINE__);
		if (timeout_ns) {
			drm_bacon_gem_bo_wait_rendering(bo);
			return 0;
		} else {
			return drm_bacon_gem_bo_busy(bo) ? -ETIME : 0;
		}
	}

	memclear(wait);
	wait.bo_handle = bo_gem->gem_handle;
	wait.timeout_ns = timeout_ns;
	ret = drmIoctl(bufmgr_gem->fd, DRM_IOCTL_I915_GEM_WAIT, &wait);
	if (ret == -1)
		return -errno;

	return ret;
}

/**
 * Sets the object to the GTT read and possibly write domain, used by the X
 * 2D driver in the absence of kernel support to do drm_bacon_gem_bo_map_gtt().
 *
 * In combination with drm_bacon_gem_bo_pin() and manual fence management, we
 * can do tiled pixmaps this way.
 */
void
drm_bacon_gem_bo_start_gtt_access(drm_bacon_bo *bo, int write_enable)
{
	drm_bacon_bufmgr_gem *bufmgr_gem = (drm_bacon_bufmgr_gem *) bo->bufmgr;
	drm_bacon_bo_gem *bo_gem = (drm_bacon_bo_gem *) bo;
	struct drm_i915_gem_set_domain set_domain;
	int ret;

	memclear(set_domain);
	set_domain.handle = bo_gem->gem_handle;
	set_domain.read_domains = I915_GEM_DOMAIN_GTT;
	set_domain.write_domain = write_enable ? I915_GEM_DOMAIN_GTT : 0;
	ret = drmIoctl(bufmgr_gem->fd,
		       DRM_IOCTL_I915_GEM_SET_DOMAIN,
		       &set_domain);
	if (ret != 0) {
		DBG("%s:%d: Error setting memory domains %d (%08x %08x): %s .\n",
		    __FILE__, __LINE__, bo_gem->gem_handle,
		    set_domain.read_domains, set_domain.write_domain,
		    strerror(errno));
	}
}

static void
drm_bacon_bufmgr_gem_destroy(drm_bacon_bufmgr *bufmgr)
{
	drm_bacon_bufmgr_gem *bufmgr_gem = (drm_bacon_bufmgr_gem *) bufmgr;
	struct drm_gem_close close_bo;
	int i, ret;

	free(bufmgr_gem->exec2_objects);
	free(bufmgr_gem->exec_bos);

	pthread_mutex_destroy(&bufmgr_gem->lock);

	/* Free any cached buffer objects we were going to reuse */
	for (i = 0; i < bufmgr_gem->num_buckets; i++) {
		struct drm_bacon_gem_bo_bucket *bucket =
		    &bufmgr_gem->cache_bucket[i];
		drm_bacon_bo_gem *bo_gem;

		while (!list_empty(&bucket->head)) {
			bo_gem = LIST_ENTRY(drm_bacon_bo_gem,
					    bucket->head.next, head);
			list_del(&bo_gem->head);

			drm_bacon_gem_bo_free(&bo_gem->bo);
		}
	}

	/* Release userptr bo kept hanging around for optimisation. */
	if (bufmgr_gem->userptr_active.ptr) {
		memclear(close_bo);
		close_bo.handle = bufmgr_gem->userptr_active.handle;
		ret = drmIoctl(bufmgr_gem->fd, DRM_IOCTL_GEM_CLOSE, &close_bo);
		free(bufmgr_gem->userptr_active.ptr);
		if (ret)
			fprintf(stderr,
				"Failed to release test userptr object! (%d) "
				"i915 kernel driver may not be sane!\n", errno);
	}

	free(bufmgr);
}

/**
 * Adds the target buffer to the validation list and adds the relocation
 * to the reloc_buffer's relocation list.
 *
 * The relocation entry at the given offset must already contain the
 * precomputed relocation value, because the kernel will optimize out
 * the relocation entry write when the buffer hasn't moved from the
 * last known offset in target_bo.
 */
static int
do_bo_emit_reloc(drm_bacon_bo *bo, uint32_t offset,
		 drm_bacon_bo *target_bo, uint32_t target_offset,
		 uint32_t read_domains, uint32_t write_domain)
{
	drm_bacon_bufmgr_gem *bufmgr_gem = (drm_bacon_bufmgr_gem *) bo->bufmgr;
	drm_bacon_bo_gem *bo_gem = (drm_bacon_bo_gem *) bo;
	drm_bacon_bo_gem *target_bo_gem = (drm_bacon_bo_gem *) target_bo;

	if (bo_gem->has_error)
		return -ENOMEM;

	if (target_bo_gem->has_error) {
		bo_gem->has_error = true;
		return -ENOMEM;
	}

	/* Create a new relocation list if needed */
	if (bo_gem->relocs == NULL && drm_bacon_setup_reloc_list(bo))
		return -ENOMEM;

	/* Check overflow */
	assert(bo_gem->reloc_count < bufmgr_gem->max_relocs);

	/* Check args */
	assert(offset <= bo->size - 4);
	assert((write_domain & (write_domain - 1)) == 0);

	/* Make sure that we're not adding a reloc to something whose size has
	 * already been accounted for.
	 */
	assert(!bo_gem->used_as_reloc_target);
	if (target_bo_gem != bo_gem) {
		target_bo_gem->used_as_reloc_target = true;
		bo_gem->reloc_tree_size += target_bo_gem->reloc_tree_size;
	}

	bo_gem->reloc_target_info[bo_gem->reloc_count].bo = target_bo;
	if (target_bo != bo)
		drm_bacon_gem_bo_reference(target_bo);

	bo_gem->relocs[bo_gem->reloc_count].offset = offset;
	bo_gem->relocs[bo_gem->reloc_count].delta = target_offset;
	bo_gem->relocs[bo_gem->reloc_count].target_handle =
	    target_bo_gem->gem_handle;
	bo_gem->relocs[bo_gem->reloc_count].read_domains = read_domains;
	bo_gem->relocs[bo_gem->reloc_count].write_domain = write_domain;
	bo_gem->relocs[bo_gem->reloc_count].presumed_offset = target_bo->offset64;
	bo_gem->reloc_count++;

	return 0;
}

static int
drm_bacon_gem_bo_add_softpin_target(drm_bacon_bo *bo, drm_bacon_bo *target_bo)
{
	drm_bacon_bufmgr_gem *bufmgr_gem = (drm_bacon_bufmgr_gem *) bo->bufmgr;
	drm_bacon_bo_gem *bo_gem = (drm_bacon_bo_gem *) bo;
	drm_bacon_bo_gem *target_bo_gem = (drm_bacon_bo_gem *) target_bo;
	if (bo_gem->has_error)
		return -ENOMEM;

	if (target_bo_gem->has_error) {
		bo_gem->has_error = true;
		return -ENOMEM;
	}

	if (!(target_bo_gem->kflags & EXEC_OBJECT_PINNED))
		return -EINVAL;
	if (target_bo_gem == bo_gem)
		return -EINVAL;

	if (bo_gem->softpin_target_count == bo_gem->softpin_target_size) {
		int new_size = bo_gem->softpin_target_size * 2;
		if (new_size == 0)
			new_size = bufmgr_gem->max_relocs;

		bo_gem->softpin_target = realloc(bo_gem->softpin_target, new_size *
				sizeof(drm_bacon_bo *));
		if (!bo_gem->softpin_target)
			return -ENOMEM;

		bo_gem->softpin_target_size = new_size;
	}
	bo_gem->softpin_target[bo_gem->softpin_target_count] = target_bo;
	drm_bacon_gem_bo_reference(target_bo);
	bo_gem->softpin_target_count++;

	return 0;
}

static int
drm_bacon_gem_bo_emit_reloc(drm_bacon_bo *bo, uint32_t offset,
			    drm_bacon_bo *target_bo, uint32_t target_offset,
			    uint32_t read_domains, uint32_t write_domain)
{
	drm_bacon_bo_gem *target_bo_gem = (drm_bacon_bo_gem *)target_bo;

	if (target_bo_gem->kflags & EXEC_OBJECT_PINNED)
		return drm_bacon_gem_bo_add_softpin_target(bo, target_bo);
	else
		return do_bo_emit_reloc(bo, offset, target_bo, target_offset,
					read_domains, write_domain);
}

int
drm_bacon_gem_bo_get_reloc_count(drm_bacon_bo *bo)
{
	drm_bacon_bo_gem *bo_gem = (drm_bacon_bo_gem *) bo;

	return bo_gem->reloc_count;
}

/**
 * Removes existing relocation entries in the BO after "start".
 *
 * This allows a user to avoid a two-step process for state setup with
 * counting up all the buffer objects and doing a
 * drm_bacon_bufmgr_check_aperture_space() before emitting any of the
 * relocations for the state setup.  Instead, save the state of the
 * batchbuffer including drm_bacon_gem_get_reloc_count(), emit all the
 * state, and then check if it still fits in the aperture.
 *
 * Any further drm_bacon_bufmgr_check_aperture_space() queries
 * involving this buffer in the tree are undefined after this call.
 *
 * This also removes all softpinned targets being referenced by the BO.
 */
void
drm_bacon_gem_bo_clear_relocs(drm_bacon_bo *bo, int start)
{
	drm_bacon_bufmgr_gem *bufmgr_gem = (drm_bacon_bufmgr_gem *) bo->bufmgr;
	drm_bacon_bo_gem *bo_gem = (drm_bacon_bo_gem *) bo;
	int i;
	struct timespec time;

	clock_gettime(CLOCK_MONOTONIC, &time);

	assert(bo_gem->reloc_count >= start);

	/* Unreference the cleared target buffers */
	pthread_mutex_lock(&bufmgr_gem->lock);

	for (i = start; i < bo_gem->reloc_count; i++) {
		drm_bacon_bo_gem *target_bo_gem = (drm_bacon_bo_gem *) bo_gem->reloc_target_info[i].bo;
		if (&target_bo_gem->bo != bo) {
			drm_bacon_gem_bo_unreference_locked_timed(&target_bo_gem->bo,
								  time.tv_sec);
		}
	}
	bo_gem->reloc_count = start;

	for (i = 0; i < bo_gem->softpin_target_count; i++) {
		drm_bacon_bo_gem *target_bo_gem = (drm_bacon_bo_gem *) bo_gem->softpin_target[i];
		drm_bacon_gem_bo_unreference_locked_timed(&target_bo_gem->bo, time.tv_sec);
	}
	bo_gem->softpin_target_count = 0;

	pthread_mutex_unlock(&bufmgr_gem->lock);

}

static void
drm_bacon_gem_bo_process_reloc2(drm_bacon_bo *bo)
{
	drm_bacon_bo_gem *bo_gem = (drm_bacon_bo_gem *)bo;
	int i;

	if (bo_gem->relocs == NULL && bo_gem->softpin_target == NULL)
		return;

	for (i = 0; i < bo_gem->reloc_count; i++) {
		drm_bacon_bo *target_bo = bo_gem->reloc_target_info[i].bo;

		if (target_bo == bo)
			continue;

		drm_bacon_gem_bo_mark_mmaps_incoherent(bo);

		/* Continue walking the tree depth-first. */
		drm_bacon_gem_bo_process_reloc2(target_bo);

		/* Add the target to the validate list */
		drm_bacon_add_validate_buffer2(target_bo);
	}

	for (i = 0; i < bo_gem->softpin_target_count; i++) {
		drm_bacon_bo *target_bo = bo_gem->softpin_target[i];

		if (target_bo == bo)
			continue;

		drm_bacon_gem_bo_mark_mmaps_incoherent(bo);
		drm_bacon_gem_bo_process_reloc2(target_bo);
		drm_bacon_add_validate_buffer2(target_bo);
	}
}

static void
drm_bacon_update_buffer_offsets2 (drm_bacon_bufmgr_gem *bufmgr_gem)
{
	int i;

	for (i = 0; i < bufmgr_gem->exec_count; i++) {
		drm_bacon_bo *bo = bufmgr_gem->exec_bos[i];
		drm_bacon_bo_gem *bo_gem = (drm_bacon_bo_gem *)bo;

		/* Update the buffer offset */
		if (bufmgr_gem->exec2_objects[i].offset != bo->offset64) {
			/* If we're seeing softpinned object here it means that the kernel
			 * has relocated our object... Indicating a programming error
			 */
			assert(!(bo_gem->kflags & EXEC_OBJECT_PINNED));
			DBG("BO %d (%s) migrated: 0x%08x %08x -> 0x%08x %08x\n",
			    bo_gem->gem_handle, bo_gem->name,
			    upper_32_bits(bo->offset64),
			    lower_32_bits(bo->offset64),
			    upper_32_bits(bufmgr_gem->exec2_objects[i].offset),
			    lower_32_bits(bufmgr_gem->exec2_objects[i].offset));
			bo->offset64 = bufmgr_gem->exec2_objects[i].offset;
			bo->offset = bufmgr_gem->exec2_objects[i].offset;
		}
	}
}

static int
do_exec2(drm_bacon_bo *bo, int used, drm_bacon_context *ctx,
	 drm_clip_rect_t *cliprects, int num_cliprects, int DR4,
	 int in_fence, int *out_fence,
	 unsigned int flags)
{
	drm_bacon_bufmgr_gem *bufmgr_gem = (drm_bacon_bufmgr_gem *)bo->bufmgr;
	struct drm_i915_gem_execbuffer2 execbuf;
	int ret = 0;
	int i;

	if (to_bo_gem(bo)->has_error)
		return -ENOMEM;

	switch (flags & 0x7) {
	default:
		return -EINVAL;
	case I915_EXEC_BLT:
		if (!bufmgr_gem->has_blt)
			return -EINVAL;
		break;
	case I915_EXEC_BSD:
		if (!bufmgr_gem->has_bsd)
			return -EINVAL;
		break;
	case I915_EXEC_VEBOX:
		if (!bufmgr_gem->has_vebox)
			return -EINVAL;
		break;
	case I915_EXEC_RENDER:
	case I915_EXEC_DEFAULT:
		break;
	}

	pthread_mutex_lock(&bufmgr_gem->lock);
	/* Update indices and set up the validate list. */
	drm_bacon_gem_bo_process_reloc2(bo);

	/* Add the batch buffer to the validation list.  There are no relocations
	 * pointing to it.
	 */
	drm_bacon_add_validate_buffer2(bo);

	memclear(execbuf);
	execbuf.buffers_ptr = (uintptr_t)bufmgr_gem->exec2_objects;
	execbuf.buffer_count = bufmgr_gem->exec_count;
	execbuf.batch_start_offset = 0;
	execbuf.batch_len = used;
	execbuf.cliprects_ptr = (uintptr_t)cliprects;
	execbuf.num_cliprects = num_cliprects;
	execbuf.DR1 = 0;
	execbuf.DR4 = DR4;
	execbuf.flags = flags;
	if (ctx == NULL)
		i915_execbuffer2_set_context_id(execbuf, 0);
	else
		i915_execbuffer2_set_context_id(execbuf, ctx->ctx_id);
	execbuf.rsvd2 = 0;
	if (in_fence != -1) {
		execbuf.rsvd2 = in_fence;
		execbuf.flags |= I915_EXEC_FENCE_IN;
	}
	if (out_fence != NULL) {
		*out_fence = -1;
		execbuf.flags |= I915_EXEC_FENCE_OUT;
	}

	if (bufmgr_gem->no_exec)
		goto skip_execution;

	ret = drmIoctl(bufmgr_gem->fd,
		       DRM_IOCTL_I915_GEM_EXECBUFFER2_WR,
		       &execbuf);
	if (ret != 0) {
		ret = -errno;
		if (ret == -ENOSPC) {
			DBG("Execbuffer fails to pin. "
			    "Estimate: %u. Actual: %u. Available: %u\n",
			    drm_bacon_gem_estimate_batch_space(bufmgr_gem->exec_bos,
							       bufmgr_gem->exec_count),
			    drm_bacon_gem_compute_batch_space(bufmgr_gem->exec_bos,
							      bufmgr_gem->exec_count),
			    (unsigned int) bufmgr_gem->gtt_size);
		}
	}
	drm_bacon_update_buffer_offsets2(bufmgr_gem);

	if (ret == 0 && out_fence != NULL)
		*out_fence = execbuf.rsvd2 >> 32;

skip_execution:
	if (bufmgr_gem->bufmgr.debug)
		drm_bacon_gem_dump_validation_list(bufmgr_gem);

	for (i = 0; i < bufmgr_gem->exec_count; i++) {
		drm_bacon_bo_gem *bo_gem = to_bo_gem(bufmgr_gem->exec_bos[i]);

		bo_gem->idle = false;

		/* Disconnect the buffer from the validate list */
		bo_gem->validate_index = -1;
		bufmgr_gem->exec_bos[i] = NULL;
	}
	bufmgr_gem->exec_count = 0;
	pthread_mutex_unlock(&bufmgr_gem->lock);

	return ret;
}

static int
drm_bacon_gem_bo_exec2(drm_bacon_bo *bo, int used,
		       drm_clip_rect_t *cliprects, int num_cliprects,
		       int DR4)
{
	return do_exec2(bo, used, NULL, cliprects, num_cliprects, DR4,
			-1, NULL, I915_EXEC_RENDER);
}

static int
drm_bacon_gem_bo_mrb_exec2(drm_bacon_bo *bo, int used,
			drm_clip_rect_t *cliprects, int num_cliprects, int DR4,
			unsigned int flags)
{
	return do_exec2(bo, used, NULL, cliprects, num_cliprects, DR4,
			-1, NULL, flags);
}

int
drm_bacon_gem_bo_context_exec(drm_bacon_bo *bo, drm_bacon_context *ctx,
			      int used, unsigned int flags)
{
	return do_exec2(bo, used, ctx, NULL, 0, 0, -1, NULL, flags);
}

int
drm_bacon_gem_bo_fence_exec(drm_bacon_bo *bo,
			    drm_bacon_context *ctx,
			    int used,
			    int in_fence,
			    int *out_fence,
			    unsigned int flags)
{
	return do_exec2(bo, used, ctx, NULL, 0, 0, in_fence, out_fence, flags);
}

static int
drm_bacon_gem_bo_set_tiling_internal(drm_bacon_bo *bo,
				     uint32_t tiling_mode,
				     uint32_t stride)
{
	drm_bacon_bufmgr_gem *bufmgr_gem = (drm_bacon_bufmgr_gem *) bo->bufmgr;
	drm_bacon_bo_gem *bo_gem = (drm_bacon_bo_gem *) bo;
	struct drm_i915_gem_set_tiling set_tiling;
	int ret;

	if (bo_gem->global_name == 0 &&
	    tiling_mode == bo_gem->tiling_mode &&
	    stride == bo_gem->stride)
		return 0;

	memset(&set_tiling, 0, sizeof(set_tiling));
	do {
		/* set_tiling is slightly broken and overwrites the
		 * input on the error path, so we have to open code
		 * rmIoctl.
		 */
		set_tiling.handle = bo_gem->gem_handle;
		set_tiling.tiling_mode = tiling_mode;
		set_tiling.stride = stride;

		ret = ioctl(bufmgr_gem->fd,
			    DRM_IOCTL_I915_GEM_SET_TILING,
			    &set_tiling);
	} while (ret == -1 && (errno == EINTR || errno == EAGAIN));
	if (ret == -1)
		return -errno;

	bo_gem->tiling_mode = set_tiling.tiling_mode;
	bo_gem->swizzle_mode = set_tiling.swizzle_mode;
	bo_gem->stride = set_tiling.stride;
	return 0;
}

static int
drm_bacon_gem_bo_set_tiling(drm_bacon_bo *bo, uint32_t * tiling_mode,
			    uint32_t stride)
{
	drm_bacon_bufmgr_gem *bufmgr_gem = (drm_bacon_bufmgr_gem *) bo->bufmgr;
	drm_bacon_bo_gem *bo_gem = (drm_bacon_bo_gem *) bo;
	int ret;

	/* Tiling with userptr surfaces is not supported
	 * on all hardware so refuse it for time being.
	 */
	if (bo_gem->is_userptr)
		return -EINVAL;

	/* Linear buffers have no stride. By ensuring that we only ever use
	 * stride 0 with linear buffers, we simplify our code.
	 */
	if (*tiling_mode == I915_TILING_NONE)
		stride = 0;

	ret = drm_bacon_gem_bo_set_tiling_internal(bo, *tiling_mode, stride);
	if (ret == 0)
		drm_bacon_bo_gem_set_in_aperture_size(bufmgr_gem, bo_gem, 0);

	*tiling_mode = bo_gem->tiling_mode;
	return ret;
}

static int
drm_bacon_gem_bo_get_tiling(drm_bacon_bo *bo, uint32_t * tiling_mode,
			    uint32_t * swizzle_mode)
{
	drm_bacon_bo_gem *bo_gem = (drm_bacon_bo_gem *) bo;

	*tiling_mode = bo_gem->tiling_mode;
	*swizzle_mode = bo_gem->swizzle_mode;
	return 0;
}

static int
drm_bacon_gem_bo_set_softpin_offset(drm_bacon_bo *bo, uint64_t offset)
{
	drm_bacon_bo_gem *bo_gem = (drm_bacon_bo_gem *) bo;

	bo->offset64 = offset;
	bo->offset = offset;
	bo_gem->kflags |= EXEC_OBJECT_PINNED;

	return 0;
}

drm_bacon_bo *
drm_bacon_bo_gem_create_from_prime(drm_bacon_bufmgr *bufmgr, int prime_fd, int size)
{
	drm_bacon_bufmgr_gem *bufmgr_gem = (drm_bacon_bufmgr_gem *) bufmgr;
	int ret;
	uint32_t handle;
	drm_bacon_bo_gem *bo_gem;
	struct drm_i915_gem_get_tiling get_tiling;

	pthread_mutex_lock(&bufmgr_gem->lock);
	ret = drmPrimeFDToHandle(bufmgr_gem->fd, prime_fd, &handle);
	if (ret) {
		DBG("create_from_prime: failed to obtain handle from fd: %s\n", strerror(errno));
		pthread_mutex_unlock(&bufmgr_gem->lock);
		return NULL;
	}

	/*
	 * See if the kernel has already returned this buffer to us. Just as
	 * for named buffers, we must not create two bo's pointing at the same
	 * kernel object
	 */
	HASH_FIND(handle_hh, bufmgr_gem->handle_table,
		  &handle, sizeof(handle), bo_gem);
	if (bo_gem) {
		drm_bacon_gem_bo_reference(&bo_gem->bo);
		goto out;
	}

	bo_gem = calloc(1, sizeof(*bo_gem));
	if (!bo_gem)
		goto out;

	p_atomic_set(&bo_gem->refcount, 1);
	list_inithead(&bo_gem->vma_list);

	/* Determine size of bo.  The fd-to-handle ioctl really should
	 * return the size, but it doesn't.  If we have kernel 3.12 or
	 * later, we can lseek on the prime fd to get the size.  Older
	 * kernels will just fail, in which case we fall back to the
	 * provided (estimated or guess size). */
	ret = lseek(prime_fd, 0, SEEK_END);
	if (ret != -1)
		bo_gem->bo.size = ret;
	else
		bo_gem->bo.size = size;

	bo_gem->bo.handle = handle;
	bo_gem->bo.bufmgr = bufmgr;

	bo_gem->gem_handle = handle;
	HASH_ADD(handle_hh, bufmgr_gem->handle_table,
		 gem_handle, sizeof(bo_gem->gem_handle), bo_gem);

	bo_gem->name = "prime";
	bo_gem->validate_index = -1;
	bo_gem->used_as_reloc_target = false;
	bo_gem->has_error = false;
	bo_gem->reusable = false;

	memclear(get_tiling);
	get_tiling.handle = bo_gem->gem_handle;
	if (drmIoctl(bufmgr_gem->fd,
		     DRM_IOCTL_I915_GEM_GET_TILING,
		     &get_tiling))
		goto err;

	bo_gem->tiling_mode = get_tiling.tiling_mode;
	bo_gem->swizzle_mode = get_tiling.swizzle_mode;
	/* XXX stride is unknown */
	drm_bacon_bo_gem_set_in_aperture_size(bufmgr_gem, bo_gem, 0);

out:
	pthread_mutex_unlock(&bufmgr_gem->lock);
	return &bo_gem->bo;

err:
	drm_bacon_gem_bo_free(&bo_gem->bo);
	pthread_mutex_unlock(&bufmgr_gem->lock);
	return NULL;
}

int
drm_bacon_bo_gem_export_to_prime(drm_bacon_bo *bo, int *prime_fd)
{
	drm_bacon_bufmgr_gem *bufmgr_gem = (drm_bacon_bufmgr_gem *) bo->bufmgr;
	drm_bacon_bo_gem *bo_gem = (drm_bacon_bo_gem *) bo;

	if (drmPrimeHandleToFD(bufmgr_gem->fd, bo_gem->gem_handle,
			       DRM_CLOEXEC, prime_fd) != 0)
		return -errno;

	bo_gem->reusable = false;

	return 0;
}

static int
drm_bacon_gem_bo_flink(drm_bacon_bo *bo, uint32_t * name)
{
	drm_bacon_bufmgr_gem *bufmgr_gem = (drm_bacon_bufmgr_gem *) bo->bufmgr;
	drm_bacon_bo_gem *bo_gem = (drm_bacon_bo_gem *) bo;

	if (!bo_gem->global_name) {
		struct drm_gem_flink flink;

		memclear(flink);
		flink.handle = bo_gem->gem_handle;
		if (drmIoctl(bufmgr_gem->fd, DRM_IOCTL_GEM_FLINK, &flink))
			return -errno;

		pthread_mutex_lock(&bufmgr_gem->lock);
		if (!bo_gem->global_name) {
			bo_gem->global_name = flink.name;
			bo_gem->reusable = false;

			HASH_ADD(name_hh, bufmgr_gem->name_table,
				 global_name, sizeof(bo_gem->global_name),
				 bo_gem);
		}
		pthread_mutex_unlock(&bufmgr_gem->lock);
	}

	*name = bo_gem->global_name;
	return 0;
}

/**
 * Enables unlimited caching of buffer objects for reuse.
 *
 * This is potentially very memory expensive, as the cache at each bucket
 * size is only bounded by how many buffers of that size we've managed to have
 * in flight at once.
 */
void
drm_bacon_bufmgr_gem_enable_reuse(drm_bacon_bufmgr *bufmgr)
{
	drm_bacon_bufmgr_gem *bufmgr_gem = (drm_bacon_bufmgr_gem *) bufmgr;

	bufmgr_gem->bo_reuse = true;
}

/**
 * Disables implicit synchronisation before executing the bo
 *
 * This will cause rendering corruption unless you correctly manage explicit
 * fences for all rendering involving this buffer - including use by others.
 * Disabling the implicit serialisation is only required if that serialisation
 * is too coarse (for example, you have split the buffer into many
 * non-overlapping regions and are sharing the whole buffer between concurrent
 * independent command streams).
 *
 * Note the kernel must advertise support via I915_PARAM_HAS_EXEC_ASYNC,
 * which can be checked using drm_bacon_bufmgr_can_disable_implicit_sync,
 * or subsequent execbufs involving the bo will generate EINVAL.
 */
void
drm_bacon_gem_bo_disable_implicit_sync(drm_bacon_bo *bo)
{
	drm_bacon_bo_gem *bo_gem = (drm_bacon_bo_gem *) bo;

	bo_gem->kflags |= EXEC_OBJECT_ASYNC;
}

/**
 * Enables implicit synchronisation before executing the bo
 *
 * This is the default behaviour of the kernel, to wait upon prior writes
 * completing on the object before rendering with it, or to wait for prior
 * reads to complete before writing into the object.
 * drm_bacon_gem_bo_disable_implicit_sync() can stop this behaviour, telling
 * the kernel never to insert a stall before using the object. Then this
 * function can be used to restore the implicit sync before subsequent
 * rendering.
 */
void
drm_bacon_gem_bo_enable_implicit_sync(drm_bacon_bo *bo)
{
	drm_bacon_bo_gem *bo_gem = (drm_bacon_bo_gem *) bo;

	bo_gem->kflags &= ~EXEC_OBJECT_ASYNC;
}

/**
 * Query whether the kernel supports disabling of its implicit synchronisation
 * before execbuf. See drm_bacon_gem_bo_disable_implicit_sync()
 */
int
drm_bacon_bufmgr_gem_can_disable_implicit_sync(drm_bacon_bufmgr *bufmgr)
{
	drm_bacon_bufmgr_gem *bufmgr_gem = (drm_bacon_bufmgr_gem *) bufmgr;

	return bufmgr_gem->has_exec_async;
}

/**
 * Return the additional aperture space required by the tree of buffer objects
 * rooted at bo.
 */
static int
drm_bacon_gem_bo_get_aperture_space(drm_bacon_bo *bo)
{
	drm_bacon_bo_gem *bo_gem = (drm_bacon_bo_gem *) bo;
	int i;
	int total = 0;

	if (bo == NULL || bo_gem->included_in_check_aperture)
		return 0;

	total += bo->size;
	bo_gem->included_in_check_aperture = true;

	for (i = 0; i < bo_gem->reloc_count; i++)
		total +=
		    drm_bacon_gem_bo_get_aperture_space(bo_gem->
							reloc_target_info[i].bo);

	return total;
}

/**
 * Clear the flag set by drm_bacon_gem_bo_get_aperture_space() so we're ready
 * for the next drm_bacon_bufmgr_check_aperture_space() call.
 */
static void
drm_bacon_gem_bo_clear_aperture_space_flag(drm_bacon_bo *bo)
{
	drm_bacon_bo_gem *bo_gem = (drm_bacon_bo_gem *) bo;
	int i;

	if (bo == NULL || !bo_gem->included_in_check_aperture)
		return;

	bo_gem->included_in_check_aperture = false;

	for (i = 0; i < bo_gem->reloc_count; i++)
		drm_bacon_gem_bo_clear_aperture_space_flag(bo_gem->
							   reloc_target_info[i].bo);
}

/**
 * Return a conservative estimate for the amount of aperture required
 * for a collection of buffers. This may double-count some buffers.
 */
static unsigned int
drm_bacon_gem_estimate_batch_space(drm_bacon_bo **bo_array, int count)
{
	int i;
	unsigned int total = 0;

	for (i = 0; i < count; i++) {
		drm_bacon_bo_gem *bo_gem = (drm_bacon_bo_gem *) bo_array[i];
		if (bo_gem != NULL)
			total += bo_gem->reloc_tree_size;
	}
	return total;
}

/**
 * Return the amount of aperture needed for a collection of buffers.
 * This avoids double counting any buffers, at the cost of looking
 * at every buffer in the set.
 */
static unsigned int
drm_bacon_gem_compute_batch_space(drm_bacon_bo **bo_array, int count)
{
	int i;
	unsigned int total = 0;

	for (i = 0; i < count; i++) {
		total += drm_bacon_gem_bo_get_aperture_space(bo_array[i]);
		/* For the first buffer object in the array, we get an
		 * accurate count back for its reloc_tree size (since nothing
		 * had been flagged as being counted yet).  We can save that
		 * value out as a more conservative reloc_tree_size that
		 * avoids double-counting target buffers.  Since the first
		 * buffer happens to usually be the batch buffer in our
		 * callers, this can pull us back from doing the tree
		 * walk on every new batch emit.
		 */
		if (i == 0) {
			drm_bacon_bo_gem *bo_gem =
			    (drm_bacon_bo_gem *) bo_array[i];
			bo_gem->reloc_tree_size = total;
		}
	}

	for (i = 0; i < count; i++)
		drm_bacon_gem_bo_clear_aperture_space_flag(bo_array[i]);
	return total;
}

/**
 * Return -1 if the batchbuffer should be flushed before attempting to
 * emit rendering referencing the buffers pointed to by bo_array.
 *
 * This is required because if we try to emit a batchbuffer with relocations
 * to a tree of buffers that won't simultaneously fit in the aperture,
 * the rendering will return an error at a point where the software is not
 * prepared to recover from it.
 *
 * However, we also want to emit the batchbuffer significantly before we reach
 * the limit, as a series of batchbuffers each of which references buffers
 * covering almost all of the aperture means that at each emit we end up
 * waiting to evict a buffer from the last rendering, and we get synchronous
 * performance.  By emitting smaller batchbuffers, we eat some CPU overhead to
 * get better parallelism.
 */
static int
drm_bacon_gem_check_aperture_space(drm_bacon_bo **bo_array, int count)
{
	drm_bacon_bufmgr_gem *bufmgr_gem =
	    (drm_bacon_bufmgr_gem *) bo_array[0]->bufmgr;
	unsigned int total = 0;
	unsigned int threshold = bufmgr_gem->gtt_size * 3 / 4;

	total = drm_bacon_gem_estimate_batch_space(bo_array, count);

	if (total > threshold)
		total = drm_bacon_gem_compute_batch_space(bo_array, count);

	if (total > threshold) {
		DBG("check_space: overflowed available aperture, "
		    "%dkb vs %dkb\n",
		    total / 1024, (int)bufmgr_gem->gtt_size / 1024);
		return -ENOSPC;
	} else {
		DBG("drm_check_space: total %dkb vs bufgr %dkb\n", total / 1024,
		    (int)bufmgr_gem->gtt_size / 1024);
		return 0;
	}
}

/*
 * Disable buffer reuse for objects which are shared with the kernel
 * as scanout buffers
 */
static int
drm_bacon_gem_bo_disable_reuse(drm_bacon_bo *bo)
{
	drm_bacon_bo_gem *bo_gem = (drm_bacon_bo_gem *) bo;

	bo_gem->reusable = false;
	return 0;
}

static int
drm_bacon_gem_bo_is_reusable(drm_bacon_bo *bo)
{
	drm_bacon_bo_gem *bo_gem = (drm_bacon_bo_gem *) bo;

	return bo_gem->reusable;
}

static int
_drm_bacon_gem_bo_references(drm_bacon_bo *bo, drm_bacon_bo *target_bo)
{
	drm_bacon_bo_gem *bo_gem = (drm_bacon_bo_gem *) bo;
	int i;

	for (i = 0; i < bo_gem->reloc_count; i++) {
		if (bo_gem->reloc_target_info[i].bo == target_bo)
			return 1;
		if (bo == bo_gem->reloc_target_info[i].bo)
			continue;
		if (_drm_bacon_gem_bo_references(bo_gem->reloc_target_info[i].bo,
						target_bo))
			return 1;
	}

	for (i = 0; i< bo_gem->softpin_target_count; i++) {
		if (bo_gem->softpin_target[i] == target_bo)
			return 1;
		if (_drm_bacon_gem_bo_references(bo_gem->softpin_target[i], target_bo))
			return 1;
	}

	return 0;
}

/** Return true if target_bo is referenced by bo's relocation tree. */
static int
drm_bacon_gem_bo_references(drm_bacon_bo *bo, drm_bacon_bo *target_bo)
{
	drm_bacon_bo_gem *target_bo_gem = (drm_bacon_bo_gem *) target_bo;

	if (bo == NULL || target_bo == NULL)
		return 0;
	if (target_bo_gem->used_as_reloc_target)
		return _drm_bacon_gem_bo_references(bo, target_bo);
	return 0;
}

static void
add_bucket(drm_bacon_bufmgr_gem *bufmgr_gem, int size)
{
	unsigned int i = bufmgr_gem->num_buckets;

	assert(i < ARRAY_SIZE(bufmgr_gem->cache_bucket));

	list_inithead(&bufmgr_gem->cache_bucket[i].head);
	bufmgr_gem->cache_bucket[i].size = size;
	bufmgr_gem->num_buckets++;
}

static void
init_cache_buckets(drm_bacon_bufmgr_gem *bufmgr_gem)
{
	unsigned long size, cache_max_size = 64 * 1024 * 1024;

	/* OK, so power of two buckets was too wasteful of memory.
	 * Give 3 other sizes between each power of two, to hopefully
	 * cover things accurately enough.  (The alternative is
	 * probably to just go for exact matching of sizes, and assume
	 * that for things like composited window resize the tiled
	 * width/height alignment and rounding of sizes to pages will
	 * get us useful cache hit rates anyway)
	 */
	add_bucket(bufmgr_gem, 4096);
	add_bucket(bufmgr_gem, 4096 * 2);
	add_bucket(bufmgr_gem, 4096 * 3);

	/* Initialize the linked lists for BO reuse cache. */
	for (size = 4 * 4096; size <= cache_max_size; size *= 2) {
		add_bucket(bufmgr_gem, size);

		add_bucket(bufmgr_gem, size + size * 1 / 4);
		add_bucket(bufmgr_gem, size + size * 2 / 4);
		add_bucket(bufmgr_gem, size + size * 3 / 4);
	}
}

void
drm_bacon_bufmgr_gem_set_vma_cache_size(drm_bacon_bufmgr *bufmgr, int limit)
{
	drm_bacon_bufmgr_gem *bufmgr_gem = (drm_bacon_bufmgr_gem *)bufmgr;

	bufmgr_gem->vma_max = limit;

	drm_bacon_gem_bo_purge_vma_cache(bufmgr_gem);
}

static int
parse_devid_override(const char *devid_override)
{
	static const struct {
		const char *name;
		int pci_id;
	} name_map[] = {
		{ "brw", PCI_CHIP_I965_GM },
		{ "g4x", PCI_CHIP_GM45_GM },
		{ "ilk", PCI_CHIP_ILD_G },
		{ "snb", PCI_CHIP_SANDYBRIDGE_M_GT2_PLUS },
		{ "ivb", PCI_CHIP_IVYBRIDGE_S_GT2 },
		{ "hsw", PCI_CHIP_HASWELL_CRW_E_GT3 },
		{ "byt", PCI_CHIP_VALLEYVIEW_3 },
		{ "bdw", 0x1620 | BDW_ULX },
		{ "skl", PCI_CHIP_SKYLAKE_DT_GT2 },
		{ "kbl", PCI_CHIP_KABYLAKE_DT_GT2 },
	};
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(name_map); i++) {
		if (!strcmp(name_map[i].name, devid_override))
			return name_map[i].pci_id;
	}

	return strtod(devid_override, NULL);
}

/**
 * Get the PCI ID for the device.  This can be overridden by setting the
 * INTEL_DEVID_OVERRIDE environment variable to the desired ID.
 */
static int
get_pci_device_id(drm_bacon_bufmgr_gem *bufmgr_gem)
{
	char *devid_override;
	int devid = 0;
	int ret;
	drm_i915_getparam_t gp;

	if (geteuid() == getuid()) {
		devid_override = getenv("INTEL_DEVID_OVERRIDE");
		if (devid_override) {
			bufmgr_gem->no_exec = true;
			return parse_devid_override(devid_override);
		}
	}

	memclear(gp);
	gp.param = I915_PARAM_CHIPSET_ID;
	gp.value = &devid;
	ret = drmIoctl(bufmgr_gem->fd, DRM_IOCTL_I915_GETPARAM, &gp);
	if (ret) {
		fprintf(stderr, "get chip id failed: %d [%d]\n", ret, errno);
		fprintf(stderr, "param: %d, val: %d\n", gp.param, *gp.value);
	}
	return devid;
}

int
drm_bacon_bufmgr_gem_get_devid(drm_bacon_bufmgr *bufmgr)
{
	drm_bacon_bufmgr_gem *bufmgr_gem = (drm_bacon_bufmgr_gem *)bufmgr;

	return bufmgr_gem->pci_device;
}

drm_bacon_context *
drm_bacon_gem_context_create(drm_bacon_bufmgr *bufmgr)
{
	drm_bacon_bufmgr_gem *bufmgr_gem = (drm_bacon_bufmgr_gem *)bufmgr;
	struct drm_i915_gem_context_create create;
	drm_bacon_context *context = NULL;
	int ret;

	context = calloc(1, sizeof(*context));
	if (!context)
		return NULL;

	memclear(create);
	ret = drmIoctl(bufmgr_gem->fd, DRM_IOCTL_I915_GEM_CONTEXT_CREATE, &create);
	if (ret != 0) {
		DBG("DRM_IOCTL_I915_GEM_CONTEXT_CREATE failed: %s\n",
		    strerror(errno));
		free(context);
		return NULL;
	}

	context->ctx_id = create.ctx_id;
	context->bufmgr = bufmgr;

	return context;
}

int
drm_bacon_gem_context_get_id(drm_bacon_context *ctx, uint32_t *ctx_id)
{
	if (ctx == NULL)
		return -EINVAL;

	*ctx_id = ctx->ctx_id;

	return 0;
}

void
drm_bacon_gem_context_destroy(drm_bacon_context *ctx)
{
	drm_bacon_bufmgr_gem *bufmgr_gem;
	struct drm_i915_gem_context_destroy destroy;
	int ret;

	if (ctx == NULL)
		return;

	memclear(destroy);

	bufmgr_gem = (drm_bacon_bufmgr_gem *)ctx->bufmgr;
	destroy.ctx_id = ctx->ctx_id;
	ret = drmIoctl(bufmgr_gem->fd, DRM_IOCTL_I915_GEM_CONTEXT_DESTROY,
		       &destroy);
	if (ret != 0)
		fprintf(stderr, "DRM_IOCTL_I915_GEM_CONTEXT_DESTROY failed: %s\n",
			strerror(errno));

	free(ctx);
}

int
drm_bacon_get_reset_stats(drm_bacon_context *ctx,
			  uint32_t *reset_count,
			  uint32_t *active,
			  uint32_t *pending)
{
	drm_bacon_bufmgr_gem *bufmgr_gem;
	struct drm_i915_reset_stats stats;
	int ret;

	if (ctx == NULL)
		return -EINVAL;

	memclear(stats);

	bufmgr_gem = (drm_bacon_bufmgr_gem *)ctx->bufmgr;
	stats.ctx_id = ctx->ctx_id;
	ret = drmIoctl(bufmgr_gem->fd,
		       DRM_IOCTL_I915_GET_RESET_STATS,
		       &stats);
	if (ret == 0) {
		if (reset_count != NULL)
			*reset_count = stats.reset_count;

		if (active != NULL)
			*active = stats.batch_active;

		if (pending != NULL)
			*pending = stats.batch_pending;
	}

	return ret;
}

int
drm_bacon_reg_read(drm_bacon_bufmgr *bufmgr,
		   uint32_t offset,
		   uint64_t *result)
{
	drm_bacon_bufmgr_gem *bufmgr_gem = (drm_bacon_bufmgr_gem *)bufmgr;
	struct drm_i915_reg_read reg_read;
	int ret;

	memclear(reg_read);
	reg_read.offset = offset;

	ret = drmIoctl(bufmgr_gem->fd, DRM_IOCTL_I915_REG_READ, &reg_read);

	*result = reg_read.val;
	return ret;
}

static pthread_mutex_t bufmgr_list_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct list_head bufmgr_list = { &bufmgr_list, &bufmgr_list };

static drm_bacon_bufmgr_gem *
drm_bacon_bufmgr_gem_find(int fd)
{
	list_for_each_entry(drm_bacon_bufmgr_gem,
                            bufmgr_gem, &bufmgr_list, managers) {
		if (bufmgr_gem->fd == fd) {
			p_atomic_inc(&bufmgr_gem->refcount);
			return bufmgr_gem;
		}
	}

	return NULL;
}

static void
drm_bacon_bufmgr_gem_unref(drm_bacon_bufmgr *bufmgr)
{
	drm_bacon_bufmgr_gem *bufmgr_gem = (drm_bacon_bufmgr_gem *)bufmgr;

	if (atomic_add_unless(&bufmgr_gem->refcount, -1, 1)) {
		pthread_mutex_lock(&bufmgr_list_mutex);

		if (p_atomic_dec_zero(&bufmgr_gem->refcount)) {
			list_del(&bufmgr_gem->managers);
			drm_bacon_bufmgr_gem_destroy(bufmgr);
		}

		pthread_mutex_unlock(&bufmgr_list_mutex);
	}
}

void *drm_bacon_gem_bo_map__gtt(drm_bacon_bo *bo)
{
	drm_bacon_bufmgr_gem *bufmgr_gem = (drm_bacon_bufmgr_gem *) bo->bufmgr;
	drm_bacon_bo_gem *bo_gem = (drm_bacon_bo_gem *) bo;

	if (bo_gem->gtt_virtual)
		return bo_gem->gtt_virtual;

	if (bo_gem->is_userptr)
		return NULL;

	pthread_mutex_lock(&bufmgr_gem->lock);
	if (bo_gem->gtt_virtual == NULL) {
		struct drm_i915_gem_mmap_gtt mmap_arg;
		void *ptr;

		DBG("bo_map_gtt: mmap %d (%s), map_count=%d\n",
		    bo_gem->gem_handle, bo_gem->name, bo_gem->map_count);

		if (bo_gem->map_count++ == 0)
			drm_bacon_gem_bo_open_vma(bufmgr_gem, bo_gem);

		memclear(mmap_arg);
		mmap_arg.handle = bo_gem->gem_handle;

		/* Get the fake offset back... */
		ptr = MAP_FAILED;
		if (drmIoctl(bufmgr_gem->fd,
			     DRM_IOCTL_I915_GEM_MMAP_GTT,
			     &mmap_arg) == 0) {
			/* and mmap it */
			ptr = drm_mmap(0, bo->size, PROT_READ | PROT_WRITE,
				       MAP_SHARED, bufmgr_gem->fd,
				       mmap_arg.offset);
		}
		if (ptr == MAP_FAILED) {
			if (--bo_gem->map_count == 0)
				drm_bacon_gem_bo_close_vma(bufmgr_gem, bo_gem);
			ptr = NULL;
		}

		bo_gem->gtt_virtual = ptr;
	}
	pthread_mutex_unlock(&bufmgr_gem->lock);

	return bo_gem->gtt_virtual;
}

void *drm_bacon_gem_bo_map__cpu(drm_bacon_bo *bo)
{
	drm_bacon_bufmgr_gem *bufmgr_gem = (drm_bacon_bufmgr_gem *) bo->bufmgr;
	drm_bacon_bo_gem *bo_gem = (drm_bacon_bo_gem *) bo;

	if (bo_gem->mem_virtual)
		return bo_gem->mem_virtual;

	if (bo_gem->is_userptr) {
		/* Return the same user ptr */
		return bo_gem->user_virtual;
	}

	pthread_mutex_lock(&bufmgr_gem->lock);
	if (!bo_gem->mem_virtual) {
		struct drm_i915_gem_mmap mmap_arg;

		if (bo_gem->map_count++ == 0)
			drm_bacon_gem_bo_open_vma(bufmgr_gem, bo_gem);

		DBG("bo_map: %d (%s), map_count=%d\n",
		    bo_gem->gem_handle, bo_gem->name, bo_gem->map_count);

		memclear(mmap_arg);
		mmap_arg.handle = bo_gem->gem_handle;
		mmap_arg.size = bo->size;
		if (drmIoctl(bufmgr_gem->fd,
			     DRM_IOCTL_I915_GEM_MMAP,
			     &mmap_arg)) {
			DBG("%s:%d: Error mapping buffer %d (%s): %s .\n",
			    __FILE__, __LINE__, bo_gem->gem_handle,
			    bo_gem->name, strerror(errno));
			if (--bo_gem->map_count == 0)
				drm_bacon_gem_bo_close_vma(bufmgr_gem, bo_gem);
		} else {
			VG(VALGRIND_MALLOCLIKE_BLOCK(mmap_arg.addr_ptr, mmap_arg.size, 0, 1));
			bo_gem->mem_virtual = (void *)(uintptr_t) mmap_arg.addr_ptr;
		}
	}
	pthread_mutex_unlock(&bufmgr_gem->lock);

	return bo_gem->mem_virtual;
}

void *drm_bacon_gem_bo_map__wc(drm_bacon_bo *bo)
{
	drm_bacon_bufmgr_gem *bufmgr_gem = (drm_bacon_bufmgr_gem *) bo->bufmgr;
	drm_bacon_bo_gem *bo_gem = (drm_bacon_bo_gem *) bo;

	if (bo_gem->wc_virtual)
		return bo_gem->wc_virtual;

	if (bo_gem->is_userptr)
		return NULL;

	pthread_mutex_lock(&bufmgr_gem->lock);
	if (!bo_gem->wc_virtual) {
		struct drm_i915_gem_mmap mmap_arg;

		if (bo_gem->map_count++ == 0)
			drm_bacon_gem_bo_open_vma(bufmgr_gem, bo_gem);

		DBG("bo_map: %d (%s), map_count=%d\n",
		    bo_gem->gem_handle, bo_gem->name, bo_gem->map_count);

		memclear(mmap_arg);
		mmap_arg.handle = bo_gem->gem_handle;
		mmap_arg.size = bo->size;
		mmap_arg.flags = I915_MMAP_WC;
		if (drmIoctl(bufmgr_gem->fd,
			     DRM_IOCTL_I915_GEM_MMAP,
			     &mmap_arg)) {
			DBG("%s:%d: Error mapping buffer %d (%s): %s .\n",
			    __FILE__, __LINE__, bo_gem->gem_handle,
			    bo_gem->name, strerror(errno));
			if (--bo_gem->map_count == 0)
				drm_bacon_gem_bo_close_vma(bufmgr_gem, bo_gem);
		} else {
			VG(VALGRIND_MALLOCLIKE_BLOCK(mmap_arg.addr_ptr, mmap_arg.size, 0, 1));
			bo_gem->wc_virtual = (void *)(uintptr_t) mmap_arg.addr_ptr;
		}
	}
	pthread_mutex_unlock(&bufmgr_gem->lock);

	return bo_gem->wc_virtual;
}

/**
 * Initializes the GEM buffer manager, which uses the kernel to allocate, map,
 * and manage map buffer objections.
 *
 * \param fd File descriptor of the opened DRM device.
 */
drm_bacon_bufmgr *
drm_bacon_bufmgr_gem_init(int fd, int batch_size)
{
	drm_bacon_bufmgr_gem *bufmgr_gem;
	struct drm_i915_gem_get_aperture aperture;
	drm_i915_getparam_t gp;
	int ret, tmp;

	pthread_mutex_lock(&bufmgr_list_mutex);

	bufmgr_gem = drm_bacon_bufmgr_gem_find(fd);
	if (bufmgr_gem)
		goto exit;

	bufmgr_gem = calloc(1, sizeof(*bufmgr_gem));
	if (bufmgr_gem == NULL)
		goto exit;

	bufmgr_gem->fd = fd;
	p_atomic_set(&bufmgr_gem->refcount, 1);

	if (pthread_mutex_init(&bufmgr_gem->lock, NULL) != 0) {
		free(bufmgr_gem);
		bufmgr_gem = NULL;
		goto exit;
	}

	memclear(aperture);
	ret = drmIoctl(bufmgr_gem->fd,
		       DRM_IOCTL_I915_GEM_GET_APERTURE,
		       &aperture);

	if (ret == 0)
		bufmgr_gem->gtt_size = aperture.aper_available_size;
	else {
		fprintf(stderr, "DRM_IOCTL_I915_GEM_APERTURE failed: %s\n",
			strerror(errno));
		bufmgr_gem->gtt_size = 128 * 1024 * 1024;
		fprintf(stderr, "Assuming %dkB available aperture size.\n"
			"May lead to reduced performance or incorrect "
			"rendering.\n",
			(int)bufmgr_gem->gtt_size / 1024);
	}

	bufmgr_gem->pci_device = get_pci_device_id(bufmgr_gem);

	if (IS_GEN4(bufmgr_gem->pci_device))
		bufmgr_gem->gen = 4;
	else if (IS_GEN5(bufmgr_gem->pci_device))
		bufmgr_gem->gen = 5;
	else if (IS_GEN6(bufmgr_gem->pci_device))
		bufmgr_gem->gen = 6;
	else if (IS_GEN7(bufmgr_gem->pci_device))
		bufmgr_gem->gen = 7;
	else if (IS_GEN8(bufmgr_gem->pci_device))
		bufmgr_gem->gen = 8;
	else if (IS_GEN9(bufmgr_gem->pci_device))
		bufmgr_gem->gen = 9;
	else {
		free(bufmgr_gem);
		bufmgr_gem = NULL;
		goto exit;
	}

	memclear(gp);
	gp.value = &tmp;

	gp.param = I915_PARAM_HAS_BSD;
	ret = drmIoctl(bufmgr_gem->fd, DRM_IOCTL_I915_GETPARAM, &gp);
	bufmgr_gem->has_bsd = ret == 0;

	gp.param = I915_PARAM_HAS_BLT;
	ret = drmIoctl(bufmgr_gem->fd, DRM_IOCTL_I915_GETPARAM, &gp);
	bufmgr_gem->has_blt = ret == 0;

	gp.param = I915_PARAM_HAS_EXEC_ASYNC;
	ret = drmIoctl(bufmgr_gem->fd, DRM_IOCTL_I915_GETPARAM, &gp);
	bufmgr_gem->has_exec_async = ret == 0;

	bufmgr_gem->bufmgr.bo_alloc_userptr = check_bo_alloc_userptr;

	gp.param = I915_PARAM_HAS_WAIT_TIMEOUT;
	ret = drmIoctl(bufmgr_gem->fd, DRM_IOCTL_I915_GETPARAM, &gp);
	bufmgr_gem->has_wait_timeout = ret == 0;

	gp.param = I915_PARAM_HAS_LLC;
	ret = drmIoctl(bufmgr_gem->fd, DRM_IOCTL_I915_GETPARAM, &gp);
	if (ret != 0) {
		/* Kernel does not supports HAS_LLC query, fallback to GPU
		 * generation detection and assume that we have LLC on GEN6/7
		 */
		bufmgr_gem->has_llc = (IS_GEN6(bufmgr_gem->pci_device) |
				IS_GEN7(bufmgr_gem->pci_device));
	} else
		bufmgr_gem->has_llc = *gp.value;

	gp.param = I915_PARAM_HAS_VEBOX;
	ret = drmIoctl(bufmgr_gem->fd, DRM_IOCTL_I915_GETPARAM, &gp);
	bufmgr_gem->has_vebox = (ret == 0) & (*gp.value > 0);

	gp.param = I915_PARAM_HAS_EXEC_SOFTPIN;
	ret = drmIoctl(bufmgr_gem->fd, DRM_IOCTL_I915_GETPARAM, &gp);
	if (ret == 0 && *gp.value > 0)
		bufmgr_gem->bufmgr.bo_set_softpin_offset = drm_bacon_gem_bo_set_softpin_offset;

	/* Let's go with one relocation per every 2 dwords (but round down a bit
	 * since a power of two will mean an extra page allocation for the reloc
	 * buffer).
	 *
	 * Every 4 was too few for the blender benchmark.
	 */
	bufmgr_gem->max_relocs = batch_size / sizeof(uint32_t) / 2 - 2;

	bufmgr_gem->bufmgr.bo_alloc = drm_bacon_gem_bo_alloc;
	bufmgr_gem->bufmgr.bo_alloc_for_render =
	    drm_bacon_gem_bo_alloc_for_render;
	bufmgr_gem->bufmgr.bo_alloc_tiled = drm_bacon_gem_bo_alloc_tiled;
	bufmgr_gem->bufmgr.bo_reference = drm_bacon_gem_bo_reference;
	bufmgr_gem->bufmgr.bo_unreference = drm_bacon_gem_bo_unreference;
	bufmgr_gem->bufmgr.bo_map = drm_bacon_gem_bo_map;
	bufmgr_gem->bufmgr.bo_unmap = drm_bacon_gem_bo_unmap;
	bufmgr_gem->bufmgr.bo_subdata = drm_bacon_gem_bo_subdata;
	bufmgr_gem->bufmgr.bo_get_subdata = drm_bacon_gem_bo_get_subdata;
	bufmgr_gem->bufmgr.bo_wait_rendering = drm_bacon_gem_bo_wait_rendering;
	bufmgr_gem->bufmgr.bo_emit_reloc = drm_bacon_gem_bo_emit_reloc;
	bufmgr_gem->bufmgr.bo_get_tiling = drm_bacon_gem_bo_get_tiling;
	bufmgr_gem->bufmgr.bo_set_tiling = drm_bacon_gem_bo_set_tiling;
	bufmgr_gem->bufmgr.bo_flink = drm_bacon_gem_bo_flink;
	bufmgr_gem->bufmgr.bo_exec = drm_bacon_gem_bo_exec2;
	bufmgr_gem->bufmgr.bo_mrb_exec = drm_bacon_gem_bo_mrb_exec2;
	bufmgr_gem->bufmgr.bo_busy = drm_bacon_gem_bo_busy;
	bufmgr_gem->bufmgr.bo_madvise = drm_bacon_gem_bo_madvise;
	bufmgr_gem->bufmgr.destroy = drm_bacon_bufmgr_gem_unref;
	bufmgr_gem->bufmgr.debug = 0;
	bufmgr_gem->bufmgr.check_aperture_space =
	    drm_bacon_gem_check_aperture_space;
	bufmgr_gem->bufmgr.bo_disable_reuse = drm_bacon_gem_bo_disable_reuse;
	bufmgr_gem->bufmgr.bo_is_reusable = drm_bacon_gem_bo_is_reusable;
	bufmgr_gem->bufmgr.bo_references = drm_bacon_gem_bo_references;

	init_cache_buckets(bufmgr_gem);

	list_inithead(&bufmgr_gem->vma_cache);
	bufmgr_gem->vma_max = -1; /* unlimited by default */

	list_add(&bufmgr_gem->managers, &bufmgr_list);

exit:
	pthread_mutex_unlock(&bufmgr_list_mutex);

	return bufmgr_gem != NULL ? &bufmgr_gem->bufmgr : NULL;
}