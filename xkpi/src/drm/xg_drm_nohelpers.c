// SPDX-License-Identifier: MIT
/*
 * XeGfx: the HDCP and writeback helpers i915 calls, as "not supported here" answers. drm-kmod's own versions are
 * GPL-only and excluded (exclude.txt); before this file they were panicking stubs, and N-223 hit one in intel_hdcp_init.
 * HDCP: no content-protection property, no revoked receivers. Writeback: i915 has no writeback connector.
 * <wtl> project: xegfx | author: watchthelight </wtl>
 */
#include <linux/types.h>

#include <drm/drm_connector.h>
#include <drm/drm_writeback.h>
#include <drm/display/drm_hdcp_helper.h>

int
drm_hdcp_check_ksvs_revoked(struct drm_device *dev, u8 *ksvs, u32 ksv_count)
{
	return 0;
}

int
drm_connector_attach_content_protection_property(struct drm_connector *connector, bool hdcp_content_type)
{
	return 0;
}

void
drm_hdcp_update_content_protection(struct drm_connector *connector, u64 val)
{
}

struct dma_fence *
drm_writeback_get_out_fence(struct drm_writeback_connector *connector)
{
	return NULL;
}

int
drm_writeback_prepare_job(struct drm_writeback_job *j)
{
	return 0;
}

int
drm_writeback_set_fb(struct drm_connector_state *cs, struct drm_framebuffer *fb)
{
	return fb == NULL ? 0 : -EINVAL;
}

void
drm_writeback_cleanup_job(struct drm_writeback_job *j)
{
}
