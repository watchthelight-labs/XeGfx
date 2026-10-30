/*
 * xgprobe: the first userspace client of an XeGfx DRM node. It walks the path Mesa ANV needs, one step per line,
 * and stops at the first failure: open the render node, DRM version, i915 params, the engine query, a context, then
 * on every legacy ring a softpinned batch that stores a dword into a second buffer, both mapped through the
 * kext's map ioctl. Exit status 0 only when every ring wrote its value.
 *   cc -O2 -I<libdrm>/include/drm -o xgprobe xgprobe.c
 * <wtl> project: xegfx | author: watchthelight </wtl>
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <drm.h>
#include <i915_drm.h>

/* the kext's map ioctls (xkpi/src/i915/xg_i915_map.c; NullMoth's Darwin libdrm ABI) */
struct xg_drm_mmap { uint64_t offset, size; uint32_t prot, flags; uint64_t addr; };
struct xg_drm_munmap { uint64_t addr, size; };
#define XG_DRM_IOCTL_MMAP	_IOWR('N', 0x40, struct xg_drm_mmap)
#define XG_DRM_IOCTL_MUNMAP	_IOW('N', 0x41, struct xg_drm_munmap)

#define MI_STORE_DWORD_IMM	((0x20u << 23) | 2)	/* gen8+: 64-bit address, then the value */
#define MI_BATCH_BUFFER_END	(0x0au << 23)
#define BATCH_VA		0x100000ull
#define TARGET_VA		0x200000ull

static int fd = -1;

static int
step(const char *what, int rc)
{
	printf("xgprobe: %-28s %s", what, rc == 0 ? "ok" : "FAIL");
	if (rc != 0)
		printf(" (%s)", strerror(errno));
	printf("\n");
	return rc;
}

static int
io(unsigned long cmd, void *arg)
{
	int r;

	do
		r = ioctl(fd, cmd, arg);
	while (r == -1 && (errno == EINTR || errno == EAGAIN));
	return r;
}

static int
getparam(int p, int *v)
{
	struct drm_i915_getparam gp = { .param = p, .value = v };

	return io(DRM_IOCTL_I915_GETPARAM, &gp);
}

static void *
map(uint32_t handle, uint64_t size)
{
	struct drm_i915_gem_mmap_offset mo = { .handle = handle, .flags = I915_MMAP_OFFSET_WC };
	struct xg_drm_mmap m = { .size = size, .prot = PROT_READ | PROT_WRITE };

	if (io(DRM_IOCTL_I915_GEM_MMAP_OFFSET, &mo) != 0)
		return NULL;
	m.offset = mo.offset;
	if (io(XG_DRM_IOCTL_MMAP, &m) != 0)
		return NULL;
	return (void *)(uintptr_t)m.addr;
}

static void
unmap(void *p, uint64_t size)
{
	struct xg_drm_munmap u = { (uint64_t)(uintptr_t)p, size };

	if (p != NULL)
		io(XG_DRM_IOCTL_MUNMAP, &u);
}

static uint32_t
bo(uint64_t size)
{
	struct drm_i915_gem_create c = { .size = size };

	return io(DRM_IOCTL_I915_GEM_CREATE, &c) == 0 ? c.handle : 0;
}

static void
bo_close(uint32_t h)
{
	struct drm_gem_close c = { .handle = h };

	if (h != 0)
		io(DRM_IOCTL_GEM_CLOSE, &c);
}

/* one batch on ring `ring` in context ctx: store `value` at TARGET_VA, wait, read it back through the mapping */
static int
store_on(const char *name, uint64_t ring, uint32_t ctx, uint32_t value)
{
	uint32_t hb = bo(4096), ht = bo(4096);
	volatile uint32_t *batch = hb ? map(hb, 4096) : NULL, *target = ht ? map(ht, 4096) : NULL;
	struct drm_i915_gem_exec_object2 obj[2];
	struct drm_i915_gem_execbuffer2 eb;
	struct drm_i915_gem_wait w = { .bo_handle = hb, .timeout_ns = 2000000000ll };
	char what[64];
	int rc = -1;

	snprintf(what, sizeof(what), "map buffers (%s)", name);
	if (step(what, batch && target ? 0 : -1) != 0)
		goto out;
	target[0] = 0;
	batch[0] = MI_STORE_DWORD_IMM;
	batch[1] = (uint32_t)TARGET_VA;
	batch[2] = (uint32_t)(TARGET_VA >> 32);
	batch[3] = value;
	batch[4] = MI_BATCH_BUFFER_END;
	batch[5] = 0;
	memset(obj, 0, sizeof(obj));
	obj[0].handle = ht;
	obj[0].offset = TARGET_VA;
	obj[0].flags = EXEC_OBJECT_PINNED | EXEC_OBJECT_SUPPORTS_48B_ADDRESS | EXEC_OBJECT_WRITE;
	obj[1].handle = hb;	/* the batch goes last */
	obj[1].offset = BATCH_VA;
	obj[1].flags = EXEC_OBJECT_PINNED | EXEC_OBJECT_SUPPORTS_48B_ADDRESS;
	memset(&eb, 0, sizeof(eb));
	eb.buffers_ptr = (uint64_t)(uintptr_t)obj;
	eb.buffer_count = 2;
	eb.batch_len = 24;
	eb.flags = ring | I915_EXEC_NO_RELOC;
	eb.rsvd1 = ctx;
	snprintf(what, sizeof(what), "execbuffer (%s)", name);
	if (step(what, io(DRM_IOCTL_I915_GEM_EXECBUFFER2, &eb)) != 0)
		goto out;
	snprintf(what, sizeof(what), "wait (%s)", name);
	if (step(what, io(DRM_IOCTL_I915_GEM_WAIT, &w)) != 0)
		goto out;
	printf("xgprobe: %-28s 0x%08x, want 0x%08x\n", "readback", target[0], value);
	rc = target[0] == value ? 0 : -1;
out:
	unmap((void *)batch, 4096);
	unmap((void *)target, 4096);
	bo_close(hb);
	bo_close(ht);
	return rc;
}

int
main(int argc, char **argv)
{
	static const struct { int cls; uint64_t ring; const char *name; } rings[] = {
		{ I915_ENGINE_CLASS_RENDER, I915_EXEC_RENDER, "render" },
		{ I915_ENGINE_CLASS_COPY, I915_EXEC_BLT, "copy" },
		{ I915_ENGINE_CLASS_VIDEO, I915_EXEC_BSD, "video" },
		{ I915_ENGINE_CLASS_VIDEO_ENHANCE, I915_EXEC_VEBOX, "video-enhance" },
	};
	const char *path = argc > 1 ? argv[1] : "/dev/dri/renderD128";
	char name[32] = { 0 };
	struct drm_version v = { .name = name, .name_len = sizeof(name) - 1 };
	struct drm_i915_query_item qi = { .query_id = DRM_I915_QUERY_ENGINE_INFO };
	struct drm_i915_query q = { .num_items = 1, .items_ptr = (uint64_t)(uintptr_t)&qi };
	struct drm_i915_query_engine_info *info;
	struct drm_i915_gem_context_create cc = { 0 };
	struct drm_i915_gem_context_destroy cd;
	int chip = 0, rev = 0, softpin = 0, has[5] = { 0 }, failed = 0;

	setvbuf(stdout, NULL, _IOLBF, 0);
	fd = open(path, O_RDWR | O_CLOEXEC);
	if (step(path, fd < 0 ? -1 : 0) != 0)
		return 2;
	if (step("drm version", io(DRM_IOCTL_VERSION, &v)) != 0)
		return 2;
	printf("xgprobe: %-28s %s %d.%d.%d\n", "driver", name, v.version_major, v.version_minor, v.version_patchlevel);
	if (step("getparam chipset", getparam(I915_PARAM_CHIPSET_ID, &chip)) != 0)
		return 2;
	getparam(I915_PARAM_REVISION, &rev);
	getparam(I915_PARAM_HAS_EXEC_SOFTPIN, &softpin);
	printf("xgprobe: %-28s 0x%04x rev %d softpin %d\n", "device", chip, rev, softpin);

	/* two calls: the size, then the data */
	if (step("query engines (size)", io(DRM_IOCTL_I915_QUERY, &q) || qi.length <= 0 ? -1 : 0) != 0)
		return 2;
	info = calloc(1, (size_t)qi.length);
	qi.data_ptr = (uint64_t)(uintptr_t)info;
	if (step("query engines", info == NULL || io(DRM_IOCTL_I915_QUERY, &q) || qi.length <= 0 ? -1 : 0) != 0)
		return 2;
	for (unsigned i = 0; i < info->num_engines; i++) {
		struct i915_engine_class_instance *e = &info->engines[i].engine;

		printf("xgprobe: %-28s class %u instance %u\n", "engine", e->engine_class, e->engine_instance);
		if (e->engine_class < 5)
			has[e->engine_class] = 1;
	}
	if (step("context create", io(DRM_IOCTL_I915_GEM_CONTEXT_CREATE, &cc)) != 0)
		return 2;
	for (unsigned i = 0; i < sizeof(rings) / sizeof(rings[0]); i++)
		if (has[rings[i].cls] && store_on(rings[i].name, rings[i].ring, cc.ctx_id, 0x58670000u | i) != 0)
			failed++;
	cd.ctx_id = cc.ctx_id;
	cd.pad = 0;
	step("context destroy", io(DRM_IOCTL_I915_GEM_CONTEXT_DESTROY, &cd));
	printf("xgprobe: %s\n", failed ? "FAILED" : "all rings stored their value");
	return failed ? 1 : 0;
}
