/* SPDX-License-Identifier: (GPL-2.0+ OR BSD-3-Clause) */
/*
 * S32CC pinmux core definitions
 *
 * Copyright 2016-2018, 2020 NXP
 * Copyright 2015-2016 Freescale Semiconductor, Inc.
 * Copyright (C) 2012 Linaro Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef __DRIVERS_PINCTRL_S32CC_H
#define __DRIVERS_PINCTRL_S32CC_H

/**
 * Pinctrl driver versions
 */
enum s32cc_pinctrl_version {
	/* Pinctrl driver reserves all SIUL2 registers */
	PINCTRL_V1,
	/* Pinctrl driver reserves only pinctrl registers */
	PINCTRL_V2,
};

struct platform_device;

/**
 * struct s32cc_pin - describes a single S32 pin
 * @pin_id: the pin_id of this pin
 * @sss: source signal select of the pin
 */
struct s32cc_pin {
	unsigned int pin_id;
	unsigned long sss;
};

/**
 * struct s32cc_pin_group - describes an S32 pin group
 * @name: the name of this specific pin group
 * @npins: the number of pins in this group array, i.e. the number of
 *	elements in .pins so we can iterate over that array
 * @pin_ids: array of pin_ids. pinctrl forces us to maintain such an array
 * @pins: array of pins
 */
struct s32cc_pin_group {
	const char *name;
	unsigned int npins;
	unsigned int *pin_ids;
	struct s32cc_pin *pins;
};

/**
 * struct s32cc_pmx_func - describes S32 pinmux functions
 * @name: the name of this specific function
 * @groups: corresponding pin groups
 * @num_groups: the number of groups
 */
struct s32cc_pmx_func {
	const char *name;
	const char **groups;
	unsigned int num_groups;
};


struct s32cc_pinctrl_soc_info {
	struct device *dev;
	const struct pinctrl_pin_desc *pins;
	unsigned int npins;
	struct s32cc_pin_group *groups;
	unsigned int ngroups;
	struct s32cc_pmx_func *functions;
	unsigned int nfunctions;
	unsigned int flags;
	unsigned int grp_index;
	unsigned int mem_regions;
};

#define S32CC_PINCTRL_PIN(pin)	PINCTRL_PIN(pin, #pin)
#define S32CC_MSCR_OFFSET	(0x240)
#define S32CC_PAD_CONFIG(idx)	((idx) * 4)

int s32cc_pinctrl_probe(struct platform_device *pdev,
			struct s32cc_pinctrl_soc_info *info);
int s32cc_pinctrl_remove(struct platform_device *pdev);
#ifdef CONFIG_PM_SLEEP
int s32cc_pinctrl_resume(struct device *dev);
int s32cc_pinctrl_suspend(struct device *dev);
#endif
#endif /* __DRIVERS_PINCTRL_S32CC_H */

