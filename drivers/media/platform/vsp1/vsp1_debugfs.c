/*
 * vsp1_debugfs.c  --  R-Car VSP1 driver debug support
 *
 * Copyright (C) 2016 Renesas Electronics Corporation
 *
 * Contact: Kieran Bingham (kieran@bingham.xyz)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <linux/module.h>
#include <linux/slab.h>

#include "vsp1.h"
#include "vsp1_rwpf.h"
#include "vsp1_pipe.h"

#include "vsp1_debugfs.h"

/* -----------------------------------------------------------------------------
 * Data Tables
 */

/*
 * Register maps can be generated with something similar to this expression:
 *  cat vsp1_regs.h | \
 *	grep -E "#define VI6_.*0[xX][0-9a-fA-F]{4}$" | \
 *	sed -r 's/^#define (VI6\w*).*$/\tVSP1_DBFS_REG(\1),/'
 */

/* Do not use __stringify() here as that will expand the macros */
#define VSP1_DBFS_REG(reg) { #reg, reg, NULL }
#define VSP1_DBFS_REG_DECODE(reg, func)  { #reg, reg, func }

static void decode_vi6_status(struct seq_file *s, u32 val)
{
	seq_printf(s, " WPF0 = %s : WPF1 = %s",
		   val & VI6_STATUS_SYS_ACT(0) ? "active" : "inactive",
		   val & VI6_STATUS_SYS_ACT(1) ? "active" : "inactive");
}

static void decode_vi6_disp_irq_sta(struct seq_file *s, u32 val)
{
	seq_printf(s, " %s%s%s%s%s%s%s",
		   val & VI6_DISP_IRQ_STA_DST ? " DST" : "",
		   val & VI6_DISP_IRQ_STA_MAE ? " MAE" : "",
		   val & VI6_DISP_IRQ_STA_LNE(0) ? " LNE(0)" : "",
		   val & VI6_DISP_IRQ_STA_LNE(1) ? " LNE(1)" : "",
		   val & VI6_DISP_IRQ_STA_LNE(2) ? " LNE(2)" : "",
		   val & VI6_DISP_IRQ_STA_LNE(3) ? " LNE(3)" : "",
		   val & VI6_DISP_IRQ_STA_LNE(4) ? " LNE(4)" : "");
}

static void decode_vi6_wpf_enb(struct seq_file *s, u32 enable)
{
	seq_printf(s, " %s%s%s",
		   enable & VI6_WPF_IRQ_ENB_UNDE ? " UND" : "",
		   enable & VI6_WPF_IRQ_ENB_DFEE ? " DFE" : "",
		   enable & VI6_WPF_IRQ_ENB_FREE ? " FRE" : "");
}

static void decode_vi6_wpf_sta(struct seq_file *s, u32 status)
{
	seq_printf(s, " %s%s%s",
		   status & VI6_WPF_IRQ_STA_UND ? " UND" : "",
		   status & VI6_WPF_IRQ_STA_DFE ? " DFE" : "",
		   status & VI6_WPF_IRQ_STA_FRE ? " FRE" : "");
}

static void decode_vi6_wpf_wrbck_ctrl(struct seq_file *s, u32 reg)
{
	seq_printf(s, " %s",
		   reg & VI6_WPF_WRBCK_CTRL_WBMD ? " WBMD" : "");
}

/* RPF is a special case that defines multiple sets of the same registers */
#define VSP1_DBFS_RPF_REG(reg, idx) \
	{ #reg "[" #idx "]", (reg + idx * VI6_RPF_OFFSET), NULL }

#define VSP1_DBFS_RPF(index) \
	\
	VSP1_DBFS_RPF_REG(VI6_RPF_SRC_BSIZE, index), \
	VSP1_DBFS_RPF_REG(VI6_RPF_SRC_ESIZE, index), \
	VSP1_DBFS_RPF_REG(VI6_RPF_INFMT, index), \
	VSP1_DBFS_RPF_REG(VI6_RPF_DSWAP, index), \
	VSP1_DBFS_RPF_REG(VI6_RPF_LOC, index), \
	VSP1_DBFS_RPF_REG(VI6_RPF_ALPH_SEL, index), \
	VSP1_DBFS_RPF_REG(VI6_RPF_VRTCOL_SET, index), \
	VSP1_DBFS_RPF_REG(VI6_RPF_MSK_CTRL, index), \
	VSP1_DBFS_RPF_REG(VI6_RPF_MSK_SET0, index), \
	VSP1_DBFS_RPF_REG(VI6_RPF_MSK_SET1, index), \
	VSP1_DBFS_RPF_REG(VI6_RPF_CKEY_CTRL, index), \
	VSP1_DBFS_RPF_REG(VI6_RPF_CKEY_SET0, index), \
	VSP1_DBFS_RPF_REG(VI6_RPF_CKEY_SET1, index), \
	VSP1_DBFS_RPF_REG(VI6_RPF_SRCM_PSTRIDE, index), \
	VSP1_DBFS_RPF_REG(VI6_RPF_SRCM_ASTRIDE, index), \
	VSP1_DBFS_RPF_REG(VI6_RPF_SRCM_ADDR_Y, index), \
	VSP1_DBFS_RPF_REG(VI6_RPF_SRCM_ADDR_C0, index), \
	VSP1_DBFS_RPF_REG(VI6_RPF_SRCM_ADDR_C1, index), \
	VSP1_DBFS_RPF_REG(VI6_RPF_SRCM_ADDR_AI, index), \
	VSP1_DBFS_RPF_REG(VI6_RPF_MULT_ALPHA, index)

static const struct debugfs_reg32 vsp1_regset[] = {
	VSP1_DBFS_REG(VI6_CMD(0)),
	VSP1_DBFS_REG(VI6_CMD(1)),

	VSP1_DBFS_REG(VI6_CLK_DCSWT),

	VSP1_DBFS_REG(VI6_SRESET),

	VSP1_DBFS_REG_DECODE(VI6_STATUS, decode_vi6_status),

	VSP1_DBFS_REG_DECODE(VI6_WPF_IRQ_ENB(0), decode_vi6_wpf_enb),
	VSP1_DBFS_REG_DECODE(VI6_WPF_IRQ_ENB(1), decode_vi6_wpf_enb),
	VSP1_DBFS_REG_DECODE(VI6_WPF_IRQ_STA(0), decode_vi6_wpf_sta),
	VSP1_DBFS_REG_DECODE(VI6_WPF_IRQ_STA(1), decode_vi6_wpf_sta),

	VSP1_DBFS_REG(VI6_DISP_IRQ_ENB(0)),
	VSP1_DBFS_REG(VI6_DISP_IRQ_ENB(1)),

	VSP1_DBFS_REG_DECODE(VI6_DISP_IRQ_STA(0), decode_vi6_disp_irq_sta),
	VSP1_DBFS_REG_DECODE(VI6_DISP_IRQ_STA(1), decode_vi6_disp_irq_sta),

	VSP1_DBFS_REG(VI6_DL_CTRL),
	VSP1_DBFS_REG(VI6_DL_SWAP),
	VSP1_DBFS_REG(VI6_DL_EXT_CTRL(0)),
	VSP1_DBFS_REG(VI6_DL_EXT_CTRL(1)),

	VSP1_DBFS_REG(VI6_DL_BODY_SIZE),

	VSP1_DBFS_RPF(0),
	VSP1_DBFS_RPF(1),
	VSP1_DBFS_RPF(2),
	VSP1_DBFS_RPF(3),
	VSP1_DBFS_RPF(4),

	VSP1_DBFS_REG(VI6_WPF_SRCRPF),
	VSP1_DBFS_REG(VI6_WPF_HSZCLIP),
	VSP1_DBFS_REG(VI6_WPF_VSZCLIP),
	VSP1_DBFS_REG(VI6_WPF_OUTFMT),
	VSP1_DBFS_REG(VI6_WPF_DSWAP),
	VSP1_DBFS_REG(VI6_WPF_RNDCTRL),
	VSP1_DBFS_REG(VI6_WPF_ROT_CTRL),
	VSP1_DBFS_REG(VI6_WPF_DSTM_STRIDE_Y),
	VSP1_DBFS_REG(VI6_WPF_DSTM_STRIDE_C),
	VSP1_DBFS_REG(VI6_WPF_DSTM_ADDR_Y),
	VSP1_DBFS_REG(VI6_WPF_DSTM_ADDR_C0),
	VSP1_DBFS_REG(VI6_WPF_DSTM_ADDR_C1),

	VSP1_DBFS_REG_DECODE(VI6_WPF_WRBCK_CTRL(0), decode_vi6_wpf_wrbck_ctrl),

	VSP1_DBFS_REG(VI6_DPR_RPF_ROUTE(0)),
	VSP1_DBFS_REG(VI6_DPR_RPF_ROUTE(1)),
	VSP1_DBFS_REG(VI6_DPR_RPF_ROUTE(2)),
	VSP1_DBFS_REG(VI6_DPR_RPF_ROUTE(3)),
	VSP1_DBFS_REG(VI6_DPR_RPF_ROUTE(4)),

	VSP1_DBFS_REG(VI6_DPR_WPF_FPORCH(0)),
	VSP1_DBFS_REG(VI6_DPR_WPF_FPORCH(1)),

	VSP1_DBFS_REG(VI6_DPR_SRU_ROUTE),

	VSP1_DBFS_REG(VI6_DPR_UDS_ROUTE(0)),

	VSP1_DBFS_REG(VI6_DPR_LUT_ROUTE),
	VSP1_DBFS_REG(VI6_DPR_CLU_ROUTE),
	VSP1_DBFS_REG(VI6_DPR_HST_ROUTE),
	VSP1_DBFS_REG(VI6_DPR_HSI_ROUTE),
	VSP1_DBFS_REG(VI6_DPR_BRU_ROUTE),
	VSP1_DBFS_REG(VI6_DPR_HGO_SMPPT),
	VSP1_DBFS_REG(VI6_DPR_HGT_SMPPT),

	VSP1_DBFS_REG(VI6_SRU_CTRL0),
	VSP1_DBFS_REG(VI6_SRU_CTRL1),
	VSP1_DBFS_REG(VI6_SRU_CTRL2),

	VSP1_DBFS_REG(VI6_UDS_CTRL),
	VSP1_DBFS_REG(VI6_UDS_SCALE),
	VSP1_DBFS_REG(VI6_UDS_ALPTH),
	VSP1_DBFS_REG(VI6_UDS_ALPVAL),
	VSP1_DBFS_REG(VI6_UDS_PASS_BWIDTH),
	VSP1_DBFS_REG(VI6_UDS_HPHASE),
	VSP1_DBFS_REG(VI6_UDS_IPC),
	VSP1_DBFS_REG(VI6_UDS_HSZCLIP),
	VSP1_DBFS_REG(VI6_UDS_CLIP_SIZE),
	VSP1_DBFS_REG(VI6_UDS_FILL_COLOR),

	VSP1_DBFS_REG(VI6_LUT_CTRL),
	VSP1_DBFS_REG(VI6_CLU_CTRL),
	VSP1_DBFS_REG(VI6_HST_CTRL),
	VSP1_DBFS_REG(VI6_HSI_CTRL),

	VSP1_DBFS_REG(VI6_BRU_INCTRL),
	VSP1_DBFS_REG(VI6_BRU_VIRRPF_SIZE),
	VSP1_DBFS_REG(VI6_BRU_VIRRPF_LOC),
	VSP1_DBFS_REG(VI6_BRU_VIRRPF_COL),

	VSP1_DBFS_REG(VI6_BRU_CTRL(0)),
	VSP1_DBFS_REG(VI6_BRU_CTRL(1)),
	VSP1_DBFS_REG(VI6_BRU_CTRL(2)),
	VSP1_DBFS_REG(VI6_BRU_CTRL(3)),
	VSP1_DBFS_REG(VI6_BRU_CTRL(4)),

	VSP1_DBFS_REG(VI6_BRU_BLD(0)),
	VSP1_DBFS_REG(VI6_BRU_BLD(1)),
	VSP1_DBFS_REG(VI6_BRU_BLD(2)),
	VSP1_DBFS_REG(VI6_BRU_BLD(3)),
	VSP1_DBFS_REG(VI6_BRU_BLD(4)),

	VSP1_DBFS_REG(VI6_BRU_ROP),

	VSP1_DBFS_REG(VI6_HGO_OFFSET),
	VSP1_DBFS_REG(VI6_HGO_SIZE),
	VSP1_DBFS_REG(VI6_HGO_MODE),
	VSP1_DBFS_REG(VI6_HGO_LB_TH),
	//VSP1_DBFS_REG(VI6_HGO_R_HISTO(0)),
	VSP1_DBFS_REG(VI6_HGO_R_MAXMIN),
	VSP1_DBFS_REG(VI6_HGO_R_SUM),
	VSP1_DBFS_REG(VI6_HGO_R_LB_DET),
	//VSP1_DBFS_REG(VI6_HGO_G_HISTO(0)),
	VSP1_DBFS_REG(VI6_HGO_G_MAXMIN),
	VSP1_DBFS_REG(VI6_HGO_G_SUM),
	VSP1_DBFS_REG(VI6_HGO_G_LB_DET),
	//VSP1_DBFS_REG(VI6_HGO_B_HISTO(0)),
	VSP1_DBFS_REG(VI6_HGO_B_MAXMIN),
	VSP1_DBFS_REG(VI6_HGO_B_SUM),
	VSP1_DBFS_REG(VI6_HGO_B_LB_DET),
	VSP1_DBFS_REG(VI6_HGO_REGRST),

	VSP1_DBFS_REG(VI6_HGT_OFFSET),
	VSP1_DBFS_REG(VI6_HGT_SIZE),
	VSP1_DBFS_REG(VI6_HGT_MODE),
	VSP1_DBFS_REG(VI6_HGT_LB_TH),
	VSP1_DBFS_REG(VI6_HGT_MAXMIN),
	VSP1_DBFS_REG(VI6_HGT_SUM),
	VSP1_DBFS_REG(VI6_HGT_LB_DET),
	VSP1_DBFS_REG(VI6_HGT_REGRST),

	VSP1_DBFS_REG(VI6_LIF_CTRL),
	VSP1_DBFS_REG(VI6_LIF_CSBTH),
	VSP1_DBFS_REG(VI6_SECURITY_CTRL0),
	VSP1_DBFS_REG(VI6_SECURITY_CTRL1),
	VSP1_DBFS_REG(VI6_IP_VERSION),
	/* VSP-D's don't have this and will segfault if you try to read them */
	//VSP1_DBFS_REG(VI6_CLUT_TABLE),
	//VSP1_DBFS_REG(VI6_LUT_TABLE),
	//VSP1_DBFS_REG(VI6_CLU_ADDR),
	//VSP1_DBFS_REG(VI6_CLU_DATA),
};

/*
 * vsp1_reg_to_name
 *
 * Find the name of the register which matches the offset given.
 * This function assumes that the regset has only unique offsets
 * in the table.
 */
char *vsp1_reg_to_name(u32 offset)
{
	unsigned int i;
	static char notfound[16];

	for (i = 0; i < ARRAY_SIZE(vsp1_regset); i++)
		if (vsp1_regset[i].offset == offset)
			return vsp1_regset[i].name;

	snprintf(notfound, sizeof(notfound), "<0x%08x>", offset);
	return notfound;
}

/*
 * vsp1_reg_decode
 *
 * Find the name of the register which matches the offset given.
 * This function assumes that the regset has only unique offsets
 * in the table.
 */
static int vsp1_reg_decode(struct seq_file *s, u32 offset, u32 value)
{
	unsigned int i;
	const struct debugfs_reg32 *reg = NULL;

	for (i = 0; i < ARRAY_SIZE(vsp1_regset); i++)
		if (vsp1_regset[i].offset == offset) {
			reg = &vsp1_regset[i];
			break;
		}

	if (reg) {
		seq_printf(s, "%0x08x : %s", value, reg->name);
		if (reg->decode_reg)
			reg->decode_reg(s, value);
		seq_puts(s, "\n");
	} else {
		seq_printf(s, "%0x08x : <0x%08x not found>\n", value, offset);
	}

	return 0;
}

/*
 * vsp1_reg_read_and_decode
 *
 * Read a register, and if available decode the value into a seq_buffer.
 * Returns the value, and decode using seq_printf formatting, or just the value
 * to the string buffer if no decoding is available.
 */
static void vsp1_reg_read_and_decode(struct vsp1_device *vsp1,
				     struct seq_file *s,
				     struct debugfs_reg32 *reg)
{
	u32 value = vsp1_read(vsp1, reg->offset);

	if (reg->decode_reg)
		return reg->decode_reg(s, value);

	return seq_printf(s, "0x%08x", value);
}

/* -----------------------------------------------------------------------------
 * Debugfs management
 */

static void vsp1_debugfs_print_vsp1_pipe(struct seq_file *seq,
					 struct vsp1_pipeline *pipe, char *msg)
{

	if (!pipe) {
		seq_printf(seq, "No Pipe @ %p : %s\n", pipe, msg);

		return;
	}

	seq_printf(seq,	"%s Pipe %s\n", msg,
			pipe->state == VSP1_PIPELINE_STOPPED ? "Stopped" :
			pipe->state == VSP1_PIPELINE_STOPPING ? "Stopping" :
			pipe->state == VSP1_PIPELINE_RUNNING ? "Running" : "X");

	seq_printf(seq,	"stream_count: %d\n"
			"buffers_ready: %d\n"
			"sequence: %d\n"
			"num_inputs: %d\n"
			"partitions: %d\n",
			pipe->stream_count,
			pipe->buffers_ready,
			pipe->sequence,
			pipe->num_inputs,
			pipe->partitions);

}

static int vsp1_debugfs_info(struct seq_file *s, void *p)
{
	struct vsp1_device *vsp1 = s->private;
	unsigned int i;
	u32 status;

	/* Make sure all reads are with 'powered' device */
	vsp1_device_get(vsp1);

	seq_printf(s, "name: %s\n", vsp1->info->model ? vsp1->info->model : "");

	status = vsp1_read(vsp1, VI6_STATUS);
	seq_printf(s, "VI6_STATUS = 0x%08x : ", status);
	decode_vi6_status(s, status);
	seq_puts(s, "\n");

	seq_printf(s, "VI6_WPF0_FRCNT = 0x%08x", vsp1_read(vsp1, VI6_WPF0_FRCNT));

	for (i = 0; i < vsp1->info->wpf_count; ++i) {
		struct vsp1_rwpf *wpf = vsp1->wpf[i];
		u32 status;
		u32 enable;

		if (wpf == NULL)
			continue;

		enable = vsp1_read(vsp1, VI6_WPF_IRQ_ENB(i));
		status = vsp1_read(vsp1, VI6_WPF_IRQ_STA(i));

		seq_printf(s, "VI6_WPF_IRQ_ENB(%d) = ", i);
		decode_vi6_wpf_enb(s, enable);
		seq_puts(s, "\n");

		seq_printf(s, "VI6_WPF_IRQ_STA(%d) = ", i);
		decode_vi6_wpf_sta(s, status);
		seq_puts(s, "\n");

		seq_printf(s, "WPF[%d]->entity->Pipe = %p\n", i, wpf->entity.pipe);
		/* Report the WPF Pipe information */
		vsp1_debugfs_print_vsp1_pipe(s, wpf->entity.pipe, "WPF");

		status = vsp1_read(vsp1, VI6_DISP_IRQ_STA(i));
		seq_printf(s, "VI6_DISP_IRQ_STA(%d) = ", i);
		decode_vi6_disp_irq_sta(s, status);
		seq_puts(s, "\n");
	}

	for (i = 0; i < vsp1->info->rpf_count; ++i) {
		struct vsp1_rwpf *rpf = vsp1->rpf[i];

		if (rpf == NULL)
			continue;

		seq_printf(s,
			"RPF[%d]->entity.pipe = %p\n", i, rpf->entity.pipe);

		vsp1_debugfs_print_vsp1_pipe(s, rpf->entity.pipe, "RPF");

	}

	vsp1_device_put(vsp1);

	return 0;
}
DEBUGFS_RO_ATTR(vsp1_debugfs_info);

static int vsp1_debugfs_regs(struct seq_file *s, void *p)
{
	struct vsp1_device *vsp1 = s->private;
	unsigned int i;
	u32 status;

	/* Make sure all reads are with 'powered' device */
	vsp1_device_get(vsp1);

	for (i = 0; i < ARRAY_SIZE(vsp1_regset); i++) {
		const struct debugfs_reg32 *reg = &vsp1_regset[i];

		status = vsp1_read(vsp1, reg->offset);

		seq_printf(s, "0x%08x [%s]\n", status, reg->name);
	}

	vsp1_device_put(vsp1);

	return 0;
}
DEBUGFS_RO_ATTR(vsp1_debugfs_regs);

static int vsp1_debugfs_reset_wpf(struct seq_file *s, void *p)
{
	struct vsp1_device *vsp1 = s->private;
	int ret;

	/* Make sure all reads are with 'powered' device */
	vsp1_device_get(vsp1);

	/* Prints are 'before' and 'after' to preserve timing on output */
	seq_puts(s, "Resetting WPF[0] : ");

	ret = vsp1_reset_wpf(vsp1->wpf[0]->entity.vsp1,
			     vsp1->wpf[0]->entity.index);
	if (ret == 0)
		seq_puts(s, "Success\n");
	else
		seq_puts(s, "Failed\n");

	vsp1_device_put(vsp1);

	return 0;
}
DEBUGFS_RO_ATTR(vsp1_debugfs_reset_wpf);

/* Debugfs initialised after entities are created */
int vsp1_debugfs_init(struct vsp1_device *vsp1)
{
	struct dentry *info_file;

	vsp1->regset.regs = vsp1_regset;
	vsp1->regset.base = vsp1->mmio;
	vsp1->regset.nregs = ARRAY_SIZE(vsp1_regset);

	vsp1->dbgroot = debugfs_create_dir(dev_name(vsp1->dev), NULL);
	if (!vsp1->dbgroot)
		return -ENOMEM;

	/* dentry pointer discarded */
	info_file = debugfs_create_file("info", 0444,
						 vsp1->dbgroot,
						 vsp1,
						 &vsp1_debugfs_info_fops);

	/* dentry pointer discarded */
	info_file = debugfs_create_file("regs_local", 0444,
						 vsp1->dbgroot,
						 vsp1,
						 &vsp1_debugfs_regs_fops);

	/* dentry pointer discarded */
	info_file = debugfs_create_file("reset_wpf0", 0444,
						 vsp1->dbgroot,
						 vsp1,
						 &vsp1_debugfs_reset_wpf_fops);

	debugfs_create_regset32("regs", 0444, vsp1->dbgroot, &vsp1->regset);

	return 0;
}

void vsp1_debugfs_remove(struct vsp1_device *vsp1)
{
	debugfs_remove_recursive(vsp1->dbgroot);
	vsp1_device_put(vsp1);
}
