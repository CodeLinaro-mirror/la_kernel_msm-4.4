// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2022, Linaro Limited

#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/hashtable.h>
#include <linux/kref.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/qcom_scm.h>
#include <linux/signal.h>
#include <linux/slab.h>
#include <linux/tee_drv.h>
#include <linux/uaccess.h>
#include "../tee_private.h"
#include "qcomtee.h"

#define DRIVER_NAME	"qcomtee"

#define OBJECT_OP_METHOD_MASK     (0x0000FFFFu)
#define OBJECT_OP_METHODID(op)    ((op) & OBJECT_OP_METHOD_MASK)
#define OBJECT_OP_RELEASE       (OBJECT_OP_METHOD_MASK - 0)
#define OBJECT_OP_RETAIN        (OBJECT_OP_METHOD_MASK - 1)
#define OBJECT_OP_MAP_REGION    0
#define OBJECT_OP_YIELD 1

#define OBJECT_COUNTS_MAX_BI   0xF
#define OBJECT_COUNTS_MAX_BO   0xF
#define OBJECT_COUNTS_MAX_OI   0xF
#define OBJECT_COUNTS_MAX_OO   0xF

/* unpack counts */
#define OBJECT_COUNTS_NUM_BI(k)  ((size_t) (((k) >> 0) & OBJECT_COUNTS_MAX_BI))
#define OBJECT_COUNTS_NUM_BO(k)  ((size_t) (((k) >> 4) & OBJECT_COUNTS_MAX_BO))
#define OBJECT_COUNTS_NUM_OI(k)  ((size_t) (((k) >> 8) & OBJECT_COUNTS_MAX_OI))
#define OBJECT_COUNTS_NUM_OO(k)  ((size_t) (((k) >> 12) & OBJECT_COUNTS_MAX_OO))
#define OBJECT_COUNTS_NUM_buffers(k)	\
			(OBJECT_COUNTS_NUM_BI(k) + OBJECT_COUNTS_NUM_BO(k))

#define OBJECT_COUNTS_NUM_objects(k)	\
			(OBJECT_COUNTS_NUM_OI(k) + OBJECT_COUNTS_NUM_OO(k))

#define OBJECT_COUNTS_INDEX_BI(k)   0
#define OBJECT_COUNTS_INDEX_BO(k) (OBJECT_COUNTS_INDEX_BI(k) + OBJECT_COUNTS_NUM_BI(k))
#define OBJECT_COUNTS_INDEX_OI(k) (OBJECT_COUNTS_INDEX_BO(k) + OBJECT_COUNTS_NUM_BO(k))
#define OBJECT_COUNTS_INDEX_OO(k) (OBJECT_COUNTS_INDEX_OI(k) + OBJECT_COUNTS_NUM_OI(k))
#define OBJECT_COUNTS_TOTAL(k)	(OBJECT_COUNTS_INDEX_OO(k) + OBJECT_COUNTS_NUM_OO(k))

#define OBJECT_COUNTS_PACK(in_bufs, out_bufs, in_objs, out_objs) \
	((uint32_t) ((in_bufs) | ((out_bufs) << 4) | \
			((in_objs) << 8) | ((out_objs) << 12)))

#define FOR_ARGS(ndxvar, counts, section) \
	for (ndxvar = OBJECT_COUNTS_INDEX_##section(counts); \
		ndxvar < (OBJECT_COUNTS_INDEX_##section(counts) \
		+ OBJECT_COUNTS_NUM_##section(counts)); \
		++ndxvar)

#define TZCB_BUF_OFFSET(tzcb_req) (sizeof(tzcb_req->result) + \
			sizeof(struct smcinvoke_msg_hdr) + \
			sizeof(union smcinvoke_tz_args) * \
				OBJECT_COUNTS_TOTAL(tzcb_req->hdr.counts))
/* Generic error codes */
#define OBJECT_OK                  0   /* non-specific success code */
#define OBJECT_ERROR               1   /* non-specific error */
#define OBJECT_ERROR_INVALID       2   /* unsupported/unrecognized request */
#define OBJECT_ERROR_SIZE_IN       3   /* supplied buffer/string too large */
#define OBJECT_ERROR_SIZE_OUT      4   /* supplied output buffer too small */
#define OBJECT_ERROR_USERBASE     10   /* start of user-defined error range */
/* Transport layer error codes */
#define OBJECT_ERROR_DEFUNCT     -90   /* object no longer exists */
#define OBJECT_ERROR_ABORT       -91   /* calling thread must exit */
#define OBJECT_ERROR_BADOBJ      -92   /* invalid object context */
#define OBJECT_ERROR_NOSLOTS     -93   /* caller's object table full */
#define OBJECT_ERROR_MAXARGS     -94   /* too many args */
#define OBJECT_ERROR_MAXDATA     -95   /* buffers too large */
#define OBJECT_ERROR_UNAVAIL     -96   /* the request could not be processed */
#define OBJECT_ERROR_KMEM        -97   /* kernel out of memory */
#define OBJECT_ERROR_REMOTE      -98   /* local method sent to remote object */
#define OBJECT_ERROR_BUSY        -99   /* Object is busy */

/* Context type */
#define SMCINVOKE_OBJ_TYPE_TZ_OBJ       0
#define SMCINVOKE_OBJ_TYPE_SERVER       1
/* tzhandle */
#define SMCINVOKE_TZ_OBJ_NULL           0
#define SMCINVOKE_TZ_ROOT_OBJ           1

#define SMCINVOKE_USERSPACE_OBJ_NULL	-1
#define UHANDLE_NULL (SMCINVOKE_USERSPACE_OBJ_NULL)

/*
 * +ve tzhandle : remote object i.e. owned by TZ
 * -ve tzhandle : local object i.e. owned by linux
 * --------------------------------------------------
 *| 1 (1 bit) | Obj Id (15 bits) | srvr id (16 bits) |
 * ---------------------------------------------------
 * Server ids are defined below for various local objects
 * server id 0 : Kernel Obj
 * server id 1 : Memory region Obj
 * server id 2 : Memory map Obj
 * server id 3-15: Reserverd
 * server id 16 & up: Callback Objs
 */
#define KRNL_SRVR_ID			0
#define MEM_RGN_SRVR_ID			1
#define MEM_MAP_SRVR_ID			2
#define CBOBJ_SERVER_ID_START		(16)
#define CBOBJ_SERVER_ID_END		(31)
/* local obj id is represented by 15 bits */
#define MAX_LOCAL_OBJ_ID ((1<<15) - 1)
/* CBOBJs will be served by server id 0x10 onwards */
#define TZHANDLE_GET_SERVER(h) ((uint16_t)((h) & 0xFFFF))
#define TZHANDLE_GET_OBJID(h) (((h) >> 16) & 0x7FFF)
#define TZHANDLE_MAKE_LOCAL(s, o) (((0x8000 | (o)) << 16) | s)
#define TZHANDLE_IS_NULL(h) ((h) == SMCINVOKE_TZ_OBJ_NULL)
#define TZHANDLE_IS_LOCAL(h) ((h) & 0x80000000)
#define TZHANDLE_IS_REMOTE(h) (!TZHANDLE_IS_NULL(h) && !TZHANDLE_IS_LOCAL(h))

#define TZHANDLE_IS_KERNEL_OBJ(h) (TZHANDLE_IS_LOCAL(h) && \
				TZHANDLE_GET_SERVER(h) == KRNL_SRVR_ID)
#define TZHANDLE_IS_MEM_RGN_OBJ(h) (TZHANDLE_IS_LOCAL(h) && \
				TZHANDLE_GET_SERVER(h) == MEM_RGN_SRVR_ID)
#define TZHANDLE_IS_MEM_MAP_OBJ(h) (TZHANDLE_IS_LOCAL(h) && \
				TZHANDLE_GET_SERVER(h) == MEM_MAP_SRVR_ID)
#define TZHANDLE_IS_MEM_OBJ(h) (TZHANDLE_IS_MEM_RGN_OBJ(h) || \
				TZHANDLE_IS_MEM_MAP_OBJ(h))
#define TZHANDLE_IS_CB_OBJ(h) (TZHANDLE_IS_LOCAL(h) && \
				TZHANDLE_GET_SERVER(h) >= CBOBJ_SERVER_ID_START)

#define FILE_IS_REMOTE_OBJ(f) ((f)->f_op && (f)->f_op == &g_smcinvoke_fops)

#define SMCINVOKE_TZ_MIN_BUF_SIZE       4096
#define SMCINVOKE_MAX_CB_BUF_SIZE	SMCINVOKE_TZ_MIN_BUF_SIZE
#define SMCINVOKE_INVOKE_CMD_LEGACY     0x32000600
#define SMCINVOKE_INVOKE_CMD            0x32000602
#define SMCINVOKE_CB_RSP_CMD            0x32000601

#define TZHANDLE_GET_SERVER(h) ((uint16_t)((h) & 0xFFFF))
#define TZHANDLE_GET_OBJID(h) (((h) >> 16) & 0x7FFF)
#define TZHANDLE_MAKE_LOCAL(s, o) (((0x8000 | (o)) << 16) | s)

#define SMCINVOKE_ARGS_ALIGN_SIZE       (sizeof(uint64_t))

#define IS_IN_BUF_PARAM(attr) ((attr & TEE_IOCTL_PARAM_ATTR_TYPE_MASK) \
				== TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_INPUT)
#define IS_OUT_BUF_PARAM(attr) ((attr & TEE_IOCTL_PARAM_ATTR_TYPE_MASK) \
				== TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_OUTPUT)
#define IS_IN_OBJ_PARAM(attr) ((attr & TEE_IOCTL_PARAM_ATTR_TYPE_MASK) \
				== TEE_IOCTL_PARAM_ATTR_TYPE_OBJECT_INPUT)
#define IS_OUT_OBJ_PARAM(attr) ((attr & TEE_IOCTL_PARAM_ATTR_TYPE_MASK) \
				== TEE_IOCTL_PARAM_ATTR_TYPE_OBJECT_OUTPUT)
#define IS_IN_MEMOBJ_PARAM(attr) ((attr & TEE_IOCTL_PARAM_ATTR_TYPE_MASK) \
				== TEE_IOCTL_PARAM_ATTR_TYPE_MEMREF_INPUT)
#define IS_INOUT_MEMOBJ_PARAM(attr) ((attr & TEE_IOCTL_PARAM_ATTR_TYPE_MASK) \
				== TEE_IOCTL_PARAM_ATTR_TYPE_MEMREF_INOUT)
#define IS_OUT_MEMOBJ_PARAM(attr) ((attr & TEE_IOCTL_PARAM_ATTR_TYPE_MASK) \
				== TEE_IOCTL_PARAM_ATTR_TYPE_MEMREF_OUTPUT)

#define SMCINVOKE_NEXT_AVAILABLE_TXN    0
#define SMCINVOKE_REQ_PLACED            1
#define SMCINVOKE_REQ_PROCESSING        2
#define SMCINVOKE_REQ_PROCESSED         3

#define SMCINVOKE_SERVER_STATE_DEFUNCT		1
#define SMCINVOKE_MEM_MAP_OBJ			0
#define SMCINVOKE_MEM_RGN_OBJ			1
#define SMCINVOKE_MEM_PERM_RW			6
#define CBOBJ_MAX_RETRIES			5
#define QCOMTEE_PARAM_ATTR (TEE_IOCTL_PARAM_ATTR_META | TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_INOUT)

enum smcinvoke_cmd_status {
	SMCINVOKE_RESULT_SUCCESS = 0,
	SMCINVOKE_RESULT_INCOMPLETE,
	SMCINVOKE_RESULT_BLOCKED_ON_LISTENER,
	SMCINVOKE_RESULT_CBACK_REQUEST,
	SMCINVOKE_RESULT_FAILURE  = 0xFFFFFFFF
};

/* TZ headers */
struct smcinvoke_buf_hdr {
	uint32_t offset;
	uint32_t size;
};

union smcinvoke_tz_args {
	struct smcinvoke_buf_hdr b;
	int32_t handle;
};
#define SMCINVOKE_TZ_ARGS_SZ	(sizeof(union smcinvoke_tz_args))

struct smcinvoke_msg_hdr {
	uint32_t tzhandle;
	uint32_t op;
	uint32_t counts;
};
#define SMCINVOKE_MSG_HDR_SZ	(sizeof(struct smcinvoke_msg_hdr))

/* outgoing packet */
struct smcinvoke_invoke_request {
	struct smcinvoke_msg_hdr hdr;
	union smcinvoke_tz_args args[];
};

/* Inbound reqs from TZ */
struct smcinvoke_tzcb_req {
	int32_t result;
	struct smcinvoke_msg_hdr hdr;
	union smcinvoke_tz_args args[0];
};

struct smcinvoke_piggyback_msg {
	uint32_t version;
	uint32_t op;
	uint32_t counts;
	int32_t objs[0];
};

/* Data structure to hold request coming from TZ */
struct qcomtee_cb_txn {
	uint32_t txn_id;
	int32_t state;
	struct qcomtee *qtee;
	struct smcinvoke_tzcb_req *cb_req;
	size_t cb_req_bytes;
	struct file **filp_to_release;
	struct hlist_node hash;
	struct kref ref_cnt;
};

struct qcomtee_supp_info {
	uint16_t server_id;
	uint16_t state;
	uint32_t txn_id;
	struct kref ref_cnt;
	struct qcomtee *qtee;

	wait_queue_head_t req_wait_q;
	wait_queue_head_t rsp_wait_q;

	DECLARE_HASHTABLE(reqs_table, 4);
	DECLARE_HASHTABLE(responses_table, 4);

	struct hlist_node hash;
	struct list_head pending_cbobjs;

	size_t cb_buf_size;
};

struct qcomtee_remote_object {
	int id;
	struct list_head list;
	struct kref ref_cnt;
	struct tee_context *ctx;
};

struct qcomtee_context_data {
	uint32_t context_type;
	union {
		uint32_t tzhandle;
		uint16_t server_id;
	};
	struct qcomtee *qtee;
	struct qcomtee_supp_info *supp_info;
	struct list_head remote_obj_list;
};

struct qcomtee_cbobj {
	uint16_t id;
	struct kref ref_cnt;
	struct qcomtee_supp_info *server;
	struct list_head list;
};

/*
 * We require couple of objects, one for mem region & another
 * for mapped mem_obj once mem region has been mapped. It is
 * possible that TZ can release either independent of other.
 */
struct qcomtee_mem_obj {
	struct qcomtee_shm_bridge *bridge;
	struct tee_shm *shm;
	struct qcomtee *qtee;
	struct kref ref_cnt;
	/* these ids are objid part of tzhandle */
	uint16_t id;
	uint16_t mmap_obj_id;
};

struct qcomtee_mem_map_obj {
	struct qcomtee_mem_obj *memobj;
	struct kref ref_cnt;
	uint16_t id;
};

static void qcomtee_free_mem_obj(struct kref *kref)
{
	struct qcomtee_mem_obj *memobj;

	memobj = container_of(kref, struct qcomtee_mem_obj, ref_cnt);
	qcomtee_put_shm_bridge(memobj->bridge);
	kfree(memobj);
}

static void qcomtee_put_mem_obj(struct qcomtee *qtee, uint16_t id)
{
	struct qcomtee_mem_obj *memobj;

	memobj = idr_find(&qtee->mem_region_idr, id);

	if (memobj)
		kref_put(&memobj->ref_cnt, qcomtee_free_mem_obj);
}

static struct qcomtee_mem_obj *qcomtee_get_mem_obj(struct qcomtee *qtee,
						   uint16_t id, bool alloc)
{
	struct qcomtee_mem_obj *memobj;
	int ret;

	if (!alloc) {
		memobj = idr_find(&qtee->mem_region_idr, id);
		if (memobj) {
			kref_get(&memobj->ref_cnt);
			return memobj;
		}
	}

	memobj = kzalloc(sizeof(*memobj), GFP_KERNEL);
	if (!memobj)
		return ERR_PTR(-ENOMEM);

	kref_init(&memobj->ref_cnt);
	ret = idr_alloc_cyclic(&qtee->mem_region_idr, memobj, 1, MAX_LOCAL_OBJ_ID, GFP_KERNEL);
	if (ret < 0) {
		dev_err(qtee->dev, "Error, Unable to allocated memobj idr (%d)\n", ret);
		return ERR_PTR(ret);
	}
	memobj->id = ret;
	memobj->qtee = qtee;
	memobj->mmap_obj_id = 0;

	return memobj;
}

static void qcomtee_free_mmap_obj(struct kref *kref)
{
	struct qcomtee_mem_map_obj *mmapobj;
	struct qcomtee_mem_obj *memobj;

	mmapobj = container_of(kref, struct qcomtee_mem_map_obj, ref_cnt);
	memobj = mmapobj->memobj;
	kref_put(&memobj->ref_cnt, qcomtee_free_mem_obj);

	kfree(mmapobj);
}

static void qcomtee_put_mmap_obj(struct qcomtee *qtee, uint16_t id)
{
	struct qcomtee_mem_map_obj *obj;

	obj = idr_find(&qtee->mem_map_idr, id);
	if (obj)
		kref_put(&obj->ref_cnt, qcomtee_free_mmap_obj);
}

static struct qcomtee_mem_map_obj *qcomtee_alloc_mmap_obj(struct tee_context *ctx,
							  struct qcomtee_mem_obj *memobj)
{
	struct qcomtee *qtee = tee_get_drvdata(ctx->teedev);
	struct qcomtee_mem_map_obj *obj;
	int ret;

	obj = kzalloc(sizeof(*obj), GFP_KERNEL);
	if (!obj)
		return ERR_PTR(-ENOMEM);

	kref_init(&obj->ref_cnt);
	ret = idr_alloc_cyclic(&qtee->mem_map_idr, obj, 1, MAX_LOCAL_OBJ_ID, GFP_KERNEL);
	if (ret < 0) {
		dev_err(qtee->dev, "Error, Unable to allocated obj idr (%d)\n", ret);
		return ERR_PTR(ret);
	}
	obj->id = ret;
	obj->memobj = memobj;
	memobj->mmap_obj_id = ret;

	return obj;
}

static struct qcomtee_mem_map_obj *qcomtee_get_mmap_obj(struct qcomtee *qtee, uint16_t id)
{
	struct qcomtee_mem_map_obj *obj;

	obj = idr_find(&qtee->mem_map_idr, id);
	if (obj)
		kref_get(&obj->ref_cnt);

	return obj;
}

static void qcomtee_get_version(struct tee_device *teedev,
				struct tee_ioctl_version_data *vers)
{
	vers->impl_id = TEE_IMPL_ID_QCOMTEE;
	/* FIXME define caps specific to implementaion */
	vers->impl_caps = 0;
	vers->gen_caps = TEE_GEN_CAP_REG_MEM;
}

static struct qcomtee_supp_info *qcomtee_alloc_server(struct qcomtee *qtee)
{
	struct qcomtee_supp_info *supp_info;
	int ret;

	supp_info = kzalloc(sizeof(*supp_info), GFP_KERNEL);
	if (!supp_info)
		return ERR_PTR(-ENOMEM);

	kref_init(&supp_info->ref_cnt);

	init_waitqueue_head(&supp_info->req_wait_q);
	init_waitqueue_head(&supp_info->rsp_wait_q);

	supp_info->cb_buf_size = SMCINVOKE_TZ_MIN_BUF_SIZE;

	hash_init(supp_info->reqs_table);
	hash_init(supp_info->responses_table);

	INIT_LIST_HEAD(&supp_info->pending_cbobjs);

	mutex_lock(&qtee->lock);
	/* Should we use Cyclic ?? */
	ret = idr_alloc(&qtee->server_idr, supp_info,	CBOBJ_SERVER_ID_START,
			CBOBJ_SERVER_ID_END, GFP_KERNEL);
	if (ret < 0) {
		kfree(supp_info);
		return ERR_PTR(ret);
	}
	supp_info->server_id = ret;
	supp_info->qtee = qtee;
	mutex_unlock(&qtee->lock);

	return supp_info;
}

static int qcomtee_supp_open(struct tee_context *ctx)
{
	struct qcomtee *qtee = tee_get_drvdata(ctx->teedev);
	struct qcomtee_supp_info *supp_info;
	struct qcomtee_context_data *ctxdata;

	supp_info = qcomtee_alloc_server(qtee);
	if (IS_ERR(supp_info))
		return PTR_ERR(supp_info);

	ctxdata = kzalloc(sizeof(*ctxdata), GFP_KERNEL);
	if (!ctxdata)
		return -ENOMEM;

	ctxdata->context_type = SMCINVOKE_OBJ_TYPE_SERVER;
	ctxdata->server_id = supp_info->server_id;
	ctxdata->supp_info = supp_info;
	ctxdata->qtee = qtee;
	ctx->data = ctxdata;

	return 0;
}

static int qcomtee_open(struct tee_context *ctx)
{
	struct qcomtee *qtee = tee_get_drvdata(ctx->teedev);
	struct qcomtee_context_data *ctxdata;

	ctxdata = kzalloc(sizeof(*ctxdata), GFP_KERNEL);
	if (!ctxdata)
		return -ENOMEM;

	ctxdata->tzhandle = SMCINVOKE_TZ_ROOT_OBJ;
	ctxdata->context_type = SMCINVOKE_OBJ_TYPE_TZ_OBJ;
	ctxdata->qtee = qtee;
	ctx->data = ctxdata;
	INIT_LIST_HEAD(&ctxdata->remote_obj_list);

	return 0;
}

static bool qcomtee_is_inbound_req(int val)
{
	return (val == SMCINVOKE_RESULT_CBACK_REQUEST ||
		val == SMCINVOKE_RESULT_INCOMPLETE ||
		val == SMCINVOKE_RESULT_BLOCKED_ON_LISTENER);
}

static struct qcomtee_remote_object *qcomtee_get_remote_object(struct tee_context *ctx,
							       int obj_id)
{
	struct qcomtee_context_data *ctxdata = ctx->data;
	struct qcomtee_remote_object *obj;
	struct list_head *head;

	head = &ctxdata->remote_obj_list;
	list_for_each_entry(obj, head, list) {
		if (obj->id == obj_id)  {
			kref_get(&obj->ref_cnt);
			return obj;
		}
	}

	obj = kzalloc(sizeof(*obj), GFP_KERNEL);
	if (!obj)
		return ERR_PTR(-ENOMEM);

	obj->id = obj_id;
	obj->ctx = ctx;
	kref_init(&obj->ref_cnt);

	list_add_tail(&obj->list, head);
	return obj;
}

static struct qcomtee_supp_info *qcomtee_get_cb_server(struct qcomtee *qtee, uint16_t server_id)
{
	struct qcomtee_supp_info *server;

	server = idr_find(&qtee->server_idr, server_id);
	if (server)
		kref_get(&server->ref_cnt);

	return server;
}

static void qcomtee_destroy_cb_server(struct kref *kref)
{
	struct qcomtee_supp_info *server;

	server = container_of(kref, struct qcomtee_supp_info, ref_cnt);

	idr_remove(&server->qtee->server_idr, server->server_id);

	hash_del(&server->hash);
	kfree(server);
}

static void qcomtee_put_cb_server(struct qcomtee_supp_info *server)
{
	if (server)
		kref_put(&server->ref_cnt, qcomtee_destroy_cb_server);
}

static void qcomtee_free_pending_cbobj(struct kref *kref)
{
	struct qcomtee_supp_info *server;
	struct qcomtee_cbobj *obj;

	obj = container_of(kref, struct qcomtee_cbobj, ref_cnt);
	list_del(&obj->list);
	server = obj->server;
	kfree(obj);

	qcomtee_put_cb_server(server);
}

static int qcomtee_put_pending_cbobj(struct qcomtee *qtee, uint16_t srvr_id, int16_t obj_id)
{
	struct qcomtee_supp_info *srvr_info = qcomtee_get_cb_server(qtee, srvr_id);
	struct qcomtee_cbobj *cbobj;
	struct list_head *head;
	int ret = -EINVAL;

	if (!srvr_info)
		return ret;

	head = &srvr_info->pending_cbobjs;
	list_for_each_entry(cbobj, head, list) {
		if (cbobj->id == obj_id)  {
			kref_put(&cbobj->ref_cnt, qcomtee_free_pending_cbobj);
			ret = 0;
			break;
		}
	}

	qcomtee_put_cb_server(srvr_info);
	return ret;
}

static int qcomtee_release_tzhandle(struct qcomtee *qtee, int32_t tzhandle)
{
	if (TZHANDLE_IS_MEM_RGN_OBJ(tzhandle)) {
		qcomtee_put_mem_obj(qtee, TZHANDLE_GET_OBJID(tzhandle));
		return 0;
	} else if (TZHANDLE_IS_MEM_MAP_OBJ(tzhandle)) {
		qcomtee_put_mmap_obj(qtee, TZHANDLE_GET_OBJID(tzhandle));
		return 0;
	} else if (TZHANDLE_IS_CB_OBJ(tzhandle)) {
		return qcomtee_put_pending_cbobj(qtee, TZHANDLE_GET_SERVER(tzhandle),
						TZHANDLE_GET_OBJID(tzhandle));
	}
	return OBJECT_ERROR;
}

static void qcomtee_delete_cb_txn(struct kref *kref)
{
	struct qcomtee_cb_txn *cb_txn = container_of(kref,
						     struct qcomtee_cb_txn, ref_cnt);

	if (OBJECT_OP_METHODID(cb_txn->cb_req->hdr.op) == OBJECT_OP_RELEASE)
		qcomtee_release_tzhandle(cb_txn->qtee, cb_txn->cb_req->hdr.tzhandle);

	kfree(cb_txn->cb_req);
	hash_del(&cb_txn->hash);
	kfree(cb_txn);
}

static int32_t qcomtee_map_mem_region(struct tee_context *ctx, void *buf, size_t buf_len)
{
	struct qcomtee *qtee = tee_get_drvdata(ctx->teedev);
	struct qcomtee_mem_map_obj *mem_map_obj;
	struct smcinvoke_tzcb_req *msg = buf;
	struct qcomtee_mem_obj *memobj;
	uint16_t memobj_id;
	int ret = OBJECT_OK;
	int32_t *oo;
	struct {
		uint64_t paddr;
		uint64_t len;
		uint32_t perms;
	} *ob;

	if (msg->hdr.counts != OBJECT_COUNTS_PACK(0, 1, 1, 1) ||
	    (buf_len - msg->args[0].b.offset < msg->args[0].b.size)) {
		return OBJECT_ERROR_INVALID;
	}
	/* args[0] = BO, args[1] = OI, args[2] = OO */
	ob = buf + msg->args[0].b.offset;
	oo = &msg->args[2].handle;
	memobj_id = TZHANDLE_GET_OBJID(msg->args[1].handle);

	mutex_lock(&qtee->lock);
	memobj = qcomtee_get_mem_obj(qtee, memobj_id, false);
	if (IS_ERR(memobj)) {
		mutex_unlock(&qtee->lock);
		dev_err(qtee->dev, "Memory object not found\n");
		return OBJECT_ERROR_BADOBJ;
	}

	if (!memobj->mmap_obj_id) {
		struct qcomtee_shm_bridge *bridge;

		mem_map_obj = qcomtee_alloc_mmap_obj(ctx, memobj);
		bridge = qcomtee_get_shm_bridge(ctx, memobj->shm->paddr,
						 memobj->shm->size);
		if (!bridge) {
			/*
			 * Bridge should already have been allocated something
			 * is not right
			 */
			qcomtee_put_mem_obj(qtee, memobj->id);
			ret = OBJECT_ERROR_INVALID;
			goto out;
		}
		memobj->bridge = bridge;
	} else {
		qcomtee_get_mmap_obj(qtee, memobj->mmap_obj_id);
	}
	ob->paddr = memobj->shm->paddr;
	ob->len = memobj->shm->size;
	ob->perms = SMCINVOKE_MEM_PERM_RW;
	*oo = TZHANDLE_MAKE_LOCAL(MEM_MAP_SRVR_ID, memobj->mmap_obj_id);
out:
	mutex_unlock(&qtee->lock);
	return ret;
}

static void qcomtee_process_kernel_obj(struct tee_context *ctx, void *buf, size_t buf_len)
{
	struct smcinvoke_tzcb_req *cb_req = buf;

	switch (cb_req->hdr.op) {
	case OBJECT_OP_MAP_REGION:
		cb_req->result = qcomtee_map_mem_region(ctx, buf, buf_len);
		break;
	case OBJECT_OP_YIELD:
		cb_req->result = OBJECT_OK;
		break;
	default:
		dev_err(&ctx->teedev->dev, "invalid operation for tz kernel object\n");
		cb_req->result = OBJECT_ERROR_INVALID;
		break;
	}
}

static int32_t qcomtee_release_mem_obj_locked(struct tee_context *ctx, void *buf, size_t buf_len)
{
	struct qcomtee *qtee = tee_get_drvdata(ctx->teedev);
	struct smcinvoke_tzcb_req *msg = buf;

	if (msg->hdr.counts != OBJECT_COUNTS_PACK(0, 0, 0, 0)) {
		dev_err(qtee->dev, "Invalid object count in %s\n", __func__);
		return OBJECT_ERROR_INVALID;
	}

	return qcomtee_release_tzhandle(qtee, msg->hdr.tzhandle);
}

static void qcomtee_process_mem_obj(struct tee_context *ctx, void *buf, size_t buf_len)
{
	struct qcomtee *qtee = tee_get_drvdata(ctx->teedev);
	struct smcinvoke_tzcb_req *cb_req = buf;

	mutex_lock(&qtee->lock);
	cb_req->result = (cb_req->hdr.op == OBJECT_OP_RELEASE) ?
			qcomtee_release_mem_obj_locked(ctx, buf, buf_len) :
			OBJECT_ERROR_INVALID;
	mutex_unlock(&qtee->lock);
}

static void qcomtee_process_tzcb_req(struct tee_context *ctx, void *buf, size_t buf_len, struct file **arr_filp)
{
	struct qcomtee *qtee = tee_get_drvdata(ctx->teedev);
	struct smcinvoke_tzcb_req *cb_req, *tmp_cb_req;
	int ret = OBJECT_ERROR_DEFUNCT, cbobj_retries = 0;
	struct qcomtee_supp_info *srvr_info;
	struct qcomtee_cb_txn *cb_txn;
	long timeout_jiff;

	if (buf_len < sizeof(struct smcinvoke_tzcb_req)) {
		dev_err(qtee->dev, "smaller buffer length : %ld\n", buf_len);
		return;
	}

	cb_req = buf;

	/* check whether it is to be served by kernel or userspace */
	if (TZHANDLE_IS_KERNEL_OBJ(cb_req->hdr.tzhandle)) {
		qcomtee_process_kernel_obj(ctx, buf, buf_len);
		return;
	} else if (TZHANDLE_IS_MEM_OBJ(cb_req->hdr.tzhandle)) {
		qcomtee_process_mem_obj(ctx, buf, buf_len);
		return;
	} else if (!TZHANDLE_IS_CB_OBJ(cb_req->hdr.tzhandle)) {
		cb_req->result = OBJECT_ERROR_INVALID;
		return;
	}

	/*
	 * We need a copy of req that could be sent to server. Otherwise, if
	 * someone kills invoke caller, buf would go away and server would be
	 * working on already freed buffer, causing a device crash.
	 */
	tmp_cb_req = kmemdup(buf, buf_len, GFP_KERNEL);
	if (!tmp_cb_req) {
		/* we need to return error to caller so fill up result */
		cb_req->result = OBJECT_ERROR_KMEM;
		return;
	}

	cb_txn = kzalloc(sizeof(*cb_txn), GFP_KERNEL);
	if (!cb_txn) {
		cb_req->result = OBJECT_ERROR_KMEM;
		kfree(tmp_cb_req);
		return;
	}
	/* no need for memcpy as we did kmemdup() above */
	cb_req  = tmp_cb_req;

	cb_txn->qtee = qtee;
	cb_txn->state = SMCINVOKE_REQ_PLACED;
	cb_txn->cb_req = cb_req;
	cb_txn->cb_req_bytes = buf_len;
	cb_txn->filp_to_release = arr_filp;
	kref_init(&cb_txn->ref_cnt);

	mutex_lock(&qtee->lock);

	/*
	 * callback fd are of type TEE_IOCTL_PARAM_ATTR_TYPE_QTEE_OBJECT_INPUT
	 * so making them and using them as server ids in tzhandle should just
	 * work
	 We will also maintain the servers like this one for now.
	 */
	srvr_info = qcomtee_get_cb_server(qtee, TZHANDLE_GET_SERVER(cb_req->hdr.tzhandle));
	if (!srvr_info || srvr_info->state == SMCINVOKE_SERVER_STATE_DEFUNCT) {
		/* ret equals Object_ERROR_DEFUNCT, at this point go to out */
		if (!srvr_info)
			dev_err(qtee->dev, "server is invalid or already released\n");
		else
			dev_err(qtee->dev, "server is defunct, state= %d tzhandle = %d\n",
				srvr_info->state, cb_req->hdr.tzhandle);
		mutex_unlock(&qtee->lock);
		goto out;
	}

	cb_txn->txn_id = ++srvr_info->txn_id;
	hash_add(srvr_info->reqs_table, &cb_txn->hash, cb_txn->txn_id);
	mutex_unlock(&qtee->lock);
	/*
	 * we need not worry that supp_info will be deleted because as long
	 * as this CBObj is served by this server, srvr_info will be valid.
	 */
	wake_up_interruptible_all(&srvr_info->req_wait_q);
	/* timeout before 1s otherwise tzbusy would come */
	timeout_jiff = msecs_to_jiffies(1000);

	while (cbobj_retries++ < CBOBJ_MAX_RETRIES) {
		ret = wait_event_interruptible_timeout(srvr_info->rsp_wait_q,
			(cb_txn->state == SMCINVOKE_REQ_PROCESSED) ||
			(srvr_info->state == SMCINVOKE_SERVER_STATE_DEFUNCT),
			timeout_jiff);

		if (ret == 0)
			dev_err(qtee->dev, "Timeout...\n");
		else
			break;
	}

out:
	/*
	 * we could be here because of either: a. Req is PROCESSED
	 * b. Server was killed                c. Invoke thread is killed
	 * sometime invoke thread and server are part of same process.
	 */
	mutex_lock(&qtee->lock);
	hash_del(&cb_txn->hash);
	if (ret == 0) {
		dev_err(qtee->dev, "CBObj timed out! No more retries\n");
		cb_req->result = OBJECT_ERROR_ABORT;
	} else if (ret == -ERESTARTSYS) {
		dev_err(qtee->dev, "wait event interruped, ret: %d\n", ret);
		cb_req->result = OBJECT_ERROR_ABORT;
	} else {
		if (cb_txn->state == SMCINVOKE_REQ_PROCESSED) {
			/*
			 * it is possible that server was killed immediately
			 * after CB Req was processed but who cares now!
			 */
		} else if (!srvr_info ||
			srvr_info->state == SMCINVOKE_SERVER_STATE_DEFUNCT) {
			cb_req->result = OBJECT_ERROR_DEFUNCT;
			dev_err(qtee->dev, "server invalid/released, res: %d\n", cb_req->result);
		} else {
			dev_err(qtee->dev, "%s: unexpected event happened, ret:%d\n", __func__, ret);
			cb_req->result = OBJECT_ERROR_ABORT;
		}
	}
	memcpy(buf, cb_req, buf_len);
	kref_put(&cb_txn->ref_cnt, qcomtee_delete_cb_txn);
	qcomtee_put_cb_server(srvr_info);
	mutex_unlock(&qtee->lock);
}

static void qcomtee_process_piggyback_data(struct tee_context *ctx, void *buf, size_t buf_size)
{
	struct smcinvoke_tzcb_req req = {0};
	struct smcinvoke_piggyback_msg *msg = buf;
	int32_t *objs = msg->objs;
	int i;

	for (i = 0; i < msg->counts; i++) {
		req.hdr.op = msg->op;
		req.hdr.counts = 0; /* release op does not require any args */
		req.hdr.tzhandle = objs[i];
		qcomtee_process_tzcb_req(ctx, &req, sizeof(struct smcinvoke_tzcb_req), NULL);
		/* cbobjs_in_flight will be adjusted during CB processing */
	}
}

static void qcomtee_supp_release(struct tee_context *ctx)
{
	struct qcomtee_context_data *ctxdata = ctx->data;
	struct qcomtee_supp_info *supp_info = ctxdata->supp_info;

	qcomtee_put_cb_server(supp_info);
	supp_info->state = SMCINVOKE_SERVER_STATE_DEFUNCT;
	kfree(ctxdata);
	ctx->data = NULL;
}

static int qcomtee_open_session(struct tee_context *ctx,
			 struct tee_ioctl_open_session_arg *arg,
			 struct tee_param *param)
{
	/*
	 * Sessions are handled as object in smcinvoke,
	 * Object returned as part of invoke will be the session id.
	 * session ids can be for static TA and new TA that are loaded using
	 * helper objects/interface
	 */

	return 0;
}

static int qcomtee_close_session(struct tee_context *ctx, u32 session)
{
	return 0;
}

static u32 qcomtee_get_counts_from_tee_param(struct qcomtee *qtee, struct tee_param *param,
					     int num_args)
{
	int i, in_objs = 0, out_objs = 0, in_bufs = 0, out_bufs = 0;

	for (i = 0; i < num_args; i++) {
		switch (param->attr & TEE_IOCTL_PARAM_ATTR_TYPE_MASK) {
		case TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_INPUT:
			in_bufs++;
			break;
		case TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_OUTPUT:
			out_bufs++;
			break;
		case TEE_IOCTL_PARAM_ATTR_TYPE_OBJECT_INPUT:
		case TEE_IOCTL_PARAM_ATTR_TYPE_MEMREF_INPUT:
			in_objs++;
			break;
		case TEE_IOCTL_PARAM_ATTR_TYPE_OBJECT_OUTPUT:
		case TEE_IOCTL_PARAM_ATTR_TYPE_MEMREF_OUTPUT:
			out_objs++;
			break;
		default:
			break;
		}
		param++;
	}

	dev_dbg(qtee->dev, "counts (%d, %d, %d, %d) 0x%08X\n",
		in_bufs, out_bufs, in_objs, out_objs,
		OBJECT_COUNTS_PACK(in_bufs, out_bufs, in_objs, out_objs));
	return OBJECT_COUNTS_PACK(in_bufs, out_bufs, in_objs, out_objs);
}

/*
 * SMC expects arguments in following format
 * ---------------------------------------------------------------------------
 * | cxt | op | counts | ptr|size |ptr|size...|ORef|ORef|...| rest of payload |
 * ---------------------------------------------------------------------------
 * cxt: target, op: operation, counts: total arguments
 * offset: offset is from beginning of buffer i.e. cxt
 * size: size is 8 bytes aligned value
 */
static size_t compute_in_msg_size(struct tee_param *param, int num_args,
				  u32 counts)
{
	size_t total_size;
	int buf_size, i;
	total_size = SMCINVOKE_MSG_HDR_SZ +
			OBJECT_COUNTS_TOTAL(counts) * SMCINVOKE_TZ_ARGS_SZ;

	/* Computed total_size should be 8 bytes aligned from start of buf */
	total_size = ALIGN(total_size, SMCINVOKE_ARGS_ALIGN_SIZE);

	/* each buffer has to be 8 bytes aligned */
	for (i = 0; i < num_args; i++) {
		switch (param->attr & TEE_IOCTL_PARAM_ATTR_TYPE_MASK) {
		case TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_INPUT:
		case TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_OUTPUT:
			buf_size = param->u.value.b;
			total_size = total_size + ALIGN(buf_size, SMCINVOKE_ARGS_ALIGN_SIZE);
			break;
		default:
			break;
		}

		param++;
	}

	return PAGE_ALIGN(total_size);
}

static uint16_t qcomtee_get_server_id(int cb_server_fd)
{
	struct file *tmp_filp = fget(cb_server_fd);
	struct qcomtee_context_data *srv_ctx;
	struct tee_context *ctx;
	uint16_t server_id = 0;

	if (!tmp_filp)
		return server_id;

	ctx = tmp_filp->private_data;
	if (!ctx)
		goto err;

	srv_ctx = ctx->data;
	if (srv_ctx && srv_ctx->context_type ==  SMCINVOKE_OBJ_TYPE_SERVER)
		server_id = srv_ctx->server_id;

err:
	if (tmp_filp)
		fput(tmp_filp);

	return server_id;
}

static int qcomtee_get_pending_cbobj(struct qcomtee *qtee, uint16_t srvr_id, int16_t obj_id)
{
	struct qcomtee_supp_info *server = qcomtee_get_cb_server(qtee, srvr_id);
	struct qcomtee_cbobj *cbobj, *obj;
	bool release_server = true;
	struct list_head *head;
	int ret = 0;

	if (!server) {
		dev_err(qtee->dev, "%s, server id : %u not found\n", __func__, srvr_id);
		return OBJECT_ERROR_BADOBJ;
	}

	head = &server->pending_cbobjs;
	list_for_each_entry(cbobj, head, list)
		if (cbobj->id == obj_id)  {
			kref_get(&cbobj->ref_cnt);
			goto out;
		}

	obj = kzalloc(sizeof(*obj), GFP_KERNEL);
	if (!obj) {
		ret = OBJECT_ERROR_KMEM;
		goto out;
	}

	obj->id = obj_id;
	kref_init(&obj->ref_cnt);
	obj->server = server;
	/*
	 * Only take server kref for the very first time,
	 * new references to obj id will anyway refcounted in cbobj
	 * we are holding server ref in cbobj; we will
	 * release server ref when cbobj is destroyed
	 */
	release_server = false;
	list_add_tail(&obj->list, head);
out:
	if (release_server)
		qcomtee_put_cb_server(server);

	return ret;
}

static int qcomtee_create_mem_obj(struct tee_context *ctx, struct tee_shm *shm, int32_t *mem_obj)
{
	struct qcomtee *qtee = tee_get_drvdata(ctx->teedev);
	struct qcomtee_mem_obj *memobj;

	memobj = qcomtee_get_mem_obj(qtee, 0, true);
	if (IS_ERR(memobj))
		return -ENOMEM;

	memobj->shm = shm;
	*mem_obj = TZHANDLE_MAKE_LOCAL(MEM_RGN_SRVR_ID, memobj->id);

	return 0;
}

static int qcomtee_get_uhandle_from_tzhandle(struct tee_context *ctx, int32_t tzhandle,
					     int32_t srvr_id, int32_t *uhandle, bool lock)
{
	struct qcomtee *qtee = tee_get_drvdata(ctx->teedev);
	int ret = -1;

	if (TZHANDLE_IS_NULL(tzhandle)) {
		*uhandle = UHANDLE_NULL;
		ret = 0;
	} else if (TZHANDLE_IS_CB_OBJ(tzhandle)) {
		if (srvr_id != TZHANDLE_GET_SERVER(tzhandle))
			goto out;

		*uhandle = TZHANDLE_GET_OBJID(tzhandle);

		if (lock)
			mutex_lock(&qtee->lock);

		ret = qcomtee_get_pending_cbobj(qtee, TZHANDLE_GET_SERVER(tzhandle),
					       TZHANDLE_GET_OBJID(tzhandle));
		if (lock)
			mutex_unlock(&qtee->lock);
	} else if (TZHANDLE_IS_MEM_RGN_OBJ(tzhandle)) {
		struct qcomtee_mem_obj *mem_obj =  NULL;

		if (lock)
			mutex_lock(&qtee->lock);

		mem_obj = qcomtee_get_mem_obj(qtee,
					      TZHANDLE_GET_OBJID(tzhandle),
					      false);
		if (mem_obj != NULL) {
			int fd = mem_obj->shm->id;

			if (fd < 0)
				goto exit_lock;
			*uhandle = fd;
			ret = 0;
		}
exit_lock:
		if (lock)
			mutex_unlock(&qtee->lock);
	} else if (TZHANDLE_IS_REMOTE(tzhandle)) {
		/* new remote object instance id received */
		/* if execution comes here => tzhandle is an unsigned int */
		*uhandle = tzhandle;
		ret = 0;
	}
out:
	return ret;
}

/*
 * This function retrieves file pointer corresponding to FD provided. It stores
 * retrieved file pointer until IOCTL call is concluded. Once call is completed,
 * all stored file pointers are released. file pointers are stored to prevent
 * other threads from releasing that FD while IOCTL is in progress.
 */
static int qcomtee_get_tzhandle_from_uhandle(struct tee_context *ctx, struct tee_param *param,
					     int fd, uint32_t *tzhandle)
{
	struct qcomtee *qtee = tee_get_drvdata(ctx->teedev);
	int ret = -EBADF;
	uint16_t server_id = 0;

	if (fd == 0) {
		*tzhandle = SMCINVOKE_TZ_OBJ_NULL;
		ret = 0;
	} else if (IS_IN_OBJ_PARAM(param->attr)) {
		server_id = qcomtee_get_server_id(param->u.obj.fd);
		if (server_id < CBOBJ_SERVER_ID_START)
			goto out;

		mutex_lock(&qtee->lock);
		ret = qcomtee_get_pending_cbobj(qtee, server_id, param->u.obj.id);
		mutex_unlock(&qtee->lock);
		if (ret)
			goto out;

		*tzhandle = TZHANDLE_MAKE_LOCAL(server_id, param->u.obj.id);
		ret = 0;
	} else if (IS_IN_MEMOBJ_PARAM(param->attr)) {
		ret = qcomtee_create_mem_obj(ctx, param->u.memref.shm, tzhandle);
	}
out:
	return ret;
}

static int qcomtee_marshal_out_invoke_req(struct tee_context *ctx, const uint8_t *buf,
					  uint32_t buf_size, int num_params,
					  struct tee_param *params, u32 counts)
{
	struct device *dev = &ctx->teedev->dev;
	struct smcinvoke_invoke_request *req;
	union smcinvoke_tz_args *tz_args;
	int32_t temp_fd = UHANDLE_NULL;
	struct tee_param *param;
	int ret = -EINVAL, i;
	u64 addr;
	size_t req_sz;

	req_sz = struct_size(req, args, OBJECT_COUNTS_TOTAL(counts));
	if (req_sz > buf_size)
		goto out;

	req = (struct smcinvoke_invoke_request *) buf;
	tz_args = &req->args[0];
	tz_args += OBJECT_COUNTS_NUM_BI(counts);

	for (i = 0; i < num_params; i++) {
		param = &params[i];
		if (IS_OUT_BUF_PARAM(param->attr)) {
			addr = param->u.value.a;
			param->u.value.b = tz_args->b.size;
			if ((buf_size - tz_args->b.offset < tz_args->b.size) ||
				tz_args->b.offset > buf_size) {
				dev_err(dev, "%s: buffer overflow detected\n", __func__);
				goto out;
			}

			if (copy_to_user((void __user *)(uintptr_t)(addr),
				(uint8_t *)(buf) + tz_args->b.offset, tz_args->b.size)) {
				dev_err(dev, "Error %d copying ctxt to user\n", ret);
				goto out;
			}
			tz_args++;
		}
	}

	tz_args += OBJECT_COUNTS_NUM_OI(counts);

	for (i = 0; i < num_params; i++) {
		param = &params[i];
		if (IS_OUT_OBJ_PARAM(param->attr)) {
			temp_fd = UHANDLE_NULL;
			ret = qcomtee_get_uhandle_from_tzhandle(ctx, tz_args->handle,
							TZHANDLE_GET_SERVER(tz_args->handle),
							&temp_fd, false);
			if (ret)
				goto out;

			param->u.obj.fd = temp_fd;
			qcomtee_get_remote_object(ctx, temp_fd);
			tz_args++;
		}
	}

	ret = 0;
out:
	return ret;
}

static int qcomtee_prepare_send_scm_msg(struct tee_context *ctx, const uint8_t *in_buf,
					phys_addr_t in_paddr, size_t in_buf_len,
					uint8_t *out_buf, phys_addr_t out_paddr,
					size_t out_buf_len,
					int num_params,
					struct tee_param *params,
					bool *tz_acked,
					struct tee_shm *in_shm, struct tee_shm *out_shm,
					u32 counts, int32_t *result)
{
	struct qcomtee *qtee = tee_get_drvdata(ctx->teedev);
	int ret = 0, cmd;
	u64 response_type;
	unsigned int data;
	struct file *arr_filp[OBJECT_COUNTS_MAX_OO] = {NULL};

	*tz_acked = false;
	/* buf size should be page aligned */
	if ((in_buf_len % PAGE_SIZE) != 0 || (out_buf_len % PAGE_SIZE) != 0)
		return -EINVAL;

	cmd = SMCINVOKE_INVOKE_CMD;
	/*
	 * purpose of lock here is to ensure that any CB obj that may be going
	 * to user as OO is not released by piggyback message on another invoke
	 * request. We should not move this lock to process_invoke_req() because
	 * that will either cause deadlock or prevent any other invoke request
	 * to come in. We release this lock when either
	 *     a) TZ requires HLOS action to complete ongoing invoke operation
	 *     b) Final response to invoke has been marshalled out
	 */
	while (1) {
		mutex_lock(&qtee->lock);
		if (cmd == SMCINVOKE_INVOKE_CMD) {
			ret = qcom_scm_invoke_smc(in_paddr, in_buf_len,
						  out_paddr, out_buf_len,
						  result, &response_type, &data);
		} else /* SMCINVOKE_CB_RSP_CMD */ {
			ret = qcom_scm_invoke_callback_response(out_paddr /*virt_to_phys(out_buf)*/,
								out_buf_len,
								result,
								&response_type, &data);
		}

		if (!ret && !qcomtee_is_inbound_req(response_type)) {
			/* dont marshal if Obj returns an error */
			if (!*result) {
				if (params != NULL)
					ret = qcomtee_marshal_out_invoke_req(ctx, in_buf, in_buf_len,
								     num_params, params, counts);
			}
			*tz_acked = true;
		}
		mutex_unlock(&qtee->lock);

		if (ret || !qcomtee_is_inbound_req(response_type))
			break;

		/* We cannot support qseecom in upstream kernel */
		/* process listener request */
		if (response_type == SMCINVOKE_RESULT_INCOMPLETE ||
		    response_type == SMCINVOKE_RESULT_BLOCKED_ON_LISTENER) {
			if (!*result &&	response_type != SMCINVOKE_RESULT_CBACK_REQUEST) {
				ret = qcomtee_marshal_out_invoke_req(ctx, in_buf,
							     in_buf_len, num_params, params, counts);
			}
			*tz_acked = true;
		}

		/*
		 * qseecom does not understand smcinvoke's callback object &&
		 * erringly sets ret value as -EINVAL :( We need to handle it.
		 */
		if (response_type != SMCINVOKE_RESULT_CBACK_REQUEST)
			break;

		if (response_type == SMCINVOKE_RESULT_CBACK_REQUEST) {
			qcomtee_process_tzcb_req(ctx, out_buf, out_buf_len, arr_filp);
			cmd = SMCINVOKE_CB_RSP_CMD;
		}
	}
	return ret;
}

static void qcomtee_release_remote_object(struct qcomtee_remote_object *obj)
{
	struct tee_context *ctx = obj->ctx;
	struct qcomtee *qtee = tee_get_drvdata(ctx->teedev);
	bool release_handles;
	uint8_t *in_buf, *out_buf;
	struct smcinvoke_msg_hdr hdr;
	uint32_t tzhandle;
	struct tee_shm *in_shm = NULL, *out_shm = NULL;
	int32_t result;
	int ret = 0;

	tzhandle = obj->id;

	if (!tzhandle || tzhandle == SMCINVOKE_TZ_ROOT_OBJ) {
		/* Root object is special in sense it is indestructible */
		dev_err(qtee->dev, "ERROR trying to free root object\n");
		goto out;
	}

	in_shm = tee_shm_alloc_priv_buf(ctx, SMCINVOKE_TZ_MIN_BUF_SIZE);
	if (IS_ERR(in_shm)) {
		ret = PTR_ERR(in_shm);
		dev_err(qtee->dev, "shmbridge alloc failed for in msg in release\n");
		goto out;
	}

	out_shm = tee_shm_alloc_priv_buf(ctx, SMCINVOKE_TZ_MIN_BUF_SIZE);
	if (IS_ERR(out_shm)) {
		ret = PTR_ERR(out_shm);
		dev_err(qtee->dev, "shmbridge alloc failed for out msg in release\n");
		goto out;
	}

	in_buf = in_shm->kaddr;
	out_buf = out_shm->kaddr;
	hdr.tzhandle = tzhandle;
	hdr.op = OBJECT_OP_RELEASE;
	hdr.counts = 0;

	*(struct smcinvoke_msg_hdr *)in_buf = hdr;

	ret = qcomtee_prepare_send_scm_msg(ctx, in_buf, in_shm->paddr,
		SMCINVOKE_TZ_MIN_BUF_SIZE, out_buf, out_shm->paddr,
		SMCINVOKE_TZ_MIN_BUF_SIZE, 0, NULL, &release_handles,
		in_shm, out_shm, hdr.counts, &result);

	qcomtee_process_piggyback_data(ctx, out_buf, SMCINVOKE_TZ_MIN_BUF_SIZE);
out:
	if (in_shm)
		tee_shm_free(in_shm);
	if (out_shm)
		tee_shm_free(out_shm);
	if (ret)
		dev_err(qtee->dev, "Error releasing remote object %d\n",
			tzhandle);
}

static void qcomtee_remove_remote_object(struct kref *kref)
{
	struct qcomtee_remote_object *obj;

	obj = container_of(kref, struct qcomtee_remote_object, ref_cnt);
	qcomtee_release_remote_object(obj);
	kfree(obj);
}

static void qcomtee_put_remote_object(struct tee_context *ctx,
				      struct qcomtee_remote_object *obj)
{
	list_del(&obj->list);
	kref_put(&obj->ref_cnt, qcomtee_remove_remote_object);
}

static void qcomtee_release(struct tee_context *ctx)
{
	struct qcomtee_context_data *ctxdata = ctx->data;
	struct qcomtee_remote_object *obj, *p;
	struct list_head *head;

	head = &ctxdata->remote_obj_list;
	list_for_each_entry_safe(obj, p, head, list)
		qcomtee_put_remote_object(ctx, obj);

	kfree(ctxdata);

	ctx->data = NULL;
}

static int marshal_in_invoke_req(struct tee_context *ctx,
				 const struct tee_ioctl_invoke_arg *req,
				 struct tee_param *params, uint32_t tzhandle,
				 uint8_t *buf, size_t buf_size, int counts)
{
	struct smcinvoke_invoke_request *invoke_req;
	union smcinvoke_tz_args *tz_args;
	struct tee_param *param;
	u64 addr, offset, size;
	int ret = -EINVAL, i;

	offset = struct_size(invoke_req, args, OBJECT_COUNTS_TOTAL(counts));
	if (offset > buf_size)
		goto out;

	invoke_req = (struct smcinvoke_invoke_request *) buf;
	invoke_req->hdr.tzhandle = tzhandle;
	invoke_req->hdr.op = req->func;
	invoke_req->hdr.counts = counts;
	tz_args = &invoke_req->args[0];

	for (i = 0; i < req->num_params; i++) {
		param = &params[i];
		if (IS_IN_BUF_PARAM(param->attr)) {
			addr = param->u.value.a;
			size = param->u.value.b;

			offset = ALIGN(offset, SMCINVOKE_ARGS_ALIGN_SIZE);
			if ((offset > buf_size) || (size > (buf_size - offset)))
				goto out;

			tz_args[i].b.offset = offset;
			tz_args[i].b.size = size;

			if (copy_from_user(buf + offset, (void __user *)(uintptr_t)(addr), size))
				goto out;

			offset += size;
		}
	}

	for (i = 0; i < req->num_params; i++) {
		param = &params[i];
		if (IS_OUT_BUF_PARAM(param->attr)) {
			addr = param->u.value.a;
			size = param->u.value.b;

			offset = ALIGN(offset, SMCINVOKE_ARGS_ALIGN_SIZE);
			if ((offset > buf_size) || (size > (buf_size - offset)))
				goto out;

			tz_args[i].b.offset = offset;
			tz_args[i].b.size = size;
			offset += size;
		}
	}

	for (i = 0; i < req->num_params; i++) {
		param = &params[i];
		if (IS_IN_OBJ_PARAM(param->attr)) {

			ret = qcomtee_get_tzhandle_from_uhandle(ctx, param, param->u.obj.fd,
							&(tz_args[i].handle));
			if (ret)
				goto out;
		}
	}

	for (i = 0; i < req->num_params; i++) {
		param = &params[i];
		if (IS_IN_MEMOBJ_PARAM(param->attr)) {
			ret = qcomtee_get_tzhandle_from_uhandle(ctx, param,
							param->u.memref.shm->id,
							&(tz_args[i].handle));
			if (ret)
				goto out;
		}
	}

	ret = 0;
out:
	return ret;
}

static int qcomtee_marshal_in_tzcb_req(struct tee_context *ctx,
				       const struct qcomtee_cb_txn *cb_txn,
				       struct tee_param *params, u32 *pnum_params,
				       u32 *func, int srvr_id)
{
	struct qcomtee *qtee = tee_get_drvdata(ctx->teedev);
	struct smcinvoke_tzcb_req *tzcb_req = cb_txn->cb_req;
	union smcinvoke_tz_args *tz_args = tzcb_req->args;
	size_t tzcb_req_len = cb_txn->cb_req_bytes;
	size_t tz_buf_offset = TZCB_BUF_OFFSET(tzcb_req);
	struct tee_param *param;
	size_t user_req_buf_offset = sizeof(*param) * OBJECT_COUNTS_TOTAL(tzcb_req->hdr.counts);
	u64 txn_id, buf_len, buf_addr, addr, size;
	int ret = 0, i = 0, n;
	u32 cbobj_id;
	int num_params = *pnum_params;
	int32_t temp_fd = UHANDLE_NULL;

	if (tz_buf_offset > tzcb_req_len) {
		ret = -EINVAL;
		goto out;
	}

	for (i = 0; i < num_params; i++) {
		if (params[i].attr && params[i].attr != QCOMTEE_PARAM_ATTR) {
			dev_err(qtee->dev, "Expected Meta data in first	parameter\n");
			return -EINVAL;
		}
	}

	txn_id = cb_txn->txn_id;
	if (qcomtee_get_uhandle_from_tzhandle(ctx, tzcb_req->hdr.tzhandle, srvr_id,
				&cbobj_id, true)) {
		ret = -EINVAL;
		goto out;
	}

	*func = tzcb_req->hdr.op;

	/*
	 * From Meta Param
	 *  get buf_addr from b
	 *	buf_len from c
	 *	add txn_id and cbobj_id to a.
	 */

	i = 0;
	n = 0;
	/* we expect first parameter to be have meta data like buffer addr
	 * and size
	 */
	if (params[0].attr != QCOMTEE_PARAM_ATTR)
		return -EINVAL;

	param = &params[0];
	param->u.value.a = (cbobj_id | (txn_id << 32));
	buf_addr = param->u.value.b;
	buf_len = param->u.value.c;

	n++;
	FOR_ARGS(i, tzcb_req->hdr.counts, BI) {
		user_req_buf_offset = ALIGN(user_req_buf_offset,
						 SMCINVOKE_ARGS_ALIGN_SIZE);
		param = &params[n++];
		param->attr = TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_INPUT;

		size = tz_args[i].b.size;

		if ((tz_args[i].b.offset > tzcb_req_len) ||
		    (tz_args[i].b.size > tzcb_req_len - tz_args[i].b.offset) ||
		    (user_req_buf_offset > buf_len) ||
		    (size > buf_len - user_req_buf_offset)) {
			ret = -EINVAL;
			dev_err(qtee->dev, "%s: buffer overflow detected\n", __func__);
			goto out;
		}
		addr = buf_addr + user_req_buf_offset;

		param->u.value.a = addr;
		param->u.value.b = size;

		if (copy_to_user(u64_to_user_ptr(addr),
				 (uint8_t *)(tzcb_req) + tz_args[i].b.offset,
				 size)) {
			ret = -EFAULT;
			goto out;
		}
		user_req_buf_offset += size;
	}

	FOR_ARGS(i, tzcb_req->hdr.counts, BO) {
		user_req_buf_offset = ALIGN(user_req_buf_offset,
					SMCINVOKE_ARGS_ALIGN_SIZE);
		param = &params[n++];
		param->attr = TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_OUTPUT;

		size = tz_args[i].b.size;

		if ((user_req_buf_offset > buf_len) ||
		    (size > buf_len - user_req_buf_offset)) {
			ret = -EINVAL;
			dev_err(qtee->dev, "%s: buffer overflow detected\n", __func__);
			goto out;
		}
		addr = buf_addr + user_req_buf_offset;

		param->u.value.a = addr;
		param->u.value.b = size;

		user_req_buf_offset += size;
	}

	FOR_ARGS(i, tzcb_req->hdr.counts, OI) {
		/*
		 * create a new FD and assign to output object's
		 * context
		 */
		temp_fd = UHANDLE_NULL;
		param = &params[n++];
		param->attr = TEE_IOCTL_PARAM_ATTR_TYPE_OBJECT_INPUT;

		ret = qcomtee_get_uhandle_from_tzhandle(ctx, tz_args[i].handle, srvr_id,
					&temp_fd, true);

		param->u.obj.fd = temp_fd;

		if (ret) {
			ret = -EINVAL;
			goto out;
		}
	}
	*pnum_params = n + 1;

out:
	return ret;
}

static int qcomtee_marshal_out_tzcb_req(struct tee_context *ctx, struct tee_param *params,
					int num_params,	u32 result,
					struct qcomtee_cb_txn *cb_txn, int srvr_id)
{
	struct qcomtee *qtee = tee_get_drvdata(ctx->teedev);
	struct smcinvoke_tzcb_req *tzcb_req = cb_txn->cb_req;
	union smcinvoke_tz_args *tz_args = tzcb_req->args;
	struct tee_param *param;
	__u64 addr, size;
	int ret = -EINVAL, i = 0;
	int n = 0;

	tzcb_req->result = result;
	n = 0;
	FOR_ARGS(i, tzcb_req->hdr.counts, BO) {
		for (; n < num_params; n++)
			if (IS_OUT_BUF_PARAM(params[n].attr))
				break;

		param = &params[n];
		addr = param->u.value.a;
		size = param->u.value.b;

		if (size > tz_args[i].b.size)
			goto out;
		if (copy_from_user((uint8_t *)(tzcb_req) + tz_args[i].b.offset,
					u64_to_user_ptr(addr), size)) {
			ret = -EFAULT;
			goto out;
		}
	}

	n  = 0;
	FOR_ARGS(i, tzcb_req->hdr.counts, OO) {
		for ( ; n < num_params; n++)
			if (IS_OUT_OBJ_PARAM(params[n].attr))
				break;

		param = &params[n];

		addr = param->u.value.a;
		size = param->u.value.b;

		ret = qcomtee_get_tzhandle_from_uhandle(ctx, param,
				param->u.obj.id, &(tz_args[i].handle));
		if (ret)
			goto out;
	}
	param = &params[0];
	if (param->attr == QCOMTEE_PARAM_ATTR) {
		uint32_t cobj_id = lower_32_bits(param->u.value.a);

		qcomtee_put_pending_cbobj(qtee, srvr_id, cobj_id);
	}
	ret = 0;
out:
	return ret;
}

static int qcomtee_invoke_func(struct tee_context *ctx,
			       struct tee_ioctl_invoke_arg *arg,
			       struct tee_param *params)
{
	struct qcomtee *qtee = tee_get_drvdata(ctx->teedev);
	struct tee_shm *in_shm, *out_shm;
	size_t inmsg_size, outmsg_size;
	void *in_msg, *out_msg;
	bool tz_acked;
	s32 result;
	u32 counts;
	int ret;
	/*
	 * If anything goes wrong, release alloted tzhandles for
	 * local objs which could be either CBObj or MemObj.
	 */
	counts = qcomtee_get_counts_from_tee_param(qtee, params, arg->num_params);
	inmsg_size = compute_in_msg_size(params, arg->num_params, counts);
	in_shm = tee_shm_alloc_priv_buf(ctx, inmsg_size);
	if (IS_ERR(in_shm)) {
		ret = PTR_ERR(in_shm);
		dev_err(qtee->dev, "shmbridge alloc failed for in msg in invoke req\n");
		goto out;
	}
	in_msg = in_shm->kaddr;

	outmsg_size = SMCINVOKE_MAX_CB_BUF_SIZE;
	out_shm = tee_shm_alloc_priv_buf(ctx, outmsg_size);
	if (IS_ERR(out_shm)) {
		ret = PTR_ERR(out_shm);
		dev_err(qtee->dev, "shmbridge alloc failed for out msg in invoke req\n");
		goto out;
	}
	out_msg = out_shm->kaddr;

	ret = marshal_in_invoke_req(ctx, arg, params, arg->session, in_msg,
				    inmsg_size, counts);
	if (ret) {
		dev_err(qtee->dev, "failed to marshal in invoke req, ret :%d\n",
			ret);
		goto out;
	}
	ret = qcomtee_prepare_send_scm_msg(ctx, in_msg, in_shm->paddr, inmsg_size,
				   out_msg, out_shm->paddr, outmsg_size,
				   arg->num_params, params, &tz_acked, in_shm,
				   out_shm, counts, &result);
	/*
	 * If scm_call is success, TZ owns responsibility to release
	 * refs for local objs.
	 */
	if (!tz_acked)
		goto out;

	/* Outbuf could be carrying local objs to be released. */
	qcomtee_process_piggyback_data(ctx, out_msg, outmsg_size);
out:
	tee_shm_free(in_shm);
	tee_shm_free(out_shm);

	return ret;
}

static struct qcomtee_cb_txn *find_cbtxn_locked(struct qcomtee_supp_info *server,
						uint32_t txn_id, int32_t state)
{
	struct qcomtee_cb_txn *cb_txn;
	int i = 0;

	/*
	 * Since HASH_BITS() does not work on pointers, we can't select hash
	 * table using state and loop over it.
	 */
	if (state == SMCINVOKE_REQ_PLACED) {
		/* pick up 1st req */
		hash_for_each(server->reqs_table, i, cb_txn, hash) {
			kref_get(&cb_txn->ref_cnt);
			hash_del(&cb_txn->hash);
			return cb_txn;
		}
	} else if (state == SMCINVOKE_REQ_PROCESSING) {
		hash_for_each_possible(server->responses_table, cb_txn, hash, txn_id) {
			if (cb_txn->txn_id == txn_id) {
				kref_get(&cb_txn->ref_cnt);
				hash_del(&cb_txn->hash);
				return cb_txn;
			}
		}
	}
	return NULL;
}

static int qcomtee_supp_send(struct tee_context *ctx, u32 result,
			     u32 num_params, struct tee_param *params)
{
	struct qcomtee *qtee = tee_get_drvdata(ctx->teedev);
	struct qcomtee_context_data *ctxdata = ctx->data;
	struct qcomtee_supp_info *supp_info;
	struct qcomtee_cb_txn *cb_txn;
	int ret;
	u32 txn_id;

	mutex_lock(&qtee->lock);
	supp_info = qcomtee_get_cb_server(qtee, ctxdata->server_id);
	if (!supp_info) {
		dev_err(qtee->dev, "No matching server with server id : %u found\n",
			ctxdata->server_id);
		mutex_unlock(&qtee->lock);
		return -EINVAL;
	}

	if (supp_info->state == SMCINVOKE_SERVER_STATE_DEFUNCT)
		supp_info->state = 0;

	mutex_unlock(&qtee->lock);

	/*
	 * we expect first parameter to be have meta data like buffer addr
	 * and size
	 */
	if (params[0].attr != QCOMTEE_PARAM_ATTR) {
		dev_err(qtee->dev, "Error First parameter is not META data\n");
		return -EINVAL;
	}

	txn_id = params->u.value.a >> 32;
	mutex_lock(&qtee->lock);
	cb_txn = find_cbtxn_locked(supp_info, txn_id, SMCINVOKE_REQ_PROCESSING);
	mutex_unlock(&qtee->lock);
	/*
	 * cb_txn can be null if userspace provides wrong txn id OR
	 * invoke thread died while server was processing cb req.
	 * if invoke thread dies, it would remove req from Q. So
	 * no matching cb_txn would be on Q and hence NULL cb_txn.
	 * In this case, we want this thread to come back and start
	 * waiting for new cb requests, hence return EAGAIN here
	 */
	if (!cb_txn) {
		dev_err(qtee->dev, "%s txn %d either invalid or removed from Q\n",
			__func__, txn_id);
		ret = -EAGAIN;
		goto out;
	}
	ret = qcomtee_marshal_out_tzcb_req(ctx, params, num_params, result, cb_txn,
					   supp_info->server_id);
	/*
	 * if client did not set error and we get error locally,
	 * we return local error to TA
	 */
	if (ret && cb_txn->cb_req->result == 0)
		cb_txn->cb_req->result = OBJECT_ERROR_UNAVAIL;

	cb_txn->state = SMCINVOKE_REQ_PROCESSED;
	kref_put(&cb_txn->ref_cnt, qcomtee_delete_cb_txn);

	wake_up(&supp_info->rsp_wait_q);

out:
	qcomtee_put_cb_server(supp_info);

	return ret;
}

static int qcomtee_supp_recv(struct tee_context *ctx, u32 *func, u32 *num_params,
			     struct tee_param *params)
{
	struct qcomtee *qtee = tee_get_drvdata(ctx->teedev);
	struct qcomtee_context_data *ctxdata = ctx->data;
	struct qcomtee_supp_info *supp_info;
	struct qcomtee_cb_txn *cb_txn;
	int ret;

	mutex_lock(&qtee->lock);
	supp_info = qcomtee_get_cb_server(qtee, ctxdata->server_id);
	if (!supp_info) {
		dev_err(qtee->dev, "No matching server with server id : %d found\n",
			ctxdata->server_id);
		mutex_unlock(&qtee->lock);
		return -EINVAL;
	}

	if (supp_info->state == SMCINVOKE_SERVER_STATE_DEFUNCT)
		supp_info->state = 0;

	mutex_unlock(&qtee->lock);

	do {
		ret = wait_event_interruptible(supp_info->req_wait_q,
					       !hash_empty(supp_info->reqs_table));
		if (ret) {
			dev_err(qtee->dev, "%s wait_event interrupted: ret = %d\n",
				__func__, ret);
			/*
			 * Ideally, we should destroy server if accept threads
			 * are returning due to client being killed or device
			 * going down (Shutdown/Reboot) but that would make
			 * supp_info invalid. Other accept/invoke threads are
			 * using supp_info and would crash. So dont do that.
			 */
			mutex_lock(&qtee->lock);
			supp_info->state = SMCINVOKE_SERVER_STATE_DEFUNCT;
			mutex_unlock(&qtee->lock);
			wake_up_interruptible(&supp_info->rsp_wait_q);
			goto out;
		}
		mutex_lock(&qtee->lock);
		cb_txn = find_cbtxn_locked(supp_info, SMCINVOKE_NEXT_AVAILABLE_TXN,
					   SMCINVOKE_REQ_PLACED);
		mutex_unlock(&qtee->lock);
		if (cb_txn) {
			cb_txn->state = SMCINVOKE_REQ_PROCESSING;
			ret = qcomtee_marshal_in_tzcb_req(ctx, cb_txn, params, num_params, func,
							  ctxdata->server_id);
			if (ret) {
				cb_txn->cb_req->result = OBJECT_ERROR_UNAVAIL;
				cb_txn->state = SMCINVOKE_REQ_PROCESSED;
				kref_put(&cb_txn->ref_cnt, qcomtee_delete_cb_txn);
				wake_up_interruptible(&supp_info->rsp_wait_q);
				continue;
			}

			mutex_lock(&qtee->lock);
			hash_add(supp_info->responses_table, &cb_txn->hash,
							cb_txn->txn_id);
			kref_put(&cb_txn->ref_cnt, qcomtee_delete_cb_txn);
			mutex_unlock(&qtee->lock);
		}
	}	while (!cb_txn);
out:
	qcomtee_put_cb_server(supp_info);

	return ret;
}

static int qcomtee_shm_register(struct tee_context *ctx, struct tee_shm *shm,
				struct page **pages, size_t num_pages,
				unsigned long start)
{
	/* TODO */
	return -ENOTSUPP;
}

static int qcomtee_shm_unregister(struct tee_context *ctx, struct tee_shm *shm)
{
	/* TODO */
	return -ENOTSUPP;
}

static const struct tee_driver_ops qcomtee_ops = {
	.get_version = qcomtee_get_version,
	.open = qcomtee_open,
	.release = qcomtee_release,
	.open_session = qcomtee_open_session,
	.close_session = qcomtee_close_session,
	.invoke_func = qcomtee_invoke_func,
	.shm_register = qcomtee_shm_register,
	.shm_unregister = qcomtee_shm_unregister,
};

static const struct tee_driver_ops qcomtee_supp_ops = {
	.get_version = qcomtee_get_version,
	.open = qcomtee_supp_open,
	.release = qcomtee_supp_release,
	.supp_recv = qcomtee_supp_recv,
	.supp_send = qcomtee_supp_send,
	.shm_register = qcomtee_shm_register,
	.shm_unregister = qcomtee_shm_unregister,
};

static const struct tee_desc qcomtee_desc = {
	.name = DRIVER_NAME "-clnt",
	.ops = &qcomtee_ops,
	.owner = THIS_MODULE,
};

static const struct tee_desc qcomtee_supp_desc = {
	.name = DRIVER_NAME "-supp",
	.ops = &qcomtee_supp_ops,
	.owner = THIS_MODULE,
	.flags = TEE_DESC_PRIVILEGED,
};

static int qcomtee_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct tee_device *teedev;
	struct tee_shm_pool *pool;
	struct qcomtee *qcomtee;
	int rc;

	if (!qcom_scm_is_available())
		return -EPROBE_DEFER;

	qcomtee = devm_kzalloc(dev, sizeof(*qcomtee), GFP_KERNEL);
	if (!qcomtee)
		return -ENOMEM;

	qcomtee->dev = dev;
	rc = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(64));
	if (rc) {
		dev_err(dev, "dma_set_mask_and_coherent failed %d\n", rc);
		return rc;
	}
	mutex_init(&qcomtee->lock);
	idr_init(&qcomtee->server_idr);
	idr_init(&qcomtee->mem_region_idr);
	idr_init(&qcomtee->mem_map_idr);

	INIT_LIST_HEAD(&qcomtee->bridge_list);

	pool = qcomtee_shmbridge_init(qcomtee);
	if (IS_ERR(pool)) {
		dev_err(dev, "Error creating shared memory bridge\n");
		return PTR_ERR(pool);
	}

	/* tee client device */
	teedev = tee_device_alloc(&qcomtee_desc, NULL, pool, qcomtee);
	if (IS_ERR(teedev)) {
		rc = PTR_ERR(teedev);
		goto err_free_pool;
	}
	qcomtee->teedev = teedev;

	teedev = tee_device_alloc(&qcomtee_supp_desc, NULL, pool, qcomtee);
	if (IS_ERR(teedev)) {
		rc = PTR_ERR(teedev);
		goto err_free_pool;
	}
	qcomtee->supp_teedev = teedev;

	rc = tee_device_register(qcomtee->teedev);
	if (rc)
		goto err_device_unregister;

	rc = tee_device_register(qcomtee->supp_teedev);
	if (rc)
		goto err_device_supp_unregister;

	qcomtee->pool = pool;

	return 0;

err_device_supp_unregister:
	tee_device_unregister(qcomtee->supp_teedev);
err_device_unregister:
	tee_device_unregister(qcomtee->teedev);

err_free_pool:

	tee_shm_pool_free(pool);
	return rc;
}

static const struct of_device_id qcomtee_match[] = {
	{
		.compatible = "qcom,tee",
	},
	{},
};

static struct platform_driver qcomtee_platform_driver = {
	.probe = qcomtee_probe,
	.driver = {
		.name = "qcomtee",
		.of_match_table = qcomtee_match,
	},
};
module_platform_driver(qcomtee_platform_driver);

MODULE_DESCRIPTION("Qualcomm TEE driver");
MODULE_LICENSE("GPL");
