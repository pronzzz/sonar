/* ARM PrimeCell PL011 UART: polled transmit, interrupt-driven receive. */
#include "console.h"
#include "fdt.h"
#include "irq.h"
#include "kernel.h"
#include "mm.h"
#include "mmio.h"

#define UART_DR 0x00
#define UART_FR 0x18
#define UART_IBRD 0x24
#define UART_FBRD 0x28
#define UART_LCRH 0x2c
#define UART_CR 0x30
#define UART_IMSC 0x38
#define UART_MIS 0x40
#define UART_ICR 0x44

#define FR_BUSY (1 << 3)
#define FR_RXFE (1 << 4)
#define FR_TXFF (1 << 5)
#define LCRH_FEN (1 << 4)
#define INT_RX (1 << 4)
#define INT_RT (1 << 6)

static uintptr_t base;

static void pl011_putc(char c)
{
    while (mmio_read32(base + UART_FR) & FR_TXFF)
        ;
    mmio_write32(base + UART_DR, (u8)c);
}

static void pl011_write(const char *s, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        if (s[i] == '\n')
            pl011_putc('\r');
        pl011_putc(s[i]);
    }
}

static struct console_sink pl011_sink = { "serial", pl011_write, NULL };

static void pl011_irq(int irq, void *ctx)
{
    char buf[32];
    size_t n = 0;
    /* Acknowledge first, then drain: a byte that arrives after the drain
     * loop raises a fresh interrupt instead of being acknowledged unseen
     * (which, with a full FIFO, stalls receive for good). */
    mmio_write32(base + UART_ICR, INT_RX | INT_RT);
    while (!(mmio_read32(base + UART_FR) & FR_RXFE)) {
        u32 v = mmio_read32(base + UART_DR);
        if (v & 0xf00)
            continue; /* framing/parity/break/overrun error */
        buf[n++] = (char)(v & 0xff);
        if (n == sizeof(buf)) {
            console_input(buf, n);
            n = 0;
        }
    }
    if (n)
        console_input(buf, n);
}

int fdt_console_node(void);
static int uart_node = -1;

void pl011_init(void)
{
    int node = fdt_console_node();
    u64 addr;
    if (node < 0 || !fdt_reg(node, 0, &addr, NULL))
        return;
    uart_node = node;
    base = (uintptr_t)ioremap(addr, 0x1000);
    /* Keep the line settings the firmware chose (QEMU ignores them anyway);
     * make sure TX/RX and the UART are enabled. */
    u32 cr = mmio_read32(base + UART_CR);
    u32 lcrh = mmio_read32(base + UART_LCRH);
    if (!(lcrh & LCRH_FEN)) {
        /* Without the FIFO the UART holds one byte, so pasted input is
         * lost or delayed. LCR_H may only change while the UART is
         * disabled and idle (PL011 TRM, UARTCR). */
        mmio_write32(base + UART_CR, cr & ~1u);
        while (mmio_read32(base + UART_FR) & FR_BUSY)
            ;
        mmio_write32(base + UART_LCRH, lcrh | LCRH_FEN);
    }
    mmio_write32(base + UART_CR, cr | (1 << 0) | (1 << 8) | (1 << 9));
    console_register(&pl011_sink);
}

/* Receive interrupts; call after the interrupt controller is up. */
void pl011_init_irq(void)
{
    if (uart_node < 0)
        return;
    int irq = fdt_irq(uart_node, 0);
    if (irq >= 0) {
        irq_register(irq, pl011_irq, NULL);
        mmio_write32(base + UART_ICR, 0x7ff);
        mmio_write32(base + UART_IMSC, INT_RX | INT_RT);
        irq_unmask(irq);
    }
}
