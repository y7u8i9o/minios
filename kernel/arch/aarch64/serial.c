#include <drivers/serial.h>
#include "early_mmio.h"

/* The PL011 UART of the QEMU virt machine at 0x09000000. QEMU accepts
 * output without baud rate setup. The registers are mapped on first use,
 * once boot_init has made the direct map offset known; output before that
 * is dropped. */

#define PL011_BASE  0x09000000UL
#define UART_DR     0x00
#define UART_FR     0x18
#define UART_CR     0x30
#define FR_TXFF     (1u << 5)
#define CR_UARTEN   (1u << 0)
#define CR_TXE      (1u << 8)

static volatile uint32_t *uart;
static bool serial_wanted;

void serial_init(void)
{
    serial_wanted = true;
}

static bool serial_ready(void)
{
    if (uart)
        return true;
    if (!serial_wanted)
        return false;
    uart = early_map_device(PL011_BASE, 0x1000);
    if (uart)
        uart[UART_CR / 4] |= CR_UARTEN | CR_TXE;
    return uart != NULL;
}

void serial_putc(char c)
{
    if (!serial_ready())
        return;
    while (uart[UART_FR / 4] & FR_TXFF)
        ;
    uart[UART_DR / 4] = (uint8_t)c;
}

void serial_write(const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '\n')
            serial_putc('\r');
        serial_putc(s[i]);
    }
}
