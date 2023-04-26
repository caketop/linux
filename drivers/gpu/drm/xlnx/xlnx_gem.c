// SPDX-License-Identifier: GPL-2.0
/*
 * Xilinx DRM GEM helper
 *
 *  Copyright 2022 Ichiro Kawazome
 *
 *  Copyright (C) 2015 - 2018 Xilinx, Inc.
 *
 *  Author: Hyun Woo Kwon <hyun.kwon@xilinx.com>
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/dma-buf.h>
#include <linux/dma-mapping.h>
#include <linux/export.h>
#include <linux/mm.h>
#include <linux/mutex.h>
#include <linux/slab.h>

#include <drm/drm.h>
#include <drm/drm_device.h>
#include <drm/drm_drv.h>
#include <drm/xlnx_drm.h>
#include <drm/drm_vma_manager.h>
#include <drm/drm_file.h>
#include <drm/drm_ioctl.h>
#include <drm/drm_gem.h>

#include "xlnx_drv.h"
#include "xlnx_gem.h"

/**
 * struct xlnx_gem_object - Xilinx GEM object backed by CMA memory allocations
 * @base:      base GEM object
 * @paddr:     physical address of the backing memory
 * @vaddr:     kernel virtual address of the backing memory
 * @size:      size of the backing memory
 * @sgt:       scatter/gather table for imported PRIME buffers. 
 *             The table can have more than one entry but they are guaranteed to 
 *             have contiguous DMA addresses.
 * @cached:    map object cached (instead of using writecombine).
 */
struct xlnx_gem_object {
	struct drm_gem_object   base;
	dma_addr_t              paddr;
	void*                   vaddr;
	size_t                  size;
	struct sg_table*        sgt;
	bool                    cached;
};

#define to_xlnx_gem_object(gem_obj) \
	container_of(gem_obj, struct xlnx_gem_object, base)


/**
 * xlnx_gem_free_object() - free resources associated with a Xilinx GEM object
 * @gem_obj:   GEM object to free
 *
 * This function frees the backing memory of the Xilinx GEM object, cleans up 
 * the GEM object state and frees the memory used to store the object itself.
 * If the buffer is imported and the virtual address is set, it is released.
 */
void xlnx_gem_free_object(struct drm_gem_object *gem_obj)
{
	struct xlnx_gem_object* xlnx_obj;

	xlnx_obj = to_xlnx_gem_object(gem_obj);

	if (gem_obj->import_attach) {
		if (xlnx_obj->vaddr) {
			dma_buf_vunmap(gem_obj->import_attach->dmabuf, xlnx_obj->vaddr);
			xlnx_obj->vaddr = NULL;
		}
		drm_prime_gem_destroy(gem_obj, xlnx_obj->sgt);
	} else {
		if (xlnx_obj->vaddr) {
			struct device* dev   = gem_obj->dev->dev;
			size_t         size  = xlnx_obj->size;
			void*          vaddr = xlnx_obj->vaddr;
			dma_addr_t     paddr = xlnx_obj->paddr;
			if (xlnx_obj->cached)
				dma_free_coherent(dev, size, vaddr, paddr);
			else
				dma_free_wc(      dev, size, vaddr, paddr);
			xlnx_obj->vaddr = NULL;
		}
	}

	drm_gem_object_release(gem_obj);

	kfree(xlnx_obj);
}

/**
 * xlnx_gem_print_info() - Print &xlnx_gem_object info for debugfs
 * @p:         DRM printer
 * @indent:    Tab indentation level
 * @obj:       GEM object
 *
 * This function can be used as the &drm_driver->gem_print_info callback.
 * It prints paddr and vaddr for use in e.g. debugfs output.
 */
void xlnx_gem_print_info(struct drm_printer *p,
			 unsigned int indent,
			 const struct drm_gem_object *obj)
{
	const struct xlnx_gem_object* xlnx_obj = to_xlnx_gem_object(obj);

	drm_printf_indent(p, indent, "paddr=%pad\n", &xlnx_obj->paddr);
	drm_printf_indent(p, indent, "vaddr=%p\n"  ,  xlnx_obj->vaddr);
}

/**
 * xlnx_gem_vm_fault() - Xilinx GEM object vm area fault operation.
 * @vfm:       Pointer to the vm fault structure.
 * Return:     VM_FAULT_RETURN_TYPE (Success(=0) or error status(!=0)).
 */
static
vm_fault_t xlnx_gem_vm_fault(struct vm_fault *vmf)
{
	struct vm_area_struct*  vma      = vmf->vma;
	struct drm_gem_object*  gem_obj  = vma->vm_private_data;
	struct xlnx_gem_object* xlnx_obj = to_xlnx_gem_object(gem_obj);
	unsigned long offset             = vmf->address - vma->vm_start;
	unsigned long virt_addr          = vmf->address;
	unsigned long phys_addr          = xlnx_obj->paddr + offset;
	unsigned long page_frame_num     = phys_addr  >> PAGE_SHIFT;
	unsigned long request_size       = 1          << PAGE_SHIFT;
	unsigned long available_size     = xlnx_obj->size - offset;

	if (request_size > available_size)
	        return VM_FAULT_SIGBUS;

	if (!pfn_valid(page_frame_num))
	        return VM_FAULT_SIGBUS;

	return vmf_insert_pfn(vma, virt_addr, page_frame_num);
}

/**
 * Xilinx GEM object vm operation table.
 */
const struct vm_operations_struct xlnx_gem_vm_ops = {
	.fault = xlnx_gem_vm_fault,
	.open  = drm_gem_vm_open,
	.close = drm_gem_vm_close,
};

/**
 * __xlnx_gem_mmap() - memory-map a Xilinx GEM object
 * @gem_obj:   GEM object
 * @vma:       VMA for the area to be mapped
 *
 * Returns:    0 on success.
 */
static
int __xlnx_gem_mmap(struct drm_gem_object *gem_obj, struct vm_area_struct *vma)
{
	struct xlnx_gem_object *xlnx_obj = to_xlnx_gem_object(gem_obj);

	if (gem_obj->import_attach) {
		/* Drop the reference drm_gem_mmap_obj() acquired.*/
		drm_gem_object_put(gem_obj);
		vma->vm_private_data = NULL;

		return dma_buf_mmap(gem_obj->dma_buf, vma, 0);
	}
	
        vma->vm_flags |= VM_IO | VM_PFNMAP | VM_DONTEXPAND | VM_DONTDUMP;
	vma->vm_page_prot = vm_get_page_prot(vma->vm_flags);
	if (!xlnx_obj->cached)
        	vma->vm_page_prot = pgprot_writecombine(vma->vm_page_prot);
	vma->vm_ops = &xlnx_gem_vm_ops;

	return 0;
}

/**
 * xlnx_gem_file_mmap() - memory-map a Xilinx GEM object by file operation.
 * @filp:      file object
 * @vma:       VMA for the area to be mapped
 *
 * Returns:    0 on success or a negative error code on failure.
 */
int xlnx_gem_file_mmap(struct file *filp, struct vm_area_struct *vma)
{
	struct drm_gem_object  *gem_obj;
	int ret;

	ret = drm_gem_mmap(filp, vma);
	if (ret)
		return ret;

	gem_obj  = vma->vm_private_data;

	return __xlnx_gem_mmap(gem_obj, vma);
}

/**
 * xlnx_gem_prime_mmap() - memory-map a Xilinx GEM object by prime operation.
 * @gem_obj:   GEM object
 * @vma:       VMA for the area to be mapped
 *
 * Returns:    0 on success or a negative error code on failure.
 */
int xlnx_gem_prime_mmap(struct drm_gem_object *gem_obj, struct vm_area_struct *vma)
{
	int ret;

	ret = drm_gem_mmap_obj(gem_obj, gem_obj->size, vma);
	if (ret < 0)
		return ret;

	return __xlnx_gem_mmap(gem_obj, vma);
}

/**
 * xlnx_gem_get_sg_table() - provide a scatter/gather table of pinned
 *     pages for a Xilinx GEM object
 * @gem_obj:   GEM object
 *
 * This function exports a scatter/gather table suitable for PRIME usage by
 * calling the standard DMA mapping API. 
 *
 * Returns:
 * A pointer to the scatter/gather table of pinned pages or NULL on failure.
 */
struct sg_table *xlnx_gem_prime_get_sg_table(struct drm_gem_object *gem_obj)
{
	struct xlnx_gem_object* xlnx_obj = to_xlnx_gem_object(gem_obj);
	struct sg_table*        sgt;
	int ret;

	sgt = kzalloc(sizeof(*sgt), GFP_KERNEL);
	if (!sgt)
		return ERR_PTR(-ENOMEM);

	ret = dma_get_sgtable(
		gem_obj->dev->dev, /* struct device*   dev      */
		sgt              , /* struct sg_table* sgt      */
		xlnx_obj->vaddr  , /* void*            cpu_addr */
		xlnx_obj->paddr  , /* dma_addr_t       dma_addr */
		gem_obj->size      /* size_t           size     */
	      );
	if (ret < 0) {
		DRM_ERROR("failed to get sgtable, return=%d", ret);
		goto out;
	}

	return sgt;

out:
	kfree(sgt);
	return ERR_PTR(ret);
}

/**
 * xlnx_gem_vmap() - map a Xilinx GEM object into the kernel's virtual
 *     address space
 * @gem_obj:   GEM object
 *
 * This function maps a buffer exported via DRM PRIME into the kernel's
 * virtual address space. Since the CMA buffers are already mapped into the
 * kernel virtual address space this simply returns the cached virtual
 * address. 
 *
 * Returns:
 * The kernel virtual address of the Xilinx GEM object's backing store.
 */
void* xlnx_gem_prime_vmap(struct drm_gem_object *gem_obj)
{
	struct xlnx_gem_object* xlnx_obj = to_xlnx_gem_object(gem_obj);

	if ((xlnx_obj->vaddr == NULL) && (gem_obj->import_attach)) {
                void* vaddr = dma_buf_vmap(gem_obj->import_attach->dmabuf);
        	if (!vaddr) 
        		DRM_ERROR("Failed to vmap PRIME buffer");
                xlnx_obj->vaddr = vaddr;
        }
	return xlnx_obj->vaddr;
}

/**
 * xlnx_gem_vunmap() - unmap a Xilinx GEM object from the kernel's virtual
 *     address space
 * @gem_obj:   GEM object
 * @vaddr:     kernel virtual address where the GEM object was mapped
 *
 * This function removes a buffer exported via DRM PRIME from the kernel's
 * virtual address space. This is a no-op because CMA buffers cannot be
 * unmapped from kernel space. 
 */
void xlnx_gem_prime_vunmap(struct drm_gem_object *gem_obj, void *vaddr)
{
	/* Nothing to do */
}

/**
 * Xilinx GEM object function table.
 */
static const struct drm_gem_object_funcs xlnx_gem_funcs = {
    	.free         = xlnx_gem_free_object, 
    	.print_info   = xlnx_gem_print_info, 
    	.get_sg_table = xlnx_gem_prime_get_sg_table,
    	.vmap         = xlnx_gem_prime_vmap,
    	.vunmap       = xlnx_gem_prime_vunmap,
	.vm_ops       = &xlnx_gem_vm_ops,
    /* 	.mmap         = xlnx_gem_mmap, */
};

/**
 * __xlnx_gem_create() - Create a Xilinx GEM object without allocating memory
 * @drm:       DRM device
 * @size:      size of the object to allocate
 *
 * This function creates and initializes a Xilinx GEM object of the given size,
 * but doesn't allocate any memory to back the object.
 *
 * Returns:
 * A struct xlnx_gem_object * on success or an ERR_PTR()-encoded negative
 * error code on failure.
 */
static
struct xlnx_gem_object*
__xlnx_gem_create(struct drm_device *drm, size_t size)
{
	struct xlnx_gem_object* xlnx_obj;
	struct drm_gem_object*  gem_obj;
	int ret;

	if (drm->driver->gem_create_object)
		gem_obj = drm->driver->gem_create_object(drm, size);
	else
		gem_obj = kzalloc(sizeof(*xlnx_obj), GFP_KERNEL);

	if (!gem_obj) {
		ret = -ENOMEM;
		DRM_DEV_ERROR(drm->dev, "failed to allocate gem object, return=%d", ret);
		goto error;
	}

	/* if (!gem_obj->funcs)
	 *	gem_obj->funcs = &xlnx_gem_funcs;
         */

	ret = drm_gem_object_init(drm, gem_obj, size);
	if (ret) {
		DRM_DEV_ERROR(drm->dev, "failed to initialize gem object, return=%d", ret);
		goto error;
	}

	ret = drm_gem_create_mmap_offset(gem_obj);
	if (ret) {
		DRM_DEV_ERROR(drm->dev, "failed to create mmap offset, return=%d", ret);
		drm_gem_object_release(gem_obj);
		goto error;
	}

	xlnx_obj = to_xlnx_gem_object(gem_obj);
	xlnx_obj->paddr  = (dma_addr_t)NULL;
	xlnx_obj->vaddr  = NULL;
	xlnx_obj->size   = 0;
	xlnx_obj->cached = 0;
	
	return xlnx_obj;

error:
	if (gem_obj)
		kfree(gem_obj);
	return ERR_PTR(ret);
}

/**
 * xlnx_gem_create - Create a Xilinx GEM object with allocating memory
 * @drm:       DRM device
 * @size:      size of the object to allocate
 * @cache:     cache mode
 *
 * This function creates a Xilinx GEM object and allocates a contiguous chunk of
 * memory as backing store. The backing memory has the writecombine attribute
 * set.
 *
 * Returns:
 * A struct drm_gem_object * on success or an ERR_PTR()-encoded negative
 * error code on failure.
 */
struct drm_gem_object*
xlnx_gem_create_object(struct drm_device *drm, size_t size, bool cache)
{
	struct xlnx_gem_object* xlnx_obj;
	struct drm_gem_object*  gem_obj;
	void*                   vaddr;
	dma_addr_t              paddr;
	int                     ret;

	size = round_up(size, PAGE_SIZE);

	xlnx_obj = __xlnx_gem_create(drm, size);
	if (IS_ERR(xlnx_obj))
		return ERR_CAST(xlnx_obj);

	gem_obj = &xlnx_obj->base;

	if (cache)
		vaddr = dma_alloc_coherent(drm->dev, size, &paddr, GFP_KERNEL | __GFP_NOWARN);
	else
		vaddr = dma_alloc_wc(      drm->dev, size, &paddr, GFP_KERNEL | __GFP_NOWARN);
	
	if (IS_ERR_OR_NULL(vaddr)) {
		ret = (IS_ERR(vaddr)) ? PTR_ERR(vaddr) : -ENOMEM;
		DRM_ERROR("failed to allocate buffer with size=%zu, return=%d", size, ret);
		goto error;
	}

	xlnx_obj->paddr  = paddr;
	xlnx_obj->vaddr  = vaddr;
	xlnx_obj->size   = size;
	xlnx_obj->cached = cache;

	return gem_obj;

error:
	drm_gem_object_put(gem_obj);
	return ERR_PTR(ret);
}

/**
 * xlnx_gem_create_with_handle - allocate an object with the given size and
 *     return a GEM handle to it
 * @file_priv: DRM file-private structure to register the handle for
 * @drm:       DRM device
 * @size:      size of the object to allocate
 * @cache:     cache mode
 * @handle:    return location for the GEM handle
 *
 * This function creates a GEM object, allocating a physically contiguous
 * chunk of memory as backing store. The GEM object is then added to the list
 * of object associated with the given file and a handle to it is returned.
 *
 * Returns:
 * A struct drm_gem_object * on success or an ERR_PTR()-encoded negative
 * error code on failure.
 */
struct drm_gem_object*
xlnx_gem_create_with_handle(struct drm_file *file_priv,
			    struct drm_device *drm, size_t size, bool cache, 
			    uint32_t *handle)
{
	struct drm_gem_object*  gem_obj;
	int ret;

	gem_obj = xlnx_gem_create_object(drm, size, cache);
	if (IS_ERR(gem_obj))
		return ERR_CAST(gem_obj);

	/*
	 * allocate a id of idr table where the obj is registered
	 * and handle has the id what user can see.
	 */
	ret = drm_gem_handle_create(file_priv, gem_obj, handle);
	/* drop reference from allocate - handle holds it now. */
	drm_gem_object_put(gem_obj);
	if (ret) {
		DRM_ERROR("failed to create gem handle, return=%d", ret);
		return ERR_PTR(ret);
	}

	return gem_obj;
}

/**
 * xlnx_gem_prime_import_sg_table() - produce a GEM object from another
 *     driver's scatter/gather table of pinned pages
 * @dev:       device to import into
 * @attach:    DMA-BUF attachment
 * @sgt:       scatter/gather table of pinned pages
 *
 * This function imports a scatter/gather table exported via DMA-BUF by
 * another driver. Imported buffers must be physically contiguous in memory
 * (i.e. the scatter/gather table must contain a single entry). Drivers that
 * use the CMA helpers should set this as their
 * &drm_driver.gem_prime_import_sg_table callback.
 *
 * Returns:
 * A pointer to a newly created GEM object or an ERR_PTR-encoded negative
 * error code on failure.
 */
struct drm_gem_object*
xlnx_gem_prime_import_sg_table(struct drm_device *dev,
			       struct dma_buf_attachment *attach,
			       struct sg_table *sgt)
{
	struct xlnx_gem_object *xlnx_obj;

	/* check if the entries in the sg_table are contiguous */
	if (drm_prime_get_contiguous_size(sgt) < attach->dmabuf->size) {
		DRM_ERROR("buffer chunks must be mapped contiguously");
		return ERR_PTR(-EINVAL);
	}

	/* Create a Xilinx GEM Object */
	xlnx_obj = __xlnx_gem_create(dev, attach->dmabuf->size);
	if (IS_ERR(xlnx_obj))
		return ERR_CAST(xlnx_obj);

	xlnx_obj->paddr = sg_dma_address(sgt->sgl);
	xlnx_obj->sgt   = sgt;

	DRM_DEBUG_PRIME("dma_addr = %pad, size = %zu\n", &xlnx_obj->paddr, attach->dmabuf->size);

	return &xlnx_obj->base;
}
/**
 * xlnx_gem_prime_import_sg_table_vmap() - PRIME import another driver's
 *	scatter/gather table and get the virtual address of the buffer
 * @dev:       device to import into
 * @attach:    DMA-BUF attachment
 * @sgt:       Scatter/gather table of pinned pages
 *
 * This function imports a scatter/gather table using
 * xlnx_gem_prime_import_sg_table() and uses dma_buf_vmap() to get the kernel
 * virtual address. This ensures that a GEM object always has its virtual
 * address set. This address is released when the object is freed.
 *
 * Returns:
 * A pointer to a newly created GEM object or an ERR_PTR-encoded negative
 * error code on failure.
 */
struct drm_gem_object*
xlnx_gem_prime_import_sg_table_vmap(struct drm_device *dev,
				    struct dma_buf_attachment *attach,
				    struct sg_table *sgt)
{
	struct xlnx_gem_object* xlnx_obj;
	struct drm_gem_object*  gem_obj;
	void *vaddr;

	vaddr = dma_buf_vmap(attach->dmabuf);
	if (!vaddr) {
		DRM_ERROR("Failed to vmap PRIME buffer");
		return ERR_PTR(-ENOMEM);
	}

	gem_obj = xlnx_gem_prime_import_sg_table(dev, attach, sgt);
	if (IS_ERR(gem_obj)) {
		dma_buf_vunmap(attach->dmabuf, vaddr);
		return gem_obj;
	}

	xlnx_obj = to_xlnx_gem_object(gem_obj);
	xlnx_obj->vaddr = vaddr;

	return gem_obj;
}

struct drm_mode_create_dumb;
/*
 * xlnx_gem_dumb_create - (struct drm_driver)->dumb_create callback
 * @file_priv: drm_file object
 * @drm: DRM object
 * @args: info for dumb buffer creation
 *
 * This function is for dumb_create callback of drm_driver struct. Simply
 * it wraps around drm_gem_cma_dumb_create() and sets the pitch value
 * by retrieving the value from the device.
 *
 * Return: The return value from drm_gem_cacma_create_with_handle()
 */
int
xlnx_gem_dumb_create(struct drm_file *file_priv,
                     struct drm_device *drm,
                     struct drm_mode_create_dumb *args)
{
	bool scanout = (((args->flags) & DRM_XLNX_GEM_DUMB_SCANOUT_MASK) == DRM_XLNX_GEM_DUMB_SCANOUT);
        bool cache;
	int  align     = xlnx_get_align(drm, scanout);
	int  min_pitch = DIV_ROUND_UP(args->width * args->bpp, 8);
	struct drm_gem_object* gem_obj;

	if (!args->pitch || !IS_ALIGNED(args->pitch, align))
		args->pitch = ALIGN(min_pitch, align);

	if (args->pitch < min_pitch)
		args->pitch = min_pitch;

	if (args->size < args->pitch * args->height)
		args->size = args->pitch * args->height;

	switch ((args->flags) & DRM_XLNX_GEM_DUMB_CACHE_MASK) {
        case DRM_XLNX_GEM_DUMB_CACHE_OFF:
            cache = false;
            break;
        case DRM_XLNX_GEM_DUMB_CACHE_ON:
            cache = true;
            break;
        case DRM_XLNX_GEM_DUMB_CACHE_DEFAULT:
            cache = (xlnx_get_dumb_cache_default_mode(drm) != 0);
            break;
        default:
            cache = false;
            break;
        }

	DRM_DEBUG("width=%d, height=%d, bpp=%d, pitch=%d, align=%d, cache=%d\n",
                  args->width, args->height, args->bpp, args->pitch, align, cache);
                  
	gem_obj = xlnx_gem_create_with_handle(file_priv, drm, args->size, cache, &args->handle);

	return PTR_ERR_OR_ZERO(gem_obj);
}
