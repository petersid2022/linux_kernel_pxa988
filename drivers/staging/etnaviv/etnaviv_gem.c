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

#include <linux/spinlock.h>
#include <linux/shmem_fs.h>

#include "etnaviv_drv.h"
#include "etnaviv_gem.h"
#include "etnaviv_gpu.h"
#include "etnaviv_mmu.h"

#define DRM_MM_SEARCH_DEFAULT 0 //added

void etnaviv_gem_scatter_map(struct etnaviv_gem_object *etnaviv_obj)
{
	struct drm_device *dev = etnaviv_obj->base.dev;
	struct sg_table *sgt = etnaviv_obj->sgt;

	/* For non-cached buffers, ensure the new pages are clean
	 * because display controller, GPU, etc. are not coherent:
	 */
	if (etnaviv_obj->flags & (ETNA_BO_WC|ETNA_BO_UNCACHED)) {
		dma_map_sg(dev->dev, sgt->sgl, sgt->nents, DMA_BIDIRECTIONAL);
		dma_unmap_sg(dev->dev, sgt->sgl, sgt->nents, DMA_BIDIRECTIONAL);
	} else {
		struct scatterlist *sg;
		unsigned int i;

		for_each_sg(etnaviv_obj->sgt->sgl, sg, etnaviv_obj->sgt->nents, i) {
			sg_dma_address(sg) = sg_phys(sg);
#ifdef CONFIG_NEED_SG_DMA_LENGTH
			sg_dma_len(sg) = sg->length;
#endif
		}
	}
}

void etnaviv_gem_scatterlist_unmap(struct etnaviv_gem_object *etnaviv_obj)
{
	struct drm_device *dev = etnaviv_obj->base.dev;
	struct sg_table *sgt = etnaviv_obj->sgt;

	/* For non-cached buffers, ensure the new pages are clean
	 * because display controller, GPU, etc. are not coherent:
	 */
	if (etnaviv_obj->flags & (ETNA_BO_WC|ETNA_BO_UNCACHED)) {
		dma_map_sg(dev->dev, sgt->sgl, sgt->nents, DMA_BIDIRECTIONAL);
		dma_unmap_sg(dev->dev, sgt->sgl, sgt->nents, DMA_BIDIRECTIONAL);
	}
}

static void put_pages(struct drm_gem_object *obj)
{
	struct etnaviv_gem_object *etnaviv_obj = to_etnaviv_bo(obj);

	if (etnaviv_obj->sgt) {
		/* For non-cached buffers, ensure the new pages are clean
		 * because display controller, GPU, etc. are not coherent:
		 */
		etnaviv_gem_scatterlist_unmap(etnaviv_obj);
		sg_free_table(etnaviv_obj->sgt);
		kfree(etnaviv_obj->sgt);

		etnaviv_obj->sgt = NULL;
	}

	if (etnaviv_obj->pages) {
		drm_gem_put_pages(obj, etnaviv_obj->pages, true, false);

		etnaviv_obj->pages = NULL;
	}
}

struct page **etnaviv_gem_get_pages(struct drm_gem_object *obj)
{
	struct etnaviv_gem_object *etnaviv_obj = to_etnaviv_bo(obj);
	int ret;

	if (!etnaviv_obj->pages) {
		ret = etnaviv_obj->ops->get_pages(etnaviv_obj);
		if (ret < 0)
			return ERR_PTR(ret);
	}

	if (!etnaviv_obj->sgt) {
		ret = etnaviv_obj->ops->get_sgt(etnaviv_obj);
		if (ret < 0)
			return ERR_PTR(ret);

		etnaviv_gem_scatter_map(etnaviv_obj);
	}

	return etnaviv_obj->pages;
}

void msm_gem_put_pages(struct drm_gem_object *obj)
{
	/* when we start tracking the pin count, then do something here */
}

int etnaviv_gem_mmap(struct file *filp, struct vm_area_struct *vma)
{
	struct etnaviv_gem_object *etnaviv_obj;
	int ret;

	ret = drm_gem_mmap(filp, vma);
	if (ret) {
		DBG("mmap failed: %d", ret);
		return ret;
	}

	etnaviv_obj = to_etnaviv_bo(vma->vm_private_data);
	ret = etnaviv_obj->ops->mmap(vma->vm_private_data, vma);

	return ret;
}

int etnaviv_gem_fault(struct vm_area_struct *vma, struct vm_fault *vmf)
{
	struct drm_gem_object *obj = vma->vm_private_data;
	struct drm_device *dev = obj->dev;
	struct page **pages;
	unsigned long pfn;
	pgoff_t pgoff;
	int ret;

	/* Make sure we don't parallel update on a fault, nor move or remove
	 * something from beneath our feet
	 */
	ret = mutex_lock_interruptible(&dev->struct_mutex);
	if (ret)
		goto out;

	/* make sure we have pages attached now */
	pages = etnaviv_gem_get_pages(obj);
	if (IS_ERR(pages)) {
		ret = PTR_ERR(pages);
		goto out_unlock;
	}

	/* We don't use vmf->pgoff since that has the fake offset: */
	pgoff = ((unsigned long)vmf->virtual_address -
			vma->vm_start) >> PAGE_SHIFT;

	pfn = page_to_pfn(pages[pgoff]);

	VERB("Inserting %p pfn %lx, pa %lx", vmf->virtual_address,
			pfn, pfn << PAGE_SHIFT);

	ret = vm_insert_mixed(vma, (unsigned long)vmf->virtual_address, pfn);

out_unlock:
	mutex_unlock(&dev->struct_mutex);
out:
	switch (ret) {
	case -EAGAIN:
	case 0:
	case -ERESTARTSYS:
	case -EINTR:
	case -EBUSY:
		/*
		 * EBUSY is ok: this just means that another thread
		 * already did the job.
		 */
		return VM_FAULT_NOPAGE;
	case -ENOMEM:
		return VM_FAULT_OOM;
	default:
		return VM_FAULT_SIGBUS;
	}
}

/** get mmap offset */
static uint64_t mmap_offset(struct drm_gem_object *obj)
{
	struct drm_device *dev = obj->dev;
	int ret;

	WARN_ON(!mutex_is_locked(&dev->struct_mutex));

	/* Make it mmapable */
	ret = drm_gem_create_mmap_offset(obj);

	if (ret) {
		dev_err(dev->dev, "could not allocate mmap offset\n");
		return 0;
	}

	return drm_vma_node_offset_addr(&obj->vma_node);
}

uint64_t etnaviv_gem_mmap_offset(struct drm_gem_object *obj)
{
	uint64_t offset;

	mutex_lock(&obj->dev->struct_mutex);
	offset = mmap_offset(obj);
	mutex_unlock(&obj->dev->struct_mutex);

	return offset;
}

/* should be called under struct_mutex.. although it can be called
 * from atomic context without struct_mutex to acquire an extra
 * iova ref if you know one is already held.
 *
 * That means when I do eventually need to add support for unpinning
 * the refcnt counter needs to be atomic_t.
 */
int etnaviv_gem_get_iova_locked(struct etnaviv_gpu *gpu,
	struct drm_gem_object *obj, uint32_t *iova)
{
	struct etnaviv_gem_object *etnaviv_obj = to_etnaviv_bo(obj);
	int ret = 0;

	if (!etnaviv_obj->iova && (etnaviv_obj->backend != ETNA_BO_BACKEND_DMA)) {
		struct etnaviv_drm_private *priv = obj->dev->dev_private;
		struct etnaviv_iommu *mmu = priv->mmu;
		struct page **pages = etnaviv_gem_get_pages(obj);
		uint32_t offset;
		struct drm_mm_node *node = NULL;

		if (IS_ERR(pages))
			return PTR_ERR(pages);

		node = kzalloc(sizeof(*node), GFP_KERNEL);
		if (!node)
			return -ENOMEM;
			

		ret = drm_mm_insert_node(&mmu->mm, node, obj->size, 0);//,
				//DRM_MM_SEARCH_DEFAULT);

		if (!ret) {
			offset = node->start;
			etnaviv_obj->iova = offset;
			etnaviv_obj->gpu_vram_node = node;

			ret = etnaviv_iommu_map(mmu, offset, etnaviv_obj->sgt,
					obj->size, IOMMU_READ | IOMMU_WRITE);
		} else
			kfree(node);
	}

	if (!ret)
		*iova = etnaviv_obj->iova;

	return ret;
}

int etnaviv_gem_get_iova(struct etnaviv_gpu *gpu, struct drm_gem_object *obj,
	int id, uint32_t *iova)
{
	struct etnaviv_gem_object *etnaviv_obj = to_etnaviv_bo(obj);
	int ret;

	/* this is safe right now because we don't unmap until the
	 * bo is deleted:
	 */
	if (etnaviv_obj->iova) {
		*iova = etnaviv_obj->iova;
		return 0;
	}

	mutex_lock(&obj->dev->struct_mutex);
	ret = etnaviv_gem_get_iova_locked(gpu, obj, iova);
	mutex_unlock(&obj->dev->struct_mutex);

	return ret;
}

void etnaviv_gem_put_iova(struct drm_gem_object *obj)
{
	/*
	 * XXX TODO ..
	 * NOTE: probably don't need a _locked() version.. we wouldn't
	 * normally unmap here, but instead just mark that it could be
	 * unmapped (if the iova refcnt drops to zero), but then later
	 * if another _get_iova_locked() fails we can start unmapping
	 * things that are no longer needed..
	 */
}

void *etnaviv_gem_vaddr_locked(struct drm_gem_object *obj)
{
	struct etnaviv_gem_object *etnaviv_obj = to_etnaviv_bo(obj);

	WARN_ON(!mutex_is_locked(&obj->dev->struct_mutex));

	if (!etnaviv_obj->vaddr) {
		struct page **pages = etnaviv_gem_get_pages(obj);

		if (IS_ERR(pages))
			return ERR_CAST(pages);

		etnaviv_obj->vaddr = vmap(pages, obj->size >> PAGE_SHIFT,
				VM_MAP, pgprot_writecombine(PAGE_KERNEL));
	}

	return etnaviv_obj->vaddr;
}

void *etnaviv_gem_vaddr(struct drm_gem_object *obj)
{
	void *ret;

	mutex_lock(&obj->dev->struct_mutex);
	ret = etnaviv_gem_vaddr_locked(obj);
	mutex_unlock(&obj->dev->struct_mutex);

	return ret;
}

dma_addr_t etnaviv_gem_paddr_locked(struct drm_gem_object *obj)
{
	struct etnaviv_gem_object *etnaviv_obj = to_etnaviv_bo(obj);

	WARN_ON(!mutex_is_locked(&obj->dev->struct_mutex));

	return etnaviv_obj->paddr;
}

void etnaviv_gem_move_to_active(struct drm_gem_object *obj,
		struct etnaviv_gpu *gpu, bool write, uint32_t fence)
{
	struct etnaviv_gem_object *etnaviv_obj = to_etnaviv_bo(obj);

	etnaviv_obj->gpu = gpu;

	if (write)
		etnaviv_obj->write_fence = fence;
	else
		etnaviv_obj->read_fence = fence;

	list_del_init(&etnaviv_obj->mm_list);
	list_add_tail(&etnaviv_obj->mm_list, &gpu->active_list);
}

void etnaviv_gem_move_to_inactive(struct drm_gem_object *obj)
{
	struct drm_device *dev = obj->dev;
	struct etnaviv_drm_private *priv = dev->dev_private;
	struct etnaviv_gem_object *etnaviv_obj = to_etnaviv_bo(obj);

	WARN_ON(!mutex_is_locked(&dev->struct_mutex));

	etnaviv_obj->gpu = NULL;
	etnaviv_obj->read_fence = 0;
	etnaviv_obj->write_fence = 0;
	list_del_init(&etnaviv_obj->mm_list);
	list_add_tail(&etnaviv_obj->mm_list, &priv->inactive_list);
}

int etnaviv_gem_cpu_prep(struct drm_gem_object *obj, uint32_t op,
		struct timespec *timeout)
{
	struct drm_device *dev = obj->dev;
	struct etnaviv_gem_object *etnaviv_obj = to_etnaviv_bo(obj);
	int ret = 0;

	if (is_active(etnaviv_obj)) {
		uint32_t fence = 0;

		if (op & ETNA_PREP_READ)
			fence = etnaviv_obj->write_fence;
		if (op & ETNA_PREP_WRITE)
			fence = max(fence, etnaviv_obj->read_fence);
		if (op & ETNA_PREP_NOSYNC)
			timeout = NULL;

		ret = etnaviv_wait_fence_interruptable(dev, etnaviv_obj->gpu,
				fence, timeout);
	}

	/* TODO cache maintenance */

	return ret;
}

int etnaviv_gem_cpu_fini(struct drm_gem_object *obj)
{
	/* TODO cache maintenance */
	return 0;
}

#ifdef CONFIG_DEBUG_FS
static void etnaviv_gem_describe(struct drm_gem_object *obj, struct seq_file *m)
{
	struct drm_device *dev = obj->dev;
	struct etnaviv_gem_object *etnaviv_obj = to_etnaviv_bo(obj);
	uint64_t off = drm_vma_node_start(&obj->vma_node);

	WARN_ON(!mutex_is_locked(&dev->struct_mutex));
	seq_printf(m, "%08x: %c(r=%u,w=%u) %2d (%2d) %08llx %p %zd\n",
			etnaviv_obj->flags, is_active(etnaviv_obj) ? 'A' : 'I',
			etnaviv_obj->read_fence, etnaviv_obj->write_fence,
			obj->name, obj->refcount.refcount.counter,
			off, etnaviv_obj->vaddr, obj->size);
}

void etnaviv_gem_describe_objects(struct list_head *list, struct seq_file *m)
{
	struct etnaviv_gem_object *etnaviv_obj;
	int count = 0;
	size_t size = 0;

	list_for_each_entry(etnaviv_obj, list, mm_list) {
		struct drm_gem_object *obj = &etnaviv_obj->base;

		seq_puts(m, "   ");
		etnaviv_gem_describe(obj, m);
		count++;
		size += obj->size;
	}

	seq_printf(m, "Total %d objects, %zu bytes\n", count, size);
}
#endif

static void etnaviv_free_obj(struct drm_gem_object *obj)
{
	struct etnaviv_gem_object *etnaviv_obj = to_etnaviv_bo(obj);
	struct etnaviv_drm_private *priv = obj->dev->dev_private;
	struct etnaviv_iommu *mmu = priv->mmu;

	if (mmu && etnaviv_obj->iova) {
		uint32_t offset = etnaviv_obj->gpu_vram_node->start;

		etnaviv_iommu_unmap(mmu, offset, etnaviv_obj->sgt, obj->size);
		drm_mm_remove_node(etnaviv_obj->gpu_vram_node);
		kfree(etnaviv_obj->gpu_vram_node);
	}
}

/* called with dev->struct_mutex held */
static int etnaviv_gem_shmem_get_pages(struct etnaviv_gem_object *etnaviv_obj)
{
	struct drm_device *dev = etnaviv_obj->base.dev;
	struct page **p = drm_gem_get_pages(&etnaviv_obj->base);

	if (IS_ERR(p)) {
		dev_err(dev->dev, "could not get pages: %ld\n",
				PTR_ERR(p));
		return PTR_ERR(p);
	}

	etnaviv_obj->pages = p;

	return 0;
}

static int etnaviv_gem_shmem_get_sgt(struct etnaviv_gem_object *etnaviv_obj)
{
	struct drm_device *dev = etnaviv_obj->base.dev;
	int npages = etnaviv_obj->base.size >> PAGE_SHIFT;
	struct sg_table *sgt;

	sgt = drm_prime_pages_to_sg(etnaviv_obj->pages, npages);
	if (IS_ERR(sgt)) {
		dev_err(dev->dev, "failed to allocate sgt: %ld\n",
				PTR_ERR(sgt));
		return PTR_ERR(etnaviv_obj->sgt);
	}

	etnaviv_obj->sgt = sgt;
	return 0;
}

static int etnaviv_gem_shmem_mmap(struct drm_gem_object *obj,
		struct vm_area_struct *vma)
{
	struct etnaviv_gem_object *etnaviv_obj = to_etnaviv_bo(obj);
	pgprot_t vm_page_prot;

	vma->vm_flags &= ~VM_PFNMAP;
	vma->vm_flags |= VM_MIXEDMAP;

	vm_page_prot = vm_get_page_prot(vma->vm_flags);

	if (etnaviv_obj->flags & ETNA_BO_WC) {
		vma->vm_page_prot = pgprot_writecombine(vm_page_prot);
	} else if (etnaviv_obj->flags & ETNA_BO_UNCACHED) {
		vma->vm_page_prot = pgprot_noncached(vm_page_prot);
	} else {
		/*
		 * Shunt off cached objs to shmem file so they have their own
		 * address_space (so unmap_mapping_range does what we want,
		 * in particular in the case of mmap'd dmabufs)
		 */
		fput(vma->vm_file);
		get_file(obj->filp);
		vma->vm_pgoff = 0;
		vma->vm_file  = obj->filp;

		vma->vm_page_prot = vm_page_prot;
	}

	return 0;
}

static void etnaviv_gem_shmem_release(struct etnaviv_gem_object *etnaviv_obj)
{
	if (etnaviv_obj->vaddr)
		vunmap(etnaviv_obj->vaddr);
	put_pages(&etnaviv_obj->base);
}

static const struct etnaviv_gem_ops etnaviv_gem_shmem_ops = {
	.get_pages = etnaviv_gem_shmem_get_pages,
	.get_sgt = etnaviv_gem_shmem_get_sgt,
	.mmap = etnaviv_gem_shmem_mmap,
	.release = etnaviv_gem_shmem_release,
};

static int etnaviv_gem_dma_get_pages(struct etnaviv_gem_object *etnaviv_obj)
{
	return 0;
}

static int etnaviv_gem_dma_get_sgt(struct etnaviv_gem_object *etnaviv_obj)
{
	struct sg_table *sgt;
	int ret;

	sgt = kzalloc(sizeof(*sgt), GFP_KERNEL);
	if (!sgt)
		return -ENOMEM;

	ret = dma_get_sgtable(etnaviv_obj->base.dev->dev, sgt, etnaviv_obj->vaddr,
			       etnaviv_obj->paddr, etnaviv_obj->base.size);

	if (ret < 0)
		kfree(sgt);
	else
		etnaviv_obj->sgt = sgt;

	return ret;
}

static int etnaviv_gem_dma_mmap(struct drm_gem_object *obj,
	struct vm_area_struct *vma)
{
	struct etnaviv_gem_object *etnaviv_obj = to_etnaviv_bo(obj);
	int ret;

	/*
	 * Clear the VM_PFNMAP flag that was set by drm_gem_mmap(), and set the
	 * vm_pgoff (used as a fake buffer offset by DRM) to 0 as we want to map
	 * the whole buffer.
	 */
	vma->vm_flags &= ~VM_PFNMAP;
	vma->vm_pgoff = 0;

	ret = dma_mmap_coherent(obj->dev->dev, vma,
				etnaviv_obj->vaddr, etnaviv_obj->paddr,
				vma->vm_end - vma->vm_start);

	return ret;
}

static void etnaviv_gem_dma_release(struct etnaviv_gem_object *etnaviv_obj)
{
	dma_free_coherent(etnaviv_obj->base.dev->dev, etnaviv_obj->base.size,
		etnaviv_obj->vaddr, etnaviv_obj->paddr);
}

static const struct etnaviv_gem_ops etnaviv_gem_dma_ops = {
	.get_pages = etnaviv_gem_dma_get_pages,
	.get_sgt = etnaviv_gem_dma_get_sgt,
	.mmap = etnaviv_gem_dma_mmap,
	.release = etnaviv_gem_dma_release,
};

void etnaviv_gem_free_object(struct drm_gem_object *obj)
{
	struct drm_device *dev = obj->dev;
	struct etnaviv_gem_object *etnaviv_obj = to_etnaviv_bo(obj);

	WARN_ON(!mutex_is_locked(&dev->struct_mutex));

	/* object should not be on active list: */
	if (!etnaviv_obj->is_ring_buffer)
		WARN_ON(is_active(etnaviv_obj));

	list_del(&etnaviv_obj->mm_list);

	if (etnaviv_obj->backend != ETNA_BO_BACKEND_DMA)
		etnaviv_free_obj(obj);

	drm_gem_free_mmap_offset(obj);
	etnaviv_obj->ops->release(etnaviv_obj);
	reservation_object_fini(&etnaviv_obj->_resv);
	drm_gem_object_release(obj);

	kfree(etnaviv_obj);
}

/* convenience method to construct a GEM buffer object, and userspace handle */
int etnaviv_gem_new_handle(struct drm_device *dev, struct drm_file *file,
		uint32_t size, uint32_t flags, uint32_t *handle)
{
	struct drm_gem_object *obj;
	int ret;

	ret = mutex_lock_interruptible(&dev->struct_mutex);
	if (ret)
		return ret;

	obj = etnaviv_gem_new(dev, size, flags);

	mutex_unlock(&dev->struct_mutex);

	if (IS_ERR(obj))
		return PTR_ERR(obj);

	ret = drm_gem_handle_create(file, obj, handle);

	/* drop reference from allocate - handle holds it now */
	drm_gem_object_unreference_unlocked(obj);

	return ret;
}

static int etnaviv_gem_new_impl(struct drm_device *dev,
		uint32_t size, uint32_t flags,
		struct drm_gem_object **obj)
{
	struct etnaviv_drm_private *priv = dev->dev_private;
	struct etnaviv_gem_object *etnaviv_obj;
	unsigned sz = sizeof(*etnaviv_obj);
	bool valid = true;
	enum etna_bo_backend backend;

	/* validate flags */
	if (flags & (ETNA_BO_CMDSTREAM | ETNA_BO_SCANOUT)) {
		backend = ETNA_BO_BACKEND_DMA;
		if ((flags & ETNA_BO_CACHE_MASK) != 0)
			valid = false;
	} else {
		backend = ETNA_BO_BACKEND_SHMEM;
		switch (flags & ETNA_BO_CACHE_MASK) {
		case ETNA_BO_UNCACHED:
		case ETNA_BO_CACHED:
		case ETNA_BO_WC:
			break;
		default:
			valid = false;
		}
	}

	if (!valid) {
		dev_err(dev->dev, "invalid cache flag: %x (cmd: %d)\n",
				(flags & ETNA_BO_CACHE_MASK),
				(flags & ETNA_BO_CMDSTREAM));
		return -EINVAL;
	}

	etnaviv_obj = kzalloc(sz, GFP_KERNEL);
	if (!etnaviv_obj)
		return -ENOMEM;

	if (backend == ETNA_BO_BACKEND_DMA) {
		etnaviv_obj->vaddr = dma_alloc_coherent(dev->dev, size,
				&etnaviv_obj->paddr, GFP_KERNEL);

		if (!etnaviv_obj->vaddr) {
			kfree(etnaviv_obj);
			return -ENOMEM;
		}
	}

	etnaviv_obj->backend = backend;
	etnaviv_obj->flags = flags;

	etnaviv_obj->resv = &etnaviv_obj->_resv;
	reservation_object_init(&etnaviv_obj->_resv);

	INIT_LIST_HEAD(&etnaviv_obj->submit_entry);
	list_add_tail(&etnaviv_obj->mm_list, &priv->inactive_list);

	*obj = &etnaviv_obj->base;

	return 0;
}

struct drm_gem_object *etnaviv_gem_new(struct drm_device *dev,
		uint32_t size, uint32_t flags)
{
	struct drm_gem_object *obj = NULL;
	struct etnaviv_gem_object *etnaviv_obj;
	int ret;

	WARN_ON(!mutex_is_locked(&dev->struct_mutex));

	size = PAGE_ALIGN(size);

	ret = etnaviv_gem_new_impl(dev, size, flags, &obj);
	if (ret)
		goto fail;

	etnaviv_obj = to_etnaviv_bo(obj);
	if (etnaviv_obj->backend == ETNA_BO_BACKEND_DMA) {
		etnaviv_obj->ops = &etnaviv_gem_dma_ops;
		drm_gem_private_object_init(dev, obj, size);
	} else {
		etnaviv_obj->ops = &etnaviv_gem_shmem_ops;
		ret = drm_gem_object_init(dev, obj, size);
	}

	if (ret)
		goto fail;

	return obj;

fail:
	if (obj)
		drm_gem_object_unreference(obj);

	return ERR_PTR(ret);
}

int etnaviv_gem_new_private(struct drm_device *dev, size_t size, uint32_t flags,
	struct etnaviv_gem_object **res)
{
	struct drm_gem_object *obj;
	int ret;

	ret = etnaviv_gem_new_impl(dev, size, flags, &obj);
	if (ret)
		return ret;

	drm_gem_private_object_init(dev, obj, size);

	*res = to_etnaviv_bo(obj);

	return 0;
}
