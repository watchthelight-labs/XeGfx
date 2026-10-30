/*
 * XeGfx vm: kernel mappings of page lists at an address IOKit picks, for LinuxKPI's vmap() family
 * (xkpi/src/lkpi/linux_page.c). Plain C types so LinuxKPI code and the kext side can include it.
 * <wtl> project: xegfx | author: watchthelight </wtl>
 */
#ifndef _XG_VMAP_H_
#define _XG_VMAP_H_
#ifdef __cplusplus
extern "C" {
#endif

/* n page frames (physical addresses, page aligned) mapped contiguously; NULL on failure */
void *xg_vm_vmap_pa(const unsigned long long *pa, unsigned int n, int memattr);
/* 0 and the mapping gone, or -1 when va did not come from xg_vm_vmap_pa */
int xg_vm_vunmap(void *va);
/* map two pages, write through the new mapping, read through the old one; 0 or a negative step */
int xg_vm_vmap_selftest(void);

#ifdef __cplusplus
}
#endif
#endif
