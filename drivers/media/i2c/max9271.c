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
 *
 * This file exports functions to control Maxim MAX9271 GMSL serializer
 * chip. This is not a self-contained driver, as MAX9271 is usually embedded in
 * camera modules with at least one image sensor and optional additional
 * components, such as uController units or ISPs/DSPs.
 *
 * Driver for the camera modules (ie rdacm20) are expected to use functions
 * exported from this library file to maximize code re-use across drivers.
 */
#include <linux/delay.h>
#include <linux/i2c.h>

#include "max9271.h"

static int max9271_read(struct max9271_device *dev, u8 reg)
{
	int ret;

	dev_dbg(&dev->client->dev, "%s(0x%02x)\n", __func__, reg);

	ret = i2c_smbus_read_byte_data(dev->client, reg);
	if (ret < 0)
		dev_dbg(&dev->client->dev,
			"%s: register 0x%02x read failed (%d)\n",
			__func__, reg, ret);

	return ret;
}

static int max9271_write(struct max9271_device *dev, u8 reg, u8 val)
{
	int ret;

	dev_dbg(&dev->client->dev, "%s(0x%02x, 0x%02x)\n", __func__, reg, val);

	ret = i2c_smbus_write_byte_data(dev->client, reg, val);
	if (ret < 0)
		dev_err(&dev->client->dev,
			"%s: register 0x%02x write failed (%d)\n",
			__func__, reg, ret);

	return ret;
}

/*
 * max9271_pclk_detect() - Detect valid pixel clock from image sensor
 *
 * Wait up to 10ms for a valid pixel clock.
 *
 * Returns 0 for success, < 0 for pixel clock not properly detected
 */
static int max9271_pclk_detect(struct max9271_device *dev)
{
	unsigned int i;
	int ret;

	for (i = 0; i < 100; i++) {
		ret = max9271_read(dev, 0x15);
		if (ret < 0)
			return ret;

		if (ret & MAX9271_PCLKDET)
			return 0;

		usleep_range(50, 100);
	}

	dev_err(&dev->client->dev, "Unable to detect valid pixel clock\n");
	return -EIO;
}

int max9271_s_stream(struct max9271_device *dev, bool enable)
{
	int ret;

	if (enable) {
		ret = max9271_pclk_detect(dev);
		if (ret)
			return ret;

		/* Enable the serial link. */
		max9271_write(dev, 0x04, MAX9271_SEREN | MAX9271_REVCCEN |
			      MAX9271_FWDCCEN);
	} else {
		/* Disable the serial link. */
		max9271_write(dev, 0x04, MAX9271_CLINKEN | MAX9271_REVCCEN |
			      MAX9271_FWDCCEN);
	}

	return 0;
}
EXPORT_SYMBOL_GPL(max9271_s_stream);

int max9271_configure_i2c(struct max9271_device *dev)
{
	/*
	 * Configure the I2C bus:
	 *
	 * - Enable high thresholds on the reverse channel
	 * - Disable artificial ACK and set I2C speed
	 */
	max9271_write(dev, 0x08, MAX9271_REV_HIVTH);
	usleep_range(5000, 8000);

	max9271_write(dev, 0x0d, MAX9271_I2CSLVSH_469NS_234NS |
		      MAX9271_I2CSLVTO_1024US | MAX9271_I2CMSTBT_105KBPS);
	usleep_range(5000, 8000);

	return 0;
}
EXPORT_SYMBOL_GPL(max9271_configure_i2c);

int max9271_configure_gmsl_link(struct max9271_device *dev)
{
	/*
	 * Disable the serial link and enable the configuration link to allow
	 * the control channel to operate in a low-speed mode in the absence of
	 * the serial link clock.
	 */
	max9271_write(dev, 0x04, MAX9271_CLINKEN | MAX9271_REVCCEN |
		      MAX9271_FWDCCEN);

	/*
	 * The serializer temporarily disables the reverse control channel for
	 * 350µs after starting/stopping the forward serial link, but the
	 * deserializer synchronization time isn't clearly documented.
	 *
	 * According to the serializer datasheet we should wait 3ms, while
	 * according to the deserializer datasheet we should wait 5ms.
	 *
	 * Short delays here appear to show bit-errors in the writes following.
	 * Therefore a conservative delay seems best here.
	 */
	usleep_range(5000, 8000);

	/*
	 * Configure the GMSL link:
	 *
	 * - Double input mode, high data rate, 24-bit mode
	 * - Latch input data on PCLKIN rising edge
	 * - Enable HS/VS encoding
	 * - 1-bit parity error detection
	 */
	max9271_write(dev, 0x07, MAX9271_DBL | MAX9271_HVEN |
		      MAX9271_EDC_1BIT_PARITY);
	usleep_range(5000, 8000);

	return 0;
}
EXPORT_SYMBOL_GPL(max9271_configure_gmsl_link);

int max9271_set_gpio(struct max9271_device *dev, u8 val)
{
	int ret = max9271_write(dev, 0x0f, val);
	if (ret < 0) {
		dev_err(&dev->client->dev, "Failed to set gpio (%d)\n", ret);
		return ret;
	}

	usleep_range(10000, 20000);

	return 0;
}
EXPORT_SYMBOL_GPL(max9271_set_gpio);

int max9271_verify_id(struct max9271_device *dev)
{
	int ret;

	ret = max9271_read(dev, 0x1e);
	if (ret < 0) {
		dev_err(&dev->client->dev, "MAX9271 ID read failed (%d)\n",
			ret);
		return ret;
	}

	if (ret != MAX9271_ID) {
		dev_err(&dev->client->dev, "MAX9271 ID mismatch (0x%02x)\n",
			ret);
		return -ENXIO;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(max9271_verify_id);

int max9271_set_address(struct max9271_device *dev, u8 addr)
{
	int ret;

	ret = max9271_write(dev, 0x00, addr << 1);
	if (ret < 0) {
		dev_err(&dev->client->dev,
			"MAX9271 I2C address change failed (%d)\n", ret);
		return ret;
	}
	dev->client->addr = addr;
	usleep_range(3500, 5000);

	return 0;
}
EXPORT_SYMBOL_GPL(max9271_set_address);
