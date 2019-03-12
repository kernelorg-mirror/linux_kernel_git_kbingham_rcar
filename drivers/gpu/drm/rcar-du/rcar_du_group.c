// SPDX-License-Identifier: GPL-2.0+
/*
 * rcar_du_group.c  --  R-Car Display Unit Channels Pair
 *
 * Copyright (C) 2013-2015 Renesas Electronics Corporation
 *
 * Contact: Laurent Pinchart (laurent.pinchart@ideasonboard.com)
 */

/*
 * The R8A7779 DU is split in per-CRTC resources (scan-out engine, blending
 * unit, timings generator, ...) and device-global resources (start/stop
 * control, planes, ...) shared between the two CRTCs.
 *
 * The R8A7790 introduced a third CRTC with its own set of global resources.
 * This would be modeled as two separate DU device instances if it wasn't for
 * a handful or resources that are shared between the three CRTCs (mostly
 * related to input and output routing). For this reason the R8A7790 DU must be
 * modeled as a single device with three CRTCs, two sets of "semi-global"
 * resources, and a few device-global resources.
 *
 * The rcar_du_group object is a driver specific object, without any real
 * counterpart in the DU documentation, that models those semi-global resources.
 */

#include <linux/clk.h>
#include <linux/err.h>
#include <linux/io.h>

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_state_helper.h>
#include <drm/drm_device.h>

#include "rcar_du_drv.h"
#include "rcar_du_group.h"
#include "rcar_du_regs.h"

u32 rcar_du_group_read(struct rcar_du_group *rgrp, u32 reg)
{
	return rcar_du_read(rgrp->dev, rgrp->mmio_offset + reg);
}

void rcar_du_group_write(struct rcar_du_group *rgrp, u32 reg, u32 data)
{
	rcar_du_write(rgrp->dev, rgrp->mmio_offset + reg, data);
}

static void rcar_du_group_setup_pins(struct rcar_du_group *rgrp)
{
	u32 defr6 = DEFR6_CODE;

	if (rgrp->channels_mask & BIT(0))
		defr6 |= DEFR6_ODPM02_DISP;

	if (rgrp->channels_mask & BIT(1))
		defr6 |= DEFR6_ODPM12_DISP;

	rcar_du_group_write(rgrp, DEFR6, defr6);
}

static void rcar_du_group_setup_defr8(struct rcar_du_group *rgrp)
{
	struct rcar_du_device *rcdu = rgrp->dev;
	u32 defr8 = DEFR8_CODE;

	if (rcdu->info->gen < 3) {
		defr8 |= DEFR8_DEFE8;

		/*
		 * On Gen2 the DEFR8 register for the first group also controls
		 * RGB output routing to DPAD0 and VSPD1 routing to DU0/1/2 for
		 * DU instances that support it.
		 */
		if (rgrp->index == 0) {
			defr8 |= DEFR8_DRGBS_DU(rcdu->dpad0_source);
			if (rgrp->dev->vspd1_sink == 2)
				defr8 |= DEFR8_VSCS;
		}
	} else {
		/*
		 * On Gen3 VSPD routing can't be configured, and DPAD routing
		 * is set in the group corresponding to the DPAD output (no Gen3
		 * SoC has multiple DPAD sources belonging to separate groups).
		 */
		if (rgrp->index == rcdu->dpad0_source / 2)
			defr8 |= DEFR8_DRGBS_DU(rcdu->dpad0_source);
	}

	rcar_du_group_write(rgrp, DEFR8, defr8);
}

static void rcar_du_group_setup_didsr(struct rcar_du_group *rgrp)
{
	struct rcar_du_device *rcdu = rgrp->dev;
	struct rcar_du_crtc *rcrtc;
	unsigned int num_crtcs = 0;
	unsigned int i;
	u32 didsr;

	/*
	 * Configure input dot clock routing with a hardcoded configuration. If
	 * the DU channel can use the LVDS encoder output clock as the dot
	 * clock, do so. Otherwise route DU_DOTCLKINn signal to DUn.
	 *
	 * Each channel can then select between the dot clock configured here
	 * and the clock provided by the CPG through the ESCR register.
	 */
	if (rcdu->info->gen < 3 && rgrp->index == 0) {
		/*
		 * On Gen2 a single register in the first group controls dot
		 * clock selection for all channels.
		 */
		rcrtc = rcdu->crtcs;
		num_crtcs = rcdu->num_crtcs;
	} else if (rcdu->info->gen == 3 && rgrp->num_crtcs > 1) {
		/*
		 * On Gen3 dot clocks are setup through per-group registers,
		 * only available when the group has two channels.
		 */
		rcrtc = &rcdu->crtcs[rgrp->index * 2];
		num_crtcs = rgrp->num_crtcs;
	}

	if (!num_crtcs)
		return;

	didsr = DIDSR_CODE;
	for (i = 0; i < num_crtcs; ++i, ++rcrtc) {
		if (rcdu->info->lvds_clk_mask & BIT(rcrtc->index))
			didsr |= DIDSR_LCDS_LVDS0(i)
			      |  DIDSR_PDCS_CLK(i, 0);
		else
			didsr |= DIDSR_LCDS_DCLKIN(i)
			      |  DIDSR_PDCS_CLK(i, 0);
	}

	rcar_du_group_write(rgrp, DIDSR, didsr);
}

static void rcar_du_group_setup(struct rcar_du_group *rgrp)
{
	struct rcar_du_device *rcdu = rgrp->dev;

	/* Enable extended features */
	rcar_du_group_write(rgrp, DEFR, DEFR_CODE | DEFR_DEFE);
	if (rcdu->info->gen < 3) {
		rcar_du_group_write(rgrp, DEFR2, DEFR2_CODE | DEFR2_DEFE2G);
		rcar_du_group_write(rgrp, DEFR3, DEFR3_CODE | DEFR3_DEFE3);
		rcar_du_group_write(rgrp, DEFR4, DEFR4_CODE);
	}
	rcar_du_group_write(rgrp, DEFR5, DEFR5_CODE | DEFR5_DEFE5);

	rcar_du_group_setup_pins(rgrp);

	if (rcdu->info->gen >= 2) {
		rcar_du_group_setup_defr8(rgrp);
		rcar_du_group_setup_didsr(rgrp);
	}

	if (rcdu->info->gen >= 3)
		rcar_du_group_write(rgrp, DEFR10, DEFR10_CODE | DEFR10_DEFE10);

	/*
	 * Use DS1PR and DS2PR to configure planes priorities and connects the
	 * superposition 0 to DU0 pins. DU1 pins will be configured dynamically.
	 */
	rcar_du_group_write(rgrp, DORCR, DORCR_PG1D_DS1 | DORCR_DPRS);

	/* Apply planes to CRTCs association. */
	mutex_lock(&rgrp->lock);
	rcar_du_group_write(rgrp, DPTSR, (rgrp->dptsr_planes << 16) |
			    rgrp->dptsr_planes);
	mutex_unlock(&rgrp->lock);
}

static void __rcar_du_group_start_stop(struct rcar_du_group *rgrp, bool start)
{
	struct rcar_du_device *rcdu = rgrp->dev;

	/*
	 * Group start/stop is controlled by the DRES and DEN bits of DSYSR0
	 * for the first group and DSYSR2 for the second group. On most DU
	 * instances, this maps to the first CRTC of the group, and we can just
	 * use rcar_du_crtc_dsysr_clr_set() to access the correct DSYSR. On
	 * M3-N, however, DU2 doesn't exist, but DSYSR2 does. We thus need to
	 * access the register directly using group read/write.
	 */
	if (rcdu->info->channels_mask & BIT(rgrp->index * 2)) {
		struct rcar_du_crtc *rcrtc = &rgrp->dev->crtcs[rgrp->index * 2];

		rcar_du_crtc_dsysr_clr_set(rcrtc, DSYSR_DRES | DSYSR_DEN,
					   start ? DSYSR_DEN : DSYSR_DRES);
	} else {
		rcar_du_group_write(rgrp, DSYSR,
				    start ? DSYSR_DEN : DSYSR_DRES);
	}
}

void rcar_du_group_start_stop(struct rcar_du_group *rgrp, bool start)
{
	/*
	 * Many of the configuration bits are only updated when the display
	 * reset (DRES) bit in DSYSR is set to 1, disabling *both* CRTCs. Some
	 * of those bits could be pre-configured, but others (especially the
	 * bits related to plane assignment to display timing controllers) need
	 * to be modified at runtime.
	 *
	 * Restart the display controller if a start is requested. Sorry for the
	 * flicker. It should be possible to move most of the "DRES-update" bits
	 * setup to driver initialization time and minimize the number of cases
	 * when the display controller will have to be restarted.
	 */
	if (start) {
		if (rgrp->used_crtcs++ != 0)
			__rcar_du_group_start_stop(rgrp, false);
		__rcar_du_group_start_stop(rgrp, true);
	} else {
		if (--rgrp->used_crtcs == 0)
			__rcar_du_group_start_stop(rgrp, false);
	}
}

void rcar_du_group_restart(struct rcar_du_group *rgrp)
{
	rgrp->need_restart = false;

	__rcar_du_group_start_stop(rgrp, false);
	__rcar_du_group_start_stop(rgrp, true);
}

int rcar_du_set_dpad0_vsp1_routing(struct rcar_du_device *rcdu)
{
	struct rcar_du_group *rgrp;
	struct rcar_du_crtc *crtc;
	unsigned int index;
	int ret;

	if (rcdu->info->gen < 2)
		return 0;

	/*
	 * RGB output routing to DPAD0 and VSP1D routing to DU0/1/2 are
	 * configured in the DEFR8 register of the first group on Gen2 and the
	 * last group on Gen3. As this function can be called with the DU
	 * channels of the corresponding CRTCs disabled, we need to enable the
	 * group clock before accessing the register.
	 */
	index = rcdu->info->gen < 3 ? 0 : DIV_ROUND_UP(rcdu->num_crtcs, 2) - 1;
	rgrp = &rcdu->groups[index];
	crtc = &rcdu->crtcs[index * 2];

	ret = clk_prepare_enable(crtc->clock);
	if (ret < 0)
		return ret;

	rcar_du_group_setup_defr8(rgrp);

	clk_disable_unprepare(crtc->clock);

	return 0;
}

static void rcar_du_group_set_dpad_levels(struct rcar_du_group *rgrp)
{
	static const u32 doflr_values[2] = {
		DOFLR_HSYCFL0 | DOFLR_VSYCFL0 | DOFLR_ODDFL0 |
		DOFLR_DISPFL0 | DOFLR_CDEFL0  | DOFLR_RGBFL0,
		DOFLR_HSYCFL1 | DOFLR_VSYCFL1 | DOFLR_ODDFL1 |
		DOFLR_DISPFL1 | DOFLR_CDEFL1  | DOFLR_RGBFL1,
	};
	static const u32 dpad_mask = BIT(RCAR_DU_OUTPUT_DPAD1)
				   | BIT(RCAR_DU_OUTPUT_DPAD0);
	struct rcar_du_device *rcdu = rgrp->dev;
	u32 doflr = DOFLR_CODE;
	unsigned int i;

	if (rcdu->info->gen < 2)
		return;

	/*
	 * The DPAD outputs can't be controlled directly. However, the parallel
	 * output of the DU channels routed to DPAD can be set to fixed levels
	 * through the DOFLR group register. Use this to turn the DPAD on or off
	 * by driving fixed low-level signals at the output of any DU channel
	 * not routed to a DPAD output. This doesn't affect the DU output
	 * signals going to other outputs, such as the internal LVDS and HDMI
	 * encoders.
	 */

	for (i = 0; i < rgrp->num_crtcs; ++i) {
		struct rcar_du_crtc_state *rstate;
		struct rcar_du_crtc *rcrtc;

		rcrtc = &rcdu->crtcs[rgrp->index * 2 + i];
		rstate = to_rcar_crtc_state(rcrtc->crtc.state);

		if (!(rstate->outputs & dpad_mask))
			doflr |= doflr_values[i];
	}

	rcar_du_group_write(rgrp, DOFLR, doflr);
}

int rcar_du_group_set_routing(struct rcar_du_group *rgrp)
{
	struct rcar_du_device *rcdu = rgrp->dev;
	u32 dorcr = rcar_du_group_read(rgrp, DORCR);

	dorcr &= ~(DORCR_PG2T | DORCR_DK2S | DORCR_PG2D_MASK);

	/*
	 * Set the DPAD1 pins sources. Select CRTC 0 if explicitly requested and
	 * CRTC 1 in all other cases to avoid cloning CRTC 0 to DPAD0 and DPAD1
	 * by default.
	 */
	if (rcdu->dpad1_source == rgrp->index * 2)
		dorcr |= DORCR_PG2D_DS1;
	else
		dorcr |= DORCR_PG2T | DORCR_DK2S | DORCR_PG2D_DS2;

	rcar_du_group_write(rgrp, DORCR, dorcr);

	rcar_du_group_set_dpad_levels(rgrp);

	return rcar_du_set_dpad0_vsp1_routing(rgrp->dev);
}

static struct drm_private_state *
rcar_du_group_atomic_duplicate_state(struct drm_private_obj *obj)
{
	struct rcar_du_group_state *state;

	if (WARN_ON(!obj->state))
		return NULL;

	state = kzalloc(sizeof(*state), GFP_KERNEL);
	if (state == NULL)
		return NULL;

	__drm_atomic_helper_private_obj_duplicate_state(obj, &state->state);

	return &state->state;
}

static void rcar_du_group_atomic_destroy_state(struct drm_private_obj *obj,
					       struct drm_private_state *state)
{
	kfree(to_rcar_group_state(state));
}

static const struct drm_private_state_funcs rcar_du_group_state_funcs = {
	.atomic_duplicate_state = rcar_du_group_atomic_duplicate_state,
	.atomic_destroy_state = rcar_du_group_atomic_destroy_state,
};

#define for_each_oldnew_group_in_state(__state, __obj, __old_state, __new_state, __i) \
	for_each_oldnew_private_obj_in_state((__state), (__obj), (__old_state), (__new_state), (__i)) \
		for_each_if((__obj)->funcs == &rcar_du_group_state_funcs)

static struct rcar_du_group_state *
rcar_du_get_group_state(struct drm_atomic_state *state,
			struct rcar_du_group *rgrp)
{
	struct drm_private_state *pstate;

	pstate = drm_atomic_get_private_obj_state(state, &rgrp->private);
	if (IS_ERR(pstate))
		return ERR_CAST(pstate);

	return to_rcar_group_state(pstate);
}

int rcar_du_group_atomic_check(struct drm_device *dev,
			       struct drm_atomic_state *state)
{
	struct drm_crtc *crtc;
	struct drm_crtc_state *crtc_state;
	unsigned int i;

	for_each_new_crtc_in_state(state, crtc, crtc_state, i) {
		struct rcar_du_crtc *rcrtc = to_rcar_crtc(crtc);
		struct rcar_du_group_state *rstate;

		if (crtc_state->active_changed || crtc_state->mode_changed) {
			rstate = rcar_du_get_group_state(state, rcrtc->group);
			if (IS_ERR(rstate))
				return PTR_ERR(rstate);

			if (crtc_state->active)
				rstate->use_count++;
		}
	}

	return 0;
}

int rcar_du_group_atomic_pre_commit(struct drm_device *dev,
				    struct drm_atomic_state *state)
{
	struct drm_private_state *old_pstate, *new_pstate;
	struct drm_private_obj *obj;
	unsigned int i;

	for_each_oldnew_group_in_state(state, obj, old_pstate, new_pstate, i) {
		struct rcar_du_group *rgrp = to_rcar_group(obj);
		struct rcar_du_group_state *old_state, *new_state;

		old_state = to_rcar_group_state(old_pstate);
		new_state = to_rcar_group_state(new_pstate);

		if (!old_state->use_count && new_state->use_count)
			rcar_du_group_setup(rgrp);
	}

	return 0;
}

int rcar_du_group_atomic_post_commit(struct drm_device *dev,
				     struct drm_atomic_state *state)
{
	return 0;
}

/*
 * rcar_du_group_init - Initialise and reset a group object
 *
 * Return 0 in case of success or a negative error code otherwise.
 */
int rcar_du_group_init(struct rcar_du_device *rcdu, struct rcar_du_group *rgrp,
		       unsigned int index)
{
	static const unsigned int mmio_offsets[] = {
		DU0_REG_OFFSET, DU2_REG_OFFSET
	};

	struct rcar_du_group_state *state;

	state = kzalloc(sizeof(*state), GFP_KERNEL);
	if (!state)
		return -ENOMEM;

	drm_atomic_private_obj_init(rcdu->ddev, &rgrp->private, &state->state,
				    &rcar_du_group_state_funcs);

	mutex_init(&rgrp->lock);

	rgrp->dev = rcdu;
	rgrp->mmio_offset = mmio_offsets[index];
	rgrp->index = index;
	/* Extract the channel mask for this group only. */
	rgrp->channels_mask = (rcdu->info->channels_mask >> (2 * index))
			    & GENMASK(1, 0);
	rgrp->num_crtcs = hweight8(rgrp->channels_mask);

	/*
	 * If we have more than one CRTC in this group pre-associate
	 * the low-order planes with CRTC 0 and the high-order planes
	 * with CRTC 1 to minimize flicker occurring when the
	 * association is changed.
	 */
	rgrp->dptsr_planes = rgrp->num_crtcs > 1
			   ? (rcdu->info->gen >= 3 ? 0x04 : 0xf0)
			   : 0;

	return 0;
}

void rcar_du_group_cleanup(struct rcar_du_group *rgrp)
{
	drm_atomic_private_obj_fini(&rgrp->private);
}
