/*
 * Virtio console (virtio 1.x section 5.3), port 0 only: a serial console.
 * Apple's Virtualization framework gives guests no PL011; its serial ports
 * are virtio consoles, so this is Sonar's console there.
 *
 * Output is a console sink: text goes into a small ring of buffers that the
 * device consumes (completed ones are reclaimed on the next write). Input
 * arrives in receive buffers, which are fed to the shared console input
 * queue from the interrupt (or the idle loop) and given back to the device.
 */
#include "console.h"
#include "irq.h"
#include "kernel.h"
#include "mm.h"
#include "string.h"
#include "virtio.h"

#define TXN 32
#define TXSZ 512
#define RXN 16
#define RXSZ 64

static struct virtio_dev *con;
static struct virtq *rxq, *txq;
static u8 *txbuf, *rxbuf;
static bool tx_busy[TXN];
static int tx_next;
static spinlock_t rx_lock;

static void reclaim(void)
{
    void *ck;
    while ((ck = virtq_poll(txq, NULL)) != NULL)
        tx_busy[((u8 *)ck - txbuf) / TXSZ] = false;
}

static void con_write(const char *s, size_t len)
{
    while (len) {
        reclaim();
        int slot = tx_next;
        u64 deadline = time_ms() + 200;
        while (tx_busy[slot]) { /* all buffers in flight: wait for the device */
            reclaim();
            if (time_ms() > deadline)
                return; /* host not reading; drop rather than hang */
        }
        size_t n = len < TXSZ ? len : TXSZ;
        u8 *b = txbuf + slot * TXSZ;
        memcpy(b, s, n);
        struct vq_buf vb = { b, (u32)n, false };
        if (!virtq_submit(txq, &vb, 1, b))
            return;
        tx_busy[slot] = true;
        tx_next = (slot + 1) % TXN;
        virtq_kick(txq);
        s += n;
        len -= n;
    }
}

static void con_flush(void) {}

static struct console_sink sink = { "virtio-console", con_write, con_flush };

static void rx_poll(void)
{
    if (!rxq)
        return;
    u64 f = spin_lock_irqsave(&rx_lock);
    void *ck;
    u32 len;
    bool any = false;
    while ((ck = virtq_poll(rxq, &len)) != NULL) {
        if (len > RXSZ)
            len = RXSZ;
        console_input(ck, len);
        struct vq_buf vb = { ck, RXSZ, true };
        virtq_submit(rxq, &vb, 1, ck);
        any = true;
    }
    if (any)
        virtq_kick(rxq);
    spin_unlock_irqrestore(&rx_lock, f);
}

static void con_irq(struct virtio_dev *d) { rx_poll(); }

bool virtio_console_probe(struct virtio_dev *d)
{
    if (con || !virtio_negotiate(d, 0))
        return false;
    rxq = virtio_setup_queue(d, 0, RXN);
    txq = virtio_setup_queue(d, 1, TXN);
    txbuf = page_alloc(ALIGN_UP(TXN * TXSZ, PAGE_SIZE) / PAGE_SIZE);
    rxbuf = page_alloc(ALIGN_UP(RXN * RXSZ, PAGE_SIZE) / PAGE_SIZE);
    if (!rxq || !txq || !txbuf || !rxbuf) {
        virtio_fail(d);
        return false;
    }
    for (int i = 0; i < RXN; i++) {
        struct vq_buf vb = { rxbuf + i * RXSZ, RXSZ, true };
        virtq_submit(rxq, &vb, 1, rxbuf + i * RXSZ);
    }
    con = d;
    d->on_interrupt = con_irq;
    virtio_driver_ok(d);
    virtq_kick(rxq);
    virtq_interrupts(txq, false); /* tx completions are reclaimed lazily */
    console_add_idle_hook(rx_poll);
    /* Show what was printed before this console existed. */
    static char early[16384];
    size_t n = console_log_read(early, sizeof(early));
    con_write(early, n);
    console_register(&sink);
    printk("virtio: console%s\n", d->msix ? "" : " (polled)");
    return true;
}
