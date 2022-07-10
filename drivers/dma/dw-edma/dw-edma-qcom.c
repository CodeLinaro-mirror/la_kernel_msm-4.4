// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2022 Linaro Ltd.
 *
 * Author: Dmitry Baryshkov <dmitry.baryshkov@linaro.org>
 */

#include <linux/dma-mapping.h>
#include <linux/module.h>
#include <linux/of_device.h>

#include "dw-edma-core.h"

static int dw_edma_qcom_irq_vector(struct device *dev, unsigned int nr)
{
	return platform_get_irq_byname(to_platform_device(dev), "edma") + nr;
}

static const struct dw_edma_core_ops dw_edma_qcom_core_ops = {
	.irq_vector = dw_edma_qcom_irq_vector,
};

struct dw_edma_chip *dw_edma_qcom_probe(struct platform_device *pdev)
{
	struct resource *regs;
	struct dw_edma_chip *chip;
	struct dw_edma *dw;
	int i;

	/* Data structure allocation */
	chip = devm_kzalloc(&pdev->dev, sizeof(*chip), GFP_KERNEL);
	if (!chip)
		return ERR_PTR(-ENOMEM);

	dw = devm_kzalloc(&pdev->dev, sizeof(*dw), GFP_KERNEL);
	if (!dw)
		return ERR_PTR(-ENOMEM);

	/* Data structure initialization */
	chip->dw = dw;
	chip->dev = &pdev->dev;
	chip->id = 0;
	chip->irq = platform_get_irq_byname(pdev, "edma");
	if (chip->irq < 0)
		return ERR_PTR(chip->irq);

	dw->mf = EDMA_MF_EDMA_UNROLL;
	dw->nr_irqs = 1;
	dw->ops = &dw_edma_qcom_core_ops;
	dw->wr_ch_cnt = 8;
	dw->rd_ch_cnt = 8;

	regs = platform_get_resource_byname(pdev, IORESOURCE_MEM, "edma");
	if (!regs)
		return ERR_PTR(-ENODEV);

	dw->rg_region.paddr = regs->start;
	dw->rg_region.sz = resource_size(regs);
	dw->rg_region.vaddr = devm_ioremap_resource(&pdev->dev, regs);
	if (IS_ERR(dw->rg_region.vaddr))
		return ERR_CAST(dw->rg_region.vaddr);

	dw->irq = devm_kcalloc(&pdev->dev, dw->nr_irqs, sizeof(*dw->irq), GFP_KERNEL);
	if (!dw->irq)
		return ERR_PTR(-ENOMEM);

	for (i = 0; i < dw->wr_ch_cnt; i++) {
		struct dw_edma_region *ll_region = &dw->ll_region_wr[i];
		dma_addr_t dma_addr;
		void *virt;

		// FIXME: the memory is not freed. This must be fixed!
		virt = dma_alloc_coherent(&pdev->dev, PAGE_SIZE, &dma_addr, GFP_KERNEL);
		if (!virt)
			return ERR_PTR(-ENOMEM);


		ll_region->vaddr = virt;
		ll_region->paddr = dma_addr;
		ll_region->sz = PAGE_SIZE;
	}

	for (i = 0; i < dw->rd_ch_cnt; i++) {
		struct dw_edma_region *ll_region = &dw->ll_region_rd[i];
		dma_addr_t dma_addr;
		void *virt;

		// FIXME: the memory is not freed. This must be fixed!
		virt = dma_alloc_coherent(&pdev->dev, PAGE_SIZE, &dma_addr, GFP_KERNEL);
		if (!virt)
			return ERR_PTR(-ENOMEM);


		ll_region->vaddr = virt;
		ll_region->paddr = dma_addr;
		ll_region->sz = PAGE_SIZE;
	}

	return chip;
}

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Qualcomm Snapdragon glue driver for Synopsys DesignWare eDMA");
MODULE_AUTHOR("Dmitry Baryshkov <dmitry.baryshkov@linaro.org>");
