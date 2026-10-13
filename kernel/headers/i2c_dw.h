#ifndef I2C_DW_H
#define I2C_DW_H

#include <stdint.h>

/*
 * Драйвер I2C-контролера Synopsys DesignWare, у вигляді Intel LPSS (PCI "Serial IO I2C").
 * Саме на ньому в сучасних ноутбуках сидить тачпад. Усе робиться опитуванням, без переривань.
 */

#define DW_MAX_CTRL 8

/* коди помилок */
#define DW_OK           0
#define DW_ERR_NOACK   (-1)   /* за адресою ніхто не відповів */
#define DW_ERR_DATA    (-2)   /* пристрій відмовився приймати байт */
#define DW_ERR_TIMEOUT (-3)
#define DW_ERR_BUS     (-4)   /* інше: втрата арбітражу, і т.д. */
#define DW_ERR_ARGS    (-5)

typedef struct {
    uintptr_t base;                  /* MMIO-база регістрів */
    uint8_t   pci_bus, pci_slot, pci_func;
    uint16_t  pci_device;
    int       tx_depth, rx_depth;    /* глибина FIFO */
    uint32_t  clk_khz;               /* припущена частота тактування (від неї залежить швидкість шини) */

    /* поточна транзакція */
    int            busy;
    const uint8_t *wbuf; int wlen, wsent;
    uint8_t       *rbuf; int rlen, rcmds, rrecv;
    uint64_t       deadline;
    uint32_t       abrt_src;         /* останнє значення IC_TX_ABRT_SOURCE */
} dw_i2c_t;

/* Знаходить на PCI всі DesignWare I2C-контролери, виводить їх зі скидання. Повертає кількість. */
int  dw_i2c_discover(dw_i2c_t *out, int max);

/* Налаштовує швидкість шини (~100 кГц) під припущену частоту тактування clk_khz. */
int  dw_i2c_setup(dw_i2c_t *c, uint32_t clk_khz);

/* Транзакція з блокуванням: записати wlen байт, потім (з повторним стартом) прочитати rlen.
 * wlen або rlen може бути 0. Повертає DW_OK або DW_ERR_*. */
int  dw_i2c_xfer(dw_i2c_t *c, uint8_t addr, const uint8_t *wr, int wlen,
                 uint8_t *rd, int rlen, uint32_t timeout_ms);

/* Те саме, але без очікування: begin запускає, step викликати у циклі.
 * step: 1 — готово, 0 — ще йде, <0 — помилка DW_ERR_*. */
int  dw_i2c_begin(dw_i2c_t *c, uint8_t addr, const uint8_t *wr, int wlen,
                  uint8_t *rd, int rlen, uint32_t timeout_ms);
int  dw_i2c_step(dw_i2c_t *c);

/* ---- час (за лічильником TSC) ---- */
uint64_t i2c_now(void);                 /* такти */
uint32_t i2c_cycles_per_ms(void);
void     i2c_delay_ms(uint32_t ms);

#endif
