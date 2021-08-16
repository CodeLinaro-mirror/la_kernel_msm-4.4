// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2021 Linaro Ltd.
 * Author: Manivannan Sadhasivam <manivannan.sadhasivam@linaro.org>
 */

#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/mhi_ep.h>
#include "internal.h"

const char * const mhi_state_str[MHI_STATE_MAX] = {
	[MHI_STATE_RESET] = "RESET",
	[MHI_STATE_READY] = "READY",
	[MHI_STATE_M0] = "M0",
	[MHI_STATE_M1] = "M1",
	[MHI_STATE_M2] = "M2",
	[MHI_STATE_M3] = "M3",
	[MHI_STATE_M3_FAST] = "M3 FAST",
	[MHI_STATE_BHI] = "BHI",
	[MHI_STATE_SYS_ERR] = "SYS ERROR",
};

bool __must_check mhi_ep_check_mhi_state(struct mhi_ep_cntrl *mhi_cntrl,
					 enum mhi_state cur_mhi_state,
					 enum mhi_state mhi_state)
{
	bool valid = false;

	switch (mhi_state) {
	case MHI_STATE_READY:
		valid = (cur_mhi_state == MHI_STATE_RESET);
		break;
	case MHI_STATE_M0:
		valid = (cur_mhi_state == MHI_STATE_READY ||
			  cur_mhi_state == MHI_STATE_M3);
		break;
	case MHI_STATE_M3:
		valid = (cur_mhi_state == MHI_STATE_M0);
		break;
	case MHI_STATE_SYS_ERR:
		/* Transition to SYS_ERR state is allowed all the time */
		valid = true;
		break;
	default:
		break;
	}

	return valid;
}

int mhi_ep_set_mhi_state(struct mhi_ep_cntrl *mhi_cntrl, enum mhi_state mhi_state)
{
	struct device *dev = &mhi_cntrl->mhi_dev->dev;

	if (!mhi_ep_check_mhi_state(mhi_cntrl, mhi_cntrl->mhi_state, mhi_state)) {
		dev_err(dev, "MHI state change to %s from %s is not allowed!\n",
			TO_MHI_STATE_STR(mhi_state),
			TO_MHI_STATE_STR(mhi_cntrl->mhi_state));
		return -EACCES;
	}

	switch (mhi_state) {
	case MHI_STATE_READY:
		mhi_ep_mmio_masked_write(mhi_cntrl, MHISTATUS,
				MHISTATUS_READY_MASK,
				MHISTATUS_READY_SHIFT, 1);

		mhi_ep_mmio_masked_write(mhi_cntrl, MHISTATUS,
				MHISTATUS_MHISTATE_MASK,
				MHISTATUS_MHISTATE_SHIFT, mhi_state);
		break;
	case MHI_STATE_SYS_ERR:
		mhi_ep_mmio_masked_write(mhi_cntrl, MHISTATUS,
				MHISTATUS_SYSERR_MASK,
				MHISTATUS_SYSERR_SHIFT, 1);

		mhi_ep_mmio_masked_write(mhi_cntrl, MHISTATUS,
				MHISTATUS_MHISTATE_MASK,
				MHISTATUS_MHISTATE_SHIFT, mhi_state);
		break;
	case MHI_STATE_M1:
	case MHI_STATE_M2:
		dev_err(dev, "MHI state (%s) not supported\n", TO_MHI_STATE_STR(mhi_state));
		return -EOPNOTSUPP;
	case MHI_STATE_M0:
	case MHI_STATE_M3:
		mhi_ep_mmio_masked_write(mhi_cntrl, MHISTATUS,
					  MHISTATUS_MHISTATE_MASK,
					  MHISTATUS_MHISTATE_SHIFT, mhi_state);
		break;
	default:
		dev_err(dev, "Invalid MHI state (%d)", mhi_state);
		return -EINVAL;
	}

	mhi_cntrl->mhi_state = mhi_state;

	return 0;
}

int mhi_ep_set_m0_state(struct mhi_ep_cntrl *mhi_cntrl)
{
	struct device *dev = &mhi_cntrl->mhi_dev->dev;
	enum mhi_state old_state;
	int ret;

	/* If MHI is in M3, resume suspended channels */
	spin_lock_bh(&mhi_cntrl->state_lock);
	old_state = mhi_cntrl->mhi_state;
	if (old_state == MHI_STATE_M3)
		mhi_ep_resume_channels(mhi_cntrl);

	ret = mhi_ep_set_mhi_state(mhi_cntrl, MHI_STATE_M0);
	if (ret) {
		mhi_ep_handle_syserr(mhi_cntrl);
		spin_unlock_bh(&mhi_cntrl->state_lock);
		return ret;
	}

	spin_unlock_bh(&mhi_cntrl->state_lock);
	/* Signal host that the device moved to M0 */
	ret = mhi_ep_send_state_change_event(mhi_cntrl, MHI_STATE_M0);
	if (ret) {
		dev_err(dev, "Failed sending M0 state change event: %d\n", ret);
		return ret;
	}

	if (old_state == MHI_STATE_READY) {
		/* Allow the host to process state change event */
		mdelay(1);

		/* Send AMSS EE event to host */
		ret = mhi_ep_send_ee_event(mhi_cntrl, MHI_EP_AMSS_EE);
		if (ret) {
			dev_err(dev, "Failed sending AMSS EE event: %d\n", ret);
			return ret;
		}
	}

	return 0;
}

int mhi_ep_set_m3_state(struct mhi_ep_cntrl *mhi_cntrl)
{
	struct device *dev = &mhi_cntrl->mhi_dev->dev;
	int ret;

	spin_lock_bh(&mhi_cntrl->state_lock);
	ret = mhi_ep_set_mhi_state(mhi_cntrl, MHI_STATE_M3);
	if (ret) {
		mhi_ep_handle_syserr(mhi_cntrl);
		spin_unlock_bh(&mhi_cntrl->state_lock);
		return ret;
	}

	spin_unlock_bh(&mhi_cntrl->state_lock);
	mhi_ep_suspend_channels(mhi_cntrl);

	/* Signal host that the device moved to M3 */
	ret = mhi_ep_send_state_change_event(mhi_cntrl, MHI_STATE_M3);
	if (ret) {
		dev_err(dev, "Failed sending M3 state change event: %d\n", ret);
		return ret;
	}

	return 0;
}

int mhi_ep_set_ready_state(struct mhi_ep_cntrl *mhi_cntrl)
{
	struct device *dev = &mhi_cntrl->mhi_dev->dev;
	enum mhi_state mhi_state;
	int ret, is_ready;

	spin_lock_bh(&mhi_cntrl->state_lock);
	/* Ensure that the MHISTATUS is set to RESET by host */
	mhi_ep_mmio_masked_read(mhi_cntrl, MHISTATUS, MHISTATUS_MHISTATE_MASK,
				 MHISTATUS_MHISTATE_SHIFT, &mhi_state);
	mhi_ep_mmio_masked_read(mhi_cntrl, MHISTATUS, MHISTATUS_READY_MASK,
				 MHISTATUS_READY_SHIFT, &is_ready);

	if (mhi_state != MHI_STATE_RESET || is_ready) {
		dev_err(dev, "READY state transition failed. MHI host not in RESET state\n");
		spin_unlock_bh(&mhi_cntrl->state_lock);
		return -EFAULT;
	}

	ret = mhi_ep_set_mhi_state(mhi_cntrl, MHI_STATE_READY);
	spin_unlock_bh(&mhi_cntrl->state_lock);

	return ret;
}
