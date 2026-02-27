/* fs/sysfs.c — Minimal synthetic /sys for Linux-compatible userland tools */
#include "fs/vfs.h"
#include "fs/sysfs.h"
#include "mm/kmalloc.h"
#include "lib/string.h"
#include "lib/klog.h"
#include "lib/string.h"
#include "drivers/storage/blkdev.h"
#include "arch/x86_64/pci.h"
#include "drivers/usb/usb_core.h"
#include <stdint.h>
#include <stddef.h>

#define SYS_ROOT             0
#define SYS_BUS_DIR          1
#define SYS_BLOCK_DIR        2
#define SYS_BUS_PCI_DIR      3
#define SYS_BUS_USB_DIR      4
#define SYS_PCI_DEVICES_DIR  5
#define SYS_USB_DEVICES_DIR  6
#define SYS_DEVICES_DIR      7
#define SYS_DEVICES_PCI_DIR  8
#define SYS_DEVICES_USB_DIR  9
#define SYS_PCI_DEV_DIR      10
#define SYS_USB_DEV_DIR      11
#define SYS_BLOCK_DEV_DIR    12
#define SYS_PCI_DEV_LINK     13
#define SYS_USB_DEV_LINK     14
#define SYS_FILE_PCI_UEVENT  15
#define SYS_FILE_USB_UEVENT  16
#define SYS_FILE_USB_MFR     17
#define SYS_FILE_USB_PROD    18
#define SYS_FILE_BLK_SIZE    19
#define SYS_FILE_BLK_DEV     20

typedef struct {
    int type;
    int idx;
} sysfs_node_t;

static vnode_t *sysfs_root = NULL;
static fs_inst_t *sysfs_fsi = NULL;

static vnode_t *sysfs_lookup(vnode_t *dir, const char *name);
static int      sysfs_open(vnode_t *v, int flags);
static int      sysfs_close(vnode_t *v);
static ssize_t  sysfs_read(vnode_t *v, void *buf, size_t len, uint64_t off);
static ssize_t  sysfs_write(vnode_t *v, const void *buf, size_t len, uint64_t off);
static int      sysfs_readdir(vnode_t *dir, uint64_t *cookie, vfs_dirent_t *out);
static int      sysfs_stat(vnode_t *v, vfs_stat_t *st);
static int      sysfs_readlink(vnode_t *v, char *buf, size_t bufsize);
static vnode_t *sysfs_mount(fs_inst_t *fsi, blkdev_t *dev);
static void     sysfs_unmount(fs_inst_t *fsi);
static void     sysfs_evict(vnode_t *v);

static fs_ops_t sysfs_ops;

static vnode_t *sysfs_alloc_node(int type, int idx, uint32_t mode) {
    vnode_t *v = vfs_alloc_vnode();
    if (!v) return NULL;
    sysfs_node_t *sn = kmalloc(sizeof(sysfs_node_t));
    if (!sn) {
        kfree(v);
        return NULL;
    }
    sn->type = type;
    sn->idx = idx;

    v->ino = ((uint64_t)type << 32) | (uint32_t)(idx + 1);
    v->mode = mode;
    v->ops = &sysfs_ops;
    v->fsi = sysfs_fsi;
    v->fs_data = sn;
    v->refcount = 1;
    return v;
}

static int usb_name_from_index(int idx, char *out, size_t out_sz) {
    const usb_device_t *u = usb_get_device(idx);
    if (!u || !out || out_sz < 4) return -1;
    int n = ksnprintf(out, out_sz, "1-%u", (unsigned)u->port);
    return (n < 0) ? -1 : 0;
}

static int pci_name_from_index(int idx, char *out, size_t out_sz) {
    pci_device_t *devs = NULL;
    int ndev = pci_get_devices(&devs);
    if (idx < 0 || idx >= ndev || !out || out_sz < 16) return -1;
    pci_device_t *d = &devs[idx];
    int n = ksnprintf(out, out_sz, "0000:%02x:%02x.%u", d->bus, d->dev, d->fn);
    return (n < 0) ? -1 : 0;
}

static int blk_name_from_index(int idx, char *out, size_t out_sz) {
    blkdev_t *d = blkdev_get_nth(idx);
    if (!d || !out || out_sz < 2) return -1;
    strncpy(out, d->name, out_sz - 1);
    out[out_sz - 1] = '\0';
    return 0;
}

static vnode_t *sysfs_lookup(vnode_t *dir, const char *name) {
    if (!dir || !dir->fs_data || !name) return NULL;
    sysfs_node_t *sn = (sysfs_node_t *)dir->fs_data;

    if (sn->type == SYS_ROOT) {
        if (strcmp(name, "bus") == 0)   return sysfs_alloc_node(SYS_BUS_DIR, 0, VFS_S_IFDIR | 0555);
        if (strcmp(name, "block") == 0) return sysfs_alloc_node(SYS_BLOCK_DIR, 0, VFS_S_IFDIR | 0555);
        if (strcmp(name, "devices") == 0) return sysfs_alloc_node(SYS_DEVICES_DIR, 0, VFS_S_IFDIR | 0555);
        return NULL;
    }

    if (sn->type == SYS_BUS_DIR) {
        if (strcmp(name, "pci") == 0) return sysfs_alloc_node(SYS_BUS_PCI_DIR, 0, VFS_S_IFDIR | 0555);
        if (strcmp(name, "usb") == 0) return sysfs_alloc_node(SYS_BUS_USB_DIR, 0, VFS_S_IFDIR | 0555);
        return NULL;
    }

    if (sn->type == SYS_BUS_PCI_DIR) {
        if (strcmp(name, "devices") == 0) return sysfs_alloc_node(SYS_PCI_DEVICES_DIR, 0, VFS_S_IFDIR | 0555);
        return NULL;
    }

    if (sn->type == SYS_BUS_USB_DIR) {
        if (strcmp(name, "devices") == 0) return sysfs_alloc_node(SYS_USB_DEVICES_DIR, 0, VFS_S_IFDIR | 0555);
        return NULL;
    }

    if (sn->type == SYS_PCI_DEVICES_DIR) {
        pci_device_t *devs = NULL;
        int n = pci_get_devices(&devs);
        char dname[24];
        for (int i = 0; i < n; i++) {
            if (pci_name_from_index(i, dname, sizeof(dname)) == 0 && strcmp(name, dname) == 0)
                return sysfs_alloc_node(SYS_PCI_DEV_LINK, i, VFS_S_IFLNK | 0777);
        }
        return NULL;
    }

    if (sn->type == SYS_USB_DEVICES_DIR) {
        int n = usb_device_count();
        char dname[16];
        for (int i = 0; i < n; i++) {
            if (usb_name_from_index(i, dname, sizeof(dname)) == 0 && strcmp(name, dname) == 0)
                return sysfs_alloc_node(SYS_USB_DEV_LINK, i, VFS_S_IFLNK | 0777);
        }
        return NULL;
    }

    if (sn->type == SYS_DEVICES_DIR) {
        if (strcmp(name, "pci") == 0) return sysfs_alloc_node(SYS_DEVICES_PCI_DIR, 0, VFS_S_IFDIR | 0555);
        if (strcmp(name, "usb") == 0) return sysfs_alloc_node(SYS_DEVICES_USB_DIR, 0, VFS_S_IFDIR | 0555);
        return NULL;
    }

    if (sn->type == SYS_DEVICES_PCI_DIR) {
        pci_device_t *devs = NULL;
        int n = pci_get_devices(&devs);
        char dname[24];
        for (int i = 0; i < n; i++) {
            if (pci_name_from_index(i, dname, sizeof(dname)) == 0 && strcmp(name, dname) == 0)
                return sysfs_alloc_node(SYS_PCI_DEV_DIR, i, VFS_S_IFDIR | 0555);
        }
        return NULL;
    }

    if (sn->type == SYS_DEVICES_USB_DIR) {
        int n = usb_device_count();
        char dname[16];
        for (int i = 0; i < n; i++) {
            if (usb_name_from_index(i, dname, sizeof(dname)) == 0 && strcmp(name, dname) == 0)
                return sysfs_alloc_node(SYS_USB_DEV_DIR, i, VFS_S_IFDIR | 0555);
        }
        return NULL;
    }

    if (sn->type == SYS_PCI_DEV_DIR) {
        if (strcmp(name, "uevent") == 0)
            return sysfs_alloc_node(SYS_FILE_PCI_UEVENT, sn->idx, VFS_S_IFREG | 0444);
        return NULL;
    }

    if (sn->type == SYS_USB_DEV_DIR) {
        if (strcmp(name, "uevent") == 0)
            return sysfs_alloc_node(SYS_FILE_USB_UEVENT, sn->idx, VFS_S_IFREG | 0444);
        if (strcmp(name, "manufacturer") == 0)
            return sysfs_alloc_node(SYS_FILE_USB_MFR, sn->idx, VFS_S_IFREG | 0444);
        if (strcmp(name, "product") == 0)
            return sysfs_alloc_node(SYS_FILE_USB_PROD, sn->idx, VFS_S_IFREG | 0444);
        return NULL;
    }

    if (sn->type == SYS_BLOCK_DIR) {
        int n = blkdev_count();
        char dname[32];
        for (int i = 0; i < n; i++) {
            if (blk_name_from_index(i, dname, sizeof(dname)) == 0 && strcmp(name, dname) == 0)
                return sysfs_alloc_node(SYS_BLOCK_DEV_DIR, i, VFS_S_IFDIR | 0555);
        }
        return NULL;
    }

    if (sn->type == SYS_BLOCK_DEV_DIR) {
        if (strcmp(name, "size") == 0)
            return sysfs_alloc_node(SYS_FILE_BLK_SIZE, sn->idx, VFS_S_IFREG | 0444);
        if (strcmp(name, "dev") == 0)
            return sysfs_alloc_node(SYS_FILE_BLK_DEV, sn->idx, VFS_S_IFREG | 0444);
        return NULL;
    }

    return NULL;
}

static int sysfs_open(vnode_t *v, int flags) { (void)v; (void)flags; return 0; }
static int sysfs_close(vnode_t *v) { (void)v; return 0; }

static ssize_t sysfs_emit_text(char *dst, size_t dst_sz, const char *src, uint64_t off, size_t len) {
    size_t total = strlen(src);
    if (off >= total) return 0;
    size_t avail = total - (size_t)off;
    if (len < avail) avail = len;
    memcpy(dst, src + off, avail);
    return (ssize_t)avail;
}

static ssize_t sysfs_read(vnode_t *v, void *buf, size_t len, uint64_t off) {
    if (!v || !v->fs_data) return -EIO;
    sysfs_node_t *sn = (sysfs_node_t *)v->fs_data;
    char tmp[512];
    tmp[0] = '\0';

    if (sn->type == SYS_FILE_PCI_UEVENT) {
        pci_device_t *devs = NULL;
        int n = pci_get_devices(&devs);
        if (sn->idx < 0 || sn->idx >= n) return -ENOENT;
        pci_device_t *d = &devs[sn->idx];
        uint16_t sub_vendor = pci_read16(d->bus, d->dev, d->fn, 0x2c);
        uint16_t sub_device = pci_read16(d->bus, d->dev, d->fn, 0x2e);
        uint32_t class_code = ((uint32_t)d->class << 16) | ((uint32_t)d->subclass << 8) | d->prog_if;
        ksnprintf(tmp, sizeof(tmp),
                  "PCI_CLASS=%06x\n"
                  "PCI_ID=%04x:%04x\n"
                  "PCI_SUBSYS_ID=%04x:%04x\n"
                  "PCI_SLOT_NAME=%02x:%02x.%u\n"
                  "DRIVER=exo-pci\n",
                  class_code,
                  d->vendor_id, d->device_id,
                  sub_vendor, sub_device,
                  d->bus, d->dev, d->fn);
        return sysfs_emit_text((char *)buf, len, tmp, off, len);
    }

    if (sn->type == SYS_FILE_USB_UEVENT) {
        const usb_device_t *u = usb_get_device(sn->idx);
        if (!u) return -ENOENT;
        ksnprintf(tmp, sizeof(tmp),
                  "PRODUCT=%04x/%04x/0001\n"
                  "BUSNUM=%03u\n"
                  "DEVNUM=%03u\n",
                  u->dev_desc.idVendor, u->dev_desc.idProduct,
                  1u,
                  (unsigned)u->addr);
        return sysfs_emit_text((char *)buf, len, tmp, off, len);
    }

    if (sn->type == SYS_FILE_USB_MFR) {
        const usb_device_t *u = usb_get_device(sn->idx);
        if (!u) return -ENOENT;
        (void)u;
        ksnprintf(tmp, sizeof(tmp), "EXO_OS\n");
        return sysfs_emit_text((char *)buf, len, tmp, off, len);
    }

    if (sn->type == SYS_FILE_USB_PROD) {
        const usb_device_t *u = usb_get_device(sn->idx);
        if (!u) return -ENOENT;
        ksnprintf(tmp, sizeof(tmp), "USB Device %04x:%04x\n", u->dev_desc.idVendor, u->dev_desc.idProduct);
        return sysfs_emit_text((char *)buf, len, tmp, off, len);
    }

    if (sn->type == SYS_FILE_BLK_SIZE) {
        blkdev_t *d = blkdev_get_nth(sn->idx);
        if (!d) return -ENOENT;
        ksnprintf(tmp, sizeof(tmp), "%llu\n", (unsigned long long)d->block_count);
        return sysfs_emit_text((char *)buf, len, tmp, off, len);
    }

    if (sn->type == SYS_FILE_BLK_DEV) {
        ksnprintf(tmp, sizeof(tmp), "254:%d\n", sn->idx);
        return sysfs_emit_text((char *)buf, len, tmp, off, len);
    }

    return -EIO;
}

static ssize_t sysfs_write(vnode_t *v, const void *buf, size_t len, uint64_t off) {
    (void)v; (void)buf; (void)len; (void)off;
    return -EACCES;
}

static int sysfs_readdir(vnode_t *dir, uint64_t *cookie, vfs_dirent_t *out) {
    if (!dir || !dir->fs_data) return -1;
    sysfs_node_t *sn = (sysfs_node_t *)dir->fs_data;
    uint64_t idx = *cookie;

    if (sn->type == SYS_ROOT) {
        static const char *ents[] = { "bus", "block", "devices" };
        if (idx >= 3) return 0;
        out->ino = 10 + idx;
        out->type = VFS_DT_DIR;
        strncpy(out->name, ents[idx], VFS_NAME_MAX);
        out->name[VFS_NAME_MAX] = '\0';
        *cookie = idx + 1;
        return 1;
    }

    if (sn->type == SYS_BUS_DIR) {
        static const char *ents[] = { "pci", "usb" };
        if (idx >= 2) return 0;
        out->ino = 20 + idx;
        out->type = VFS_DT_DIR;
        strncpy(out->name, ents[idx], VFS_NAME_MAX);
        out->name[VFS_NAME_MAX] = '\0';
        *cookie = idx + 1;
        return 1;
    }

    if (sn->type == SYS_BUS_PCI_DIR || sn->type == SYS_BUS_USB_DIR) {
        if (idx > 0) return 0;
        out->ino = 30;
        out->type = VFS_DT_DIR;
        strncpy(out->name, "devices", VFS_NAME_MAX);
        out->name[VFS_NAME_MAX] = '\0';
        *cookie = 1;
        return 1;
    }

    if (sn->type == SYS_PCI_DEVICES_DIR) {
        pci_device_t *devs = NULL;
        int n = pci_get_devices(&devs);
        if ((int)idx >= n) return 0;
        char dname[24];
        if (pci_name_from_index((int)idx, dname, sizeof(dname)) < 0) return 0;
        out->ino = 100 + idx;
        out->type = VFS_DT_LNK;
        strncpy(out->name, dname, VFS_NAME_MAX);
        out->name[VFS_NAME_MAX] = '\0';
        *cookie = idx + 1;
        return 1;
    }

    if (sn->type == SYS_USB_DEVICES_DIR) {
        int n = usb_device_count();
        if ((int)idx >= n) return 0;
        char dname[16];
        if (usb_name_from_index((int)idx, dname, sizeof(dname)) < 0) return 0;
        out->ino = 200 + idx;
        out->type = VFS_DT_LNK;
        strncpy(out->name, dname, VFS_NAME_MAX);
        out->name[VFS_NAME_MAX] = '\0';
        *cookie = idx + 1;
        return 1;
    }

    if (sn->type == SYS_DEVICES_DIR) {
        static const char *ents[] = { "pci", "usb" };
        if (idx >= 2) return 0;
        out->ino = 220 + idx;
        out->type = VFS_DT_DIR;
        strncpy(out->name, ents[idx], VFS_NAME_MAX);
        out->name[VFS_NAME_MAX] = '\0';
        *cookie = idx + 1;
        return 1;
    }

    if (sn->type == SYS_DEVICES_PCI_DIR) {
        pci_device_t *devs = NULL;
        int n = pci_get_devices(&devs);
        if ((int)idx >= n) return 0;
        char dname[24];
        if (pci_name_from_index((int)idx, dname, sizeof(dname)) < 0) return 0;
        out->ino = 240 + idx;
        out->type = VFS_DT_DIR;
        strncpy(out->name, dname, VFS_NAME_MAX);
        out->name[VFS_NAME_MAX] = '\0';
        *cookie = idx + 1;
        return 1;
    }

    if (sn->type == SYS_DEVICES_USB_DIR) {
        int n = usb_device_count();
        if ((int)idx >= n) return 0;
        char dname[16];
        if (usb_name_from_index((int)idx, dname, sizeof(dname)) < 0) return 0;
        out->ino = 260 + idx;
        out->type = VFS_DT_DIR;
        strncpy(out->name, dname, VFS_NAME_MAX);
        out->name[VFS_NAME_MAX] = '\0';
        *cookie = idx + 1;
        return 1;
    }

    if (sn->type == SYS_PCI_DEV_DIR) {
        if (idx > 0) return 0;
        out->ino = 300 + sn->idx;
        out->type = VFS_DT_REG;
        strncpy(out->name, "uevent", VFS_NAME_MAX);
        out->name[VFS_NAME_MAX] = '\0';
        *cookie = 1;
        return 1;
    }

    if (sn->type == SYS_USB_DEV_DIR) {
        static const char *ents[] = { "uevent", "manufacturer", "product" };
        if (idx >= 3) return 0;
        out->ino = 400 + idx + (uint64_t)sn->idx * 8;
        out->type = VFS_DT_REG;
        strncpy(out->name, ents[idx], VFS_NAME_MAX);
        out->name[VFS_NAME_MAX] = '\0';
        *cookie = idx + 1;
        return 1;
    }

    if (sn->type == SYS_BLOCK_DIR) {
        int n = blkdev_count();
        if ((int)idx >= n) return 0;
        char dname[32];
        if (blk_name_from_index((int)idx, dname, sizeof(dname)) < 0) return 0;
        out->ino = 500 + idx;
        out->type = VFS_DT_DIR;
        strncpy(out->name, dname, VFS_NAME_MAX);
        out->name[VFS_NAME_MAX] = '\0';
        *cookie = idx + 1;
        return 1;
    }

    if (sn->type == SYS_BLOCK_DEV_DIR) {
        static const char *ents[] = { "size", "dev" };
        if (idx >= 2) return 0;
        out->ino = 600 + idx + (uint64_t)sn->idx * 8;
        out->type = VFS_DT_REG;
        strncpy(out->name, ents[idx], VFS_NAME_MAX);
        out->name[VFS_NAME_MAX] = '\0';
        *cookie = idx + 1;
        return 1;
    }

    return 0;
}

static int sysfs_stat(vnode_t *v, vfs_stat_t *st) {
    if (!st) return 0;
    memset(st, 0, sizeof(*st));
    sysfs_node_t *sn = (sysfs_node_t *)v->fs_data;
    st->mode = v->mode;
    st->ino = v->ino;
    if (sn && (sn->type == SYS_FILE_PCI_UEVENT ||
               sn->type == SYS_FILE_USB_UEVENT ||
               sn->type == SYS_FILE_USB_MFR ||
               sn->type == SYS_FILE_USB_PROD ||
               sn->type == SYS_FILE_BLK_SIZE ||
               sn->type == SYS_FILE_BLK_DEV)) {
        st->size = 256;
    } else if (sn && (sn->type == SYS_PCI_DEV_LINK || sn->type == SYS_USB_DEV_LINK)) {
        st->size = 64;
    } else {
        st->size = 0;
    }
    st->blksize = 4096;
    return 0;
}

static int sysfs_readlink(vnode_t *v, char *buf, size_t bufsize) {
    if (!v || !v->fs_data || !buf || bufsize < 2) return -EINVAL;
    sysfs_node_t *sn = (sysfs_node_t *)v->fs_data;

    char name[24];
    int n = -1;
    if (sn->type == SYS_PCI_DEV_LINK) {
        if (pci_name_from_index(sn->idx, name, sizeof(name)) < 0) return -ENOENT;
        n = ksnprintf(buf, bufsize, "/sys/devices/pci/%s", name);
    } else if (sn->type == SYS_USB_DEV_LINK) {
        char uname[16];
        if (usb_name_from_index(sn->idx, uname, sizeof(uname)) < 0) return -ENOENT;
        n = ksnprintf(buf, bufsize, "/sys/devices/usb/%s", uname);
    } else {
        return -EINVAL;
    }
    if (n < 0 || (size_t)n >= bufsize) return -ERANGE;
    return 0;
}

static void sysfs_evict(vnode_t *v) {
    if (v && v->fs_data) {
        kfree(v->fs_data);
        v->fs_data = NULL;
    }
}

static vnode_t *sysfs_mount(fs_inst_t *fsi, blkdev_t *dev) {
    (void)dev;
    sysfs_fsi = fsi;
    sysfs_root = sysfs_alloc_node(SYS_ROOT, 0, VFS_S_IFDIR | 0555);
    if (!sysfs_root) return NULL;
    KLOG_INFO("sysfs: mounted at /sys\n");
    return sysfs_root;
}

static void sysfs_unmount(fs_inst_t *fsi) {
    (void)fsi;
    if (sysfs_root) {
        if (sysfs_root->fs_data) kfree(sysfs_root->fs_data);
        kfree(sysfs_root);
        sysfs_root = NULL;
    }
}

static fs_ops_t sysfs_ops = {
    .name = "sysfs",
    .lookup = sysfs_lookup,
    .open = sysfs_open,
    .close = sysfs_close,
    .read = sysfs_read,
    .write = sysfs_write,
    .readdir = sysfs_readdir,
    .create = NULL,
    .mkdir = NULL,
    .unlink = NULL,
    .rmdir = NULL,
    .rename = NULL,
    .stat = sysfs_stat,
    .symlink = NULL,
    .readlink = sysfs_readlink,
    .truncate = NULL,
    .sync = NULL,
    .evict = sysfs_evict,
    .mount = sysfs_mount,
    .unmount = sysfs_unmount,
};

void sysfs_init(void) {
    vfs_register_fs(&sysfs_ops);
    if (vfs_mount("/sys", NULL, "sysfs") == 0) {
        KLOG_INFO("sysfs: /sys mounted successfully\n");
    } else {
        KLOG_WARN("sysfs: failed to mount /sys\n");
    }
}
