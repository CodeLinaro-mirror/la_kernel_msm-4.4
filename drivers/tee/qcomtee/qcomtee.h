#ifndef QCOMTEE_H
#define QCOMTEE_H

struct qcomtee {
	struct device *dev;
	struct tee_device *teedev;
	struct tee_device *supp_teedev;
	struct tee_shm_pool *pool;
	struct mutex lock;

	struct idr server_idr;
	struct idr mem_region_idr;
	struct idr mem_map_idr;
	struct list_head bridge_list;

	struct qcomtee_shm_bridge *bridge;
	bool shmbridge_enabled;
};

struct qcomtee_shm_bridge {
	phys_addr_t paddr;
	size_t size;
	uint64_t handle;
	struct kref ref_cnt;
	struct list_head list;
};

struct tee_shm_pool *qcomtee_shmbridge_init(struct qcomtee *qtee);
int32_t qtee_shmbridge_enable(struct qcomtee *qtee);
void qcomtee_put_shm_bridge(struct qcomtee_shm_bridge *bridge);
struct qcomtee_shm_bridge *qcomtee_get_shm_bridge(struct tee_context *ctx,
						  phys_addr_t paddr,
						  size_t size);

#endif /* QCOMTEE_H */
