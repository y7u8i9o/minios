#include <arch/pit.h>
#include <arch/io.h>

#define PIT_CH2      0x42
#define PIT_CMD      0x43
#define PIT_GATE     0x61

void pit_wait_us(unsigned us)
{
    uint32_t count = (uint32_t)(((uint64_t)PIT_FREQUENCY * us) / 1000000);
    if (count == 0)
        count = 1;
    if (count > 0xffff)
        count = 0xffff;

    /* Gate channel 2 off, speaker off. */
    uint8_t gate = inb(PIT_GATE) & ~0x03;
    outb(PIT_GATE, gate);
    /* Channel 2, lobyte/hibyte, mode 0 (interrupt on terminal count). */
    outb(PIT_CMD, 0xb0);
    outb(PIT_CH2, count & 0xff);
    outb(PIT_CH2, (count >> 8) & 0xff);
    /* Raise the gate to start counting. */
    outb(PIT_GATE, gate | 0x01);
    while (!(inb(PIT_GATE) & 0x20))
        ;
    outb(PIT_GATE, gate);
}
