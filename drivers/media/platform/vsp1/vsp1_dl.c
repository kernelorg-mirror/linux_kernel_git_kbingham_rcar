/*
 * vsp1_dl.h  --  R-Car VSP1 Display List
 *
 * Copyright (C) 2015 Renesas Corporation
 *
 * Contact: Laurent Pinchart (laurent.pinchart@ideasonboard.com)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/gfp.h>

#include "vsp1.h"
#include "vsp1_dl.h"
#include "vsp1_pipe.h"

/*
 * Global resources
 *
 * - Display-related interrupts (can be used for vblank evasion ?)
 * - Display-list enable
 * - Header-less for WPF0
 * - DL swap
 */

#define VSP1_DL_BODY_SIZE		(2 * 4 * 256)
#define VSP1_DL_NUM_LISTS		3

struct vsp1_dl_entry {
	u32 addr;
	u32 data;
} __attribute__((__packed__));

struct vsp1_dl_list {
	size_t size;
	int reg_count;

	bool in_use;

	struct vsp1_dl_entry *body;
	dma_addr_t dma;
};

/**
 * struct vsp1_dl - Display List manager
 * @vsp1: the VSP1 device
 * @lock: protects the active, queued and pending lists
 * @lists.all: array of all allocate display lists
 * @lists.active: list currently processed by hardware
 * @lists.queued: list queued to the hardware for after the next vblank
 * @lists.pending: list waiting to be queued to the hardware
 * @lists.write: list being written to by software
 */
struct vsp1_dl {
	struct vsp1_device *vsp1;

	spinlock_t lock;

	size_t size;
	dma_addr_t dma;
	void *mem;

	struct {
		struct vsp1_dl_list all[VSP1_DL_NUM_LISTS];

		struct vsp1_dl_list *active;
		struct vsp1_dl_list *queued;
		struct vsp1_dl_list *pending;
		struct vsp1_dl_list *write;
	} lists;
};

/* -----------------------------------------------------------------------------
 * Display List Transaction Management
 */

static void vsp1_dl_free_list(struct vsp1_dl_list *list)
{
	if (!list)
		return;

	list->in_use = false;
}

void vsp1_dl_reset(struct vsp1_dl *dl)
{
	unsigned int i;

	dl->lists.active = NULL;
	dl->lists.queued = NULL;
	dl->lists.pending = NULL;
	dl->lists.write = NULL;

	for (i = 0; i < ARRAY_SIZE(dl->lists.all); ++i)
		dl->lists.all[i].in_use = false;
}

void vsp1_dl_begin(struct vsp1_dl *dl)
{
	struct vsp1_dl_list *list = NULL;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(dl->lists.all); ++i) {
		if (!dl->lists.all[i].in_use) {
			list = &dl->lists.all[i];
			break;
		}
	}

	dl->lists.write = list;

	list->in_use = true;
	list->reg_count = 0;
}

void vsp1_dl_add(struct vsp1_entity *e, u32 reg, u32 data)
{
	struct vsp1_pipeline *pipe = to_vsp1_pipeline(&e->subdev.entity);
	struct vsp1_dl *dl = pipe->dl;
	struct vsp1_dl_list *list = dl->lists.write;

	list->body[list->reg_count].addr = reg;
	list->body[list->reg_count].data = data;
	list->reg_count++;
}

void vsp1_dl_commit(struct vsp1_dl *dl)
{
	struct vsp1_device *vsp1 = dl->vsp1;
	struct vsp1_dl_list *list;
	unsigned long flags;
	bool update;

	list = dl->lists.write;
	dl->lists.write = NULL;

	/* Once the UPD bit has been set the hardware can start processing the
	 * display list at any time and we can't touch the address and size
	 * registers. In that case mark the update as pending, it will be
	 * queued up to the hardware by the frame end interrupt handler.
	 */
	update = !!(vsp1_read(vsp1, VI6_DL_BODY_SIZE) & VI6_DL_BODY_SIZE_UPD);
	if (update) {
		spin_lock_irqsave(&dl->lock, flags);
		vsp1_dl_free_list(dl->lists.pending);
		dl->lists.pending = list;
		spin_unlock_irqrestore(&dl->lock, flags);
		return;
	}

	spin_lock_irqsave(&dl->lock, flags);

	/* Program the hardware with the display list body address and size.
	 * The UPD bit will be cleared by the device when the display list is
	 * processed.
	 */
	vsp1_write(vsp1, VI6_DL_HDR_ADDR(0), list->dma);
	vsp1_write(vsp1, VI6_DL_BODY_SIZE, VI6_DL_BODY_SIZE_UPD |
		   (list->reg_count * 8));

	vsp1_dl_free_list(dl->lists.queued);
	dl->lists.queued = list;

	spin_unlock_irqrestore(&dl->lock, flags);
}

/* -----------------------------------------------------------------------------
 * Interrupt Handling
 */

void vsp1_dl_irq_display_start(struct vsp1_dl *dl)
{
	struct vsp1_device *vsp1 = dl->vsp1;
	struct vsp1_dl_list *list = NULL;

	spin_lock(&dl->lock);

	if (dl->lists.queued) {
		vsp1_dl_free_list(dl->lists.active);
		dl->lists.active = dl->lists.queued;
		dl->lists.queued = NULL;

		list = dl->lists.active;
	}

	spin_unlock(&dl->lock);

	if (list) {
		vsp1_write(vsp1, VI6_DL_HDR_ADDR(0), list->dma);
		vsp1_write(vsp1, VI6_DL_BODY_SIZE,
			(list->reg_count * 8) | VI6_DL_BODY_SIZE_UPD);
	}
}

void vsp1_dl_irq_frame_end(struct vsp1_dl *dl)
{
	spin_lock(&dl->lock);

	if (dl->lists.queued) {
		vsp1_dl_free_list(dl->lists.active);
		dl->lists.active = dl->lists.queued;
		dl->lists.queued = NULL;
	}

	spin_unlock(&dl->lock);
}

/* -----------------------------------------------------------------------------
 * Hardware Setup
 */

void vsp1_dl_setup(struct vsp1_device *vsp1)
{
	u32 ctrl = (256 << VI6_DL_CTRL_AR_WAIT_SHIFT)
		 | VI6_DL_CTRL_DC2 | VI6_DL_CTRL_DC1 | VI6_DL_CTRL_DC0
		 | VI6_DL_CTRL_DLE;

	/* The DRM pipeline operates with header-less display lists in
	 * Continuous Frame Mode.
	 */
	if (vsp1->drm)
		ctrl |= VI6_DL_CTRL_CFM0 | VI6_DL_CTRL_NH0;

	vsp1_write(vsp1, VI6_DL_CTRL, ctrl);
	vsp1_write(vsp1, VI6_DL_SWAP, VI6_DL_SWAP_LWS);
}

/* -----------------------------------------------------------------------------
 * Initialization and Cleanup
 */

struct vsp1_dl *vsp1_dl_create(struct vsp1_device *vsp1)
{
	struct vsp1_dl *dl;
	unsigned int i;

	dl = kzalloc(vsp1->dev, sizeof(*dl), GFP_KERNEL);
	if (!dl)
		return NULL;

	spin_lock_init(&dl->lock);

	dl->vsp1 = vsp1;
	dl->size = VSP1_DL_BODY_SIZE * ARRAY_SIZE(dl->lists.all);

	dl->mem = dma_alloc_writecombine(vsp1->dev, dl->size, &dl->dma,
					 GFP_KERNEL);
	if (!dl->mem) {
		kfree(dl);
		return NULL;
	}

	for (i = 0; i < ARRAY_SIZE(dl->lists.all); ++i) {
		struct vsp1_dl_list *list = &dl->lists.all[i];

		list->size = VSP1_DL_BODY_SIZE;
		list->reg_count = 0;
		list->in_use = false;
		list->dma = dl->dma + VSP1_DL_BODY_SIZE * i;
		list->body = dl->mem + VSP1_DL_BODY_SIZE * i;
	}

	return dl;
}

void vsp1_dl_destroy(struct vsp1_dl *dl)
{
	dma_free_writecombine(dl->vsp1->dev, dl->size, dl->mem, dl->dma);
	kfree(dl);
}
