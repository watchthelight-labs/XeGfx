// SPDX-License-Identifier: MIT
/*
 * XeGfx stages 4 and 5: after i915 attaches, report whether its GTs are usable (probe returns 0 even when GuC
 * loading failed and the GT was wedged), then run one kernel-context request on every uabi engine (stage 5), then
 * have every engine write a dword the CPU can read back (stage 6).
 * <wtl> project: xegfx | author: watchthelight </wtl>
 */
#include <linux/pci.h>
#include <linux/string.h>

#include "i915_drv.h"
#include "i915_request.h"
#include "gt/intel_engine.h"
#include "gt/intel_engine_pm.h"
#include "gt/intel_gt_pm.h"
#include "gt/intel_gpu_commands.h"
#include "gt/intel_ring.h"
#include "gt/intel_gt.h"
#include "gt/uc/intel_uc.h"
#include "gt/intel_gtt.h"
#include "i915_gem_gtt.h"

#include "xg_i915.h"

static struct drm_i915_private *
xg_i915_find(struct pci_dev **pdevp)
{
	struct pci_dev *pdev = NULL;

	while ((pdev = lkpi_pci_get_device(PCI_VENDOR_ID_INTEL, PCI_ANY_ID, pdev)) != NULL)
		if (pdev_to_i915(pdev) != NULL) {
			*pdevp = pdev;
			return (pdev_to_i915(pdev));
		}
	return (NULL);
}

int
xg_i915_state(struct xg_i915_state *st)
{
	struct drm_i915_private *i915;
	struct intel_engine_cs *engine;
	struct intel_uc_fw_ver *v;
	struct pci_dev *pdev;
	struct intel_gt *gt;
	unsigned int id;

	memset(st, 0, sizeof(*st));
	st->render_minor = -1;
	if ((i915 = xg_i915_find(&pdev)) == NULL)
		return (-ENODEV);
	for_each_gt(gt, i915, id) {
		st->gts++;
		if (intel_gt_is_wedged(gt))
			st->wedged |= 1 << id;
	}
	gt = to_gt(i915);
	st->guc_status = __intel_uc_fw_status(&gt->uc.guc.fw);
	st->huc_status = __intel_uc_fw_status(&gt->uc.huc.fw);
	st->gsc_status = __intel_uc_fw_status(&gt->uc.gsc.fw);
	st->guc_submission = intel_uc_uses_guc_submission(&gt->uc);
	v = &gt->uc.guc.fw.file_selected.ver;
	st->guc_ver = v->major << 16 | (v->minor & 0xff) << 8 | (v->patch & 0xff);
	for_each_uabi_engine(engine, i915)
		st->engines++;
	if (i915->drm.render != NULL)
		st->render_minor = i915->drm.render->index;
	pci_dev_put(pdev);
	return (0);
}

int
xg_i915_gtawake(int level)
{
	struct drm_i915_private *i915;
	struct pci_dev *pdev;
	struct intel_gt *gt;
	unsigned int id;
	int n = 0;

	if (level <= 0)
		return (0);
	if ((i915 = xg_i915_find(&pdev)) == NULL)
		return (-ENODEV);
	/* what i915's forcewake_user debugfs file does on open; never released, the A/B runs to the next restart */
	for_each_gt(gt, i915, id) {
		atomic_inc(&gt->user_wakeref);
		intel_gt_pm_get_untracked(gt);
		if (level >= 2)
			intel_uncore_forcewake_user_get(gt->uncore);
		n++;
	}
	pci_dev_put(pdev);
	printf("xegfx: gtawake %d held on %d GTs\n", level, n);
	return (n);
}

int
xg_i915_exec(struct xg_i915_exec *ex, int timeout_ms)
{
	struct drm_i915_private *i915;
	struct intel_engine_cs *engine;
	struct i915_request *rq;
	struct pci_dev *pdev;
	ktime_t t0;
	long t;
	int n;

	memset(ex, 0, sizeof(*ex));
	if ((i915 = xg_i915_find(&pdev)) == NULL)
		return (-ENODEV);
	for_each_uabi_engine(engine, i915) {
		if ((n = ex->engines) >= XG_I915_MAX_ENGINES)
			break;
		ex->engines++;
		strscpy(ex->name[n], engine->name, sizeof(ex->name[n]));
		t0 = ktime_get();
		rq = intel_engine_create_kernel_request(engine);
		if (IS_ERR(rq)) {
			ex->err[n] = PTR_ERR(rq);
			continue;
		}
		i915_request_get(rq);
		i915_request_add(rq);
		t = i915_request_wait(rq, 0, msecs_to_jiffies(timeout_ms));
		ex->ns[n] = ktime_to_ns(ktime_sub(ktime_get(), t0));
		i915_request_put(rq);
		if (t < 0)
			ex->err[n] = (int)t;
		else
			ex->done++;
	}
	pci_dev_put(pdev);
	return (0);
}

/* a slot past i915's own status page users (I915_GEM_HWS_SCRATCH is only written on gen2-6) */
#define XG_HWS_SLOT	(I915_GEM_HWS_SCRATCH + 1)

int
xg_i915_store(struct xg_i915_store *sd, int timeout_ms)
{
	struct drm_i915_private *i915;
	struct intel_engine_cs *engine;
	struct i915_request *rq;
	struct pci_dev *pdev;
	u32 *cs, *slot;
	long t;
	int n;

	memset(sd, 0, sizeof(*sd));
	if ((i915 = xg_i915_find(&pdev)) == NULL)
		return (-ENODEV);
	for_each_uabi_engine(engine, i915) {
		if (!intel_engine_can_store_dword(engine))
			continue;
		if ((n = sd->engines) >= XG_I915_MAX_ENGINES)
			break;
		sd->engines++;
		strscpy(sd->name[n], engine->name, sizeof(sd->name[n]));
		sd->want[n] = 0x58670000 | (n << 8) | engine->uabi_class;
		slot = &engine->status_page.addr[XG_HWS_SLOT];
		intel_write_status_page(engine, XG_HWS_SLOT, 0);
		rq = intel_engine_create_kernel_request(engine);
		if (IS_ERR(rq)) {
			sd->err[n] = PTR_ERR(rq);
			continue;
		}
		cs = intel_ring_begin(rq, 4);
		if (IS_ERR(cs)) {
			sd->err[n] = PTR_ERR(cs);
			i915_request_add(rq);
			continue;
		}
		*cs++ = MI_STORE_DWORD_IMM_GEN4 | MI_USE_GGTT;
		*cs++ = i915_ggtt_offset(engine->status_page.vma) + XG_HWS_SLOT * sizeof(u32);
		*cs++ = 0;
		*cs++ = sd->want[n];
		intel_ring_advance(rq, cs);
		i915_request_get(rq);
		i915_request_add(rq);
		t = i915_request_wait(rq, 0, msecs_to_jiffies(timeout_ms));
		i915_request_put(rq);
		drm_clflush_virt_range(slot, sizeof(*slot));
		sd->got[n] = intel_read_status_page(engine, XG_HWS_SLOT);
		if (t < 0)
			sd->err[n] = (int)t;
		else if (sd->got[n] != sd->want[n])
			sd->err[n] = -EIO;
		else
			sd->ok++;
	}
	pci_dev_put(pdev);
	return (0);
}

/*
 * With display disabled i915 never takes over the firmware's plane, so the GGTT range the boot surface scans out of
 * is free for the next pin: once WindowServer composites into it (X6), a context or ring placed there means our
 * frame copy writes over GPU state (N-251, N-255 froze about a minute in). Reserve it, PTEs left as they are.
 */
int
xg_i915_reserve_bootfb(unsigned long long offset, unsigned long long size)
{
	static struct drm_mm_node node;
	struct drm_i915_private *i915;
	struct pci_dev *pdev;
	struct i915_ggtt *ggtt;
	int err;

	if ((i915 = xg_i915_find(&pdev)) == NULL)
		return (-ENODEV);
	if (drm_mm_node_allocated(&node))
		return (0);
	ggtt = to_gt(i915)->ggtt;
	offset = round_down(offset, I915_GTT_PAGE_SIZE);
	size = round_up(size, I915_GTT_PAGE_SIZE);
	mutex_lock(&ggtt->vm.mutex);
	err = i915_gem_gtt_reserve(&ggtt->vm, NULL, &node, size, offset, I915_COLOR_UNEVICTABLE, PIN_NOEVICT);
	mutex_unlock(&ggtt->vm.mutex);
	return (err);
}

/*
 * Zero-copy scanout (xezcflip): a few GGTT slots whose PTEs the accelerator points at a compositor frame's own pages,
 * then writes PLANE_SURF itself. Based on Mountainatic/ArrowWhatever patches 0008/0009 (PolyForm Noncommercial).
 */
static struct drm_mm_node xg_flip_node[XG_FLIP_MAX_SLOTS];
static unsigned int xg_flip_slots;
static unsigned long long xg_flip_bytes;

/* reserve up to `slots` slots of `surface_bytes` each below the GuC top (the plane addresses 32 bits); 0 or -errno */
int
xg_flip_init(unsigned long long surface_bytes, unsigned int slots, unsigned long long *ggtt_out,
    unsigned int *count_out)
{
	struct drm_i915_private *i915;
	struct pci_dev *pdev;
	struct i915_ggtt *ggtt;
	unsigned long long size;
	unsigned int i, done = 0;
	int err = 0;

	if (ggtt_out == NULL || count_out == NULL || surface_bytes == 0)
		return (-EINVAL);
	if ((i915 = xg_i915_find(&pdev)) == NULL)
		return (-ENODEV);
	size = round_up(surface_bytes, I915_GTT_PAGE_SIZE);
	if (xg_flip_slots != 0) {	/* already reserved: same slots if they are big enough */
		if (xg_flip_bytes < size)
			return (-ENOSPC);
		for (i = 0; i < xg_flip_slots; i++)
			ggtt_out[i] = xg_flip_node[i].start;
		*count_out = xg_flip_slots;
		return (0);
	}
	if (slots > XG_FLIP_MAX_SLOTS)
		slots = XG_FLIP_MAX_SLOTS;
	ggtt = to_gt(i915)->ggtt;
	mutex_lock(&ggtt->vm.mutex);
	for (i = 0; i < slots; i++) {
		err = i915_gem_gtt_insert(&ggtt->vm, NULL, &xg_flip_node[i], size, I915_GTT_PAGE_SIZE,
		    I915_COLOR_UNEVICTABLE, 0x80000000ULL, 0xfee00000ULL, PIN_NOEVICT);
		if (err)
			break;
		ggtt_out[i] = xg_flip_node[i].start;
		done++;
	}
	mutex_unlock(&ggtt->vm.mutex);
	if (done == 0)
		return (err != 0 ? err : -ENOSPC);
	xg_flip_slots = done;
	xg_flip_bytes = size;
	*count_out = done;
	return (0);
}

/* point a slot's PTEs at these 4 KiB physical pages, then one TLB invalidate for the slot */
int
xg_flip_map(unsigned int slot, unsigned long long surface_bytes, const unsigned long long *phys,
    unsigned int npages)
{
	struct drm_i915_private *i915;
	struct pci_dev *pdev;
	struct i915_ggtt *ggtt;
	unsigned int i, need, pat;

	if (slot >= xg_flip_slots || phys == NULL || surface_bytes == 0 || surface_bytes > xg_flip_bytes)
		return (-EINVAL);
	if ((i915 = xg_i915_find(&pdev)) == NULL)
		return (-ENODEV);
	need = (unsigned int)(round_up(surface_bytes, I915_GTT_PAGE_SIZE) >> 12);
	if (npages < need)
		return (-EINVAL);
	ggtt = to_gt(i915)->ggtt;
	pat = i915_gem_get_pat_index(i915, I915_CACHE_NONE);	/* system memory, scanned by the display */
	for (i = 0; i < need; i++)
		ggtt->vm.insert_page(&ggtt->vm, (dma_addr_t)(phys[i] & ~0xfffULL),
		    xg_flip_node[slot].start + ((u64)i << 12), pat, 0);
	ggtt->invalidate(ggtt);
	return (0);
}

/* the slot's PTEs back to the scratch page, so a stale frame cannot be scanned out */
int
xg_flip_unmap(unsigned int slot, unsigned long long surface_bytes)
{
	struct drm_i915_private *i915;
	struct pci_dev *pdev;
	struct i915_ggtt *ggtt;

	if (slot >= xg_flip_slots || surface_bytes == 0 || surface_bytes > xg_flip_bytes)
		return (-EINVAL);
	if ((i915 = xg_i915_find(&pdev)) == NULL)
		return (-ENODEV);
	ggtt = to_gt(i915)->ggtt;
	if (ggtt->vm.scratch_range != NULL)
		ggtt->vm.scratch_range(&ggtt->vm, xg_flip_node[slot].start, round_up(surface_bytes, I915_GTT_PAGE_SIZE));
	ggtt->invalidate(ggtt);
	return (0);
}
