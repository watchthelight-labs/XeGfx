// drmdiag: every step libdrm's device enumeration takes on Darwin, printed one by one, so one boot shows where
// drmGetDevices2 drops the XeGfx nodes (N-230, N-231 found 0 devices).
// <wtl> project: xegfx | author: watchthelight </wtl>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <xf86drm.h>

struct drm_pciinfo {
    uint16_t domain;
    uint8_t bus, dev, func;
    uint16_t vendor_id, device_id, subvendor_id, subdevice_id;
    uint8_t revision_id;
};
#define DIAG_GET_PCIINFO DRM_IOR(0x15, struct drm_pciinfo)

static void probe_fd(const char *path)
{
    struct stat st;
    struct drm_pciinfo pi;
    drmDevicePtr d = NULL;
    int fd = open(path, O_RDWR | O_CLOEXEC), r;

    if (fd < 0) { printf("  open %s: errno %d\n", path, errno); return; }
    fstat(fd, &st);
    printf("  open %s: fd %d, fstat rdev 0x%08x (major %d minor 0x%06x)\n", path, fd, (unsigned)st.st_rdev,
           (int)major(st.st_rdev), (unsigned)st.st_rdev & 0xffffff);
    memset(&pi, 0, sizeof(pi));
    r = ioctl(fd, DIAG_GET_PCIINFO, &pi);
    printf("    GET_PCIINFO (0x%08lx): %d errno %d -> %04x:%02x:%02x.%u %04x:%04x sub %04x:%04x rev %02x\n",
           (unsigned long)DIAG_GET_PCIINFO, r, r ? errno : 0, pi.domain, pi.bus, pi.dev, pi.func, pi.vendor_id,
           pi.device_id, pi.subvendor_id, pi.subdevice_id, pi.revision_id);
    printf("    drmGetNodeTypeFromFd: %d\n", drmGetNodeTypeFromFd(fd));
    char *name = drmGetDeviceNameFromFd2(fd);
    printf("    drmGetDeviceNameFromFd2: %s\n", name ? name : "(null)");
    free(name);
    r = drmGetDevice2(fd, 0, &d);
    printf("    drmGetDevice2: %d", r);
    if (r == 0 && d) {
        printf(" bustype %d nodes 0x%x", d->bustype, d->available_nodes);
        drmFreeDevice(&d);
    }
    printf("\n");
    close(fd);
}

int main(void)
{
    DIR *dir = opendir(DRM_DIR_NAME);
    struct dirent *e;
    char path[512];

    printf("DRM_DIR_NAME %s\n", DRM_DIR_NAME);
    if (!dir) { printf("opendir: errno %d\n", errno); return 1; }
    while ((e = readdir(dir)) != NULL) {
        struct stat st;

        if (e->d_name[0] == '.')
            continue;
        snprintf(path, sizeof(path), "%s/%s", DRM_DIR_NAME, e->d_name);
        if (stat(path, &st)) { printf("%s: stat errno %d\n", path, errno); continue; }
        printf("%s: mode %06o rdev 0x%08x (major %d minor 0x%06x)\n", path, st.st_mode, (unsigned)st.st_rdev,
               (int)major(st.st_rdev), (unsigned)st.st_rdev & 0xffffff);
        if (S_ISCHR(st.st_mode))
            probe_fd(path);
    }
    closedir(dir);
    printf("drmGetDevices2(0): %d\n", drmGetDevices2(0, NULL, 0));
    printf("drmGetDevices2(PCI_REVISION): %d\n", drmGetDevices2(DRM_DEVICE_GET_PCI_REVISION, NULL, 0));
    return 0;
}
