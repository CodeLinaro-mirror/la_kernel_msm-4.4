/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */

#ifndef __QCOM_FASTRPC_H__
#define __QCOM_FASTRPC_H__

#include <linux/types.h>

#define FASTRPC_IOCTL_ALLOC_DMA_BUFF	_IOWR('R', 1, struct fastrpc_alloc_dma_buf)
#define FASTRPC_IOCTL_FREE_DMA_BUFF	_IOWR('R', 2, __u32)
#define FASTRPC_IOCTL_INVOKE		_IOWR('R', 3, struct fastrpc_invoke)
#define FASTRPC_IOCTL_INIT_ATTACH	_IO('R', 4)
#define FASTRPC_IOCTL_INIT_CREATE	_IOWR('R', 5, struct fastrpc_init_create)
#define FASTRPC_IOCTL_MMAP		_IOWR('R', 6, struct fastrpc_req_mmap)
#define FASTRPC_IOCTL_MUNMAP		_IOWR('R', 7, struct fastrpc_req_munmap)
#define FASTRPC_IOCTL_INIT_ATTACH_SNS	_IO('R', 8)

enum fastrpc_proc_attr {
	/* Macro for Debug attr */
	FASTRPC_MODE_DEBUG		= (1 << 0),
	/* Macro for Ptrace */
	FASTRPC_MODE_PTRACE		= (1 << 1),
	/* Macro for CRC Check */
	FASTRPC_MODE_CRC		= (1 << 2),
	/* Macro for Unsigned PD */
	FASTRPC_MODE_UNSIGNED_MODULE	= (1 << 3),
	/* Macro for Adaptive QoS */
	FASTRPC_MODE_ADAPTIVE_QOS	= (1 << 4),
	/* Macro for System Process */
	FASTRPC_MODE_SYSTEM_PROCESS	= (1 << 5),
	/* Macro for Prvileged Process */
	FASTRPC_MODE_PRIVILEGED		= (1 << 6),
};

struct fastrpc_invoke_args {
	__u64 ptr;
	__u64 length;
	__s32 fd;
	__u32 reserved;
};

struct fastrpc_invoke {
	__u32 handle;
	__u32 sc;
	__u64 args;
};

struct fastrpc_init_create {
	__u32 filelen;	/* elf file length */
	__s32 filefd;	/* fd for the file */
	__u32 attrs;
	__u32 siglen;
	__u64 file;	/* pointer to elf file */
};

struct fastrpc_alloc_dma_buf {
	__s32 fd;	/* fd */
	__u32 flags;	/* flags to map with */
	__u64 size;	/* size */
};

struct fastrpc_req_mmap {
	__s32 fd;
	__u32 flags;	/* flags for dsp to map with */
	__u64 vaddrin;	/* optional virtual address */
	__u64 size;	/* size */
	__u64 vaddrout;	/* dsp virtual address */
};

struct fastrpc_req_munmap {
	__u64 vaddrout;	/* address to unmap */
	__u64 size;	/* size */
};

#endif /* __QCOM_FASTRPC_H__ */
