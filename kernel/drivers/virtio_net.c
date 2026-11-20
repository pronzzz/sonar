/*
 * Virtio network device (virtio 1.x, section 5.1). No offloads are
 * negotiated: the stack computes all checksums and every frame fits one
 * receive buffer. With VIRTIO_F_VERSION_1 each frame is preceded by a
 * 12-byte virtio_net_hdr (num_buffers is always present).
 */
#include "irq.h"
#include "kernel.h"
#include "mm.h"
#include "net.h"
#include "string.h"
#include "virtio.h"

#define VIRTIO_NET_F_MTU (1ULL << 3)
#define VIRTIO_NET_F_MAC (1ULL << 5)
#define VIRTIO_NET_F_STATUS (1ULL << 16)
#define VIRTIO_NET_S_LINK_UP 1

#define HDR 12
#define BUF_SIZE 2048 /* >= HDR + 1514, as required without MRG_RXBUF */
#define RX_BUFS 128
#define TX_BUFS 64

struct vnet {
    struct netif nif;
    struct virtio_dev *vd;
    struct virtq *rxq, *txq;
    u8 *rxbuf, *txbuf; /* RX_BUFS / TX_BUFS slots of BUF_SIZE */
    bool tx_busy[TX_BUFS];
    int tx_inflight;
    /* receive buffers waiting to be given back to the device, in batches
     * so that each notification (a VM exit under virtualization) covers many */
    u16 rx_spare[RX_BUFS];
    int n_spare;
    u64 status_checked; /* config reads trap to the hypervisor: rate-limit */
};

#define RX_BATCH 32

static void rx_post(struct vnet *v, int i)
{
    struct vq_buf b = { v->rxbuf + (size_t)i * BUF_SIZE, BUF_SIZE, true };
    virtq_submit(v->rxq, &b, 1, (void *)(uintptr_t)(i + 1));
}

static void tx_reclaim(struct vnet *v)
{
    void *ck;
    while ((ck = virtq_poll(v->txq, NULL)) != NULL) {
        v->tx_busy[(uintptr_t)ck - 1] = false;
        v->tx_inflight--;
    }
}

static bool vnet_send(struct netif *nif, const void *frame, size_t len)
{
    struct vnet *v = nif->priv;
    if (len > BUF_SIZE - HDR)
        return false;
    tx_reclaim(v);
    int slot = -1;
    for (int i = 0; i < TX_BUFS; i++) {
        if (!v->tx_busy[i]) {
            slot = i;
            break;
        }
    }
    if (slot < 0)
        return false;
    u8 *p = v->txbuf + (size_t)slot * BUF_SIZE;
    memset(p, 0, HDR); /* no checksum offload, no segmentation */
    memcpy(p + HDR, frame, len);
    struct vq_buf b = { p, (u32)(HDR + len), false };
    if (!virtq_submit(v->txq, &b, 1, (void *)(uintptr_t)(slot + 1)))
        return false;
    v->tx_busy[slot] = true;
    v->tx_inflight++;
    virtq_kick(v->txq);
    return true;
}

static void vnet_poll(struct netif *nif)
{
    struct vnet *v = nif->priv;
    void *ck;
    u32 len;
    int n = 0;
    while ((ck = virtq_poll(v->rxq, &len)) != NULL) {
        int i = (int)(uintptr_t)ck - 1;
        if (len > HDR)
            net_input(nif, v->rxbuf + (size_t)i * BUF_SIZE + HDR, len - HDR);
        v->rx_spare[v->n_spare++] = (u16)i;
        n++;
    }
    if (v->n_spare >= RX_BATCH || (n == 0 && v->n_spare)) {
        /* refill in a batch, or when the link went quiet */
        for (int k = 0; k < v->n_spare; k++)
            rx_post(v, v->rx_spare[k]);
        v->n_spare = 0;
        virtq_kick(v->rxq);
    }
    if ((v->vd->features & VIRTIO_NET_F_STATUS) && time_ms() - v->status_checked >= 1000) {
        v->status_checked = time_ms();
        nif->link_up = (virtio_cfg16(v->vd, 6) & VIRTIO_NET_S_LINK_UP) != 0;
    }
    tx_reclaim(v);
}

static void vnet_arm(struct netif *nif, bool on)
{
    struct vnet *v = nif->priv;
    virtq_interrupts(v->rxq, on);
}

static bool vnet_pending(struct netif *nif)
{
    struct vnet *v = nif->priv;
    return virtq_pending(v->rxq);
}

bool virtio_net_probe(struct virtio_dev *d)
{
    if (!virtio_negotiate(d, VIRTIO_NET_F_MAC | VIRTIO_NET_F_STATUS | VIRTIO_NET_F_MTU))
        return false;
    struct vnet *v = kzalloc(sizeof(*v));
    if (!v) {
        virtio_fail(d);
        return false;
    }
    v->vd = d;
    v->rxq = virtio_setup_queue(d, 0, RX_BUFS);
    v->txq = virtio_setup_queue(d, 1, TX_BUFS);
    v->rxbuf = kmalloc((size_t)RX_BUFS * BUF_SIZE);
    v->txbuf = kmalloc((size_t)TX_BUFS * BUF_SIZE);
    if (!v->rxq || !v->txq || !v->rxbuf || !v->txbuf) {
        virtio_fail(d);
        return false;
    }
    struct netif *nif = &v->nif;
    snprintf(nif->name, sizeof(nif->name), "eth%d", d->index);
    if (d->features & VIRTIO_NET_F_MAC) {
        for (int i = 0; i < 6; i++)
            nif->mac[i] = virtio_cfg8(d, (u32)i);
    } else {
        /* locally administered unicast address */
        nif->mac[0] = 0x02;
        nif->mac[1] = 0x53; /* 'S' */
        for (int i = 2; i < 6; i++)
            nif->mac[i] = (u8)(time_ns() >> (8 * i));
    }
    nif->mtu = 1500;
    if (d->features & VIRTIO_NET_F_MTU) {
        u16 mtu = virtio_cfg16(d, 10);
        if (mtu >= 576 && mtu < 1500)
            nif->mtu = mtu;
    }
    nif->link_up = true;
    nif->send = vnet_send;
    nif->poll = vnet_poll;
    nif->pending = vnet_pending;
    nif->arm = vnet_arm;
    nif->priv = v;
    d->priv = v;
    /* post receive buffers now; notify only after DRIVER_OK (virtio 1.x 3.1.1) */
    for (int i = 0; i < RX_BUFS; i++)
        rx_post(v, i);
    /* completions are polled: transmit never interrupts, receive only
     * while the stack sleeps in net_wait() */
    virtq_interrupts(v->txq, false);
    virtq_interrupts(v->rxq, false);
    virtio_driver_ok(d);
    virtq_kick(v->rxq);
    if (d->features & VIRTIO_NET_F_STATUS)
        nif->link_up = (virtio_cfg16(d, 6) & VIRTIO_NET_S_LINK_UP) != 0;
    printk("  %s: virtio network, MAC %02x:%02x:%02x:%02x:%02x:%02x%s\n", nif->name, nif->mac[0],
           nif->mac[1], nif->mac[2], nif->mac[3], nif->mac[4], nif->mac[5],
           nif->link_up ? "" : " (link down)");
    netif_register(nif);
    return true;
}
