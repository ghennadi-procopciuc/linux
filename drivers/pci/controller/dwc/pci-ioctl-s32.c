// SPDX-License-Identifier: GPL-2.0
/*
 * PCIe ioctl handler for Freescale S32 SoCs
 * This file was split from pci-s32v234.c
 *
 * Copyright (C) 2013 Kosagi
 *		http://www.kosagi.com
 *
 * Copyright (C) 2014-2015 Freescale Semiconductor, Inc. All Rights Reserved.
 * Copyright 2017-2021 NXP
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/mfd/syscon.h>
#include <linux/mfd/syscon/s32v234-src.h>
#include <linux/module.h>
#include <linux/of_gpio.h>
#include <linux/of_address.h>
#include <linux/pci.h>
#include <linux/pci_regs.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/io.h>
#include <linux/debugfs.h>
#include <linux/dma-mapping.h>
#include <linux/uaccess.h>
#include <linux/fs.h>
#include <linux/sizes.h>
#include <linux/of_platform.h>
#include <linux/rcupdate.h>
#include <linux/sched/signal.h>
#include <linux/version.h>

#include "pcie-designware.h"
#include "pci-dma-s32.h"
#include "pci-ioctl-s32.h"

struct task_struct *task;

#ifdef CONFIG_PCI_DW_DMA
static int s32_store_ll_array(struct dma_info *di, void __user *argp)
{
	int ret = 0;
	u32 ll_nr_elem = di->ll_info.nr_elem;

	if (argp && di->dma_linked_list) {
		if (copy_from_user(di->dma_linked_list, argp,
			sizeof(struct dma_list) * ll_nr_elem))
			return -EFAULT;
	} else {/* Null argument */
		return -EFAULT;
	}
	ret = dw_pcie_dma_load_linked_list(di,
		ll_nr_elem, di->ll_info.phy_list_addr,
		di->ll_info.next_phy_list_addr);

	return ret;
}

int s32_start_dma_ll(struct dma_info *di, void __user *argp)
{
	int ret = 0;
	u32 phy_addr;

	if (argp) {
		if (copy_from_user(&phy_addr, argp, sizeof(phy_addr)))
			return -EFAULT;
	} else {/* Null argument */
		return -EFAULT;
	}
	ret = dw_pcie_dma_start_linked_list(di,
		phy_addr);
	return ret;
}

int s32_store_ll_array_info(struct dma_info *di, void __user *argp)
{
	int ret = 0;

	if (argp) {
		if (copy_from_user(&di->ll_info, argp,
			sizeof(struct dma_ll_info)))
			return -EFAULT;
	} else {/* Null argument */
		return -EFAULT;
	}
	/* Alloc here space for pointer to array of structs */
	/* Make sure it is null before allocating space */
	if (!di->dma_linked_list) {
		di->dma_linked_list =
			(struct dma_list(*)[])kcalloc(di->ll_info.nr_elem,
				sizeof(struct dma_list), GFP_KERNEL);
	}

	return ret;
}

static int s32_send_dma_errors(struct dma_info *di, void __user *argp)
{
	int ret = 0, ch;
	u32 dma_errors = DMA_ERR_NONE;
	u32 wr_errors = DMA_ERR_NONE, rd_errors = DMA_ERR_NONE;

	/* Find first channels with write and read errors */
	for (ch = 0; ch < PCIE_DMA_NR_CH; ch++)
		if (di->wr_ch[ch].errors != DMA_ERR_NONE) {
			wr_errors = di->wr_ch[ch].errors;
			break;
		}
	for (ch = 0; ch < PCIE_DMA_NR_CH; ch++)
		if (di->rd_ch[ch].errors != DMA_ERR_NONE) {
			rd_errors = di->rd_ch[ch].errors;
			break;
		}

	dma_errors = (wr_errors << 16) | rd_errors;

	if (copy_to_user((unsigned int *)argp, &dma_errors, sizeof(u32)))
		return -EFAULT;
	return ret;
}

static int s32_send_dma_single(struct dma_info *di, void __user *argp)
{
	int ret = 0;
	struct dma_data_elem dma_elem_local;

	if (argp) {
		if (copy_from_user(&dma_elem_local, argp,
			sizeof(struct dma_data_elem)))
			return -EFAULT;
	} else
		return -EFAULT;
	ret = dw_pcie_dma_single_rw(di, &dma_elem_local);
	return ret;
}
#endif /* CONFIG_PCI_DW_DMA */

static int send_signal_to_user(struct s32_userspace_info *uinfo)
{
	int ret = 0;

	if (uinfo->user_pid > 0) {

		rcu_read_lock();
		task = pid_task(find_pid_ns(uinfo->user_pid, &init_pid_ns),
						PIDTYPE_PID);
		rcu_read_unlock();

		ret = send_sig_info(SIGUSR1, &uinfo->info, task);
		if (ret < 0)
			ret = -EFAULT;
	}

	return ret;
}

int s32_store_pid(struct s32_userspace_info *uinfo, void __user *argp)
{
	int ret = 0;

	if (argp) {
		if (copy_from_user(&uinfo->user_pid, argp,
				sizeof(uinfo->user_pid)))
			return -EFAULT;
	}
	return ret;
}

static int s32_get_bar_info(struct dw_pcie *pcie, void __user *argp)
{
	struct s32_bar bar_info;
	u8	bar_nr = 0;
	u32 addr = 0;
	int ret = 0;

	if (copy_from_user(&bar_info, argp, sizeof(bar_info))) {
		dev_err(pcie->dev, "Error while copying from user\n");
		return -EFAULT;
	}
	if (bar_info.bar_nr)
		bar_nr = bar_info.bar_nr;

	addr = readl(pcie->dbi_base + (PCI_BASE_ADDRESS_0 +
				bar_info.bar_nr * 4));
	bar_info.addr = addr & 0xFFFFFFF0;
	writel(0xFFFFFFFF, pcie->dbi_base +
		(PCI_BASE_ADDRESS_0 + bar_nr * 4));
	bar_info.size = readl(pcie->dbi_base +
		(PCI_BASE_ADDRESS_0 + bar_nr * 4));
	bar_info.size = ~(bar_info.size & 0xFFFFFFF0) + 1;
	writel(addr, pcie->dbi_base +
		(PCI_BASE_ADDRESS_0 + bar_nr * 4));

	if (copy_to_user(argp, &bar_info, sizeof(bar_info)))
		return -EFAULT;

	return ret;
}

static ssize_t s32_ioctl(struct file *filp, u32 cmd,
		unsigned long data)
{
	int ret = 0;
	void __user *argp = (void __user *)data;
	struct dw_pcie *pcie = (struct dw_pcie *)(filp->private_data);
	struct s32_userspace_info *uinfo = dw_get_userspace_info(pcie);
#ifdef CONFIG_PCI_DW_DMA
	struct dma_info *di = dw_get_dma_info(pcie);
#endif
	struct s32_inbound_region	inbStr;
	struct s32_outbound_region	outbStr;

	switch (cmd) {
		/* Call to retrieve BAR setup*/
	case GET_BAR_INFO:
		ret = s32_get_bar_info(pcie, argp);
		break;
	case SETUP_OUTBOUND:
		/* Call to setup outbound region */
		if (copy_from_user(&outbStr, argp, sizeof(outbStr)))
			return -EFAULT;
		ret = s32_pcie_setup_outbound(&outbStr);
		return ret;
	case SETUP_INBOUND:
		/* Call to setup inbound region */
		if (copy_from_user(&inbStr, argp, sizeof(inbStr)))
			return -EFAULT;
		ret = s32_pcie_setup_inbound(&inbStr);
		return ret;
	case SEND_MSI:
		/* Setup MSI */
		/* TODO: allow selection of the MSI index and
		 * also trigger the interrupts and catch them on the
		 * receiver side
		 */
		ret = s32_set_msi(pcie);
		return ret;
	case STORE_PID:
		ret = s32_store_pid(uinfo, argp);
		return ret;
	case SEND_SIGNAL:
		ret = send_signal_to_user(uinfo);
		return ret;
#ifdef CONFIG_PCI_DW_DMA
	case SEND_SINGLE_DMA:
		ret = s32_send_dma_single(di, argp);
		return ret;
	case GET_DMA_CH_ERRORS:
		ret = s32_send_dma_errors(di, argp);
		return ret;
	case RESET_DMA_WRITE:
		dw_pcie_dma_write_soft_reset(di);
		return ret;
	case RESET_DMA_READ:
		dw_pcie_dma_read_soft_reset(di);
		return ret;
	case STORE_LL_INFO:
		ret = s32_store_ll_array_info(di, argp);
		return ret;
	case SEND_LL:
		ret = s32_store_ll_array(di, argp);
		return ret;
	case START_LL:
		ret = s32_start_dma_ll(di, argp);
		return ret;
#endif
	default:
		return -EINVAL;
	}
	return ret;
}

static const struct file_operations s32_pcie_ep_dbgfs_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.unlocked_ioctl = s32_ioctl,
};

void s32_config_user_space_data(struct s32_userspace_info *uinfo,
		struct dw_pcie *pcie)
{
	struct dentry *pfile;

	uinfo->send_signal_to_user = send_signal_to_user;
	uinfo->user_pid = 0;

	/* Init signal info data */
	memset(&uinfo->info, 0, sizeof(struct kernel_siginfo));
	uinfo->info.si_signo = SIGUSR1;
	uinfo->info.si_code = SI_USER;
	uinfo->info.si_int = 0;

	/* Init debugfs entry */
	uinfo->dir = debugfs_create_dir("ep_dbgfs", NULL);
	if (!uinfo->dir)
		dev_info(pcie->dev, "Creating debugfs dir failed\n");
	pfile = debugfs_create_file("ep_file", 0444, uinfo->dir,
		(void *)pcie, &s32_pcie_ep_dbgfs_fops);
	if (!pfile)
		dev_info(pcie->dev, "debugfs regs for failed\n");
}
