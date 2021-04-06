// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2020, The Linux Foundation. All rights reserved.
 */

#include <linux/module.h>
#include <linux/bitops.h>
#include <linux/of.h>
#include <linux/io.h>
#include <linux/clk.h>
#include <linux/platform_device.h>
#include <linux/of_platform.h>
#include <linux/kernel.h>
#include <linux/regulator/consumer.h>
#include <linux/gpio/consumer.h>
#include <linux/of_gpio.h>
#include <linux/reset.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/uaccess.h>
#include <linux/reset.h>
#include <linux/delay.h>
#include <linux/pci-epf.h>
#include <linux/pci-epc.h>
#include <linux/phy/phy.h>
#include <linux/pm_domain.h>
#include <asm/io.h>

#include <linux/dma-mapping.h>
#include <linux/platform_device.h>
#include "pcie-designware.h"

#define PCIE_EP_PARF_SYS_CTRL				0x00
#define PCIE_EP_PARF_DB_CTRL				0x10
#define PCIE_EP_PARF_PM_CTRL				0x20
#define PCIE_EP_PARF_PM_STTS				0x24
#define PCIE_EP_PARF_PHY_CTRL				0x40
#define PCIE_EP_PARF_PHY_REFCLK				0x4C
#define PCIE_EP_PARF_CONFIG_BITS			0x50
#define PCIE_EP_PARF_TEST_BUS				0xE4
#define PCIE_EP_PARF_MHI_BASE_ADDR_LOWER		0x178
#define PCIE_EP_PARF_MHI_BASE_ADDR_UPPER		0x17c
#define PCIE_EP_PARF_MSI_GEN				0x188
#define PCIE_EP_PARF_DEBUG_INT_EN			0x190
#define PCIE_EP_PARF_MHI_IPA_DBS			0x198
#define PCIE_EP_PARF_MHI_IPA_CDB_TARGET_LOWER		0x19C
#define PCIE_EP_PARF_MHI_IPA_EDB_TARGET_LOWER		0x1A0
#define PCIE_EP_PARF_AXI_MSTR_RD_HALT_NO_WRITES		0x1A4
#define PCIE_EP_PARF_AXI_MSTR_WR_ADDR_HALT		0x1A8
#define PCIE_EP_PARF_Q2A_FLUSH				0x1AC
#define PCIE_EP_PARF_LTSSM				0x1B0
#define PCIE_EP_PARF_CFG_BITS				0x210
#define PCIE_EP_PARF_LTR_MSI_EXIT_L1SS			0x214
#define PCIE_EP_PARF_INT_ALL_STATUS			0x224
#define PCIE_EP_PARF_INT_ALL_CLEAR			0x228
#define PCIE_EP_PARF_INT_ALL_MASK			0x22C
#define PCIE_EP_PARF_SLV_ADDR_MSB_CTRL			0x2C0
#define PCIE_EP_PARF_DBI_BASE_ADDR			0x350
#define PCIE_EP_PARF_DBI_BASE_ADDR_HI			0x354
#define PCIE_EP_PARF_SLV_ADDR_SPACE_SIZE		0x358
#define PCIE_EP_PARF_SLV_ADDR_SPACE_SIZE_HI		0x35C
#define PCIE_EP_PARF_ATU_BASE_ADDR			0x634
#define PCIE_EP_PARF_ATU_BASE_ADDR_HI			0x638
#define PCIE_EP_PARF_DEVICE_TYPE			0x1000

#define PCIE_EP_ELBI_VERSION				0x00
#define PCIE_EP_ELBI_SYS_CTRL				0x04
#define PCIE_EP_ELBI_SYS_STTS				0x08
#define PCIE_EP_ELBI_CS2_ENABLE				0xA4

#define PCIE_EP_DEVICE_ID_VENDOR_ID			0x00
#define PCIE_EP_DEVICE_ID_MASK				0xffff0000
#define PCIE_EP_VENDOR_ID_MASK				0xffff

#define PCIE_EP_COMMAND_STATUS				0x04

#define PCIE_EP_CLASS_CODE_REV_ID			0x08
#define PCIE_EP_CLASS_CODE_REV_ID_BASE_CLASS_CODE_MASK	0xff000000
#define PCIE_EP_CLASS_CODE_REV_ID_SUBCLASS_CODE_MASK	0xff0000
#define PCIE_EP_CLASS_CODE_REV_ID_PROG_IFACE_MASK	0xff00
#define PCIE_EP_CLASS_CODE_REV_ID_REV_ID_MASK		0xff

#define PCIE_EP_BIST_HDR_TYPE				0x0C
#define PCIE_EP_BIST_HDR_TYPE_CACHE_LINE_SIZE_MASK	0xff

#define PCIE_EP_DWC_BAR0				0x10
#define PCIE_EP_DWC_BAR1				0x14
#define PCIE_EP_DWC_BAR2				0x18
#define PCIE_EP_DWC_BAR3				0x1c
#define PCIE_EP_DWC_BAR4				0x20
#define PCIE_EP_DWC_BAR5				0x24

#define PCIE_EP_SUBSYSTEM				0x2c
#define PCIE_EP_SUBSYS_DEV_ID_MASK			0xffff0000
#define PCIE_EP_SUBSYS_VENDOR_ID_MASK			0xffff

#define PCIE_EP_CAP_ID_NXT_PTR				0x40
#define PCIE_EP_CON_STATUS				0x44
#define PCIE_EP_MSI_CAP_ID_NEXT_CTRL			0x50
#define PCIE_EP_MSI_LOWER				0x54
#define PCIE_EP_MSI_UPPER				0x58
#define PCIE_EP_MSI_DATA				0x5C
#define PCIE_EP_MSI_MASK				0x60
#define PCIE_EP_DEVICE_CAPABILITIES			0x74
#define PCIE_EP_MASK_EP_L1_ACCPT_LATENCY		0xE00
#define PCIE_EP_MASK_EP_L0S_ACCPT_LATENCY		0x1C0
#define PCIE_EP_LINK_CAPABILITIES			0x7C
#define PCIE_EP_MASK_CLOCK_POWER_MAN			0x40000
#define PCIE_EP_MASK_L1_EXIT_LATENCY			0x38000
#define PCIE_EP_MASK_L0S_EXIT_LATENCY			0x7000
#define PCIE_EP_CAP_LINKCTRLSTATUS			0x80
#define PCIE_EP_DEVICE_CONTROL2_STATUS2			0x98
#define PCIE_EP_LINK_CONTROL2_LINK_STATUS2		0xA0
#define PCIE_EP_L1SUB_CAPABILITY			0x154
#define PCIE_EP_L1SUB_CONTROL1				0x158
#define PCIE_EP_ACK_F_ASPM_CTRL_REG			0x70C
#define PCIE_EP_MASK_ACK_N_FTS				0xff00
#define PCIE_EP_MISC_CONTROL_1				0x8BC

#define PCIE_EP_PLR_IATU_VIEWPORT			0x900
#define PCIE_EP_PLR_IATU_CTRL1				0x904
#define PCIE_EP_PLR_IATU_CTRL2				0x908
#define PCIE_EP_PLR_IATU_LBAR				0x90C
#define PCIE_EP_PLR_IATU_UBAR				0x910
#define PCIE_EP_PLR_IATU_LAR				0x914
#define PCIE_EP_PLR_IATU_LTAR				0x918
#define PCIE_EP_PLR_IATU_UTAR				0x91c

#define PCIE_EP_IATU_BASE(n)				(n * 0x200)

#define PCIE_EP_IATU_I_CTRL1(n)			(PCIE_EP_IATU_BASE(n) + 0x100)
#define PCIE_EP_IATU_I_CTRL2(n)			(PCIE_EP_IATU_BASE(n) + 0x104)
#define PCIE_EP_IATU_I_LBAR(n)			(PCIE_EP_IATU_BASE(n) + 0x108)
#define PCIE_EP_IATU_I_UBAR(n)			(PCIE_EP_IATU_BASE(n) + 0x10c)
#define PCIE_EP_IATU_I_LAR(n)			(PCIE_EP_IATU_BASE(n) + 0x110)
#define PCIE_EP_IATU_I_LTAR(n)			(PCIE_EP_IATU_BASE(n) + 0x114)
#define PCIE_EP_IATU_I_UTAR(n)			(PCIE_EP_IATU_BASE(n) + 0x118)

#define PCIE_EP_MHICFG					0x110
#define PCIE_EP_BHI_EXECENV				0x228
#define PCIE_EP_MHIVER					0x108
#define PCIE_EP_MHICTRL					0x138
#define PCIE_EP_MHISTATUS				0x148
#define PCIE_EP_BHI_VERSION_LOWER			0x200
#define PCIE_EP_BHI_VERSION_UPPER			0x204
#define PCIE_EP_BHI_INTVEC				0x220

#define PCIE_EP_AUX_CLK_FREQ_REG			0xB40

#define PERST_TIMEOUT_US_MIN				1000
#define PERST_TIMEOUT_US_MAX				1000
#define PERST_CHECK_MAX_COUNT				30000
#define LINK_UP_TIMEOUT_US_MIN				1000
#define LINK_UP_TIMEOUT_US_MAX				1000
#define LINK_UP_CHECK_MAX_COUNT				30000
#define BME_TIMEOUT_US_MIN				1000
#define BME_TIMEOUT_US_MAX				1000
#define BME_CHECK_MAX_COUNT				30000
#define PHY_STABILIZATION_DELAY_US_MIN	      		1000
#define PHY_STABILIZATION_DELAY_US_MAX			1000
#define REFCLK_STABILIZATION_DELAY_US_MIN		1000
#define REFCLK_STABILIZATION_DELAY_US_MAX		1000
#define PHY_READY_TIMEOUT_COUNT				30000
#define MSI_EXIT_L1SS_WAIT				10
#define MSI_EXIT_L1SS_WAIT_MAX_COUNT			100
#define XMLH_LINK_UP					0x400
#define PARF_XMLH_LINK_UP				0x40000000
#define EP_CORE_RESET_TIME_MIN				1000
#define EP_CORE_RESET_TIME_MAX				1005
#define EP_CORE_LINKDOWN				0xffffffff
#define EP_MHICFG					0x2800880
#define EP_BHI_EXECENV					2
#define EP_MHICTRL_INIT					0x0
#define EP_MHISTATUS_INIT				0x0
#define EP_MHIVER_INIT					0x1000000
#define EP_BHI_VERSION_LOWER_DATA			0x2
#define EP_BHI_VERSION_UPPER_DATA			0x1
#define EP_BHI_INTVEC_VAL				0xffffffff
#define EP_PCIE_INT_MAX				13
/* 2ms */
#define WAKE_DELAY_US				2000
#define TCSR_PERST_SEPARATION_ENABLE		0x270

#define to_pcie_ep(x)	dev_get_drvdata((x)->dev)

struct qcom_pcie_ep *pcie_ep;
struct pcie_ep_plat_data *pcie_ep_pdata;

enum qcom_pcie_ep_irq {
	EP_PCIE_INT_PM_TURNOFF,
	EP_PCIE_INT_DSTATE_CHANGE,
	EP_PCIE_INT_L1SUB_TIMEOUT,
	EP_PCIE_INT_LINK_UP,
	EP_PCIE_INT_LINK_DOWN,
	EP_PCIE_INT_BRIDGE_FLUSH_N,
	EP_PCIE_INT_BME,
	EP_PCIE_INT_MHI_A7,
	EP_PCIE_INT_GLOBAL,
	EP_PCIE_MAX_IRQ,
};

enum qcom_ep_pcie_link_state {
	/* Controller is configured but LTSSM is not enabled */
	EP_PCIE_LINK_CONFIGURED,
	/* Parf register link_up bit is set */
	EP_PCIE_LINK_UP,
	/* LTSSM state indicates link is up */
	EP_PCIE_LINK_LTSSM_EN,
	/* Link enumerated, i.e link is up, BME is set */
	EP_PCIE_LINK_ENUMERATED,
	/* Link disabled and not configured */
	EP_PCIE_LINK_DISABLE,
	/* Link down detected */
	EP_PCIE_LINK_DOWN,
};

struct qcom_pcie_ep_ops {
	/* Configure vregs and clocks */
	int (*enable_resources)(struct qcom_pcie_ep *pcie);
	void (*disable_resources)(struct qcom_pcie_ep *pcie);
	/* Initialize MHI MMIO */
	void (*mmio_init)(struct qcom_pcie_ep *pcie);
	/* Initialize PCIe controller core */
	int (*core_init)(struct qcom_pcie_ep *pcie);
	/* Reset PCIe controller core */
	int (*core_reset)(struct qcom_pcie_ep *pcie);
	/* Enable PCIe global IRQ's */
	void (*configure_irq)(struct qcom_pcie_ep *pcie);
	/* GPIO related functions */
	void (*toggle_wake)(struct qcom_pcie_ep *pcie);
	int (*check_perst)(struct qcom_pcie_ep *pcie);
	/* Update and enumerate the internal PCIe link status */
	void (*enumerate)(struct qcom_pcie_ep *pcie);
	/* Start link training sequence */
	void (*enable_ltssm)(struct qcom_pcie_ep *pcie);
	/* Misc: Configure TCSR related PCIe configuration */
	void (*configure_tcsr)(struct qcom_pcie_ep *pcie);
	/* Check if BME is set */
	void (*check_bme)(struct qcom_pcie_ep *pcie);
	/* Check if link is already configured in Bootloader */
	int (*pcie_early_init)(struct qcom_pcie_ep *pcie);
};

static const struct pci_epf_header mdm_prairie_ep_header = {
	.vendorid		= 0x17cb,
	.deviceid		= 0x306,
	.revid			= 0x0,
	.progif_code		= 0x0,
	.subclass_code		= 0x0,
	.baseclass_code		= 0xff,
	.cache_line_size	= 0x10,
	.subsys_vendor_id	= 0x0,
	.subsys_id		= 0x0,
};

struct pcie_ep_plat_data {
	u32				link_speed;
	struct pci_epf_header		*header;
	const struct qcom_pcie_ep_ops	*ops;
};

struct qcom_pcie_ep_resources {
	struct clk		*ahb_clk;
	struct clk		*axi_m;
	struct clk		*axi_s;
	struct clk		*aux_clk;	/* Set rate: 1000000 */
	struct clk		*ldo;
	struct clk		*sleep_clk;
	struct clk		*slave_q2a_axi_clk;
	struct clk		*pipe_clk;	/* Set rate: 62500000 */

	struct reset_control	*core_reset;

	struct regulator	*vdda;
	struct regulator	*vdda_phy;
	struct device		*gdsc;

	struct gpio_desc	*reset;
	struct gpio_desc	*wake;
	struct gpio_desc	*clkreq;
};

struct qcom_pcie_ep_state {
	bool				mmio_init;
	bool				core_configured;
	/* Store subsys id for restore after D3_COLD */
	u32				subsys_id;
	u32				sys_id;
	/* Local state of the link state */
	enum qcom_ep_pcie_link_state	link_state;
};

struct qcom_pcie_ep {
	struct device			*dev;

	struct dw_pcie			*pci;

	void __iomem			*parf;
	void __iomem			*mmio;
	void __iomem			*msi;
	void __iomem			*elbi;
	void __iomem			*tcsr;
	void __iomem			*phys_base;
	int				phys_addr_size;

	struct qcom_pcie_ep_resources	*res;

	struct phy			*phy;

	struct qcom_pcie_ep_state	state;

	const struct pcie_ep_plat_data	*data;

	spinlock_t			res_lock;

	struct mutex			lock;

	int				perst_irq;
};

static void qcom_pcie_ep_enable_ltssm(struct qcom_pcie_ep *pcie_ep)
{
	u32 reg;

	/* enable link training */
	reg = readl(pcie_ep->parf + PCIE_EP_PARF_LTSSM);
	reg |= BIT(8);
	writel_relaxed(reg, pcie_ep->parf + PCIE_EP_PARF_LTSSM);
}

static int qcom_pcie_ep_core_reset(struct qcom_pcie_ep *pcie_ep)
{
	struct device *dev = pcie_ep->dev;
	struct qcom_pcie_ep_resources *res = pcie_ep->res;
	int ret = 0;

	ret = reset_control_assert(res->core_reset);
	if (ret) {
		dev_err(dev, "cannot assert core\n");
		return ret;
	}

	usleep_range(EP_CORE_RESET_TIME_MIN, EP_CORE_RESET_TIME_MAX);

	ret = reset_control_deassert(res->core_reset);
	if (ret) {
		dev_err(dev, "cannot assert core\n");
		return ret;
	}

	return 0;
}

static int qcom_pcie_ep_pcie_early_init(struct qcom_pcie_ep *pcie_ep)
{
	struct dw_pcie *pci = pcie_ep->pci;
	u32 reg;

	/* Check link status */
	reg = readl(pcie_ep->parf + PCIE_EP_PARF_PM_STTS);
	pr_info("pcie_ep: parf_pm_stts:0x%x\n", reg);
	if (reg & PARF_XMLH_LINK_UP) {
		pr_info("pcie_ep: Link already initialized in bootloader\n");
		/*
		 * Read and store subsystem ID set in bootloader
		 * and restore it during D3 to D0 state.
		 */
		pcie_ep->state.subsys_id = dw_pcie_readl_dbi(pci, PCIE_EP_SUBSYSTEM);
		/* MMIO is already initialized in bootloader */
		pcie_ep->state.mmio_init = true;
		pcie_ep->state.link_state = EP_PCIE_LINK_UP;
		pcie_ep->data->ops->check_bme(pcie_ep);

		return 0;
	} else {
		reg = readl(pcie_ep->parf + PCIE_EP_PARF_LTSSM) & BIT(8);
		if (reg) {
			pr_info("pcie_ep: Link is not up with LTSSM set\n");
			return -ENODEV;
		}
	}

	return -ENODEV;
}

static int qcom_pcie_ep_check_perst(struct qcom_pcie_ep *pcie_ep)
{
	u32 retries = 0;
	struct qcom_pcie_ep_resources *res = pcie_ep->res;
	/* wait for host side to deassert PERST */
	do {
		if (gpiod_get_value(res->reset) == 1)
			break;
		retries++;
		usleep_range(PERST_TIMEOUT_US_MIN, PERST_TIMEOUT_US_MAX);
	} while (retries < PERST_CHECK_MAX_COUNT);

	pr_info("pcie_ep: number of PERST retries: %d\n", retries);

	if (retries == PERST_CHECK_MAX_COUNT)
		return -ENODEV;
	else
		return 0;
}

static void qcom_pcie_ep_toggle_wake(struct qcom_pcie_ep *pcie_ep)
{
	struct qcom_pcie_ep_resources *res = pcie_ep->res;

	/* assert PCIe WAKE# */
	pr_info("pcie_ep: WAKE# GPIO initial:%d\n",
		gpiod_get_value(res->wake));
	
	gpiod_set_value_cansleep(res->wake, 0);
}

static void qcom_pcie_ep_configure_irq(struct qcom_pcie_ep *pcie_ep)
{
	u32 reg;

	writel_relaxed(0, pcie_ep->parf + PCIE_EP_PARF_INT_ALL_MASK);
	reg = BIT(EP_PCIE_INT_LINK_DOWN) |
		BIT(EP_PCIE_INT_BME) |
		BIT(EP_PCIE_INT_PM_TURNOFF) |
		BIT(EP_PCIE_INT_DSTATE_CHANGE) |
		BIT(EP_PCIE_INT_LINK_UP);
	writel_relaxed(reg, pcie_ep->parf + PCIE_EP_PARF_INT_ALL_MASK);
	pr_info("pcie_ep: PARF interrupt enable:0x%x\n", reg);
}

static void qcom_pcie_ep_mmio_init(struct qcom_pcie_ep *ep)
{
	if (ep->state.mmio_init) {
		pr_info("EP MMIO alreadly initialized\n");
		return;
	}

	writel_relaxed(EP_MHICFG, ep->mmio + PCIE_EP_MHICFG);
	writel_relaxed(EP_BHI_EXECENV, ep->mmio + PCIE_EP_BHI_EXECENV);
	writel_relaxed(EP_MHICTRL_INIT, ep->mmio + PCIE_EP_MHICTRL);
	writel_relaxed(EP_MHISTATUS_INIT, ep->mmio + PCIE_EP_MHISTATUS);
	writel_relaxed(EP_MHIVER_INIT, ep->mmio + PCIE_EP_MHIVER);
	writel_relaxed(EP_BHI_VERSION_LOWER_DATA,
			ep->mmio + PCIE_EP_BHI_VERSION_LOWER);
	writel_relaxed(EP_BHI_VERSION_UPPER_DATA,
			ep->mmio + PCIE_EP_BHI_VERSION_UPPER);
	writel_relaxed(EP_BHI_INTVEC_VAL, ep->mmio + PCIE_EP_BHI_INTVEC);

	ep->state.mmio_init = true;
}

#if 0
static void qcom_pcie_ep_wake_assert(struct qcom_pcie_ep *ep)
{
	gpiod_set_value_cansleep(ep->wake, 1);
	usleep_range(WAKE_DELAY_US, WAKE_DELAY_US + 500);
}
#endif

static void qcom_pcie_ep_wake_deassert(struct qcom_pcie_ep *pcie_ep)
{
	struct qcom_pcie_ep_resources *res = pcie_ep->res;

	gpiod_set_value_cansleep(res->wake, 0);
	usleep_range(WAKE_DELAY_US, WAKE_DELAY_US + 500);
}

static const struct pci_epc_features qcom_pcie_epc_features = {
	.linkup_notifier = true,
	.msi_capable = true,
	.msix_capable = false,
};

static const struct pci_epc_features *
qcom_pcie_epc_get_features(struct dw_pcie_ep *pci_ep)
{
	return &qcom_pcie_epc_features;
}

static void qcom_pcie_ep_init(struct dw_pcie_ep *ep)
{
	struct dw_pcie *pci = to_dw_pcie_from_ep(ep);
	enum pci_barno bar;

	for (bar = BAR_0; bar <= BAR_5; bar++)
		dw_pcie_ep_reset_bar(pci, bar);
}

static struct dw_pcie_ep_ops pci_ep_ops = {
	.ep_init = qcom_pcie_ep_init,
	.get_features = qcom_pcie_epc_get_features,
};

static int qcom_pcie_ep_enable_resources(struct qcom_pcie_ep *pcie_ep)
{
	struct qcom_pcie_ep_resources *res = pcie_ep->res;
	struct device *dev = pcie_ep->dev;
	int ret = 0;

	pr_info("%s: %d",__func__, __LINE__);
	ret = regulator_enable(res->vdda);
	if (ret) {
		dev_err(dev, "Cannot enable vdda\n");
		return ret;
	}

	ret = regulator_enable(res->vdda_phy);
	if (ret) {
		dev_err(dev, "Cannot prepare vdda phy\n");
		return ret;
	}

	ret = clk_prepare_enable(res->ahb_clk);
	if (ret) {
		dev_err(dev, "Cannot prepare AHB clock\n");
		return ret;
	}

	ret = clk_prepare_enable(res->aux_clk);
	if (ret) {
		dev_err(dev, "Cannot prepare aux clock\n");
		return ret;
	}

	ret = clk_prepare_enable(res->axi_m);
	if (ret) {
		dev_err(dev, "Cannot prepare axi master clock\n");
		return ret;
	}

	ret = clk_prepare_enable(res->axi_s);
	if (ret) {
		dev_err(dev, "Cannot prepare axi slave clock\n");
		return ret;
	}

	ret = clk_prepare_enable(res->ldo);
	if (ret) {
		dev_err(dev, "Cannot prepare LDO clock\n");
		return ret;
	}

	ret = clk_prepare_enable(res->sleep_clk);
	if (ret) {
		dev_err(dev, "Cannot prepare sleep clock\n");
		return ret;
	}

	ret = clk_prepare_enable(res->slave_q2a_axi_clk);
	if (ret) {
		dev_err(dev, "Cannot prepare slave_bus clock\n");
		return ret;
	}

	ret = clk_prepare_enable(res->pipe_clk);
	if (ret) {
		dev_err(dev, "Cannot enable pipe clock\n");
		return ret;
	}

	return 0;
}

static void qcom_pcie_ep_disable_resources(struct qcom_pcie_ep *pcie_ep)
{
	struct qcom_pcie_ep_resources *res = pcie_ep->res;	

	clk_disable_unprepare(res->slave_q2a_axi_clk);
	clk_disable_unprepare(res->pipe_clk);
	clk_disable_unprepare(res->sleep_clk);
	clk_disable_unprepare(res->ldo);
	clk_disable_unprepare(res->aux_clk);
	clk_disable_unprepare(res->axi_s);
	clk_disable_unprepare(res->axi_m);
	clk_disable_unprepare(res->ahb_clk);
	regulator_disable(res->vdda_phy);
	regulator_disable(res->vdda);
}

static void qcom_pcie_ep_enumerate(struct qcom_pcie_ep *pcie_ep)
{
	struct dw_pcie *pci = pcie_ep->pci;
	if (pcie_ep->state.link_state == EP_PCIE_LINK_ENUMERATED) {
		pr_info("PCIe EP link already enumerated\n");
		return;
	}

	pcie_ep->state.sys_id = dw_pcie_readl_dbi(pci, 0);
	pcie_ep->state.link_state = EP_PCIE_LINK_ENUMERATED;
}

static void qcom_pcie_ep_check_bme(struct qcom_pcie_ep *pcie_ep)
{
	struct dw_pcie *pci = pcie_ep->pci;
	/*
	 * De-assert WAKE# GPIO following link until L2/3 and WAKE#
	 * is triggered to send data from device to host at which point
	 * it will assert WAKE#.
	 */
	qcom_pcie_ep_wake_deassert(pcie_ep);

	dw_pcie_writel_dbi(pci, PCIE_EP_AUX_CLK_FREQ_REG, 0x14);
	if (dw_pcie_readl_dbi(pci, PCIE_EP_COMMAND_STATUS) & BIT(2)) {
		pr_info("pcie_ep: BME is set\n");
		pcie_ep->data->ops->enumerate(pcie_ep);
	}
}

static void qcom_pcie_ep_configure_tcsr(struct qcom_pcie_ep *pcie_ep)
{
	u32 reg;

	reg = readl_relaxed(pcie_ep->tcsr + 0x258);
	pr_info("pcie_ep: TSCR PERST_EN:val:0x%x\n", reg);
	writel_relaxed(reg, pcie_ep->tcsr + 0x258);

	reg = readl_relaxed(pcie_ep->tcsr + TCSR_PERST_SEPARATION_ENABLE);
	pr_info("pcie_ep: TSCR PERST_SEP_EN:val:0x%x\n", reg);
	writel_relaxed(reg, pcie_ep->tcsr + TCSR_PERST_SEPARATION_ENABLE);
}

static int qcom_pcie_confirm_linkup(struct dw_pcie *pci)
{
	struct qcom_pcie_ep *pcie_ep = to_pcie_ep(pci);
	u32 reg;

	reg = readl_relaxed(pcie_ep->elbi + PCIE_EP_ELBI_SYS_STTS);
	pr_info("pcie_ep:elbi_sys_stts:0x%x\n", reg);
	if (!(reg & XMLH_LINK_UP))
		return -ENODEV;

	return 0;
}

static inline void ep_pcie_write_mask(void __iomem *addr,
                                u32 clear_mask, u32 set_mask)
{
        u32 val;

        val = (readl_relaxed(addr) & ~clear_mask) | set_mask;
        writel_relaxed(val, addr);
        /* ensure register write goes through before next regiser operation */
        wmb();
}

static int qcom_pcie_ep_core_init(struct qcom_pcie_ep *pcie_ep)
{
	struct dw_pcie *pci = pcie_ep->pci;
	struct pci_epc *epc = pci->ep.epc;
	struct pci_epf_header *hdr = pcie_ep->data->header;

	/* enable debug IRQ */
	writel_relaxed((BIT(3) | BIT(2) | BIT(1)),
			pcie_ep->parf + PCIE_EP_PARF_DEBUG_INT_EN);

	/* Configure PCIe to endpoint mode */
	writel_relaxed(0x0, pcie_ep->parf + PCIE_EP_PARF_DEVICE_TYPE);

	/* adjust DBI base address */
	writel_relaxed(0x3FFFE000, pcie_ep->parf + PCIE_EP_PARF_DBI_BASE_ADDR);

	/* Configure PCIe core to support 1GB aperture */
	writel_relaxed(0x40000000, pcie_ep->parf + PCIE_EP_PARF_SLV_ADDR_SPACE_SIZE);

	writel_relaxed(0x101, pcie_ep->parf + PCIE_EP_PARF_PM_CTRL);

	/* Configure Slave address, DBI and iATU */
	writel_relaxed(0x0, pcie_ep->parf + PCIE_EP_PARF_SLV_ADDR_MSB_CTRL);
	writel_relaxed(0x200, pcie_ep->parf + PCIE_EP_PARF_SLV_ADDR_SPACE_SIZE_HI);
	writel_relaxed(0x0, pcie_ep->parf + PCIE_EP_PARF_SLV_ADDR_SPACE_SIZE);
	writel_relaxed(0x100, pcie_ep->parf + PCIE_EP_PARF_DBI_BASE_ADDR_HI);
	writel_relaxed(pci->atu_base, pcie_ep->parf + PCIE_EP_PARF_DBI_BASE_ADDR);
	writel_relaxed(0x100, pcie_ep->parf + PCIE_EP_PARF_ATU_BASE_ADDR_HI);
	writel_relaxed(pci->atu_base, pcie_ep->parf + PCIE_EP_PARF_ATU_BASE_ADDR);
	
	dw_pcie_writel_dbi(pci, PCIE_EP_LINK_CONTROL2_LINK_STATUS2,
				pcie_ep->data->link_speed);

	/* Read halts write */
	writel_relaxed(0, pcie_ep->parf + PCIE_EP_PARF_AXI_MSTR_RD_HALT_NO_WRITES);
	/* Write after write halt */
	writel_relaxed(BIT(31), pcie_ep->parf + PCIE_EP_PARF_AXI_MSTR_WR_ADDR_HALT);
	/* Q2A flush disable */
	writel_relaxed(0, pcie_ep->parf + PCIE_EP_PARF_Q2A_FLUSH);
	/* Disable the DBI Wakeup */
	writel_relaxed(BIT(11), pcie_ep->parf + PCIE_EP_PARF_SYS_CTRL);
	/* Disable the debouncers */
	writel_relaxed(0x73, pcie_ep->parf + PCIE_EP_PARF_DB_CTRL);
	/* Disable core clock CGC */
	writel_relaxed(BIT(6), pcie_ep->parf + PCIE_EP_PARF_SYS_CTRL);
	/* Set AUX power to be on */
	writel_relaxed(BIT(4), pcie_ep->parf + PCIE_EP_PARF_SYS_CTRL);
	/* Request to exit from L1SS for MSI and LTR MSG */
	writel_relaxed(BIT(1), pcie_ep->parf + PCIE_EP_PARF_CFG_BITS);

	/* Update config space header information */
	pci->ep.epc->ops->write_header(epc, 0, hdr);

	dw_pcie_dbi_ro_wr_en(pci);
	/* Set the PMC Register - to support PME in D0/D3hot/D3cold */
	dw_pcie_writel_dbi(pci, PCIE_EP_CAP_ID_NXT_PTR,
			(BIT(31) | BIT(30) | BIT(27)));
	/* Set the Endpoint L0s Acceptable Latency to 1us (max) */
	dw_pcie_writel_dbi(pci, PCIE_EP_DEVICE_CAPABILITIES, 0x7);
	/* Set the L0s Exit Latency to 2us-4us = 0x6 */
	/* Set the L1 Exit Latency to be 32us-64 us = 0x6 */
	dw_pcie_writel_dbi(pci, PCIE_EP_LINK_CAPABILITIES, 0x6);
	/* L1ss is supported */
	dw_pcie_writel_dbi(pci, PCIE_EP_L1SUB_CAPABILITY, 0x1f);
	/* Enable Clock Power Management */
	dw_pcie_writel_dbi(pci, PCIE_EP_LINK_CAPABILITIES, 0x1);
	dw_pcie_dbi_ro_wr_dis(pci);

	/* Set FTS value to match the PHY setting */
	dw_pcie_writel_dbi(pci, PCIE_EP_ACK_F_ASPM_CTRL_REG, 0x80);
	dw_pcie_writel_dbi(pci, PCIE_EP_AUX_CLK_FREQ_REG, 0x14);

	/* Enable L1 */
	writel_relaxed(BIT(5), pcie_ep->parf + PCIE_EP_PARF_PM_CTRL);

	/* Configure aggregated IRQ's */
	pcie_ep->data->ops->configure_irq(pcie_ep);

	/* Configure MMIO */
//	pcie_ep->data->ops->mmio_init(pcie_ep);

	return 0;
}

static int qcom_pcie_establish_link(struct dw_pcie *pci)
{
	struct qcom_pcie_ep *pcie_ep = to_pcie_ep(pci);
	struct pci_epc *epc = pci->ep.epc;
	struct pci_epf_bar epf_bar;
	int ret;

	if (pcie_ep->state.link_state == EP_PCIE_LINK_ENUMERATED) {
		pr_err("Link is already enumerated");
		return 0;
	}

	mutex_lock(&pcie_ep->lock);

	/* Enable power and clocks */
	ret = pcie_ep->data->ops->enable_resources(pcie_ep);
	if (ret) {
		pr_err("pcie_ep: Enable resources failed\n");
		return ret;
	}

	/* Configure tcsr to avoid device reset during host reboot */
	pcie_ep->data->ops->configure_tcsr(pcie_ep);

	/* Check if link is initialized in bootloader */
	ret = pcie_ep->data->ops->pcie_early_init(pcie_ep);
	if (ret == -ENODEV) {
		pr_info("pcie_ep: pcie early init failure %d\n", ret);
	} else {
		pr_info("pcie_ep: link initialized in bootloader\n");
		goto exit;
	}

	/* Perform controller reset */
	ret = pcie_ep->data->ops->core_reset(pcie_ep);
	if (ret) {
		pr_info("pcie_ep: Failed to reset the core\n");
		goto disable_resource;
	}

	/* Assert WAKE# to RC to indicate device is ready */
	pcie_ep->data->ops->toggle_wake(pcie_ep);

	/* Check for PERST deassertion from host */
	ret = pcie_ep->data->ops->check_perst(pcie_ep);
	if (ret) {
		pr_info("pcie_ep: Failed to detect perst deassert\n");
		goto disable_resource;
	}

	/* Initialize PHY */
	ret = phy_init(pcie_ep->phy);
	if (ret) {
		pr_info("pcie_ep: PHY init failed\n");
		goto disable_resource;
	}

	/* TODO: check for phy is ready */


	/* Initialize the controller */
	ret = pcie_ep->data->ops->core_init(pcie_ep);
	if (ret) {
		pr_info("pcie_ep: Controller init failed\n");
		goto disable_resource;
	}

	/* Set the BAR and program iATU */
	epf_bar.phys_addr = pcie_ep->phys_base;
	epf_bar.size = pcie_ep->phys_addr_size;
	epf_bar.barno = BAR_0;
	epf_bar.flags = PCI_BASE_ADDRESS_SPACE;
	ret = pci->ep.epc->ops->set_bar(epc, 0, &epf_bar);
	if (ret) {
		pr_info("pcie_ep: setting BAR and ATU mapping failed\n");
		goto disable_resource;
	}

	/* Enable LTSSM */
	pcie_ep->data->ops->enable_ltssm(pcie_ep);

	qcom_pcie_ep_wake_deassert(pcie_ep);

	ret = dw_pcie_wait_for_link(pci);
	if (ret)
		pr_err("Link training failed");

	mutex_unlock(&pcie_ep->lock);

	return 0;
	
disable_resource:
	pcie_ep->data->ops->disable_resources(pcie_ep);
exit:
	mutex_unlock(&pcie_ep->lock);

	return 0;
}

static void qcom_pcie_disable_link(struct dw_pcie *pci)
{
	struct qcom_pcie_ep *pcie_ep = to_pcie_ep(pci);

	return pcie_ep->data->ops->disable_resources(pcie_ep);
}

/* Device specific ops */
static struct qcom_pcie_ep_ops ops_mdm = {
	.enable_resources	= qcom_pcie_ep_enable_resources,
	.disable_resources	= qcom_pcie_ep_disable_resources,
	.mmio_init		= qcom_pcie_ep_mmio_init,
	.core_init		= qcom_pcie_ep_core_init,
	.core_reset		= qcom_pcie_ep_core_reset,
	.configure_irq		= qcom_pcie_ep_configure_irq,
	.toggle_wake		= qcom_pcie_ep_toggle_wake,
	.check_perst		= qcom_pcie_ep_check_perst,
	.enumerate		= qcom_pcie_ep_enumerate,
	.enable_ltssm		= qcom_pcie_ep_enable_ltssm,
	.configure_tcsr		= qcom_pcie_ep_configure_tcsr,
	.check_bme		= qcom_pcie_ep_check_bme,
	.pcie_early_init	= qcom_pcie_ep_pcie_early_init,
};

static const struct pcie_ep_plat_data data_prairie_ep = {
	.link_speed		= 3,
	.header			= &mdm_prairie_ep_header,
	.ops			= &ops_mdm,
};

static irqreturn_t qcom_pcie_ep_clkreq_threaded_irq(int irq, void *data)
{
	pr_info("Received CLKREQ IRQ\n");

	return IRQ_HANDLED;
}

static irqreturn_t qcom_pcie_ep_perst_threaded_irq(int irq, void *data)
{
	struct qcom_pcie_ep *pcie_ep = data;
	struct dw_pcie *pci = pcie_ep->pci;
	struct qcom_pcie_ep_resources *res = pcie_ep->res;
	u32 perst;

	perst = gpiod_get_value(res->reset);

	pr_info("PCIe PERST is %sasserted\n", perst ? "de" : "");
	if (perst) {
		/* start work for link enumeration with the host side */
		pr_info("Start enumeration due to PERST deassertion\n");
		pci->ops->start_link(pci);
	} else {
		/* shutdown the link if the link is already on */
		pr_info("Shutdown the PCIe link\n");
		pci->ops->stop_link(pci);
	}

	/* Set trigger type based on the next expected value of perst gpio */
	irq_set_irq_type(gpiod_to_irq(res->reset),
		(perst ? IRQF_TRIGGER_LOW : IRQF_TRIGGER_HIGH));

	return IRQ_HANDLED;
}

/* Common DWC controller ops */
static const struct dw_pcie_ops pci_ops = {
	.link_up = qcom_pcie_confirm_linkup,
	.start_link = qcom_pcie_establish_link,
	.stop_link = qcom_pcie_disable_link,
};

static irqreturn_t qcom_pcie_ep_global_threaded_irq(int irq, void *data)
{
	struct qcom_pcie_ep *pcie_ep = data;
	struct dw_pcie *pci = pcie_ep->pci;
	u32 status = readl(pcie_ep->parf + PCIE_EP_PARF_INT_ALL_STATUS);
	u32 mask = readl(pcie_ep->parf + PCIE_EP_PARF_INT_ALL_MASK);
	u32 dstate, int_num;

	writel_relaxed(status, pcie_ep->parf + PCIE_EP_PARF_INT_ALL_CLEAR);
	status &= mask;

	for (int_num = 1; int_num <= EP_PCIE_INT_MAX; int_num++) {
		if (status & BIT(int_num)) {
			switch (int_num) {
			case EP_PCIE_INT_LINK_DOWN:
				pr_info("linkdown event\n");
				pcie_ep->data->ops->disable_resources(pcie_ep);
				break;
			case EP_PCIE_INT_BME:
				pr_info("handle BME event\n");
				pcie_ep->data->ops->enumerate(pcie_ep);
				break;
			case EP_PCIE_INT_PM_TURNOFF:
				pr_info("handle PM Turn-off event\n");
				pcie_ep->data->ops->disable_resources(pcie_ep);
				break;
			case EP_PCIE_INT_MHI_A7:
				pr_info("received MHI A7 event\n");
				break;
			case EP_PCIE_INT_DSTATE_CHANGE:
				dstate = dw_pcie_readl_dbi(
						pci, PCIE_EP_CON_STATUS) & 0x3;
				pr_info("Received D state:%x\n", dstate);
				break;
			case EP_PCIE_INT_LINK_UP:
				pr_info("linkup event\n");
				break;
			case EP_PCIE_INT_L1SUB_TIMEOUT:
				pr_info("L1ss timeout event\n");
				break;
			default:
				pr_err("Unexpected event %d\n", int_num);
			}
		}
	}

	return IRQ_HANDLED;
}

static int qcom_pcie_ep_enable_irq_resources(struct platform_device *pdev,
			struct qcom_pcie_ep *pcie_ep)
{
	struct qcom_pcie_ep_resources *res = pcie_ep->res;	
	int irq, ret;

	irq = platform_get_irq_byname(pdev, "int_global");
	if (irq < 0) {
		dev_err(&pdev->dev, "failed to get irq %s\n",
					"int_global");
		return irq;
	}

	ret = devm_request_threaded_irq(&pdev->dev, irq, NULL,
				qcom_pcie_ep_global_threaded_irq,
				IRQF_TRIGGER_HIGH | IRQF_ONESHOT,
				"int_global", pcie_ep);
	if (ret) {
		dev_err(&pdev->dev, "failed to get irq %s\n", "int_global");
		return ret;
	}

	irq = gpiod_to_irq(res->reset);
	ret = devm_request_threaded_irq(&pdev->dev, irq, NULL,
				qcom_pcie_ep_perst_threaded_irq,
				IRQF_TRIGGER_HIGH | IRQF_ONESHOT,
				"ep_pcie_perst", pcie_ep);
	if (ret) {
		dev_err(&pdev->dev, "failed to get irq %s\n", "ep_pcie_perst");
		return ret;
	}
	pcie_ep->perst_irq = irq;

	irq = gpiod_to_irq(res->clkreq);
	irq_set_status_flags(irq, IRQ_NOAUTOEN);
	ret = devm_request_threaded_irq(&pdev->dev, irq, NULL,
				qcom_pcie_ep_clkreq_threaded_irq,
				IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
				"ep_pcie_clkreq", pcie_ep);
	if (ret) {
		dev_err(&pdev->dev, "failed to get irq %s\n", "ep_pcie_clkreq");
		return ret;
	}
	//enable_irq_wake(irq);

	return 0;
}

static int qcom_pcie_ep_get_vreg_clock_resources(struct platform_device *pdev,
						struct qcom_pcie_ep *pcie_ep)
{
	struct qcom_pcie_ep_resources *res = pcie_ep->res;
	struct device *dev = &pdev->dev;

	res->vdda = devm_regulator_get(dev, "vdda");
	if (IS_ERR(res->vdda))
		return PTR_ERR(res->vdda);

	res->vdda_phy = devm_regulator_get(dev, "vdda_phy");
	if (IS_ERR(res->vdda_phy))
		return PTR_ERR(res->vdda_phy);

	res->gdsc = dev_pm_domain_attach(dev, true);
	if (IS_ERR(res->gdsc))
		return PTR_ERR(res->gdsc);

	res->ahb_clk = devm_clk_get(dev, "ahb_clk");
	if (IS_ERR(res->ahb_clk))
		return PTR_ERR(res->ahb_clk);

	res->axi_m = devm_clk_get(dev, "master_axi_clk");
	if (IS_ERR(res->axi_m))
		return PTR_ERR(res->axi_m);

	res->axi_s = devm_clk_get(dev, "slave_axi_clk");
	if (IS_ERR(res->axi_s))
		return PTR_ERR(res->axi_s);

	res->aux_clk = devm_clk_get(dev, "aux_clk");
	if (IS_ERR(res->aux_clk))
		return PTR_ERR(res->aux_clk);

	res->ldo = devm_clk_get(dev, "ldo");
	if (IS_ERR(res->ldo))
		return PTR_ERR(res->ldo);

	res->sleep_clk = devm_clk_get(dev, "sleep_clk");
	if (IS_ERR(res->sleep_clk))
		return PTR_ERR(res->sleep_clk);

	res->pipe_clk = devm_clk_get(dev, "pipe_clk");
	if (IS_ERR(res->pipe_clk))
		return PTR_ERR(res->pipe_clk);

	res->slave_q2a_axi_clk = devm_clk_get(dev, "slave_q2a_axi_clk");
	if (IS_ERR(res->slave_q2a_axi_clk))
		return PTR_ERR(res->slave_q2a_axi_clk);

	res->core_reset = devm_reset_control_get_exclusive(dev, "core_reset");
	if (IS_ERR(res->core_reset))
		return PTR_ERR(res->core_reset);

	return 0;
}

static int qcom_pcie_ep_get_gpio_resources(struct platform_device *pdev,
					struct qcom_pcie_ep *pcie_ep)
{
	struct device *dev = &pdev->dev;
	struct qcom_pcie_ep_resources *res = pcie_ep->res;

	res->reset = devm_gpiod_get_optional(dev, "perst", GPIOD_IN);
	if (IS_ERR(res->reset))
		return PTR_ERR(res->reset);

	res->wake = devm_gpiod_get_optional(dev, "wake", GPIOD_OUT_HIGH);
	if (IS_ERR(res->wake))
		return PTR_ERR(res->wake);

	res->clkreq = devm_gpiod_get_optional(dev, "clkreq", GPIOD_OUT_LOW);
	if (IS_ERR(res->clkreq))
		return PTR_ERR(res->clkreq);

	return 0;
}

static int qcom_pcie_ep_get_io_resources(struct platform_device *pdev,
					struct qcom_pcie_ep *pcie_ep)
{
	struct device *dev = &pdev->dev;
	struct dw_pcie *pci = pcie_ep->pci;
	struct resource *res;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "dbi");
	pci->dbi_base = devm_pci_remap_cfg_resource(dev, res);
	if (IS_ERR(pci->dbi_base))
		return PTR_ERR(pci->dbi_base);
	pci->dbi_base2 = pci->dbi_base;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "elbi");
	pcie_ep->elbi = devm_pci_remap_cfg_resource(dev, res);
	if (IS_ERR(pcie_ep->elbi))
		return PTR_ERR(pcie_ep->elbi);

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "iatu");
	pci->atu_base = devm_pci_remap_cfg_resource(dev, res);
	if (IS_ERR(pci->atu_base))
		return PTR_ERR(pci->atu_base);

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "parf");
	pcie_ep->parf = devm_ioremap_resource(dev, res);
	if (IS_ERR(pcie_ep->parf))
		return PTR_ERR(pcie_ep->parf);

//	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "mmio");
//	pcie_ep->mmio = devm_ioremap_resource(dev, res);
//	if (IS_ERR(pcie_ep->mmio))
//		return PTR_ERR(pcie_ep->mmio);

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "tcsr");
	pcie_ep->tcsr = devm_pci_remap_cfg_resource(dev, res);
	if (IS_ERR(pcie_ep->tcsr))
		return PTR_ERR(pcie_ep->tcsr);

	pci->ep.phys_base = res->start;
	pci->ep.addr_size = resource_size(res);

	return 0;
}

static int qcom_pcie_ep_get_resources(struct platform_device *pdev,
					struct qcom_pcie_ep *pcie_ep)
{
	int ret;

	ret = qcom_pcie_ep_get_io_resources(pdev, pcie_ep);
	if (ret) {
		dev_err(&pdev->dev, "failed to get io resources %d\n", ret);
		return ret;
	}

	ret = qcom_pcie_ep_get_vreg_clock_resources(pdev, pcie_ep);
	if (ret) {
		dev_err(&pdev->dev, "failed to get vreg, clocks %d\n", ret);
		return ret;
	}

	ret = qcom_pcie_ep_get_gpio_resources(pdev, pcie_ep);
	if (ret) {
		dev_err(&pdev->dev, "failed to get GPIO resources %d\n", ret);
		return ret;
	}

	pcie_ep->phy = devm_phy_optional_get(&pdev->dev, "pciephy");
	if (IS_ERR(pcie_ep->phy))
		ret = PTR_ERR(pcie_ep->phy);

	return ret;
}

static const struct of_device_id qcom_pcie_ep_match[] = {
	{ .compatible = "qcom,pcie-ep", .data = &data_prairie_ep },
	{ }
};

static int qcom_pcie_ep_probe(struct platform_device *pdev)
{
	struct device_node *node = pdev->dev.of_node;
	const struct pcie_ep_plat_data *data;
	struct device *dev = &pdev->dev;
	const struct of_device_id *id;
	struct qcom_pcie_ep *pcie_ep;
	struct dw_pcie *pci;
	int ret;

	pcie_ep = devm_kzalloc(dev, sizeof(*pcie_ep), GFP_KERNEL);
	if (!pcie_ep)
		return -ENOMEM;

	pcie_ep->res = devm_kzalloc(dev, sizeof(*pcie_ep->res), GFP_KERNEL);
	if (!pcie_ep->res)
		return -ENOMEM;

	id = of_match_node(qcom_pcie_ep_match, node);
	if (id)
		data = id->data;
	else
		data = &data_prairie_ep;

	pcie_ep->data = data;

	pci = devm_kzalloc(dev, sizeof(*pci), GFP_KERNEL);
	if (!pci)
		return -ENOMEM;
	pci->dev = dev;
	pci->ops = &pci_ops;
	pci->ep.ops = &pci_ep_ops;
	pcie_ep->pci = pci;

	spin_lock_init(&pcie_ep->res_lock);
	mutex_init(&pcie_ep->lock);

	ret = qcom_pcie_ep_get_resources(pdev, pcie_ep);
	if (ret) {
		dev_err(dev, "failed to get resources:%d\n", ret);
		return ret;
	}

	/* Enable power and clocks */
	ret = pcie_ep->data->ops->enable_resources(pcie_ep);
	if (ret) {
		pr_err("pcie_ep: Enable resources failed\n");
		return ret;
	}

	/* Perform controller reset */
	ret = pcie_ep->data->ops->core_reset(pcie_ep);
	if (ret) {
		pr_info("pcie_ep: Failed to reset the core\n");
		return ret;
	}

	/* Initialize PHY */
	ret = phy_init(pcie_ep->phy);
	if (ret) {
		pr_info("pcie_ep: PHY init failed\n");
		return ret;
	}

	ret = phy_power_on(pcie_ep->phy);
	if (ret) {
		dev_err(dev, "failed to initialize endpoint:%d\n", ret);
		return ret;
	}

	platform_set_drvdata(pdev, pcie_ep);

	ret = dw_pcie_ep_init(&pci->ep);
	if (ret) {
		dev_err(dev, "failed to initialize endpoint:%d\n", ret);
		return ret;
	}

	ret = qcom_pcie_ep_enable_irq_resources(pdev, pcie_ep);
	if (ret) {
		dev_err(dev, "failed to get IRQ resources %d\n", ret);
		return ret;
	}

	return 0;
}

static struct platform_driver qcom_pcie_ep_driver = {
	.probe	= qcom_pcie_ep_probe,
	.driver	= {
		.name		= "pcie-ep",
		.suppress_bind_attrs = true,
		.owner		= THIS_MODULE,
		.of_match_table	= qcom_pcie_ep_match,
	},
};
builtin_platform_driver(qcom_pcie_ep_driver);
