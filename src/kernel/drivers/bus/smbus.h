/* drivers/bus/smbus.h — ICH9 SMBus controller driver
 *
 * Drives the Intel ICH9 SMBus host controller (PCI 8086:2930, class 0C/05).
 * Provides byte/word read/write over the SMBus, registers itself as an I2C
 * bus ("smbus0"), and optionally probes DDR SPD EEPROMs at 0x50-0x57.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>

/* SPD EEPROM information (partial, first few bytes) */
typedef struct {
    bool    present;
    uint8_t type;       /* SPD byte 2: DRAM type (11=DDR3, 12=DDR4, 18=DDR5) */
    uint8_t module_type;/* SPD byte 3 */
    uint8_t density;    /* SPD byte 4 */
    uint8_t ranks;      /* SPD byte 12 (DDR3/4) or 234 (DDR5) */
} spd_info_t;

#define SPD_MAX_DIMMS 8

/* Initialise the ICH9 SMBus controller.  Returns true if found and ready. */
bool smbus_init(void);

/* Read a byte from register `cmd` on device at 7-bit address `addr`.
 * Returns the byte value on success, -1 on error. */
int smbus_read_byte(uint8_t addr, uint8_t cmd);

/* Write a byte to register `cmd` on device at 7-bit address `addr`.
 * Returns 0 on success, -1 on error. */
int smbus_write_byte(uint8_t addr, uint8_t cmd, uint8_t val);

/* Read a word from register `cmd` on device at 7-bit address `addr`.
 * Returns the 16-bit word on success, -1 on error. */
int smbus_read_word(uint8_t addr, uint8_t cmd);

/* Probe for SPD EEPROMs at 0x50-0x57.  Fills info[], returns count found. */
int smbus_probe_spd(spd_info_t *info, int max);

/* Check if SMBus was initialised successfully */
bool smbus_is_available(void);
