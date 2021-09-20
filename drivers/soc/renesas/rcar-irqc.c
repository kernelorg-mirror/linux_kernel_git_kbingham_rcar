// SPDX-License-Identifier: GPL-2.0

#include <linux/compiler.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/slab.h>

#define IRQC_IRQ_MAX	32	/* maximum 32 interrupts per driver instance */

#define IRQC_REQ_STS	0x00	/* Interrupt Request Status Register */
#define IRQC_EN_STS	0x04	/* Interrupt Enable Status Register */
#define IRQC_EN_SET	0x08	/* Interrupt Enable Set Register */
#define IRQC_INT_CPU_BASE(n) (0x000 + ((n) * 0x10))
				/* SYS-CPU vs. RT-CPU */
#define DETECT_STATUS	0x100	/* IRQn Detect Status Register */
#define MONITOR		0x104	/* IRQn Signal Level Monitor Register */
#define HLVL_STS	0x108	/* IRQn High Level Detect Status Register */
#define LLVL_STS	0x10c	/* IRQn Low Level Detect Status Register */
#define S_R_EDGE_STS	0x110	/* IRQn Sync Rising Edge Detect Status Reg. */
#define S_F_EDGE_STS	0x114	/* IRQn Sync Falling Edge Detect Status Reg. */
#define A_R_EDGE_STS	0x118	/* IRQn Async Rising Edge Detect Status Reg. */
#define A_F_EDGE_STS	0x11c	/* IRQn Async Falling Edge Detect Status Reg. */
#define CHTEN_STS	0x120	/* Chattering Reduction Status Register */
#define IRQC_CONFIG(n) (0x180 + ((n) * 0x04))
				/* IRQn Configuration Register */

static void __iomem *irqc;

static int irqc_proc_show(struct seq_file *m, void *v)
{
	seq_puts(m, "-LEVEL-- --HIGH-- --LOW--- -S-RISE- -S-FALL- -A-RISE- -A-FALL-\n");
	seq_printf(m, "%08x %08x %08x %08x %08x %08x %08x\n",
		   ioread32(irqc + MONITOR), ioread32(irqc + HLVL_STS),
		   ioread32(irqc + LLVL_STS), ioread32(irqc + S_R_EDGE_STS),
		   ioread32(irqc + S_F_EDGE_STS),
		   ioread32(irqc + A_R_EDGE_STS),
		   ioread32(irqc + A_F_EDGE_STS));
	return 0;
}

static int irqc_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, irqc_proc_show, NULL);
}

static const struct proc_ops irqc_proc_ops = {
	.proc_open	= irqc_proc_open,
	.proc_read	= seq_read,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
};

static int proc_irqc_setup(unsigned long phys)
{
	if (irqc) {
		pr_warn("GPIO already set up for different CPU\n");
		return -EINVAL;
	}

	irqc = ioremap(phys, PAGE_SIZE);
	if (!irqc) {
		pr_err("Cannot ioremap IRQC regs\n");
		return -ENOMEM;
	}

	return 0;
}

static int __init proc_irqc_init(void)
{
	int error = -ENODEV;

#ifdef CONFIG_ARCH_RENESAS
	if (of_machine_is_compatible("renesas,r8a73a4") ||
	    of_machine_is_compatible("renesas,r8a7742") ||
	    of_machine_is_compatible("renesas,r8a7743") ||
	    of_machine_is_compatible("renesas,r8a7744") ||
	    of_machine_is_compatible("renesas,r8a7745") ||
	    of_machine_is_compatible("renesas,r8a7790") ||
	    of_machine_is_compatible("renesas,r8a7791") ||
	    of_machine_is_compatible("renesas,r8a7792") ||
	    of_machine_is_compatible("renesas,r8a7793") ||
	    of_machine_is_compatible("renesas,r8a7794") ||
	    of_machine_is_compatible("renesas,r8a7795") ||
	    of_machine_is_compatible("renesas,r8a7796") ||
	    of_machine_is_compatible("renesas,r8a77965") ||
	    of_machine_is_compatible("renesas,r8a77970") ||
	    of_machine_is_compatible("renesas,r8a77980") ||
	    of_machine_is_compatible("renesas,r8a77990") ||
	    of_machine_is_compatible("renesas,r8a77995") ||
	    of_machine_is_compatible("renesas,r8a779a0"))
		error = proc_irqc_setup(0xe61c0000);
#endif

	if (error)
		return error;

	proc_create("irqc", 0, NULL, &irqc_proc_ops);
	return 0;
}
fs_initcall(proc_irqc_init);
