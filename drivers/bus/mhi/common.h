/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2021, Linaro Ltd.
 *
 */

#ifndef _MHI_COMMON_H
#define _MHI_COMMON_H

#include <linux/mhi.h>

/* MHI register bits */
#define MHIREGLEN_MHIREGLEN_MASK		GENMASK(31, 0)
#define MHIREGLEN_MHIREGLEN_SHIFT		0

#define MHIVER_MHIVER_MASK			GENMASK(31, 0)
#define MHIVER_MHIVER_SHIFT			0

#define MHICFG_NHWER_MASK			GENMASK(31, 24)
#define MHICFG_NHWER_SHIFT			24
#define MHICFG_NER_MASK				GENMASK(23, 16)
#define MHICFG_NER_SHIFT			16
#define MHICFG_NHWCH_MASK			GENMASK(15, 8)
#define MHICFG_NHWCH_SHIFT			8
#define MHICFG_NCH_MASK				GENMASK(7, 0)
#define MHICFG_NCH_SHIFT			0

#define CHDBOFF_CHDBOFF_MASK			GENMASK(31, 0)
#define CHDBOFF_CHDBOFF_SHIFT			0

#define ERDBOFF_ERDBOFF_MASK			GENMASK(31, 0)
#define ERDBOFF_ERDBOFF_SHIFT			0

#define BHIOFF_BHIOFF_MASK			GENMASK(31, 0)
#define BHIOFF_BHIOFF_SHIFT			0

#define BHIEOFF_BHIEOFF_MASK			GENMASK(31, 0)
#define BHIEOFF_BHIEOFF_SHIFT			0

#define DEBUGOFF_DEBUGOFF_MASK			GENMASK(31, 0)
#define DEBUGOFF_DEBUGOFF_SHIFT			0

#define MHICTRL_MHISTATE_MASK			GENMASK(15, 8)
#define MHICTRL_MHISTATE_SHIFT			8
#define MHICTRL_RESET_MASK			2
#define MHICTRL_RESET_SHIFT			1

#define MHISTATUS_MHISTATE_MASK			GENMASK(15, 8)
#define MHISTATUS_MHISTATE_SHIFT		8
#define MHISTATUS_SYSERR_MASK			4
#define MHISTATUS_SYSERR_SHIFT			2
#define MHISTATUS_READY_MASK			1
#define MHISTATUS_READY_SHIFT			0

#define CCABAP_LOWER_CCABAP_LOWER_MASK		GENMASK(31, 0)
#define CCABAP_LOWER_CCABAP_LOWER_SHIFT		0

#define CCABAP_HIGHER_CCABAP_HIGHER_MASK	GENMASK(31, 0)
#define CCABAP_HIGHER_CCABAP_HIGHER_SHIFT	0

#define ECABAP_LOWER_ECABAP_LOWER_MASK		GENMASK(31, 0)
#define ECABAP_LOWER_ECABAP_LOWER_SHIFT		0

#define ECABAP_HIGHER_ECABAP_HIGHER_MASK	GENMASK(31, 0)
#define ECABAP_HIGHER_ECABAP_HIGHER_SHIFT	0

#define CRCBAP_LOWER_CRCBAP_LOWER_MASK		GENMASK(31, 0)
#define CRCBAP_LOWER_CRCBAP_LOWER_SHIFT		0

#define CRCBAP_HIGHER_CRCBAP_HIGHER_MASK	GENMASK(31, 0)
#define CRCBAP_HIGHER_CRCBAP_HIGHER_SHIFT	0

#define CRDB_LOWER_CRDB_LOWER_MASK		GENMASK(31, 0)
#define CRDB_LOWER_CRDB_LOWER_SHIFT		0

#define CRDB_HIGHER_CRDB_HIGHER_MASK		GENMASK(31, 0)
#define CRDB_HIGHER_CRDB_HIGHER_SHIFT		0

#define MHICTRLBASE_LOWER_MHICTRLBASE_LOWER_MASK GENMASK(31, 0)
#define MHICTRLBASE_LOWER_MHICTRLBASE_LOWER_SHIFT 0

#define MHICTRLBASE_HIGHER_MHICTRLBASE_HIGHER_MASK GENMASK(31, 0)
#define MHICTRLBASE_HIGHER_MHICTRLBASE_HIGHER_SHIFT 0

#define MHICTRLLIMIT_LOWER_MHICTRLLIMIT_LOWER_MASK GENMASK(31, 0)
#define MHICTRLLIMIT_LOWER_MHICTRLLIMIT_LOWER_SHIFT 0

#define MHICTRLLIMIT_HIGHER_MHICTRLLIMIT_HIGHER_MASK GENMASK(31, 0)
#define MHICTRLLIMIT_HIGHER_MHICTRLLIMIT_HIGHER_SHIFT 0

#define MHIDATABASE_LOWER_MHIDATABASE_LOWER_MASK GENMASK(31, 0)
#define MHIDATABASE_LOWER_MHIDATABASE_LOWER_SHIFT 0

#define MHIDATABASE_HIGHER_MHIDATABASE_HIGHER_MASK GENMASK(31, 0)
#define MHIDATABASE_HIGHER_MHIDATABASE_HIGHER_SHIFT 0

#define MHIDATALIMIT_LOWER_MHIDATALIMIT_LOWER_MASK GENMASK(31, 0)
#define MHIDATALIMIT_LOWER_MHIDATALIMIT_LOWER_SHIFT 0

#define MHIDATALIMIT_HIGHER_MHIDATALIMIT_HIGHER_MASK GENMASK(31, 0)
#define MHIDATALIMIT_HIGHER_MHIDATALIMIT_HIGHER_SHIFT 0

/* Command Ring Element macros */
/* No operation command */
#define MHI_TRE_CMD_NOOP_PTR 0
#define MHI_TRE_CMD_NOOP_DWORD0 0
#define MHI_TRE_CMD_NOOP_DWORD1 (MHI_CMD_NOP << 16)

/* Channel reset command */
#define MHI_TRE_CMD_RESET_PTR 0
#define MHI_TRE_CMD_RESET_DWORD0 0
#define MHI_TRE_CMD_RESET_DWORD1(chid) ((chid << 24) | \
					(MHI_CMD_RESET_CHAN << 16))

/* Channel stop command */
#define MHI_TRE_CMD_STOP_PTR 0
#define MHI_TRE_CMD_STOP_DWORD0 0
#define MHI_TRE_CMD_STOP_DWORD1(chid) ((chid << 24) | \
				       (MHI_CMD_STOP_CHAN << 16))

/* Channel start command */
#define MHI_TRE_CMD_START_PTR 0
#define MHI_TRE_CMD_START_DWORD0 0
#define MHI_TRE_CMD_START_DWORD1(chid) ((chid << 24) | \
					(MHI_CMD_START_CHAN << 16))

#define MHI_TRE_GET_CMD_CHID(tre) (((tre)->dword[1] >> 24) & 0xff)
#define MHI_TRE_GET_CMD_TYPE(tre) (((tre)->dword[1] >> 16) & 0xff)

/* Event descriptor macros */
/* Transfer completion event */
#define MHI_TRE_EV_PTR(ptr) (ptr)
#define MHI_TRE_EV_DWORD0(code, len) ((code << 24) | len)
#define MHI_TRE_EV_DWORD1(chid, type) ((chid << 24) | (type << 16))
#define MHI_TRE_GET_EV_PTR(tre) ((tre)->ptr)
#define MHI_TRE_GET_EV_CODE(tre) (((tre)->dword[0] >> 24) & 0xff)
#define MHI_TRE_GET_EV_LEN(tre) ((tre)->dword[0] & 0xffff)
#define MHI_TRE_GET_EV_CHID(tre) (((tre)->dword[1] >> 24) & 0xff)
#define MHI_TRE_GET_EV_TYPE(tre) (((tre)->dword[1] >> 16) & 0xff)
#define MHI_TRE_GET_EV_STATE(tre) (((tre)->dword[0] >> 24) & 0xff)
#define MHI_TRE_GET_EV_EXECENV(tre) (((tre)->dword[0] >> 24) & 0xff)
#define MHI_TRE_GET_EV_SEQ(tre) ((tre)->dword[0])
#define MHI_TRE_GET_EV_TIME(tre) ((tre)->ptr)
#define MHI_TRE_GET_EV_COOKIE(tre) lower_32_bits((tre)->ptr)
#define MHI_TRE_GET_EV_VEID(tre) (((tre)->dword[0] >> 16) & 0xff)
#define MHI_TRE_GET_EV_LINKSPEED(tre) (((tre)->dword[1] >> 24) & 0xff)
#define MHI_TRE_GET_EV_LINKWIDTH(tre) ((tre)->dword[0] & 0xff)

/* State change event */
#define MHI_SC_EV_PTR 0
#define MHI_SC_EV_DWORD0(state) (state << 24)
#define MHI_SC_EV_DWORD1(type) (type << 16)

/* EE event */
#define MHI_EE_EV_PTR 0
#define MHI_EE_EV_DWORD0(ee) (ee << 24)
#define MHI_EE_EV_DWORD1(type) (type << 16)

/* Command Completion event */
#define MHI_CC_EV_PTR(ptr) (ptr)
#define MHI_CC_EV_DWORD0(code) (code << 24)
#define MHI_CC_EV_DWORD1(type) (type << 16)

/* Transfer descriptor macros */
#define MHI_TRE_DATA_PTR(ptr) (ptr)
#define MHI_TRE_DATA_DWORD0(len) (len & MHI_MAX_MTU)
#define MHI_TRE_DATA_DWORD1(bei, ieot, ieob, chain) ((2 << 16) | (bei << 10) \
	| (ieot << 9) | (ieob << 8) | chain)

/* RSC transfer descriptor macros */
#define MHI_RSCTRE_DATA_PTR(ptr, len) (((u64)len << 48) | ptr)
#define MHI_RSCTRE_DATA_DWORD0(cookie) (cookie)
#define MHI_RSCTRE_DATA_DWORD1 (MHI_PKT_TYPE_COALESCING << 16)

enum mhi_pkt_type {
	MHI_PKT_TYPE_INVALID = 0x0,
	MHI_PKT_TYPE_NOOP_CMD = 0x1,
	MHI_PKT_TYPE_TRANSFER = 0x2,
	MHI_PKT_TYPE_COALESCING = 0x8,
	MHI_PKT_TYPE_RESET_CHAN_CMD = 0x10,
	MHI_PKT_TYPE_STOP_CHAN_CMD = 0x11,
	MHI_PKT_TYPE_START_CHAN_CMD = 0x12,
	MHI_PKT_TYPE_STATE_CHANGE_EVENT = 0x20,
	MHI_PKT_TYPE_CMD_COMPLETION_EVENT = 0x21,
	MHI_PKT_TYPE_TX_EVENT = 0x22,
	MHI_PKT_TYPE_RSC_TX_EVENT = 0x28,
	MHI_PKT_TYPE_EE_EVENT = 0x40,
	MHI_PKT_TYPE_TSYNC_EVENT = 0x48,
	MHI_PKT_TYPE_BW_REQ_EVENT = 0x50,
	MHI_PKT_TYPE_STALE_EVENT, /* internal event */
};

/* MHI transfer completion events */
enum mhi_ev_ccs {
	MHI_EV_CC_INVALID = 0x0,
	MHI_EV_CC_SUCCESS = 0x1,
	MHI_EV_CC_EOT = 0x2, /* End of transfer event */
	MHI_EV_CC_OVERFLOW = 0x3,
	MHI_EV_CC_EOB = 0x4, /* End of block event */
	MHI_EV_CC_OOB = 0x5, /* Out of block event */
	MHI_EV_CC_DB_MODE = 0x6,
	MHI_EV_CC_UNDEFINED_ERR = 0x10,
	MHI_EV_CC_BAD_TRE = 0x11,
};

/* Channel state */
enum mhi_ch_state {
	MHI_CH_STATE_DISABLED,
	MHI_CH_STATE_ENABLED,
	MHI_CH_STATE_RUNNING,
	MHI_CH_STATE_SUSPENDED,
	MHI_CH_STATE_STOP,
	MHI_CH_STATE_ERROR,
};

enum mhi_cmd_type {
	MHI_CMD_NOP = 1,
	MHI_CMD_RESET_CHAN = 16,
	MHI_CMD_STOP_CHAN = 17,
	MHI_CMD_START_CHAN = 18,
};

#define EV_CTX_RESERVED_MASK GENMASK(7, 0)
#define EV_CTX_INTMODC_MASK GENMASK(15, 8)
#define EV_CTX_INTMODC_SHIFT 8
#define EV_CTX_INTMODT_MASK GENMASK(31, 16)
#define EV_CTX_INTMODT_SHIFT 16
struct mhi_event_ctxt {
	__u32 intmod;
	__u32 ertype;
	__u32 msivec;

	__u64 rbase __packed __aligned(4);
	__u64 rlen __packed __aligned(4);
	__u64 rp __packed __aligned(4);
	__u64 wp __packed __aligned(4);
};

#define CHAN_CTX_CHSTATE_MASK GENMASK(7, 0)
#define CHAN_CTX_CHSTATE_SHIFT 0
#define CHAN_CTX_BRSTMODE_MASK GENMASK(9, 8)
#define CHAN_CTX_BRSTMODE_SHIFT 8
#define CHAN_CTX_POLLCFG_MASK GENMASK(15, 10)
#define CHAN_CTX_POLLCFG_SHIFT 10
#define CHAN_CTX_RESERVED_MASK GENMASK(31, 16)
struct mhi_chan_ctxt {
	__u32 chcfg;
	__u32 chtype;
	__u32 erindex;

	__u64 rbase __packed __aligned(4);
	__u64 rlen __packed __aligned(4);
	__u64 rp __packed __aligned(4);
	__u64 wp __packed __aligned(4);
};

struct mhi_cmd_ctxt {
	__u32 reserved0;
	__u32 reserved1;
	__u32 reserved2;

	__u64 rbase __packed __aligned(4);
	__u64 rlen __packed __aligned(4);
	__u64 rp __packed __aligned(4);
	__u64 wp __packed __aligned(4);
};

extern const char * const mhi_state_str[MHI_STATE_MAX];
#define TO_MHI_STATE_STR(state) ((state >= MHI_STATE_MAX || \
				  !mhi_state_str[state]) ? \
				"INVALID_STATE" : mhi_state_str[state])

#endif /* _MHI_COMMON_H */
