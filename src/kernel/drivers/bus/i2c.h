/* drivers/bus/i2c.h — Generic I2C bus interface
 *
 * Provides a minimal abstraction so different I2C/SMBus controllers
 * can register themselves and higher-level drivers can probe buses
 * without knowing the underlying transport.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>

#define I2C_MAX_BUSES 4
#define I2C_BUS_NAME_MAX 16

typedef struct i2c_bus i2c_bus_t;

typedef struct i2c_ops {
    /* Read a single byte from register `reg` at device address `addr` (7-bit).
     * Returns the byte value, or -1 on error. */
    int (*read_byte)(i2c_bus_t *bus, uint8_t addr, uint8_t reg);

    /* Write a single byte to register `reg` at device address `addr` (7-bit).
     * Returns 0 on success, -1 on error. */
    int (*write_byte)(i2c_bus_t *bus, uint8_t addr, uint8_t reg, uint8_t val);

    /* Read a 16-bit word (SMBus "read word data") from register `reg`.
     * Returns the word value, or -1 on error. */
    int (*read_word)(i2c_bus_t *bus, uint8_t addr, uint8_t reg);

    /* Quick probe: check if a device ACKs at `addr`.  Returns true if present. */
    bool (*probe)(i2c_bus_t *bus, uint8_t addr);
} i2c_ops_t;

struct i2c_bus {
    char       name[I2C_BUS_NAME_MAX];
    i2c_ops_t *ops;
    void      *priv;   /* controller-private data */
};

/* Register an I2C bus.  Returns 0 on success, -1 if full. */
int i2c_register_bus(i2c_bus_t *bus);

/* Return the nth registered bus (0-based).  NULL if out of bounds. */
i2c_bus_t *i2c_get_bus(int n);

/* Return the named bus.  NULL if not found. */
i2c_bus_t *i2c_get_bus_by_name(const char *name);

/* Return the total number of registered buses. */
int i2c_bus_count(void);
