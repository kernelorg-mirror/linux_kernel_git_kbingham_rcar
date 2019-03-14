// SPDX-License-Identifier: GPL-2.0+
/*
 * rcar_du_drv.c  --  R-Car Display Unit DRM driver
 *
 * Copyright (C) 2013-2015 Renesas Electronics Corporation
 *
 * Contact: Laurent Pinchart (laurent.pinchart@ideasonboard.com)
 */

#include <linux/clk.h>
#include <linux/io.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/slab.h>
#include <linux/wait.h>

#include <drm/drm_atomic_helper.h>
#include <drm/drm_fb_cma_helper.h>
#include <drm/drm_fb_helper.h>
#include <drm/drm_drv.h>
#include <drm/drm_gem_cma_helper.h>
#include <drm/drm_probe_helper.h>

#include "rcar_du_drv.h"
#include "rcar_du_kms.h"
#include "rcar_du_of.h"
#include "rcar_du_regs.h"

/* -----------------------------------------------------------------------------
 * Device Information
 */

static const struct rcar_du_device_info rzg1_du_r8a7743_info = {
	.gen = 2,
	.features = RCAR_DU_FEATURE_CRTC_IRQ_CLOCK
		  | RCAR_DU_FEATURE_INTERLACED
		  | RCAR_DU_FEATURE_TVM_SYNC,
	.channels_mask = BIT(1) | BIT(0),
	.routes = {
		/*
		 * R8A774[34] has one RGB output and one LVDS output
		 */
		[RCAR_DU_OUTPUT_DPAD0] = {
			.possible_crtcs = BIT(1) | BIT(0),
			.port = 0,
		},
		[RCAR_DU_OUTPUT_LVDS0] = {
			.possible_crtcs = BIT(0),
			.port = 1,
		},
	},
	.num_lvds = 1,
};

static const struct rcar_du_device_info rzg1_du_r8a7745_info = {
	.gen = 2,
	.features = RCAR_DU_FEATURE_CRTC_IRQ_CLOCK
		  | RCAR_DU_FEATURE_INTERLACED
		  | RCAR_DU_FEATURE_TVM_SYNC,
	.channels_mask = BIT(1) | BIT(0),
	.routes = {
		/*
		 * R8A7745 has two RGB outputs
		 */
		[RCAR_DU_OUTPUT_DPAD0] = {
			.possible_crtcs = BIT(0),
			.port = 0,
		},
		[RCAR_DU_OUTPUT_DPAD1] = {
			.possible_crtcs = BIT(1),
			.port = 1,
		},
	},
};

static const struct rcar_du_device_info rzg1_du_r8a77470_info = {
	.gen = 2,
	.features = RCAR_DU_FEATURE_CRTC_IRQ_CLOCK
		  | RCAR_DU_FEATURE_INTERLACED
		  | RCAR_DU_FEATURE_TVM_SYNC,
	.channels_mask = BIT(1) | BIT(0),
	.routes = {
		/*
		 * R8A77470 has two RGB outputs, one LVDS output, and
		 * one (currently unsupported) analog video output
		 */
		[RCAR_DU_OUTPUT_DPAD0] = {
			.possible_crtcs = BIT(0),
			.port = 0,
		},
		[RCAR_DU_OUTPUT_DPAD1] = {
			.possible_crtcs = BIT(1),
			.port = 1,
		},
		[RCAR_DU_OUTPUT_LVDS0] = {
			.possible_crtcs = BIT(0) | BIT(1),
			.port = 2,
		},
	},
};

static const struct rcar_du_device_info rcar_du_r8a774c0_info = {
	.gen = 3,
	.features = RCAR_DU_FEATURE_CRTC_IRQ_CLOCK
		  | RCAR_DU_FEATURE_VSP1_SOURCE,
	.channels_mask = BIT(1) | BIT(0),
	.routes = {
		/*
		 * R8A774C0 has one RGB output and two LVDS outputs
		 */
		[RCAR_DU_OUTPUT_DPAD0] = {
			.possible_crtcs = BIT(0) | BIT(1),
			.port = 0,
		},
		[RCAR_DU_OUTPUT_LVDS0] = {
			.possible_crtcs = BIT(0),
			.port = 1,
		},
		[RCAR_DU_OUTPUT_LVDS1] = {
			.possible_crtcs = BIT(1),
			.port = 2,
		},
	},
	.num_lvds = 2,
	.lvds_clk_mask =  BIT(1) | BIT(0),
};

static const struct rcar_du_device_info rcar_du_r8a7779_info = {
	.gen = 1,
	.features = RCAR_DU_FEATURE_INTERLACED
		  | RCAR_DU_FEATURE_TVM_SYNC,
	.channels_mask = BIT(1) | BIT(0),
	.routes = {
		/*
		 * R8A7779 has two RGB outputs and one (currently unsupported)
		 * TCON output.
		 */
		[RCAR_DU_OUTPUT_DPAD0] = {
			.possible_crtcs = BIT(0),
			.port = 0,
		},
		[RCAR_DU_OUTPUT_DPAD1] = {
			.possible_crtcs = BIT(1) | BIT(0),
			.port = 1,
		},
	},
};

static const struct rcar_du_device_info rcar_du_r8a7790_info = {
	.gen = 2,
	.features = RCAR_DU_FEATURE_CRTC_IRQ_CLOCK
		  | RCAR_DU_FEATURE_INTERLACED
		  | RCAR_DU_FEATURE_TVM_SYNC,
	.quirks = RCAR_DU_QUIRK_ALIGN_128B,
	.channels_mask = BIT(2) | BIT(1) | BIT(0),
	.routes = {
		/*
		 * R8A7790 has one RGB output, two LVDS outputs and one
		 * (currently unsupported) TCON output.
		 */
		[RCAR_DU_OUTPUT_DPAD0] = {
			.possible_crtcs = BIT(2) | BIT(1) | BIT(0),
			.port = 0,
		},
		[RCAR_DU_OUTPUT_LVDS0] = {
			.possible_crtcs = BIT(0),
			.port = 1,
		},
		[RCAR_DU_OUTPUT_LVDS1] = {
			.possible_crtcs = BIT(2) | BIT(1),
			.port = 2,
		},
	},
	.num_lvds = 2,
};

/* M2-W (r8a7791) and M2-N (r8a7793) are identical */
static const struct rcar_du_device_info rcar_du_r8a7791_info = {
	.gen = 2,
	.features = RCAR_DU_FEATURE_CRTC_IRQ_CLOCK
		  | RCAR_DU_FEATURE_INTERLACED
		  | RCAR_DU_FEATURE_TVM_SYNC,
	.channels_mask = BIT(1) | BIT(0),
	.routes = {
		/*
		 * R8A779[13] has one RGB output, one LVDS output and one
		 * (currently unsupported) TCON output.
		 */
		[RCAR_DU_OUTPUT_DPAD0] = {
			.possible_crtcs = BIT(1) | BIT(0),
			.port = 0,
		},
		[RCAR_DU_OUTPUT_LVDS0] = {
			.possible_crtcs = BIT(0),
			.port = 1,
		},
	},
	.num_lvds = 1,
};

static const struct rcar_du_device_info rcar_du_r8a7792_info = {
	.gen = 2,
	.features = RCAR_DU_FEATURE_CRTC_IRQ_CLOCK
		  | RCAR_DU_FEATURE_INTERLACED
		  | RCAR_DU_FEATURE_TVM_SYNC,
	.channels_mask = BIT(1) | BIT(0),
	.routes = {
		/* R8A7792 has two RGB outputs. */
		[RCAR_DU_OUTPUT_DPAD0] = {
			.possible_crtcs = BIT(0),
			.port = 0,
		},
		[RCAR_DU_OUTPUT_DPAD1] = {
			.possible_crtcs = BIT(1),
			.port = 1,
		},
	},
};

static const struct rcar_du_device_info rcar_du_r8a7794_info = {
	.gen = 2,
	.features = RCAR_DU_FEATURE_CRTC_IRQ_CLOCK
		  | RCAR_DU_FEATURE_INTERLACED
		  | RCAR_DU_FEATURE_TVM_SYNC,
	.channels_mask = BIT(1) | BIT(0),
	.routes = {
		/*
		 * R8A7794 has two RGB outputs and one (currently unsupported)
		 * TCON output.
		 */
		[RCAR_DU_OUTPUT_DPAD0] = {
			.possible_crtcs = BIT(0),
			.port = 0,
		},
		[RCAR_DU_OUTPUT_DPAD1] = {
			.possible_crtcs = BIT(1),
			.port = 1,
		},
	},
};

static const struct rcar_du_device_info rcar_du_r8a7795_info = {
	.gen = 3,
	.features = RCAR_DU_FEATURE_CRTC_IRQ_CLOCK
		  | RCAR_DU_FEATURE_VSP1_SOURCE
		  | RCAR_DU_FEATURE_INTERLACED
		  | RCAR_DU_FEATURE_TVM_SYNC,
	.channels_mask = BIT(3) | BIT(2) | BIT(1) | BIT(0),
	.routes = {
		/*
		 * R8A7795 has one RGB output, two HDMI outputs and one
		 * LVDS output.
		 */
		[RCAR_DU_OUTPUT_DPAD0] = {
			.possible_crtcs = BIT(3),
			.port = 0,
		},
		[RCAR_DU_OUTPUT_HDMI0] = {
			.possible_crtcs = BIT(1),
			.port = 1,
		},
		[RCAR_DU_OUTPUT_HDMI1] = {
			.possible_crtcs = BIT(2),
			.port = 2,
		},
		[RCAR_DU_OUTPUT_LVDS0] = {
			.possible_crtcs = BIT(0),
			.port = 3,
		},
	},
	.num_lvds = 1,
	.dpll_mask =  BIT(2) | BIT(1),
};

static const struct rcar_du_device_info rcar_du_r8a7796_info = {
	.gen = 3,
	.features = RCAR_DU_FEATURE_CRTC_IRQ_CLOCK
		  | RCAR_DU_FEATURE_VSP1_SOURCE
		  | RCAR_DU_FEATURE_INTERLACED
		  | RCAR_DU_FEATURE_TVM_SYNC,
	.channels_mask = BIT(2) | BIT(1) | BIT(0),
	.routes = {
		/*
		 * R8A7796 has one RGB output, one LVDS output and one HDMI
		 * output.
		 */
		[RCAR_DU_OUTPUT_DPAD0] = {
			.possible_crtcs = BIT(2),
			.port = 0,
		},
		[RCAR_DU_OUTPUT_HDMI0] = {
			.possible_crtcs = BIT(1),
			.port = 1,
		},
		[RCAR_DU_OUTPUT_LVDS0] = {
			.possible_crtcs = BIT(0),
			.port = 2,
		},
	},
	.num_lvds = 1,
	.dpll_mask =  BIT(1),
};

static const struct rcar_du_device_info rcar_du_r8a77965_info = {
	.gen = 3,
	.features = RCAR_DU_FEATURE_CRTC_IRQ_CLOCK
		  | RCAR_DU_FEATURE_VSP1_SOURCE
		  | RCAR_DU_FEATURE_INTERLACED
		  | RCAR_DU_FEATURE_TVM_SYNC,
	.channels_mask = BIT(3) | BIT(1) | BIT(0),
	.routes = {
		/*
		 * R8A77965 has one RGB output, one LVDS output and one HDMI
		 * output.
		 */
		[RCAR_DU_OUTPUT_DPAD0] = {
			.possible_crtcs = BIT(2),
			.port = 0,
		},
		[RCAR_DU_OUTPUT_HDMI0] = {
			.possible_crtcs = BIT(1),
			.port = 1,
		},
		[RCAR_DU_OUTPUT_LVDS0] = {
			.possible_crtcs = BIT(0),
			.port = 2,
		},
	},
	.num_lvds = 1,
	.dpll_mask =  BIT(1),
};

static const struct rcar_du_device_info rcar_du_r8a77970_info = {
	.gen = 3,
	.features = RCAR_DU_FEATURE_CRTC_IRQ_CLOCK
		  | RCAR_DU_FEATURE_VSP1_SOURCE
		  | RCAR_DU_FEATURE_INTERLACED
		  | RCAR_DU_FEATURE_TVM_SYNC,
	.channels_mask = BIT(0),
	.routes = {
		/* R8A77970 has one RGB output and one LVDS output. */
		[RCAR_DU_OUTPUT_DPAD0] = {
			.possible_crtcs = BIT(0),
			.port = 0,
		},
		[RCAR_DU_OUTPUT_LVDS0] = {
			.possible_crtcs = BIT(0),
			.port = 1,
		},
	},
	.num_lvds = 1,
};

static const struct rcar_du_device_info rcar_du_r8a7799x_info = {
	.gen = 3,
	.features = RCAR_DU_FEATURE_CRTC_IRQ_CLOCK
		  | RCAR_DU_FEATURE_VSP1_SOURCE,
	.channels_mask = BIT(1) | BIT(0),
	.routes = {
		/*
		 * R8A77990 and R8A77995 have one RGB output and two LVDS
		 * outputs.
		 */
		[RCAR_DU_OUTPUT_DPAD0] = {
			.possible_crtcs = BIT(0) | BIT(1),
			.port = 0,
		},
		[RCAR_DU_OUTPUT_LVDS0] = {
			.possible_crtcs = BIT(0),
			.port = 1,
		},
		[RCAR_DU_OUTPUT_LVDS1] = {
			.possible_crtcs = BIT(1),
			.port = 2,
		},
	},
	.num_lvds = 2,
	.lvds_clk_mask =  BIT(1) | BIT(0),
};

static const struct of_device_id rcar_du_of_table[] = {
	{ .compatible = "renesas,du-r8a7743", .data = &rzg1_du_r8a7743_info },
	{ .compatible = "renesas,du-r8a7744", .data = &rzg1_du_r8a7743_info },
	{ .compatible = "renesas,du-r8a7745", .data = &rzg1_du_r8a7745_info },
	{ .compatible = "renesas,du-r8a77470", .data = &rzg1_du_r8a77470_info },
	{ .compatible = "renesas,du-r8a774c0", .data = &rcar_du_r8a774c0_info },
	{ .compatible = "renesas,du-r8a7779", .data = &rcar_du_r8a7779_info },
	{ .compatible = "renesas,du-r8a7790", .data = &rcar_du_r8a7790_info },
	{ .compatible = "renesas,du-r8a7791", .data = &rcar_du_r8a7791_info },
	{ .compatible = "renesas,du-r8a7792", .data = &rcar_du_r8a7792_info },
	{ .compatible = "renesas,du-r8a7793", .data = &rcar_du_r8a7791_info },
	{ .compatible = "renesas,du-r8a7794", .data = &rcar_du_r8a7794_info },
	{ .compatible = "renesas,du-r8a7795", .data = &rcar_du_r8a7795_info },
	{ .compatible = "renesas,du-r8a7796", .data = &rcar_du_r8a7796_info },
	{ .compatible = "renesas,du-r8a77965", .data = &rcar_du_r8a77965_info },
	{ .compatible = "renesas,du-r8a77970", .data = &rcar_du_r8a77970_info },
	{ .compatible = "renesas,du-r8a77990", .data = &rcar_du_r8a7799x_info },
	{ .compatible = "renesas,du-r8a77995", .data = &rcar_du_r8a7799x_info },
	{ }
};

MODULE_DEVICE_TABLE(of, rcar_du_of_table);

/* -----------------------------------------------------------------------------
 * DRM operations
 */

DEFINE_DRM_GEM_CMA_FOPS(rcar_du_fops);

static struct drm_driver rcar_du_driver = {
	.driver_features	= DRIVER_GEM | DRIVER_MODESET | DRIVER_PRIME
				| DRIVER_ATOMIC,
	.gem_free_object_unlocked = drm_gem_cma_free_object,
	.gem_vm_ops		= &drm_gem_cma_vm_ops,
	.prime_handle_to_fd	= drm_gem_prime_handle_to_fd,
	.prime_fd_to_handle	= drm_gem_prime_fd_to_handle,
	.gem_prime_import	= drm_gem_prime_import,
	.gem_prime_export	= drm_gem_prime_export,
	.gem_prime_get_sg_table	= drm_gem_cma_prime_get_sg_table,
	.gem_prime_import_sg_table = drm_gem_cma_prime_import_sg_table,
	.gem_prime_vmap		= drm_gem_cma_prime_vmap,
	.gem_prime_vunmap	= drm_gem_cma_prime_vunmap,
	.gem_prime_mmap		= drm_gem_cma_prime_mmap,
	.dumb_create		= rcar_du_dumb_create,
	.fops			= &rcar_du_fops,
	.name			= "rcar-du",
	.desc			= "Renesas R-Car Display Unit",
	.date			= "20130110",
	.major			= 1,
	.minor			= 0,
};


/* -----------------------------------------------------------------------------
 * Register Maps.
 */

#define REGISTER_NAME(__REG) { (__REG), #__REG }
static struct regname {
	unsigned int addr;
	char *name;
} regnames[] = {
	REGISTER_NAME(DSYSR),
	REGISTER_NAME(DSMR),
	REGISTER_NAME(DSSR),
	REGISTER_NAME(DSRCR),
	REGISTER_NAME(DIER),
	REGISTER_NAME(CPCR),
	REGISTER_NAME(DPPR),
	REGISTER_NAME(DEFR),
	REGISTER_NAME(DAPCR),
	REGISTER_NAME(DCPCR),
	REGISTER_NAME(DEFR2),
	REGISTER_NAME(DEFR3),
	REGISTER_NAME(DEFR4),
	REGISTER_NAME(DVCSR),
	REGISTER_NAME(DEFR5),
	REGISTER_NAME(DDLTR),
	REGISTER_NAME(DEFR6),
	REGISTER_NAME(DD1SSR),
	REGISTER_NAME(DD1SRCR),
	REGISTER_NAME(DD1IER),
	REGISTER_NAME(DEFR8),
	REGISTER_NAME(DOFLR),
	REGISTER_NAME(DIDSR),
	REGISTER_NAME(DEFR10),
	REGISTER_NAME(DPLLCR),
	REGISTER_NAME(DPLLC2R),
	REGISTER_NAME(HDSR),
	REGISTER_NAME(HDER),
	REGISTER_NAME(VDSR),
	REGISTER_NAME(VDER),
	REGISTER_NAME(HCR),
	REGISTER_NAME(HSWR),
	REGISTER_NAME(VCR),
	REGISTER_NAME(VSPR),
	REGISTER_NAME(EQWR),
	REGISTER_NAME(SPWR),
	REGISTER_NAME(CLAMPSR),
	REGISTER_NAME(CLAMPWR),
	REGISTER_NAME(DESR),
	REGISTER_NAME(DEWR),
	REGISTER_NAME(CP1TR),
	REGISTER_NAME(CP2TR),
	REGISTER_NAME(CP3TR),
	REGISTER_NAME(CP4TR),
	REGISTER_NAME(DOOR),
	REGISTER_NAME(CDER),
	REGISTER_NAME(BPOR),
	REGISTER_NAME(RINTOFSR),
	REGISTER_NAME(DSHPR),
	REGISTER_NAME(PLANE_OFF),
	REGISTER_NAME(PnMR),
	REGISTER_NAME(PnMWR),
	REGISTER_NAME(PnALPHAR),
	REGISTER_NAME(PnDSXR),
	REGISTER_NAME(PnDSYR),
	REGISTER_NAME(PnDPXR),
	REGISTER_NAME(PnDPYR),
	REGISTER_NAME(PnDSA0R),
	REGISTER_NAME(PnDSA1R),
	REGISTER_NAME(PnDSA2R),
	REGISTER_NAME(PnDSA_MASK),
	REGISTER_NAME(PnSPXR),
	REGISTER_NAME(PnSPYR),
	REGISTER_NAME(PnWASPR),
	REGISTER_NAME(PnWAMWR),
	REGISTER_NAME(PnBTR),
	REGISTER_NAME(PnTC1R),
	REGISTER_NAME(PnTC2R),
	REGISTER_NAME(PnTC3R),
	REGISTER_NAME(PnMLR),
	REGISTER_NAME(PnSWAPR),
	REGISTER_NAME(PnDDCR),
	REGISTER_NAME(PnDDCR2),
	REGISTER_NAME(PnDDCR4),
	REGISTER_NAME(APnMR),
	REGISTER_NAME(APnMWR),
	REGISTER_NAME(APnDSXR),
	REGISTER_NAME(APnDSYR),
	REGISTER_NAME(APnDPXR),
	REGISTER_NAME(APnDPYR),
	REGISTER_NAME(APnDSA0R),
	REGISTER_NAME(APnDSA1R),
	REGISTER_NAME(APnDSA2R),
	REGISTER_NAME(APnSPXR),
	REGISTER_NAME(APnSPYR),
	REGISTER_NAME(APnWASPR),
	REGISTER_NAME(APnWAMWR),
	REGISTER_NAME(APnBTR),
	REGISTER_NAME(APnMLR),
	REGISTER_NAME(APnSWAPR),
	REGISTER_NAME(DCMR),
	REGISTER_NAME(DCMWR),
	REGISTER_NAME(DCSAR),
	REGISTER_NAME(DCMLR),
	REGISTER_NAME(CP1_000R),
	REGISTER_NAME(CP1_255R),
	REGISTER_NAME(CP2_000R),
	REGISTER_NAME(CP2_255R),
	REGISTER_NAME(CP3_000R),
	REGISTER_NAME(CP3_255R),
	REGISTER_NAME(CP4_000R),
	REGISTER_NAME(CP4_255R),
	REGISTER_NAME(ESCR02),
	REGISTER_NAME(ESCR13),
	REGISTER_NAME(OTAR02),
	REGISTER_NAME(OTAR13),
	REGISTER_NAME(DORCR),
	REGISTER_NAME(DPTSR),
	REGISTER_NAME(DAPTSR),
	REGISTER_NAME(DS1PR),
	REGISTER_NAME(DS2PR),
	REGISTER_NAME(YNCR),
	REGISTER_NAME(YNOR),
	REGISTER_NAME(CRNOR),
	REGISTER_NAME(CBNOR),
	REGISTER_NAME(RCRCR),
	REGISTER_NAME(GCRCR),
	REGISTER_NAME(GCBCR),
	REGISTER_NAME(BCBCR),
};
#undef REGISTER_NAME

char *rcar_du_regname(u32 longreg)
{
	unsigned int i;

	u32 reg = longreg & 0x1FFFF;

	for (i = 0; i < ARRAY_SIZE(regnames); i++)
		if (regnames[i].addr == reg)
			return regnames[i].name;

	return "";
}

/* -----------------------------------------------------------------------------
 * Power management
 */

#ifdef CONFIG_PM_SLEEP
static int rcar_du_pm_suspend(struct device *dev)
{
	struct rcar_du_device *rcdu = dev_get_drvdata(dev);

	return drm_mode_config_helper_suspend(rcdu->ddev);
}

static int rcar_du_pm_resume(struct device *dev)
{
	struct rcar_du_device *rcdu = dev_get_drvdata(dev);

	return drm_mode_config_helper_resume(rcdu->ddev);
}
#endif

static const struct dev_pm_ops rcar_du_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(rcar_du_pm_suspend, rcar_du_pm_resume)
};

/* -----------------------------------------------------------------------------
 * Platform driver
 */

static int rcar_du_remove(struct platform_device *pdev)
{
	struct rcar_du_device *rcdu = platform_get_drvdata(pdev);
	struct drm_device *ddev = rcdu->ddev;

	drm_dev_unregister(ddev);

	drm_kms_helper_poll_fini(ddev);
	drm_mode_config_cleanup(ddev);

	drm_dev_put(ddev);

	return 0;
}

static int rcar_du_probe(struct platform_device *pdev)
{
	struct rcar_du_device *rcdu;
	struct drm_device *ddev;
	struct resource *mem;
	int ret;

	/* Allocate and initialize the R-Car device structure. */
	rcdu = devm_kzalloc(&pdev->dev, sizeof(*rcdu), GFP_KERNEL);
	if (rcdu == NULL)
		return -ENOMEM;

	rcdu->dev = &pdev->dev;
	rcdu->info = of_device_get_match_data(rcdu->dev);

	platform_set_drvdata(pdev, rcdu);

	/* I/O resources */
	mem = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	rcdu->mmio = devm_ioremap_resource(&pdev->dev, mem);
	if (IS_ERR(rcdu->mmio))
		return PTR_ERR(rcdu->mmio);

	/* DRM/KMS objects */
	ddev = drm_dev_alloc(&rcar_du_driver, &pdev->dev);
	if (IS_ERR(ddev))
		return PTR_ERR(ddev);

	rcdu->ddev = ddev;
	ddev->dev_private = rcdu;

	ret = rcar_du_modeset_init(rcdu);
	if (ret < 0) {
		if (ret != -EPROBE_DEFER)
			dev_err(&pdev->dev,
				"failed to initialize DRM/KMS (%d)\n", ret);
		goto error;
	}

	ddev->irq_enabled = 1;

	/*
	 * Register the DRM device with the core and the connectors with
	 * sysfs.
	 */
	ret = drm_dev_register(ddev, 0);
	if (ret)
		goto error;

	DRM_INFO("Device %s probed\n", dev_name(&pdev->dev));

	drm_fbdev_generic_setup(ddev, 32);

	return 0;

error:
	rcar_du_remove(pdev);

	return ret;
}

static struct platform_driver rcar_du_platform_driver = {
	.probe		= rcar_du_probe,
	.remove		= rcar_du_remove,
	.driver		= {
		.name	= "rcar-du",
		.pm	= &rcar_du_pm_ops,
		.of_match_table = rcar_du_of_table,
	},
};

static int __init rcar_du_init(void)
{
	rcar_du_of_init(rcar_du_of_table);

	return platform_driver_register(&rcar_du_platform_driver);
}
module_init(rcar_du_init);

static void __exit rcar_du_exit(void)
{
	platform_driver_unregister(&rcar_du_platform_driver);
}
module_exit(rcar_du_exit);

MODULE_AUTHOR("Laurent Pinchart <laurent.pinchart@ideasonboard.com>");
MODULE_DESCRIPTION("Renesas R-Car Display Unit DRM Driver");
MODULE_LICENSE("GPL");
