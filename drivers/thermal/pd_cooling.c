// SPDX-License-Identifier: GPL-2.0
/*
 *  Copyright (C) 2022 Linaro Limited.
 */

#include <linux/device.h>
#include <linux/err.h>
#include <linux/idle_inject.h>
#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/pm_opp.h>
#include <linux/slab.h>
#include <linux/thermal.h>

struct pd_warm_data {
	struct device *dev;
	int nr_opps;
	unsigned int state;
	struct thermal_cooling_device *tcd;
	unsigned int levels[];
};

static int pd_warm_get_max_state(struct thermal_cooling_device *cdev,
				  unsigned long *state)
{
	struct pd_warm_data *data = cdev->devdata;

	*state = data->nr_opps - 1;
	return 0;
}

static int pd_warm_get_cur_state(struct thermal_cooling_device *cdev,
				  unsigned long *state)
{
	struct pd_warm_data *data = cdev->devdata;

	*state = data->state;

	return 0;
}

static int pd_warm_set_cur_state(struct thermal_cooling_device *cdev,
				  unsigned long state)
{
	struct pd_warm_data *data = cdev->devdata;
	struct dev_pm_opp *opp;
	int ret;

	opp = dev_pm_opp_find_level_exact(data->dev, data->levels[state]);
	if (IS_ERR(opp))
		return PTR_ERR(opp);

	ret = dev_pm_opp_set_opp(data->dev, opp);
	if (!ret)
		data->state = state;

	dev_pm_opp_put(opp);

	return ret;
}

static struct thermal_cooling_device_ops pd_warm_ops = {
	.get_max_state = pd_warm_get_max_state,
	.get_cur_state = pd_warm_get_cur_state,
	.set_cur_state = pd_warm_set_cur_state,
};

static int pd_warm_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct pd_warm_data *data;
	unsigned int level;
	int i, ret, nr_opps;

	ret = devm_pm_opp_of_add_table_noclk(dev, 0);
	if (ret)
		return ret;

	nr_opps = dev_pm_opp_get_opp_count(dev);
	if (nr_opps < 0)
		return nr_opps;

	/* No point in warming up if we have less than two possible states */
	if (nr_opps < 2) {
		dev_err(dev, "Invalid number of opp entries: %d\n", nr_opps);
		return -EINVAL;
	}

	data = devm_kzalloc(dev, struct_size(data, levels, nr_opps), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->dev = dev;
	data->nr_opps = nr_opps;
	data->state = nr_opps - 1;

	for (i = 0, level = UINT_MAX; i < nr_opps; i++, level--) {
		struct dev_pm_opp *opp = dev_pm_opp_find_level_floor(dev, &level);

		if (IS_ERR(opp))
			return dev_err_probe(dev, PTR_ERR(opp), "Error getting level %d\n", i);

		data->levels[i] = level;
	}

	data->tcd = devm_thermal_of_cooling_device_register(dev, dev->of_node, dev_name(dev), data, &pd_warm_ops);
	if (IS_ERR(data->tcd))
		return PTR_ERR(data->tcd);

	return 0;
}

static const struct of_device_id pd_warm_match[] = {
	{ .compatible = "power-domain-cooling" },
	{}
};
MODULE_DEVICE_TABLE(of, pd_warm_match);

static struct platform_driver pd_warm_driver = {
	.driver = {
		.name = "pd-cooling",
		.of_match_table = pd_warm_match,
	},

	.probe = pd_warm_probe,
};

module_platform_driver(pd_warm_driver);
