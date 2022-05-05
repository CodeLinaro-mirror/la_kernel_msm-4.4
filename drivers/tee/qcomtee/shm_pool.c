#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/qcom_scm.h>
#include <linux/slab.h>
#include <linux/tee_drv.h>
#include "qcomtee.h"

#define DEFAULT_BRIDGE_SIZE	SZ_4M	/*4M*/
#define MAXSHMVMS 4
#define PERM_BITS 3
#define VM_BITS 16
#define SELF_OWNER_BIT 1
#define SHM_NUM_VM_SHIFT 9
#define SHM_VM_MASK 0xFFFF
#define SHM_PERM_MASK 0x7

#define VM_PERM_R	0x4
#define VM_PERM_W	0x2
#define VMID_HLOS	0x3

#define SHMBRIDGE_E_NOT_SUPPORTED 4	/* SHMbridge is not implemented */

/* ns_vmids */
#define UPDATE_NS_VMIDS(ns_vmids, id)	\
				(((uint64_t)(ns_vmids) << VM_BITS) \
				| ((uint64_t)(id) & SHM_VM_MASK))

/* ns_perms */
#define UPDATE_NS_PERMS(ns_perms, perm)	\
				(((uint64_t)(ns_perms) << PERM_BITS) \
				| ((uint64_t)(perm) & SHM_PERM_MASK))

/* pfn_and_ns_perm_flags = paddr | ns_perms */
#define UPDATE_PFN_AND_NS_PERM_FLAGS(paddr, ns_perms)	\
				((uint64_t)(paddr) | (ns_perms))

/* ipfn_and_s_perm_flags = ipaddr | tz_perm */
#define UPDATE_IPFN_AND_S_PERM_FLAGS(ipaddr, tz_perm)	\
				((uint64_t)(ipaddr) | (uint64_t)(tz_perm))

/* size_and_flags when dest_vm is not HYP */
#define UPDATE_SIZE_AND_FLAGS(size, destnum)	\
				((size) | (destnum) << SHM_NUM_VM_SHIFT)

int32_t qtee_shmbridge_enable(struct qcomtee *qtee)
{
	int32_t ret;

	ret = qcom_scm_enable_shm_bridge();
	if (ret) {
		dev_err(qtee->dev, "Failed to enable shmbridge, ret = %d\n", ret);
		if (ret == -EIO || ret == SHMBRIDGE_E_NOT_SUPPORTED)
			dev_err(qtee->dev, "shmbridge is not supported by this target\n");
		return ret;
	}
	qtee->shmbridge_enabled = true;

	return ret;
}

static struct qcomtee_shm_bridge *qcomtee_alloc_shm_bridge(struct qcomtee *qtee,
							   phys_addr_t paddr,
							   size_t size,
							   uint64_t handle)
{
	struct qcomtee_shm_bridge *bridge;

	bridge = kzalloc(sizeof(*bridge), GFP_KERNEL);
	if (!bridge)
		return ERR_PTR(-ENOMEM);

	kref_init(&bridge->ref_cnt);
	bridge->handle = handle;
	bridge->paddr = paddr;
	bridge->size = size;
	list_add_tail(&bridge->list, &qtee->bridge_list);

	return bridge;
}

static void qcomtee_free_shm_bridge(struct kref *kref)
{
	struct qcomtee_shm_bridge *bridge;

	bridge = container_of(kref, struct qcomtee_shm_bridge, ref_cnt);

	list_del(&bridge->list);

	qcom_scm_delete_shm_bridge(bridge->handle);
//FIXME handle return Not sure what we could do here if it fails...

	kfree(bridge);
}

struct qcomtee_shm_bridge *qcomtee_get_shm_bridge(struct tee_context *ctx,
						   phys_addr_t paddr,
						   size_t size)
{
	struct qcomtee *qtee = tee_get_drvdata(ctx->teedev);
	struct qcomtee_shm_bridge *bridge;

	list_for_each_entry(bridge, &qtee->bridge_list, list) {
		if ((paddr >= bridge->paddr) &&
		    (paddr + size <= (bridge->paddr + bridge->size))) {
			kref_get(&bridge->ref_cnt);
			return bridge;

		}
	}
	return NULL;
}

void qcomtee_put_shm_bridge(struct qcomtee_shm_bridge *bridge)
{
	kref_put(&bridge->ref_cnt, qcomtee_free_shm_bridge);
}

static struct qcomtee_shm_bridge *qtee_shmbridge_register(struct qcomtee *qtee,
							  phys_addr_t paddr, size_t size,
							  uint32_t *ns_vmid_list, uint32_t *ns_vm_perm_list,
							  uint32_t ns_vmid_num, uint32_t tz_perm, uint64_t *handle)
{
	uint64_t pfn_and_ns_perm_flags, ipfn_and_s_perm_flags, size_and_flags;
	uint64_t ns_perms = 0;
	uint64_t ns_vmids = 0;
	int32_t ret;
	int i;

	if (!handle || !ns_vmid_list || !ns_vm_perm_list || ns_vmid_num > MAXSHMVMS)
		return ERR_PTR(-EINVAL);

	for (i = 0; i < ns_vmid_num; i++) {
		ns_perms = UPDATE_NS_PERMS(ns_perms, ns_vm_perm_list[i]);
		ns_vmids = UPDATE_NS_VMIDS(ns_vmids, ns_vmid_list[i]);
	}

	pfn_and_ns_perm_flags = UPDATE_PFN_AND_NS_PERM_FLAGS(paddr, ns_perms);
	ipfn_and_s_perm_flags = UPDATE_IPFN_AND_S_PERM_FLAGS(paddr, tz_perm);
	size_and_flags = UPDATE_SIZE_AND_FLAGS(size, ns_vmid_num);

	ret = qcom_scm_create_shm_bridge(pfn_and_ns_perm_flags,
					 ipfn_and_s_perm_flags, size_and_flags,
					 ns_vmids, handle);

	if (ret) {
		dev_err(qtee->dev, "create shmbridge failed, ret = %d\n", ret);
		return ERR_PTR(-EINVAL);
	}

	return qcomtee_alloc_shm_bridge(qtee, paddr, size, *handle);
}

struct tee_shm_pool *qcomtee_shmbridge_init(struct qcomtee *qtee)
{
	struct qcomtee_shm_bridge *bridge;
	struct tee_shm_pool *pool;
	int ret = 0;
	uint32_t *ns_vm_ids;
	uint32_t ns_vm_ids_hlos[] = {VMID_HLOS};
	uint32_t ns_vm_perms[] = {VM_PERM_R|VM_PERM_W};
	struct device *dev = qtee->dev;
	phys_addr_t paddr;
	void *vaddr;
	size_t size;
	uint64_t handle;

	ns_vm_ids = ns_vm_ids_hlos;

	size = DEFAULT_BRIDGE_SIZE;
	vaddr = dma_alloc_coherent(dev, size, &paddr, GFP_KERNEL);
	if (!vaddr) {
		dev_err(dev, "Error Creating coherent memory poll\n");
		return NULL;
	}

	pool = tee_shm_pool_alloc_res_mem((unsigned long) vaddr, paddr,
					size, PAGE_SHIFT);
	if (IS_ERR(pool))
		goto exit_unmap;

	ret = qtee_shmbridge_enable(qtee);
	if (ret) {
		/* keep the mem pool and return if failed to enable bridge */
		ret = 0;
		goto err_bridge_enable;
	}

	dev_info(qtee->dev, "shmbridge is enabled on tz side.");

	/*register default bridge for priv */
	bridge = qtee_shmbridge_register(qtee, paddr, size, ns_vm_ids, ns_vm_perms,
					 1, VM_PERM_R|VM_PERM_W, &handle);

	if (IS_ERR(bridge)) {
		dev_err(dev, "Failed to register default bridge, size %zu\n",
			size);
		goto exit_deregister;
	}

	qtee->bridge = bridge;

	return pool;

exit_deregister:
	qcomtee_put_shm_bridge(qtee->bridge);
	qtee_shmbridge_enable(false);
err_bridge_enable:
	tee_shm_pool_free(pool);
exit_unmap:
	dma_free_coherent(dev, size, vaddr, paddr);
	return ERR_PTR(ret);
}
