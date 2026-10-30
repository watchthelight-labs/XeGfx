/* SPDX-License-Identifier: MIT */
/*
 * Stand-in for drm-kmod's drm_format_helper.h (GPL-2.0-or-later). The only files XeGfx builds that include it
 * (drm_atomic_helper.c, i915 intel_atomic_plane.c) use none of its functions, so declarations are enough.
 * <wtl> project: xegfx | file: xkpi/include/drm/drm_format_helper.h | author: watchthelight </wtl>
 */
#ifndef __LINUX_DRM_FORMAT_HELPER_H
#define __LINUX_DRM_FORMAT_HELPER_H
struct drm_device;
struct drm_format_info;
struct drm_framebuffer;
struct drm_rect;
struct iosys_map;
#endif
