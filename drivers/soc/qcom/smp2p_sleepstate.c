// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2026, OnePlus 5T Mainline Project
 * Clean-room SMP2P AP sleepstate notifier for Qualcomm remoteproc DSPs.
 */

#include <linux/delay.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/platform_device.h>
#include <linux/soc/qcom/smem_state.h>
#include <linux/suspend.h>

struct smp2p_sleepstate {
	struct qcom_smem_state *state;
	unsigned int bit;
	struct notifier_block pm_nb;
};

static int smp2p_sleepstate_pm_notify(struct notifier_block *nb,
				      unsigned long event, void *unused)
{
	struct smp2p_sleepstate *slst =
		container_of(nb, struct smp2p_sleepstate, pm_nb);
	u32 mask = BIT(slst->bit);

	switch (event) {
	case PM_SUSPEND_PREPARE:
		qcom_smem_state_update_bits(slst->state, mask, 0);
		msleep(25);
		break;
	case PM_POST_SUSPEND:
		qcom_smem_state_update_bits(slst->state, mask, mask);
		msleep(25);
		break;
	default:
		break;
	}

	return NOTIFY_DONE;
}

static int smp2p_sleepstate_probe(struct platform_device *pdev)
{
	struct smp2p_sleepstate *slst;
	int ret;

	slst = devm_kzalloc(&pdev->dev, sizeof(*slst), GFP_KERNEL);
	if (!slst)
		return -ENOMEM;

	slst->state = devm_qcom_smem_state_get(&pdev->dev, "sleepstate",
					       &slst->bit);
	if (IS_ERR(slst->state))
		return dev_err_probe(&pdev->dev, PTR_ERR(slst->state),
				     "failed to acquire sleepstate smem state\n");

	ret = qcom_smem_state_update_bits(slst->state, BIT(slst->bit),
					  BIT(slst->bit));
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "failed to assert sleepstate bit %u\n",
				     slst->bit);

	slst->pm_nb.notifier_call = smp2p_sleepstate_pm_notify;
	slst->pm_nb.priority = INT_MAX;
	ret = register_pm_notifier(&slst->pm_nb);
	if (ret)
		return ret;

	platform_set_drvdata(pdev, slst);
	dev_info(&pdev->dev, "asserted SMP2P sleepstate bit %u\n", slst->bit);
	return 0;
}

static void smp2p_sleepstate_remove(struct platform_device *pdev)
{
	struct smp2p_sleepstate *slst = platform_get_drvdata(pdev);

	unregister_pm_notifier(&slst->pm_nb);
}

static const struct of_device_id smp2p_sleepstate_of_match[] = {
	{ .compatible = "qcom,smp2p-sleepstate" },
	{}
};
MODULE_DEVICE_TABLE(of, smp2p_sleepstate_of_match);

static struct platform_driver smp2p_sleepstate_driver = {
	.probe = smp2p_sleepstate_probe,
	.remove = smp2p_sleepstate_remove,
	.driver = {
		.name = "qcom-smp2p-sleepstate",
		.of_match_table = smp2p_sleepstate_of_match,
	},
};
module_platform_driver(smp2p_sleepstate_driver);

MODULE_DESCRIPTION("Qualcomm SMP2P AP sleepstate notifier");
MODULE_LICENSE("GPL");
