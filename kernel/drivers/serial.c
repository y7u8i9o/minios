#include <drivers/serial.h>
#include <arch/io.h>

#define COM1 0x3f8

static bool serial_ready;

void serial_init(void)
{
    outb(COM1 + 1, 0x00);   /* disable interrupts */
    outb(COM1 + 3, 0x80);   /* DLAB on */
    outb(COM1 + 0, 0x01);   /* divisor 1: 115200 baud */
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);   /* 8N1 */
    outb(COM1 + 2, 0xc7);   /* FIFO enabled and cleared, 14 byte threshold */
    outb(COM1 + 4, 0x03);   /* DTR, RTS */
    serial_ready = true;
}

void serial_putc(char c)
{
    if (!serial_ready)
        return;
    while (!(inb(COM1 + 5) & 0x20))
        ;
    outb(COM1, (uint8_t)c);
}

void serial_write(const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '\n')
            serial_putc('\r');
        serial_putc(s[i]);
    }
}
