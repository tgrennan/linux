/**
 * SPDX-License-Identifier: GPL-2.0
 * Copyright(c) 2018-2020 Platina Systems, Inc.
 *
 * Contact Information:
 * sw@platina.com
 * Platina Systems, 3180 Del La Cruz Blvd, Santa Clara, CA 95054
 */

#include "xeth_platform.h"
#include "xeth_port.h"
#include "xeth_debug.h"
#include <linux/gpio/consumer.h>
#include <linux/gpio/machine.h>

enum {
	xeth_platina_mk1_ports = 32,
};

static const char * const xeth_platina_mk1_port_et_priv_flag_names[] = {
	"copper",
	"fec74",
	"fec91",
	NULL,
};

static struct xeth_platina_mk1 {
	struct net_device *links[3];
	struct i2c_client **qsfps;
	struct xeth_port_et_stat st;
	u64 eth1_addr;
} xeth_platina_mk1 = {
	.st = {
		.max = 512,
	},
};

static int xeth_platina_mk1_init(struct platform_device *pd)
{
	static const char * const eth1_akas[] = {
		"eth1", "enp3s0f0", NULL
	};
	static const char * const eth2_akas[] = {
		"eth2", "enp3s0f1", NULL
	};
	static const char * const *link_akas[] = {
		eth1_akas,
		eth2_akas,
		NULL,
	};
	static const unsigned gpios[] = { 400, 384 };
	int err, link, aka, gpio;

	err = xeth_port_et_stat_init(&xeth_platina_mk1.st, &pd->dev);
	if (err)
		return err;

	if (!xeth_platina_mk1.qsfps) {
		xeth_platina_mk1.qsfps =
			devm_kcalloc(&pd->dev, xeth_platina_mk1_ports,
				     sizeof(struct i2c_client*), GFP_KERNEL);
		if (!xeth_platina_mk1.qsfps) {
			xeth_port_et_stat_uninit(&xeth_platina_mk1.st);
			return -ENOMEM;
		}
	}

	for (link = 0; link_akas[link]; link++) {
		if (xeth_platina_mk1.links[link])
			continue;
		for (aka = 0; !xeth_platina_mk1.links[link]; aka++) {
			if (!link_akas[link][aka])
				return -EPROBE_DEFER;
			xeth_platina_mk1.links[link] =
				dev_get_by_name(&init_net,
						link_akas[link][aka]);
		}
	}

	xeth_platina_mk1.eth1_addr =
		ether_addr_to_u64(xeth_platina_mk1.links[0]->dev_addr);

	/* toggle all PORT*_RST_L gpio pins to start optical qsfps */
	for (gpio = 0; gpio < ARRAY_SIZE(gpios); gpio++) {
		struct gpio_desc *gd =
			xeth_debug_ptr_err(gpio_to_desc(gpios[gpio]));
		if (!IS_ERR(gd)) {
			gpiod_set_value_cansleep(gd, 0xffff);
			gpiod_set_value_cansleep(gd, 0x0000);
			gpiod_set_value_cansleep(gd, 0xffff);
			gpiod_put(gd);
		}
	}

	return 0;
}

static void xeth_platina_mk1_uninit(void)
{
	int port;

	xeth_port_et_stat_uninit(&xeth_platina_mk1.st);

	if (!xeth_platina_mk1.qsfps)
		return;
	for (port = 0; port < xeth_platina_mk1_ports; port++)
		if (xeth_platina_mk1.qsfps[port])
			i2c_unregister_device(xeth_platina_mk1.qsfps[port]);
	return;
}

static void xeth_platina_mk1_ifname(char *ifname, int port, int subport)
{
	if (port < 0)
		strcpy(ifname, "platina-mk1");
	else if (subport < 0)
		scnprintf(ifname, IFNAMSIZ, "xeth%d", 1 + port);
	else
		scnprintf(ifname, IFNAMSIZ, "xeth%d-%d", 1 + port, 1 + subport);
}

static void xeth_platina_mk1alpha_ifname(char *ifname, int port, int subport)
{
	if (port < 0)
		strcpy(ifname, "platina-mk1");
	else if (subport < 0)
		scnprintf(ifname, IFNAMSIZ, "xeth%d", port);
	else
		scnprintf(ifname, IFNAMSIZ, "xeth%d-%d", port, subport);
}

/**
 * The first 4 platina-mk1 mac addresses are,
 *
 * * BMC,
 *  * management port (aka. eth0)
 *  * 1st mux link (aka. eth1) - stolen by mux
 *  * 2nd mux link (aka. eth2)
 *
 * Then,
 *
 *  * port0
 *  * ...
 *  * port31
 *  * port0.subport1
 *  * ...
 *  * port31.subport1
 *  * port0.subport2
 *  * ...
 *  * port31.subport2
 *  * port0.subport3
 *  * ...
 *  * port31.subport3
 */
static void xeth_platina_mk1_hw_addr(struct net_device *nd,
				     int port, int subport)
{
	u64 addr = xeth_platina_mk1.eth1_addr;
	if (port >= 0) {
		addr += 2 + port;
		if (subport > 0)
			addr += (subport * 4);
	}
	u64_to_ether_addr(addr, nd->dev_addr);
	nd->addr_assign_type = NET_ADDR_PERM;
}

static u32 xeth_platina_mk1_xid(int port, int subport)
{
	u32 xid;

	if (port < 0)
		xid = 3000;
	else if (subport < 0)
		xid = 3999 - port;
	else
		xid = 3999 - port - (xeth_platina_mk1_ports * subport);
	return xid;
}

static void xeth_platina_mk1_port_ksettings(struct ethtool_link_ksettings *ks)
{
	ks->base.speed = 0;
	ks->base.duplex = DUPLEX_FULL;
	ks->base.autoneg = AUTONEG_ENABLE;
	ks->base.port = PORT_OTHER;
	ethtool_link_ksettings_zero_link_mode(ks, supported);
	ethtool_link_ksettings_add_link_mode(ks, supported, Autoneg);
	ethtool_link_ksettings_add_link_mode(ks, supported, 10000baseKX4_Full);
	ethtool_link_ksettings_add_link_mode(ks, supported, 10000baseKR_Full);
	ethtool_link_ksettings_add_link_mode(ks, supported, 10000baseR_FEC);
	ethtool_link_ksettings_add_link_mode(ks, supported, 20000baseMLD2_Full);
	ethtool_link_ksettings_add_link_mode(ks, supported, 20000baseKR2_Full);
	ethtool_link_ksettings_add_link_mode(ks, supported, 25000baseCR_Full);
	ethtool_link_ksettings_add_link_mode(ks, supported, 25000baseKR_Full);
	ethtool_link_ksettings_add_link_mode(ks, supported, 25000baseSR_Full);
	ethtool_link_ksettings_add_link_mode(ks, supported, 40000baseKR4_Full);
	ethtool_link_ksettings_add_link_mode(ks, supported, 40000baseCR4_Full);
	ethtool_link_ksettings_add_link_mode(ks, supported, 40000baseSR4_Full);
	ethtool_link_ksettings_add_link_mode(ks, supported, 40000baseLR4_Full);
	ethtool_link_ksettings_add_link_mode(ks, supported, 50000baseCR2_Full);
	ethtool_link_ksettings_add_link_mode(ks, supported, 50000baseKR2_Full);
	ethtool_link_ksettings_add_link_mode(ks, supported, 50000baseSR2_Full);
	ethtool_link_ksettings_add_link_mode(ks, supported, 100000baseKR4_Full);
	ethtool_link_ksettings_add_link_mode(ks, supported, 100000baseSR4_Full);
	ethtool_link_ksettings_add_link_mode(ks, supported, 100000baseCR4_Full);
	ethtool_link_ksettings_add_link_mode(ks, supported,
					     100000baseLR4_ER4_Full);
	ethtool_link_ksettings_add_link_mode(ks, supported, TP);
	ethtool_link_ksettings_add_link_mode(ks, supported, FIBRE);
	ethtool_link_ksettings_add_link_mode(ks, supported, FEC_NONE);
	ethtool_link_ksettings_add_link_mode(ks, supported, FEC_RS);
	ethtool_link_ksettings_add_link_mode(ks, supported, FEC_BASER);
	bitmap_copy(ks->link_modes.advertising, ks->link_modes.supported,
		    __ETHTOOL_LINK_MODE_MASK_NBITS);
	/* disable FEC_NONE so that the vner-platina-mk1 interprets
	 * FEC_RS|FEC_BASER as FEC_AUTO */
	ethtool_link_ksettings_del_link_mode(ks, advertising, FEC_NONE);
}

static void
xeth_platina_mk1_subport_ksettings(struct ethtool_link_ksettings *ks)
{
	ks->base.speed = 0;
	ks->base.duplex = DUPLEX_FULL;
	ks->base.autoneg = AUTONEG_ENABLE;
	ks->base.port = PORT_OTHER;
	ethtool_link_ksettings_zero_link_mode(ks, supported);
	ethtool_link_ksettings_add_link_mode(ks, supported, Autoneg);
	ethtool_link_ksettings_add_link_mode(ks, supported, 10000baseKX4_Full);
	ethtool_link_ksettings_add_link_mode(ks, supported, 10000baseKR_Full);
	ethtool_link_ksettings_add_link_mode(ks, supported, 10000baseR_FEC);
	ethtool_link_ksettings_add_link_mode(ks, supported, 20000baseMLD2_Full);
	ethtool_link_ksettings_add_link_mode(ks, supported, 20000baseKR2_Full);
	ethtool_link_ksettings_add_link_mode(ks, supported, 25000baseCR_Full);
	ethtool_link_ksettings_add_link_mode(ks, supported, 25000baseKR_Full);
	ethtool_link_ksettings_add_link_mode(ks, supported, 25000baseSR_Full);
	ethtool_link_ksettings_add_link_mode(ks, supported, 40000baseKR4_Full);
	ethtool_link_ksettings_add_link_mode(ks, supported, 40000baseCR4_Full);
	ethtool_link_ksettings_add_link_mode(ks, supported, 40000baseSR4_Full);
	ethtool_link_ksettings_add_link_mode(ks, supported, 40000baseLR4_Full);
	ethtool_link_ksettings_add_link_mode(ks, supported, 50000baseCR2_Full);
	ethtool_link_ksettings_add_link_mode(ks, supported, 50000baseKR2_Full);
	ethtool_link_ksettings_add_link_mode(ks, supported, 50000baseSR2_Full);
	ethtool_link_ksettings_add_link_mode(ks, supported, TP);
	ethtool_link_ksettings_add_link_mode(ks, supported, FIBRE);
	ethtool_link_ksettings_add_link_mode(ks, supported, FEC_NONE);
	ethtool_link_ksettings_add_link_mode(ks, supported, FEC_RS);
	ethtool_link_ksettings_add_link_mode(ks, supported, FEC_BASER);
	bitmap_copy(ks->link_modes.advertising, ks->link_modes.supported,
		    __ETHTOOL_LINK_MODE_MASK_NBITS);
}

static size_t xeth_platina_mk1_port_et_stats(void)
{
	return xeth_platina_mk1.st.max;
}

static size_t xeth_platina_mk1_port_et_stat_named(void)
{
	return xeth_port_et_stat_named(&xeth_platina_mk1.st);
}

static void xeth_platina_mk1_port_et_stat_names(char *buf)
{
	xeth_port_et_stat_names(&xeth_platina_mk1.st, buf);
}

static int xeth_platina_qsfp_peek(struct i2c_adapter *adapter,
				  unsigned short addr)
{
	int err;
	union i2c_smbus_data data;

	if (!i2c_check_functionality(adapter, I2C_FUNC_SMBUS_READ_BYTE_DATA))
		return -ENXIO;
	err = i2c_smbus_xfer(adapter, addr, 0, I2C_SMBUS_READ, 0,
			     I2C_SMBUS_BYTE_DATA, &data);
	if (err < 0)
		return -ENXIO;
	switch (data.byte) {
	case 0x03:	/* SFP    */
	case 0x0C:	/* QSFP   */
	case 0x0D:	/* QSFP+  */
	case 0x11:	/* QSFP28 */
		break;
	default:
		return -ENXIO;
	}
	return data.byte;
}

static struct i2c_client *xeth_platina_mk1_qsfp(int port)
{
	static const int const nrs[] = {
		 3,  2,  5,  4,  7,  6,  9,  8,
		12, 11, 14, 13, 16, 15, 18, 17,
		21, 20, 23, 22, 25, 24, 27, 26,
		30, 29, 32, 31, 34, 33, 36, 35,
		-1,
	};
	static const unsigned short const addrs[] = I2C_ADDRS(0x50, 0x51);
	struct i2c_adapter *adapter;
	struct i2c_board_info info;
	int id, i;

	if (!xeth_platina_mk1.qsfps || port >= xeth_platina_mk1_ports)
		return NULL;
	if (xeth_platina_mk1.qsfps[port])
		return xeth_platina_mk1.qsfps[port];

	memset(&info, 0, sizeof(info));
	strscpy(info.type, "qsfp", sizeof(info.type));
	adapter = i2c_get_adapter(nrs[port]);
	for (i = 0; addrs[i] != I2C_CLIENT_END; i++) {
		id = xeth_platina_qsfp_peek(adapter, addrs[i]);
		if (id > 0) {
			info.addr = addrs[i];
			xeth_platina_mk1.qsfps[port] =
				i2c_new_client_device(adapter, &info);
			if (IS_ERR(xeth_platina_mk1.qsfps[port]))
				xeth_platina_mk1.qsfps[port] = NULL;
			else
				no_xeth_debug("qsfp%d @ %d, 0x%02x\n",
					      port, nrs[port], addrs[i]);
			break;
		}
	}
	i2c_put_adapter(adapter);
	return xeth_platina_mk1.qsfps[port];
}

const struct xeth_platform xeth_platina_mk1_platform = {
	.links = xeth_platina_mk1.links,
	.port_et_priv_flag_names = xeth_platina_mk1_port_et_priv_flag_names,
	.init = xeth_platina_mk1_init,
	.uninit = xeth_platina_mk1_uninit,
	.ifname = xeth_platina_mk1_ifname,
	.hw_addr = xeth_platina_mk1_hw_addr,
	.xid = xeth_platina_mk1_xid,
	.qsfp = xeth_platina_mk1_qsfp,
	.port_ksettings = xeth_platina_mk1_port_ksettings,
	.subport_ksettings = xeth_platina_mk1_subport_ksettings,
	.port_et_stats = xeth_platina_mk1_port_et_stats,
	.port_et_stat_named = xeth_platina_mk1_port_et_stat_named,
	.port_et_stat_names = xeth_platina_mk1_port_et_stat_names,
	.encap = XETH_ENCAP_VLAN,
	.ports = xeth_platina_mk1_ports,
};

const struct xeth_platform xeth_platina_mk1alpha_platform = {
	.links = xeth_platina_mk1.links,
	.port_et_priv_flag_names = xeth_platina_mk1_port_et_priv_flag_names,
	.init = xeth_platina_mk1_init,
	.uninit = xeth_platina_mk1_uninit,
	.ifname = xeth_platina_mk1alpha_ifname,
	.hw_addr = xeth_platina_mk1_hw_addr,
	.xid = xeth_platina_mk1_xid,
	.qsfp =xeth_platina_mk1_qsfp,
	.port_ksettings = xeth_platina_mk1_port_ksettings,
	.subport_ksettings = xeth_platina_mk1_subport_ksettings,
	.encap = XETH_ENCAP_VLAN,
	.ports = xeth_platina_mk1_ports,
};
