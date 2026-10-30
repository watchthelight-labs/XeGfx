/* XeGfx: what stages 4 and 5 ask i915 after its attach (xkpi/src/i915/xg_i915_exec.c). Plain C types only, so
 * the kext side can include it.
 * <wtl> project: xegfx | author: watchthelight </wtl> */
#ifndef _XG_I915_H_
#define _XG_I915_H_
#ifdef __cplusplus
extern "C" {
#endif

#define XG_I915_MAX_ENGINES	16

struct xg_i915_state {
	int		gts;		/* GTs i915 brought up (MTL/ARL: primary and media) */
	int		wedged;		/* bit n: GT n is wedged, so probe "passed" but nothing will run */
	int		guc_status;	/* enum intel_uc_fw_status of GT 0; 10 is RUNNING */
	int		huc_status;
	int		gsc_status;
	int		guc_submission;	/* 1 when requests go through the GuC */
	unsigned int	guc_ver;	/* major << 16 | minor << 8 | patch of the loaded image */
	int		engines;	/* uabi engines */
	int		render_minor;	/* -1 without a render node */
};

struct xg_i915_exec {
	int		engines;
	int		done;		/* requests that completed inside the timeout */
	char		name[XG_I915_MAX_ENGINES][16];
	long long	ns[XG_I915_MAX_ENGINES];	/* add to completion */
	int		err[XG_I915_MAX_ENGINES];	/* 0, or the negative errno of create or wait */
};

struct xg_i915_store {
	int		engines;	/* engines tried (i915 says they can store a dword) */
	int		ok;		/* engines whose write the CPU read back */
	char		name[XG_I915_MAX_ENGINES][16];
	unsigned int	want[XG_I915_MAX_ENGINES];
	unsigned int	got[XG_I915_MAX_ENGINES];
	int		err[XG_I915_MAX_ENGINES];
};

/* all return 0 with the report filled, or -ENODEV when no i915 device is bound */
int xg_i915_state(struct xg_i915_state *st);
/* I-116 A/B: 1 keeps every GT awake (wakeref), 2 also holds forcewake on all domains; the GT count, or -ENODEV */
int xg_i915_gtawake(int level);
/* one kernel-context request per uabi engine, each waited for at most timeout_ms; sleeps */
int xg_i915_exec(struct xg_i915_exec *ex, int timeout_ms);
/* install the GEM map ioctls ('N' 0x40/0x41, NullMoth's Darwin libdrm ABI) on the DRM nodes */
void xg_i915_map_init(void);
/* every engine writes a value into its own status page with MI_STORE_DWORD_IMM through the GGTT; the CPU reads it */
int xg_i915_store(struct xg_i915_store *sd, int timeout_ms);
/* keep i915 off the GGTT range the boot surface scans out of (Hybrid, display disabled); 0 or a negative errno */
int xg_i915_reserve_bootfb(unsigned long long offset, unsigned long long size);

#ifdef __cplusplus
}
#endif
#endif
