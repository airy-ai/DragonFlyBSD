/*-
 * Copyright © 2008 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 * Authors:
 *    Eric Anholt <eric@anholt.net>
 *
 * Copyright (c) 2011 The FreeBSD Foundation
 * All rights reserved.
 *
 * This software was developed by Konstantin Belousov under sponsorship from
 * the FreeBSD Foundation.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * $FreeBSD: head/sys/dev/drm2/i915/i915_gem.c 253497 2013-07-20 13:52:40Z kib $
 */

#include <sys/resourcevar.h>
#include <sys/sfbuf.h>

#include <drm/drmP.h>
#include <drm/i915_drm.h>
#include "i915_drv.h"
#include "intel_drv.h"
#include "intel_ringbuffer.h"
#include <linux/completion.h>

static void i915_gem_object_flush_gtt_write_domain(struct drm_i915_gem_object *obj);
static void i915_gem_object_flush_cpu_write_domain(struct drm_i915_gem_object *obj);
static int i915_gem_object_bind_to_gtt(struct drm_i915_gem_object *obj,
    unsigned alignment, bool map_and_fenceable);

static int i915_gem_phys_pwrite(struct drm_device *dev,
    struct drm_i915_gem_object *obj, uint64_t data_ptr, uint64_t offset,
    uint64_t size, struct drm_file *file_priv);

static uint32_t i915_gem_get_gtt_size(struct drm_device *dev, uint32_t size,
    int tiling_mode);
static uint32_t i915_gem_get_gtt_alignment(struct drm_device *dev,
    uint32_t size, int tiling_mode);
static int i915_gem_object_get_pages_gtt(struct drm_i915_gem_object *obj,
    int flags);
static int i915_gem_object_set_cpu_read_domain_range(
    struct drm_i915_gem_object *obj, uint64_t offset, uint64_t size);
static void i915_gem_object_finish_gtt(struct drm_i915_gem_object *obj);
static void i915_gem_object_truncate(struct drm_i915_gem_object *obj);
static int i915_gem_object_is_purgeable(struct drm_i915_gem_object *obj);
static bool i915_gem_object_is_inactive(struct drm_i915_gem_object *obj);
static int i915_gem_object_needs_bit17_swizzle(struct drm_i915_gem_object *obj);
static vm_page_t i915_gem_wire_page(vm_object_t object, vm_pindex_t pindex);
static void i915_gem_process_flushing_list(struct intel_ring_buffer *ring,
    uint32_t flush_domains);
static void i915_gem_clear_fence_reg(struct drm_device *dev,
    struct drm_i915_fence_reg *reg);
static void i915_gem_reset_fences(struct drm_device *dev);
static void i915_gem_lowmem(void *arg);

static int i915_gem_obj_io(struct drm_device *dev, uint32_t handle, uint64_t data_ptr,
    uint64_t size, uint64_t offset, enum uio_rw rw, struct drm_file *file);

MALLOC_DEFINE(DRM_I915_GEM, "i915gem", "Allocations from i915 gem");
long i915_gem_wired_pages_cnt;

/* some bookkeeping */
static void i915_gem_info_add_obj(struct drm_i915_private *dev_priv,
				  size_t size)
{

	dev_priv->mm.object_count++;
	dev_priv->mm.object_memory += size;
}

static void i915_gem_info_remove_obj(struct drm_i915_private *dev_priv,
				     size_t size)
{

	dev_priv->mm.object_count--;
	dev_priv->mm.object_memory -= size;
}

static int
i915_gem_wait_for_error(struct drm_device *dev)
{
	struct drm_i915_private *dev_priv = dev->dev_private;
	struct completion *x = &dev_priv->error_completion;
	int ret;

	if (!atomic_read(&dev_priv->mm.wedged))
		return 0;

	/*
	 * Only wait 10 seconds for the gpu reset to complete to avoid hanging
	 * userspace. If it takes that long something really bad is going on and
	 * we should simply try to bail out and fail as gracefully as possible.
	 */
	ret = wait_for_completion_interruptible_timeout(x, 10*hz);
	if (ret == 0) {
		DRM_ERROR("Timed out waiting for the gpu reset to complete\n");
		return -EIO;
	} else if (ret < 0) {
		return ret;
	}

	if (atomic_read(&dev_priv->mm.wedged)) {
		/* GPU is hung, bump the completion count to account for
		 * the token we just consumed so that we never hit zero and
		 * end up waiting upon a subsequent completion event that
		 * will never happen.
		 */
		spin_lock(&x->wait.lock);
		x->done++;
		spin_unlock(&x->wait.lock);
	}
	return 0;
}

int i915_mutex_lock_interruptible(struct drm_device *dev)
{
	int ret;

	ret = i915_gem_wait_for_error(dev);
	if (ret != 0)
		return (ret);

	ret = lockmgr(&dev->dev_struct_lock, LK_EXCLUSIVE|LK_SLEEPFAIL);
	if (ret)
		return -EINTR;

#if 0
	WARN_ON(i915_verify_lists(dev));
#endif
	return 0;
}

static inline bool
i915_gem_object_is_inactive(struct drm_i915_gem_object *obj)
{
	return (obj->gtt_space && !obj->active && obj->pin_count == 0);
}

int
i915_gem_init_ioctl(struct drm_device *dev, void *data,
    struct drm_file *file)
{
	struct drm_i915_gem_init *args;
	drm_i915_private_t *dev_priv;

	dev_priv = dev->dev_private;
	args = data;

	if (args->gtt_start >= args->gtt_end ||
	    (args->gtt_end | args->gtt_start) & (PAGE_SIZE - 1))
		return (-EINVAL);

	/*
	 * XXXKIB. The second-time initialization should be guarded
	 * against.
	 */
	lockmgr(&dev->dev_lock, LK_EXCLUSIVE|LK_RETRY|LK_CANRECURSE);
	i915_gem_do_init(dev, args->gtt_start, args->gtt_end, args->gtt_end);
	lockmgr(&dev->dev_lock, LK_RELEASE);

	return 0;
}

int
i915_gem_get_aperture_ioctl(struct drm_device *dev, void *data,
			    struct drm_file *file)
{
	struct drm_i915_private *dev_priv;
	struct drm_i915_gem_get_aperture *args;
	struct drm_i915_gem_object *obj;
	size_t pinned;

	dev_priv = dev->dev_private;
	args = data;

	if (!(dev->driver->driver_features & DRIVER_GEM))
		return (-ENODEV);

	pinned = 0;
	DRM_LOCK(dev);
	list_for_each_entry(obj, &dev_priv->mm.pinned_list, mm_list)
		pinned += obj->gtt_space->size;
	DRM_UNLOCK(dev);

	args->aper_size = dev_priv->mm.gtt_total;
	args->aper_available_size = args->aper_size - pinned;

	return (0);
}

int
i915_gem_create(struct drm_file *file, struct drm_device *dev, uint64_t size,
    uint32_t *handle_p)
{
	struct drm_i915_gem_object *obj;
	uint32_t handle;
	int ret;

	size = roundup(size, PAGE_SIZE);
	if (size == 0)
		return (-EINVAL);

	obj = i915_gem_alloc_object(dev, size);
	if (obj == NULL)
		return (-ENOMEM);

	handle = 0;
	ret = drm_gem_handle_create(file, &obj->base, &handle);
	if (ret != 0) {
		drm_gem_object_release(&obj->base);
		i915_gem_info_remove_obj(dev->dev_private, obj->base.size);
		drm_free(obj, DRM_I915_GEM);
		return (-ret);
	}

	/* drop reference from allocate - handle holds it now */
	drm_gem_object_unreference(&obj->base);
	*handle_p = handle;
	return (0);
}

int
i915_gem_dumb_create(struct drm_file *file,
		     struct drm_device *dev,
		     struct drm_mode_create_dumb *args)
{

	/* have to work out size/pitch and return them */
	args->pitch = roundup2(args->width * ((args->bpp + 7) / 8), 64);
	args->size = args->pitch * args->height;
	return (i915_gem_create(file, dev, args->size, &args->handle));
}

int i915_gem_dumb_destroy(struct drm_file *file,
			  struct drm_device *dev,
			  uint32_t handle)
{

	return (drm_gem_handle_delete(file, handle));
}

/**
 * Creates a new mm object and returns a handle to it.
 */
int
i915_gem_create_ioctl(struct drm_device *dev, void *data,
		      struct drm_file *file)
{
	struct drm_i915_gem_create *args = data;

	return (i915_gem_create(file, dev, args->size, &args->handle));
}

static int i915_gem_object_needs_bit17_swizzle(struct drm_i915_gem_object *obj)
{
	drm_i915_private_t *dev_priv;

	dev_priv = obj->base.dev->dev_private;
	return (dev_priv->mm.bit_6_swizzle_x == I915_BIT_6_SWIZZLE_9_10_17 &&
	    obj->tiling_mode != I915_TILING_NONE);
}

/**
 * Reads data from the object referenced by handle.
 *
 * On error, the contents of *data are undefined.
 */
int
i915_gem_pread_ioctl(struct drm_device *dev, void *data,
		     struct drm_file *file)
{
	struct drm_i915_gem_pread *args;

	args = data;
	return (i915_gem_obj_io(dev, args->handle, args->data_ptr, args->size,
	    args->offset, UIO_READ, file));
}

/**
 * Writes data to the object referenced by handle.
 *
 * On error, the contents of the buffer that were to be modified are undefined.
 */
int
i915_gem_pwrite_ioctl(struct drm_device *dev, void *data,
		      struct drm_file *file)
{
	struct drm_i915_gem_pwrite *args;

	args = data;
	return (i915_gem_obj_io(dev, args->handle, args->data_ptr, args->size,
	    args->offset, UIO_WRITE, file));
}

/**
 * Waits for a sequence number to be signaled, and cleans up the
 * request and object lists appropriately for that event.
 */
int
i915_wait_seqno(struct intel_ring_buffer *ring, uint32_t seqno)
{
	drm_i915_private_t *dev_priv;
	struct drm_i915_gem_request *request;
	uint32_t ier;
	int flags, ret;
	bool recovery_complete;
	bool do_retire = true;

	KASSERT(seqno != 0, ("Zero seqno"));

	dev_priv = ring->dev->dev_private;
	ret = 0;

	if (atomic_read(&dev_priv->mm.wedged) != 0) {
		struct completion *x = &dev_priv->error_completion;
		spin_lock(&x->wait.lock);
		/* Give the error handler a chance to run. */
		recovery_complete = x->done > 0;
		spin_unlock(&x->wait.lock);
		return (recovery_complete ? -EIO : -EAGAIN);
	}

	if (seqno == ring->outstanding_lazy_request) {
		request = kmalloc(sizeof(*request), DRM_I915_GEM,
		    M_WAITOK | M_ZERO);
		if (request == NULL)
			return (-ENOMEM);

		ret = i915_add_request(ring, NULL, request);
		if (ret != 0) {
			drm_free(request, DRM_I915_GEM);
			return (ret);
		}

		seqno = request->seqno;
	}

	if (!i915_seqno_passed(ring->get_seqno(ring,false), seqno)) {
		if (HAS_PCH_SPLIT(ring->dev))
			ier = I915_READ(DEIER) | I915_READ(GTIER);
		else
			ier = I915_READ(IER);
		if (!ier) {
			DRM_ERROR("something (likely vbetool) disabled "
				  "interrupts, re-enabling\n");
			ring->dev->driver->irq_preinstall(ring->dev);
			ring->dev->driver->irq_postinstall(ring->dev);
		}

		lockmgr(&ring->irq_lock, LK_EXCLUSIVE);
		if (ring->irq_get(ring)) {
			flags = dev_priv->mm.interruptible ? PCATCH : 0;
			while (!i915_seqno_passed(ring->get_seqno(ring,false), seqno)
			    && !atomic_read(&dev_priv->mm.wedged) &&
			    ret == 0) {
				ret = -lksleep(ring, &ring->irq_lock, flags,
				    "915gwr", 1*hz);
			}
			ring->irq_put(ring);
			lockmgr(&ring->irq_lock, LK_RELEASE);
		} else {
			lockmgr(&ring->irq_lock, LK_RELEASE);
			if (_intel_wait_for(ring->dev,
			    i915_seqno_passed(ring->get_seqno(ring,false), seqno) ||
			    atomic_read(&dev_priv->mm.wedged), 3000,
			    0, "i915wrq") != 0)
				ret = -EBUSY;
		}

	}
	if (atomic_read(&dev_priv->mm.wedged))
		ret = -EAGAIN;

	/* Directly dispatch request retiring.  While we have the work queue
	 * to handle this, the waiter on a request often wants an associated
	 * buffer to have made it to the inactive list, and we would need
	 * a separate wait queue to handle that.
	 */
	if (ret == 0 && do_retire)
		i915_gem_retire_requests_ring(ring);

	return (ret);
}

/**
 * Ensures that all rendering to the object has completed and the object is
 * safe to unbind from the GTT or access from the CPU.
 */
int
i915_gem_object_wait_rendering(struct drm_i915_gem_object *obj)
{
	u32 seqno;
	int ret;


	seqno = obj->last_rendering_seqno;
	if (seqno == 0)
		return 0;

	if (obj->active) {
		ret = i915_wait_seqno(obj->ring, obj->last_rendering_seqno);
		if (ret != 0)
			return (ret);
	}

	/* Manually manage the write flush as we may have not yet
	 * retired the buffer.
	 */
	if (obj->last_rendering_seqno &&
	    i915_seqno_passed(seqno, obj->last_rendering_seqno)) {
		obj->last_rendering_seqno = 0;
		obj->base.write_domain &= ~I915_GEM_GPU_DOMAINS;
	}

	return 0;
}

/**
 * Called when user space prepares to use an object with the CPU, either
 * through the mmap ioctl's mapping or a GTT mapping.
 */
int
i915_gem_set_domain_ioctl(struct drm_device *dev, void *data,
			  struct drm_file *file)
{
	struct drm_i915_gem_set_domain *args;
	struct drm_i915_gem_object *obj;
	uint32_t read_domains;
	uint32_t write_domain;
	int ret;

	if ((dev->driver->driver_features & DRIVER_GEM) == 0)
		return (-ENODEV);

	args = data;
	read_domains = args->read_domains;
	write_domain = args->write_domain;

	if ((write_domain & I915_GEM_GPU_DOMAINS) != 0 ||
	    (read_domains & I915_GEM_GPU_DOMAINS) != 0 ||
	    (write_domain != 0 && read_domains != write_domain))
		return (-EINVAL);

	ret = i915_mutex_lock_interruptible(dev);
	if (ret != 0)
		return (ret);

	obj = to_intel_bo(drm_gem_object_lookup(dev, file, args->handle));
	if (&obj->base == NULL) {
		ret = -ENOENT;
		goto unlock;
	}

	if ((read_domains & I915_GEM_DOMAIN_GTT) != 0) {
		ret = i915_gem_object_set_to_gtt_domain(obj, write_domain != 0);
		if (ret == -EINVAL)
			ret = 0;
	} else
		ret = i915_gem_object_set_to_cpu_domain(obj, write_domain != 0);

	drm_gem_object_unreference(&obj->base);
unlock:
	DRM_UNLOCK(dev);
	return (ret);
}

/**
 * Called when user space has done writes to this buffer
 */
int
i915_gem_sw_finish_ioctl(struct drm_device *dev, void *data,
			 struct drm_file *file)
{
	struct drm_i915_gem_sw_finish *args;
	struct drm_i915_gem_object *obj;
	int ret;

	args = data;
	ret = 0;
	if ((dev->driver->driver_features & DRIVER_GEM) == 0)
		return (ENODEV);
	ret = i915_mutex_lock_interruptible(dev);
	if (ret != 0)
		return (ret);
	obj = to_intel_bo(drm_gem_object_lookup(dev, file, args->handle));
	if (&obj->base == NULL) {
		ret = -ENOENT;
		goto unlock;
	}
	if (obj->pin_count != 0)
		i915_gem_object_flush_cpu_write_domain(obj);
	drm_gem_object_unreference(&obj->base);
unlock:
	DRM_UNLOCK(dev);
	return (ret);
}

/**
 * Maps the contents of an object, returning the address it is mapped
 * into.
 *
 * While the mapping holds a reference on the contents of the object, it doesn't
 * imply a ref on the object itself.
 */
int
i915_gem_mmap_ioctl(struct drm_device *dev, void *data,
		    struct drm_file *file)
{
	struct drm_i915_gem_mmap *args;
	struct drm_gem_object *obj;
	struct proc *p;
	vm_map_t map;
	vm_offset_t addr;
	vm_size_t size;
	int error, rv;

	args = data;

	if ((dev->driver->driver_features & DRIVER_GEM) == 0)
		return (-ENODEV);

	obj = drm_gem_object_lookup(dev, file, args->handle);
	if (obj == NULL)
		return (-ENOENT);
	error = 0;
	if (args->size == 0)
		goto out;
	p = curproc;
	map = &p->p_vmspace->vm_map;
	size = round_page(args->size);
	PROC_LOCK(p);
	if (map->size + size > p->p_rlimit[RLIMIT_VMEM].rlim_cur) {
		PROC_UNLOCK(p);
		error = ENOMEM;
		goto out;
	}
	PROC_UNLOCK(p);

	addr = 0;
	vm_object_hold(obj->vm_obj);
	vm_object_reference_locked(obj->vm_obj);
	vm_object_drop(obj->vm_obj);
	DRM_UNLOCK(dev);
	rv = vm_map_find(map, obj->vm_obj, args->offset, &addr, args->size,
	    PAGE_SIZE, /* align */
	    TRUE, /* fitit */
	    VM_MAPTYPE_NORMAL, /* maptype */
	    VM_PROT_READ | VM_PROT_WRITE, /* prot */
	    VM_PROT_READ | VM_PROT_WRITE, /* max */
	    MAP_SHARED /* cow */);
	if (rv != KERN_SUCCESS) {
		vm_object_deallocate(obj->vm_obj);
		error = -vm_mmap_to_errno(rv);
	} else {
		args->addr_ptr = (uint64_t)addr;
	}
	DRM_LOCK(dev);
out:
	drm_gem_object_unreference(obj);
	return (error);
}

/**
 * i915_gem_release_mmap - remove physical page mappings
 * @obj: obj in question
 *
 * Preserve the reservation of the mmapping with the DRM core code, but
 * relinquish ownership of the pages back to the system.
 *
 * It is vital that we remove the page mapping if we have mapped a tiled
 * object through the GTT and then lose the fence register due to
 * resource pressure. Similarly if the object has been moved out of the
 * aperture, than pages mapped into userspace must be revoked. Removing the
 * mapping will then trigger a page fault on the next user access, allowing
 * fixup by i915_gem_fault().
 */
void
i915_gem_release_mmap(struct drm_i915_gem_object *obj)
{
	vm_object_t devobj;
	vm_page_t m;
	int i, page_count;

	if (!obj->fault_mappable)
		return;

	devobj = cdev_pager_lookup(obj);
	if (devobj != NULL) {
		page_count = OFF_TO_IDX(obj->base.size);

		VM_OBJECT_LOCK(devobj);
		for (i = 0; i < page_count; i++) {
			m = vm_page_lookup_busy_wait(devobj, i, TRUE, "915unm");
			if (m == NULL)
				continue;
			cdev_pager_free_page(devobj, m);
		}
		VM_OBJECT_UNLOCK(devobj);
		vm_object_deallocate(devobj);
	}

	obj->fault_mappable = false;
}

static uint32_t
i915_gem_get_gtt_size(struct drm_device *dev, uint32_t size, int tiling_mode)
{
	uint32_t gtt_size;

	if (INTEL_INFO(dev)->gen >= 4 ||
	    tiling_mode == I915_TILING_NONE)
		return (size);

	/* Previous chips need a power-of-two fence region when tiling */
	if (INTEL_INFO(dev)->gen == 3)
		gtt_size = 1024*1024;
	else
		gtt_size = 512*1024;

	while (gtt_size < size)
		gtt_size <<= 1;

	return (gtt_size);
}

/**
 * i915_gem_get_gtt_alignment - return required GTT alignment for an object
 * @obj: object to check
 *
 * Return the required GTT alignment for an object, taking into account
 * potential fence register mapping.
 */
static uint32_t
i915_gem_get_gtt_alignment(struct drm_device *dev,
			   uint32_t size,
			   int tiling_mode)
{

	/*
	 * Minimum alignment is 4k (GTT page size), but might be greater
	 * if a fence register is needed for the object.
	 */
	if (INTEL_INFO(dev)->gen >= 4 ||
	    tiling_mode == I915_TILING_NONE)
		return (4096);

	/*
	 * Previous chips need to be aligned to the size of the smallest
	 * fence register that can contain the object.
	 */
	return (i915_gem_get_gtt_size(dev, size, tiling_mode));
}

/**
 * i915_gem_get_unfenced_gtt_alignment - return required GTT alignment for an
 *					 unfenced object
 * @dev: the device
 * @size: size of the object
 * @tiling_mode: tiling mode of the object
 *
 * Return the required GTT alignment for an object, only taking into account
 * unfenced tiled surface requirements.
 */
uint32_t
i915_gem_get_unfenced_gtt_alignment(struct drm_device *dev,
				    uint32_t size,
				    int tiling_mode)
{

	if (tiling_mode == I915_TILING_NONE)
		return (4096);

	/*
	 * Minimum alignment is 4k (GTT page size) for sane hw.
	 */
	if (INTEL_INFO(dev)->gen >= 4 || IS_G33(dev))
		return (4096);

	/*
	 * Previous hardware however needs to be aligned to a power-of-two
	 * tile height. The simplest method for determining this is to reuse
	 * the power-of-tile object size.
         */
	return (i915_gem_get_gtt_size(dev, size, tiling_mode));
}

int
i915_gem_mmap_gtt(struct drm_file *file,
		  struct drm_device *dev,
		  uint32_t handle,
		  uint64_t *offset)
{
	struct drm_i915_private *dev_priv;
	struct drm_i915_gem_object *obj;
	int ret;

	if (!(dev->driver->driver_features & DRIVER_GEM))
		return (-ENODEV);

	dev_priv = dev->dev_private;

	ret = i915_mutex_lock_interruptible(dev);
	if (ret != 0)
		return (ret);

	obj = to_intel_bo(drm_gem_object_lookup(dev, file, handle));
	if (&obj->base == NULL) {
		ret = -ENOENT;
		goto unlock;
	}

	if (obj->base.size > dev_priv->mm.gtt_mappable_end) {
		ret = -E2BIG;
		goto out;
	}

	if (obj->madv != I915_MADV_WILLNEED) {
		DRM_ERROR("Attempting to mmap a purgeable buffer\n");
		ret = -EINVAL;
		goto out;
	}

	ret = drm_gem_create_mmap_offset(&obj->base);
	if (ret != 0)
		goto out;

	*offset = DRM_GEM_MAPPING_OFF(obj->base.map_list.key) |
	    DRM_GEM_MAPPING_KEY;
out:
	drm_gem_object_unreference(&obj->base);
unlock:
	DRM_UNLOCK(dev);
	return (ret);
}

/**
 * i915_gem_mmap_gtt_ioctl - prepare an object for GTT mmap'ing
 * @dev: DRM device
 * @data: GTT mapping ioctl data
 * @file: GEM object info
 *
 * Simply returns the fake offset to userspace so it can mmap it.
 * The mmap call will end up in drm_gem_mmap(), which will set things
 * up so we can get faults in the handler above.
 *
 * The fault handler will take care of binding the object into the GTT
 * (since it may have been evicted to make room for something), allocating
 * a fence register, and mapping the appropriate aperture address into
 * userspace.
 */
int
i915_gem_mmap_gtt_ioctl(struct drm_device *dev, void *data,
			struct drm_file *file)
{
	struct drm_i915_private *dev_priv;
	struct drm_i915_gem_mmap_gtt *args;

	dev_priv = dev->dev_private;
	args = data;

	return (i915_gem_mmap_gtt(file, dev, args->handle, &args->offset));
}

/* Immediately discard the backing storage */
static void
i915_gem_object_truncate(struct drm_i915_gem_object *obj)
{
	vm_object_t vm_obj;

	vm_obj = obj->base.vm_obj;
	VM_OBJECT_LOCK(vm_obj);
	vm_object_page_remove(vm_obj, 0, 0, false);
	VM_OBJECT_UNLOCK(vm_obj);
	obj->madv = __I915_MADV_PURGED;
}

static inline int
i915_gem_object_is_purgeable(struct drm_i915_gem_object *obj)
{
	return obj->madv == I915_MADV_DONTNEED;
}

static inline void vm_page_reference(vm_page_t m)
{
	vm_page_flag_set(m, PG_REFERENCED);
}

static void
i915_gem_object_put_pages_gtt(struct drm_i915_gem_object *obj)
{
	vm_page_t m;
	int page_count, i;

	BUG_ON(obj->madv == __I915_MADV_PURGED);

	if (obj->tiling_mode != I915_TILING_NONE)
		i915_gem_object_save_bit_17_swizzle(obj);
	if (obj->madv == I915_MADV_DONTNEED)
		obj->dirty = 0;
	page_count = obj->base.size / PAGE_SIZE;
	VM_OBJECT_LOCK(obj->base.vm_obj);
#if GEM_PARANOID_CHECK_GTT
	i915_gem_assert_pages_not_mapped(obj->base.dev, obj->pages, page_count);
#endif
	for (i = 0; i < page_count; i++) {
		m = obj->pages[i];
		if (obj->dirty)
			vm_page_dirty(m);
		if (obj->madv == I915_MADV_WILLNEED)
			vm_page_reference(m);
		vm_page_busy_wait(obj->pages[i], FALSE, "i915gem");
		vm_page_unwire(obj->pages[i], 1);
		vm_page_wakeup(obj->pages[i]);
		atomic_add_long(&i915_gem_wired_pages_cnt, -1);
	}
	VM_OBJECT_UNLOCK(obj->base.vm_obj);
	obj->dirty = 0;
	drm_free(obj->pages, DRM_I915_GEM);
	obj->pages = NULL;
}

static int
i915_gem_object_get_pages_gtt(struct drm_i915_gem_object *obj,
    int flags)
{
	struct drm_device *dev;
	vm_object_t vm_obj;
	vm_page_t m;
	int page_count, i, j;

	dev = obj->base.dev;
	KASSERT(obj->pages == NULL, ("Obj already has pages"));
	page_count = obj->base.size / PAGE_SIZE;
	obj->pages = kmalloc(page_count * sizeof(vm_page_t), DRM_I915_GEM,
	    M_WAITOK);
	vm_obj = obj->base.vm_obj;
	VM_OBJECT_LOCK(vm_obj);
	for (i = 0; i < page_count; i++) {
		if ((obj->pages[i] = i915_gem_wire_page(vm_obj, i)) == NULL)
			goto failed;
	}
	VM_OBJECT_UNLOCK(vm_obj);
	if (i915_gem_object_needs_bit17_swizzle(obj))
		i915_gem_object_do_bit_17_swizzle(obj);
	return (0);

failed:
	for (j = 0; j < i; j++) {
		m = obj->pages[j];
		vm_page_busy_wait(m, FALSE, "i915gem");
		vm_page_unwire(m, 0);
		vm_page_wakeup(m);
		atomic_add_long(&i915_gem_wired_pages_cnt, -1);
	}
	VM_OBJECT_UNLOCK(vm_obj);
	drm_free(obj->pages, DRM_I915_GEM);
	obj->pages = NULL;
	return (-EIO);
}

void
i915_gem_object_move_to_active(struct drm_i915_gem_object *obj,
    struct intel_ring_buffer *ring, uint32_t seqno)
{
	struct drm_device *dev = obj->base.dev;
	struct drm_i915_private *dev_priv = dev->dev_private;
	struct drm_i915_fence_reg *reg;

	obj->ring = ring;
	KASSERT(ring != NULL, ("NULL ring"));

	/* Add a reference if we're newly entering the active list. */
	if (!obj->active) {
		drm_gem_object_reference(&obj->base);
		obj->active = 1;
	}

	/* Move from whatever list we were on to the tail of execution. */
	list_move_tail(&obj->mm_list, &dev_priv->mm.active_list);
	list_move_tail(&obj->ring_list, &ring->active_list);

	obj->last_rendering_seqno = seqno;
	if (obj->fenced_gpu_access) {
		obj->last_fenced_seqno = seqno;
		obj->last_fenced_ring = ring;

		/* Bump MRU to take account of the delayed flush */
		if (obj->fence_reg != I915_FENCE_REG_NONE) {
			reg = &dev_priv->fence_regs[obj->fence_reg];
			list_move_tail(&reg->lru_list,
				       &dev_priv->mm.fence_list);
		}
	}
}

static void
i915_gem_object_move_off_active(struct drm_i915_gem_object *obj)
{
	list_del_init(&obj->ring_list);
	obj->last_rendering_seqno = 0;
	obj->last_fenced_seqno = 0;
}

static void
i915_gem_object_move_to_inactive(struct drm_i915_gem_object *obj)
{
	struct drm_device *dev = obj->base.dev;
	struct drm_i915_private *dev_priv = dev->dev_private;

	if (obj->pin_count != 0)
		list_move_tail(&obj->mm_list, &dev_priv->mm.pinned_list);
	else
		list_move_tail(&obj->mm_list, &dev_priv->mm.inactive_list);

	KASSERT(list_empty(&obj->gpu_write_list), ("On gpu_write_list"));
	KASSERT(obj->active, ("Object not active"));
	obj->ring = NULL;
	obj->last_fenced_ring = NULL;

	i915_gem_object_move_off_active(obj);
	obj->fenced_gpu_access = false;

	obj->active = 0;
	obj->pending_gpu_write = false;
	drm_gem_object_unreference(&obj->base);

#if 1
	KIB_NOTYET();
#else
	WARN_ON(i915_verify_lists(dev));
#endif
}

static u32
i915_gem_get_seqno(struct drm_device *dev)
{
	drm_i915_private_t *dev_priv = dev->dev_private;
	u32 seqno = dev_priv->next_seqno;

	/* reserve 0 for non-seqno */
	if (++dev_priv->next_seqno == 0)
		dev_priv->next_seqno = 1;

	return seqno;
}

int
i915_add_request(struct intel_ring_buffer *ring, struct drm_file *file,
     struct drm_i915_gem_request *request)
{
	drm_i915_private_t *dev_priv;
	struct drm_i915_file_private *file_priv;
	uint32_t seqno;
	u32 request_ring_position;
	int was_empty;
	int ret;

	KASSERT(request != NULL, ("NULL request in add"));
	DRM_LOCK_ASSERT(ring->dev);
	dev_priv = ring->dev->dev_private;

	seqno = i915_gem_next_request_seqno(ring);
	request_ring_position = intel_ring_get_tail(ring);

	ret = ring->add_request(ring, &seqno);
	if (ret != 0)
	    return ret;

	request->seqno = seqno;
	request->ring = ring;
	request->tail = request_ring_position;
	request->emitted_jiffies = ticks;
	was_empty = list_empty(&ring->request_list);
	list_add_tail(&request->list, &ring->request_list);

	if (file != NULL) {
		file_priv = file->driver_priv;

		spin_lock(&file_priv->mm.lock);
		request->file_priv = file_priv;
		list_add_tail(&request->client_list,
		    &file_priv->mm.request_list);
		spin_unlock(&file_priv->mm.lock);
	}

	ring->outstanding_lazy_request = 0;

	if (!dev_priv->mm.suspended) {
		if (i915_enable_hangcheck) {
			mod_timer(&dev_priv->hangcheck_timer,
				  round_jiffies_up(jiffies + DRM_I915_HANGCHECK_JIFFIES));
		}
		if (was_empty) {
			queue_delayed_work(dev_priv->wq,
					   &dev_priv->mm.retire_work,
					   round_jiffies_up_relative(hz));
			intel_mark_busy(dev_priv->dev);
		}
	}
	return (0);
}

static inline void
i915_gem_request_remove_from_client(struct drm_i915_gem_request *request)
{
	struct drm_i915_file_private *file_priv = request->file_priv;

	if (!file_priv)
		return;

	DRM_LOCK_ASSERT(request->ring->dev);

	spin_lock(&file_priv->mm.lock);
	if (request->file_priv != NULL) {
		list_del(&request->client_list);
		request->file_priv = NULL;
	}
	spin_unlock(&file_priv->mm.lock);
}

static void
i915_gem_reset_ring_lists(struct drm_i915_private *dev_priv,
    struct intel_ring_buffer *ring)
{

	if (ring->dev != NULL)
		DRM_LOCK_ASSERT(ring->dev);

	while (!list_empty(&ring->request_list)) {
		struct drm_i915_gem_request *request;

		request = list_first_entry(&ring->request_list,
		    struct drm_i915_gem_request, list);

		list_del(&request->list);
		i915_gem_request_remove_from_client(request);
		drm_free(request, DRM_I915_GEM);
	}

	while (!list_empty(&ring->active_list)) {
		struct drm_i915_gem_object *obj;

		obj = list_first_entry(&ring->active_list,
		    struct drm_i915_gem_object, ring_list);

		obj->base.write_domain = 0;
		list_del_init(&obj->gpu_write_list);
		i915_gem_object_move_to_inactive(obj);
	}
}

static void
i915_gem_reset_fences(struct drm_device *dev)
{
	struct drm_i915_private *dev_priv = dev->dev_private;
	int i;

	for (i = 0; i < dev_priv->num_fence_regs; i++) {
		struct drm_i915_fence_reg *reg = &dev_priv->fence_regs[i];
		struct drm_i915_gem_object *obj = reg->obj;

		if (!obj)
			continue;

		if (obj->tiling_mode)
			i915_gem_release_mmap(obj);

		reg->obj->fence_reg = I915_FENCE_REG_NONE;
		reg->obj->fenced_gpu_access = false;
		reg->obj->last_fenced_seqno = 0;
		reg->obj->last_fenced_ring = NULL;
		i915_gem_clear_fence_reg(dev, reg);
	}
}

void i915_gem_reset(struct drm_device *dev)
{
	struct drm_i915_private *dev_priv = dev->dev_private;
	struct drm_i915_gem_object *obj;
	int i;

	for (i = 0; i < I915_NUM_RINGS; i++)
		i915_gem_reset_ring_lists(dev_priv, &dev_priv->ring[i]);

	/* Remove anything from the flushing lists. The GPU cache is likely
	 * to be lost on reset along with the data, so simply move the
	 * lost bo to the inactive list.
	 */
	while (!list_empty(&dev_priv->mm.flushing_list)) {
		obj = list_first_entry(&dev_priv->mm.flushing_list,
				      struct drm_i915_gem_object,
				      mm_list);

		obj->base.write_domain = 0;
		list_del_init(&obj->gpu_write_list);
		i915_gem_object_move_to_inactive(obj);
	}

	/* Move everything out of the GPU domains to ensure we do any
	 * necessary invalidation upon reuse.
	 */
	list_for_each_entry(obj, &dev_priv->mm.inactive_list, mm_list) {
		obj->base.read_domains &= ~I915_GEM_GPU_DOMAINS;
	}

	/* The fence registers are invalidated so clear them out */
	i915_gem_reset_fences(dev);
}

static void
i915_gem_object_move_to_flushing(struct drm_i915_gem_object *obj)
{
	struct drm_device *dev = obj->base.dev;
	drm_i915_private_t *dev_priv = dev->dev_private;

	KASSERT(obj->active, ("Object not active"));
	list_move_tail(&obj->mm_list, &dev_priv->mm.flushing_list);

	i915_gem_object_move_off_active(obj);
}

/**
 * This function clears the request list as sequence numbers are passed.
 */
void
i915_gem_retire_requests_ring(struct intel_ring_buffer *ring)
{
	uint32_t seqno;

	if (list_empty(&ring->request_list))
		return;

	seqno = ring->get_seqno(ring, true);

	while (!list_empty(&ring->request_list)) {
		struct drm_i915_gem_request *request;

		request = list_first_entry(&ring->request_list,
					   struct drm_i915_gem_request,
					   list);

		if (!i915_seqno_passed(seqno, request->seqno))
			break;

		/* We know the GPU must have read the request to have
		 * sent us the seqno + interrupt, so use the position
		 * of tail of the request to update the last known position
		 * of the GPU head.
		 */
		ring->last_retired_head = request->tail;

		list_del(&request->list);
		i915_gem_request_remove_from_client(request);
		drm_free(request, DRM_I915_GEM);
	}

	/* Move any buffers on the active list that are no longer referenced
	 * by the ringbuffer to the flushing/inactive lists as appropriate.
	 */
	while (!list_empty(&ring->active_list)) {
		struct drm_i915_gem_object *obj;

		obj = list_first_entry(&ring->active_list,
				      struct drm_i915_gem_object,
				      ring_list);

		if (!i915_seqno_passed(seqno, obj->last_rendering_seqno))
			break;

		if (obj->base.write_domain != 0)
			i915_gem_object_move_to_flushing(obj);
		else
			i915_gem_object_move_to_inactive(obj);
	}

	if (unlikely(ring->trace_irq_seqno &&
		     i915_seqno_passed(seqno, ring->trace_irq_seqno))) {
		ring->irq_put(ring);
		ring->trace_irq_seqno = 0;
	}

}

static void
i915_gem_free_object_tail(struct drm_i915_gem_object *obj);

void
i915_gem_retire_requests(struct drm_device *dev)
{
	drm_i915_private_t *dev_priv = dev->dev_private;
	struct drm_i915_gem_object *obj, *next;
	int i;

	if (!list_empty(&dev_priv->mm.deferred_free_list)) {
		list_for_each_entry_safe(obj, next,
		    &dev_priv->mm.deferred_free_list, mm_list)
			i915_gem_free_object_tail(obj);
	}

	for (i = 0; i < I915_NUM_RINGS; i++)
		i915_gem_retire_requests_ring(&dev_priv->ring[i]);
}

static void
i915_gem_retire_work_handler(struct work_struct *work)
{
	drm_i915_private_t *dev_priv;
	struct drm_device *dev;
	struct intel_ring_buffer *ring;
	bool idle;
	int i;

	dev_priv = container_of(work, drm_i915_private_t,
				mm.retire_work.work);
	dev = dev_priv->dev;

	/* Come back later if the device is busy... */
	if (lockmgr(&dev->dev_struct_lock, LK_EXCLUSIVE|LK_NOWAIT)) {
		queue_delayed_work(dev_priv->wq, &dev_priv->mm.retire_work,
				   round_jiffies_up_relative(hz));
		return;
	}

	i915_gem_retire_requests(dev);

	/* Send a periodic flush down the ring so we don't hold onto GEM
	 * objects indefinitely.
	 */
	idle = true;
	for_each_ring(ring, dev_priv, i) {
		if (ring->gpu_caches_dirty)
			i915_add_request(ring, NULL, NULL);

		idle &= list_empty(&ring->request_list);
	}

	if (!dev_priv->mm.suspended && !idle)
		queue_delayed_work(dev_priv->wq, &dev_priv->mm.retire_work,
				   round_jiffies_up_relative(hz));
	if (idle)
		intel_mark_idle(dev);

	DRM_UNLOCK(dev);
}

static void i915_gem_object_finish_gtt(struct drm_i915_gem_object *obj)
{
	u32 old_write_domain, old_read_domains;

	/* Act a barrier for all accesses through the GTT */
	cpu_mfence();

	/* Force a pagefault for domain tracking on next user access */
	i915_gem_release_mmap(obj);

	if ((obj->base.read_domains & I915_GEM_DOMAIN_GTT) == 0)
		return;

	old_read_domains = obj->base.read_domains;
	old_write_domain = obj->base.write_domain;

	obj->base.read_domains &= ~I915_GEM_DOMAIN_GTT;
	obj->base.write_domain &= ~I915_GEM_DOMAIN_GTT;

}

int
i915_gem_object_unbind(struct drm_i915_gem_object *obj)
{
	drm_i915_private_t *dev_priv;
	int ret;

	dev_priv = obj->base.dev->dev_private;
	ret = 0;
	if (obj->gtt_space == NULL)
		return (0);
	if (obj->pin_count != 0) {
		DRM_ERROR("Attempting to unbind pinned buffer\n");
		return (-EINVAL);
	}

	ret = i915_gem_object_finish_gpu(obj);
	if (ret == -ERESTART || ret == -EINTR)
		return (ret);

	i915_gem_object_finish_gtt(obj);

	if (ret == 0)
		ret = i915_gem_object_set_to_cpu_domain(obj, 1);
	if (ret == -ERESTART || ret == -EINTR)
		return (ret);
	if (ret != 0) {
		i915_gem_clflush_object(obj);
		obj->base.read_domains = obj->base.write_domain =
		    I915_GEM_DOMAIN_CPU;
	}

	ret = i915_gem_object_put_fence(obj);
	if (ret == -ERESTART)
		return (ret);

	i915_gem_gtt_unbind_object(obj);
	if (obj->has_aliasing_ppgtt_mapping) {
		i915_ppgtt_unbind_object(dev_priv->mm.aliasing_ppgtt, obj);
		obj->has_aliasing_ppgtt_mapping = 0;
	}
	i915_gem_object_put_pages_gtt(obj);

	list_del_init(&obj->gtt_list);
	list_del_init(&obj->mm_list);
	obj->map_and_fenceable = true;

	drm_mm_put_block(obj->gtt_space);
	obj->gtt_space = NULL;
	obj->gtt_offset = 0;

	if (i915_gem_object_is_purgeable(obj))
		i915_gem_object_truncate(obj);

	return (ret);
}

int i915_gpu_idle(struct drm_device *dev)
{
	drm_i915_private_t *dev_priv = dev->dev_private;
	struct intel_ring_buffer *ring;
	int ret, i;

	/* Flush everything onto the inactive list. */
	for_each_ring(ring, dev_priv, i) {
		ret = intel_ring_idle(ring);
		if (ret)
			return ret;
	}

	return 0;
}

static int
sandybridge_write_fence_reg(struct drm_i915_gem_object *obj,
    struct intel_ring_buffer *pipelined)
{
	struct drm_device *dev = obj->base.dev;
	drm_i915_private_t *dev_priv = dev->dev_private;
	u32 size = obj->gtt_space->size;
	int regnum = obj->fence_reg;
	uint64_t val;

	val = (uint64_t)((obj->gtt_offset + size - 4096) &
			 0xfffff000) << 32;
	val |= obj->gtt_offset & 0xfffff000;
	val |= (uint64_t)((obj->stride / 128) - 1) <<
		SANDYBRIDGE_FENCE_PITCH_SHIFT;

	if (obj->tiling_mode == I915_TILING_Y)
		val |= 1 << I965_FENCE_TILING_Y_SHIFT;
	val |= I965_FENCE_REG_VALID;

	if (pipelined) {
		int ret = intel_ring_begin(pipelined, 6);
		if (ret)
			return ret;

		intel_ring_emit(pipelined, MI_NOOP);
		intel_ring_emit(pipelined, MI_LOAD_REGISTER_IMM(2));
		intel_ring_emit(pipelined, FENCE_REG_SANDYBRIDGE_0 + regnum*8);
		intel_ring_emit(pipelined, (u32)val);
		intel_ring_emit(pipelined, FENCE_REG_SANDYBRIDGE_0 + regnum*8 + 4);
		intel_ring_emit(pipelined, (u32)(val >> 32));
		intel_ring_advance(pipelined);
	} else
		I915_WRITE64(FENCE_REG_SANDYBRIDGE_0 + regnum * 8, val);

	return 0;
}

static int
i965_write_fence_reg(struct drm_i915_gem_object *obj,
    struct intel_ring_buffer *pipelined)
{
	struct drm_device *dev = obj->base.dev;
	drm_i915_private_t *dev_priv = dev->dev_private;
	u32 size = obj->gtt_space->size;
	int regnum = obj->fence_reg;
	uint64_t val;

	val = (uint64_t)((obj->gtt_offset + size - 4096) &
		    0xfffff000) << 32;
	val |= obj->gtt_offset & 0xfffff000;
	val |= ((obj->stride / 128) - 1) << I965_FENCE_PITCH_SHIFT;
	if (obj->tiling_mode == I915_TILING_Y)
		val |= 1 << I965_FENCE_TILING_Y_SHIFT;
	val |= I965_FENCE_REG_VALID;

	if (pipelined) {
		int ret = intel_ring_begin(pipelined, 6);
		if (ret)
			return ret;

		intel_ring_emit(pipelined, MI_NOOP);
		intel_ring_emit(pipelined, MI_LOAD_REGISTER_IMM(2));
		intel_ring_emit(pipelined, FENCE_REG_965_0 + regnum*8);
		intel_ring_emit(pipelined, (u32)val);
		intel_ring_emit(pipelined, FENCE_REG_965_0 + regnum*8 + 4);
		intel_ring_emit(pipelined, (u32)(val >> 32));
		intel_ring_advance(pipelined);
	} else
		I915_WRITE64(FENCE_REG_965_0 + regnum * 8, val);

	return 0;
}

static int
i915_write_fence_reg(struct drm_i915_gem_object *obj,
    struct intel_ring_buffer *pipelined)
{
	struct drm_device *dev = obj->base.dev;
	drm_i915_private_t *dev_priv = dev->dev_private;
	u32 size = obj->gtt_space->size;
	u32 fence_reg, val, pitch_val;
	int tile_width;

	if ((obj->gtt_offset & ~I915_FENCE_START_MASK) ||
	    (size & -size) != size || (obj->gtt_offset & (size - 1))) {
		kprintf(
"object 0x%08x [fenceable? %d] not 1M or pot-size (0x%08x) aligned\n",
		 obj->gtt_offset, obj->map_and_fenceable, size);
		return -EINVAL;
	}

	if (obj->tiling_mode == I915_TILING_Y && HAS_128_BYTE_Y_TILING(dev))
		tile_width = 128;
	else
		tile_width = 512;

	/* Note: pitch better be a power of two tile widths */
	pitch_val = obj->stride / tile_width;
	pitch_val = ffs(pitch_val) - 1;

	val = obj->gtt_offset;
	if (obj->tiling_mode == I915_TILING_Y)
		val |= 1 << I830_FENCE_TILING_Y_SHIFT;
	val |= I915_FENCE_SIZE_BITS(size);
	val |= pitch_val << I830_FENCE_PITCH_SHIFT;
	val |= I830_FENCE_REG_VALID;

	fence_reg = obj->fence_reg;
	if (fence_reg < 8)
		fence_reg = FENCE_REG_830_0 + fence_reg * 4;
	else
		fence_reg = FENCE_REG_945_8 + (fence_reg - 8) * 4;

	if (pipelined) {
		int ret = intel_ring_begin(pipelined, 4);
		if (ret)
			return ret;

		intel_ring_emit(pipelined, MI_NOOP);
		intel_ring_emit(pipelined, MI_LOAD_REGISTER_IMM(1));
		intel_ring_emit(pipelined, fence_reg);
		intel_ring_emit(pipelined, val);
		intel_ring_advance(pipelined);
	} else
		I915_WRITE(fence_reg, val);

	return 0;
}

static int
i830_write_fence_reg(struct drm_i915_gem_object *obj,
    struct intel_ring_buffer *pipelined)
{
	struct drm_device *dev = obj->base.dev;
	drm_i915_private_t *dev_priv = dev->dev_private;
	u32 size = obj->gtt_space->size;
	int regnum = obj->fence_reg;
	uint32_t val;
	uint32_t pitch_val;

	if ((obj->gtt_offset & ~I830_FENCE_START_MASK) ||
	    (size & -size) != size || (obj->gtt_offset & (size - 1))) {
		kprintf(
"object 0x%08x not 512K or pot-size 0x%08x aligned\n",
		    obj->gtt_offset, size);
		return -EINVAL;
	}

	pitch_val = obj->stride / 128;
	pitch_val = ffs(pitch_val) - 1;

	val = obj->gtt_offset;
	if (obj->tiling_mode == I915_TILING_Y)
		val |= 1 << I830_FENCE_TILING_Y_SHIFT;
	val |= I830_FENCE_SIZE_BITS(size);
	val |= pitch_val << I830_FENCE_PITCH_SHIFT;
	val |= I830_FENCE_REG_VALID;

	if (pipelined) {
		int ret = intel_ring_begin(pipelined, 4);
		if (ret)
			return ret;

		intel_ring_emit(pipelined, MI_NOOP);
		intel_ring_emit(pipelined, MI_LOAD_REGISTER_IMM(1));
		intel_ring_emit(pipelined, FENCE_REG_830_0 + regnum*4);
		intel_ring_emit(pipelined, val);
		intel_ring_advance(pipelined);
	} else
		I915_WRITE(FENCE_REG_830_0 + regnum * 4, val);

	return 0;
}

static bool ring_passed_seqno(struct intel_ring_buffer *ring, u32 seqno)
{
	return i915_seqno_passed(ring->get_seqno(ring,false), seqno);
}

static int
i915_gem_object_flush_fence(struct drm_i915_gem_object *obj,
    struct intel_ring_buffer *pipelined)
{
	int ret;

	if (obj->fenced_gpu_access) {
		if (obj->base.write_domain & I915_GEM_GPU_DOMAINS) {
			ret = i915_gem_flush_ring(obj->last_fenced_ring, 0,
			    obj->base.write_domain);
			if (ret)
				return ret;
		}

		obj->fenced_gpu_access = false;
	}

	if (obj->last_fenced_seqno && pipelined != obj->last_fenced_ring) {
		if (!ring_passed_seqno(obj->last_fenced_ring,
				       obj->last_fenced_seqno)) {
			ret = i915_wait_seqno(obj->last_fenced_ring,
						obj->last_fenced_seqno);
			if (ret)
				return ret;
		}

		obj->last_fenced_seqno = 0;
		obj->last_fenced_ring = NULL;
	}

	/* Ensure that all CPU reads are completed before installing a fence
	 * and all writes before removing the fence.
	 */
	if (obj->base.read_domains & I915_GEM_DOMAIN_GTT)
		cpu_mfence();

	return 0;
}

int
i915_gem_object_put_fence(struct drm_i915_gem_object *obj)
{
	int ret;

	if (obj->tiling_mode)
		i915_gem_release_mmap(obj);

	ret = i915_gem_object_flush_fence(obj, NULL);
	if (ret)
		return ret;

	if (obj->fence_reg != I915_FENCE_REG_NONE) {
		struct drm_i915_private *dev_priv = obj->base.dev->dev_private;

		if (dev_priv->fence_regs[obj->fence_reg].pin_count != 0)
			kprintf("%s: pin_count %d\n", __func__,
			    dev_priv->fence_regs[obj->fence_reg].pin_count);
		i915_gem_clear_fence_reg(obj->base.dev,
					 &dev_priv->fence_regs[obj->fence_reg]);

		obj->fence_reg = I915_FENCE_REG_NONE;
	}

	return 0;
}

static struct drm_i915_fence_reg *
i915_find_fence_reg(struct drm_device *dev, struct intel_ring_buffer *pipelined)
{
	struct drm_i915_private *dev_priv = dev->dev_private;
	struct drm_i915_fence_reg *reg, *first, *avail;
	int i;

	/* First try to find a free reg */
	avail = NULL;
	for (i = dev_priv->fence_reg_start; i < dev_priv->num_fence_regs; i++) {
		reg = &dev_priv->fence_regs[i];
		if (!reg->obj)
			return reg;

		if (!reg->pin_count)
			avail = reg;
	}

	if (avail == NULL)
		return NULL;

	/* None available, try to steal one or wait for a user to finish */
	avail = first = NULL;
	list_for_each_entry(reg, &dev_priv->mm.fence_list, lru_list) {
		if (reg->pin_count)
			continue;

		if (first == NULL)
			first = reg;

		if (!pipelined ||
		    !reg->obj->last_fenced_ring ||
		    reg->obj->last_fenced_ring == pipelined) {
			avail = reg;
			break;
		}
	}

	if (avail == NULL)
		avail = first;

	return avail;
}

int
i915_gem_object_get_fence(struct drm_i915_gem_object *obj,
    struct intel_ring_buffer *pipelined)
{
	struct drm_device *dev = obj->base.dev;
	struct drm_i915_private *dev_priv = dev->dev_private;
	struct drm_i915_fence_reg *reg;
	int ret;

	pipelined = NULL;
	ret = 0;

	if (obj->fence_reg != I915_FENCE_REG_NONE) {
		reg = &dev_priv->fence_regs[obj->fence_reg];
		list_move_tail(&reg->lru_list, &dev_priv->mm.fence_list);

		if (obj->tiling_changed) {
			ret = i915_gem_object_flush_fence(obj, pipelined);
			if (ret)
				return ret;

			if (!obj->fenced_gpu_access && !obj->last_fenced_seqno)
				pipelined = NULL;

			if (pipelined) {
				reg->setup_seqno =
					i915_gem_next_request_seqno(pipelined);
				obj->last_fenced_seqno = reg->setup_seqno;
				obj->last_fenced_ring = pipelined;
			}

			goto update;
		}

		if (!pipelined) {
			if (reg->setup_seqno) {
				if (!ring_passed_seqno(obj->last_fenced_ring,
				    reg->setup_seqno)) {
					ret = i915_wait_seqno(
					    obj->last_fenced_ring,
					    reg->setup_seqno);
					if (ret)
						return ret;
				}

				reg->setup_seqno = 0;
			}
		} else if (obj->last_fenced_ring &&
			   obj->last_fenced_ring != pipelined) {
			ret = i915_gem_object_flush_fence(obj, pipelined);
			if (ret)
				return ret;
		}

		if (!obj->fenced_gpu_access && !obj->last_fenced_seqno)
			pipelined = NULL;
		KASSERT(pipelined || reg->setup_seqno == 0, ("!pipelined"));

		if (obj->tiling_changed) {
			if (pipelined) {
				reg->setup_seqno =
					i915_gem_next_request_seqno(pipelined);
				obj->last_fenced_seqno = reg->setup_seqno;
				obj->last_fenced_ring = pipelined;
			}
			goto update;
		}

		return 0;
	}

	reg = i915_find_fence_reg(dev, pipelined);
	if (reg == NULL)
		return -EDEADLK;

	ret = i915_gem_object_flush_fence(obj, pipelined);
	if (ret)
		return ret;

	if (reg->obj) {
		struct drm_i915_gem_object *old = reg->obj;

		drm_gem_object_reference(&old->base);

		if (old->tiling_mode)
			i915_gem_release_mmap(old);

		ret = i915_gem_object_flush_fence(old, pipelined);
		if (ret) {
			drm_gem_object_unreference(&old->base);
			return ret;
		}

		if (old->last_fenced_seqno == 0 && obj->last_fenced_seqno == 0)
			pipelined = NULL;

		old->fence_reg = I915_FENCE_REG_NONE;
		old->last_fenced_ring = pipelined;
		old->last_fenced_seqno =
			pipelined ? i915_gem_next_request_seqno(pipelined) : 0;

		drm_gem_object_unreference(&old->base);
	} else if (obj->last_fenced_seqno == 0)
		pipelined = NULL;

	reg->obj = obj;
	list_move_tail(&reg->lru_list, &dev_priv->mm.fence_list);
	obj->fence_reg = reg - dev_priv->fence_regs;
	obj->last_fenced_ring = pipelined;

	reg->setup_seqno =
		pipelined ? i915_gem_next_request_seqno(pipelined) : 0;
	obj->last_fenced_seqno = reg->setup_seqno;

update:
	obj->tiling_changed = false;
	switch (INTEL_INFO(dev)->gen) {
	case 7:
	case 6:
		ret = sandybridge_write_fence_reg(obj, pipelined);
		break;
	case 5:
	case 4:
		ret = i965_write_fence_reg(obj, pipelined);
		break;
	case 3:
		ret = i915_write_fence_reg(obj, pipelined);
		break;
	case 2:
		ret = i830_write_fence_reg(obj, pipelined);
		break;
	}

	return ret;
}

static int
i915_gem_object_bind_to_gtt(struct drm_i915_gem_object *obj,
    unsigned alignment, bool map_and_fenceable)
{
	struct drm_device *dev;
	struct drm_i915_private *dev_priv;
	struct drm_mm_node *free_space;
	uint32_t size, fence_size, fence_alignment, unfenced_alignment;
	bool mappable, fenceable;
	int ret;

	dev = obj->base.dev;
	dev_priv = dev->dev_private;

	if (obj->madv != I915_MADV_WILLNEED) {
		DRM_ERROR("Attempting to bind a purgeable object\n");
		return (-EINVAL);
	}

	fence_size = i915_gem_get_gtt_size(dev, obj->base.size,
	    obj->tiling_mode);
	fence_alignment = i915_gem_get_gtt_alignment(dev, obj->base.size,
	    obj->tiling_mode);
	unfenced_alignment = i915_gem_get_unfenced_gtt_alignment(dev,
	    obj->base.size, obj->tiling_mode);
	if (alignment == 0)
		alignment = map_and_fenceable ? fence_alignment :
		    unfenced_alignment;
	if (map_and_fenceable && (alignment & (fence_alignment - 1)) != 0) {
		DRM_ERROR("Invalid object alignment requested %u\n", alignment);
		return (-EINVAL);
	}

	size = map_and_fenceable ? fence_size : obj->base.size;

	/* If the object is bigger than the entire aperture, reject it early
	 * before evicting everything in a vain attempt to find space.
	 */
	if (obj->base.size > (map_and_fenceable ?
	    dev_priv->mm.gtt_mappable_end : dev_priv->mm.gtt_total)) {
		DRM_ERROR(
"Attempting to bind an object larger than the aperture\n");
		return (-E2BIG);
	}

 search_free:
	if (map_and_fenceable)
		free_space = drm_mm_search_free_in_range(
		    &dev_priv->mm.gtt_space, size, alignment, 0,
		    dev_priv->mm.gtt_mappable_end, 0);
	else
		free_space = drm_mm_search_free(&dev_priv->mm.gtt_space,
		    size, alignment, 0);
	if (free_space != NULL) {
		int color = 0;
		if (map_and_fenceable)
			obj->gtt_space = drm_mm_get_block_range_generic(
			    free_space, size, alignment, color, 0,
			    dev_priv->mm.gtt_mappable_end, 1);
		else
			obj->gtt_space = drm_mm_get_block_generic(free_space,
			    size, alignment, color, 1);
	}
	if (obj->gtt_space == NULL) {
		ret = i915_gem_evict_something(dev, size, alignment,
		    map_and_fenceable);
		if (ret != 0)
			return (ret);
		goto search_free;
	}

	/*
	 * NOTE: i915_gem_object_get_pages_gtt() cannot
	 *	 return ENOMEM, since we used VM_ALLOC_RETRY.
	 */
	ret = i915_gem_object_get_pages_gtt(obj, 0);
	if (ret != 0) {
		drm_mm_put_block(obj->gtt_space);
		obj->gtt_space = NULL;
		return (ret);
	}

	i915_gem_gtt_bind_object(obj, obj->cache_level);
	if (ret != 0) {
		i915_gem_object_put_pages_gtt(obj);
		drm_mm_put_block(obj->gtt_space);
		obj->gtt_space = NULL;
		if (i915_gem_evict_everything(dev, false))
			return (ret);
		goto search_free;
	}

	list_add_tail(&obj->gtt_list, &dev_priv->mm.gtt_list);
	list_add_tail(&obj->mm_list, &dev_priv->mm.inactive_list);

	obj->gtt_offset = obj->gtt_space->start;

	fenceable =
		obj->gtt_space->size == fence_size &&
		(obj->gtt_space->start & (fence_alignment - 1)) == 0;

	mappable =
		obj->gtt_offset + obj->base.size <= dev_priv->mm.gtt_mappable_end;
	obj->map_and_fenceable = mappable && fenceable;

	return (0);
}

void
i915_gem_clflush_object(struct drm_i915_gem_object *obj)
{

	/* If we don't have a page list set up, then we're not pinned
	 * to GPU, and we can ignore the cache flush because it'll happen
	 * again at bind time.
	 */
	if (obj->pages == NULL)
		return;

	/* If the GPU is snooping the contents of the CPU cache,
	 * we do not need to manually clear the CPU cache lines.  However,
	 * the caches are only snooped when the render cache is
	 * flushed/invalidated.  As we always have to emit invalidations
	 * and flushes when moving into and out of the RENDER domain, correct
	 * snooping behaviour occurs naturally as the result of our domain
	 * tracking.
	 */
	if (obj->cache_level != I915_CACHE_NONE)
		return;

	drm_clflush_pages(obj->pages, obj->base.size / PAGE_SIZE);
}

/** Flushes the GTT write domain for the object if it's dirty. */
static void
i915_gem_object_flush_gtt_write_domain(struct drm_i915_gem_object *obj)
{
	uint32_t old_write_domain;

	if (obj->base.write_domain != I915_GEM_DOMAIN_GTT)
		return;

	/* No actual flushing is required for the GTT write domain.  Writes
	 * to it immediately go to main memory as far as we know, so there's
	 * no chipset flush.  It also doesn't land in render cache.
	 *
	 * However, we do have to enforce the order so that all writes through
	 * the GTT land before any writes to the device, such as updates to
	 * the GATT itself.
	 */
	cpu_sfence();

	old_write_domain = obj->base.write_domain;
	obj->base.write_domain = 0;
}

/** Flushes the CPU write domain for the object if it's dirty. */
static void
i915_gem_object_flush_cpu_write_domain(struct drm_i915_gem_object *obj)
{
	uint32_t old_write_domain;

	if (obj->base.write_domain != I915_GEM_DOMAIN_CPU)
		return;

	i915_gem_clflush_object(obj);
	intel_gtt_chipset_flush();
	old_write_domain = obj->base.write_domain;
	obj->base.write_domain = 0;
}

static int
i915_gem_object_flush_gpu_write_domain(struct drm_i915_gem_object *obj)
{

	if ((obj->base.write_domain & I915_GEM_GPU_DOMAINS) == 0)
		return (0);
	return (i915_gem_flush_ring(obj->ring, 0, obj->base.write_domain));
}

/**
 * Moves a single object to the GTT read, and possibly write domain.
 *
 * This function returns when the move is complete, including waiting on
 * flushes to occur.
 */
int
i915_gem_object_set_to_gtt_domain(struct drm_i915_gem_object *obj, bool write)
{
	uint32_t old_write_domain, old_read_domains;
	int ret;

	if (obj->gtt_space == NULL)
		return (-EINVAL);

	if (obj->base.write_domain == I915_GEM_DOMAIN_GTT)
		return 0;

	ret = i915_gem_object_flush_gpu_write_domain(obj);
	if (ret != 0)
		return (ret);

	if (obj->pending_gpu_write || write) {
		ret = i915_gem_object_wait_rendering(obj);
		if (ret != 0)
			return (ret);
	}

	i915_gem_object_flush_cpu_write_domain(obj);

	old_write_domain = obj->base.write_domain;
	old_read_domains = obj->base.read_domains;

	KASSERT((obj->base.write_domain & ~I915_GEM_DOMAIN_GTT) == 0,
	    ("In GTT write domain"));
	obj->base.read_domains |= I915_GEM_DOMAIN_GTT;
	if (write) {
		obj->base.read_domains = I915_GEM_DOMAIN_GTT;
		obj->base.write_domain = I915_GEM_DOMAIN_GTT;
		obj->dirty = 1;
	}

	return (0);
}

int i915_gem_object_set_cache_level(struct drm_i915_gem_object *obj,
				    enum i915_cache_level cache_level)
{
	struct drm_device *dev = obj->base.dev;
	drm_i915_private_t *dev_priv = dev->dev_private;
	int ret;

	if (obj->cache_level == cache_level)
		return 0;

	if (obj->pin_count) {
		DRM_DEBUG("can not change the cache level of pinned objects\n");
		return -EBUSY;
	}

	if (obj->gtt_space) {
		ret = i915_gem_object_finish_gpu(obj);
		if (ret != 0)
			return (ret);

		i915_gem_object_finish_gtt(obj);

		/* Before SandyBridge, you could not use tiling or fence
		 * registers with snooped memory, so relinquish any fences
		 * currently pointing to our region in the aperture.
		 */
		if (INTEL_INFO(obj->base.dev)->gen < 6) {
			ret = i915_gem_object_put_fence(obj);
			if (ret)
				return ret;
		}

		if (obj->has_global_gtt_mapping)
			i915_gem_gtt_bind_object(obj, cache_level);
		if (obj->has_aliasing_ppgtt_mapping)
			i915_ppgtt_bind_object(dev_priv->mm.aliasing_ppgtt,
					       obj, cache_level);
	}

	if (cache_level == I915_CACHE_NONE) {
		u32 old_read_domains, old_write_domain;

		/* If we're coming from LLC cached, then we haven't
		 * actually been tracking whether the data is in the
		 * CPU cache or not, since we only allow one bit set
		 * in obj->write_domain and have been skipping the clflushes.
		 * Just set it to the CPU cache for now.
		 */
		KASSERT((obj->base.write_domain & ~I915_GEM_DOMAIN_CPU) == 0,
		    ("obj %p in CPU write domain", obj));
		KASSERT((obj->base.read_domains & ~I915_GEM_DOMAIN_CPU) == 0,
		    ("obj %p in CPU read domain", obj));

		old_read_domains = obj->base.read_domains;
		old_write_domain = obj->base.write_domain;

		obj->base.read_domains = I915_GEM_DOMAIN_CPU;
		obj->base.write_domain = I915_GEM_DOMAIN_CPU;

	}

	obj->cache_level = cache_level;
	return 0;
}

/*
 * Prepare buffer for display plane (scanout, cursors, etc).
 * Can be called from an uninterruptible phase (modesetting) and allows
 * any flushes to be pipelined (for pageflips).
 */
int
i915_gem_object_pin_to_display_plane(struct drm_i915_gem_object *obj,
				     u32 alignment,
				     struct intel_ring_buffer *pipelined)
{
	u32 old_read_domains, old_write_domain;
	int ret;

	ret = i915_gem_object_flush_gpu_write_domain(obj);
	if (ret != 0)
		return (ret);

	if (pipelined != obj->ring) {
		ret = i915_gem_object_wait_rendering(obj);
		if (ret == -ERESTART || ret == -EINTR)
			return (ret);
	}

	ret = i915_gem_object_set_cache_level(obj, I915_CACHE_NONE);
	if (ret != 0)
		return (ret);

	ret = i915_gem_object_pin(obj, alignment, true);
	if (ret != 0)
		return (ret);

	i915_gem_object_flush_cpu_write_domain(obj);

	old_write_domain = obj->base.write_domain;
	old_read_domains = obj->base.read_domains;

	KASSERT((obj->base.write_domain & ~I915_GEM_DOMAIN_GTT) == 0,
	    ("obj %p in GTT write domain", obj));
	obj->base.read_domains |= I915_GEM_DOMAIN_GTT;

	return (0);
}

int
i915_gem_object_finish_gpu(struct drm_i915_gem_object *obj)
{
	int ret;

	if ((obj->base.read_domains & I915_GEM_GPU_DOMAINS) == 0)
		return (0);

	if (obj->base.write_domain & I915_GEM_GPU_DOMAINS) {
		ret = i915_gem_flush_ring(obj->ring, 0, obj->base.write_domain);
		if (ret != 0)
			return (ret);
	}

	ret = i915_gem_object_wait_rendering(obj);
	if (ret != 0)
		return (ret);

	obj->base.read_domains &= ~I915_GEM_GPU_DOMAINS;

	return (0);
}

/**
 * Moves a single object to the CPU read, and possibly write domain.
 *
 * This function returns when the move is complete, including waiting on
 * flushes to occur.
 */
int
i915_gem_object_set_to_cpu_domain(struct drm_i915_gem_object *obj, bool write)
{
	uint32_t old_write_domain, old_read_domains;
	int ret;

	if (obj->base.write_domain == I915_GEM_DOMAIN_CPU)
		return 0;

	ret = i915_gem_object_flush_gpu_write_domain(obj);
	if (ret != 0)
		return (ret);

	ret = i915_gem_object_wait_rendering(obj);
	if (ret)
		return ret;

	i915_gem_object_flush_gtt_write_domain(obj);

	old_write_domain = obj->base.write_domain;
	old_read_domains = obj->base.read_domains;

	/* Flush the CPU cache if it's still invalid. */
	if ((obj->base.read_domains & I915_GEM_DOMAIN_CPU) == 0) {
		i915_gem_clflush_object(obj);

		obj->base.read_domains |= I915_GEM_DOMAIN_CPU;
	}

	/* It should now be out of any other write domains, and we can update
	 * the domain values for our changes.
	 */
	BUG_ON((obj->base.write_domain & ~I915_GEM_DOMAIN_CPU) != 0);

	/* If we're writing through the CPU, then the GPU read domains will
	 * need to be invalidated at next use.
	 */
	if (write) {
		obj->base.read_domains = I915_GEM_DOMAIN_CPU;
		obj->base.write_domain = I915_GEM_DOMAIN_CPU;
	}

	return 0;
}

/* Throttle our rendering by waiting until the ring has completed our requests
 * emitted over 20 msec ago.
 *
 * Note that if we were to use the current jiffies each time around the loop,
 * we wouldn't escape the function with any frames outstanding if the time to
 * render a frame was over 20ms.
 *
 * This should get us reasonable parallelism between CPU and GPU but also
 * relatively low latency when blocking on a particular request to finish.
 */
static int
i915_gem_ring_throttle(struct drm_device *dev, struct drm_file *file)
{
	struct drm_i915_private *dev_priv = dev->dev_private;
	struct drm_i915_file_private *file_priv = file->driver_priv;
	unsigned long recent_enough = ticks - (20 * hz / 1000);
	struct drm_i915_gem_request *request;
	struct intel_ring_buffer *ring = NULL;
	u32 seqno = 0;
	int ret;

	dev_priv = dev->dev_private;
	if (atomic_read(&dev_priv->mm.wedged))
		return -EIO;

	recent_enough = ticks - (20 * hz / 1000);
	ring = NULL;
	seqno = 0;

	spin_lock(&file_priv->mm.lock);
	list_for_each_entry(request, &file_priv->mm.request_list, client_list) {
		if (time_after_eq(request->emitted_jiffies, recent_enough))
			break;

		ring = request->ring;
		seqno = request->seqno;
	}
	spin_unlock(&file_priv->mm.lock);

	if (seqno == 0)
		return 0;

	ret = 0;
	lockmgr(&ring->irq_lock, LK_EXCLUSIVE);
	if (!i915_seqno_passed(ring->get_seqno(ring,false), seqno)) {
		if (ring->irq_get(ring)) {
			while (ret == 0 &&
			    !(i915_seqno_passed(ring->get_seqno(ring,false), seqno) ||
			    atomic_read(&dev_priv->mm.wedged)))
				ret = -lksleep(ring, &ring->irq_lock, PCATCH,
				    "915thr", 1*hz);
			ring->irq_put(ring);
			if (ret == 0 && atomic_read(&dev_priv->mm.wedged))
				ret = -EIO;
		} else if (_intel_wait_for(dev,
		    i915_seqno_passed(ring->get_seqno(ring,false), seqno) ||
		    atomic_read(&dev_priv->mm.wedged), 3000, 0, "915rtr")) {
			ret = -EBUSY;
		}
	}
	lockmgr(&ring->irq_lock, LK_RELEASE);

	if (ret == 0)
		queue_delayed_work(dev_priv->wq, &dev_priv->mm.retire_work, 0);

	return ret;
}

int
i915_gem_object_pin(struct drm_i915_gem_object *obj, uint32_t alignment,
     bool map_and_fenceable)
{
	struct drm_device *dev;
	struct drm_i915_private *dev_priv;
	int ret;

	dev = obj->base.dev;
	dev_priv = dev->dev_private;

	KASSERT(obj->pin_count != DRM_I915_GEM_OBJECT_MAX_PIN_COUNT,
	    ("Max pin count"));

	if (obj->gtt_space != NULL) {
		if ((alignment && obj->gtt_offset & (alignment - 1)) ||
		    (map_and_fenceable && !obj->map_and_fenceable)) {
			DRM_DEBUG("bo is already pinned with incorrect alignment:"
			     " offset=%x, req.alignment=%x, req.map_and_fenceable=%d,"
			     " obj->map_and_fenceable=%d\n",
			     obj->gtt_offset, alignment,
			     map_and_fenceable,
			     obj->map_and_fenceable);
			ret = i915_gem_object_unbind(obj);
			if (ret != 0)
				return (ret);
		}
	}

	if (obj->gtt_space == NULL) {
		ret = i915_gem_object_bind_to_gtt(obj, alignment,
		    map_and_fenceable);
		if (ret)
			return (ret);
	}

	if (obj->pin_count++ == 0 && !obj->active)
		list_move_tail(&obj->mm_list, &dev_priv->mm.pinned_list);
	obj->pin_mappable |= map_and_fenceable;

#if 1
	KIB_NOTYET();
#else
	WARN_ON(i915_verify_lists(dev));
#endif
	return (0);
}

void
i915_gem_object_unpin(struct drm_i915_gem_object *obj)
{
	struct drm_device *dev;
	drm_i915_private_t *dev_priv;

	dev = obj->base.dev;
	dev_priv = dev->dev_private;

#if 1
	KIB_NOTYET();
#else
	WARN_ON(i915_verify_lists(dev));
#endif
	
	KASSERT(obj->pin_count != 0, ("zero pin count"));
	KASSERT(obj->gtt_space != NULL, ("No gtt mapping"));

	if (--obj->pin_count == 0) {
		if (!obj->active)
			list_move_tail(&obj->mm_list,
			    &dev_priv->mm.inactive_list);
		obj->pin_mappable = false;
	}
#if 1
	KIB_NOTYET();
#else
	WARN_ON(i915_verify_lists(dev));
#endif
}

int
i915_gem_pin_ioctl(struct drm_device *dev, void *data,
    struct drm_file *file)
{
	struct drm_i915_gem_pin *args;
	struct drm_i915_gem_object *obj;
	struct drm_gem_object *gobj;
	int ret;

	args = data;

	ret = i915_mutex_lock_interruptible(dev);
	if (ret != 0)
		return ret;

	gobj = drm_gem_object_lookup(dev, file, args->handle);
	if (gobj == NULL) {
		ret = -ENOENT;
		goto unlock;
	}
	obj = to_intel_bo(gobj);

	if (obj->madv != I915_MADV_WILLNEED) {
		DRM_ERROR("Attempting to pin a purgeable buffer\n");
		ret = -EINVAL;
		goto out;
	}

	if (obj->pin_filp != NULL && obj->pin_filp != file) {
		DRM_ERROR("Already pinned in i915_gem_pin_ioctl(): %d\n",
		    args->handle);
		ret = -EINVAL;
		goto out;
	}

	obj->user_pin_count++;
	obj->pin_filp = file;
	if (obj->user_pin_count == 1) {
		ret = i915_gem_object_pin(obj, args->alignment, true);
		if (ret != 0)
			goto out;
	}

	/* XXX - flush the CPU caches for pinned objects
	 * as the X server doesn't manage domains yet
	 */
	i915_gem_object_flush_cpu_write_domain(obj);
	args->offset = obj->gtt_offset;
out:
	drm_gem_object_unreference(&obj->base);
unlock:
	DRM_UNLOCK(dev);
	return (ret);
}

int
i915_gem_unpin_ioctl(struct drm_device *dev, void *data,
    struct drm_file *file)
{
	struct drm_i915_gem_pin *args;
	struct drm_i915_gem_object *obj;
	int ret;

	args = data;
	ret = i915_mutex_lock_interruptible(dev);
	if (ret != 0)
		return (ret);

	obj = to_intel_bo(drm_gem_object_lookup(dev, file, args->handle));
	if (&obj->base == NULL) {
		ret = -ENOENT;
		goto unlock;
	}

	if (obj->pin_filp != file) {
		DRM_ERROR("Not pinned by caller in i915_gem_pin_ioctl(): %d\n",
		    args->handle);
		ret = -EINVAL;
		goto out;
	}
	obj->user_pin_count--;
	if (obj->user_pin_count == 0) {
		obj->pin_filp = NULL;
		i915_gem_object_unpin(obj);
	}

out:
	drm_gem_object_unreference(&obj->base);
unlock:
	DRM_UNLOCK(dev);
	return (ret);
}

int
i915_gem_busy_ioctl(struct drm_device *dev, void *data,
		    struct drm_file *file)
{
	struct drm_i915_gem_busy *args;
	struct drm_i915_gem_object *obj;
	struct drm_i915_gem_request *request;
	int ret;

	args = data;

	ret = i915_mutex_lock_interruptible(dev);
	if (ret != 0)
		return ret;

	obj = to_intel_bo(drm_gem_object_lookup(dev, file, args->handle));
	if (&obj->base == NULL) {
		ret = -ENOENT;
		goto unlock;
	}

	args->busy = obj->active;
	if (args->busy) {
		if (obj->base.write_domain & I915_GEM_GPU_DOMAINS) {
			ret = i915_gem_flush_ring(obj->ring,
			    0, obj->base.write_domain);
		} else if (obj->ring->outstanding_lazy_request ==
		    obj->last_rendering_seqno) {
			request = kmalloc(sizeof(*request), DRM_I915_GEM,
			    M_WAITOK | M_ZERO);
			ret = i915_add_request(obj->ring, NULL, request);
			if (ret != 0)
				drm_free(request, DRM_I915_GEM);
		}

		i915_gem_retire_requests_ring(obj->ring);
		args->busy = obj->active;
	}

	drm_gem_object_unreference(&obj->base);
unlock:
	DRM_UNLOCK(dev);
	return (ret);
}

int
i915_gem_throttle_ioctl(struct drm_device *dev, void *data,
    struct drm_file *file_priv)
{

	return (i915_gem_ring_throttle(dev, file_priv));
}

int
i915_gem_madvise_ioctl(struct drm_device *dev, void *data,
		       struct drm_file *file_priv)
{
	struct drm_i915_gem_madvise *args = data;
	struct drm_i915_gem_object *obj;
	int ret;

	switch (args->madv) {
	case I915_MADV_DONTNEED:
	case I915_MADV_WILLNEED:
	    break;
	default:
	    return -EINVAL;
	}

	ret = i915_mutex_lock_interruptible(dev);
	if (ret)
		return ret;

	obj = to_intel_bo(drm_gem_object_lookup(dev, file_priv, args->handle));
	if (&obj->base == NULL) {
		ret = -ENOENT;
		goto unlock;
	}

	if (obj->pin_count) {
		ret = -EINVAL;
		goto out;
	}

	if (obj->madv != __I915_MADV_PURGED)
		obj->madv = args->madv;

	/* if the object is no longer attached, discard its backing storage */
	if (i915_gem_object_is_purgeable(obj) && obj->pages == NULL)
		i915_gem_object_truncate(obj);

	args->retained = obj->madv != __I915_MADV_PURGED;

out:
	drm_gem_object_unreference(&obj->base);
unlock:
	DRM_UNLOCK(dev);
	return ret;
}

struct drm_i915_gem_object *i915_gem_alloc_object(struct drm_device *dev,
						  size_t size)
{
	struct drm_i915_private *dev_priv;
	struct drm_i915_gem_object *obj;

	dev_priv = dev->dev_private;

	obj = kmalloc(sizeof(*obj), DRM_I915_GEM, M_WAITOK | M_ZERO);

	if (drm_gem_object_init(dev, &obj->base, size) != 0) {
		drm_free(obj, DRM_I915_GEM);
		return (NULL);
	}

	obj->base.write_domain = I915_GEM_DOMAIN_CPU;
	obj->base.read_domains = I915_GEM_DOMAIN_CPU;

	if (HAS_LLC(dev))
		obj->cache_level = I915_CACHE_LLC;
	else
		obj->cache_level = I915_CACHE_NONE;
	obj->base.driver_private = NULL;
	obj->fence_reg = I915_FENCE_REG_NONE;
	INIT_LIST_HEAD(&obj->mm_list);
	INIT_LIST_HEAD(&obj->gtt_list);
	INIT_LIST_HEAD(&obj->ring_list);
	INIT_LIST_HEAD(&obj->exec_list);
	INIT_LIST_HEAD(&obj->gpu_write_list);
	obj->madv = I915_MADV_WILLNEED;
	/* Avoid an unnecessary call to unbind on the first bind. */
	obj->map_and_fenceable = true;

	i915_gem_info_add_obj(dev_priv, size);

	return (obj);
}

int i915_gem_init_object(struct drm_gem_object *obj)
{

	kprintf("i915_gem_init_object called\n");
	return (0);
}

static void
i915_gem_free_object_tail(struct drm_i915_gem_object *obj)
{
	struct drm_device *dev;
	drm_i915_private_t *dev_priv;
	int ret;

	dev = obj->base.dev;
	dev_priv = dev->dev_private;

	ret = i915_gem_object_unbind(obj);
	if (ret == -ERESTART) {
		list_move(&obj->mm_list, &dev_priv->mm.deferred_free_list);
		return;
	}

	drm_gem_free_mmap_offset(&obj->base);
	drm_gem_object_release(&obj->base);
	i915_gem_info_remove_obj(dev_priv, obj->base.size);

	drm_free(obj->page_cpu_valid, DRM_I915_GEM);
	drm_free(obj->bit_17, DRM_I915_GEM);
	drm_free(obj, DRM_I915_GEM);
}

void
i915_gem_free_object(struct drm_gem_object *gem_obj)
{
	struct drm_i915_gem_object *obj;
	struct drm_device *dev;

	obj = to_intel_bo(gem_obj);
	dev = obj->base.dev;

	while (obj->pin_count > 0)
		i915_gem_object_unpin(obj);

	if (obj->phys_obj != NULL)
		i915_gem_detach_phys_object(dev, obj);

	i915_gem_free_object_tail(obj);
}

int
i915_gem_do_init(struct drm_device *dev, unsigned long start,
    unsigned long mappable_end, unsigned long end)
{
	drm_i915_private_t *dev_priv;
	unsigned long mappable;
	int error;

	dev_priv = dev->dev_private;
	mappable = min(end, mappable_end) - start;

	drm_mm_init(&dev_priv->mm.gtt_space, start, end - start);

	dev_priv->mm.gtt_start = start;
	dev_priv->mm.gtt_mappable_end = mappable_end;
	dev_priv->mm.gtt_end = end;
	dev_priv->mm.gtt_total = end - start;
	dev_priv->mm.mappable_gtt_total = mappable;

	/* Take over this portion of the GTT */
	intel_gtt_clear_range(start / PAGE_SIZE, (end-start) / PAGE_SIZE);
	device_printf(dev->dev,
	    "taking over the fictitious range 0x%lx-0x%lx\n",
	    dev->agp->base + start, dev->agp->base + start + mappable);
	error = -vm_phys_fictitious_reg_range(dev->agp->base + start,
	    dev->agp->base + start + mappable, VM_MEMATTR_WRITE_COMBINING);
	return (error);
}

int
i915_gem_idle(struct drm_device *dev)
{
	drm_i915_private_t *dev_priv;
	int ret;

	dev_priv = dev->dev_private;
	if (dev_priv->mm.suspended)
		return (0);

	ret = i915_gpu_idle(dev);
	if (ret != 0)
		return (ret);

	/* Under UMS, be paranoid and evict. */
	if (!drm_core_check_feature(dev, DRIVER_MODESET)) {
		ret = i915_gem_evict_inactive(dev, false);
		if (ret != 0)
			return ret;
	}

	i915_gem_reset_fences(dev);

	/* Hack!  Don't let anybody do execbuf while we don't control the chip.
	 * We need to replace this with a semaphore, or something.
	 * And not confound mm.suspended!
	 */
	dev_priv->mm.suspended = 1;
	del_timer_sync(&dev_priv->hangcheck_timer);

	i915_kernel_lost_context(dev);
	i915_gem_cleanup_ringbuffer(dev);

	/* Cancel the retire work handler, which should be idle now. */
	cancel_delayed_work_sync(&dev_priv->mm.retire_work);

	return (ret);
}

void
i915_gem_init_swizzling(struct drm_device *dev)
{
	drm_i915_private_t *dev_priv;

	dev_priv = dev->dev_private;

	if (INTEL_INFO(dev)->gen < 5 ||
	    dev_priv->mm.bit_6_swizzle_x == I915_BIT_6_SWIZZLE_NONE)
		return;

	I915_WRITE(DISP_ARB_CTL, I915_READ(DISP_ARB_CTL) |
				 DISP_TILE_SURFACE_SWIZZLING);

	if (IS_GEN5(dev))
		return;

	I915_WRITE(TILECTL, I915_READ(TILECTL) | TILECTL_SWZCTL);
	if (IS_GEN6(dev))
		I915_WRITE(ARB_MODE, _MASKED_BIT_ENABLE(ARB_MODE_SWIZZLE_SNB));
	else
		I915_WRITE(ARB_MODE, _MASKED_BIT_ENABLE(ARB_MODE_SWIZZLE_IVB));
}

int
i915_gem_init_hw(struct drm_device *dev)
{
	drm_i915_private_t *dev_priv;
	int ret;

	dev_priv = dev->dev_private;

	i915_gem_init_swizzling(dev);

	ret = intel_init_render_ring_buffer(dev);
	if (ret != 0)
		return (ret);

	if (HAS_BSD(dev)) {
		ret = intel_init_bsd_ring_buffer(dev);
		if (ret != 0)
			goto cleanup_render_ring;
	}

	if (HAS_BLT(dev)) {
		ret = intel_init_blt_ring_buffer(dev);
		if (ret != 0)
			goto cleanup_bsd_ring;
	}

	dev_priv->next_seqno = 1;
	i915_gem_init_ppgtt(dev);
	return (0);

cleanup_bsd_ring:
	intel_cleanup_ring_buffer(&dev_priv->ring[VCS]);
cleanup_render_ring:
	intel_cleanup_ring_buffer(&dev_priv->ring[RCS]);
	return (ret);
}

void
i915_gem_cleanup_ringbuffer(struct drm_device *dev)
{
	drm_i915_private_t *dev_priv;
	int i;

	dev_priv = dev->dev_private;
	for (i = 0; i < I915_NUM_RINGS; i++)
		intel_cleanup_ring_buffer(&dev_priv->ring[i]);
}

int
i915_gem_entervt_ioctl(struct drm_device *dev, void *data,
		       struct drm_file *file_priv)
{
	drm_i915_private_t *dev_priv = dev->dev_private;
	int ret;

	if (drm_core_check_feature(dev, DRIVER_MODESET))
		return 0;

	if (atomic_read(&dev_priv->mm.wedged)) {
		DRM_ERROR("Reenabling wedged hardware, good luck\n");
		atomic_set(&dev_priv->mm.wedged, 0);
	}

	DRM_LOCK(dev);
	dev_priv->mm.suspended = 0;

	ret = i915_gem_init_hw(dev);
	if (ret != 0) {
		DRM_UNLOCK(dev);
		return ret;
	}

	KASSERT(list_empty(&dev_priv->mm.active_list), ("active list"));
	DRM_UNLOCK(dev);

	ret = drm_irq_install(dev);
	if (ret)
		goto cleanup_ringbuffer;

	return 0;

cleanup_ringbuffer:
	DRM_LOCK(dev);
	i915_gem_cleanup_ringbuffer(dev);
	dev_priv->mm.suspended = 1;
	DRM_UNLOCK(dev);

	return ret;
}

int
i915_gem_leavevt_ioctl(struct drm_device *dev, void *data,
    struct drm_file *file_priv)
{

	if (drm_core_check_feature(dev, DRIVER_MODESET))
		return 0;

	drm_irq_uninstall(dev);
	return (i915_gem_idle(dev));
}

void
i915_gem_lastclose(struct drm_device *dev)
{
	int ret;

	if (drm_core_check_feature(dev, DRIVER_MODESET))
		return;

	ret = i915_gem_idle(dev);
	if (ret != 0)
		DRM_ERROR("failed to idle hardware: %d\n", ret);
}

static void
init_ring_lists(struct intel_ring_buffer *ring)
{

	INIT_LIST_HEAD(&ring->active_list);
	INIT_LIST_HEAD(&ring->request_list);
	INIT_LIST_HEAD(&ring->gpu_write_list);
}

void
i915_gem_load(struct drm_device *dev)
{
	int i;
	drm_i915_private_t *dev_priv = dev->dev_private;

	INIT_LIST_HEAD(&dev_priv->mm.active_list);
	INIT_LIST_HEAD(&dev_priv->mm.flushing_list);
	INIT_LIST_HEAD(&dev_priv->mm.inactive_list);
	INIT_LIST_HEAD(&dev_priv->mm.pinned_list);
	INIT_LIST_HEAD(&dev_priv->mm.fence_list);
	INIT_LIST_HEAD(&dev_priv->mm.deferred_free_list);
	INIT_LIST_HEAD(&dev_priv->mm.gtt_list);
	for (i = 0; i < I915_NUM_RINGS; i++)
		init_ring_lists(&dev_priv->ring[i]);
	for (i = 0; i < I915_MAX_NUM_FENCES; i++)
		INIT_LIST_HEAD(&dev_priv->fence_regs[i].lru_list);
	INIT_DELAYED_WORK(&dev_priv->mm.retire_work,
			  i915_gem_retire_work_handler);
	init_completion(&dev_priv->error_completion);

	/* On GEN3 we really need to make sure the ARB C3 LP bit is set */
	if (IS_GEN3(dev)) {
		I915_WRITE(MI_ARB_STATE,
			   _MASKED_BIT_ENABLE(MI_ARB_C3_LP_WRITE_ENABLE));
	}

	dev_priv->relative_constants_mode = I915_EXEC_CONSTANTS_REL_GENERAL;

	/* Old X drivers will take 0-2 for front, back, depth buffers */
	if (!drm_core_check_feature(dev, DRIVER_MODESET))
		dev_priv->fence_reg_start = 3;

	if (INTEL_INFO(dev)->gen >= 4 || IS_I945G(dev) || IS_I945GM(dev) || IS_G33(dev))
		dev_priv->num_fence_regs = 16;
	else
		dev_priv->num_fence_regs = 8;

	/* Initialize fence registers to zero */
	i915_gem_reset_fences(dev);

	i915_gem_detect_bit_6_swizzle(dev);

	dev_priv->mm.interruptible = true;

	dev_priv->mm.i915_lowmem = EVENTHANDLER_REGISTER(vm_lowmem,
	    i915_gem_lowmem, dev, EVENTHANDLER_PRI_ANY);
}

static int
i915_gem_init_phys_object(struct drm_device *dev, int id, int size, int align)
{
	drm_i915_private_t *dev_priv;
	struct drm_i915_gem_phys_object *phys_obj;
	int ret;

	dev_priv = dev->dev_private;
	if (dev_priv->mm.phys_objs[id - 1] != NULL || size == 0)
		return (0);

	phys_obj = kmalloc(sizeof(struct drm_i915_gem_phys_object), DRM_I915_GEM,
	    M_WAITOK | M_ZERO);

	phys_obj->id = id;

	phys_obj->handle = drm_pci_alloc(dev, size, align, ~0);
	if (phys_obj->handle == NULL) {
		ret = -ENOMEM;
		goto free_obj;
	}
	pmap_change_attr((vm_offset_t)phys_obj->handle->vaddr,
	    size / PAGE_SIZE, PAT_WRITE_COMBINING);

	dev_priv->mm.phys_objs[id - 1] = phys_obj;

	return (0);

free_obj:
	drm_free(phys_obj, DRM_I915_GEM);
	return (ret);
}

static void
i915_gem_free_phys_object(struct drm_device *dev, int id)
{
	drm_i915_private_t *dev_priv;
	struct drm_i915_gem_phys_object *phys_obj;

	dev_priv = dev->dev_private;
	if (dev_priv->mm.phys_objs[id - 1] == NULL)
		return;

	phys_obj = dev_priv->mm.phys_objs[id - 1];
	if (phys_obj->cur_obj != NULL)
		i915_gem_detach_phys_object(dev, phys_obj->cur_obj);

	drm_pci_free(dev, phys_obj->handle);
	drm_free(phys_obj, DRM_I915_GEM);
	dev_priv->mm.phys_objs[id - 1] = NULL;
}

void
i915_gem_free_all_phys_object(struct drm_device *dev)
{
	int i;

	for (i = I915_GEM_PHYS_CURSOR_0; i <= I915_MAX_PHYS_OBJECT; i++)
		i915_gem_free_phys_object(dev, i);
}

void
i915_gem_detach_phys_object(struct drm_device *dev,
    struct drm_i915_gem_object *obj)
{
	vm_page_t m;
	struct sf_buf *sf;
	char *vaddr, *dst;
	int i, page_count;

	if (obj->phys_obj == NULL)
		return;
	vaddr = obj->phys_obj->handle->vaddr;

	page_count = obj->base.size / PAGE_SIZE;
	VM_OBJECT_LOCK(obj->base.vm_obj);
	for (i = 0; i < page_count; i++) {
		m = i915_gem_wire_page(obj->base.vm_obj, i);
		if (m == NULL)
			continue; /* XXX */

		VM_OBJECT_UNLOCK(obj->base.vm_obj);
		sf = sf_buf_alloc(m);
		if (sf != NULL) {
			dst = (char *)sf_buf_kva(sf);
			memcpy(dst, vaddr + IDX_TO_OFF(i), PAGE_SIZE);
			sf_buf_free(sf);
		}
		drm_clflush_pages(&m, 1);

		VM_OBJECT_LOCK(obj->base.vm_obj);
		vm_page_reference(m);
		vm_page_dirty(m);
		vm_page_busy_wait(m, FALSE, "i915gem");
		vm_page_unwire(m, 0);
		vm_page_wakeup(m);
		atomic_add_long(&i915_gem_wired_pages_cnt, -1);
	}
	VM_OBJECT_UNLOCK(obj->base.vm_obj);
	intel_gtt_chipset_flush();

	obj->phys_obj->cur_obj = NULL;
	obj->phys_obj = NULL;
}

int
i915_gem_attach_phys_object(struct drm_device *dev,
			    struct drm_i915_gem_object *obj,
			    int id,
			    int align)
{
	drm_i915_private_t *dev_priv;
	vm_page_t m;
	struct sf_buf *sf;
	char *dst, *src;
	int i, page_count, ret;

	if (id > I915_MAX_PHYS_OBJECT)
		return (-EINVAL);

	if (obj->phys_obj != NULL) {
		if (obj->phys_obj->id == id)
			return (0);
		i915_gem_detach_phys_object(dev, obj);
	}

	dev_priv = dev->dev_private;
	if (dev_priv->mm.phys_objs[id - 1] == NULL) {
		ret = i915_gem_init_phys_object(dev, id, obj->base.size, align);
		if (ret != 0) {
			DRM_ERROR("failed to init phys object %d size: %zu\n",
				  id, obj->base.size);
			return (ret);
		}
	}

	/* bind to the object */
	obj->phys_obj = dev_priv->mm.phys_objs[id - 1];
	obj->phys_obj->cur_obj = obj;

	page_count = obj->base.size / PAGE_SIZE;

	VM_OBJECT_LOCK(obj->base.vm_obj);
	ret = 0;
	for (i = 0; i < page_count; i++) {
		m = i915_gem_wire_page(obj->base.vm_obj, i);
		if (m == NULL) {
			ret = -EIO;
			break;
		}
		VM_OBJECT_UNLOCK(obj->base.vm_obj);
		sf = sf_buf_alloc(m);
		src = (char *)sf_buf_kva(sf);
		dst = (char *)obj->phys_obj->handle->vaddr + IDX_TO_OFF(i);
		memcpy(dst, src, PAGE_SIZE);
		sf_buf_free(sf);

		VM_OBJECT_LOCK(obj->base.vm_obj);

		vm_page_reference(m);
		vm_page_busy_wait(m, FALSE, "i915gem");
		vm_page_unwire(m, 0);
		vm_page_wakeup(m);
		atomic_add_long(&i915_gem_wired_pages_cnt, -1);
	}
	VM_OBJECT_UNLOCK(obj->base.vm_obj);

	return (0);
}

static int
i915_gem_phys_pwrite(struct drm_device *dev, struct drm_i915_gem_object *obj,
    uint64_t data_ptr, uint64_t offset, uint64_t size,
    struct drm_file *file_priv)
{
	char *user_data, *vaddr;
	int ret;

	vaddr = (char *)obj->phys_obj->handle->vaddr + offset;
	user_data = (char *)(uintptr_t)data_ptr;

	if (copyin_nofault(user_data, vaddr, size) != 0) {
		/* The physical object once assigned is fixed for the lifetime
		 * of the obj, so we can safely drop the lock and continue
		 * to access vaddr.
		 */
		DRM_UNLOCK(dev);
		ret = -copyin(user_data, vaddr, size);
		DRM_LOCK(dev);
		if (ret != 0)
			return (ret);
	}

	intel_gtt_chipset_flush();
	return (0);
}

void
i915_gem_release(struct drm_device *dev, struct drm_file *file)
{
	struct drm_i915_file_private *file_priv;
	struct drm_i915_gem_request *request;

	file_priv = file->driver_priv;

	/* Clean up our request list when the client is going away, so that
	 * later retire_requests won't dereference our soon-to-be-gone
	 * file_priv.
	 */
	spin_lock(&file_priv->mm.lock);
	while (!list_empty(&file_priv->mm.request_list)) {
		request = list_first_entry(&file_priv->mm.request_list,
					   struct drm_i915_gem_request,
					   client_list);
		list_del(&request->client_list);
		request->file_priv = NULL;
	}
	spin_unlock(&file_priv->mm.lock);
}

static int
i915_gem_swap_io(struct drm_device *dev, struct drm_i915_gem_object *obj,
    uint64_t data_ptr, uint64_t size, uint64_t offset, enum uio_rw rw,
    struct drm_file *file)
{
	vm_object_t vm_obj;
	vm_page_t m;
	struct sf_buf *sf;
	vm_offset_t mkva;
	vm_pindex_t obj_pi;
	int cnt, do_bit17_swizzling, length, obj_po, ret, swizzled_po;

	if (obj->gtt_offset != 0 && rw == UIO_READ)
		do_bit17_swizzling = i915_gem_object_needs_bit17_swizzle(obj);
	else
		do_bit17_swizzling = 0;

	obj->dirty = 1;
	vm_obj = obj->base.vm_obj;
	ret = 0;

	VM_OBJECT_LOCK(vm_obj);
	vm_object_pip_add(vm_obj, 1);
	while (size > 0) {
		obj_pi = OFF_TO_IDX(offset);
		obj_po = offset & PAGE_MASK;

		m = i915_gem_wire_page(vm_obj, obj_pi);
		VM_OBJECT_UNLOCK(vm_obj);

		sf = sf_buf_alloc(m);
		mkva = sf_buf_kva(sf);
		length = min(size, PAGE_SIZE - obj_po);
		while (length > 0) {
			if (do_bit17_swizzling &&
			    (VM_PAGE_TO_PHYS(m) & (1 << 17)) != 0) {
				cnt = roundup2(obj_po + 1, 64);
				cnt = min(cnt - obj_po, length);
				swizzled_po = obj_po ^ 64;
			} else {
				cnt = length;
				swizzled_po = obj_po;
			}
			if (rw == UIO_READ)
				ret = -copyout_nofault(
				    (char *)mkva + swizzled_po,
				    (void *)(uintptr_t)data_ptr, cnt);
			else
				ret = -copyin_nofault(
				    (void *)(uintptr_t)data_ptr,
				    (char *)mkva + swizzled_po, cnt);
			if (ret != 0)
				break;
			data_ptr += cnt;
			size -= cnt;
			length -= cnt;
			offset += cnt;
			obj_po += cnt;
		}
		sf_buf_free(sf);
		VM_OBJECT_LOCK(vm_obj);
		if (rw == UIO_WRITE)
			vm_page_dirty(m);
		vm_page_reference(m);
		vm_page_busy_wait(m, FALSE, "i915gem");
		vm_page_unwire(m, 1);
		vm_page_wakeup(m);
		atomic_add_long(&i915_gem_wired_pages_cnt, -1);

		if (ret != 0)
			break;
	}
	vm_object_pip_wakeup(vm_obj);
	VM_OBJECT_UNLOCK(vm_obj);

	return (ret);
}

static int
i915_gem_gtt_write(struct drm_device *dev, struct drm_i915_gem_object *obj,
    uint64_t data_ptr, uint64_t size, uint64_t offset, struct drm_file *file)
{
	vm_offset_t mkva;
	int ret;

	/*
	 * Pass the unaligned physical address and size to pmap_mapdev_attr()
	 * so it can properly calculate whether an extra page needs to be
	 * mapped or not to cover the requested range.  The function will
	 * add the page offset into the returned mkva for us.
	 */
	mkva = (vm_offset_t)pmap_mapdev_attr(dev->agp->base + obj->gtt_offset +
	    offset, size, PAT_WRITE_COMBINING);
	ret = -copyin_nofault((void *)(uintptr_t)data_ptr, (char *)mkva, size);
	pmap_unmapdev(mkva, size);
	return (ret);
}

static int
i915_gem_obj_io(struct drm_device *dev, uint32_t handle, uint64_t data_ptr,
    uint64_t size, uint64_t offset, enum uio_rw rw, struct drm_file *file)
{
	struct drm_i915_gem_object *obj;
	vm_page_t *ma;
	vm_offset_t start, end;
	int npages, ret;

	if (size == 0)
		return (0);
	start = trunc_page(data_ptr);
	end = round_page(data_ptr + size);
	npages = howmany(end - start, PAGE_SIZE);
	ma = kmalloc(npages * sizeof(vm_page_t), DRM_I915_GEM, M_WAITOK |
	    M_ZERO);
	npages = vm_fault_quick_hold_pages(&curproc->p_vmspace->vm_map,
	    (vm_offset_t)data_ptr, size,
	    (rw == UIO_READ ? VM_PROT_WRITE : 0 ) | VM_PROT_READ, ma, npages);
	if (npages == -1) {
		ret = -EFAULT;
		goto free_ma;
	}

	ret = i915_mutex_lock_interruptible(dev);
	if (ret != 0)
		goto unlocked;

	obj = to_intel_bo(drm_gem_object_lookup(dev, file, handle));
	if (&obj->base == NULL) {
		ret = -ENOENT;
		goto unlock;
	}
	if (offset > obj->base.size || size > obj->base.size - offset) {
		ret = -EINVAL;
		goto out;
	}

	if (rw == UIO_READ) {
		ret = i915_gem_object_set_cpu_read_domain_range(obj,
		    offset, size);
		if (ret != 0)
			goto out;
		ret = i915_gem_swap_io(dev, obj, data_ptr, size, offset,
		    UIO_READ, file);
	} else {
		if (obj->phys_obj) {
			ret = i915_gem_phys_pwrite(dev, obj, data_ptr, offset,
			    size, file);
		} else if (obj->gtt_space &&
		    obj->base.write_domain != I915_GEM_DOMAIN_CPU) {
			ret = i915_gem_object_pin(obj, 0, true);
			if (ret != 0)
				goto out;
			ret = i915_gem_object_set_to_gtt_domain(obj, true);
			if (ret != 0)
				goto out_unpin;
			ret = i915_gem_object_put_fence(obj);
			if (ret != 0)
				goto out_unpin;
			ret = i915_gem_gtt_write(dev, obj, data_ptr, size,
			    offset, file);
out_unpin:
			i915_gem_object_unpin(obj);
		} else {
			ret = i915_gem_object_set_to_cpu_domain(obj, true);
			if (ret != 0)
				goto out;
			ret = i915_gem_swap_io(dev, obj, data_ptr, size, offset,
			    UIO_WRITE, file);
		}
	}
out:
	drm_gem_object_unreference(&obj->base);
unlock:
	DRM_UNLOCK(dev);
unlocked:
	vm_page_unhold_pages(ma, npages);
free_ma:
	drm_free(ma, DRM_I915_GEM);
	return (ret);
}

static int
i915_gem_pager_ctor(void *handle, vm_ooffset_t size, vm_prot_t prot,
    vm_ooffset_t foff, struct ucred *cred, u_short *color)
{

	*color = 0; /* XXXKIB */
	return (0);
}

int i915_intr_pf;

static int
i915_gem_pager_fault(vm_object_t vm_obj, vm_ooffset_t offset, int prot,
    vm_page_t *mres)
{
	struct drm_gem_object *gem_obj;
	struct drm_i915_gem_object *obj;
	struct drm_device *dev;
	drm_i915_private_t *dev_priv;
	vm_page_t m, oldm;
	int cause, ret;
	bool write;

	gem_obj = vm_obj->handle;
	obj = to_intel_bo(gem_obj);
	dev = obj->base.dev;
	dev_priv = dev->dev_private;
#if 0
	write = (prot & VM_PROT_WRITE) != 0;
#else
	write = true;
#endif
	vm_object_pip_add(vm_obj, 1);

	/*
	 * Remove the placeholder page inserted by vm_fault() from the
	 * object before dropping the object lock. If
	 * i915_gem_release_mmap() is active in parallel on this gem
	 * object, then it owns the drm device sx and might find the
	 * placeholder already. Then, since the page is busy,
	 * i915_gem_release_mmap() sleeps waiting for the busy state
	 * of the page cleared. We will be not able to acquire drm
	 * device lock until i915_gem_release_mmap() is able to make a
	 * progress.
	 */
	if (*mres != NULL) {
		oldm = *mres;
		vm_page_remove(oldm);
		*mres = NULL;
	} else
		oldm = NULL;
retry:
	VM_OBJECT_UNLOCK(vm_obj);
unlocked_vmobj:
	cause = ret = 0;
	m = NULL;

	if (i915_intr_pf) {
		ret = i915_mutex_lock_interruptible(dev);
		if (ret != 0) {
			cause = 10;
			goto out;
		}
	} else
		DRM_LOCK(dev);

	/*
	 * Since the object lock was dropped, other thread might have
	 * faulted on the same GTT address and instantiated the
	 * mapping for the page.  Recheck.
	 */
	VM_OBJECT_LOCK(vm_obj);
	m = vm_page_lookup(vm_obj, OFF_TO_IDX(offset));
	if (m != NULL) {
		if ((m->flags & PG_BUSY) != 0) {
			DRM_UNLOCK(dev);
#if 0 /* XXX */
			vm_page_sleep(m, "915pee");
#endif
			goto retry;
		}
		goto have_page;
	} else
		VM_OBJECT_UNLOCK(vm_obj);

	/* Now bind it into the GTT if needed */
	if (!obj->map_and_fenceable) {
		ret = i915_gem_object_unbind(obj);
		if (ret != 0) {
			cause = 20;
			goto unlock;
		}
	}
	if (!obj->gtt_space) {
		ret = i915_gem_object_bind_to_gtt(obj, 0, true);
		if (ret != 0) {
			cause = 30;
			goto unlock;
		}

		ret = i915_gem_object_set_to_gtt_domain(obj, write);
		if (ret != 0) {
			cause = 40;
			goto unlock;
		}
	}

	if (obj->tiling_mode == I915_TILING_NONE)
		ret = i915_gem_object_put_fence(obj);
	else
		ret = i915_gem_object_get_fence(obj, NULL);
	if (ret != 0) {
		cause = 50;
		goto unlock;
	}

	if (i915_gem_object_is_inactive(obj))
		list_move_tail(&obj->mm_list, &dev_priv->mm.inactive_list);

	obj->fault_mappable = true;
	VM_OBJECT_LOCK(vm_obj);
	m = vm_phys_fictitious_to_vm_page(dev->agp->base + obj->gtt_offset +
	    offset);
	if (m == NULL) {
		cause = 60;
		ret = -EFAULT;
		goto unlock;
	}
	KASSERT((m->flags & PG_FICTITIOUS) != 0,
	    ("not fictitious %p", m));
	KASSERT(m->wire_count == 1, ("wire_count not 1 %p", m));

	if ((m->flags & PG_BUSY) != 0) {
		DRM_UNLOCK(dev);
#if 0 /* XXX */
		vm_page_sleep(m, "915pbs");
#endif
		goto retry;
	}
	m->valid = VM_PAGE_BITS_ALL;
	vm_page_insert(m, vm_obj, OFF_TO_IDX(offset));
have_page:
	*mres = m;
	vm_page_busy_try(m, false);

	DRM_UNLOCK(dev);
	if (oldm != NULL) {
		vm_page_free(oldm);
	}
	vm_object_pip_wakeup(vm_obj);
	return (VM_PAGER_OK);

unlock:
	DRM_UNLOCK(dev);
out:
	KASSERT(ret != 0, ("i915_gem_pager_fault: wrong return"));
	if (ret == -EAGAIN || ret == -EIO || ret == -EINTR) {
		goto unlocked_vmobj;
	}
	VM_OBJECT_LOCK(vm_obj);
	vm_object_pip_wakeup(vm_obj);
	return (VM_PAGER_ERROR);
}

static void
i915_gem_pager_dtor(void *handle)
{
	struct drm_gem_object *obj;
	struct drm_device *dev;

	obj = handle;
	dev = obj->dev;

	DRM_LOCK(dev);
	drm_gem_free_mmap_offset(obj);
	i915_gem_release_mmap(to_intel_bo(obj));
	drm_gem_object_unreference(obj);
	DRM_UNLOCK(dev);
}

struct cdev_pager_ops i915_gem_pager_ops = {
	.cdev_pg_fault	= i915_gem_pager_fault,
	.cdev_pg_ctor	= i915_gem_pager_ctor,
	.cdev_pg_dtor	= i915_gem_pager_dtor
};

static int
i915_gem_object_set_cpu_read_domain_range(struct drm_i915_gem_object *obj,
    uint64_t offset, uint64_t size)
{
	uint32_t old_read_domains;
	int i, ret;

	if (offset == 0 && size == obj->base.size)
		return (i915_gem_object_set_to_cpu_domain(obj, 0));

	ret = i915_gem_object_flush_gpu_write_domain(obj);
	if (ret != 0)
		return (ret);
	ret = i915_gem_object_wait_rendering(obj);
	if (ret != 0)
		return (ret);

	i915_gem_object_flush_gtt_write_domain(obj);

	if (obj->page_cpu_valid == NULL &&
	    (obj->base.read_domains & I915_GEM_DOMAIN_CPU) != 0)
		return (0);

	if (obj->page_cpu_valid == NULL) {
		obj->page_cpu_valid = kmalloc(obj->base.size / PAGE_SIZE,
		    DRM_I915_GEM, M_WAITOK | M_ZERO);
	} else if ((obj->base.read_domains & I915_GEM_DOMAIN_CPU) == 0)
		memset(obj->page_cpu_valid, 0, obj->base.size / PAGE_SIZE);

	for (i = offset / PAGE_SIZE; i <= (offset + size - 1) / PAGE_SIZE;
	     i++) {
		if (obj->page_cpu_valid[i])
			continue;
		drm_clflush_pages(obj->pages + i, 1);
		obj->page_cpu_valid[i] = 1;
	}

	KASSERT((obj->base.write_domain & ~I915_GEM_DOMAIN_CPU) == 0,
	    ("In gpu write domain"));

	old_read_domains = obj->base.read_domains;
	obj->base.read_domains |= I915_GEM_DOMAIN_CPU;

	return (0);
}

#define	GEM_PARANOID_CHECK_GTT 0
#if GEM_PARANOID_CHECK_GTT
static void
i915_gem_assert_pages_not_mapped(struct drm_device *dev, vm_page_t *ma,
    int page_count)
{
	struct drm_i915_private *dev_priv;
	vm_paddr_t pa;
	unsigned long start, end;
	u_int i;
	int j;

	dev_priv = dev->dev_private;
	start = OFF_TO_IDX(dev_priv->mm.gtt_start);
	end = OFF_TO_IDX(dev_priv->mm.gtt_end);
	for (i = start; i < end; i++) {
		pa = intel_gtt_read_pte_paddr(i);
		for (j = 0; j < page_count; j++) {
			if (pa == VM_PAGE_TO_PHYS(ma[j])) {
				panic("Page %p in GTT pte index %d pte %x",
				    ma[i], i, intel_gtt_read_pte(i));
			}
		}
	}
}
#endif

static void
i915_gem_process_flushing_list(struct intel_ring_buffer *ring,
    uint32_t flush_domains)
{
	struct drm_i915_gem_object *obj, *next;
	uint32_t old_write_domain;

	list_for_each_entry_safe(obj, next, &ring->gpu_write_list,
	    gpu_write_list) {
		if (obj->base.write_domain & flush_domains) {
			old_write_domain = obj->base.write_domain;
			obj->base.write_domain = 0;
			list_del_init(&obj->gpu_write_list);
			i915_gem_object_move_to_active(obj, ring,
			    i915_gem_next_request_seqno(ring));
		}
	}
}

#define	VM_OBJECT_LOCK_ASSERT_OWNED(object)

static vm_page_t
i915_gem_wire_page(vm_object_t object, vm_pindex_t pindex)
{
	vm_page_t m;
	int rv;

	VM_OBJECT_LOCK_ASSERT_OWNED(object);
	m = vm_page_grab(object, pindex, VM_ALLOC_NORMAL | VM_ALLOC_RETRY);
	if (m->valid != VM_PAGE_BITS_ALL) {
		if (vm_pager_has_page(object, pindex)) {
			rv = vm_pager_get_page(object, &m, 1);
			m = vm_page_lookup(object, pindex);
			if (m == NULL)
				return (NULL);
			if (rv != VM_PAGER_OK) {
				vm_page_free(m);
				return (NULL);
			}
		} else {
			pmap_zero_page(VM_PAGE_TO_PHYS(m));
			m->valid = VM_PAGE_BITS_ALL;
			m->dirty = 0;
		}
	}
	vm_page_wire(m);
	vm_page_wakeup(m);
	atomic_add_long(&i915_gem_wired_pages_cnt, 1);
	return (m);
}

int
i915_gem_flush_ring(struct intel_ring_buffer *ring, uint32_t invalidate_domains,
    uint32_t flush_domains)
{
	int ret;

	if (((invalidate_domains | flush_domains) & I915_GEM_GPU_DOMAINS) == 0)
		return 0;

	ret = ring->flush(ring, invalidate_domains, flush_domains);
	if (ret)
		return ret;

	if (flush_domains & I915_GEM_GPU_DOMAINS)
		i915_gem_process_flushing_list(ring, flush_domains);
	return 0;
}

u32
i915_gem_next_request_seqno(struct intel_ring_buffer *ring)
{
	if (ring->outstanding_lazy_request == 0)
		ring->outstanding_lazy_request = i915_gem_get_seqno(ring->dev);

	return ring->outstanding_lazy_request;
}

static void
i915_gem_clear_fence_reg(struct drm_device *dev, struct drm_i915_fence_reg *reg)
{
	drm_i915_private_t *dev_priv = dev->dev_private;
	uint32_t fence_reg = reg - dev_priv->fence_regs;

	switch (INTEL_INFO(dev)->gen) {
	case 7:
	case 6:
		I915_WRITE64(FENCE_REG_SANDYBRIDGE_0 + fence_reg*8, 0);
		break;
	case 5:
	case 4:
		I915_WRITE64(FENCE_REG_965_0 + fence_reg*8, 0);
		break;
	case 3:
		if (fence_reg >= 8)
			fence_reg = FENCE_REG_945_8 + (fence_reg - 8) * 4;
		else
	case 2:
			fence_reg = FENCE_REG_830_0 + fence_reg * 4;

		I915_WRITE(fence_reg, 0);
		break;
	}

	list_del_init(&reg->lru_list);
	reg->obj = NULL;
	reg->setup_seqno = 0;
	reg->pin_count = 0;
}

static int
i915_gpu_is_active(struct drm_device *dev)
{
	drm_i915_private_t *dev_priv;

	dev_priv = dev->dev_private;
	return (!list_empty(&dev_priv->mm.flushing_list) ||
	    !list_empty(&dev_priv->mm.active_list));
}

static void
i915_gem_lowmem(void *arg)
{
	struct drm_device *dev;
	struct drm_i915_private *dev_priv;
	struct drm_i915_gem_object *obj, *next;
	int cnt, cnt_fail, cnt_total;

	dev = arg;
	dev_priv = dev->dev_private;

	if (lockmgr(&dev->dev_struct_lock, LK_EXCLUSIVE|LK_NOWAIT))
		return;

rescan:
	/* first scan for clean buffers */
	i915_gem_retire_requests(dev);

	cnt_total = cnt_fail = cnt = 0;

	list_for_each_entry_safe(obj, next, &dev_priv->mm.inactive_list,
	    mm_list) {
		if (i915_gem_object_is_purgeable(obj)) {
			if (i915_gem_object_unbind(obj) != 0)
				cnt_total++;
		} else
			cnt_total++;
	}

	/* second pass, evict/count anything still on the inactive list */
	list_for_each_entry_safe(obj, next, &dev_priv->mm.inactive_list,
	    mm_list) {
		if (i915_gem_object_unbind(obj) == 0)
			cnt++;
		else
			cnt_fail++;
	}

	if (cnt_fail > cnt_total / 100 && i915_gpu_is_active(dev)) {
		/*
		 * We are desperate for pages, so as a last resort, wait
		 * for the GPU to finish and discard whatever we can.
		 * This has a dramatic impact to reduce the number of
		 * OOM-killer events whilst running the GPU aggressively.
		 */
		if (i915_gpu_idle(dev) == 0)
			goto rescan;
	}
	DRM_UNLOCK(dev);
}

void
i915_gem_unload(struct drm_device *dev)
{
	struct drm_i915_private *dev_priv;

	dev_priv = dev->dev_private;
	EVENTHANDLER_DEREGISTER(vm_lowmem, dev_priv->mm.i915_lowmem);
}
