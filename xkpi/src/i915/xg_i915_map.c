// SPDX-License-Identifier: MIT
/*
 * XeGfx: mmap of GEM objects for a kext. XNU does not let a kext implement mmap(2) on a device node, so userspace
 * maps through two ioctls on the DRM node instead, with the layout and numbers of NullMoth's Darwin libdrm patch
 * (userland/patches/libdrm-macos.patch), which then works unchanged. The lookup follows i915_gem_mmap(); the
 * object's pages stay pinned for as long as the mapping lives, because nothing here can revoke a mapping.
 * <wtl> project: xegfx | author: watchthelight </wtl>
 */
#include <linux/list.h>
#include <linux/pci.h>
#include <linux/mutex.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>

#include <drm/drm_file.h>
#include <drm/drm_vma_manager.h>

#include "i915_drv.h"
#include "gem/i915_gem_mman.h"
#include "gem/i915_gem_object.h"

#include "xg_drm_sync.h"
#include "xg_i915.h"
#include "xg_vm_xnu.h"

struct xg_drm_mmap {
	u64	offset;	/* the fake offset from DRM_IOCTL_I915_GEM_MMAP_OFFSET */
	u64	size;
	u32	prot;
	u32	flags;
	u64	addr;	/* out */
};

struct xg_drm_munmap {
	u64	addr;
	u64	size;
};

#define XG_DRM_IOCTL_MMAP	_IOWR('N', 0x40, struct xg_drm_mmap)
#define XG_DRM_IOCTL_MUNMAP	_IOW('N', 0x41, struct xg_drm_munmap)
#define XG_PROT_WRITE		0x2	/* PROT_WRITE on Darwin and FreeBSD */

struct xg_map {
	struct list_head		link;
	void				*key;
	u64				addr, size;
	xg_vm_xmem_t			xm;
	struct drm_i915_gem_object	*obj;
};

static LIST_HEAD(xg_maps);
static DEFINE_MUTEX(xg_maps_lock);

extern int (*xg_fd_map_ioctl)(void *lfile, void *key, unsigned long cmd, void *data);
extern void (*xg_fd_map_close)(void *key);

static void
xg_map_free(struct xg_map *m)
{
	xg_vm_x_release(m->xm);
	i915_gem_object_unpin_pages(m->obj);
	i915_gem_object_put(m->obj);
	kfree(m);
}

/* physical ranges for [skip, skip + size) of the object's pages, neighbours merged */
static int
xg_map_ranges(struct sg_table *sgt, u64 skip, u64 size, u64 **pap, u64 **lenp, unsigned int *np)
{
	struct scatterlist *sg;
	unsigned int i, n = 0;
	u64 *pa, *len;

	pa = kcalloc(sgt->orig_nents, sizeof(*pa), GFP_KERNEL);
	len = kcalloc(sgt->orig_nents, sizeof(*len), GFP_KERNEL);
	if (pa == NULL || len == NULL) {
		kfree(pa);
		kfree(len);
		return -ENOMEM;
	}
	for_each_sg(sgt->sgl, sg, sgt->orig_nents, i) {
		u64 p, l = sg->length;

		if (sg_page(sg) == NULL) {
			kfree(pa);
			kfree(len);
			return -ENODEV;	/* stolen memory has no pages to hand out */
		}
		p = page_to_phys(sg_page(sg)) + sg->offset;
		if (skip >= l) {
			skip -= l;
			continue;
		}
		p += skip;
		l -= skip;
		skip = 0;
		l = min(l, size);
		if (n > 0 && pa[n - 1] + len[n - 1] == p)
			len[n - 1] += l;
		else {
			pa[n] = p;
			len[n++] = l;
		}
		size -= l;
		if (size == 0)
			break;
	}
	if (size != 0) {
		kfree(pa);
		kfree(len);
		return -EINVAL;
	}
	*pap = pa;
	*lenp = len;
	*np = n;
	return 0;
}

static int
xg_map_mmap(struct file *filp, void *key, struct xg_drm_mmap *a)
{
	struct drm_file *priv = filp->private_data;
	struct drm_device *dev = priv->minor->dev;
	struct drm_vma_offset_node *node;
	struct drm_i915_gem_object *obj = NULL;
	struct i915_mmap_offset *mmo = NULL;
	struct xg_map *m;
	unsigned int n;
	u64 *pa, *len, skip;
	int cache, err;

	if (a->size == 0 || (a->offset | a->size) & ~PAGE_MASK)
		return -EINVAL;
	/*
	 * Lookup lock first, then RCU: the shim's rwlock_t sleeps when contended and an RCU reader here cannot sleep
	 * (N-286 panicked in exactly this spot). i915 takes them the other way round, which is fine where rwlock_t spins.
	 */
	drm_vma_offset_lock_lookup(dev->vma_offset_manager);
	rcu_read_lock();
	node = drm_vma_offset_lookup_locked(dev->vma_offset_manager, a->offset >> PAGE_SHIFT,
	    a->size >> PAGE_SHIFT);
	if (node && drm_vma_node_is_allowed(node, priv) && !node->driver_private) {
		mmo = container_of(node, struct i915_mmap_offset, vma_node);
		obj = i915_gem_object_get_rcu(mmo->obj);
	}
	rcu_read_unlock();
	drm_vma_offset_unlock_lookup(dev->vma_offset_manager);
	if (obj == NULL)
		return node ? -EACCES : -EINVAL;

	switch (mmo->mmap_type) {
	case I915_MMAP_TYPE_WB:
	case I915_MMAP_TYPE_FIXED:	/* system memory only on these parts, where FIXED means WB */
		cache = XG_VM_CACHE_WB;
		break;
	case I915_MMAP_TYPE_WC:
		cache = XG_VM_CACHE_WC;
		break;
	case I915_MMAP_TYPE_UC:
		cache = XG_VM_CACHE_UC;
		break;
	default:
		/* the GTT type needs fault-time fencing through the aperture */
		err = -ENODEV;
		goto put;
	}
	if (i915_gem_object_is_readonly(obj) && (a->prot & XG_PROT_WRITE)) {
		err = -EINVAL;
		goto put;
	}
	skip = (a->offset >> PAGE_SHIFT) - drm_vma_node_start(node);
	skip <<= PAGE_SHIFT;
	if (skip + a->size > obj->base.size) {
		err = -EINVAL;
		goto put;
	}
	if ((err = i915_gem_object_pin_pages_unlocked(obj)) != 0)
		goto put;
	if ((err = xg_map_ranges(obj->mm.pages, skip, a->size, &pa, &len, &n)) != 0)
		goto unpin;
	m = kzalloc(sizeof(*m), GFP_KERNEL);
	if (m == NULL) {
		err = -ENOMEM;
		goto ranges;
	}
	m->xm = xg_vm_x_map_user(pa, len, n, cache, (a->prot & XG_PROT_WRITE) != 0, &m->addr);
	if (m->xm == NULL) {
		kfree(m);
		err = -ENOMEM;
		goto ranges;
	}
	kfree(pa);
	kfree(len);
	m->key = key;
	m->size = a->size;
	m->obj = obj;	/* keeps the reference and the page pin */
	mutex_lock(&xg_maps_lock);
	list_add(&m->link, &xg_maps);
	mutex_unlock(&xg_maps_lock);
	a->addr = m->addr;
	return 0;
ranges:
	kfree(pa);
	kfree(len);
unpin:
	i915_gem_object_unpin_pages(obj);
put:
	i915_gem_object_put(obj);
	return err;
}

static int
xg_map_munmap(void *key, struct xg_drm_munmap *a)
{
	struct xg_map *m, *found = NULL;

	mutex_lock(&xg_maps_lock);
	list_for_each_entry(m, &xg_maps, link)
		if (m->key == key && m->addr == a->addr && m->size == a->size) {
			list_del(&m->link);
			found = m;
			break;
		}
	mutex_unlock(&xg_maps_lock);
	if (found == NULL)
		return -EINVAL;
	xg_map_free(found);
	return 0;
}

/* the PCI identity libdrm reads on the BSDs and Darwin to enumerate devices (drmParsePciBusInfo) */
static int
xg_pciinfo(struct file *filp, struct drm_pciinfo *pi)
{
	struct drm_file *priv = filp->private_data;
	struct pci_dev *pdev = to_pci_dev(priv->minor->dev->dev);

	pi->domain = pci_domain_nr(pdev->bus);
	pi->bus = pdev->bus->number;
	pi->dev = PCI_SLOT(pdev->devfn);
	pi->func = PCI_FUNC(pdev->devfn);
	pi->vendor_id = pdev->vendor;
	pi->device_id = pdev->device;
	pi->subvendor_id = pdev->subsystem_vendor;
	pi->subdevice_id = pdev->subsystem_device;
	pi->revision_id = pdev->revision;
	return 0;
}

static int
xg_map_ioctl(void *lfile, void *key, unsigned long cmd, void *data)
{
	int err;

	if (cmd == XG_DRM_IOCTL_MMAP)
		err = xg_map_mmap(lfile, key, data);
	else if (cmd == XG_DRM_IOCTL_MUNMAP)
		err = xg_map_munmap(key, data);
	else if (cmd == DRM_IOCTL_GET_PCIINFO)
		err = xg_pciinfo(lfile, data);
	else if (cmd == XG_DRM_IOCTL_BO_EXPORT_FENCE || cmd == XG_DRM_IOCTL_BO_IMPORT_FENCE)
		err = xg_drm_bo_fence(((struct file *)lfile)->private_data, cmd == XG_DRM_IOCTL_BO_EXPORT_FENCE, data);
	else
		return ENOTTY;	/* not ours: the drm ioctl path gets it */
	return -err;		/* the shim speaks FreeBSD errno */
}

static void
xg_map_close(void *key)
{
	struct xg_map *m, *t;
	LIST_HEAD(dead);

	mutex_lock(&xg_maps_lock);
	list_for_each_entry_safe(m, t, &xg_maps, link)
		if (m->key == key)
			list_move(&m->link, &dead);
	mutex_unlock(&xg_maps_lock);
	list_for_each_entry_safe(m, t, &dead, link)
		xg_map_free(m);
}

void
xg_i915_map_init(void)
{
	xg_fd_map_close = xg_map_close;
	xg_fd_map_ioctl = xg_map_ioctl;
}
