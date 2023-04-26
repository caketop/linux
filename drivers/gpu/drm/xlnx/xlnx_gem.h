/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Xilinx DRM KMS GEM helper header
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

#ifndef _XLNX_GEM_H_
#define _XLNX_GEM_H_

struct dma_buf_attachment;
struct drm_printer;
struct sg_table;

int   xlnx_gem_dumb_create(struct drm_file *file_priv,
                           struct drm_device *drm,
                           struct drm_mode_create_dumb *args);

struct drm_gem_object* xlnx_gem_create_object(struct drm_device *drm, size_t size, bool cache);
struct drm_gem_object* xlnx_gem_create_with_handle(struct drm_file *file_priv,
                                              struct drm_device *drm, size_t size, bool cache,
                                              uint32_t *handle);
struct drm_gem_object* xlnx_gem_prime_import_sg_table(struct drm_device *dev,
                                              struct dma_buf_attachment *attach,
                                              struct sg_table *sgt);
struct drm_gem_object* xlnx_gem_prime_import_sg_table_vmap(struct drm_device *dev,
                                              struct dma_buf_attachment *attach,
                                              struct sg_table *sgt);

void  xlnx_gem_free_object(struct drm_gem_object *gem_obj);

struct sg_table* xlnx_gem_prime_get_sg_table(struct drm_gem_object *gem_obj);

int   xlnx_gem_file_mmap(struct file *filp, struct vm_area_struct *vma);
int   xlnx_gem_prime_mmap(struct drm_gem_object *obj, struct vm_area_struct *vma);
void* xlnx_gem_prime_vmap(struct drm_gem_object *gem_obj);
void  xlnx_gem_prime_vunmap(struct drm_gem_object *gem_obj, void *vaddr);
void  xlnx_gem_print_info(struct drm_printer *p,
                          unsigned int indent,
                          const struct drm_gem_object *obj);

#endif /* _XLNX_GEM_H_ */
