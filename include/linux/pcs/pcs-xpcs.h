/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2020 Synopsys, Inc. and/or its affiliates.
 * Synopsys DesignWare XPCS helpers
 */

#ifndef __LINUX_PCS_XPCS_H
#define __LINUX_PCS_XPCS_H

#include <linux/phy.h>
#include <linux/phylink.h>

#define NXP_SJA1105_XPCS_ID		0x00000010
#define NXP_SJA1110_XPCS_ID		0x00000020

/* AN mode */
#define DW_AN_C73			1
#define DW_AN_C37_SGMII			2
#define DW_2500BASEX			3

struct dw_xpcs;

enum {
	DW_XPCS_USXGMII,
	DW_XPCS_10GKR,
	DW_XPCS_XLGMII,
	DW_XPCS_SGMII,
	DW_XPCS_2500BASEX,
	DW_XPCS_INTERFACE_MAX,
};

struct xpcs_compat {
	const int *supported;
	const phy_interface_t *interface;
	int num_interfaces;
	int an_mode;
	int (*pma_config)(struct dw_xpcs *xpcs);
};

struct xpcs_id {
	u32 id;
	u32 mask;
	const struct xpcs_compat *compat;
};

struct dw_xpcs {
	struct mdio_device *mdiodev;
	const struct xpcs_id *id;
	struct phylink_pcs pcs;
};

int xpcs_get_an_mode(struct dw_xpcs *xpcs, phy_interface_t interface);
void xpcs_link_up(struct phylink_pcs *pcs, unsigned int mode,
		  phy_interface_t interface, int speed, int duplex);
int xpcs_do_config(struct dw_xpcs *xpcs, phy_interface_t interface,
		   unsigned int mode);
void xpcs_validate(struct dw_xpcs *xpcs, unsigned long *supported,
		   struct phylink_link_state *state);
int xpcs_config_eee(struct dw_xpcs *xpcs, int mult_fact_100ns,
		    int enable);
struct dw_xpcs *xpcs_create(struct mdio_device *mdiodev,
			    phy_interface_t interface);
void xpcs_destroy(struct dw_xpcs *xpcs);

#endif /* __LINUX_PCS_XPCS_H */
