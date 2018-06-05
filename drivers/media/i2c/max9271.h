/* SPDX-License-Identifier: GPL-2.0+ */
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

#include <linux/i2c.h>

#define MAX9271_DEFAULT_ADDR	0x40

/* Register 0x04 */
#define MAX9271_SEREN			BIT(7)
#define MAX9271_CLINKEN			BIT(6)
#define MAX9271_PRBSEN			BIT(5)
#define MAX9271_SLEEP			BIT(4)
#define MAX9271_INTTYPE_I2C		(0 << 2)
#define MAX9271_INTTYPE_UART		(1 << 2)
#define MAX9271_INTTYPE_NONE		(2 << 2)
#define MAX9271_REVCCEN			BIT(1)
#define MAX9271_FWDCCEN			BIT(0)
/* Register 0x07 */
#define MAX9271_DBL			BIT(7)
#define MAX9271_DRS			BIT(6)
#define MAX9271_BWS			BIT(5)
#define MAX9271_ES			BIT(4)
#define MAX9271_HVEN			BIT(2)
#define MAX9271_EDC_1BIT_PARITY		(0 << 0)
#define MAX9271_EDC_6BIT_CRC		(1 << 0)
#define MAX9271_EDC_6BIT_HAMMING	(2 << 0)
/* Register 0x08 */
#define MAX9271_INVVS			BIT(7)
#define MAX9271_INVHS			BIT(6)
#define MAX9271_REV_LOGAIN		BIT(3)
#define MAX9271_REV_HIVTH		BIT(0)
/* Register 0x09 */
#define MAX9271_ID			0x09
/* Register 0x0d */
#define MAX9271_I2CLOCACK		BIT(7)
#define MAX9271_I2CSLVSH_1046NS_469NS	(3 << 5)
#define MAX9271_I2CSLVSH_938NS_352NS	(2 << 5)
#define MAX9271_I2CSLVSH_469NS_234NS	(1 << 5)
#define MAX9271_I2CSLVSH_352NS_117NS	(0 << 5)
#define MAX9271_I2CMSTBT_837KBPS	(7 << 2)
#define MAX9271_I2CMSTBT_533KBPS	(6 << 2)
#define MAX9271_I2CMSTBT_339KBPS	(5 << 2)
#define MAX9271_I2CMSTBT_173KBPS	(4 << 2)
#define MAX9271_I2CMSTBT_105KBPS	(3 << 2)
#define MAX9271_I2CMSTBT_84KBPS		(2 << 2)
#define MAX9271_I2CMSTBT_28KBPS		(1 << 2)
#define MAX9271_I2CMSTBT_8KBPS		(0 << 2)
#define MAX9271_I2CSLVTO_NONE		(3 << 0)
#define MAX9271_I2CSLVTO_1024US		(2 << 0)
#define MAX9271_I2CSLVTO_256US		(1 << 0)
#define MAX9271_I2CSLVTO_64US		(0 << 0)
/* Register 0x0f */
#define MAX9271_GPIO5OUT		BIT(5)
#define MAX9271_GPIO4OUT		BIT(4)
#define MAX9271_GPIO3OUT		BIT(3)
#define MAX9271_GPIO2OUT		BIT(2)
#define MAX9271_GPIO1OUT		BIT(1)
#define MAX9271_SETGPO			BIT(0)
/* Register 0x15 */
#define MAX9271_PCLKDET			BIT(0)

struct max9271_device {
	struct i2c_client *client;
};

int max9271_s_stream(struct max9271_device *dev, bool enable);
int max9271_configure_i2c(struct max9271_device *dev);
int max9271_configure_gmsl_link(struct max9271_device *dev);
int max9271_set_gpio(struct max9271_device *dev, u8 val);
int max9271_verify_id(struct max9271_device *dev);
int max9271_set_address(struct max9271_device *dev, u8 addr);
