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

/* FADT (Fixed ACPI Description Table) */
typedef struct {
    acpi_sdt_hdr_t hdr;
    uint32_t firmware_ctrl;
    uint32_t dsdt;
    uint8_t  reserved0;
    uint8_t  preferred_pm_profile;
    uint16_t sci_int;
    uint32_t smi_cmd;
    uint8_t  acpi_enable;
    uint8_t  acpi_disable;
    uint8_t  s4bios_req;
    uint8_t  pstate_cnt;
    uint32_t pm1a_evt_blk;
    uint32_t pm1b_evt_blk;
    uint32_t pm1a_cnt_blk;
    uint32_t pm1b_cnt_blk;
    uint32_t pm2_cnt_blk;
    uint32_t pm_tmr_blk;
    uint32_t gpe0_blk;
    uint32_t gpe1_blk;
    uint8_t  pm1_evt_len;
    uint8_t  pm1_cnt_len;
    uint8_t  pm2_cnt_len;
    uint8_t  pm_tmr_len;
    uint8_t  gpe0_blk_len;
    uint8_t  gpe1_blk_len;
    uint8_t  gpe1_base;
    uint8_t  cst_cnt;
    uint16_t p_lvl2_lat;
    uint16_t p_lvl3_lat;
    uint16_t flush_size;
    uint16_t flush_stride;
    uint8_t  duty_offset;
    uint8_t  duty_width;
    uint8_t  day_alarm;
    uint8_t  mon_alarm;
    uint8_t  century;
    uint16_t iapc_boot_arch;
    uint8_t  reserved1;
    uint32_t flags;
    /* Generic Address Structure for reset register */
    uint8_t  reset_reg_space;
    uint8_t  reset_reg_bit_width;
    uint8_t  reset_reg_bit_offset;
    uint8_t  reset_reg_access_size;
    uint64_t reset_reg_addr;
    uint8_t  reset_value;
    uint16_t arm_boot_arch;
    uint8_t  fadt_minor_version;
    uint64_t x_firmware_ctrl;
    uint64_t x_dsdt;
} __attribute__((packed)) acpi_fadt_t;

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
    /* I/O APIC info (first IOAPIC found) */
    uint32_t  ioapic_addr;
    uint32_t  ioapic_gsi_base;
    int       ioapic_found;
} madt_info_t;

/* ACPI table entry for list_tables */
#define ACPI_MAX_TABLES 64
typedef struct {
    char     sig[5];       /* 4-char signature + NUL */
    uint32_t length;
    uint8_t  revision;
} acpi_table_entry_t;

void         acpi_init(uintptr_t rsdp_addr);
madt_info_t *acpi_get_madt_info(void);

/* Find an ACPI table by 4-char signature. Returns mapped virtual address.
 * idx selects among multiple tables with the same sig (0 = first). */
acpi_sdt_hdr_t *acpi_find_table(const char *sig, int idx);

/* List all ACPI tables found in XSDT/RSDT.
 * Fills entries[], returns count. */
int acpi_list_tables(acpi_table_entry_t *entries, int max);

/* Shutdown: enter ACPI S5 state (powers off machine) */
void acpi_shutdown(void);

/* Reboot: FADT reset register → KBC fallback → triple fault */
void acpi_reboot(void);
