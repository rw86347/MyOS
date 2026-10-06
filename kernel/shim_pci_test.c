/*
 * shim_pci_test.c — Linux's device, PCI, IRQ and DMA API (W1b), checked at
 * boot in `make test` builds with a driver written only against that API.
 * Its device is QEMU's "edu" card (1234:11e8), made for teaching drivers:
 * an id register, a register that answers with the inverse of what was
 * written, an interrupt raised on request, and a DMA engine that copies
 * between RAM and a 4 KiB buffer on the card (docs/specs/edu in QEMU).
 */
#include <linux/completion.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <selftest.h>

#define EDU_ID         0x00
#define EDU_LIVE       0x04
#define EDU_IRQ_STATUS 0x24
#define EDU_IRQ_RAISE  0x60
#define EDU_IRQ_ACK    0x64
#define EDU_DMA_SRC    0x80
#define EDU_DMA_DST    0x88
#define EDU_DMA_COUNT  0x90
#define EDU_DMA_CMD    0x98
#define EDU_DMA_START  1
#define EDU_DMA_TO_RAM 2
#define EDU_DMA_IRQ    4                /* raise 0x100 when the copy is done */
#define EDU_BUF        0x40000          /* the card's buffer, as a DMA address */

struct edu {
    void __iomem *regs;
    struct completion done;
    u32 status;
    int hard, threaded;
};
static struct edu edu;
static int probed, removed;
static struct pci_dev *edu_dev;

/* In the interrupt: whose is it, acknowledge it, leave the rest to the
   thread. */
static irqreturn_t edu_hard(int irq, void *id)
{
    struct edu *e = id;
    (void)irq;
    u32 st = ioread32(e->regs + EDU_IRQ_STATUS);
    if (!st)
        return IRQ_NONE;
    iowrite32(st, e->regs + EDU_IRQ_ACK);
    e->status = st;
    e->hard++;
    return IRQ_WAKE_THREAD;
}

static irqreturn_t edu_thread(int irq, void *id)
{
    struct edu *e = id;
    (void)irq;
    e->threaded++;
    complete(&e->done);
    return IRQ_HANDLED;
}

static int edu_dma(struct edu *e, u64 src, u64 dst, int to_ram)
{
    reinit_completion(&e->done);
    writeq(src, e->regs + EDU_DMA_SRC);
    writeq(dst, e->regs + EDU_DMA_DST);
    writeq(4096, e->regs + EDU_DMA_COUNT);
    iowrite32(EDU_DMA_START | EDU_DMA_IRQ | (to_ram ? EDU_DMA_TO_RAM : 0), e->regs + EDU_DMA_CMD);
    return wait_for_completion_timeout(&e->done, 1000) > 0 && e->status == 0x100;
}

static int edu_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
    struct edu *e = &edu;
    probed = 1;
    const char *name = pci_name(pdev);
    check(id->device == 0x11e8 && strlen(name) == 12 && !strncmp(name, "0000:00:", 8),
          "shim pci: probe by id table, Linux's device name");
    edu_dev = pdev;
    if (pcim_enable_device(pdev))
        return -EIO;
    pci_set_master(pdev);
    check(pci_resource_len(pdev, 0) == 1u << 20, "shim pci: BAR size, sized the standard way");
    e->regs = pcim_iomap(pdev, 0, 0);
    if (!e->regs)
        return -ENOMEM;
    iowrite32(0x12345678, e->regs + EDU_LIVE);
    check(ioread32(e->regs + EDU_ID) == 0x010000ed && ioread32(e->regs + EDU_LIVE) == ~0x12345678u,
          "shim pci: pcim_iomap, registers");
    check(dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(28)) == 0, "shim pci: DMA mask");

    int n = pci_alloc_irq_vectors(pdev, 1, 1, PCI_IRQ_MSI);
    int irq = pci_irq_vector(pdev, 0);
    check(n == 1 && irq >= 16, "shim pci: an MSI vector");
    init_completion(&e->done);
    check(devm_request_threaded_irq(&pdev->dev, (unsigned)irq, edu_hard, edu_thread, 0, "edu", e) == 0,
          "shim pci: devm_request_threaded_irq");
    iowrite32(5, e->regs + EDU_IRQ_RAISE);
    check(wait_for_completion_timeout(&e->done, 500) > 0 && e->status == 5 && e->hard == 1 &&
          e->threaded == 1, "shim pci: MSI interrupt, hard then threaded handler");

    dma_addr_t src_dma;
    u8 *src = dma_alloc_coherent(&pdev->dev, 4096, &src_dma, GFP_KERNEL);
    u8 *dst = kzalloc(4096, GFP_KERNEL);            /* > 1 KiB: a contiguous run */
    dma_addr_t dst_dma = dst ? dma_map_single(&pdev->dev, dst, 4096, DMA_FROM_DEVICE)
                             : DMA_MAPPING_ERROR;
    int ok = src && dst && !dma_mapping_error(&pdev->dev, dst_dma);
    if (ok) {
        for (int k = 0; k < 4096; k++)
            src[k] = (u8)(k * 7 + 3);
        ok = edu_dma(e, src_dma, EDU_BUF, 0) && edu_dma(e, EDU_BUF, dst_dma, 1);
        dma_unmap_single(&pdev->dev, dst_dma, 4096, DMA_FROM_DEVICE);
        ok = ok && memcmp(src, dst, 4096) == 0;
    }
    check(ok, "shim pci: DMA into the card and back (coherent + streaming)");
    kfree(dst);
    if (src)
        dma_free_coherent(&pdev->dev, 4096, src, src_dma);
    pci_set_drvdata(pdev, e);
    return 0;
}

static void edu_remove(struct pci_dev *pdev)
{
    removed = pci_get_drvdata(pdev) == &edu;
}

static const struct pci_device_id edu_ids[] = {
    { PCI_DEVICE(0x1234, 0x11e8) },
    { 0 }
};

static struct pci_driver edu_driver = {
    .name = "edu-test", .id_table = edu_ids, .probe = edu_probe, .remove = edu_remove,
};

void shim_pci_selftest(void)
{
    pci_register_driver(&edu_driver);
    check(probed, "shim pci: the edu card was found and probed");
    pci_unregister_driver(&edu_driver);
    /* devres ran: pcim_enable_device's record switched decoding back off. */
    u16 cmd = PCI_COMMAND_MEMORY;
    if (edu_dev)
        pci_read_config_word(edu_dev, PCI_COMMAND, &cmd);
    check(removed && !(cmd & PCI_COMMAND_MEMORY), "shim pci: remove, devm_* released");
}
