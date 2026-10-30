// SPDX-License-Identifier: MIT
/*
 * XeGfx: implicit-sync fences of a GEM object moved to and from a DRM syncobj. On Linux, userspace gets these as
 * sync_file fds through a dma-buf (DMA_BUF_IOCTL_EXPORT_SYNC_FILE / IMPORT_SYNC_FILE); a kext cannot create file
 * descriptors, so the userspace layer (libdrm on Darwin) stands in a syncobj for each sync_file and calls these.
 * The semantics follow dma_buf_export_sync_file() and dma_buf_import_sync_file().
 * <wtl> project: xegfx | author: watchthelight </wtl>
 */
#include <linux/dma-fence.h>
#include <linux/dma-fence-unwrap.h>
#include <linux/dma-resv.h>
#include <uapi/linux/dma-buf.h>

#include <drm/drm_file.h>
#include <drm/drm_gem.h>
#include <drm/drm_syncobj.h>

#include "xg_drm_sync.h"

int
xg_drm_bo_fence(struct drm_file *priv, int export, struct xg_drm_bo_fence *a)
{
	struct drm_gem_object *obj;
	struct drm_syncobj *so;
	struct dma_fence *fence = NULL, *f;
	struct dma_fence_unwrap iter;
	unsigned int n = 0;
	int err = 0;

	if ((a->flags & ~DMA_BUF_SYNC_RW) != 0 || (a->flags & DMA_BUF_SYNC_RW) == 0 || a->pad != 0)
		return -EINVAL;
	if ((obj = drm_gem_object_lookup(priv, a->handle)) == NULL)
		return -ENOENT;
	if ((so = drm_syncobj_find(priv, a->syncobj)) == NULL) {
		drm_gem_object_put(obj);
		return -ENOENT;
	}
	if (export) {
		err = dma_resv_get_singleton(obj->resv, dma_resv_usage_rw(a->flags & DMA_BUF_SYNC_WRITE), &fence);
		if (err == 0) {
			/* nothing pending: an already signalled fence, as the sync_file path hands out */
			if (fence == NULL)
				fence = dma_fence_get_stub();
			drm_syncobj_replace_fence(so, fence);
		}
	} else if ((fence = drm_syncobj_fence_get(so)) == NULL) {
		err = -EINVAL;
	} else {
		dma_fence_unwrap_for_each(f, &iter, fence)
			n++;
		if (n > 0) {
			dma_resv_lock(obj->resv, NULL);
			err = dma_resv_reserve_fences(obj->resv, n);
			if (err == 0)
				dma_fence_unwrap_for_each(f, &iter, fence)
					dma_resv_add_fence(obj->resv, f, a->flags & DMA_BUF_SYNC_WRITE ?
					    DMA_RESV_USAGE_WRITE : DMA_RESV_USAGE_READ);
			dma_resv_unlock(obj->resv);
		}
	}
	dma_fence_put(fence);
	drm_syncobj_put(so);
	drm_gem_object_put(obj);
	return err;
}
