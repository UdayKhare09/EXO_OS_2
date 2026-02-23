#include "acpi.h"
#include "lib/klog.h"
#include "lib/panic.h"
#include "lib/string.h"
#include "mm/vmm.h"
#include <stdint.h>
#include <stdbool.h>

#define ACPI_PAGE_SIZE 4096ULL

static madt_info_t madt_info;

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
        madt = (madt_t *)acpi_find_table_xsdt(root, "APIC");
    } else {
        root = acpi_map_sdt((uintptr_t)rsdp->rsdt_addr);
        if (memcmp(root->signature, "RSDT", 4) != 0)
            kpanic("ACPI: RSDT signature mismatch\n");
        if (!acpi_checksum(root, root->length))
            KLOG_WARN("ACPI: RSDT checksum failure (proceeding anyway)\n");
        KLOG_INFO("ACPI: RSDT at %p rev=%u\n", (void *)root, rsdp->revision);
        madt = (madt_t *)acpi_find_table_rsdt(root, "APIC");
    }

    if (!madt) kpanic("ACPI: MADT (APIC) table not found\n");

    parse_madt(madt);
}

madt_info_t *acpi_get_madt_info(void) {
    return &madt_info;
}
