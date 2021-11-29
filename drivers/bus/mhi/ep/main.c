// SPDX-License-Identifier: GPL-2.0
/*
 * MHI Bus Endpoint stack
 *
 * Copyright (C) 2021 Linaro Ltd.
 * Author: Manivannan Sadhasivam <manivannan.sadhasivam@linaro.org>
 */

#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/dma-direction.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/mhi_ep.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include "internal.h"

#define MHI_SUSPEND_MIN			100
#define MHI_SUSPEND_TIMEOUT		600

static DEFINE_IDA(mhi_ep_cntrl_ida);

static int mhi_ep_create_device(struct mhi_ep_cntrl *mhi_cntrl, u32 ch_id);
static int mhi_ep_destroy_device(struct device *dev, void *data);

int mhi_ep_send_event(struct mhi_ep_cntrl *mhi_cntrl, u32 event_ring,
		      struct mhi_ep_ring_element *el)
{
	struct mhi_ep_ring *ring = &mhi_cntrl->mhi_event[event_ring].ring;
	struct device *dev = &mhi_cntrl->mhi_dev->dev;
	union mhi_ep_ring_ctx *ctx;
	int ret;

	mutex_lock(&mhi_cntrl->event_lock);
	ctx = (union mhi_ep_ring_ctx *)&mhi_cntrl->ev_ctx_cache[event_ring];
	if (ring->state == RING_STATE_UINT) {
		ret = mhi_ep_ring_start(mhi_cntrl, ring, ctx);
		if (ret) {
			dev_err(dev, "Error starting event ring (%d)\n", event_ring);
			goto err_unlock;
		}
	}

	/* Add element to the primary event ring (0) */
	ret = mhi_ep_ring_add_element(ring, el, 0);
	if (ret) {
		dev_err(dev, "Error adding element to event ring (%d)\n", event_ring);
		goto err_unlock;
	}

	/* Ensure that the ring pointer gets updated in host memory before triggering IRQ */
	wmb();

	mutex_unlock(&mhi_cntrl->event_lock);

	/*
	 * Raise IRQ to host only if the BEI flag is not set in TRE. Host might
	 * set this flag for interrupt moderation as per MHI protocol.
	 */
	if (!MHI_EP_TRE_GET_BEI(el))
		mhi_cntrl->raise_irq(mhi_cntrl);

	return 0;

err_unlock:
	mutex_unlock(&mhi_cntrl->event_lock);

	return ret;
}

static int mhi_ep_send_completion_event(struct mhi_ep_cntrl *mhi_cntrl,
			struct mhi_ep_ring *ring, u32 len,
			enum mhi_ev_ccs code)
{
	struct mhi_ep_ring_element event = {};
	u32 er_index, tmp;

	er_index = mhi_cntrl->ch_ctx_cache[ring->ch_id].erindex;
	event.ptr = ring->ring_ctx->generic.rbase +
			ring->rd_offset * sizeof(struct mhi_ep_ring_element);

	tmp = event.dword[0];
	tmp |= MHI_TRE_EV_DWORD0(code, len);
	event.dword[0] = tmp;

	tmp = event.dword[1];
	tmp |= MHI_TRE_EV_DWORD1(ring->ch_id, MHI_PKT_TYPE_TX_EVENT);
	event.dword[1] = tmp;

	return mhi_ep_send_event(mhi_cntrl, er_index, &event);
}

int mhi_ep_send_state_change_event(struct mhi_ep_cntrl *mhi_cntrl,
						enum mhi_state state)
{
	struct mhi_ep_ring_element event = {};
	u32 tmp;

	tmp = event.dword[0];
	tmp |= MHI_SC_EV_DWORD0(state);
	event.dword[0] = tmp;

	tmp = event.dword[1];
	tmp |= MHI_SC_EV_DWORD1(MHI_PKT_TYPE_STATE_CHANGE_EVENT);
	event.dword[1] = tmp;

	return mhi_ep_send_event(mhi_cntrl, 0, &event);
}

int mhi_ep_send_ee_event(struct mhi_ep_cntrl *mhi_cntrl, enum mhi_ep_execenv exec_env)
{
	struct mhi_ep_ring_element event = {};
	u32 tmp;

	tmp = event.dword[0];
	tmp |= MHI_EE_EV_DWORD0(exec_env);
	event.dword[0] = tmp;

	tmp = event.dword[1];
	tmp |= MHI_SC_EV_DWORD1(MHI_PKT_TYPE_EE_EVENT);
	event.dword[1] = tmp;

	return mhi_ep_send_event(mhi_cntrl, 0, &event);
}

static int mhi_ep_send_cmd_comp_event(struct mhi_ep_cntrl *mhi_cntrl,
				enum mhi_ev_ccs code)
{
	struct device *dev = &mhi_cntrl->mhi_dev->dev;
	struct mhi_ep_ring_element event = {};
	u32 tmp;

	if (code > MHI_EV_CC_BAD_TRE) {
		dev_err(dev, "Invalid command completion code: %d\n", code);
		return -EINVAL;
	}

	event.ptr = mhi_cntrl->cmd_ctx_cache->rbase
			+ (mhi_cntrl->mhi_cmd->ring.rd_offset *
			(sizeof(struct mhi_ep_ring_element)));

	tmp = event.dword[0];
	tmp |= MHI_CC_EV_DWORD0(code);
	event.dword[0] = tmp;

	tmp = event.dword[1];
	tmp |= MHI_CC_EV_DWORD1(MHI_PKT_TYPE_CMD_COMPLETION_EVENT);
	event.dword[1] = tmp;

	return mhi_ep_send_event(mhi_cntrl, 0, &event);
}

/*
 * We don't need to do anything special other than setting the MHI SYS_ERR
 * state. The host issue will reset all contexts and issue MHI RESET so that we
 * could also recover from error state.
 */
void mhi_ep_handle_syserr(struct mhi_ep_cntrl *mhi_cntrl)
{
	struct device *dev = &mhi_cntrl->mhi_dev->dev;
	int ret;

	/* If MHI EP is not enabled, nothing to do */
	if (!mhi_cntrl->is_enabled)
		return;

	ret = mhi_ep_set_mhi_state(mhi_cntrl, MHI_STATE_SYS_ERR);
	if (ret)
		return;

	/* Signal host that the device went to SYS_ERR state */
	ret = mhi_ep_send_state_change_event(mhi_cntrl, MHI_STATE_SYS_ERR);
	if (ret)
		dev_err(dev, "Failed sending SYS_ERR state change event: %d\n", ret);
}

void mhi_ep_suspend_channels(struct mhi_ep_cntrl *mhi_cntrl)
{
	struct mhi_ep_chan *mhi_chan;
	u32 tmp;
	int i;

	for (i = 0; i < mhi_cntrl->max_chan; i++) {
		mhi_chan = &mhi_cntrl->mhi_chan[i];

		if (!mhi_chan->mhi_dev)
			continue;

		mutex_lock(&mhi_chan->lock);
		/* Skip if the channel is not currently running */
		tmp = mhi_cntrl->ch_ctx_cache[i].chcfg;
		if (FIELD_GET(CHAN_CTX_CHSTATE_MASK, tmp) != MHI_CH_STATE_RUNNING) {
			mutex_unlock(&mhi_chan->lock);
			continue;
		}

		dev_dbg(&mhi_chan->mhi_dev->dev, "Suspending channel\n");
		/* Set channel state to SUSPENDED */
		tmp &= ~CHAN_CTX_CHSTATE_MASK;
		tmp |= (MHI_CH_STATE_SUSPENDED << CHAN_CTX_CHSTATE_SHIFT);
		mhi_cntrl->ch_ctx_cache[i].chcfg = tmp;
		mutex_unlock(&mhi_chan->lock);
	}
}

void mhi_ep_resume_channels(struct mhi_ep_cntrl *mhi_cntrl)
{
	struct mhi_ep_chan *mhi_chan;
	u32 tmp;
	int i;

	for (i = 0; i < mhi_cntrl->max_chan; i++) {
		mhi_chan = &mhi_cntrl->mhi_chan[i];

		if (!mhi_chan->mhi_dev)
			continue;

		mutex_lock(&mhi_chan->lock);
		/* Skip if the channel is not currently suspended */
		tmp = mhi_cntrl->ch_ctx_cache[i].chcfg;
		if (FIELD_GET(CHAN_CTX_CHSTATE_MASK, tmp) != MHI_CH_STATE_SUSPENDED) {
			mutex_unlock(&mhi_chan->lock);
			continue;
		}

		dev_dbg(&mhi_chan->mhi_dev->dev, "Resuming channel\n");
		/* Set channel state to RUNNING */
		tmp &= ~CHAN_CTX_CHSTATE_MASK;
		tmp |= (MHI_CH_STATE_RUNNING << CHAN_CTX_CHSTATE_SHIFT);
		mhi_cntrl->ch_ctx_cache[i].chcfg = tmp;
		mutex_unlock(&mhi_chan->lock);
	}
}

int mhi_ep_process_cmd_ring(struct mhi_ep_ring *ring, struct mhi_ep_ring_element *el)
{
	struct mhi_ep_cntrl *mhi_cntrl = ring->mhi_cntrl;
	struct device *dev = &mhi_cntrl->mhi_dev->dev;
	struct mhi_ep_ring *ch_ring, *event_ring;
	union mhi_ep_ring_ctx *event_ctx;
	struct mhi_result result = {};
	struct mhi_ep_chan *mhi_chan;
	u32 event_ring_idx, tmp;
	u32 ch_id;
	int ret;

	ch_id = MHI_TRE_GET_CMD_CHID(el);
	mhi_chan = &mhi_cntrl->mhi_chan[ch_id];
	ch_ring = &mhi_cntrl->mhi_chan[ch_id].ring;

	switch (MHI_TRE_GET_CMD_TYPE(el)) {
	case MHI_PKT_TYPE_START_CHAN_CMD:
		dev_dbg(dev, "Received START command for channel (%d)\n", ch_id);

		mutex_lock(&mhi_chan->lock);
		/* Initialize and configure the corresponding channel ring */
		if (ch_ring->state == RING_STATE_UINT) {
			ret = mhi_ep_ring_start(mhi_cntrl, ch_ring,
				(union mhi_ep_ring_ctx *)&mhi_cntrl->ch_ctx_cache[ch_id]);
			if (ret) {
				dev_err(dev, "Failed to start ring for channel (%d)\n", ch_id);
				ret = mhi_ep_send_cmd_comp_event(mhi_cntrl,
							MHI_EV_CC_UNDEFINED_ERR);
				if (ret)
					dev_err(dev, "Error sending completion event: %d\n",
						MHI_EV_CC_UNDEFINED_ERR);

				goto err_unlock;
			}
		}

		/* Enable DB for the channel */
		mhi_ep_mmio_enable_chdb_a7(mhi_cntrl, ch_id);

		mutex_lock(&mhi_cntrl->event_lock);
		event_ring_idx = mhi_cntrl->ch_ctx_cache[ch_id].erindex;
		event_ring = &mhi_cntrl->mhi_event[event_ring_idx].ring;
		event_ctx = (union mhi_ep_ring_ctx *)&mhi_cntrl->ev_ctx_cache[event_ring_idx];
		if (event_ring->state == RING_STATE_UINT) {
			ret = mhi_ep_ring_start(mhi_cntrl, event_ring, event_ctx);
			if (ret) {
				dev_err(dev, "Error starting event ring: %d\n",
					mhi_cntrl->ch_ctx_cache[ch_id].erindex);
				mutex_unlock(&mhi_cntrl->event_lock);
				goto err_unlock;
			}
		}

		mutex_unlock(&mhi_cntrl->event_lock);

		/* Set channel state to RUNNING */
		mhi_chan->state = MHI_CH_STATE_RUNNING;
		tmp = mhi_cntrl->ch_ctx_cache[ch_id].chcfg;
		tmp &= ~CHAN_CTX_CHSTATE_MASK;
		tmp |= (MHI_CH_STATE_RUNNING << CHAN_CTX_CHSTATE_SHIFT);
		mhi_cntrl->ch_ctx_cache[ch_id].chcfg = tmp;

		ret = mhi_ep_send_cmd_comp_event(mhi_cntrl, MHI_EV_CC_SUCCESS);
		if (ret) {
			dev_err(dev, "Error sending command completion event: %d\n",
				MHI_EV_CC_SUCCESS);
			goto err_unlock;
		}

		mutex_unlock(&mhi_chan->lock);

		/*
		 * Create MHI device only during UL channel start. Since the MHI
		 * channels operate in a pair, we'll associate both UL and DL
		 * channels to the same device.
		 *
		 * We also need to check for mhi_dev != NULL because, the host
		 * will issue START_CHAN command during resume and we don't
		 * destroy the device during suspend.
		 */
		if (!(ch_id % 2) && !mhi_chan->mhi_dev) {
			ret = mhi_ep_create_device(mhi_cntrl, ch_id);
			if (ret) {
				dev_err(dev, "Error creating device for channel (%d)\n", ch_id);
				return ret;
			}
		}

		break;
	case MHI_PKT_TYPE_STOP_CHAN_CMD:
		dev_dbg(dev, "Received STOP command for channel (%d)\n", ch_id);
		if (ch_ring->state == RING_STATE_UINT) {
			dev_err(dev, "Channel (%d) not opened\n", ch_id);
			return -ENODEV;
		}

		mutex_lock(&mhi_chan->lock);
		/* Disable DB for the channel */
		mhi_ep_mmio_disable_chdb_a7(mhi_cntrl, ch_id);

		/* Set the local value of the transfer ring read pointer to the channel context */
		ch_ring->rd_offset = mhi_ep_ring_addr2offset(ch_ring,
					ch_ring->ring_ctx->generic.rp);

		/* Send channel disconnect status to client drivers */
		result.transaction_status = -ENOTCONN;
		result.bytes_xferd = 0;
		mhi_chan->xfer_cb(mhi_chan->mhi_dev, &result);

		/* Set channel state to STOP */
		mhi_chan->state = MHI_CH_STATE_STOP;
		tmp = mhi_cntrl->ch_ctx_cache[ch_id].chcfg;
		tmp &= ~CHAN_CTX_CHSTATE_MASK;
		tmp |= (MHI_CH_STATE_STOP << CHAN_CTX_CHSTATE_SHIFT);
		mhi_cntrl->ch_ctx_cache[ch_id].chcfg = tmp;

		ret = mhi_ep_send_cmd_comp_event(mhi_cntrl, MHI_EV_CC_SUCCESS);
		if (ret) {
			dev_err(dev, "Error sending command completion event: %d\n",
				MHI_EV_CC_SUCCESS);
			goto err_unlock;
		}

		mutex_unlock(&mhi_chan->lock);
		break;
	case MHI_PKT_TYPE_RESET_CHAN_CMD:
		dev_dbg(dev, "Received STOP command for channel (%d)\n", ch_id);
		if (ch_ring->state == RING_STATE_UINT) {
			dev_err(dev, "Channel (%d) not opened\n", ch_id);
			return -ENODEV;
		}

		mutex_lock(&mhi_chan->lock);
		/* Stop and reset the transfer ring */
		mhi_ep_ring_stop(mhi_cntrl, ch_ring);

		/* Send channel disconnect status to client driver */
		result.transaction_status = -ENOTCONN;
		result.bytes_xferd = 0;
		mhi_chan->xfer_cb(mhi_chan->mhi_dev, &result);

		/* Set channel state to DISABLED */
		mhi_chan->state = MHI_CH_STATE_DISABLED;
		tmp = mhi_cntrl->ch_ctx_cache[ch_id].chcfg;
		tmp &= ~CHAN_CTX_CHSTATE_MASK;
		tmp |= (MHI_CH_STATE_DISABLED << CHAN_CTX_CHSTATE_SHIFT);
		mhi_cntrl->ch_ctx_cache[ch_id].chcfg = tmp;

		ret = mhi_ep_send_cmd_comp_event(mhi_cntrl, MHI_EV_CC_SUCCESS);
		if (ret) {
			dev_err(dev, "Error sending command completion event: %d\n",
				MHI_EV_CC_SUCCESS);
			goto err_unlock;
		}
		mutex_unlock(&mhi_chan->lock);
		break;
	default:
		dev_err(dev, "Invalid command received: %d for channel (%d)",
			MHI_TRE_GET_CMD_TYPE(el), ch_id);
		return -EINVAL;
	}

	return 0;

err_unlock:
	mutex_unlock(&mhi_chan->lock);

	return ret;
}

static int mhi_ep_check_tre_bytes_left(struct mhi_ep_cntrl *mhi_cntrl,
				       struct mhi_ep_ring *ring,
				       struct mhi_ep_ring_element *el)
{
	struct mhi_ep_chan *mhi_chan = &mhi_cntrl->mhi_chan[ring->ch_id];
	bool td_done = 0;

	/* A full TRE worth of data was consumed. Check if we are at a TD boundary */
	if (mhi_chan->tre_bytes_left == 0) {
		if (MHI_EP_TRE_GET_CHAIN(el)) {
			if (MHI_EP_TRE_GET_IEOB(el))
				mhi_ep_send_completion_event(mhi_cntrl,
				ring, MHI_EP_TRE_GET_LEN(el), MHI_EV_CC_EOB);
		} else {
			if (MHI_EP_TRE_GET_IEOT(el))
				mhi_ep_send_completion_event(mhi_cntrl,
				ring, MHI_EP_TRE_GET_LEN(el), MHI_EV_CC_EOT);
			td_done = 1;
		}

		mhi_ep_ring_inc_index(ring);
		mhi_chan->tre_bytes_left = 0;
		mhi_chan->tre_loc = 0;
	}

	return td_done;
}

static int mhi_ep_read_channel(struct mhi_ep_cntrl *mhi_cntrl,
			       struct mhi_ep_ring *ring,
			       struct mhi_result *result,
			       u32 len)
{
	struct mhi_ep_chan *mhi_chan = &mhi_cntrl->mhi_chan[ring->ch_id];
	struct device *dev = &mhi_cntrl->mhi_dev->dev;
	size_t bytes_to_read, addr_offset;
	struct mhi_ep_ring_element *el;
	ssize_t bytes_read = 0;
	u32 buf_remaining;
	void __iomem *tre_buf;
	phys_addr_t tre_phys;
	void *write_to_loc;
	u64 read_from_loc;
	bool td_done = 0;
	int ret;

	buf_remaining = len;

	do {
		/* Don't process the transfer ring if the channel is not in RUNNING state */
		if (mhi_chan->state != MHI_CH_STATE_RUNNING)
			return -ENODEV;

		el = &ring->ring_cache[ring->rd_offset];

		if (mhi_chan->tre_loc) {
			bytes_to_read = min(buf_remaining,
						mhi_chan->tre_bytes_left);
			dev_dbg(dev, "TRE bytes remaining: %d", mhi_chan->tre_bytes_left);
		} else {
			if (mhi_ep_queue_is_empty(mhi_chan->mhi_dev, DMA_TO_DEVICE))
				/* Nothing to do */
				return 0;

			mhi_chan->tre_loc = MHI_EP_TRE_GET_PTR(el);
			mhi_chan->tre_size = MHI_EP_TRE_GET_LEN(el);
			mhi_chan->tre_bytes_left = mhi_chan->tre_size;

			bytes_to_read = min(buf_remaining, mhi_chan->tre_size);
		}

		bytes_read += bytes_to_read;
		addr_offset = mhi_chan->tre_size - mhi_chan->tre_bytes_left;
		read_from_loc = mhi_chan->tre_loc + addr_offset;
		write_to_loc = result->buf_addr + (len - buf_remaining);
		mhi_chan->tre_bytes_left -= bytes_to_read;

		tre_buf = mhi_cntrl->alloc_addr(mhi_cntrl, &tre_phys, bytes_to_read);
		if (!tre_buf) {
			dev_err(dev, "Failed to allocate TRE buffer\n");
			return -ENOMEM;
		}

		ret = mhi_cntrl->map_addr(mhi_cntrl, tre_phys, read_from_loc, bytes_to_read);
		if (ret) {
			dev_err(dev, "Failed to map TRE buffer\n");
			goto err_tre_free;
		}

		dev_dbg(&mhi_chan->mhi_dev->dev, "Reading %d bytes", bytes_to_read);
		memcpy_fromio(write_to_loc, tre_buf, bytes_to_read);

		mhi_cntrl->unmap_addr(mhi_cntrl, tre_phys);
		mhi_cntrl->free_addr(mhi_cntrl, tre_phys, tre_buf, bytes_to_read);

		buf_remaining -= bytes_to_read;
		td_done = mhi_ep_check_tre_bytes_left(mhi_cntrl, ring, el);
	} while (buf_remaining && !td_done);

	result->bytes_xferd = bytes_read;

	return bytes_read;

err_tre_free:
	mhi_cntrl->free_addr(mhi_cntrl, tre_phys, tre_buf, bytes_to_read);

	return ret;
}

int mhi_ep_process_tre_ring(struct mhi_ep_ring *ring, struct mhi_ep_ring_element *el)
{
	struct mhi_ep_cntrl *mhi_cntrl = ring->mhi_cntrl;
	struct mhi_result result = {};
	u32 len = MHI_EP_DEFAULT_MTU;
	struct mhi_ep_chan *mhi_chan;
	int ret = 0;

	mhi_chan = &mhi_cntrl->mhi_chan[ring->ch_id];

	/*
	 * Bail out if transfer callback is not registered for the channel.
	 * This is most likely due to the client driver not loaded at this point.
	 */
	if (!mhi_chan->xfer_cb) {
		dev_err(&mhi_chan->mhi_dev->dev, "Client driver not available\n");
		return -ENODEV;
	}

	dev_dbg(&mhi_chan->mhi_dev->dev, "Processing TRE ring\n");

	mutex_lock(&mhi_chan->lock);
	if (ring->ch_id % 2) {
		/* DL channel */
		result.dir = mhi_chan->dir;
		mhi_chan->xfer_cb(mhi_chan->mhi_dev, &result);
	} else {
		/* UL channel */
		while (1) {
			result.buf_addr = kzalloc(len, GFP_KERNEL);
			if (!result.buf_addr) {
				ret = -ENOMEM;
				goto err_unlock;
			}

			ret = mhi_ep_read_channel(mhi_cntrl, ring, &result, len);
			if (ret < 0) {
				dev_err(&mhi_chan->mhi_dev->dev, "Failed to read channel");
				kfree(result.buf_addr);
				break;
			} else if (ret == 0) {
				/* No more data to read */
				kfree(result.buf_addr);
				break;
			}

			result.dir = mhi_chan->dir;
			mhi_chan->xfer_cb(mhi_chan->mhi_dev, &result);
			kfree(result.buf_addr);
		}
	}

err_unlock:
	mutex_unlock(&mhi_chan->lock);

	return ret;
}

static int mhi_ep_cache_host_cfg(struct mhi_ep_cntrl *mhi_cntrl)
{
	struct device *dev = &mhi_cntrl->mhi_dev->dev;
	int ret;

	/* Update the number of event rings (NER) programmed by the host */
	mhi_ep_mmio_update_ner(mhi_cntrl);

	dev_dbg(dev, "Number of Event rings: %d, HW Event rings: %d\n",
		 mhi_cntrl->event_rings, mhi_cntrl->hw_event_rings);

	mhi_cntrl->ch_ctx_host_size = sizeof(struct mhi_chan_ctxt) *
					mhi_cntrl->max_chan;
	mhi_cntrl->ev_ctx_host_size = sizeof(struct mhi_event_ctxt) *
					mhi_cntrl->event_rings;
	mhi_cntrl->cmd_ctx_host_size = sizeof(struct mhi_cmd_ctxt);

	/* Get the channel context base pointer from host */
	mhi_ep_mmio_get_chc_base(mhi_cntrl);

	/* Allocate memory for caching host channel context */
	mhi_cntrl->ch_ctx_cache = mhi_cntrl->alloc_addr(mhi_cntrl, &mhi_cntrl->ch_ctx_cache_phys,
							mhi_cntrl->ch_ctx_host_size);
	if (!mhi_cntrl->ch_ctx_cache) {
		dev_err(dev, "Failed to allocate ch_ctx_cache memory\n");
		return -ENOMEM;
	}

	/* Map the host channel context */
	ret = mhi_cntrl->map_addr(mhi_cntrl, mhi_cntrl->ch_ctx_cache_phys,
				  mhi_cntrl->ch_ctx_host_pa, mhi_cntrl->ch_ctx_host_size);
	if (ret) {
		dev_err(dev, "Failed to map ch_ctx_cache\n");
		goto err_ch_ctx;
	}

	/* Get the event context base pointer from host */
	mhi_ep_mmio_get_erc_base(mhi_cntrl);

	/* Allocate memory for caching host event context */
	mhi_cntrl->ev_ctx_cache = mhi_cntrl->alloc_addr(mhi_cntrl, &mhi_cntrl->ev_ctx_cache_phys,
							mhi_cntrl->ev_ctx_host_size);
	if (!mhi_cntrl->ev_ctx_cache) {
		dev_err(dev, "Failed to allocate ev_ctx_cache memory\n");
		ret = -ENOMEM;
		goto err_ch_ctx_map;
	}

	/* Map the host event context */
	ret = mhi_cntrl->map_addr(mhi_cntrl, mhi_cntrl->ev_ctx_cache_phys,
				  mhi_cntrl->ev_ctx_host_pa, mhi_cntrl->ev_ctx_host_size);
	if (ret) {
		dev_err(dev, "Failed to map ev_ctx_cache\n");
		goto err_ev_ctx;
	}

	/* Get the command context base pointer from host */
	mhi_ep_mmio_get_crc_base(mhi_cntrl);

	/* Allocate memory for caching host command context */
	mhi_cntrl->cmd_ctx_cache = mhi_cntrl->alloc_addr(mhi_cntrl, &mhi_cntrl->cmd_ctx_cache_phys,
							 mhi_cntrl->cmd_ctx_host_size);
	if (!mhi_cntrl->cmd_ctx_cache) {
		dev_err(dev, "Failed to allocate cmd_ctx_cache memory\n");
		ret = -ENOMEM;
		goto err_ev_ctx_map;
	}

	/* Map the host command context */
	ret = mhi_cntrl->map_addr(mhi_cntrl, mhi_cntrl->cmd_ctx_cache_phys,
				  mhi_cntrl->cmd_ctx_host_pa, mhi_cntrl->cmd_ctx_host_size);
	if (ret) {
		dev_err(dev, "Failed to map cmd_ctx_cache\n");
		goto err_cmd_ctx;
	}

	/* Initialize command ring */
	ret = mhi_ep_ring_start(mhi_cntrl, &mhi_cntrl->mhi_cmd->ring,
				(union mhi_ep_ring_ctx *)mhi_cntrl->cmd_ctx_cache);
	if (ret) {
		dev_err(dev, "Failed to start the command ring\n");
		goto err_cmd_ctx_map;
	}

	return ret;

err_cmd_ctx_map:
	mhi_cntrl->unmap_addr(mhi_cntrl, mhi_cntrl->cmd_ctx_cache_phys);

err_cmd_ctx:
	mhi_cntrl->free_addr(mhi_cntrl, mhi_cntrl->cmd_ctx_cache_phys,
			     mhi_cntrl->cmd_ctx_cache, mhi_cntrl->cmd_ctx_host_size);

err_ev_ctx_map:
	mhi_cntrl->unmap_addr(mhi_cntrl, mhi_cntrl->ev_ctx_cache_phys);

err_ev_ctx:
	mhi_cntrl->free_addr(mhi_cntrl, mhi_cntrl->ev_ctx_cache_phys,
			     mhi_cntrl->ev_ctx_cache, mhi_cntrl->ev_ctx_host_size);

err_ch_ctx_map:
	mhi_cntrl->unmap_addr(mhi_cntrl, mhi_cntrl->ch_ctx_cache_phys);

err_ch_ctx:
	mhi_cntrl->free_addr(mhi_cntrl, mhi_cntrl->ch_ctx_cache_phys,
			     mhi_cntrl->ch_ctx_cache, mhi_cntrl->ch_ctx_host_size);

	return ret;
}

static void mhi_ep_free_host_cfg(struct mhi_ep_cntrl *mhi_cntrl)
{
	mhi_cntrl->unmap_addr(mhi_cntrl, mhi_cntrl->cmd_ctx_cache_phys);
	mhi_cntrl->free_addr(mhi_cntrl, mhi_cntrl->cmd_ctx_cache_phys,
			     mhi_cntrl->cmd_ctx_cache, mhi_cntrl->cmd_ctx_host_size);
	mhi_cntrl->unmap_addr(mhi_cntrl, mhi_cntrl->ev_ctx_cache_phys);
	mhi_cntrl->free_addr(mhi_cntrl, mhi_cntrl->ev_ctx_cache_phys,
			     mhi_cntrl->ev_ctx_cache, mhi_cntrl->ev_ctx_host_size);
	mhi_cntrl->unmap_addr(mhi_cntrl, mhi_cntrl->ch_ctx_cache_phys);
	mhi_cntrl->free_addr(mhi_cntrl, mhi_cntrl->ch_ctx_cache_phys,
			     mhi_cntrl->ch_ctx_cache, mhi_cntrl->ch_ctx_host_size);
}

static void mhi_ep_enable_int(struct mhi_ep_cntrl *mhi_cntrl)
{
	mhi_ep_mmio_enable_chdb_interrupts(mhi_cntrl);
	mhi_ep_mmio_enable_ctrl_interrupt(mhi_cntrl);
	mhi_ep_mmio_enable_cmdb_interrupt(mhi_cntrl);
}

static int mhi_ep_enable(struct mhi_ep_cntrl *mhi_cntrl)
{
	struct device *dev = &mhi_cntrl->mhi_dev->dev;
	enum mhi_state state;
	u32 max_cnt = 0;
	bool mhi_reset;
	int ret;

	/* Wait for Host to set the M0 state */
	do {
		msleep(MHI_SUSPEND_MIN);
		mhi_ep_mmio_get_mhi_state(mhi_cntrl, &state, &mhi_reset);
		if (mhi_reset) {
			/* Clear the MHI reset if host is in reset state */
			mhi_ep_mmio_clear_reset(mhi_cntrl);
			dev_dbg(dev, "Host initiated reset while waiting for M0\n");
		}
		max_cnt++;
	} while (state != MHI_STATE_M0 && max_cnt < MHI_SUSPEND_TIMEOUT);

	if (state == MHI_STATE_M0) {
		ret = mhi_ep_cache_host_cfg(mhi_cntrl);
		if (ret) {
			dev_err(dev, "Failed to cache host config\n");
			return ret;
		}

		mhi_ep_mmio_set_env(mhi_cntrl, MHI_EP_AMSS_EE);
	} else {
		dev_err(dev, "Host failed to enter M0\n");
		return -ETIMEDOUT;
	}

	/* Enable all interrupts now */
	mhi_ep_enable_int(mhi_cntrl);

	return 0;
}

static void mhi_ep_ring_worker(struct work_struct *work)
{
	struct mhi_ep_cntrl *mhi_cntrl = container_of(work,
				struct mhi_ep_cntrl, ring_work);
	struct device *dev = &mhi_cntrl->mhi_dev->dev;
	struct mhi_ep_ring *ring;
	struct list_head *cp, *q;
	unsigned long flags;
	int ret = 0;

	/* Process the command ring first */
	ret = mhi_ep_process_ring(&mhi_cntrl->mhi_cmd->ring);
	if (ret) {
		dev_err(dev, "Error processing command ring\n");
		goto err_unlock;
	}

	spin_lock_irqsave(&mhi_cntrl->transition_lock, flags);
	/* Process the channel rings now */
	list_for_each_safe(cp, q, &mhi_cntrl->ring_transition_list) {
		ring = list_entry(cp, struct mhi_ep_ring, list);
		list_del(cp);
		ret = mhi_ep_process_ring(ring);
		if (ret) {
			dev_err(dev, "Error processing channel ring: %d\n", ring->ch_id);
			goto err_unlock;
		}

		/* Re-enable channel interrupt */
		mhi_ep_mmio_enable_chdb_a7(mhi_cntrl, ring->ch_id);
	}

err_unlock:
	spin_unlock_irqrestore(&mhi_cntrl->transition_lock, flags);
}

static void mhi_ep_queue_channel_db(struct mhi_ep_cntrl *mhi_cntrl,
				    unsigned long ch_int, u32 ch_idx)
{
	struct mhi_ep_ring *ring;
	unsigned int i;

	for_each_set_bit(i, &ch_int, 32) {
		/* Channel index varies for each register: 0, 32, 64, 96 */
		i += ch_idx;
		ring = &mhi_cntrl->mhi_chan[i].ring;

		spin_lock(&mhi_cntrl->transition_lock);
		list_add(&ring->list, &mhi_cntrl->ring_transition_list);
		spin_unlock(&mhi_cntrl->transition_lock);
		/*
		 * Disable the channel interrupt here and enable it once
		 * the current interrupt got serviced
		 */
		mhi_ep_mmio_disable_chdb_a7(mhi_cntrl, i);
		queue_work(mhi_cntrl->ring_wq, &mhi_cntrl->ring_work);
	}
}

/*
 * Channel interrupt statuses are contained in 4 registers each of 32bit length.
 * For checking all interrupts, we need to loop through each registers and then
 * check for bits set.
 */
static void mhi_ep_check_channel_interrupt(struct mhi_ep_cntrl *mhi_cntrl)
{
	struct device *dev = &mhi_cntrl->mhi_dev->dev;
	u32 ch_int, ch_idx;
	int i;

	mhi_ep_mmio_read_chdb_status_interrupts(mhi_cntrl);

	for (i = 0; i < MHI_MASK_ROWS_CH_EV_DB; i++) {
		ch_idx = i * MHI_MASK_CH_EV_LEN;

		/* Only process channel interrupt if the mask is enabled */
		ch_int = (mhi_cntrl->chdb[i].status & mhi_cntrl->chdb[i].mask);
		if (ch_int) {
			dev_dbg(dev, "Processing channel doorbell interrupt\n");
			mhi_ep_queue_channel_db(mhi_cntrl, ch_int, ch_idx);
			mhi_ep_mmio_write(mhi_cntrl, MHI_CHDB_INT_CLEAR_A7_n(i),
							mhi_cntrl->chdb[i].status);
		}
	}
}

void mhi_ep_state_worker(struct work_struct *work)
{
	struct mhi_ep_cntrl *mhi_cntrl = container_of(work, struct mhi_ep_cntrl, state_work);
	struct device *dev = &mhi_cntrl->mhi_dev->dev;
	struct mhi_ep_state_transition *itr, *tmp;
	unsigned long flags;
	LIST_HEAD(head);
	int ret;

	spin_lock_irqsave(&mhi_cntrl->transition_lock, flags);
	list_splice_tail_init(&mhi_cntrl->st_transition_list, &head);
	spin_unlock_irqrestore(&mhi_cntrl->transition_lock, flags);

	list_for_each_entry_safe(itr, tmp, &head, node) {
		list_del(&itr->node);
		dev_dbg(dev, "Handling MHI state transition to %s\n",
			 TO_MHI_STATE_STR(itr->state));

		switch (itr->state) {
		case MHI_STATE_M0:
			ret = mhi_ep_set_m0_state(mhi_cntrl);
			if (ret)
				dev_err(dev, "Failed to transition to M0 state\n");
			break;
		case MHI_STATE_M3:
			ret = mhi_ep_set_m3_state(mhi_cntrl);
			if (ret)
				dev_err(dev, "Failed to transition to M3 state\n");
			break;
		default:
			dev_err(dev, "Invalid MHI state transition: %d", itr->state);
			break;
		}
	}
}

static void mhi_ep_process_ctrl_interrupt(struct mhi_ep_cntrl *mhi_cntrl,
					 enum mhi_state state)
{
	struct mhi_ep_state_transition *item = kmalloc(sizeof(*item), GFP_ATOMIC);

	item->state = state;
	spin_lock(&mhi_cntrl->transition_lock);
	list_add_tail(&item->node, &mhi_cntrl->st_transition_list);
	spin_unlock(&mhi_cntrl->transition_lock);

	queue_work(mhi_cntrl->state_wq, &mhi_cntrl->state_work);
}

static void mhi_ep_abort_transfer(struct mhi_ep_cntrl *mhi_cntrl)
{
	struct mhi_ep_ring *ch_ring, *ev_ring;
	struct mhi_result result = {};
	struct mhi_ep_chan *mhi_chan;
	int i;

	/* Stop all the channels */
	for (i = 0; i < mhi_cntrl->max_chan; i++) {
		ch_ring = &mhi_cntrl->mhi_chan[i].ring;
		if (ch_ring->state == RING_STATE_UINT)
			continue;

		mhi_chan = &mhi_cntrl->mhi_chan[i];
		mutex_lock(&mhi_chan->lock);
		/* Send channel disconnect status to client drivers */
		if (mhi_chan->xfer_cb) {
			result.transaction_status = -ENOTCONN;
			result.bytes_xferd = 0;
			mhi_chan->xfer_cb(mhi_chan->mhi_dev, &result);
		}

		/* Set channel state to DISABLED */
		mhi_chan->state = MHI_CH_STATE_DISABLED;
		mutex_unlock(&mhi_chan->lock);
	}

	flush_workqueue(mhi_cntrl->ring_wq);
	flush_workqueue(mhi_cntrl->state_wq);

	/* Destroy devices associated with all channels */
	device_for_each_child(&mhi_cntrl->mhi_dev->dev, NULL, mhi_ep_destroy_device);

	/* Stop and reset the transfer rings */
	for (i = 0; i < mhi_cntrl->max_chan; i++) {
		ch_ring = &mhi_cntrl->mhi_chan[i].ring;
		if (ch_ring->state == RING_STATE_UINT)
			continue;

		mhi_chan = &mhi_cntrl->mhi_chan[i];
		mutex_lock(&mhi_chan->lock);
		mhi_ep_ring_stop(mhi_cntrl, ch_ring);
		mutex_unlock(&mhi_chan->lock);
	}

	/* Stop and reset the event rings */
	for (i = 0; i < mhi_cntrl->event_rings; i++) {
		ev_ring = &mhi_cntrl->mhi_event[i].ring;
		if (ev_ring->state == RING_STATE_UINT)
			continue;

		mutex_lock(&mhi_cntrl->event_lock);
		mhi_ep_ring_stop(mhi_cntrl, ev_ring);
		mutex_unlock(&mhi_cntrl->event_lock);
	}

	/* Stop and reset the command ring */
	mhi_ep_ring_stop(mhi_cntrl, &mhi_cntrl->mhi_cmd->ring);

	mhi_ep_free_host_cfg(mhi_cntrl);
	mhi_ep_mmio_mask_interrupts(mhi_cntrl);

	mhi_cntrl->is_enabled = false;
}

/*
 * Interrupt handler that services interrupts raised by the host writing to
 * MHICTRL and Command ring doorbell (CRDB) registers for state change and
 * channel interrupts.
 */
static irqreturn_t mhi_ep_irq(int irq, void *data)
{
	struct mhi_ep_cntrl *mhi_cntrl = data;

	struct device *dev = &mhi_cntrl->mhi_dev->dev;
	enum mhi_state state;
	u32 int_value = 0;
	bool mhi_reset;

	/* Acknowledge the interrupts */
	mhi_ep_mmio_read(mhi_cntrl, MHI_CTRL_INT_STATUS_A7, &int_value);
	mhi_ep_mmio_write(mhi_cntrl, MHI_CTRL_INT_CLEAR_A7, int_value);

	/* Check for ctrl interrupt */
	if (FIELD_GET(MHI_CTRL_INT_STATUS_A7_MSK, int_value)) {
		dev_dbg(dev, "Processing ctrl interrupt\n");
		mhi_ep_mmio_get_mhi_state(mhi_cntrl, &state, &mhi_reset);
		if (mhi_reset) {
			dev_info(dev, "Host triggered MHI reset!\n");
			disable_irq_nosync(mhi_cntrl->irq);
			schedule_work(&mhi_cntrl->reset_work);
			return IRQ_HANDLED;
		}

		mhi_ep_process_ctrl_interrupt(mhi_cntrl, state);
	}

	/* Check for command doorbell interrupt */
	if (FIELD_GET(MHI_CTRL_INT_STATUS_CRDB_MSK, int_value)) {
		dev_dbg(dev, "Processing command doorbell interrupt\n");
		queue_work(mhi_cntrl->ring_wq, &mhi_cntrl->ring_work);
	}

	/* Check for channel interrupts */
	mhi_ep_check_channel_interrupt(mhi_cntrl);

	return IRQ_HANDLED;
}

void mhi_ep_reset_worker(struct work_struct *work)
{
	struct mhi_ep_cntrl *mhi_cntrl = container_of(work, struct mhi_ep_cntrl, reset_work);
	struct device *dev = &mhi_cntrl->mhi_dev->dev;
	enum mhi_state cur_state;
	int ret;

	mhi_ep_abort_transfer(mhi_cntrl);

	spin_lock_bh(&mhi_cntrl->state_lock);
	/* Reset MMIO to signal host that the MHI_RESET is completed in endpoint */
	mhi_ep_mmio_reset(mhi_cntrl);
	cur_state = mhi_cntrl->mhi_state;
	spin_unlock_bh(&mhi_cntrl->state_lock);

	/*
	 * Only proceed further if the reset is due to SYS_ERR. The host will
	 * issue reset during shutdown also and we don't need to do re-init in
	 * that case.
	 */
	if (cur_state == MHI_STATE_SYS_ERR) {
		mhi_ep_mmio_init(mhi_cntrl);

		/* Set AMSS EE before signaling ready state */
		mhi_ep_mmio_set_env(mhi_cntrl, MHI_EP_AMSS_EE);

		/* All set, notify the host that we are ready */
		ret = mhi_ep_set_ready_state(mhi_cntrl);
		if (ret)
			return;

		dev_dbg(dev, "READY state notification sent to the host\n");

		ret = mhi_ep_enable(mhi_cntrl);
		if (ret) {
			dev_err(dev, "Failed to enable MHI endpoint: %d\n", ret);
			return;
		}

		enable_irq(mhi_cntrl->irq);
	}
}

void mhi_ep_init_worker(struct work_struct *work)
{
	struct mhi_ep_cntrl *mhi_cntrl = container_of(work, struct mhi_ep_cntrl, init_work);
	struct device *dev = &mhi_cntrl->mhi_dev->dev;
	int ret, i;

	/*
	 * Mask all interrupts until the state machine is ready. Interrupts will
	 * be enabled later with mhi_ep_enable().
	 */
	mhi_ep_mmio_mask_interrupts(mhi_cntrl);
	mhi_ep_mmio_init(mhi_cntrl);

	mhi_cntrl->mhi_event = kzalloc(mhi_cntrl->event_rings * (sizeof(*mhi_cntrl->mhi_event)),
				       GFP_KERNEL);
	if (!mhi_cntrl->mhi_event)
		return;

	/* Initialize command, channel and event rings */
	mhi_ep_ring_init(&mhi_cntrl->mhi_cmd->ring, RING_TYPE_CMD, 0);
	for (i = 0; i < mhi_cntrl->max_chan; i++)
		mhi_ep_ring_init(&mhi_cntrl->mhi_chan[i].ring, RING_TYPE_CH, i);
	for (i = 0; i < mhi_cntrl->event_rings; i++)
		mhi_ep_ring_init(&mhi_cntrl->mhi_event[i].ring, RING_TYPE_ER, i);

	spin_lock_bh(&mhi_cntrl->state_lock);
	mhi_cntrl->mhi_state = MHI_STATE_RESET;
	spin_unlock_bh(&mhi_cntrl->state_lock);

	/* Set AMSS EE before signaling ready state */
	mhi_ep_mmio_set_env(mhi_cntrl, MHI_EP_AMSS_EE);

	/* All set, notify the host that we are ready */
	ret = mhi_ep_set_ready_state(mhi_cntrl);
	if (ret)
		goto err_free_event;

	dev_dbg(dev, "READY state notification sent to the host\n");

	ret = mhi_ep_enable(mhi_cntrl);
	if (ret) {
		dev_err(dev, "Failed to enable MHI endpoint: %d\n", ret);
		goto err_free_event;
	}

	enable_irq(mhi_cntrl->irq);

	return;

err_free_event:
	kfree(mhi_cntrl->mhi_event);
}

static void skip_to_next_td(struct mhi_ep_chan *mhi_chan, struct mhi_ep_ring *ring)
{
	struct mhi_ep_ring_element *el;
	u32 td_boundary_reached = 0;

	mhi_chan->skip_td = 1;
	el = &ring->ring_cache[ring->rd_offset];
	while (ring->rd_offset != ring->wr_offset) {
		if (td_boundary_reached) {
			mhi_chan->skip_td = 0;
			break;
		}

		if (!MHI_EP_TRE_GET_CHAIN(el))
			td_boundary_reached = 1;

		mhi_ep_ring_inc_index(ring);
		el = &ring->ring_cache[ring->rd_offset];
	}
}

bool mhi_ep_queue_is_empty(struct mhi_ep_device *mhi_dev, enum dma_data_direction dir)
{
	struct mhi_ep_chan *mhi_chan = (dir == DMA_FROM_DEVICE) ? mhi_dev->dl_chan :
							       mhi_dev->ul_chan;
	struct mhi_ep_cntrl *mhi_cntrl = mhi_dev->mhi_cntrl;
	struct mhi_ep_ring *ring = &mhi_cntrl->mhi_chan[mhi_chan->chan].ring;

	return !!(ring->rd_offset == ring->wr_offset);
}
EXPORT_SYMBOL_GPL(mhi_ep_queue_is_empty);

int mhi_ep_queue_skb(struct mhi_ep_device *mhi_dev, enum dma_data_direction dir,
		     struct sk_buff *skb, size_t len, enum mhi_flags mflags)
{
	struct mhi_ep_chan *mhi_chan = (dir == DMA_FROM_DEVICE) ? mhi_dev->dl_chan :
							       mhi_dev->ul_chan;
	size_t usr_buf_offset, bytes_to_write;
	struct mhi_ep_cntrl *mhi_cntrl = mhi_dev->mhi_cntrl;
	struct device *dev = &mhi_cntrl->mhi_dev->dev;
	enum mhi_ev_ccs code = MHI_EV_CC_INVALID;
	u64 write_to_loc, skip_tre = 0;
	struct mhi_ep_ring_element *el;
	struct mhi_ep_ring *ring;
	u32 buf_remaining;
	void *read_from_loc;
	void __iomem *tre_buf;
	phys_addr_t tre_phys;
	u32 tre_len;
	int ret = 0;

	if (dir == DMA_TO_DEVICE)
		return -EINVAL;

	buf_remaining = len;
	ring = &mhi_cntrl->mhi_chan[mhi_chan->chan].ring;

	mutex_lock(&mhi_chan->lock);
	if (mhi_chan->skip_td)
		skip_to_next_td(mhi_chan, ring);

	do {
		/* Don't process the transfer ring if the channel is not in RUNNING state */
		if (mhi_chan->state != MHI_CH_STATE_RUNNING) {
			dev_err(dev, "Channel (%d) not available", mhi_chan->chan);
			ret = -ENODEV;
			goto err_exit;
		}

		if (mhi_ep_queue_is_empty(mhi_dev, dir)) {
			dev_err(dev, "TRE not available!\n");
			ret = -EINVAL;
			goto err_exit;
		}

		el = &ring->ring_cache[ring->rd_offset];
		tre_len = MHI_EP_TRE_GET_LEN(el);
		if (skb->len > tre_len) {
			dev_err(dev, "Buffer size (%d) is too large for TRE length (%d)!\n",
				skb->len, tre_len);
			ret = -ENOMEM;
			goto err_exit;
		}

		bytes_to_write = min(buf_remaining, tre_len);
		usr_buf_offset = skb->len - bytes_to_write;
		read_from_loc = skb->data;
		write_to_loc = MHI_EP_TRE_GET_PTR(el);

		tre_buf = mhi_cntrl->alloc_addr(mhi_cntrl, &tre_phys, bytes_to_write);
		if (!tre_buf) {
			dev_err(dev, "Failed to allocate TRE buffer\n");
			ret = -ENOMEM;
			goto err_exit;
		}

		ret = mhi_cntrl->map_addr(mhi_cntrl, tre_phys, write_to_loc, bytes_to_write);
		if (ret) {
			dev_err(dev, "Failed to map TRE buffer\n");
			goto err_tre_free;
		}

		dev_dbg(dev, "Writing %d bytes to channel (%d)", bytes_to_write, ring->ch_id);
		memcpy_toio(tre_buf, read_from_loc, bytes_to_write);

		mhi_cntrl->unmap_addr(mhi_cntrl, tre_phys);
		mhi_cntrl->free_addr(mhi_cntrl, tre_phys, tre_buf, bytes_to_write);

		buf_remaining -= bytes_to_write;
		if (buf_remaining) {
			if (!MHI_EP_TRE_GET_CHAIN(el))
				code = MHI_EV_CC_OVERFLOW;
			else if (MHI_EP_TRE_GET_IEOB(el))
				code = MHI_EV_CC_EOB;
		} else {
			if (MHI_EP_TRE_GET_CHAIN(el))
				skip_tre = 1;
			code = MHI_EV_CC_EOT;
		}

		ret = mhi_ep_send_completion_event(mhi_cntrl, ring, bytes_to_write, code);
		if (ret) {
			dev_err(dev, "Error sending completion event for channel (%d)\n",
				ring->ch_id);
			goto err_exit;
		}

		mhi_ep_ring_inc_index(ring);
	} while (!skip_tre && buf_remaining);

	if (skip_tre)
		skip_to_next_td(mhi_chan, ring);

	mutex_unlock(&mhi_chan->lock);

	return 0;

err_tre_free:
	mhi_cntrl->free_addr(mhi_cntrl, tre_phys, tre_buf, bytes_to_write);
err_exit:
	mutex_unlock(&mhi_chan->lock);

	return ret;
}
EXPORT_SYMBOL_GPL(mhi_ep_queue_skb);

void mhi_ep_power_down(struct mhi_ep_cntrl *mhi_cntrl)
{
	if (mhi_cntrl->is_enabled)
		mhi_ep_abort_transfer(mhi_cntrl);
	disable_irq(mhi_cntrl->irq);
}
EXPORT_SYMBOL_GPL(mhi_ep_power_down);

void mhi_ep_power_up(struct mhi_ep_cntrl *mhi_cntrl)
{
	schedule_work(&mhi_cntrl->init_work);
	mhi_cntrl->is_enabled = true;
}
EXPORT_SYMBOL_GPL(mhi_ep_power_up);

static void mhi_ep_release_device(struct device *dev)
{
	struct mhi_ep_device *mhi_dev = to_mhi_ep_device(dev);

	/*
	 * We need to set the mhi_chan->mhi_dev to NULL here since the MHI
	 * devices for the channels will only get created if the mhi_dev
	 * associated with it is NULL.
	 */
	if (mhi_dev->ul_chan)
		mhi_dev->ul_chan->mhi_dev = NULL;

	if (mhi_dev->dl_chan)
		mhi_dev->dl_chan->mhi_dev = NULL;

	kfree(mhi_dev);
}

struct mhi_ep_device *mhi_ep_alloc_device(struct mhi_ep_cntrl *mhi_cntrl)
{
	struct mhi_ep_device *mhi_dev;
	struct device *dev;

	mhi_dev = kzalloc(sizeof(*mhi_dev), GFP_KERNEL);
	if (!mhi_dev)
		return ERR_PTR(-ENOMEM);

	dev = &mhi_dev->dev;
	device_initialize(dev);
	dev->bus = &mhi_ep_bus_type;
	dev->release = mhi_ep_release_device;

	if (mhi_cntrl->mhi_dev) {
		/* for MHI client devices, parent is the MHI controller device */
		dev->parent = &mhi_cntrl->mhi_dev->dev;
	} else {
		/* for MHI controller device, parent is the bus device (e.g. PCI EPF) */
		dev->parent = mhi_cntrl->cntrl_dev;
	}

	mhi_dev->mhi_cntrl = mhi_cntrl;

	return mhi_dev;
}

static int mhi_ep_create_device(struct mhi_ep_cntrl *mhi_cntrl, u32 ch_id)
{
	struct mhi_ep_device *mhi_dev;
	struct mhi_ep_chan *mhi_chan = &mhi_cntrl->mhi_chan[ch_id];
	int ret;

	mhi_dev = mhi_ep_alloc_device(mhi_cntrl);
	if (IS_ERR(mhi_dev))
		return PTR_ERR(mhi_dev);

	mhi_dev->dev_type = MHI_DEVICE_XFER;

	/* Configure primary channel */
	if (mhi_chan->dir == DMA_TO_DEVICE) {
		mhi_dev->ul_chan = mhi_chan;
		mhi_dev->ul_chan_id = mhi_chan->chan;
	} else {
		mhi_dev->dl_chan = mhi_chan;
		mhi_dev->dl_chan_id = mhi_chan->chan;
	}

	get_device(&mhi_dev->dev);
	mhi_chan->mhi_dev = mhi_dev;

	/* Configure secondary channel as well */
	mhi_chan++;
	if (mhi_chan->dir == DMA_TO_DEVICE) {
		mhi_dev->ul_chan = mhi_chan;
		mhi_dev->ul_chan_id = mhi_chan->chan;
	} else {
		mhi_dev->dl_chan = mhi_chan;
		mhi_dev->dl_chan_id = mhi_chan->chan;
	}

	get_device(&mhi_dev->dev);
	mhi_chan->mhi_dev = mhi_dev;

	/* Channel name is same for both UL and DL */
	mhi_dev->name = mhi_chan->name;
	dev_set_name(&mhi_dev->dev, "%s_%s",
		     dev_name(&mhi_cntrl->mhi_dev->dev),
		     mhi_dev->name);

	ret = device_add(&mhi_dev->dev);
	if (ret)
		put_device(&mhi_dev->dev);

	return ret;
}

static int mhi_ep_destroy_device(struct device *dev, void *data)
{
	struct mhi_ep_device *mhi_dev;
	struct mhi_ep_cntrl *mhi_cntrl;
	struct mhi_ep_chan *ul_chan, *dl_chan;

	if (dev->bus != &mhi_ep_bus_type)
		return 0;

	mhi_dev = to_mhi_ep_device(dev);
	mhi_cntrl = mhi_dev->mhi_cntrl;

	/* Only destroy devices created for channels */
	if (mhi_dev->dev_type == MHI_DEVICE_CONTROLLER)
		return 0;

	ul_chan = mhi_dev->ul_chan;
	dl_chan = mhi_dev->dl_chan;

	if (ul_chan)
		put_device(&ul_chan->mhi_dev->dev);

	if (dl_chan)
		put_device(&dl_chan->mhi_dev->dev);

	dev_dbg(&mhi_cntrl->mhi_dev->dev, "Destroying device for chan:%s\n",
		 mhi_dev->name);

	/* Notify the client and remove the device from MHI bus */
	device_del(dev);
	put_device(dev);

	return 0;
}

static int parse_ch_cfg(struct mhi_ep_cntrl *mhi_cntrl,
			const struct mhi_ep_cntrl_config *config)
{
	const struct mhi_ep_channel_config *ch_cfg;
	struct device *dev = mhi_cntrl->cntrl_dev;
	u32 chan, i;
	int ret = -EINVAL;

	mhi_cntrl->max_chan = config->max_channels;

	/*
	 * Allocate max_channels supported by the MHI endpoint and populate
	 * only the defined channels
	 */
	mhi_cntrl->mhi_chan = kcalloc(mhi_cntrl->max_chan, sizeof(*mhi_cntrl->mhi_chan),
				      GFP_KERNEL);
	if (!mhi_cntrl->mhi_chan)
		return -ENOMEM;

	for (i = 0; i < config->num_channels; i++) {
		struct mhi_ep_chan *mhi_chan;

		ch_cfg = &config->ch_cfg[i];

		chan = ch_cfg->num;
		if (chan >= mhi_cntrl->max_chan) {
			dev_err(dev, "Channel %d not available\n", chan);
			goto error_chan_cfg;
		}

		mhi_chan = &mhi_cntrl->mhi_chan[chan];
		mhi_chan->name = ch_cfg->name;
		mhi_chan->chan = chan;
		mhi_chan->dir = ch_cfg->dir;
		mutex_init(&mhi_chan->lock);

		/* Bi-directional and direction less channels are not supported */
		if (mhi_chan->dir == DMA_BIDIRECTIONAL || mhi_chan->dir == DMA_NONE) {
			dev_err(dev, "Invalid channel configuration\n");
			goto error_chan_cfg;
		}
	}

	return 0;

error_chan_cfg:
	kfree(mhi_cntrl->mhi_chan);

	return ret;
}

/*
 * Allocate channel and command rings here. Event rings will be allocated
 * in mhi_ep_init_worker() as the config comes from the host.
 */
int mhi_ep_register_controller(struct mhi_ep_cntrl *mhi_cntrl,
				const struct mhi_ep_cntrl_config *config)
{
	struct mhi_ep_device *mhi_dev;
	int ret;

	if (!mhi_cntrl || !mhi_cntrl->cntrl_dev || !mhi_cntrl->mmio || !mhi_cntrl->irq)
		return -EINVAL;

	ret = parse_ch_cfg(mhi_cntrl, config);
	if (ret)
		return ret;

	mhi_cntrl->mhi_cmd = kcalloc(NR_OF_CMD_RINGS, sizeof(*mhi_cntrl->mhi_cmd), GFP_KERNEL);
	if (!mhi_cntrl->mhi_cmd) {
		ret = -ENOMEM;
		goto err_free_ch;
	}

	INIT_WORK(&mhi_cntrl->ring_work, mhi_ep_ring_worker);
	INIT_WORK(&mhi_cntrl->state_work, mhi_ep_state_worker);
	INIT_WORK(&mhi_cntrl->init_work, mhi_ep_init_worker);
	INIT_WORK(&mhi_cntrl->reset_work, mhi_ep_reset_worker);

	mhi_cntrl->ring_wq = alloc_ordered_workqueue("mhi_ep_ring_wq", WQ_HIGHPRI);
	if (!mhi_cntrl->ring_wq) {
		ret = -ENOMEM;
		goto err_free_cmd;
	}

	mhi_cntrl->state_wq = alloc_ordered_workqueue("mhi_ep_state_wq", WQ_HIGHPRI);
	if (!mhi_cntrl->state_wq) {
		ret = -ENOMEM;
		goto err_destroy_ring_wq;
	}

	INIT_LIST_HEAD(&mhi_cntrl->ring_transition_list);
	INIT_LIST_HEAD(&mhi_cntrl->st_transition_list);
	spin_lock_init(&mhi_cntrl->transition_lock);
	spin_lock_init(&mhi_cntrl->state_lock);
	mutex_init(&mhi_cntrl->event_lock);

	/* Set MHI version and AMSS EE before enumeration */
	mhi_ep_mmio_write(mhi_cntrl, MHIVER, config->mhi_version);
	mhi_ep_mmio_set_env(mhi_cntrl, MHI_EP_AMSS_EE);

	/* Set controller index */
	mhi_cntrl->index = ida_alloc(&mhi_ep_cntrl_ida, GFP_KERNEL);
	if (mhi_cntrl->index < 0) {
		ret = mhi_cntrl->index;
		goto err_destroy_state_wq;
	}

	irq_set_status_flags(mhi_cntrl->irq, IRQ_NOAUTOEN);
	ret = request_irq(mhi_cntrl->irq, mhi_ep_irq, IRQF_TRIGGER_HIGH,
			  "doorbell_irq", mhi_cntrl);
	if (ret) {
		dev_err(mhi_cntrl->cntrl_dev, "Failed to request Doorbell IRQ: %d\n", ret);
		goto err_ida_free;
	}

	/* Allocate the controller device */
	mhi_dev = mhi_ep_alloc_device(mhi_cntrl);
	if (IS_ERR(mhi_dev)) {
		dev_err(mhi_cntrl->cntrl_dev, "Failed to allocate controller device\n");
		ret = PTR_ERR(mhi_dev);
		goto err_free_irq;
	}

	mhi_dev->dev_type = MHI_DEVICE_CONTROLLER;
	dev_set_name(&mhi_dev->dev, "mhi_ep%d", mhi_cntrl->index);
	mhi_dev->name = dev_name(&mhi_dev->dev);

	ret = device_add(&mhi_dev->dev);
	if (ret)
		goto err_release_dev;

	mhi_cntrl->mhi_dev = mhi_dev;

	dev_dbg(&mhi_dev->dev, "MHI EP Controller registered\n");

	return 0;

err_release_dev:
	put_device(&mhi_dev->dev);
err_free_irq:
	free_irq(mhi_cntrl->irq, mhi_cntrl);
err_ida_free:
	ida_free(&mhi_ep_cntrl_ida, mhi_cntrl->index);
err_destroy_state_wq:
	destroy_workqueue(mhi_cntrl->state_wq);
err_destroy_ring_wq:
	destroy_workqueue(mhi_cntrl->ring_wq);
err_free_cmd:
	kfree(mhi_cntrl->mhi_cmd);
err_free_ch:
	kfree(mhi_cntrl->mhi_chan);

	return ret;
}
EXPORT_SYMBOL_GPL(mhi_ep_register_controller);

/*
 * It is expected that the controller drivers will power down the MHI EP stack
 * using "mhi_ep_power_down()" before calling this function to unregister themselves.
 */
void mhi_ep_unregister_controller(struct mhi_ep_cntrl *mhi_cntrl)
{
	struct mhi_ep_device *mhi_dev = mhi_cntrl->mhi_dev;

	destroy_workqueue(mhi_cntrl->state_wq);
	destroy_workqueue(mhi_cntrl->ring_wq);

	free_irq(mhi_cntrl->irq, mhi_cntrl);

	kfree(mhi_cntrl->mhi_cmd);
	kfree(mhi_cntrl->mhi_event);
	kfree(mhi_cntrl->mhi_chan);

	device_del(&mhi_dev->dev);
	put_device(&mhi_dev->dev);

	ida_free(&mhi_ep_cntrl_ida, mhi_cntrl->index);
}
EXPORT_SYMBOL_GPL(mhi_ep_unregister_controller);

static int mhi_ep_driver_probe(struct device *dev)
{
	struct mhi_ep_device *mhi_dev = to_mhi_ep_device(dev);
	struct mhi_ep_driver *mhi_drv = to_mhi_ep_driver(dev->driver);
	struct mhi_ep_chan *ul_chan = mhi_dev->ul_chan;
	struct mhi_ep_chan *dl_chan = mhi_dev->dl_chan;

	if (ul_chan)
		ul_chan->xfer_cb = mhi_drv->ul_xfer_cb;

	if (dl_chan)
		dl_chan->xfer_cb = mhi_drv->dl_xfer_cb;

	return mhi_drv->probe(mhi_dev, mhi_dev->id);
}

static int mhi_ep_driver_remove(struct device *dev)
{
	struct mhi_ep_device *mhi_dev = to_mhi_ep_device(dev);
	struct mhi_ep_driver *mhi_drv = to_mhi_ep_driver(dev->driver);
	struct mhi_result result = {};
	struct mhi_ep_chan *mhi_chan;
	int dir;

	/* Skip if it is a controller device */
	if (mhi_dev->dev_type == MHI_DEVICE_CONTROLLER)
		return 0;

	/* Disconnect the channels associated with the driver */
	for (dir = 0; dir < 2; dir++) {
		mhi_chan = dir ? mhi_dev->ul_chan : mhi_dev->dl_chan;

		if (!mhi_chan)
			continue;

		mutex_lock(&mhi_chan->lock);
		/* Send channel disconnect status to the client driver */
		if (mhi_chan->xfer_cb) {
			result.transaction_status = -ENOTCONN;
			result.bytes_xferd = 0;
			mhi_chan->xfer_cb(mhi_chan->mhi_dev, &result);
		}

		/* Set channel state to DISABLED */
		mhi_chan->state = MHI_CH_STATE_DISABLED;
		mhi_chan->xfer_cb = NULL;
		mutex_unlock(&mhi_chan->lock);
	}

	/* Remove the client driver now */
	mhi_drv->remove(mhi_dev);

	return 0;
}

int __mhi_ep_driver_register(struct mhi_ep_driver *mhi_drv, struct module *owner)
{
	struct device_driver *driver = &mhi_drv->driver;

	if (!mhi_drv->probe || !mhi_drv->remove)
		return -EINVAL;

	driver->bus = &mhi_ep_bus_type;
	driver->owner = owner;
	driver->probe = mhi_ep_driver_probe;
	driver->remove = mhi_ep_driver_remove;

	return driver_register(driver);
}
EXPORT_SYMBOL_GPL(__mhi_ep_driver_register);

void mhi_ep_driver_unregister(struct mhi_ep_driver *mhi_drv)
{
	driver_unregister(&mhi_drv->driver);
}
EXPORT_SYMBOL_GPL(mhi_ep_driver_unregister);

static int mhi_ep_uevent(struct device *dev, struct kobj_uevent_env *env)
{
	struct mhi_ep_device *mhi_dev = to_mhi_ep_device(dev);

	return add_uevent_var(env, "MODALIAS=" MHI_EP_DEVICE_MODALIAS_FMT,
					mhi_dev->name);
}

static int mhi_ep_match(struct device *dev, struct device_driver *drv)
{
	struct mhi_ep_device *mhi_dev = to_mhi_ep_device(dev);
	struct mhi_ep_driver *mhi_drv = to_mhi_ep_driver(drv);
	const struct mhi_device_id *id;

	/*
	 * If the device is a controller type then there is no client driver
	 * associated with it
	 */
	if (mhi_dev->dev_type == MHI_DEVICE_CONTROLLER)
		return 0;

	for (id = mhi_drv->id_table; id->chan[0]; id++)
		if (!strcmp(mhi_dev->name, id->chan)) {
			mhi_dev->id = id;
			return 1;
		}

	return 0;
};

struct bus_type mhi_ep_bus_type = {
	.name = "mhi_ep",
	.dev_name = "mhi_ep",
	.match = mhi_ep_match,
	.uevent = mhi_ep_uevent,
};

static int __init mhi_ep_init(void)
{
	return bus_register(&mhi_ep_bus_type);
}

static void __exit mhi_ep_exit(void)
{
	bus_unregister(&mhi_ep_bus_type);
}

postcore_initcall(mhi_ep_init);
module_exit(mhi_ep_exit);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("MHI Bus Endpoint stack");
MODULE_AUTHOR("Manivannan Sadhasivam <manivannan.sadhasivam@linaro.org>");
