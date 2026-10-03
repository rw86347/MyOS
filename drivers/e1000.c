/*
 * e1000.c — Intel 82540EM, the card QEMU emulates (step 10)
 *
 * Written from Intel's public register manual, against our net_device.
 * This is not Linux's e1000 driver. The calls it makes are the same ones
 * that driver makes: register_netdev, ndo_start_xmit, napi_schedule, netif_rx.
 *
 * The card DMA's packet bytes itself. The CPU only writes descriptors
 * (where the bytes are, how long) and reads them back. An interrupt does
 * nothing but note "look at the ring"; net_rx_action pulls the packets.
 * Rings are fixed: 32 receive slots, 16 transmit slots. A flood cannot
 * allocate without bound.
 */
#include <e1000.h>
#include <interrupts.h>
#include <io.h>
#include <kprintf.h>
#include <memlayout.h>
#include <netdevice.h>
#include <paging.h>
#include <pci.h>
#include <pmm.h>
#include <string.h>
#include <thread.h>
#include <vfs.h>

#define E1000_CTRL   0x0000
#define E1000_STATUS 0x0008
#define E1000_EERD   0x0014
#define E1000_ICR    0x00C0
#define E1000_IMS    0x00D0
#define E1000_IMC    0x00D8
#define E1000_RCTL   0x0100
#define E1000_TCTL   0x0400
#define E1000_TIPG   0x0410
#define E1000_RDBAL  0x2800
#define E1000_RDBAH  0x2804
#define E1000_RDLEN  0x2808
#define E1000_RDH    0x2810
#define E1000_RDT    0x2818
#define E1000_TDBAL  0x3800
#define E1000_TDBAH  0x3804
#define E1000_TDLEN  0x3808
#define E1000_TDH    0x3810
#define E1000_TDT    0x3818
#define E1000_MTA    0x5200
#define E1000_RAL    0x5400
#define E1000_RAH    0x5404

#define CTRL_FD      0x00000001
#define CTRL_SLU     0x00000040
#define CTRL_RST     0x04000000
#define CTRL_SPEED   0x00000200     /* 1000 Mb/s */
#define STATUS_LU    0x00000002
#define RCTL_EN      0x00000002
#define RCTL_BAM     0x00008000     /* accept broadcasts (ARP) */
#define RCTL_SECRC   0x04000000     /* drop the Ethernet checksum */
#define TCTL_EN      0x00000002
#define TCTL_PSP     0x00000008
#define IMS_RXT0     0x00000080
#define IMS_LSC      0x00000004

#define RX_N         32
#define TX_N         16
#define BUF_SZ       2048
#define MMIO_VA      0xF4000000u

struct rx_desc {
    uint64_t addr;
    uint16_t length;
    uint16_t csum;
    uint8_t status;
    uint8_t errors;
    uint16_t special;
} __attribute__((packed));

struct tx_desc {
    uint64_t addr;
    uint16_t length;
    uint8_t cso;
    uint8_t cmd;
    uint8_t status;
    uint8_t css;
    uint16_t special;
} __attribute__((packed));

#define RX_DD  0x01
#define TX_DD  0x01
#define TX_EOP 0x01
#define TX_IFCS 0x02
#define TX_RS  0x08

struct e1000 {
    volatile uint32_t *mmio;
    struct pci_dev *pci;
    struct net_device dev;
    struct napi_struct napi;
    struct rx_desc *rx;
    struct tx_desc *tx;
    uint32_t rx_phys, tx_phys;
    uint8_t *rx_buf[RX_N];
    uint32_t rx_buf_phys[RX_N];
    uint8_t *tx_buf[TX_N];
    uint32_t tx_buf_phys[TX_N];
    unsigned tx_tail, tx_head;
    unsigned rx_cur;
};

static struct e1000 nic;
static uint32_t irq_count;

/* The packet rings live in ordinary write-back RAM. PCI bus-master DMA on
   a PC is cache-coherent (the chipset snoops the CPU's caches), so nothing
   needs flushing — wbinvd here cost a whole-cache write-back per packet.
   What is needed is ordering: descriptor stores must reach memory before
   the doorbell (an uncached MMIO write), and we must read a descriptor's
   status before its contents. x86 keeps stores in order and loads in order
   for write-back memory, and an uncached write waits for earlier stores, so
   a compiler barrier is enough (Linux: dma_wmb()/dma_rmb() are the same). */
static void dma_sync(void)
{
    __asm__ volatile ("" ::: "memory");
}

static uint32_t rd(uint32_t reg)
{
    return nic.mmio[reg / 4];
}

static void wr(uint32_t reg, uint32_t val)
{
    nic.mmio[reg / 4] = val;
}

/* One frame, mapped uncached so a read sees the device, not a stale cache line. */
static int map_mmio(uint32_t phys, uint32_t bytes)
{
    uint32_t pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    if (pages == 0 || pages > 32)
        return -1;
    for (uint32_t i = 0; i < pages; i++) {
        if (vmm_map(MMIO_VA + i * PAGE_SIZE, phys + i * PAGE_SIZE,
                    PTE_WRITABLE | 0x10) < 0)
            return -1;
    }
    nic.mmio = (volatile uint32_t *)MMIO_VA;
    return 0;
}

static uint16_t eeprom(uint8_t addr)
{
    wr(E1000_EERD, ((uint32_t)addr << 8) | 1);
    for (int i = 0; i < 10000; i++) {
        uint32_t v = rd(E1000_EERD);
        if (v & 0x10)
            return (uint16_t)(v >> 16);
    }
    return 0xFFFF;
}

static int read_mac(uint8_t mac[6])
{
    uint16_t w0 = eeprom(0), w1 = eeprom(1), w2 = eeprom(2);
    if (w0 == 0xFFFF || w0 == 0x0000)
        return -1;
    mac[0] = (uint8_t)w0;
    mac[1] = (uint8_t)(w0 >> 8);
    mac[2] = (uint8_t)w1;
    mac[3] = (uint8_t)(w1 >> 8);
    mac[4] = (uint8_t)w2;
    mac[5] = (uint8_t)(w2 >> 8);
    return 0;
}

static int alloc_buf(uint8_t **virt, uint32_t *phys)
{
    uint32_t frame = pmm_alloc();
    if (!frame)
        return -1;
    *phys = frame;
    *virt = P2V(frame);
    memset(*virt, 0, PAGE_SIZE);
    return 0;
}

static int setup_rings(void)
{
    if (alloc_buf((uint8_t **)&nic.rx, &nic.rx_phys) < 0)
        return -1;
    if (alloc_buf((uint8_t **)&nic.tx, &nic.tx_phys) < 0)
        return -1;
    for (int i = 0; i < RX_N; i++) {
        if (alloc_buf(&nic.rx_buf[i], &nic.rx_buf_phys[i]) < 0)
            return -1;
        nic.rx[i].addr = nic.rx_buf_phys[i];
        nic.rx[i].status = 0;
    }
    for (int i = 0; i < TX_N; i++) {
        if (alloc_buf(&nic.tx_buf[i], &nic.tx_buf_phys[i]) < 0)
            return -1;
        nic.tx[i].addr = nic.tx_buf_phys[i];
        nic.tx[i].status = TX_DD;
        nic.tx[i].cmd = 0;
    }
    wr(E1000_RDBAL, nic.rx_phys);
    wr(E1000_RDBAH, 0);
    wr(E1000_RDLEN, RX_N * sizeof(struct rx_desc));
    wr(E1000_RDH, 0);
    wr(E1000_RDT, RX_N - 1);
    nic.rx_cur = 0;

    wr(E1000_TDBAL, nic.tx_phys);
    wr(E1000_TDBAH, 0);
    wr(E1000_TDLEN, TX_N * sizeof(struct tx_desc));
    wr(E1000_TDH, 0);
    wr(E1000_TDT, 0);
    nic.tx_tail = nic.tx_head = 0;
    dma_sync();
    return 0;
}

static int e1000_poll(struct napi_struct *napi, int budget)
{
    (void)napi;
    dma_sync();
    int work = 0;
    int last = -1;
    while (work < budget) {
        struct rx_desc *d = &nic.rx[nic.rx_cur];
        if (!(d->status & RX_DD))
            break;
        uint16_t len = d->length;
        if (len >= ETH_HLEN && len <= BUF_SZ && !d->errors) {
            struct sk_buff *skb = alloc_skb(len);
            if (skb) {
                unsigned char *p = skb_put(skb, len);
                if (p) {
                    memcpy(p, nic.rx_buf[nic.rx_cur], len);
                    eth_type_trans(skb, &nic.dev);
                    netif_rx(skb);
                } else {
                    kfree_skb(skb);
                }
            }
        }
        d->status = 0;
        d->errors = 0;
        last = (int)nic.rx_cur;
        nic.rx_cur = (nic.rx_cur + 1) % RX_N;
        work++;
    }
    if (last >= 0) {
        dma_sync();                     /* status 0 is in RAM before the card reuses it */
        wr(E1000_RDT, (uint32_t)last);
    }
    while (nic.tx_head != nic.tx_tail) {
        if (!(nic.tx[nic.tx_head].status & TX_DD))
            break;
        nic.tx_head = (nic.tx_head + 1) % TX_N;
    }
    if (work < budget) {
        dma_sync();
        if (nic.rx[nic.rx_cur].status & RX_DD)
            return work;                /* leave NAPI scheduled; poll again */
        napi_complete_done(&nic.napi, work);
    }
    return work;
}

static void e1000_irq(struct regs *r)
{
    (void)r;
    uint32_t cause = rd(E1000_ICR);     /* reading clears it */
    if (cause) {
        irq_count++;
        napi_schedule(&nic.napi);
        wq_wake_all(&net_wait);         /* a socket read may be asleep */
        wq_wake_all(&poll_wait);
    }
}

static int e1000_xmit(struct sk_buff *skb, struct net_device *dev)
{
    (void)dev;
    if (!skb->len || skb->len > BUF_SZ) {
        kfree_skb(skb);
        return NETDEV_TX_OK;
    }
    unsigned next = (nic.tx_tail + 1) % TX_N;
    if (next == nic.tx_head)
        return NETDEV_TX_BUSY;          /* caller still owns the skb */
    memcpy(nic.tx_buf[nic.tx_tail], skb->data, skb->len);
    /* TCTL_PSP has the card pad up to the 60-byte minimum frame out of this
       buffer, and the buffer is reused. Zero the padding, or a short frame
       (an ARP is 42 bytes) carries bytes of whatever last used the slot out
       onto the wire. */
    if (skb->len < ETH_ZLEN)
        memset(nic.tx_buf[nic.tx_tail] + skb->len, 0, ETH_ZLEN - skb->len);
    nic.tx[nic.tx_tail].length = (uint16_t)skb->len;
    nic.tx[nic.tx_tail].cmd = TX_EOP | TX_IFCS | TX_RS;
    nic.tx[nic.tx_tail].status = 0;
    dma_sync();
    nic.tx_tail = next;
    wr(E1000_TDT, nic.tx_tail);
    kfree_skb(skb);
    return NETDEV_TX_OK;
}

static int e1000_open(struct net_device *dev)
{
    dev->flags |= IFF_UP | IFF_RUNNING | IFF_BROADCAST;
    return 0;
}

static const struct net_device_ops e1000_ops = {
    .ndo_open = e1000_open,
    .ndo_start_xmit = e1000_xmit,
};

int e1000_init(void)
{
    struct pci_dev *pci = pci_find(0x8086, 0x100e);
    if (!pci)
        return -1;
    uint32_t bar = pci->bar[0] & ~0xFu;
    if (!bar || (pci->bar[0] & 1))
        return -1;                      /* I/O-port BAR: not this card's MMIO */
    if (map_mmio(bar, 0x20000) < 0)
        return -1;
    nic.pci = pci;

    /* Bus master: the card may write packet bytes to RAM. Memory space: the
       registers answer. */
    uint32_t cmd = pci_read_config(pci, 4);
    pci_write_config(pci, 4, cmd | 0x6);

    wr(E1000_IMC, 0xFFFFFFFF);
    wr(E1000_CTRL, rd(E1000_CTRL) | CTRL_RST);
    for (int i = 0; i < 100000 && (rd(E1000_CTRL) & CTRL_RST); i++)
        ;
    wr(E1000_IMC, 0xFFFFFFFF);
    (void)rd(E1000_ICR);

    uint8_t mac[6];
    if (read_mac(mac) < 0) {
        /* QEMU's default when the EEPROM read does not answer. */
        mac[0] = 0x52; mac[1] = 0x54; mac[2] = 0x00;
        mac[3] = 0x12; mac[4] = 0x34; mac[5] = 0x56;
    }
    memcpy(nic.dev.dev_addr, mac, 6);
    wr(E1000_RAL, (uint32_t)mac[0] | ((uint32_t)mac[1] << 8) |
                  ((uint32_t)mac[2] << 16) | ((uint32_t)mac[3] << 24));
    wr(E1000_RAH, (uint32_t)mac[4] | ((uint32_t)mac[5] << 8) | (1u << 31));
    for (int i = 0; i < 128; i++)
        wr(E1000_MTA + (uint32_t)i * 4, 0);

    wr(E1000_CTRL, CTRL_SLU | CTRL_FD | CTRL_SPEED);
    if (setup_rings() < 0) {
        kprintf("e1000: out of memory for rings\n");
        return -1;
    }

    wr(E1000_RCTL, RCTL_EN | RCTL_BAM | RCTL_SECRC);    /* 2048-byte buffers */
    wr(E1000_TCTL, TCTL_EN | TCTL_PSP | (0x0Fu << 4) | (0x40u << 12));
    wr(E1000_TIPG, 0x0060200A);

    memcpy(nic.dev.name, "eth0", 5);
    nic.dev.mtu = ETH_DATA_LEN;
    nic.dev.netdev_ops = &e1000_ops;
    netif_napi_add(&nic.dev, &nic.napi, e1000_poll, 16);
    napi_enable(&nic.napi);
    if (pci->irq)
        irq_install(pci->irq, e1000_irq);
    wr(E1000_IMS, IMS_RXT0 | IMS_LSC);

    int err = register_netdev(&nic.dev);
    kprintf("e1000: %02x:%02x:%02x:%02x:%02x:%02x bar %p irq %u link %s\n",
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
            (void *)bar, pci->irq,
            (rd(E1000_STATUS) & STATUS_LU) ? "up" : "down");
    return err;
}

void e1000_debug(void)
{
    if (!nic.mmio) {
        kprintf("e1000: not attached\n");
        return;
    }
    kprintf("e1000: irq %lu tpt %lu gprc %lu mpc %lu rdh %lu rdt %lu tdh %lu tdt %lu rxst %x\n",
            irq_count, rd(0x40D4), rd(0x4074), rd(0x4010),
            rd(E1000_RDH), rd(E1000_RDT), rd(E1000_TDH), rd(E1000_TDT),
            nic.rx ? nic.rx[nic.rx_cur].status : 0);
}
