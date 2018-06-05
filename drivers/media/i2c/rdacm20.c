// SPDX-License-Identifier: GPL-2.0+
/*
 * IMI RDACM20 GMSL Camera Driver
 *
 * Copyright (C) 2017-2018 Jacopo Mondi
 * Copyright (C) 2017-2018 Kieran Bingham
 * Copyright (C) 2017-2018 Laurent Pinchart
 * Copyright (C) 2017-2018 Niklas Söderlund
 * Copyright (C) 2016 Renesas Electronics Corporation
 * Copyright (C) 2015 Cogent Embedded, Inc.
 */

/*
 * The camera is mode of an Omnivision OV10635 sensor connected to a Maxim
 * MAX9271 GMSL serializer.
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

#include "rdacm20-ov10635.h"
#include "max9271.h"

#define RDACM20_SENSOR_HARD_RESET

#define OV10635_I2C_ADDRESS		0x30

#define OV10635_SOFTWARE_RESET		0x0103
#define OV10635_PID			0x300a
#define OV10635_VER			0x300b
#define OV10635_SC_CMMN_SCCB_ID		0x300c
#define OV10635_SC_CMMN_SCCB_ID_SELECT	BIT(0)
#define OV10635_VERSION			0xa635

#define OV10635_WIDTH			1280
#define OV10635_HEIGHT			800
#define OV10635_FORMAT			MEDIA_BUS_FMT_UYVY8_2X8
/* #define OV10635_FORMAT			MEDIA_BUS_FMT_UYVY10_2X10 */

struct rdacm20_device {
	struct device			*dev;
	struct max9271_device		*serializer;
	struct i2c_client		*sensor;
	struct v4l2_subdev		sd;
	struct media_pad		pad;
	struct v4l2_ctrl_handler	ctrls;
};

static inline struct rdacm20_device *sd_to_rdacm20(struct v4l2_subdev *sd)
{
	return container_of(sd, struct rdacm20_device, sd);
}

static inline struct rdacm20_device *i2c_to_rdacm20(struct i2c_client *client)
{
	return sd_to_rdacm20(i2c_get_clientdata(client));
}

static int ov10635_read16(struct rdacm20_device *dev, u16 reg)
{
	u8 buf[2] = { reg >> 8, reg & 0xff };
	int ret;

	ret = i2c_master_send(dev->sensor, buf, 2);
	if (ret == 2)
		ret = i2c_master_recv(dev->sensor, buf, 2);

	if (ret < 0) {
		dev_dbg(dev->dev, "%s: register 0x%04x read failed (%d)\n",
			__func__, reg, ret);
		return ret;
	}

	return (buf[0] << 8) | buf[1];
}

static int __ov10635_write(struct rdacm20_device *dev, u16 reg, u8 val)
{
	u8 buf[3] = { reg >> 8, reg & 0xff, val };
	int ret;

	dev_dbg(dev->dev, "%s(0x%04x, 0x%02x)\n", __func__, reg, val);

	ret = i2c_master_send(dev->sensor, buf, 3);
	return ret < 0 ? ret : 0;
}

static int ov10635_write(struct rdacm20_device *dev, u16 reg, u8 val)
{
	int ret;

	ret = __ov10635_write(dev, reg, val);
	if (ret < 0)
		dev_err(dev->dev, "%s: register 0x%04x write failed (%d)\n",
			__func__, reg, ret);

	return ret;
}

static int ov10635_set_regs(struct rdacm20_device *dev,
			    const struct ov10635_reg *regs,
			    unsigned int nr_regs)
{
	unsigned int i;
	int ret;

	for (i = 0; i < nr_regs; i++) {
		ret = __ov10635_write(dev, regs[i].reg, regs[i].val);
		if (ret) {
			dev_err(dev->dev,
				"%s: register %u (0x%04x) write failed (%d)\n",
				__func__, i, regs[i].reg, ret);
			return ret;
		}
	}

	return 0;
}

static int rdacm20_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct rdacm20_device *dev = sd_to_rdacm20(sd);

	return max9271_s_stream(dev->serializer, enable);
}

static int rdacm20_enum_mbus_code(struct v4l2_subdev *sd,
				  struct v4l2_subdev_pad_config *cfg,
				  struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->pad || code->index > 0)
		return -EINVAL;

	code->code = OV10635_FORMAT;

	return 0;
}

static int rdacm20_get_fmt(struct v4l2_subdev *sd,
			   struct v4l2_subdev_pad_config *cfg,
			   struct v4l2_subdev_format *format)
{
	struct v4l2_mbus_framefmt *mf = &format->format;

	if (format->pad)
		return -EINVAL;

	mf->width		= OV10635_WIDTH;
	mf->height		= OV10635_HEIGHT;
	mf->code		= OV10635_FORMAT;
	mf->colorspace		= V4L2_COLORSPACE_RAW;
	mf->field		= V4L2_FIELD_NONE;
	mf->ycbcr_enc		= V4L2_YCBCR_ENC_601;
	mf->quantization	= V4L2_QUANTIZATION_FULL_RANGE;
	mf->xfer_func		= V4L2_XFER_FUNC_NONE;

	return 0;
}

static struct v4l2_subdev_video_ops rdacm20_video_ops = {
	.s_stream	= rdacm20_s_stream,
};

static const struct v4l2_subdev_pad_ops rdacm20_subdev_pad_ops = {
	.enum_mbus_code = rdacm20_enum_mbus_code,
	.get_fmt	= rdacm20_get_fmt,
	.set_fmt	= rdacm20_get_fmt,
};

static struct v4l2_subdev_ops rdacm20_subdev_ops = {
	.video		= &rdacm20_video_ops,
	.pad		= &rdacm20_subdev_pad_ops,
};

static int rdacm20_initialize(struct rdacm20_device *dev)
{
	u32 addrs[2];
	int ret;

	ret = of_property_read_u32_array(dev->dev->of_node, "reg",
					 addrs, ARRAY_SIZE(addrs));
	if (ret < 0) {
		dev_err(dev->dev, "Invalid DT reg property\n");
		return -EINVAL;
	}

	/*
	 * FIXME: The MAX9271 boots at a default address that we will change to
	 * the address specified in DT. Set the client address back to the
	 * default for initial communication.
	 */
	dev->serializer->client->addr = MAX9271_DEFAULT_ADDR;

	/* Verify communication with the MAX9271: ping to wakeup. */
	i2c_smbus_read_byte(dev->serializer->client);

	/*
	 *  Ensure that we have a good link configuration before attempting to
	 *  identify the device.
	 */
	max9271_configure_i2c(dev->serializer);
	max9271_configure_gmsl_link(dev->serializer);

	ret = max9271_verify_id(dev->serializer);
	if (ret < 0)
		return ret;

	ret = max9271_set_address(dev->serializer, addrs[0]);
	if (ret < 0)
		return ret;

	/* Reset and verify communication with the OV10635. */
#ifdef RDACM20_SENSOR_HARD_RESET
	/* Cycle the OV10635 reset signal connected to the MAX9271 GPIO1. */
	max9271_set_gpio(dev->serializer,
			 0xff & ~(MAX9271_GPIO1OUT | MAX9271_SETGPO));
	max9271_set_gpio(dev->serializer,
			0xff & ~MAX9271_SETGPO);
#else
	/* Perform a software reset. */
	ret = ov10635_write(dev, OV10635_SOFTWARE_RESET, 1);
	if (ret < 0) {
		dev_err(dev->dev, "OV10635 reset failed (%d)\n", ret);
		return -ENXIO;
	}

	udelay(100);
#endif

	ret = ov10635_read16(dev, OV10635_PID);
	if (ret < 0) {
		dev_err(dev->dev, "OV10635 ID read failed (%d)\n",
			ret);
		return -ENXIO;
	}

	if (ret != OV10635_VERSION) {
		dev_err(dev->dev, "OV10635 ID mismatch (0x%04x)\n",
			ret);
		return -ENXIO;
	}

	dev_info(dev->dev, "Identified MAX9271 + OV10635 device\n");

	/* Change the sensor I2C address. */
	ret = ov10635_write(dev, OV10635_SC_CMMN_SCCB_ID,
			    (addrs[1] << 1) | OV10635_SC_CMMN_SCCB_ID_SELECT);
	if (ret < 0) {
		dev_err(dev->dev,
			"OV10635 I2C address change failed (%d)\n", ret);
		return ret;
	}
	dev->sensor->addr = addrs[1];
	usleep_range(3500, 5000);

	/* Program the 0V10635 initial configuration. */
	return ov10635_set_regs(dev, ov10635_regs_wizard,
				ARRAY_SIZE(ov10635_regs_wizard));
}

static int rdacm20_probe(struct i2c_client *client)
{
	struct rdacm20_device *dev;
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

	/* Create the dummy I2C client for the sensor. */
	dev->sensor = i2c_new_dummy_device(client->adapter,
					   OV10635_I2C_ADDRESS);
	if (IS_ERR(dev->sensor)) {
		ret = PTR_ERR(dev->sensor);
		goto error;
	}

	/* Initialize the hardware. */
	ret = rdacm20_initialize(dev);
	if (ret < 0)
		goto error;

	/* Initialize and register the subdevice. */
	v4l2_i2c_subdev_init(&dev->sd, client, &rdacm20_subdev_ops);
	dev->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;

	v4l2_ctrl_handler_init(&dev->ctrls, 1);
	/*
	 * FIXME: Compute the real pixel rate. The 50 MP/s value comes from the
	 * hardcoded frequency in the BSP CSI-2 receiver driver.
	 */
	v4l2_ctrl_new_std(&dev->ctrls, NULL, V4L2_CID_PIXEL_RATE, 50000000,
			  50000000, 1, 50000000);
	dev->sd.ctrl_handler = &dev->ctrls;

	ret = dev->ctrls.error;
	if (ret)
		goto error_free_ctrls;

	dev->pad.flags = MEDIA_PAD_FL_SOURCE;
	dev->sd.entity.flags |= MEDIA_ENT_F_CAM_SENSOR;
	ret = media_entity_pads_init(&dev->sd.entity, 1, &dev->pad);
	if (ret < 0)
		goto error_free_ctrls;

	ep = fwnode_graph_get_next_endpoint(dev_fwnode(&client->dev), NULL);
	if (!ep) {
		dev_err(&client->dev,
			"Unable to get endpoint in node %pOF\n",
			client->dev.of_node);
		ret = -ENOENT;
		goto error_free_ctrls;
	}
	dev->sd.fwnode = ep;

	ret = v4l2_async_register_subdev(&dev->sd);
	if (ret)
		goto error_put_node;

	return 0;

error_put_node:
	fwnode_handle_put(ep);
error_free_ctrls:
	v4l2_ctrl_handler_free(&dev->ctrls);
error:
	media_entity_cleanup(&dev->sd.entity);
	if (dev->sensor)
		i2c_unregister_device(dev->sensor);

	dev_err(&client->dev, "probe failed\n");

	return ret;
}

static int rdacm20_remove(struct i2c_client *client)
{
	struct rdacm20_device *dev = i2c_to_rdacm20(client);

	fwnode_handle_put(dev->sd.fwnode);
	v4l2_async_unregister_subdev(&dev->sd);
	v4l2_ctrl_handler_free(&dev->ctrls);
	media_entity_cleanup(&dev->sd.entity);
	i2c_unregister_device(dev->sensor);

	return 0;
}

static void rdacm20_shutdown(struct i2c_client *client)
{
	struct rdacm20_device *dev = i2c_to_rdacm20(client);

	/* make sure stream off during shutdown (reset/reboot) */
	rdacm20_s_stream(&dev->sd, 0);
}

static const struct of_device_id rdacm20_of_ids[] = {
	{ .compatible = "imi,rdacm20", },
	{ }
};
MODULE_DEVICE_TABLE(of, rdacm20_of_ids);

static struct i2c_driver rdacm20_i2c_driver = {
	.driver	= {
		.name	= "rdacm20",
		.of_match_table = rdacm20_of_ids,
	},
	.probe_new	= rdacm20_probe,
	.remove		= rdacm20_remove,
	.shutdown	= rdacm20_shutdown,
};

module_i2c_driver(rdacm20_i2c_driver);

MODULE_DESCRIPTION("GMSL Camera driver for RDACM20");
MODULE_AUTHOR("Vladimir Barinov");
MODULE_LICENSE("GPL");
