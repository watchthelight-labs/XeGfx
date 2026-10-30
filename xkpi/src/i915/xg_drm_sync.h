/* SPDX-License-Identifier: MIT */
/*
 * XeGfx DRM node ioctls in group 'N' beside the map pair (0x40/0x41): a GEM object's implicit fences to and from a
 * syncobj, standing in for DMA_BUF_IOCTL_EXPORT_SYNC_FILE / IMPORT_SYNC_FILE. Userspace keeps an identical copy.
 * <wtl> project: xegfx | author: watchthelight </wtl>
 */
#ifndef _XG_DRM_SYNC_H_
#define _XG_DRM_SYNC_H_

struct xg_drm_bo_fence {
	__u32	handle;		/* GEM handle */
	__u32	flags;		/* DMA_BUF_SYNC_READ and/or DMA_BUF_SYNC_WRITE */
	__u32	syncobj;	/* export: replaced with the object's fences; import: added to the object */
	__u32	pad;
};

#define XG_DRM_IOCTL_BO_EXPORT_FENCE	_IOW('N', 0x42, struct xg_drm_bo_fence)
#define XG_DRM_IOCTL_BO_IMPORT_FENCE	_IOW('N', 0x43, struct xg_drm_bo_fence)

struct drm_file;
int xg_drm_bo_fence(struct drm_file *priv, int export, struct xg_drm_bo_fence *a);

#endif
