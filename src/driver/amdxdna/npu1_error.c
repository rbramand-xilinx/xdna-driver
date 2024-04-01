// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2023-2024, Advanced Micro Devices, Inc.
 */

#include <linux/kthread.h>
#include <linux/kernel.h>
#include <drm/drm_cache.h>
#include "npu1_msg_priv.h"
#include "npu1_pci.h"

struct async_event {
	struct npu_device		*ndev;
	struct async_event_msg_resp	resp;
	struct workqueue_struct		*wq;
	struct work_struct		work;
	u8				*buf;
	dma_addr_t			addr;
	u32				size;
};

struct async_events {
	struct workqueue_struct		*wq;
	u8				*buf;
	dma_addr_t			addr;
	u32				size;
	u32				event_cnt;
	struct async_event		event[] __counted_by(event_cnt);
};

/*
 * Below enum, struct and lookup tables are porting from XAIE util header file.
 *
 * Below data is defined by AIE device and it is used for decode error message
 * from the device.
 */

enum aie_module_type {
	AIE_MEM_MOD = 0,
	AIE_CORE_MOD,
	AIE_PL_MOD,
};

enum aie_error_category {
	AIE_ERROR_SATURATION = 0,
	AIE_ERROR_FP,
	AIE_ERROR_STREAM,
	AIE_ERROR_ACCESS,
	AIE_ERROR_BUS,
	AIE_ERROR_INSTRUCTION,
	AIE_ERROR_ECC,
	AIE_ERROR_LOCK,
	AIE_ERROR_DMA,
	AIE_ERROR_MEM_PARITY,
	/* Unknown is not from XAIE, added for better category */
	AIE_ERROR_UNKNOWN,
};

/* Don't pack, unless XAIE side changed */
struct aie_error {
	u8			row;
	u8			col;
	enum aie_module_type mod_type;
	u8			event_id;
};

struct aie_event_category {
	u8			event_id;
	enum aie_error_category category;
};

#define EVENT_CATEGORY(id, cat) { id, cat }
static const struct aie_event_category aie_ml_mem_event_cat[] = {
	EVENT_CATEGORY(88U,  AIE_ERROR_ECC),
	EVENT_CATEGORY(90U,  AIE_ERROR_ECC),
	EVENT_CATEGORY(91U,  AIE_ERROR_MEM_PARITY),
	EVENT_CATEGORY(92U,  AIE_ERROR_MEM_PARITY),
	EVENT_CATEGORY(93U,  AIE_ERROR_MEM_PARITY),
	EVENT_CATEGORY(94U,  AIE_ERROR_MEM_PARITY),
	EVENT_CATEGORY(95U,  AIE_ERROR_MEM_PARITY),
	EVENT_CATEGORY(96U,  AIE_ERROR_MEM_PARITY),
	EVENT_CATEGORY(97U,  AIE_ERROR_DMA),
	EVENT_CATEGORY(98U,  AIE_ERROR_DMA),
	EVENT_CATEGORY(99U,  AIE_ERROR_DMA),
	EVENT_CATEGORY(100U, AIE_ERROR_DMA),
	EVENT_CATEGORY(101U, AIE_ERROR_LOCK),
};

static const struct aie_event_category aie_ml_core_event_cat[] = {
	EVENT_CATEGORY(55U, AIE_ERROR_ACCESS),
	EVENT_CATEGORY(56U, AIE_ERROR_STREAM),
	EVENT_CATEGORY(57U, AIE_ERROR_STREAM),
	EVENT_CATEGORY(58U, AIE_ERROR_BUS),
	EVENT_CATEGORY(59U, AIE_ERROR_INSTRUCTION),
	EVENT_CATEGORY(60U, AIE_ERROR_ACCESS),
	EVENT_CATEGORY(62U, AIE_ERROR_ECC),
	EVENT_CATEGORY(64U, AIE_ERROR_ECC),
	EVENT_CATEGORY(65U, AIE_ERROR_ACCESS),
	EVENT_CATEGORY(66U, AIE_ERROR_ACCESS),
	EVENT_CATEGORY(67U, AIE_ERROR_LOCK),
	EVENT_CATEGORY(70U, AIE_ERROR_INSTRUCTION),
	EVENT_CATEGORY(71U, AIE_ERROR_STREAM),
	EVENT_CATEGORY(72U, AIE_ERROR_BUS),
};

static const struct aie_event_category aie_ml_mem_tile_event_cat[] = {
	EVENT_CATEGORY(130U, AIE_ERROR_ECC),
	EVENT_CATEGORY(132U, AIE_ERROR_ECC),
	EVENT_CATEGORY(133U, AIE_ERROR_DMA),
	EVENT_CATEGORY(134U, AIE_ERROR_DMA),
	EVENT_CATEGORY(135U, AIE_ERROR_STREAM),
	EVENT_CATEGORY(136U, AIE_ERROR_STREAM),
	EVENT_CATEGORY(137U, AIE_ERROR_STREAM),
	EVENT_CATEGORY(138U, AIE_ERROR_BUS),
	EVENT_CATEGORY(139U, AIE_ERROR_LOCK),
};

static const struct aie_event_category aie_ml_shim_tile_event_cat[] = {
	EVENT_CATEGORY(64U, AIE_ERROR_BUS),
	EVENT_CATEGORY(65U, AIE_ERROR_STREAM),
	EVENT_CATEGORY(66U, AIE_ERROR_STREAM),
	EVENT_CATEGORY(67U, AIE_ERROR_BUS),
	EVENT_CATEGORY(68U, AIE_ERROR_BUS),
	EVENT_CATEGORY(69U, AIE_ERROR_BUS),
	EVENT_CATEGORY(70U, AIE_ERROR_BUS),
	EVENT_CATEGORY(71U, AIE_ERROR_BUS),
	EVENT_CATEGORY(72U, AIE_ERROR_DMA),
	EVENT_CATEGORY(73U, AIE_ERROR_DMA),
	EVENT_CATEGORY(74U, AIE_ERROR_LOCK),
};

static enum aie_error_category
aie_get_error_category(u8 row, u8 event_id, enum aie_module_type mod_type)
{
	const struct aie_event_category *lut;
	int num_entry;
	int i;

	switch (mod_type) {
	case AIE_PL_MOD:
		lut = aie_ml_shim_tile_event_cat;
		num_entry = ARRAY_SIZE(aie_ml_shim_tile_event_cat);
		break;
	case AIE_CORE_MOD:
		lut = aie_ml_core_event_cat;
		num_entry = ARRAY_SIZE(aie_ml_core_event_cat);
		break;
	case AIE_MEM_MOD:
		if (row == 1) {
			lut = aie_ml_mem_tile_event_cat;
			num_entry = ARRAY_SIZE(aie_ml_mem_tile_event_cat);
		} else {
			lut = aie_ml_mem_event_cat;
			num_entry = ARRAY_SIZE(aie_ml_mem_event_cat);
		}
		break;
	default:
		return AIE_ERROR_UNKNOWN;
	}

	for (i = 0; i < num_entry; i++) {
		if (event_id != lut[i].event_id)
			continue;

		return lut[i].category;
	}

	return AIE_ERROR_UNKNOWN;
}

static u32 npu1_error_backtrack(struct npu_device *ndev, void *err_info, u32 num_err)
{
	struct aie_error *errs = err_info;
	u32 err_col = 0; /* assume that AIE has less than 32 columns */
	int i;

	/* Get err column bitmap */
	for (i = 0; i < num_err; i++) {
		struct aie_error *err = &errs[i];
		enum aie_error_category cat;

		cat = aie_get_error_category(err->row, err->event_id, err->mod_type);
		XDNA_ERR(ndev->xdna, "Row: %d, Col: %d, module %d, event ID %d, category %d",
			 err->row, err->col, err->mod_type,
			 err->event_id, cat);

		err_col |= (1 << err->col);
	}

	/* TODO: Send AIE error to EDAC system */

	return err_col;
}

static void npu1_error_async_cb(void *handle, const u32 *data, size_t size)
{
	struct async_event_msg_resp *resp;
	struct async_event *e = handle;

	if (data) {
		resp = (struct async_event_msg_resp *)data;
		e->resp.type = resp->type;
		wmb(); /* Update status in the end, so that no lock for here */
		e->resp.status = resp->status;
	}
	queue_work(e->wq, &e->work);
}

static void npu1_error_worker(struct work_struct *err_work)
{
	struct amdxdna_client *client;
	struct amdxdna_dev *xdna;
	struct async_event *e;
	u32 err_col;

	e = container_of(err_work, struct async_event, work);

	xdna = e->ndev->xdna;

	if (e->resp.status == NPU_STATUS_MAX_NPU_STATUS_CODE)
		return;

	e->resp.status = NPU_STATUS_MAX_NPU_STATUS_CODE;
	drm_clflush_virt_range(e->buf, e->size);

	print_hex_dump_debug("AIE error: ", DUMP_PREFIX_OFFSET, 16, 4,
			     e->buf, sizeof(struct aie_error), false);

	err_col = npu1_error_backtrack(e->ndev, e->buf, 1);
	if (!err_col) {
		XDNA_WARN(xdna, "Did not get error column");
		return;
	}

	/* Found error columns, let's start recovery */
	mutex_lock(&xdna->dev_lock);
	list_for_each_entry(client, &xdna->client_list, node)
		npu1_stop_ctx_by_col_map(client, err_col);

	/*
	 * The error columns will be reset after all hardware
	 * contexts which use these columns are destroyed.
	 * So try to restart the hardware contexts.
	 */
	list_for_each_entry(client, &xdna->client_list, node)
		npu1_restart_ctx(client);

	/* Re-sent this event to firmware */
	if (npu1_register_asyn_event_msg(e->ndev, e->addr, e->size, e,
					 npu1_error_async_cb))
		XDNA_WARN(xdna, "Unable to register async event");
	mutex_unlock(&xdna->dev_lock);
}

int npu1_error_async_events_send(struct npu_device *ndev)
{
	struct amdxdna_dev *xdna = ndev->xdna;
	struct async_event *e;
	int i, ret;

	drm_WARN_ON(&xdna->ddev, !mutex_is_locked(&xdna->dev_lock));
	for (i = 0; i < ndev->async_events->event_cnt; i++) {
		e = &ndev->async_events->event[i];
		ret = npu1_register_asyn_event_msg(ndev, e->addr, e->size,
						   e, npu1_error_async_cb);
		if (ret)
			return ret;
	}

	return 0;
}

void npu1_error_async_events_free(struct npu_device *ndev)
{
	struct amdxdna_dev *xdna = ndev->xdna;
	struct async_events *events;

	events = ndev->async_events;
	destroy_workqueue(events->wq);
	dma_free_noncoherent(xdna->ddev.dev, events->size, events->buf,
			     events->addr, DMA_FROM_DEVICE);
	kfree(events);
}

int npu1_error_async_events_alloc(struct npu_device *ndev)
{
	struct amdxdna_dev *xdna = ndev->xdna;
	u32 total_col = ndev->total_col;
	u32 total_size = ASYNC_BUF_SIZE * total_col;
	struct async_events *events;
	int i, ret;

	events = kzalloc(struct_size(events, event, total_col), GFP_KERNEL);
	if (!events)
		return -ENOMEM;

	events->buf = dma_alloc_noncoherent(xdna->ddev.dev, total_size, &events->addr,
					    DMA_FROM_DEVICE, GFP_KERNEL);
	if (!events->buf) {
		ret = -ENOMEM;
		goto free_events;
	}
	events->size = total_size;
	events->event_cnt = total_col;

	events->wq = alloc_ordered_workqueue("async_wq", 0);
	if (!events->wq) {
		ret = -ENOMEM;
		goto free_buf;
	}

	for (i = 0; i < events->event_cnt; i++) {
		struct async_event *e = &events->event[i];
		u32 offset = i * ASYNC_BUF_SIZE;

		e->ndev = ndev;
		e->wq = events->wq;
		e->buf = &events->buf[offset];
		e->addr = events->addr + offset;
		e->size = ASYNC_BUF_SIZE;
		e->resp.status = NPU_STATUS_MAX_NPU_STATUS_CODE;
		INIT_WORK(&e->work, npu1_error_worker);
	}

	ndev->async_events = events;

	XDNA_DBG(xdna, "Async event count %d, buf total size 0x%x",
		 events->event_cnt, events->size);
	return 0;

free_buf:
	dma_free_noncoherent(xdna->ddev.dev, events->size, events->buf,
			     events->addr, DMA_FROM_DEVICE);
free_events:
	kfree(events);
	return ret;
}
