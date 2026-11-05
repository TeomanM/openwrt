// SPDX-License-Identifier: GPL-2.0-only
/*
 * SPI NOR flash controller of the EcoNet/TrendChip MT7510 (TC3162 family).
 *
 * The controller sends an opcode, up to 3 address bytes and up to 4 data
 * bytes per transaction and can receive up to 4 bytes. The whole flash is
 * also readable through a memory mapped window, which is used for reads.
 *
 * Based on the AR2315 spiflash driver (Atheros, OpenWrt) and the TrendChip
 * spiflash_tc3162 driver.
 */

#include <linux/bitfield.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/mtd/mtd.h>
#include <linux/mtd/partitions.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/sizes.h>

#define SFC_CTL			0x00
#define   SFC_CTL_TXCNT		GENMASK(3, 0)
#define   SFC_CTL_RXCNT		GENMASK(7, 4)
#define   SFC_CTL_START		BIT(8)
#define   SFC_CTL_BUSY		BIT(16)
#define SFC_OPCODE		0x04
#define SFC_DATA		0x08

#define SPINOR_OP_WREN		0x06
#define SPINOR_OP_RDSR		0x05
#define SPINOR_OP_WRSR		0x01
#define SPINOR_OP_PP		0x02
#define SPINOR_OP_SE		0xd8
#define SPINOR_OP_RDID		0x9f

#define SR_WIP			BIT(0)
#define SR_BP_MASK		GENMASK(4, 2)

#define SFC_PAGE_SIZE		256
#define SFC_SECTOR_SIZE		SZ_64K

struct econet_sfc {
	struct mtd_info mtd;
	struct device *dev;
	void __iomem *regs;
	void __iomem *window;
	struct mutex lock;
};

/* DSL-N16 bring-up debug: set when the flash is registered */
int econet_sfc_debug_ok;


static inline struct econet_sfc *mtd_to_sfc(struct mtd_info *mtd)
{
	return container_of(mtd, struct econet_sfc, mtd);
}

static int sfc_wait_idle(struct econet_sfc *sfc, u32 *ctl)
{
	u32 val;
	int ret;

	ret = readl_poll_timeout_atomic(sfc->regs + SFC_CTL, val,
					!(val & SFC_CTL_BUSY), 0, 10000);
	if (ret)
		dev_err(sfc->dev, "controller stuck busy (ctl 0x%08x)\n", val);
	if (ctl)
		*ctl = val;
	return ret;
}

/*
 * One transaction: opcode, then (tx - 1) bytes taken from 'opcode_arg'
 * (address or status byte) and DATA, then receive 'rx' bytes.
 */
static int sfc_xfer(struct econet_sfc *sfc, u8 opcode, u32 arg, u32 data,
		    unsigned int tx, unsigned int rx, u32 *out)
{
	u32 ctl;
	int ret;

	ret = sfc_wait_idle(sfc, &ctl);
	if (ret)
		return ret;

	writel(data, sfc->regs + SFC_DATA);
	writel(opcode | (arg << 8), sfc->regs + SFC_OPCODE);

	ctl &= ~(SFC_CTL_TXCNT | SFC_CTL_RXCNT);
	ctl |= FIELD_PREP(SFC_CTL_TXCNT, tx) | FIELD_PREP(SFC_CTL_RXCNT, rx) |
	       SFC_CTL_START;
	writel(ctl, sfc->regs + SFC_CTL);

	ret = sfc_wait_idle(sfc, NULL);
	if (ret)
		return ret;

	if (out) {
		u32 val = readl(sfc->regs + SFC_DATA);

		*out = rx < 4 ? val & GENMASK(rx * 8 - 1, 0) : val;
	}

	return 0;
}

static int sfc_read_sr(struct econet_sfc *sfc, u32 *sr)
{
	return sfc_xfer(sfc, SPINOR_OP_RDSR, 0, 0, 1, 1, sr);
}

static int sfc_write_enable(struct econet_sfc *sfc)
{
	return sfc_xfer(sfc, SPINOR_OP_WREN, 0, 0, 1, 0, NULL);
}

static int sfc_wait_ready(struct econet_sfc *sfc, unsigned long timeout_us,
			  bool can_sleep)
{
	u32 sr = 0;
	int ret, err = 0;

	if (can_sleep)
		ret = read_poll_timeout(sfc_read_sr, err, err || !(sr & SR_WIP),
					100, timeout_us, false, sfc, &sr);
	else
		ret = read_poll_timeout_atomic(sfc_read_sr, err,
					       err || !(sr & SR_WIP), 1,
					       timeout_us, false, sfc, &sr);
	if (err)
		return err;
	if (ret)
		dev_err(sfc->dev, "flash stuck busy (sr 0x%02x)\n", sr);
	return ret;
}

static int sfc_erase(struct mtd_info *mtd, struct erase_info *instr)
{
	struct econet_sfc *sfc = mtd_to_sfc(mtd);
	u32 addr = instr->addr;
	u32 end = instr->addr + instr->len;
	int ret = 0;

	mutex_lock(&sfc->lock);
	for (; addr < end; addr += mtd->erasesize) {
		ret = sfc_write_enable(sfc);
		if (!ret)
			ret = sfc_xfer(sfc, SPINOR_OP_SE, addr, 0, 4, 0, NULL);
		if (!ret)
			ret = sfc_wait_ready(sfc, 3 * USEC_PER_SEC, true);
		if (ret) {
			instr->fail_addr = addr;
			break;
		}
	}
	mutex_unlock(&sfc->lock);

	return ret;
}

static int sfc_read(struct mtd_info *mtd, loff_t from, size_t len,
		    size_t *retlen, u_char *buf)
{
	struct econet_sfc *sfc = mtd_to_sfc(mtd);

	mutex_lock(&sfc->lock);
	memcpy_fromio(buf, sfc->window + from, len);
	mutex_unlock(&sfc->lock);

	*retlen = len;
	return 0;
}

static int sfc_write(struct mtd_info *mtd, loff_t to, size_t len,
		     size_t *retlen, const u_char *buf)
{
	struct econet_sfc *sfc = mtd_to_sfc(mtd);
	int ret = 0;

	*retlen = 0;

	mutex_lock(&sfc->lock);
	while (len) {
		size_t n = min_t(size_t, len, 4);
		u32 page_left = SFC_PAGE_SIZE - (to & (SFC_PAGE_SIZE - 1));
		u32 data = 0;
		size_t i;

		n = min_t(size_t, n, page_left);
		for (i = 0; i < n; i++)
			data |= (u32)buf[i] << (8 * i);

		ret = sfc_write_enable(sfc);
		if (!ret)
			ret = sfc_xfer(sfc, SPINOR_OP_PP, to, data, 4 + n, 0,
				       NULL);
		if (!ret)
			ret = sfc_wait_ready(sfc, 10000, false);
		if (ret)
			break;

		to += n;
		buf += n;
		len -= n;
		*retlen += n;
	}
	mutex_unlock(&sfc->lock);

	return ret;
}

static int sfc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct econet_sfc *sfc;
	struct resource *res;
	struct mtd_info *mtd;
	u32 id = 0, sr;
	bool id_ok = false;
	u8 capacity;
	int ret;

	sfc = devm_kzalloc(dev, sizeof(*sfc), GFP_KERNEL);
	if (!sfc)
		return -ENOMEM;

	sfc->dev = dev;
	mutex_init(&sfc->lock);

	/* map without claiming: the register block is shared with other users */
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return -EINVAL;
	sfc->regs = devm_ioremap(dev, res->start, resource_size(res));
	if (!sfc->regs)
		return -ENOMEM;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 1);
	if (!res)
		return -EINVAL;
	sfc->window = devm_ioremap(dev, res->start, resource_size(res));
	if (!sfc->window)
		return -ENOMEM;

	ret = sfc_xfer(sfc, SPINOR_OP_RDID, 0, 0, 1, 3, &id);
	if (ret) {
		dev_warn(dev, "JEDEC id read failed (%d)\n", ret);
	} else {
		/* first received byte ends up in bits 7:0 (stock swaps it) */
		capacity = (id >> 16) & 0xff;
		dev_info(dev, "JEDEC id %02x %02x %02x\n", id & 0xff,
			 (id >> 8) & 0xff, capacity);
		id_ok = capacity >= 0x10 && capacity <= 0x18 &&
			(1ULL << capacity) == resource_size(res);
		if (!id_ok)
			dev_warn(dev, "id does not match the read window\n");
	}

	if (!id_ok)
		dev_warn(dev, "commands not working, flash is read-only\n");

	/* clear block protection, if any */
	if (id_ok && !sfc_read_sr(sfc, &sr) && (sr & SR_BP_MASK)) {
		dev_info(dev, "clearing block protection (sr 0x%02x)\n", sr);
		sfc_write_enable(sfc);
		sfc_xfer(sfc, SPINOR_OP_WRSR, 0, 0, 2, 0, NULL);
		sfc_wait_ready(sfc, 100000, true);
	}

	mtd = &sfc->mtd;
	mtd->name = dev_name(dev);
	mtd->dev.parent = dev;
	mtd_set_of_node(mtd, dev->of_node);
	mtd->type = MTD_NORFLASH;
	mtd->flags = id_ok ? MTD_CAP_NORFLASH : MTD_CAP_ROM;
	mtd->size = resource_size(res);
	mtd->erasesize = SFC_SECTOR_SIZE;
	mtd->writesize = 1;
	mtd->writebufsize = SFC_PAGE_SIZE;
	mtd->owner = THIS_MODULE;
	mtd->_erase = sfc_erase;
	mtd->_read = sfc_read;
	mtd->_write = sfc_write;

	platform_set_drvdata(pdev, sfc);

	ret = mtd_device_register(mtd, NULL, 0);
	econet_sfc_debug_ok = (ret ? 0 : 1) | (id_ok ? 2 : 0);
	return ret;
}

static void sfc_remove(struct platform_device *pdev)
{
	struct econet_sfc *sfc = platform_get_drvdata(pdev);

	WARN_ON(mtd_device_unregister(&sfc->mtd));
}

static const struct of_device_id sfc_of_match[] = {
	{ .compatible = "econet,mt7510-sfc" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, sfc_of_match);

static struct platform_driver sfc_driver = {
	.probe = sfc_probe,
	.remove = sfc_remove,
	.driver = {
		.name = "econet-sfc",
		.of_match_table = sfc_of_match,
	},
};
module_platform_driver(sfc_driver);

MODULE_DESCRIPTION("EcoNet MT7510 SPI NOR flash controller");
MODULE_LICENSE("GPL");
