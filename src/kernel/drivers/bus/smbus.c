/* drivers/bus/smbus.c — ICH9 SMBus host controller driver
 *
 * Programs the Intel ICH9/ICH10 SMBus controller found at PCI device
 * 8086:2930 (class 0C/05) on the Q35 chipset used by QEMU.  The SMBus
 * host interface is accessed via I/O ports at BAR4 (typically 0x0700).
 *
 * Register map (offsets from SMBBASE):
 *   +0x00  HSTS   Host Status
 *   +0x02  HCNT   Host Control
 *   +0x03  HCMD   Host Command
 *   +0x04  XMIT   Transmit Slave Address
 *   +0x05  HDAT0  Host Data 0
 *   +0x06  HDAT1  Host Data 1
 *   +0x07  BLKDA  Block Data
 */
#include "smbus.h"
#include "i2c.h"
#include "arch/x86_64/pci.h"
#include "arch/x86_64/cpu.h"
#include "lib/klog.h"
#include "lib/string.h"
#include <stdint.h>
#include <stdbool.h>

/* ICH9 SMBus PCI IDs */
#define SMBUS_VID  0x8086
#define SMBUS_DID  0x2930

/* SMBus host register offsets from SMBBASE */
#define SMB_HSTS   0x00   /* Host Status                       */
#define SMB_HCNT   0x02   /* Host Control                      */
#define SMB_HCMD   0x03   /* Host Command (register/command)   */
#define SMB_XMIT   0x04   /* Transmit Slave Address            */
#define SMB_HDAT0  0x05   /* Host Data 0                       */
#define SMB_HDAT1  0x06   /* Host Data 1                       */
#define SMB_BLKDA  0x07   /* Block Data                        */

/* HSTS bits */
#define HSTS_BUSY     (1 << 0)
#define HSTS_INTR     (1 << 1)
#define HSTS_DERR     (1 << 2)
#define HSTS_BERR     (1 << 3)
#define HSTS_FAILED   (1 << 4)

/* HCNT protocol bits [4:2] */
#define HCNT_START    (1 << 6)
#define HCNT_QUICK    (0x00 << 2)
#define HCNT_BYTE     (0x01 << 2)
#define HCNT_BYTE_D   (0x02 << 2)   /* Byte Data */
#define HCNT_WORD_D   (0x03 << 2)   /* Word Data */
#define HCNT_BLOCK    (0x05 << 2)

/* Module state */
static uint16_t g_smbbase = 0;
static bool     g_smbus_ready = false;

/* I2C bus registration */
static i2c_bus_t g_smbus_i2c;
static int g_i2c_buses_count = 0;
static i2c_bus_t *g_i2c_buses[I2C_MAX_BUSES];

int i2c_register_bus(i2c_bus_t *bus) {
    if (g_i2c_buses_count >= I2C_MAX_BUSES) return -1;
    g_i2c_buses[g_i2c_buses_count++] = bus;
    KLOG_INFO("i2c: registered bus \"%s\"\n", bus->name);
    return 0;
}

i2c_bus_t *i2c_get_bus(int n) {
    if (n < 0 || n >= g_i2c_buses_count) return NULL;
    return g_i2c_buses[n];
}

i2c_bus_t *i2c_get_bus_by_name(const char *name) {
    for (int i = 0; i < g_i2c_buses_count; i++) {
        if (strcmp(g_i2c_buses[i]->name, name) == 0)
            return g_i2c_buses[i];
    }
    return NULL;
}

int i2c_bus_count(void) { return g_i2c_buses_count; }

/* ── Low-level SMBus host I/O ────────────────────────────────────────────── */
static inline uint8_t smb_inb(uint16_t reg) {
    return inb(g_smbbase + reg);
}

static inline void smb_outb(uint16_t reg, uint8_t val) {
    outb(g_smbbase + reg, val);
}

/* Clear all status bits by writing 1s */
static void smb_clear_status(void) {
    smb_outb(SMB_HSTS, 0xFF);
}

/* Wait for transaction to complete.  Returns 0 on success, -1 on error. */
static int smb_wait(void) {
    for (int i = 0; i < 100000; i++) {
        uint8_t sts = smb_inb(SMB_HSTS);
        if (sts & (HSTS_DERR | HSTS_BERR | HSTS_FAILED)) {
            smb_clear_status();
            return -1;
        }
        if (sts & HSTS_INTR) {
            smb_clear_status();
            return 0;
        }
        io_wait();
    }
    smb_clear_status();
    return -1;  /* timeout */
}

/* ── SMBus byte/word operations ──────────────────────────────────────────── */
int smbus_read_byte(uint8_t addr, uint8_t cmd) {
    if (!g_smbus_ready) return -1;
    smb_clear_status();
    smb_outb(SMB_XMIT, (addr << 1) | 1);   /* read */
    smb_outb(SMB_HCMD, cmd);
    smb_outb(SMB_HCNT, HCNT_BYTE_D | HCNT_START);
    if (smb_wait() < 0) return -1;
    return smb_inb(SMB_HDAT0);
}

int smbus_write_byte(uint8_t addr, uint8_t cmd, uint8_t val) {
    if (!g_smbus_ready) return -1;
    smb_clear_status();
    smb_outb(SMB_XMIT, (addr << 1) | 0);   /* write */
    smb_outb(SMB_HCMD, cmd);
    smb_outb(SMB_HDAT0, val);
    smb_outb(SMB_HCNT, HCNT_BYTE_D | HCNT_START);
    return smb_wait();
}

int smbus_read_word(uint8_t addr, uint8_t cmd) {
    if (!g_smbus_ready) return -1;
    smb_clear_status();
    smb_outb(SMB_XMIT, (addr << 1) | 1);   /* read */
    smb_outb(SMB_HCMD, cmd);
    smb_outb(SMB_HCNT, HCNT_WORD_D | HCNT_START);
    if (smb_wait() < 0) return -1;
    uint8_t lo = smb_inb(SMB_HDAT0);
    uint8_t hi = smb_inb(SMB_HDAT1);
    return (hi << 8) | lo;
}

bool smbus_is_available(void) { return g_smbus_ready; }

/* ── I2C ops bridge ──────────────────────────────────────────────────────── */
static int i2c_smbus_read_byte(i2c_bus_t *bus, uint8_t addr, uint8_t reg) {
    (void)bus;
    return smbus_read_byte(addr, reg);
}

static int i2c_smbus_write_byte(i2c_bus_t *bus, uint8_t addr, uint8_t reg, uint8_t val) {
    (void)bus;
    return smbus_write_byte(addr, reg, val);
}

static int i2c_smbus_read_word(i2c_bus_t *bus, uint8_t addr, uint8_t reg) {
    (void)bus;
    return smbus_read_word(addr, reg);
}

static bool i2c_smbus_probe(i2c_bus_t *bus, uint8_t addr) {
    (void)bus;
    /* Quick command: just check for ACK */
    if (!g_smbus_ready) return false;
    smb_clear_status();
    smb_outb(SMB_XMIT, (addr << 1) | 1);
    smb_outb(SMB_HCNT, HCNT_QUICK | HCNT_START);
    return smb_wait() == 0;
}

static i2c_ops_t smbus_i2c_ops = {
    .read_byte  = i2c_smbus_read_byte,
    .write_byte = i2c_smbus_write_byte,
    .read_word  = i2c_smbus_read_word,
    .probe      = i2c_smbus_probe,
};

/* ── SPD EEPROM probing ──────────────────────────────────────────────────── */
int smbus_probe_spd(spd_info_t *info, int max) {
    if (!g_smbus_ready) return 0;
    int found = 0;
    for (uint8_t slot = 0; slot < 8 && found < max; slot++) {
        uint8_t addr = 0x50 + slot;
        int byte0 = smbus_read_byte(addr, 0);
        if (byte0 < 0) continue;
        info[found].present = true;
        int byte2 = smbus_read_byte(addr, 2);
        int byte3 = smbus_read_byte(addr, 3);
        int byte4 = smbus_read_byte(addr, 4);
        int byte12 = smbus_read_byte(addr, 12);
        info[found].type        = (byte2 >= 0) ? (uint8_t)byte2 : 0;
        info[found].module_type = (byte3 >= 0) ? (uint8_t)byte3 : 0;
        info[found].density     = (byte4 >= 0) ? (uint8_t)byte4 : 0;
        info[found].ranks       = (byte12 >= 0) ? (uint8_t)byte12 : 0;
        KLOG_INFO("SMBus: SPD at 0x%02x — type=%u module=%u density=%u\n",
                  addr, info[found].type, info[found].module_type, info[found].density);
        found++;
    }
    return found;
}

/* ── Initialisation ──────────────────────────────────────────────────────── */
bool smbus_init(void) {
    pci_device_t *dev = pci_find(SMBUS_VID, SMBUS_DID);
    if (!dev) {
        KLOG_INFO("SMBus: ICH9 controller not found on PCI\n");
        return false;
    }

    KLOG_INFO("SMBus: found ICH9 at %02x:%02x.%x\n", dev->bus, dev->dev, dev->fn);

    /* Enable I/O space + bus master */
    pci_enable_device(dev);

    /* Read SMBus I/O base from BAR4 (PCI config offset 0x20) */
    uint32_t bar4 = pci_read32(dev->bus, dev->dev, dev->fn, 0x20);
    g_smbbase = (uint16_t)(bar4 & 0xFFF0);   /* mask off low bits */

    if (g_smbbase == 0 || g_smbbase == 0xFFF0) {
        KLOG_WARN("SMBus: invalid base address 0x%04x\n", g_smbbase);
        return false;
    }

    /* Enable SMBus host controller via SMBUS host config (PCI offset 0x40) */
    uint8_t hcfg = pci_read8(dev->bus, dev->dev, dev->fn, 0x40);
    hcfg |= (1 << 0);   /* SMBus Host Enable */
    pci_write8(dev->bus, dev->dev, dev->fn, 0x40, hcfg);

    /* Clear initial status */
    smb_clear_status();

    g_smbus_ready = true;
    KLOG_INFO("SMBus: ready at I/O base 0x%04x\n", g_smbbase);

    /* Register as I2C bus */
    strncpy(g_smbus_i2c.name, "smbus0", I2C_BUS_NAME_MAX);
    g_smbus_i2c.ops  = &smbus_i2c_ops;
    g_smbus_i2c.priv = NULL;
    i2c_register_bus(&g_smbus_i2c);

    return true;
}
