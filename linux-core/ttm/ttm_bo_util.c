/**************************************************************************
 *
 * Copyright (c) 2007-2008 Tungsten Graphics, Inc., Cedar Park, TX., USA
 * All Rights Reserved.
 * Copyright (c) 2009 VMware, Inc., Palo Alto, CA., USA
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
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE COPYRIGHT HOLDERS, AUTHORS AND/OR ITS SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 **************************************************************************/
/*
 * Authors: Thomas Hellstr�m <thomas-at-tungstengraphics-dot-com>
 */

#include "ttm/ttm_bo_driver.h"
#include "ttm/ttm_placement_common.h"
#include "ttm/ttm_pat_compat.h"
#include <linux/io.h>
#include <linux/highmem.h>
#include <linux/wait.h>
#include <linux/version.h>

void ttm_bo_free_old_node(struct ttm_buffer_object *bo)
{
	struct ttm_mem_reg *old_mem = &bo->mem;

	if (old_mem->mm_node) {
		spin_lock(&bo->bdev->lru_lock);
		drm_mm_put_block(old_mem->mm_node);
		spin_unlock(&bo->bdev->lru_lock);
	}
	old_mem->mm_node = NULL;
}

int ttm_bo_move_ttm(struct ttm_buffer_object *bo,
		    bool evict, bool no_wait, struct ttm_mem_reg *new_mem)
{
	struct ttm_tt *ttm = bo->ttm;
	struct ttm_mem_reg *old_mem = &bo->mem;
	uint32_t save_flags = old_mem->flags;
	uint32_t save_proposed_flags = old_mem->proposed_flags;
	int ret;

	if (old_mem->mem_type != TTM_PL_SYSTEM) {
		ttm_tt_unbind(ttm);
		ttm_bo_free_old_node(bo);
		ttm_flag_masked(&old_mem->flags, TTM_PL_FLAG_SYSTEM,
				TTM_PL_MASK_MEM);
		old_mem->mem_type = TTM_PL_SYSTEM;
		save_flags = old_mem->flags;
	}

	ret = ttm_tt_set_placement_caching(ttm, new_mem->flags);
	if (unlikely(ret != 0))
		return ret;

	if (new_mem->mem_type != TTM_PL_SYSTEM) {
		ret = ttm_tt_bind(ttm, new_mem);
		if (unlikely(ret != 0))
			return ret;
	}

	*old_mem = *new_mem;
	new_mem->mm_node = NULL;
	old_mem->proposed_flags = save_proposed_flags;
	ttm_flag_masked(&save_flags, new_mem->flags, TTM_PL_MASK_MEMTYPE);
	return 0;
}

int ttm_mem_reg_ioremap(struct ttm_bo_device *bdev, struct ttm_mem_reg *mem,
			void **virtual)
{
	struct ttm_mem_type_manager *man = &bdev->man[mem->mem_type];
	unsigned long bus_offset;
	unsigned long bus_size;
	unsigned long bus_base;
	int ret;
	void *addr;

	*virtual = NULL;
	ret = ttm_bo_pci_offset(bdev, mem, &bus_base, &bus_offset, &bus_size);
	if (ret || bus_size == 0)
		return ret;

	if (!(man->flags & TTM_MEMTYPE_FLAG_NEEDS_IOREMAP))
		addr = (void *)(((u8 *) man->io_addr) + bus_offset);
	else {
#if  (LINUX_VERSION_CODE > KERNEL_VERSION(2,6,26))
		if (mem->flags & TTM_PL_FLAG_WC)
			addr = ioremap_wc(bus_base + bus_offset, bus_size);
		else
			addr = ioremap_nocache(bus_base + bus_offset, bus_size);
#else
		addr = ioremap_nocache(bus_base + bus_offset, bus_size);
#endif
		if (!addr)
			return -ENOMEM;
	}
	*virtual = addr;
	return 0;
}

void ttm_mem_reg_iounmap(struct ttm_bo_device *bdev, struct ttm_mem_reg *mem,
			 void *virtual)
{
	struct ttm_mem_type_manager *man;

	man = &bdev->man[mem->mem_type];

	if (virtual && (man->flags & TTM_MEMTYPE_FLAG_NEEDS_IOREMAP))
		iounmap(virtual);
}

static int ttm_copy_io_page(void *dst, void *src, unsigned long page)
{
	uint32_t *dstP =
	    (uint32_t *) ((unsigned long)dst + (page << PAGE_SHIFT));
	uint32_t *srcP =
	    (uint32_t *) ((unsigned long)src + (page << PAGE_SHIFT));

	int i;
	for (i = 0; i < PAGE_SIZE / sizeof(uint32_t); ++i)
		iowrite32(ioread32(srcP++), dstP++);
	return 0;
}

static int ttm_copy_io_ttm_page(struct ttm_tt *ttm, void *src,
				unsigned long page)
{
	struct page *d = ttm_tt_get_page(ttm, page);
	void *dst;

	if (!d)
		return -ENOMEM;

	src = (void *)((unsigned long)src + (page << PAGE_SHIFT));
	dst = kmap(d);
	if (!dst)
		return -ENOMEM;

	memcpy_fromio(dst, src, PAGE_SIZE);
	kunmap(d);
	return 0;
}

static int ttm_copy_ttm_io_page(struct ttm_tt *ttm, void *dst,
				unsigned long page)
{
	struct page *s = ttm_tt_get_page(ttm, page);
	void *src;

	if (!s)
		return -ENOMEM;

	dst = (void *)((unsigned long)dst + (page << PAGE_SHIFT));
	src = kmap(s);
	if (!src)
		return -ENOMEM;

	memcpy_toio(dst, src, PAGE_SIZE);
	kunmap(s);
	return 0;
}

int ttm_bo_move_memcpy(struct ttm_buffer_object *bo,
		       bool evict, bool no_wait, struct ttm_mem_reg *new_mem)
{
	struct ttm_bo_device *bdev = bo->bdev;
	struct ttm_mem_type_manager *man = &bdev->man[new_mem->mem_type];
	struct ttm_tt *ttm = bo->ttm;
	struct ttm_mem_reg *old_mem = &bo->mem;
	struct ttm_mem_reg old_copy = *old_mem;
	void *old_iomap;
	void *new_iomap;
	int ret;
	uint32_t save_flags = old_mem->flags;
	uint32_t save_proposed_flags = old_mem->proposed_flags;
	unsigned long i;
	unsigned long page;
	unsigned long add = 0;
	int dir;

	ret = ttm_mem_reg_ioremap(bdev, old_mem, &old_iomap);
	if (ret)
		return ret;
	ret = ttm_mem_reg_ioremap(bdev, new_mem, &new_iomap);
	if (ret)
		goto out;

	if (old_iomap == NULL && new_iomap == NULL)
		goto out2;
	if (old_iomap == NULL && ttm == NULL)
		goto out2;

	add = 0;
	dir = 1;

	if ((old_mem->mem_type == new_mem->mem_type) &&
	    (new_mem->mm_node->start <
	     old_mem->mm_node->start + old_mem->mm_node->size)) {
		dir = -1;
		add = new_mem->num_pages - 1;
	}

	for (i = 0; i < new_mem->num_pages; ++i) {
		page = i * dir + add;
		if (old_iomap == NULL)
			ret = ttm_copy_ttm_io_page(ttm, new_iomap, page);
		else if (new_iomap == NULL)
			ret = ttm_copy_io_ttm_page(ttm, old_iomap, page);
		else
			ret = ttm_copy_io_page(new_iomap, old_iomap, page);
		if (ret)
			goto out1;
	}
	mb();
      out2:
	ttm_bo_free_old_node(bo);

	*old_mem = *new_mem;
	new_mem->mm_node = NULL;
	old_mem->proposed_flags = save_proposed_flags;
	ttm_flag_masked(&save_flags, new_mem->flags, TTM_PL_MASK_MEMTYPE);

	if ((man->flags & TTM_MEMTYPE_FLAG_FIXED) && (ttm != NULL)) {
		ttm_tt_unbind(ttm);
		ttm_tt_destroy(ttm);
		bo->ttm = NULL;
	}

      out1:
	ttm_mem_reg_iounmap(bdev, new_mem, new_iomap);
      out:
	ttm_mem_reg_iounmap(bdev, &old_copy, old_iomap);
	return ret;
}

static void ttm_transfered_destroy(struct ttm_buffer_object *bo)
{
	kfree(bo);
}

/**
 * ttm_buffer_object_transfer
 *
 * @bo: A pointer to a struct ttm_buffer_object.
 * @new_obj: A pointer to a pointer to a newly created ttm_buffer_object,
 * holding the data of @bo with the old placement.
 *
 * This is a utility function that may be called after an accelerated move
 * has been scheduled. A new buffer object is created as a placeholder for
 * the old data while it's being copied. When that buffer object is idle,
 * it can be destroyed, releasing the space of the old placement.
 * Returns:
 * !0: Failure.
 */

static int ttm_buffer_object_transfer(struct ttm_buffer_object *bo,
				      struct ttm_buffer_object **new_obj)
{
	struct ttm_buffer_object *fbo;
	struct ttm_bo_device *bdev = bo->bdev;
	struct ttm_bo_driver *driver = bdev->driver;

	fbo = kzalloc(sizeof(*fbo), GFP_KERNEL);
	if (!fbo)
		return -ENOMEM;

	*fbo = *bo;
	mutex_init(&fbo->mutex);
	mutex_lock(&fbo->mutex);

	init_waitqueue_head(&fbo->event_queue);
	INIT_LIST_HEAD(&fbo->ddestroy);
	INIT_LIST_HEAD(&fbo->lru);
	INIT_LIST_HEAD(&fbo->swap);

	fbo->sync_obj = driver->sync_obj_ref(bo->sync_obj);
	if (fbo->mem.mm_node)
		fbo->mem.mm_node->private = (void *)fbo;
	kref_init(&fbo->list_kref);
	kref_init(&fbo->kref);
	fbo->destroy = &ttm_transfered_destroy;

	mutex_unlock(&fbo->mutex);

	*new_obj = fbo;
	return 0;
}

pgprot_t ttm_io_prot(uint32_t caching_flags, pgprot_t tmp)
{
#if defined(__i386__) || defined(__x86_64__)
	if (caching_flags & TTM_PL_FLAG_WC) {
		tmp = pgprot_ttm_x86_wc(tmp);
	} else if (boot_cpu_data.x86 > 3) {
		tmp = pgprot_noncached(tmp);
	}
#elif defined(__powerpc__)
	if (!(caching_flags & TTM_PL_FLAG_CACHED)) {
		pgprot_val(tmp) |= _PAGE_NO_CACHE;
		if (caching_flags & TTM_PL_FLAG_UNCACHED)
			pgprot_val(tmp) |= _PAGE_GUARDED;
	}
#endif
#if defined(__ia64__)
	if (caching_flags & TTM_PL_FLAG_WC)
		tmp = pgprot_writecombine(tmp);
	else
		tmp = pgprot_noncached(tmp);
#endif
#if defined(__sparc__)
	if (!(caching_flags & TTM_PL_FLAG_CACHED))
		tmp = pgprot_noncached(tmp);
#endif
	return tmp;
}

static int ttm_bo_ioremap(struct ttm_buffer_object *bo,
			  unsigned long bus_base,
			  unsigned long bus_offset,
			  unsigned long bus_size,
			  struct ttm_bo_kmap_obj *map)
{
	struct ttm_bo_device * bdev = bo->bdev;
	struct ttm_mem_reg * mem = &bo->mem;
	struct ttm_mem_type_manager * man = &bdev->man[mem->mem_type];

	if (!(man->flags & TTM_MEMTYPE_FLAG_NEEDS_IOREMAP)) {
		map->bo_kmap_type = ttm_bo_map_premapped;
		map->virtual = (void *)(((u8 *) man->io_addr) + bus_offset);} else {
			map->bo_kmap_type = ttm_bo_map_iomap;
#if  (LINUX_VERSION_CODE > KERNEL_VERSION(2,6,26))
			if (mem->flags & TTM_PL_FLAG_WC)
				map->virtual = ioremap_wc(bus_base + bus_offset, bus_size);
			else
				map->virtual = ioremap_nocache(bus_base + bus_offset, bus_size);
#else
			map->virtual = ioremap_nocache(bus_base + bus_offset, bus_size);
#endif
		}
		return (!map->virtual) ? -ENOMEM : 0;
}

static int ttm_bo_kmap_ttm(struct ttm_buffer_object *bo,
			   unsigned long start_page,
			   unsigned long num_pages,
			   struct ttm_bo_kmap_obj *map)
{
	struct ttm_mem_reg * mem = &bo->mem; pgprot_t prot;
	struct ttm_tt * ttm = bo->ttm;
	struct page * d;
	int i;
	BUG_ON(!ttm);
	if (num_pages == 1 && (mem->flags & TTM_PL_FLAG_CACHED)) {
	    /*
	     * We're mapping a single page, and the desired
	     * page protection is consistent with the bo.
	     */
		map->bo_kmap_type = ttm_bo_map_kmap;
		map->page = ttm_tt_get_page(ttm, start_page);
		map->virtual = kmap(map->page);
	} else {
	    /*
	     * Populate the part we're mapping;
	     */
		for (i = start_page; i < start_page + num_pages; ++i) {
			d = ttm_tt_get_page(ttm, i); if (!d)
				return -ENOMEM;
		}

		/*
		 * We need to use vmap to get the desired page protection
		 * or to make the buffer object look contigous.
		 */
		prot = (mem->flags & TTM_PL_FLAG_CACHED) ?
			PAGE_KERNEL :
			ttm_io_prot(mem->flags, PAGE_KERNEL);
		map->bo_kmap_type = ttm_bo_map_vmap;
		map->virtual = vmap(ttm->pages + start_page, num_pages, 0, prot);
	}
	return (!map->virtual) ? -ENOMEM : 0;
}

int ttm_bo_kmap(struct ttm_buffer_object *bo,
		unsigned long start_page, unsigned long num_pages,
		struct ttm_bo_kmap_obj *map)
{
	    int ret;
	    unsigned long bus_base;
	    unsigned long bus_offset;
	    unsigned long bus_size;
	    BUG_ON(!list_empty(&bo->swap));
	    map->virtual = NULL;
	    if (num_pages > bo->num_pages)
		    return -EINVAL;
	    if (start_page > bo->num_pages)
		    return -EINVAL;
#if 0
	    if (num_pages > 1 && !DRM_SUSER(DRM_CURPROC))
	    return -EPERM;
#endif
	    ret = ttm_bo_pci_offset(bo->bdev, &bo->mem, &bus_base,
				    &bus_offset, &bus_size);
	    if (ret)
		    return ret;
	    if (bus_size == 0) {
		    return ttm_bo_kmap_ttm(bo, start_page, num_pages, map);
	    } else {
		    bus_offset += start_page << PAGE_SHIFT;
		    bus_size = num_pages << PAGE_SHIFT;
		    return ttm_bo_ioremap(bo, bus_base, bus_offset, bus_size, map);
	    }
}

void ttm_bo_kunmap(struct ttm_bo_kmap_obj *map)
{
	if (!map->virtual)
		return;
	switch (map->bo_kmap_type) {
	case ttm_bo_map_iomap:
		iounmap(map->virtual);
		break;
	case ttm_bo_map_vmap:
		vunmap(map->virtual);
		break;
	case ttm_bo_map_kmap:
		kunmap(map->page);
		break;
	case ttm_bo_map_premapped:
		break;
	default:
		BUG();
	}
	map->virtual = NULL;
	map->page = NULL;
}

int ttm_bo_pfn_prot(struct ttm_buffer_object *bo,
		    unsigned long dst_offset,
		    unsigned long *pfn, pgprot_t * prot)
{
	struct ttm_mem_reg * mem = &bo->mem;
	struct ttm_bo_device * bdev = bo->bdev;
	unsigned long bus_offset;
	unsigned long bus_size;
	unsigned long bus_base;
	int ret;
	ret = ttm_bo_pci_offset(bdev, mem, &bus_base, &bus_offset,
			&bus_size);
	if (ret)
		return -EINVAL;
	if (bus_size != 0)
		* pfn = (bus_base + bus_offset + dst_offset) >> PAGE_SHIFT;
	else
		if (!bo->ttm)
			return -EINVAL;
		else
			*pfn =
				page_to_pfn(ttm_tt_get_page(bo->ttm, dst_offset >> PAGE_SHIFT));
	*prot =
		(mem->flags & TTM_PL_FLAG_CACHED) ? PAGE_KERNEL : ttm_io_prot(mem->
				flags,
				PAGE_KERNEL);
	return 0;
}

int ttm_bo_move_accel_cleanup(struct ttm_buffer_object *bo,
			      void *sync_obj,
			      void *sync_obj_arg,
			      bool evict, bool no_wait,
			      struct ttm_mem_reg *new_mem)
{
	struct ttm_bo_device * bdev = bo->bdev;
	struct ttm_bo_driver * driver = bdev->driver;
	struct ttm_mem_type_manager * man = &bdev->man[new_mem->mem_type];
	struct ttm_mem_reg * old_mem = &bo->mem;
	int ret;
	uint32_t save_flags = old_mem->flags;
	uint32_t save_proposed_flags = old_mem->proposed_flags;
	struct ttm_buffer_object *ghost_obj;
	if (bo->sync_obj)
		driver->sync_obj_unref(&bo->sync_obj);
	bo->sync_obj = driver->sync_obj_ref(sync_obj);
	bo->sync_obj_arg = sync_obj_arg;
	if (evict) {
		ret = ttm_bo_wait(bo, false, false, false);
		if (ret)
			return ret;
		ttm_bo_free_old_node(bo);
		if ((man->flags & TTM_MEMTYPE_FLAG_FIXED) && (bo->ttm != NULL)) {
			ttm_tt_unbind(bo->ttm); ttm_tt_destroy(bo->ttm); bo->ttm = NULL;
		}
	} else {
		/**
		 * This should help pipeline ordinary buffer moves.
		 *
		 * Hang old buffer memory on a new buffer object,
		 * and leave it to be released when the GPU
		 * operation has completed.
		 */

		ret = ttm_buffer_object_transfer(bo, &ghost_obj);
		if (ret)
			return ret;

		/**
		 * If we're not moving to fixed memory, the TTM object
		 * needs to stay alive. Otherwhise hang it on the ghost
		 * bo to be unbound and destroyed.
		 */

		if (!(man->flags & TTM_MEMTYPE_FLAG_FIXED))
			ghost_obj->ttm = NULL;
		else
			ghost_obj = NULL;

		bo->priv_flags |= TTM_BO_PRIV_FLAG_MOVING;
		ttm_bo_unreserve(ghost_obj);
		ttm_bo_unref(&ghost_obj);
	}

	*old_mem = *new_mem;
	new_mem->mm_node = NULL;
	old_mem->proposed_flags = save_proposed_flags;
	ttm_flag_masked(&save_flags, new_mem->flags, TTM_PL_MASK_MEMTYPE);
	return 0;
}