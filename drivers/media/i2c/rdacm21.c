// SPDX-License-Identifier: GPL-2.0+
/*
 * IMI RDACM21 GMSL Camera Driver
 *
 * Copyright (C) 2017-2019 Jacopo Mondi
 * Copyright (C) 2017-2019 Kieran Bingham
 * Copyright (C) 2017-2019 Laurent Pinchart
 * Copyright (C) 2017-2019 Niklas Söderlund
 * Copyright (C) 2016 Renesas Electronics Corporation
 * Copyright (C) 2015 Cogent Embedded, Inc.
 */

#include <linux/delay.h>
#include <linux/fwnode.h>
#include <linux/init.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/videodev2.h>

#include <media/v4l2-async.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-subdev.h>
#include "max9271.h"

#define OV10640_I2C_ADDRESS		0x30
#define OV490_I2C_ADDRESS		0x24

#define OV490_ISP_HSIZE_LOW		0x60
#define OV490_ISP_HSIZE_HIGH		0x61
#define OV490_ISP_VSIZE_LOW		0x62
#define OV490_ISP_VSIZE_HIGH		0x63

#define OV490_PID			0x300a
#define OV490_VER			0x300b
#define OV490_ID_VAL			0x0490
#define OV490_ID(_p, _v)		((((_p) & 0xff) << 8) | ((_v) & 0xff))

struct rdacm21_device {
	struct device			*dev;
	struct max9271_device		*serializer;
	struct i2c_client		*isp;
	struct v4l2_subdev		sd;
	struct media_pad		pad;
	struct v4l2_mbus_framefmt	fmt;
	u32				addrs[32];
};

static inline struct rdacm21_device *sd_to_rdacm21(struct v4l2_subdev *sd)
{
	return container_of(sd, struct rdacm21_device, sd);
}

static inline struct rdacm21_device *i2c_to_rdacm21(struct i2c_client *client)
{
	return sd_to_rdacm21(i2c_get_clientdata(client));
}

static const struct ov490_reg {
	u16 reg;
	u8 val;
} ov490_regs_wizard[] = {
	{0xfffd, 0x80},
	{0xfffe, 0x82},
	{0x0071, 0x11},
	{0x0075, 0x11},
	{0xfffd, 0x80},
	{0xfffe, 0x29},
	{0x6010, 0x01},
	{0x4017, 0x00},
	{0xfffd, 0x80},
	{0xfffe, 0x28},
	{0x6000, 0x04},
	{0x6004, 0x00},
	{0x6008, 0x01},
	{0xfffd, 0x80},
	{0xfffe, 0x80},
	{0x0091, 0x00},
	{0x00bb, 0x1d},
};

static int ov490_read(struct rdacm21_device *dev, u16 reg, u16 *val)
{
	u8 buf[2] = { reg >> 8, reg & 0xff };
	int ret;

	ret = i2c_master_send(dev->isp, buf, 2);
	if (ret == 2)
		ret = i2c_master_recv(dev->isp, buf, 2);

	if (ret < 0) {
		dev_dbg(dev->dev, "%s: register 0x%04x read failed (%d)\n",
			__func__, reg, ret);
		return ret;
	}

	*val = (buf[0] << 8) | buf[1];

	return 0;
}

static int ov490_write(struct rdacm21_device *dev, u16 reg, u8 val)
{
	u8 buf[3] = { reg >> 8, reg & 0xff, val };
	int ret;

	dev_dbg(dev->dev, "%s: 0x%04x = 0x%02x\n", __func__, reg, val);

	ret = i2c_master_send(dev->isp, buf, 3);
	if (ret < 0) {
		dev_err(dev->dev, "%s: register 0x%04x write failed (%d)\n",
			__func__, reg, ret);
		return ret;
	}

	return 0;
}

static int rdacm21_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct rdacm21_device *dev = sd_to_rdacm21(sd);

	return max9271_s_stream(dev->serializer, enable);
}

static int rdacm21_enum_mbus_code(struct v4l2_subdev *sd,
				  struct v4l2_subdev_pad_config *cfg,
				  struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->pad || code->index > 0)
		return -EINVAL;

	code->code = MEDIA_BUS_FMT_YUYV8_2X8;

	return 0;
}

static int rdacm21_get_fmt(struct v4l2_subdev *sd,
			   struct v4l2_subdev_pad_config *cfg,
			   struct v4l2_subdev_format *format)
{
	struct v4l2_mbus_framefmt *mf = &format->format;
	struct rdacm21_device *dev = sd_to_rdacm21(sd);

	if (format->pad)
		return -EINVAL;

	mf->width		= dev->fmt.width;
	mf->height		= dev->fmt.height;
	mf->code		= MEDIA_BUS_FMT_YUYV8_2X8;
	mf->colorspace		= V4L2_COLORSPACE_SRGB;
	mf->field		= V4L2_FIELD_NONE;
	mf->ycbcr_enc		= V4L2_YCBCR_ENC_601;
	mf->quantization	= V4L2_QUANTIZATION_FULL_RANGE;
	mf->xfer_func		= V4L2_XFER_FUNC_NONE;

	return 0;
}

static struct v4l2_subdev_video_ops rdacm21_video_ops = {
	.s_stream	= rdacm21_s_stream,
};

static const struct v4l2_subdev_pad_ops rdacm21_subdev_pad_ops = {
	.enum_mbus_code = rdacm21_enum_mbus_code,
	.get_fmt	= rdacm21_get_fmt,
	.set_fmt	= rdacm21_get_fmt,
};

static struct v4l2_subdev_ops rdacm21_subdev_ops = {
	.video		= &rdacm21_video_ops,
	.pad		= &rdacm21_subdev_pad_ops,
};

static int ov490_initialize(struct rdacm21_device *dev)
{
	u16 pid, ver, val;
	unsigned int i;
	int ret;

	dev->isp->addr = OV490_I2C_ADDRESS;

	ret = ov490_write(dev, 0xfffd, 0x80);
	if (ret)
		return ret;

	ret = ov490_write(dev, 0xfffe, 0x80);
	if (ret)
		return ret;

	ret = ov490_read(dev, OV490_PID, &pid);
	if (ret < 0) {
		dev_err(dev->dev, "OV490 PID read failed (%d)\n", ret);
		return ret;
	}

	ret = ov490_read(dev, OV490_VER, &ver);
	if (ret < 0) {
		dev_err(dev->dev, "OV490 VERSION read failed (%d)\n", ret);
		return ret;
	}

	if (OV490_ID(pid, ver) != OV490_ID_VAL) {
		dev_err(dev->dev, "OV490 ID mismatch: (0x%04x)\n",
			OV490_ID(pid, ver));
		return -ENODEV;
	}

	dev_err(dev->dev, "Identified RDACM21 camera module\n");

	/*
	 * The ISP is programmed with the content of a serial flash memory.
	 * Read the firmware configuration to reflect it through the V4L2 APIs.
	 */
	ov490_write(dev, 0xfffd, 0x80);
	ov490_write(dev, 0xfffe, 0x82);
	ov490_read(dev, OV490_ISP_HSIZE_HIGH, &val);
	dev->fmt.width = (val & 0xf) << 8;
	ov490_read(dev, OV490_ISP_HSIZE_LOW, &val);
	dev->fmt.width |= (val & 0xff);

	ov490_read(dev, OV490_ISP_VSIZE_HIGH, &val);
	dev->fmt.height = (val & 0xf) << 8;
	ov490_read(dev, OV490_ISP_VSIZE_LOW, &val);
	dev->fmt.height |= val & 0xff;

	/* Set bus width to 12 bits [0:11] */
	ov490_write(dev, 0xfffd, 0x80);
	ov490_write(dev, 0xfffe, 0x28);
	ov490_write(dev, 0x6009, 0x10);

	for (i = 0; i < ARRAY_SIZE(ov490_regs_wizard); ++i) {
		ret = ov490_write(dev, ov490_regs_wizard[i].reg,
				  ov490_regs_wizard[i].val);
		if (ret < 0) {
			dev_err(dev->dev,
				"%s: register %u (0x%04x) write failed (%d)\n",
				__func__, i, ov490_regs_wizard[i].reg, ret);
			return ret;
		}
	}

	return 0;
}

static int rdacm21_initialize(struct rdacm21_device *dev)
{
	int ret;

	dev->serializer->client->addr = MAX9271_DEFAULT_ADDR;

	/* Verify communication with the MAX9271: ping to wakeup. */
	i2c_smbus_read_byte(dev->serializer->client);
	max9271_configure_gmsl_link(dev->serializer);
	max9271_configure_i2c(dev->serializer);

	ret = max9271_verify_id(dev->serializer);
	if (ret < 0)
		return ret;

	ret = max9271_set_address(dev->serializer, dev->addrs[0]);
	if (ret < 0)
		return ret;
	dev->serializer->client->addr = dev->addrs[0];

	return ov490_initialize(dev);
}

static int rdacm21_probe(struct i2c_client *client)
{
	struct rdacm21_device *dev;
	struct fwnode_handle *ep;
	int ret;

	dev = devm_kzalloc(&client->dev, sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;
	dev->dev = &client->dev;

	dev->serializer = devm_kzalloc(&client->dev, sizeof(*dev->serializer),
				       GFP_KERNEL);
	if (!dev->serializer)
		return -ENOMEM;

	dev->serializer->client = client;

	ret = of_property_read_u32_array(client->dev.of_node, "reg",
					 dev->addrs, 2);
	if (ret < 0) {
		dev_err(dev->dev, "Invalid DT reg property: %d\n", ret);
		return -EINVAL;
	}

	dev->isp = i2c_new_dummy_device(client->adapter, OV490_I2C_ADDRESS);
	if (IS_ERR(dev->isp))
		return PTR_ERR(dev->isp);

	ret = rdacm21_initialize(dev);
	if (ret < 0)
		goto error;

	/* Initialize and register the subdevice. */
	v4l2_i2c_subdev_init(&dev->sd, client, &rdacm21_subdev_ops);
	dev->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;

	dev->pad.flags = MEDIA_PAD_FL_SOURCE;
	dev->sd.entity.flags |= MEDIA_ENT_F_CAM_SENSOR;
	ret = media_entity_pads_init(&dev->sd.entity, 1, &dev->pad);
	if (ret < 0)
		goto error;

	ep = fwnode_graph_get_next_endpoint(dev_fwnode(&client->dev), NULL);
	if (!ep) {
		dev_err(&client->dev,
			"Unable to get endpoint in node %pOF\n",
			client->dev.of_node);
		ret = -ENOENT;
		goto error;
	}
	dev->sd.fwnode = ep;

	ret = v4l2_async_register_subdev(&dev->sd);
	if (ret)
		goto error_put_node;

	return 0;

error_put_node:
	fwnode_handle_put(dev->sd.fwnode);
error:
	i2c_unregister_device(dev->isp);

	return ret;
}

static int rdacm21_remove(struct i2c_client *client)
{
	struct rdacm21_device *dev = i2c_to_rdacm21(client);

	fwnode_handle_put(dev->sd.fwnode);
	v4l2_async_unregister_subdev(&dev->sd);
	i2c_unregister_device(dev->isp);

	return 0;
}

static const struct of_device_id rdacm21_of_ids[] = {
	{ .compatible = "imi,rdacm21" },
	{ }
};
MODULE_DEVICE_TABLE(of, rdacm21_of_ids);

static struct i2c_driver rdacm21_i2c_driver = {
	.driver	= {
		.name	= "rdacm21",
		.of_match_table = rdacm21_of_ids,
	},
	.probe_new	= rdacm21_probe,
	.remove		= rdacm21_remove,
};

module_i2c_driver(rdacm21_i2c_driver);

MODULE_DESCRIPTION("GMSL Camera driver for RDACM21");
MODULE_AUTHOR("Jacopo Mondi, Kieran Bingham, Laurent Pinchart, Niklas Söderlund, Vladimir Barinov");
MODULE_LICENSE("GPL v2");
