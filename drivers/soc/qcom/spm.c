// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2011-2014, The Linux Foundation. All rights reserved.
 * Copyright (c) 2014,2015, Linaro Ltd.
 *
 * SAW power controller driver
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/err.h>
#include <linux/platform_device.h>
#include <linux/regulator/driver.h>
#include <soc/qcom/spm.h>

#define SPM_CTL_INDEX		0x7f
#define SPM_CTL_INDEX_SHIFT	4
#define SPM_CTL_EN		BIT(0)

enum spm_reg {
	SPM_REG_CFG,
	SPM_REG_SPM_CTL,
	SPM_REG_DLY,
	SPM_REG_PMIC_DLY,
	SPM_REG_PMIC_DATA_0,
	SPM_REG_PMIC_DATA_1,
	SPM_REG_PMIC_DATA_2,
	SPM_REG_PMIC_DATA_3,
	SPM_REG_VCTL,
	SPM_REG_SEQ_ENTRY,
	SPM_REG_SPM_STS,
	SPM_REG_PMIC_STS,
	SPM_REG_AVS_CTL,
	SPM_REG_AVS_LIMIT,
	SPM_REG_RST,
	SPM_REG_NR,
};

static const u16 spm_reg_offset_v4_1[SPM_REG_NR] = {
	[SPM_REG_AVS_CTL]	= 0x904,
	[SPM_REG_AVS_LIMIT]	= 0x908,
};

static const struct spm_reg_data spm_reg_660_gold_l2  = {
	.reg_offset = spm_reg_offset_v4_1,
	.avs_ctl = 0x1010031,
	.avs_limit = 0x4580458,
};

static const struct spm_reg_data spm_reg_660_silver_l2  = {
	.reg_offset = spm_reg_offset_v4_1,
	.avs_ctl = 0x101c031,
	.avs_limit = 0x4580458,
};

static const struct spm_reg_data spm_reg_8998_gold_l2  = {
	.reg_offset = spm_reg_offset_v4_1,
	.avs_ctl = 0x1010031,
	.avs_limit = 0x4700470,
};

static const struct spm_reg_data spm_reg_8998_silver_l2  = {
	.reg_offset = spm_reg_offset_v4_1,
	.avs_ctl = 0x1010031,
	.avs_limit = 0x4200420,
};

static const u16 spm_reg_offset_v3_0[SPM_REG_NR] = {
	[SPM_REG_CFG]		= 0x08,
	[SPM_REG_PMIC_STS]	= 0x14,
	[SPM_REG_RST]		= 0x18,
	[SPM_REG_VCTL]		= 0x1c,
	[SPM_REG_AVS_CTL]	= 0x20,
	[SPM_REG_SPM_CTL]	= 0x30,
	[SPM_REG_DLY]		= 0x34,
	[SPM_REG_PMIC_DATA_3]	= 0x4c,
	[SPM_REG_SEQ_ENTRY]	= 0x400,
};

/* SPM register data for 8916 */
static const struct spm_reg_data spm_reg_8916_cpu = {
	.reg_offset = spm_reg_offset_v3_0,
	.spm_cfg = 0x1,
	.spm_dly = 0x3C102800,
	.seq = { 0x60, 0x03, 0x60, 0x0B, 0x0F, 0x20, 0x10, 0x80, 0x30, 0x90,
		0x5B, 0x60, 0x03, 0x60, 0x3B, 0x76, 0x76, 0x0B, 0x94, 0x5B,
		0x80, 0x10, 0x26, 0x30, 0x0F },
	.start_index[PM_SLEEP_MODE_STBY] = 0,
	.start_index[PM_SLEEP_MODE_SPC] = 5,
};

static const struct spm_reg_data spm_reg_8996_cbf  = {
	.reg_offset = spm_reg_offset_v3_0,
	.avs_ctl = 0x1100,
	.has_regulator = true,
};

static const u16 spm_reg_offset_v2_1[SPM_REG_NR] = {
	[SPM_REG_CFG]		= 0x08,
	[SPM_REG_SPM_CTL]	= 0x30,
	[SPM_REG_DLY]		= 0x34,
	[SPM_REG_SEQ_ENTRY]	= 0x80,
};

/* SPM register data for 8974, 8084 */
static const struct spm_reg_data spm_reg_8974_8084_cpu  = {
	.reg_offset = spm_reg_offset_v2_1,
	.spm_cfg = 0x1,
	.spm_dly = 0x3C102800,
	.seq = { 0x03, 0x0B, 0x0F, 0x00, 0x20, 0x80, 0x10, 0xE8, 0x5B, 0x03,
		0x3B, 0xE8, 0x5B, 0x82, 0x10, 0x0B, 0x30, 0x06, 0x26, 0x30,
		0x0F },
	.start_index[PM_SLEEP_MODE_STBY] = 0,
	.start_index[PM_SLEEP_MODE_SPC] = 3,
};

/* SPM register data for 8226 */
static const struct spm_reg_data spm_reg_8226_cpu  = {
	.reg_offset = spm_reg_offset_v2_1,
	.spm_cfg = 0x0,
	.spm_dly = 0x3C102800,
	.seq = { 0x60, 0x03, 0x60, 0x0B, 0x0F, 0x20, 0x10, 0x80, 0x30, 0x90,
		0x5B, 0x60, 0x03, 0x60, 0x3B, 0x76, 0x76, 0x0B, 0x94, 0x5B,
		0x80, 0x10, 0x26, 0x30, 0x0F },
	.start_index[PM_SLEEP_MODE_STBY] = 0,
	.start_index[PM_SLEEP_MODE_SPC] = 5,
};

static const u16 spm_reg_offset_v1_1[SPM_REG_NR] = {
	[SPM_REG_CFG]		= 0x08,
	[SPM_REG_SPM_CTL]	= 0x20,
	[SPM_REG_PMIC_DLY]	= 0x24,
	[SPM_REG_PMIC_DATA_0]	= 0x28,
	[SPM_REG_PMIC_DATA_1]	= 0x2C,
	[SPM_REG_SEQ_ENTRY]	= 0x80,
};

/* SPM register data for 8064 */
static const struct spm_reg_data spm_reg_8064_cpu = {
	.reg_offset = spm_reg_offset_v1_1,
	.spm_cfg = 0x1F,
	.pmic_dly = 0x02020004,
	.pmic_data[0] = 0x0084009C,
	.pmic_data[1] = 0x00A4001C,
	.seq = { 0x03, 0x0F, 0x00, 0x24, 0x54, 0x10, 0x09, 0x03, 0x01,
		0x10, 0x54, 0x30, 0x0C, 0x24, 0x30, 0x0F },
	.start_index[PM_SLEEP_MODE_STBY] = 0,
	.start_index[PM_SLEEP_MODE_SPC] = 2,
};

static inline void spm_register_write(struct spm_driver_data *drv,
					enum spm_reg reg, u32 val)
{
	if (drv->reg_data->reg_offset[reg])
		writel_relaxed(val, drv->reg_base +
				drv->reg_data->reg_offset[reg]);
}

/* Ensure a guaranteed write, before return */
static inline void spm_register_write_sync(struct spm_driver_data *drv,
					enum spm_reg reg, u32 val)
{
	u32 ret;

	if (!drv->reg_data->reg_offset[reg])
		return;

	do {
		writel_relaxed(val, drv->reg_base +
				drv->reg_data->reg_offset[reg]);
		ret = readl_relaxed(drv->reg_base +
				drv->reg_data->reg_offset[reg]);
		if (ret == val)
			break;
		cpu_relax();
	} while (1);
}

static inline u32 spm_register_read(struct spm_driver_data *drv,
				    enum spm_reg reg)
{
	return readl_relaxed(drv->reg_base + drv->reg_data->reg_offset[reg]);
}

void spm_set_low_power_mode(struct spm_driver_data *drv,
			    enum pm_sleep_mode mode)
{
	u32 start_index;
	u32 ctl_val;

	start_index = drv->reg_data->start_index[mode];

	ctl_val = spm_register_read(drv, SPM_REG_SPM_CTL);
	ctl_val &= ~(SPM_CTL_INDEX << SPM_CTL_INDEX_SHIFT);
	ctl_val |= start_index << SPM_CTL_INDEX_SHIFT;
	ctl_val |= SPM_CTL_EN;
	spm_register_write_sync(drv, SPM_REG_SPM_CTL, ctl_val);
}

#define SAW3_VCTL_DATA_MASK	0xFF
#define SAW3_VCTL_CLEAR_MASK	0x700FF
#define SAW3_AVS_CTL_EN_MASK	0x1
#define SAW3_AVS_CTL_TGGL_MASK	0x8000000
#define SAW3_AVS_CTL_CLEAR_MASK	0x7efc00

static void spmi_saw_set_vdd(void *data)
{
	struct spm_driver_data *drv = data;
	u32 vctl, data3, avs_ctl, pmic_sts;
	bool avs_enabled = false;
	unsigned long timeout;
	u32 voltage_sel = drv->volt_sel;

	avs_ctl = spm_register_read(drv, SPM_REG_AVS_CTL);
	vctl = spm_register_read(drv, SPM_REG_VCTL);
	data3 = spm_register_read(drv, SPM_REG_PMIC_DATA_3);

	/* select the band */
	vctl &= ~SAW3_VCTL_CLEAR_MASK;
	vctl |= voltage_sel;

	data3 &= ~SAW3_VCTL_CLEAR_MASK;
	data3 |= voltage_sel;

	/* If AVS is enabled, switch it off during the voltage change */
	avs_enabled = SAW3_AVS_CTL_EN_MASK & avs_ctl;
	if (avs_enabled) {
		avs_ctl &= ~SAW3_AVS_CTL_TGGL_MASK;
		spm_register_write(drv, SPM_REG_AVS_CTL, avs_ctl);
	}

	/* Kick the state machine back to idle */
	spm_register_write(drv, SPM_REG_RST, 1);
	spm_register_write(drv, SPM_REG_VCTL, vctl);
	spm_register_write(drv, SPM_REG_PMIC_DATA_3, data3);

	timeout = jiffies + usecs_to_jiffies(100);
	do {
		pmic_sts = spm_register_read(drv, SPM_REG_PMIC_STS);
		pmic_sts &= SAW3_VCTL_DATA_MASK;
		if (pmic_sts == voltage_sel)
			break;

		cpu_relax();

	} while (time_before(jiffies, timeout));

	/* After successful voltage change, switch the AVS back on */
	if (avs_enabled) {
		pmic_sts &= 0x3f;
		avs_ctl &= ~SAW3_AVS_CTL_CLEAR_MASK;
		avs_ctl |= ((pmic_sts - 4) << 10);
		avs_ctl |= (pmic_sts << 17);
		avs_ctl |= SAW3_AVS_CTL_TGGL_MASK;
		spm_register_write(drv, SPM_REG_AVS_CTL, avs_ctl);
	}
}

static int spm_avs_set_voltage_sel(struct regulator_dev *rdev, unsigned int selector)
{
	struct spm_driver_data *drv = rdev_get_drvdata(rdev);

	drv->volt_sel = selector;

	/* Always do the SAW register writes on the first CPU */
	return smp_call_function_single(0, spmi_saw_set_vdd, drv, true);
}

static int spm_avs_get_voltage_sel(struct regulator_dev *rdev)
{
	struct spm_driver_data *drv = rdev_get_drvdata(rdev);

	return spm_register_read(drv, SPM_REG_PMIC_STS) & 0xff;
}

static const struct regulator_ops spm_avs_ops = {
	.set_voltage_sel	= spm_avs_set_voltage_sel,
	.get_voltage_sel	= spm_avs_get_voltage_sel,
	.list_voltage		= regulator_list_voltage_linear,
	.map_voltage		= regulator_map_voltage_linear,
};

static const struct regulator_desc spm_regulator = {
	.name = "spm",
	.of_match = of_match_ptr("spm"),
	.id = 0,
	.type = REGULATOR_VOLTAGE,
	.owner = THIS_MODULE,
	.ops = &spm_avs_ops,
	.n_voltages = 256,

	/* FIXME: these are applicable to msm8996 only */
	.min_uV = 80000 + 54 * 5000,
	.uV_step = 5000,
	.linear_min_sel = 54,
};

static int spm_avs_register_regulator(struct device *dev, struct spm_driver_data *drv)
{
	struct regulator_config config = {
		.dev = dev,
		.driver_data = drv,
	};
	struct regulator_dev *rdev;

	/* Program initial voltage, high enough, otherwise PMIC_STS returns 0 */
	/* FIXME: this should come from platform data */
	drv->volt_sel = DIV_ROUND_UP(980000 - spm_regulator.min_uV, spm_regulator.uV_step) + spm_regulator.linear_min_sel;

	/* Always do the SAW register writes on the first CPU */
	smp_call_function_single(0, spmi_saw_set_vdd, drv, true);

	rdev = devm_regulator_register(dev,
				       &spm_regulator,
				       &config);
	if (IS_ERR(rdev)) {
		dev_err(dev, "failed to register regulator\n");
		return PTR_ERR(rdev);
	}

	return 0;
}

static const struct of_device_id spm_match_table[] = {
	{ .compatible = "qcom,sdm660-gold-saw2-v4.1-l2",
	  .data = &spm_reg_660_gold_l2 },
	{ .compatible = "qcom,sdm660-silver-saw2-v4.1-l2",
	  .data = &spm_reg_660_silver_l2 },
	{ .compatible = "qcom,msm8226-saw2-v2.1-cpu",
	  .data = &spm_reg_8226_cpu },
	{ .compatible = "qcom,msm8916-saw2-v3.0-cpu",
	  .data = &spm_reg_8916_cpu },
	{ .compatible = "qcom,msm8974-saw2-v2.1-cpu",
	  .data = &spm_reg_8974_8084_cpu },
	{ .compatible = "qcom,msm8996-saw2-v3.0-cbf",
	  .data = &spm_reg_8996_cbf },
	{ .compatible = "qcom,msm8998-gold-saw2-v4.1-l2",
	  .data = &spm_reg_8998_gold_l2 },
	{ .compatible = "qcom,msm8998-silver-saw2-v4.1-l2",
	  .data = &spm_reg_8998_silver_l2 },
	{ .compatible = "qcom,apq8084-saw2-v2.1-cpu",
	  .data = &spm_reg_8974_8084_cpu },
	{ .compatible = "qcom,apq8064-saw2-v1.1-cpu",
	  .data = &spm_reg_8064_cpu },
	{ },
};
MODULE_DEVICE_TABLE(of, spm_match_table);

static int spm_dev_probe(struct platform_device *pdev)
{
	const struct of_device_id *match_id;
	struct spm_driver_data *drv;
	struct resource *res;
	void __iomem *addr;

	drv = devm_kzalloc(&pdev->dev, sizeof(*drv), GFP_KERNEL);
	if (!drv)
		return -ENOMEM;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	drv->reg_base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(drv->reg_base))
		return PTR_ERR(drv->reg_base);

	match_id = of_match_node(spm_match_table, pdev->dev.of_node);
	if (!match_id)
		return -ENODEV;

	drv->reg_data = match_id->data;
	platform_set_drvdata(pdev, drv);

	/* Write the SPM sequences first.. */
	addr = drv->reg_base + drv->reg_data->reg_offset[SPM_REG_SEQ_ENTRY];
	__iowrite32_copy(addr, drv->reg_data->seq,
			ARRAY_SIZE(drv->reg_data->seq) / 4);

	/*
	 * ..and then the control registers.
	 * On some SoC if the control registers are written first and if the
	 * CPU was held in reset, the reset signal could trigger the SPM state
	 * machine, before the sequences are completely written.
	 */
	spm_register_write(drv, SPM_REG_AVS_CTL, drv->reg_data->avs_ctl);
	spm_register_write(drv, SPM_REG_AVS_LIMIT, drv->reg_data->avs_limit);
	spm_register_write(drv, SPM_REG_CFG, drv->reg_data->spm_cfg);
	spm_register_write(drv, SPM_REG_DLY, drv->reg_data->spm_dly);
	spm_register_write(drv, SPM_REG_PMIC_DLY, drv->reg_data->pmic_dly);
	spm_register_write(drv, SPM_REG_PMIC_DATA_0,
				drv->reg_data->pmic_data[0]);
	spm_register_write(drv, SPM_REG_PMIC_DATA_1,
				drv->reg_data->pmic_data[1]);

	/* Set up Standby as the default low power mode */
	if (drv->reg_data->reg_offset[SPM_REG_SPM_CTL])
		spm_set_low_power_mode(drv, PM_SLEEP_MODE_STBY);

	if (!drv->reg_data->has_regulator)
		return 0;

	return spm_avs_register_regulator(&pdev->dev, drv);
}

static struct platform_driver spm_driver = {
	.probe = spm_dev_probe,
	.driver = {
		.name = "qcom_spm",
		.of_match_table = spm_match_table,
	},
};

static int __init qcom_spm_init(void)
{
	return platform_driver_register(&spm_driver);
}
arch_initcall(qcom_spm_init);

MODULE_LICENSE("GPL v2");
