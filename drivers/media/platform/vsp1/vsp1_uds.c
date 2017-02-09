// SPDX-License-Identifier: GPL-2.0+
/*
 * vsp1_uds.c  --  R-Car VSP1 Up and Down Scaler
 *
 * Copyright (C) 2013-2014 Renesas Electronics Corporation
 *
 * Contact: Laurent Pinchart (laurent.pinchart@ideasonboard.com)
 */

#include <linux/device.h>
#include <linux/gfp.h>

#include <media/v4l2-subdev.h>

#include "vsp1.h"
#include "vsp1_dl.h"
#include "vsp1_pipe.h"
#include "vsp1_uds.h"

#define UDS_MIN_SIZE				4U
#define UDS_MAX_SIZE				8190U

#define UDS_MIN_FACTOR				0x0100
#define UDS_MAX_FACTOR				0xffff

/* -----------------------------------------------------------------------------
 * Device Access
 */

static inline void vsp1_uds_write(struct vsp1_uds *uds,
				  struct vsp1_dl_body *dlb, u32 reg, u32 data)
{
	vsp1_dl_body_write(dlb, reg + uds->entity.index * VI6_UDS_OFFSET, data);
}

/* -----------------------------------------------------------------------------
 * Scaling Computation
 */

void vsp1_uds_set_alpha(struct vsp1_entity *entity, struct vsp1_dl_body *dlb,
			unsigned int alpha)
{
	struct vsp1_uds *uds = to_uds(&entity->subdev);

	vsp1_uds_write(uds, dlb, VI6_UDS_ALPVAL,
		       alpha << VI6_UDS_ALPVAL_VAL0_SHIFT);
}

/*
 * Determine the pre-filter binning divider.
 *
 * The UDS uses a two stage filter scaler process. This determines the
 * rate at which pixels are reduced for large down-scaling ratios before
 * being fed into the bicubic filter.
 *
 * This calculation assumes that the BLADV bit is unset.
 */
static unsigned int uds_pre_scaling_divisor(int ratio)
{
	unsigned int mp = ratio / 4096;

	return mp < 4 ? 1 : (mp < 8 ? 2 : 4);
}

/*
 * uds_output_size - Return the output size for an input size and scaling ratio
 * @input: input size in pixels
 * @ratio: scaling ratio in U4.12 fixed-point format
 */
static unsigned int uds_output_size(unsigned int input, unsigned int ratio)
{
	if (ratio > 4096) {
		/* Down-scaling */
		unsigned int mp = uds_pre_scaling_divisor(ratio);

		return (input - 1) / mp * mp * 4096 / ratio + 1;
	} else {
		/* Up-scaling */
		return (input - 1) * 4096 / ratio + 1;
	}
}

/*
 * uds_output_limits - Return the min and max output sizes for an input size
 * @input: input size in pixels
 * @minimum: minimum output size (returned)
 * @maximum: maximum output size (returned)
 */
static void uds_output_limits(unsigned int input,
			      unsigned int *minimum, unsigned int *maximum)
{
	*minimum = max(uds_output_size(input, UDS_MAX_FACTOR), UDS_MIN_SIZE);
	*maximum = min(uds_output_size(input, UDS_MIN_FACTOR), UDS_MAX_SIZE);
}

/*
 * uds_passband_width - Return the passband filter width for a scaling ratio
 * @ratio: scaling ratio in U4.12 fixed-point format
 */
static unsigned int uds_passband_width(unsigned int ratio)
{
	if (ratio >= 4096) {
		/* Down-scaling */
		unsigned int mp = uds_pre_scaling_divisor(ratio);

		return 64 * 4096 * mp / ratio;
	} else {
		/* Up-scaling */
		return 64;
	}
}

static unsigned int uds_compute_ratio(unsigned int input, unsigned int output)
{
	/* TODO: This is an approximation that will need to be refined. */
	return (input - 1) * 4096 / (output - 1);
}

/* -----------------------------------------------------------------------------
 * V4L2 Subdevice Pad Operations
 */

static int uds_enum_mbus_code(struct v4l2_subdev *subdev,
			      struct v4l2_subdev_pad_config *cfg,
			      struct v4l2_subdev_mbus_code_enum *code)
{
	static const unsigned int codes[] = {
		MEDIA_BUS_FMT_ARGB8888_1X32,
		MEDIA_BUS_FMT_AYUV8_1X32,
	};

	return vsp1_subdev_enum_mbus_code(subdev, cfg, code, codes,
					  ARRAY_SIZE(codes));
}

static int uds_enum_frame_size(struct v4l2_subdev *subdev,
			       struct v4l2_subdev_pad_config *cfg,
			       struct v4l2_subdev_frame_size_enum *fse)
{
	struct vsp1_uds *uds = to_uds(subdev);
	struct v4l2_subdev_pad_config *config;
	struct v4l2_mbus_framefmt *format;
	int ret = 0;

	config = vsp1_entity_get_pad_config(&uds->entity, cfg, fse->which);
	if (!config)
		return -EINVAL;

	format = vsp1_entity_get_pad_format(&uds->entity, config,
					    UDS_PAD_SINK);

	mutex_lock(&uds->entity.lock);

	if (fse->index || fse->code != format->code) {
		ret = -EINVAL;
		goto done;
	}

	if (fse->pad == UDS_PAD_SINK) {
		fse->min_width = UDS_MIN_SIZE;
		fse->max_width = UDS_MAX_SIZE;
		fse->min_height = UDS_MIN_SIZE;
		fse->max_height = UDS_MAX_SIZE;
	} else {
		uds_output_limits(format->width, &fse->min_width,
				  &fse->max_width);
		uds_output_limits(format->height, &fse->min_height,
				  &fse->max_height);
	}

done:
	mutex_unlock(&uds->entity.lock);
	return ret;
}

static void uds_try_format(struct vsp1_uds *uds,
			   struct v4l2_subdev_pad_config *config,
			   unsigned int pad, struct v4l2_mbus_framefmt *fmt)
{
	struct v4l2_mbus_framefmt *format;
	unsigned int minimum;
	unsigned int maximum;

	switch (pad) {
	case UDS_PAD_SINK:
		/* Default to YUV if the requested format is not supported. */
		if (fmt->code != MEDIA_BUS_FMT_ARGB8888_1X32 &&
		    fmt->code != MEDIA_BUS_FMT_AYUV8_1X32)
			fmt->code = MEDIA_BUS_FMT_AYUV8_1X32;

		fmt->width = clamp(fmt->width, UDS_MIN_SIZE, UDS_MAX_SIZE);
		fmt->height = clamp(fmt->height, UDS_MIN_SIZE, UDS_MAX_SIZE);
		break;

	case UDS_PAD_SOURCE:
		/* The UDS scales but can't perform format conversion. */
		format = vsp1_entity_get_pad_format(&uds->entity, config,
						    UDS_PAD_SINK);
		fmt->code = format->code;

		uds_output_limits(format->width, &minimum, &maximum);
		fmt->width = clamp(fmt->width, minimum, maximum);
		uds_output_limits(format->height, &minimum, &maximum);
		fmt->height = clamp(fmt->height, minimum, maximum);
		break;
	}

	fmt->field = V4L2_FIELD_NONE;
	fmt->colorspace = V4L2_COLORSPACE_SRGB;
}

static int uds_set_format(struct v4l2_subdev *subdev,
			  struct v4l2_subdev_pad_config *cfg,
			  struct v4l2_subdev_format *fmt)
{
	struct vsp1_uds *uds = to_uds(subdev);
	struct v4l2_subdev_pad_config *config;
	struct v4l2_mbus_framefmt *format;
	int ret = 0;

	mutex_lock(&uds->entity.lock);

	config = vsp1_entity_get_pad_config(&uds->entity, cfg, fmt->which);
	if (!config) {
		ret = -EINVAL;
		goto done;
	}

	uds_try_format(uds, config, fmt->pad, &fmt->format);

	format = vsp1_entity_get_pad_format(&uds->entity, config, fmt->pad);
	*format = fmt->format;

	if (fmt->pad == UDS_PAD_SINK) {
		/* Propagate the format to the source pad. */
		format = vsp1_entity_get_pad_format(&uds->entity, config,
						    UDS_PAD_SOURCE);
		*format = fmt->format;

		uds_try_format(uds, config, UDS_PAD_SOURCE, format);
	}

done:
	mutex_unlock(&uds->entity.lock);
	return ret;
}

/* -----------------------------------------------------------------------------
 * V4L2 Subdevice Operations
 */

static const struct v4l2_subdev_pad_ops uds_pad_ops = {
	.init_cfg = vsp1_entity_init_cfg,
	.enum_mbus_code = uds_enum_mbus_code,
	.enum_frame_size = uds_enum_frame_size,
	.get_fmt = vsp1_subdev_get_pad_format,
	.set_fmt = uds_set_format,
};

static const struct v4l2_subdev_ops uds_ops = {
	.pad    = &uds_pad_ops,
};

/* -----------------------------------------------------------------------------
 * VSP1 Entity Operations
 */

static void uds_configure_stream(struct vsp1_entity *entity,
				 struct vsp1_pipeline *pipe,
				 struct vsp1_dl_list *dl,
				 struct vsp1_dl_body *dlb)
{
	struct vsp1_uds *uds = to_uds(&entity->subdev);
	const struct v4l2_mbus_framefmt *output;
	const struct v4l2_mbus_framefmt *input;
	unsigned int hscale;
	unsigned int vscale;
	bool manual_phase = vsp1_pipeline_partitioned(pipe);
	bool multitap;

	input = vsp1_entity_get_pad_format(&uds->entity, uds->entity.config,
					   UDS_PAD_SINK);
	output = vsp1_entity_get_pad_format(&uds->entity, uds->entity.config,
					    UDS_PAD_SOURCE);

	hscale = uds_compute_ratio(input->width, output->width);
	vscale = uds_compute_ratio(input->height, output->height);

	dev_dbg(uds->entity.vsp1->dev, "hscale %u vscale %u\n", hscale, vscale);

	/*
	 * Multi-tap scaling can't be enabled along with alpha scaling when
	 * scaling down with a factor lower than or equal to 1/2 in either
	 * direction.
	 */
	if (uds->scale_alpha && (hscale >= 8192 || vscale >= 8192))
		multitap = false;
	else
		multitap = true;

	vsp1_uds_write(uds, dlb, VI6_UDS_CTRL,
		       (uds->scale_alpha ? VI6_UDS_CTRL_AON : 0) |
		       (multitap ? VI6_UDS_CTRL_BC : 0) |
		       (manual_phase ? VI6_UDS_CTRL_AMDSLH : 0));

	vsp1_uds_write(uds, dlb, VI6_UDS_PASS_BWIDTH,
		       (uds_passband_width(hscale)
				<< VI6_UDS_PASS_BWIDTH_H_SHIFT) |
		       (uds_passband_width(vscale)
				<< VI6_UDS_PASS_BWIDTH_V_SHIFT));

	/* Set the scaling ratios. */
	vsp1_uds_write(uds, dlb, VI6_UDS_SCALE,
		       (hscale << VI6_UDS_SCALE_HFRAC_SHIFT) |
		       (vscale << VI6_UDS_SCALE_VFRAC_SHIFT));
}

static void uds_configure_partition(struct vsp1_entity *entity,
				    struct vsp1_pipeline *pipe,
				    struct vsp1_dl_list *dl,
				    struct vsp1_dl_body *dlb)
{
	struct vsp1_uds *uds = to_uds(&entity->subdev);
	struct vsp1_partition *partition = pipe->partition;
	const struct v4l2_mbus_framefmt *output;

	output = vsp1_entity_get_pad_format(&uds->entity, uds->entity.config,
					    UDS_PAD_SOURCE);

	/* Input size clipping. */
	vsp1_uds_write(uds, dlb, VI6_UDS_HSZCLIP, VI6_UDS_HSZCLIP_HCEN |
		       (partition->uds_sink.offset
				<< VI6_UDS_HSZCLIP_HCL_OFST_SHIFT) |
		       (partition->uds_sink.width
				<< VI6_UDS_HSZCLIP_HCL_SIZE_SHIFT));

	/* Output size clipping. */
	vsp1_uds_write(uds, dlb, VI6_UDS_CLIP_SIZE,
		       (partition->uds_source.width
				<< VI6_UDS_CLIP_SIZE_HSIZE_SHIFT) |
		       (output->height
				<< VI6_UDS_CLIP_SIZE_VSIZE_SHIFT));

	vsp1_uds_write(uds, dlb, VI6_UDS_HPHASE,
		       (partition->start_phase
				<< VI6_UDS_HPHASE_HSTP_SHIFT) |
		       (partition->end_phase
				<< VI6_UDS_HPHASE_HEDP_SHIFT));
}

static unsigned int uds_max_width(struct vsp1_entity *entity,
				  struct vsp1_pipeline *pipe)
{
	struct vsp1_uds *uds = to_uds(&entity->subdev);
	const struct v4l2_mbus_framefmt *output;
	const struct v4l2_mbus_framefmt *input;
	unsigned int hscale;

	input = vsp1_entity_get_pad_format(&uds->entity, uds->entity.config,
					   UDS_PAD_SINK);
	output = vsp1_entity_get_pad_format(&uds->entity, uds->entity.config,
					    UDS_PAD_SOURCE);
	hscale = output->width / input->width;

	/*
	 * The maximum width of the UDS is 304 pixels. These are input pixels
	 * in the event of up-scaling, and output pixels in the event of
	 * downscaling.
	 *
	 * To support overlapping partition windows we clamp at units of 256 and
	 * the remaining pixels are reserved.
	 */
	if (hscale <= 2)
		return 256;
	else if (hscale <= 4)
		return 512;
	else if (hscale <= 8)
		return 1024;
	else
		return 2048;
}

/* -----------------------------------------------------------------------------
 * Partition Algorithm Support
 */

static void uds_partition(struct vsp1_entity *entity,
			  struct vsp1_pipeline *pipe,
			  struct vsp1_partition *partition,
			  unsigned int partition_idx,
			  struct vsp1_partition_window *window,
			  bool forwards)
{
	struct vsp1_uds *uds = to_uds(&entity->subdev);
	const struct v4l2_mbus_framefmt *output;
	const struct v4l2_mbus_framefmt *input;
	unsigned int hscale;
	unsigned int margin;
	unsigned int mp;
	unsigned int src_left;
	unsigned int src_right;
	unsigned int dst_left;
	unsigned int dst_right;
	unsigned int remainder;

	if (forwards) {
		/* Accept and consume any input offset (clipping) request. */
		partition->uds_sink.offset = window->offset;
		window->offset = 0;

		/* pass on the output we computed in the backwards pass. */
		*window = partition->uds_source;

		return;
	}

	input = vsp1_entity_get_pad_format(&uds->entity, uds->entity.config,
					   UDS_PAD_SINK);
	output = vsp1_entity_get_pad_format(&uds->entity, uds->entity.config,
					    UDS_PAD_SOURCE);

	/*
	 * Quantify the margin required for discontinuous overlap, and expand
	 * the window no further than the limits of the image.
	 *
	 * When downscaling, the UDS has a pre-scaling binning filter which
	 * reduces the input by either 2 or 4 depending on the downscale ratio,
	 * which we must align with. The output is then fed into a 5 tap
	 * bi-cubic filter requiring us to expand our output window by 2 pixels
	 * on each side.
	 *
	 * When upscaling the extra margin is required from the input side and
	 * we round up the expansion to a power of 2.
	 */
	hscale = uds_compute_ratio(input->width, output->width);
	margin = hscale < 0x0200 ? 32 : /* 8 <  scale */
		 hscale < 0x0400 ? 16 : /* 4 <  scale <= 8 */
		 hscale < 0x0800 ?  8 : /* 2 <  scale <= 4 */
		 hscale < 0x1000 ?  4 : /* 1 <  scale <= 2 */
				    2;  /*	scale <= 1 (downscaling) */

	dst_left = max_t(int, 0, window->left - margin);
	dst_right = min_t(int, output->width - 1,
			  window->left + window->width - 1 + margin);

	/*
	 * We must correctly identify the input positions which represent the
	 * output positions taking into account all of the processing the
	 * hardware does. This requires several operations (Steps 1-5 below).
	 *
	 * First convert output positions to input positions, then convert
	 * back to output positions to account for any rounding requirements.
	 * Once adjusted, the final input positions are determined.
	 */

	mp = uds_pre_scaling_divisor(hscale);

	/* Step 1: Calculate temporary src_left (src_pos0) position. */
	/* Step 2: Calculate temporary dst_left (dst_pos0_pb) position. */
	if (partition_idx == 0) {
		src_left = 0;
		dst_left = 0;
	} else {
		unsigned int sub_src = mp == 1 ? 1 : 2;

		src_left = ((dst_left * hscale) / (4096 * mp) - sub_src) * mp;
		dst_left = src_left * 4096 / hscale;
	}

	/*
	 * Step 3: Pull back dst_left (dst_pos0_pb) to the left to satisfy
	 * alignment restrictions (the input of the bicubic filter must be
	 * aligned to a multiple of the pre-filter divisor).
	 */
	remainder = (dst_left * hscale) % mp;
	if (remainder)
		/* Remainder can only be two when mp == 4. */
		dst_left = round_down(dst_left, hscale % mp == 2 ? 2 : mp);

	/* Step 4: Recalculate src_left from newly adjusted dst_left. */
	src_left = DIV_ROUND_UP(dst_left * hscale, 4096 * mp) * mp;

	/* Step 5: Calculate src_right (src_pos1). */
	if (partition_idx == pipe->partitions - 1)
		src_right = input->width - 1;
	else
		src_right = (((dst_right * hscale) / (mp * 4096)) + 2)
			  * mp + (mp / 2);

	/* Step 6: Calculate start phase (hstp). */
	remainder = (src_left * hscale) % (mp * 4096);
	partition->start_phase = (4096 - remainder / mp) % 4096;

	/*
	 * Step 7: Calculate end phase (hedp).
	 *
	 * We do not currently use VI6_UDS_CTRL_AMD from VI6_UDS_CTRL.
	 * In the event that we enable VI6_UDS_CTRL_AMD, we must set the end
	 * phase for the final partition to the phase_edge.
	 */
	partition->end_phase = 0;

	/*
	 * Fill in our partition windows with the updated positions, and
	 * configure our output offset to allow extraneous pixels to be
	 * clipped by later entities.
	 */
	partition->uds_sink.left = src_left;
	partition->uds_sink.width = src_right - src_left + 1;

	partition->uds_source.left = dst_left;
	partition->uds_source.width = dst_right - dst_left + 1;

	partition->uds_source.offset = window->left - dst_left;

	/* Pass a copy of our sink down to the previous entity. */
	*window = partition->uds_sink;
}

static const struct vsp1_entity_operations uds_entity_ops = {
	.configure_stream = uds_configure_stream,
	.configure_partition = uds_configure_partition,
	.max_width = uds_max_width,
	.partition = uds_partition,
};

/* -----------------------------------------------------------------------------
 * Initialization and Cleanup
 */

struct vsp1_uds *vsp1_uds_create(struct vsp1_device *vsp1, unsigned int index)
{
	struct vsp1_uds *uds;
	char name[6];
	int ret;

	uds = devm_kzalloc(vsp1->dev, sizeof(*uds), GFP_KERNEL);
	if (uds == NULL)
		return ERR_PTR(-ENOMEM);

	uds->entity.ops = &uds_entity_ops;
	uds->entity.type = VSP1_ENTITY_UDS;
	uds->entity.index = index;

	sprintf(name, "uds.%u", index);
	ret = vsp1_entity_init(vsp1, &uds->entity, name, 2, &uds_ops,
			       MEDIA_ENT_F_PROC_VIDEO_SCALER);
	if (ret < 0)
		return ERR_PTR(ret);

	return uds;
}
