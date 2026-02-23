#pragma once
#include <stdint.h>
#include <stddef.h>

/* ACPI SDT header (common to all tables) */
typedef struct {
    char     signature[4];
    uint32_t length;
    uint8_t  revision;
    uint8_t  checksum;
    char     oem_id[6];
    char     oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} __attribute__((packed)) acpi_sdt_hdr_t;

/* RSDP (Root System Descriptor Pointer) */
typedef struct {
    char     signature[8];
    uint8_t  checksum;
    char     oem_id[6];
    uint8_t  revision;
    uint32_t rsdt_addr;
    uint32_t length;
    uint64_t xsdt_addr;
    uint8_t  ext_checksum;
    uint8_t  reserved[3];
} __attribute__((packed)) rsdp_t;

/* MADT header */
typedef struct {
    acpi_sdt_hdr_t hdr;
    uint32_t       lapic_addr;
    uint32_t       flags;
} __attribute__((packed)) madt_t;

/* MADT record types */
#define MADT_LAPIC         0
#define MADT_IOAPIC        1
#define MADT_ISO           2
#define MADT_NMI           4
#define MADT_LAPIC_OVERRIDE 5

typedef struct {
    uint8_t type;
    uint8_t len;
} __attribute__((packed)) madt_record_t;

typedef struct {
    madt_record_t hdr;
    uint8_t  acpi_id;
    uint8_t  apic_id;
    uint32_t flags;   /* bit 0 = enabled */
} __attribute__((packed)) madt_lapic_t;

typedef struct {
    madt_record_t hdr;
    uint8_t  ioapic_id;
    uint8_t  reserved;
    uint32_t ioapic_addr;
    uint32_t gsi_base;
} __attribute__((packed)) madt_ioapic_t;

typedef struct {
    madt_record_t hdr;
    uint8_t  reserved[8];
    uint64_t lapic_phys_addr;
} __attribute__((packed)) madt_lapic_override_t;

/* Results of MADT parse */
#define MAX_CPUS 256
typedef struct {
    uintptr_t lapic_base;
    uint32_t  cpu_count;
    uint8_t   lapic_ids[MAX_CPUS];
    uint8_t   acpi_ids[MAX_CPUS];
} madt_info_t;

void       acpi_init(uintptr_t rsdp_addr);
madt_info_t *acpi_get_madt_info(void);
