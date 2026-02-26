#include "acpi.h"
#include "cpu.h"
#include "lib/klog.h"
#include "lib/panic.h"
#include "lib/string.h"
#include "mm/vmm.h"
#include <stdint.h>
#include <stdbool.h>

#define ACPI_PAGE_SIZE 4096ULL

static madt_info_t madt_info;

/* Cached root table pointer and whether it's XSDT or RSDT */
static acpi_sdt_hdr_t *g_root_sdt  = NULL;
static int              g_use_xsdt  = 0;

/* Cached FADT pointer for shutdown/reboot */
static acpi_fadt_t     *g_fadt      = NULL;

static inline uintptr_t acpi_align_down(uintptr_t x) {
    return x & ~(ACPI_PAGE_SIZE - 1);
}

static inline uintptr_t acpi_align_up(uintptr_t x) {
    return (x + ACPI_PAGE_SIZE - 1) & ~(ACPI_PAGE_SIZE - 1);
}

static void acpi_map_phys_range(uintptr_t phys, size_t len) {
    uintptr_t start = acpi_align_down(phys);
    uintptr_t end = acpi_align_up(phys + len);

    for (uintptr_t p = start; p < end; p += ACPI_PAGE_SIZE) {
        vmm_map_page(vmm_phys_to_virt(p), p, VMM_KERNEL_RW);
    }
}

static acpi_sdt_hdr_t *acpi_map_sdt(uintptr_t phys_addr) {
    acpi_map_phys_range(phys_addr, sizeof(acpi_sdt_hdr_t));
    acpi_sdt_hdr_t *hdr = (acpi_sdt_hdr_t *)vmm_phys_to_virt(phys_addr);
    if (hdr->length < sizeof(acpi_sdt_hdr_t))
        kpanic("ACPI: malformed SDT length\n");
    acpi_map_phys_range(phys_addr, hdr->length);
    return hdr;
}

static bool acpi_checksum(const void *table, size_t len) {
    uint8_t sum = 0;
    const uint8_t *p = (const uint8_t *)table;
    for (size_t i = 0; i < len; i++) sum += p[i];
    return sum == 0;
}

static acpi_sdt_hdr_t *acpi_find_table_xsdt(acpi_sdt_hdr_t *xsdt, const char *sig) {
    uint32_t entries = (xsdt->length - sizeof(*xsdt)) / 8;
    uint64_t *ptrs   = (uint64_t *)((uintptr_t)xsdt + sizeof(*xsdt));
    for (uint32_t i = 0; i < entries; i++) {
        acpi_sdt_hdr_t *tbl = acpi_map_sdt((uintptr_t)ptrs[i]);
        if (memcmp(tbl->signature, sig, 4) == 0) return tbl;
    }
    return NULL;
}

static acpi_sdt_hdr_t *acpi_find_table_rsdt(acpi_sdt_hdr_t *rsdt, const char *sig) {
    uint32_t entries = (rsdt->length - sizeof(*rsdt)) / 4;
    uint32_t *ptrs   = (uint32_t *)((uintptr_t)rsdt + sizeof(*rsdt));
    for (uint32_t i = 0; i < entries; i++) {
        acpi_sdt_hdr_t *tbl = acpi_map_sdt((uintptr_t)ptrs[i]);
        if (memcmp(tbl->signature, sig, 4) == 0) return tbl;
    }
    return NULL;
}

static void parse_madt(madt_t *madt) {
    madt_info.lapic_base = madt->lapic_addr;
    madt_info.cpu_count  = 0;

    uint8_t *ptr = (uint8_t *)madt + sizeof(madt_t);
    uint8_t *end = (uint8_t *)madt + madt->hdr.length;

    while (ptr < end) {
        madt_record_t *rec = (madt_record_t *)ptr;
        if (rec->type == MADT_LAPIC) {
            madt_lapic_t *lapic = (madt_lapic_t *)rec;
            if ((lapic->flags & 1) && madt_info.cpu_count < MAX_CPUS) {
                uint32_t idx = madt_info.cpu_count++;
                madt_info.lapic_ids[idx] = lapic->apic_id;
                madt_info.acpi_ids[idx]  = lapic->acpi_id;
                KLOG_INFO("MADT: CPU%u apic_id=%u acpi_id=%u\n",
                          idx, lapic->apic_id, lapic->acpi_id);
            }
        } else if (rec->type == MADT_LAPIC_OVERRIDE) {
            madt_lapic_override_t *ov = (madt_lapic_override_t *)rec;
            madt_info.lapic_base = (uintptr_t)ov->lapic_phys_addr;
            KLOG_INFO("MADT: LAPIC base override -> %p\n",
                      (void *)madt_info.lapic_base);
        } else if (rec->type == MADT_IOAPIC) {
            madt_ioapic_t *io = (madt_ioapic_t *)rec;
            KLOG_INFO("MADT: I/O APIC id=%u base=%p gsi=%u\n",
                      io->ioapic_id, (void*)(uintptr_t)io->ioapic_addr,
                      io->gsi_base);
            /* Store first IOAPIC for the kernel to programme */
            if (!madt_info.ioapic_found) {
                madt_info.ioapic_addr     = io->ioapic_addr;
                madt_info.ioapic_gsi_base = io->gsi_base;
                madt_info.ioapic_found    = 1;
            }
        }
        if (rec->len == 0) break; /* guard */
        ptr += rec->len;
    }

    KLOG_INFO("MADT: %u CPU(s), LAPIC base=%p\n",
              madt_info.cpu_count, (void *)madt_info.lapic_base);
}

void acpi_init(uintptr_t rsdp_addr) {
    rsdp_t *rsdp;

    if (rsdp_addr < (1ULL << 32)) {
        acpi_map_phys_range(rsdp_addr, sizeof(rsdp_t));
        rsdp = (rsdp_t *)vmm_phys_to_virt(rsdp_addr);
    } else {
        rsdp = (rsdp_t *)rsdp_addr;
    }

    if (memcmp(rsdp->signature, "RSD PTR ", 8) != 0)
        kpanic("ACPI: Invalid RSDP signature\n");

    acpi_sdt_hdr_t *root = NULL;
    madt_t *madt = NULL;

    if (rsdp->revision >= 2 && rsdp->xsdt_addr != 0) {
        root = acpi_map_sdt((uintptr_t)rsdp->xsdt_addr);
        if (memcmp(root->signature, "XSDT", 4) != 0)
            kpanic("ACPI: XSDT signature mismatch\n");
        if (!acpi_checksum(root, root->length))
            KLOG_WARN("ACPI: XSDT checksum failure (proceeding anyway)\n");
        KLOG_INFO("ACPI: XSDT at %p rev=%u\n", (void *)root, rsdp->revision);
        g_root_sdt  = root;
        g_use_xsdt  = 1;
        madt = (madt_t *)acpi_find_table_xsdt(root, "APIC");
    } else {
        root = acpi_map_sdt((uintptr_t)rsdp->rsdt_addr);
        if (memcmp(root->signature, "RSDT", 4) != 0)
            kpanic("ACPI: RSDT signature mismatch\n");
        if (!acpi_checksum(root, root->length))
            KLOG_WARN("ACPI: RSDT checksum failure (proceeding anyway)\n");
        KLOG_INFO("ACPI: RSDT at %p rev=%u\n", (void *)root, rsdp->revision);
        g_root_sdt  = root;
        g_use_xsdt  = 0;
        madt = (madt_t *)acpi_find_table_rsdt(root, "APIC");
    }

    if (!madt) kpanic("ACPI: MADT (APIC) table not found\n");

    parse_madt(madt);

    /* Cache the FADT for shutdown/reboot */
    g_fadt = (acpi_fadt_t *)acpi_find_table("FACP", 0);
    if (g_fadt)
        KLOG_INFO("ACPI: FADT at %p  PM1a_CNT=%x  reset_reg=%llx\n",
                  (void *)g_fadt, g_fadt->pm1a_cnt_blk,
                  (unsigned long long)g_fadt->reset_reg_addr);
    else
        KLOG_WARN("ACPI: FADT not found — shutdown/reboot unavailable\n");
}

madt_info_t *acpi_get_madt_info(void) {
    return &madt_info;
}

/* ── acpi_find_table — generic table lookup by signature ─────────────────── */
acpi_sdt_hdr_t *acpi_find_table(const char *sig, int idx) {
    if (!g_root_sdt) return NULL;

    int found = 0;
    if (g_use_xsdt) {
        uint32_t entries = (g_root_sdt->length - sizeof(*g_root_sdt)) / 8;
        uint64_t *ptrs   = (uint64_t *)((uintptr_t)g_root_sdt + sizeof(*g_root_sdt));
        for (uint32_t i = 0; i < entries; i++) {
            acpi_sdt_hdr_t *tbl = acpi_map_sdt((uintptr_t)ptrs[i]);
            if (memcmp(tbl->signature, sig, 4) == 0) {
                if (found == idx) return tbl;
                found++;
            }
        }
    } else {
        uint32_t entries = (g_root_sdt->length - sizeof(*g_root_sdt)) / 4;
        uint32_t *ptrs   = (uint32_t *)((uintptr_t)g_root_sdt + sizeof(*g_root_sdt));
        for (uint32_t i = 0; i < entries; i++) {
            acpi_sdt_hdr_t *tbl = acpi_map_sdt((uintptr_t)ptrs[i]);
            if (memcmp(tbl->signature, sig, 4) == 0) {
                if (found == idx) return tbl;
                found++;
            }
        }
    }
    return NULL;
}

/* ── acpi_list_tables — enumerate all ACPI tables ────────────────────────── */
int acpi_list_tables(acpi_table_entry_t *entries, int max) {
    if (!g_root_sdt) return 0;
    int count = 0;

    uint32_t n_entries;
    if (g_use_xsdt)
        n_entries = (g_root_sdt->length - sizeof(*g_root_sdt)) / 8;
    else
        n_entries = (g_root_sdt->length - sizeof(*g_root_sdt)) / 4;

    for (uint32_t i = 0; i < n_entries && count < max; i++) {
        uintptr_t phys;
        if (g_use_xsdt) {
            uint64_t *ptrs = (uint64_t *)((uintptr_t)g_root_sdt + sizeof(*g_root_sdt));
            phys = (uintptr_t)ptrs[i];
        } else {
            uint32_t *ptrs = (uint32_t *)((uintptr_t)g_root_sdt + sizeof(*g_root_sdt));
            phys = (uintptr_t)ptrs[i];
        }
        acpi_sdt_hdr_t *tbl = acpi_map_sdt(phys);
        memcpy(entries[count].sig, tbl->signature, 4);
        entries[count].sig[4]    = '\0';
        entries[count].length    = tbl->length;
        entries[count].revision  = tbl->revision;
        count++;
    }
    return count;
}

/* ── acpi_shutdown — enter S5 (soft off) ─────────────────────────────────── */
void acpi_shutdown(void) {
    if (!g_fadt) {
        KLOG_ERR("ACPI: no FADT — cannot shutdown\n");
        return;
    }

    /* Find the \_S5 object in the DSDT to get SLP_TYPa/b values.
     * Without a full AML interpreter we search the DSDT bytecode for
     * the _S5_ name followed by a package definition.  This is the same
     * approach used by many hobby OS kernels. */
    uintptr_t dsdt_phys = g_fadt->dsdt;
    if (g_fadt->hdr.length >= 148 && g_fadt->x_dsdt)
        dsdt_phys = (uintptr_t)g_fadt->x_dsdt;

    acpi_sdt_hdr_t *dsdt = acpi_map_sdt(dsdt_phys);
    uint8_t *aml     = (uint8_t *)dsdt;
    uint32_t aml_len = dsdt->length;

    uint16_t slp_typa = 0, slp_typb = 0;
    bool found_s5 = false;

    /* Scan for _S5_ (0x5F 0x53 0x35 0x5F) followed by package opcode */
    for (uint32_t i = 0; i + 4 < aml_len; i++) {
        if (aml[i] == '_' && aml[i+1] == 'S' && aml[i+2] == '5' && aml[i+3] == '_') {
            /* Skip name, look for package op (0x12) within next 20 bytes */
            for (int j = 4; j < 20 && (i + j) < aml_len; j++) {
                if (aml[i + j] == 0x12) {
                    /* Package: skip PkgLength byte(s), NumElements */
                    int k = j + 1;
                    /* decode PkgLength lead byte */
                    uint8_t lead = aml[i + k];
                    int pkg_len_bytes = (lead >> 6) ? ((lead >> 6) + 1) : 1;
                    k += pkg_len_bytes;
                    k++; /* NumElements */
                    /* First element = SLP_TYPa */
                    if (aml[i + k] == 0x0A) { k++; slp_typa = aml[i + k]; k++; }
                    else { slp_typa = aml[i + k]; k++; }
                    /* Second element = SLP_TYPb */
                    if (aml[i + k] == 0x0A) { k++; slp_typb = aml[i + k]; }
                    else { slp_typb = aml[i + k]; }
                    found_s5 = true;
                    break;
                }
            }
            if (found_s5) break;
        }
    }

    if (!found_s5) {
        KLOG_ERR("ACPI: _S5_ object not found in DSDT\n");
        return;
    }

    KLOG_INFO("ACPI: S5 shutdown — SLP_TYPa=%u SLP_TYPb=%u\n", slp_typa, slp_typb);

    /* Write SLP_TYPa | SLP_EN to PM1a_CNT */
    uint16_t val = (slp_typa << 10) | (1 << 13);  /* SLP_TYP bits [12:10], SLP_EN bit 13 */
    outw(g_fadt->pm1a_cnt_blk, val);

    if (g_fadt->pm1b_cnt_blk) {
        val = (slp_typb << 10) | (1 << 13);
        outw(g_fadt->pm1b_cnt_blk, val);
    }

    /* Should not reach here — machine should be off */
    for (;;) cpu_halt();
}

/* ── acpi_reboot — reset via FADT reset register ─────────────────────────── */
void acpi_reboot(void) {
    /* Method 1: FADT reset register */
    if (g_fadt && (g_fadt->flags & (1 << 10)) && g_fadt->reset_reg_addr) {
        KLOG_INFO("ACPI: reboot via FADT reset register (space=%u addr=%llx val=%u)\n",
                  g_fadt->reset_reg_space,
                  (unsigned long long)g_fadt->reset_reg_addr,
                  g_fadt->reset_value);
        if (g_fadt->reset_reg_space == 1) {
            /* System I/O */
            outb((uint16_t)g_fadt->reset_reg_addr, g_fadt->reset_value);
        } else if (g_fadt->reset_reg_space == 0) {
            /* System Memory */
            acpi_map_phys_range((uintptr_t)g_fadt->reset_reg_addr, 1);
            volatile uint8_t *p = (volatile uint8_t *)vmm_phys_to_virt(
                (uintptr_t)g_fadt->reset_reg_addr);
            *p = g_fadt->reset_value;
        }
        /* Wait a bit for reset to take effect */
        for (volatile int i = 0; i < 100000; i++);
    }

    /* Method 2: Keyboard controller reset (pulse CPU reset line) */
    KLOG_INFO("ACPI: reboot via keyboard controller 0xFE\n");
    outb(0x64, 0xFE);
    for (volatile int i = 0; i < 100000; i++);

    /* Method 3: Triple fault — load a null IDT and trigger an interrupt */
    KLOG_INFO("ACPI: reboot via triple fault\n");
    struct { uint16_t limit; uint64_t base; } __attribute__((packed)) null_idt = { 0, 0 };
    __asm__ volatile("lidt %0; int $3" : : "m"(null_idt));

    for (;;) cpu_halt();
}
