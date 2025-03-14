/*
 * Copyright (C) 2013 Red Hat
 * Author: Rob Clark <robdclark@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by
 * the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef __ETNAVIV_GEM_H__
#define __ETNAVIV_GEM_H__

#include <linux/reservation.h>
#include "etnaviv_drv.h"

struct etnaviv_gem_ops;

struct etnaviv_gem_userptr {
	uintptr_t ptr;
	struct task_struct *task;
	struct work_struct *work;
	bool ro;
};

enum etna_bo_backend {
	ETNA_BO_BACKEND_SHMEM = 0,
	ETNA_BO_BACKEND_DMA,
};

struct etnaviv_gem_object {
	struct drm_gem_object base;
	const struct etnaviv_gem_ops *ops;

	enum etna_bo_backend backend;
	uint32_t flags;

	/* And object is either:
	 *  inactive - on priv->inactive_list
	 *  active   - on one one of the gpu's active_list..  well, at
	 *     least for now we don't have (I don't think) hw sync between
	 *     2d and 3d one devices which have both, meaning we need to
	 *     block on submit if a bo is already on other ring
	 *
	 */
	struct list_head mm_list;
	struct etnaviv_gpu *gpu;     /* non-null if active */
	uint32_t read_fence, write_fence;

	/* Transiently in the process of submit ioctl, objects associated
	 * with the submit are on submit->bo_list.. this only lasts for
	 * the duration of the ioctl, so one bo can never be on multiple
	 * submit lists.
	 */
	struct list_head submit_entry;

	struct page **pages;
	struct sg_table *sgt;
	void *vaddr;
	uint32_t iova;

	/* for ETNA_BO_CMDSTREAM */
	dma_addr_t paddr;

	/* normally (resv == &_resv) except for imported bo's */
	struct reservation_object *resv;
	struct reservation_object _resv;

	struct drm_mm_node *gpu_vram_node;

	/* for buffer manipulation during submit */
	bool is_ring_buffer;
	u32 offset;
	u32 *last_wait; /* virtual address of last WAIT */

	struct etnaviv_gem_userptr userptr;
};
#define to_etnaviv_bo(x) container_of(x, struct etnaviv_gem_object, base)

struct etnaviv_gem_ops {
	int (*get_pages)(struct etnaviv_gem_object *);
	int (*get_sgt)(struct etnaviv_gem_object *);
	int (*mmap)(struct drm_gem_object *obj, struct vm_area_struct *vma);
	void (*release)(struct etnaviv_gem_object *);
};

static inline bool is_active(struct etnaviv_gem_object *etnaviv_obj)
{
	return etnaviv_obj->gpu != NULL;
}

/* Created per submit-ioctl, to track bo's and cmdstream bufs, etc,
 * associated with the cmdstream submission for synchronization (and
 * make it easier to unwind when things go wrong, etc).  This only
 * lasts for the duration of the submit-ioctl.
 */
struct etnaviv_gem_submit {
	struct drm_device *dev;
	struct etnaviv_gpu *gpu;
	struct list_head bo_list;
	struct ww_acquire_ctx *ticket; //pointer maybe?
	uint32_t fence;
	unsigned int nr_bos;
	struct {
		uint32_t size;  /* in dwords */
		struct etnaviv_gem_object *obj;
	} cmd;
	struct {
		uint32_t flags;
		struct etnaviv_gem_object *obj;
		uint32_t iova;
	} bos[0];
};

void etnaviv_gem_scatter_map(struct etnaviv_gem_object *etnaviv_obj);
void etnaviv_gem_scatterlist_unmap(struct etnaviv_gem_object *etnaviv_obj);
int etnaviv_gem_new_private(struct drm_device *dev, size_t size, uint32_t flags,
	struct etnaviv_gem_object **res);

#endif /* __ETNAVIV_GEM_H__ */
