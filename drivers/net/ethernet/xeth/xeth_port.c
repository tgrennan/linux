/**
 * SPDX-License-Identifier: GPL-2.0
 * Copyright(c) 2018-2020 Platina Systems, Inc.
 *
 * Contact Information:
 * sw@platina.com
 * Platina Systems, 3180 Del La Cruz Blvd, Santa Clara, CA 95054
 */

#include "xeth_mux.h"
#include "xeth_platform.h"
#include "xeth_proxy.h"
#include "xeth_port.h"
#include "xeth_qsfp.h"
#include "xeth_sbtx.h"
#include "xeth_version.h"
#include "xeth_debug.h"
#include <linux/xeth.h>
#include <linux/netdevice.h>

static const char xeth_port_drvname[] = "xeth-port";

struct xeth_port_ext {
	struct i2c_client *qsfp;
	u8 n_priv_flags;
	u32 priv_flags;
	atomic64_t stats[];
};

struct xeth_port_priv {
	struct xeth_proxy proxy;
	int port, subport;
	struct ethtool_link_ksettings ksettings;
	/* @ext: only included w/ subport[0] */
	struct xeth_port_ext ext[];
};

int xeth_port_of(struct net_device *nd)
{
	struct xeth_port_priv *priv = netdev_priv(nd);
	return priv->port;
}

int xeth_port_subport(struct net_device *nd)
{
	struct xeth_port_priv *priv = netdev_priv(nd);
	return priv->subport;
}

static int xeth_port_open(struct net_device *nd)
{
	struct xeth_port_priv *priv = netdev_priv(nd);
	if (!(priv->proxy.mux->flags & IFF_UP))
		dev_open(priv->proxy.mux, NULL);
	return xeth_proxy_open(nd);
}

u32 xeth_port_ethtool_priv_flags(struct net_device *nd)
{
	struct xeth_port_priv *priv = netdev_priv(nd);
	return priv->subport <= 0 ? priv->ext[0].priv_flags : 0;
}

const struct ethtool_link_ksettings *
xeth_port_ethtool_ksettings(struct net_device *nd)
{
	struct xeth_port_priv *priv = netdev_priv(nd);
	return &priv->ksettings;
}

void xeth_port_reset_ethtool_stats(struct net_device *nd)
{
	struct xeth_port_priv *priv = netdev_priv(nd);
	const struct xeth_platform *platform =
		xeth_mux_platform(priv->proxy.mux);
	size_t n = xeth_platform_port_et_stats(platform);
	int i;

	if (priv->subport <= 0)
		for (i = 0; i < n; i++)
			atomic64_set(&priv->ext[0].stats[i], 0LL);
}

void xeth_port_ethtool_stat(struct net_device *nd, u32 index, u64 count)
{
	struct xeth_port_priv *priv = netdev_priv(nd);
	const struct xeth_platform *platform =
		xeth_mux_platform(priv->proxy.mux);
	size_t n = xeth_platform_port_et_stats(platform);

	if (index < n)
		atomic64_set(&priv->ext[0].stats[index], count);
	else
		xeth_mux_inc_sbrx_invalid(priv->proxy.mux);
}

void xeth_port_speed(struct net_device *nd, u32 mbps)
{
	struct xeth_port_priv *priv = netdev_priv(nd);
	priv->ksettings.base.speed = mbps;
}

const struct net_device_ops xeth_port_ndo = {
	.ndo_init = xeth_proxy_init,
	.ndo_uninit = xeth_proxy_uninit,
	.ndo_open = xeth_port_open,
	.ndo_stop = xeth_proxy_stop,
	.ndo_start_xmit = xeth_proxy_start_xmit,
	.ndo_get_iflink = xeth_proxy_get_iflink,
	.ndo_get_stats64 = xeth_proxy_get_stats64,
	.ndo_change_mtu = xeth_proxy_change_mtu,
	.ndo_fix_features = xeth_proxy_fix_features,
	.ndo_set_features = xeth_proxy_set_features,
};

static void xeth_port_get_drvinfo(struct net_device *nd,
				  struct ethtool_drvinfo *drvinfo)
{
	struct xeth_port_priv *priv = netdev_priv(nd);
	const struct xeth_platform *platform =
		xeth_mux_platform(priv->proxy.mux);

	strlcpy(drvinfo->driver, xeth_port_drvname, sizeof(drvinfo->driver));
	strlcpy(drvinfo->version, XETH_VERSION, sizeof(drvinfo->version));
	strlcpy(drvinfo->fw_version, "n/a", ETHTOOL_FWVERS_LEN);
	strlcpy(drvinfo->erom_version, "n/a", ETHTOOL_EROMVERS_LEN);
	drvinfo->n_priv_flags = priv->ext[0].n_priv_flags;
	drvinfo->n_stats = xeth_platform_port_et_stat_named(platform);
	scnprintf(drvinfo->bus_info, ETHTOOL_BUSINFO_LEN, "%u:%u",
		  priv->port, priv->proxy.xid);
}

static void xeth_subport_get_drvinfo(struct net_device *nd,
				     struct ethtool_drvinfo *drvinfo)
{
	struct xeth_port_priv *priv = netdev_priv(nd);

	strlcpy(drvinfo->driver, xeth_port_drvname, sizeof(drvinfo->driver));
	strlcpy(drvinfo->version, XETH_VERSION, sizeof(drvinfo->version));
	strlcpy(drvinfo->fw_version, "n/a", ETHTOOL_FWVERS_LEN);
	strlcpy(drvinfo->erom_version, "n/a", ETHTOOL_EROMVERS_LEN);
	drvinfo->n_priv_flags = 0;
	drvinfo->n_stats = 0;
	scnprintf(drvinfo->bus_info, ETHTOOL_BUSINFO_LEN, "%u-%u:%u",
		  priv->port, priv->subport, priv->proxy.xid);
}

static int xeth_port_get_sset_count(struct net_device *nd, int sset)
{
	struct xeth_port_priv *priv = netdev_priv(nd);
	const struct xeth_platform *platform =
		xeth_mux_platform(priv->proxy.mux);
	int n;

	switch (sset) {
	case ETH_SS_TEST:
		n = 0;
		break;
	case ETH_SS_STATS:
		n = xeth_platform_port_et_stat_named(platform);
		break;
	case ETH_SS_PRIV_FLAGS:
		n = priv->ext[0].n_priv_flags;
		break;
	default:
		n = -EOPNOTSUPP;
		break;
	}
	return n;
}

static void xeth_port_get_strings(struct net_device *nd, u32 sset, u8 *data)
{
	struct xeth_port_priv *priv = netdev_priv(nd);
	const struct xeth_platform *platform =
		xeth_mux_platform(priv->proxy.mux);
	char *p = (char *)data;
	int i;

	switch (sset) {
	case ETH_SS_TEST:
		break;
	case ETH_SS_STATS:
		xeth_platform_port_et_stat_names(platform, data);
		break;
	case ETH_SS_PRIV_FLAGS:
		for (i = 0; i < priv->ext[0].n_priv_flags;
		     i++, p += ETH_GSTRING_LEN)
			strlcpy(p, platform->port_et_priv_flag_names[i],
				ETH_GSTRING_LEN);
			
		break;
	default:
		break;
	}
}

static void xeth_port_get_stats(struct net_device *nd,
				struct ethtool_stats *stats, u64 *data)
{
	struct xeth_port_priv *priv = netdev_priv(nd);
	const struct xeth_platform *platform =
		xeth_mux_platform(priv->proxy.mux);
	int i, n = xeth_platform_port_et_stat_named(platform);

	for (i = 0; i < n; i++)
		data[i] = atomic64_read(&priv->ext[0].stats[i]);
}

static u32 xeth_port_get_priv_flags(struct net_device *nd)
{
	struct xeth_port_priv *priv = netdev_priv(nd);
	return priv->subport <= 0 ? priv->ext[0].priv_flags : 0;
}

static int xeth_port_set_priv_flags(struct net_device *nd, u32 flags)
{
	struct xeth_port_priv *priv = netdev_priv(nd);
	if (priv->subport > 0)
		return -EINVAL;
	if (flags >= (1 << priv->ext[0].n_priv_flags))
		return -EINVAL;
	priv->ext[0].priv_flags = flags;
	xeth_sbtx_et_flags(priv->proxy.mux, priv->proxy.xid, flags);
	return 0;
}

static int xeth_port_get_link_ksettings(struct net_device *nd,
					struct ethtool_link_ksettings *p)
{
	struct xeth_port_priv *priv = netdev_priv(nd);
	memcpy(p, &priv->ksettings, sizeof(*p));
	return 0;
}

static int xeth_port_validate_port(struct net_device *nd, u8 port)
{
	struct xeth_port_priv *priv = netdev_priv(nd);
	struct ethtool_link_ksettings *ks = &priv->ksettings;
	bool t = false;

	switch (port) {
	case PORT_TP:
		t = ethtool_link_ksettings_test_link_mode(ks, supported, TP);
		break;
	case PORT_AUI:
		t = ethtool_link_ksettings_test_link_mode(ks, supported, AUI);
		break;
	case PORT_MII:
		t = ethtool_link_ksettings_test_link_mode(ks, supported, MII);
		break;
	case PORT_FIBRE:
		t = ethtool_link_ksettings_test_link_mode(ks, supported, FIBRE);
		break;
	case PORT_BNC:
		t = ethtool_link_ksettings_test_link_mode(ks, supported, BNC);
		break;
	case PORT_DA:
	case PORT_NONE:
	case PORT_OTHER:
		t = true;
		break;
	}
	return t ? 0 : -EINVAL;
}

static int xeth_port_validate_duplex(struct net_device *nd, u8 duplex)
{
	return xeth_debug_nd_err(nd,
				 duplex != DUPLEX_HALF &&
				 duplex != DUPLEX_FULL &&
				 duplex != DUPLEX_UNKNOWN) ?  -EINVAL : 0;
}

static int xeth_port_validate_speed(struct net_device *nd, u32 speed)
{
        return xeth_debug_nd_err(nd,
				 speed != 100000 &&
				 speed != 50000 &&
				 speed != 40000 &&
				 speed != 25000 &&
				 speed != 20000 &&
				 speed != 10000 &&
				 speed != 1000) ?  -EINVAL : 0;
}

static int
xeth_port_set_link_ksettings(struct net_device *nd,
			     const struct ethtool_link_ksettings *req)
{
	struct xeth_port_priv *priv = netdev_priv(nd);
	struct ethtool_link_ksettings *ks = &priv->ksettings;
	int err;

	if (req->base.port != ks->base.port) {
		err = xeth_port_validate_port(nd, req->base.port);
		if (err)
			return err;
		ks->base.port = req->base.port;
	}
	if (req->base.autoneg == AUTONEG_DISABLE) {
		err = xeth_port_validate_speed(nd, req->base.speed);
		if (err)
			return err;
		err = xeth_port_validate_duplex(nd, req->base.duplex);
		if (err)
			return err;
		ks->base.autoneg = req->base.autoneg;
		ks->base.speed = req->base.speed;
		ks->base.duplex = req->base.duplex;
	} else {
		__ETHTOOL_DECLARE_LINK_MODE_MASK(res);
		if (bitmap_andnot(res, req->link_modes.advertising,
				  ks->link_modes.supported,
				  __ETHTOOL_LINK_MODE_MASK_NBITS)) {
			return -EINVAL;
		} else {
			err = xeth_port_validate_duplex(nd, req->base.duplex);
			if (err)
				return err;
			bitmap_copy(ks->link_modes.advertising,
				    req->link_modes.advertising,
				    __ETHTOOL_LINK_MODE_MASK_NBITS);
			ks->base.autoneg = AUTONEG_ENABLE;
			ks->base.speed = 0;
			ks->base.duplex = req->base.duplex;
		}
	}

	return xeth_sbtx_et_settings(priv->proxy.mux, priv->proxy.xid, ks);
}

#define xeth_port_ks_supports(ks, mk)					\
({									\
	(ethtool_link_ksettings_test_link_mode(ks, supported, mk));	\
})

#define xeth_port_ks_advertising(ks, mk)				\
({									\
	(ethtool_link_ksettings_test_link_mode(ks, advertising, mk));	\
})

static int xeth_port_get_fecparam(struct net_device *nd,
				  struct ethtool_fecparam *param)
{
	struct xeth_port_priv *priv = netdev_priv(nd);
	struct ethtool_link_ksettings *ks = &priv->ksettings;
	const u32 fec_both = ETHTOOL_FEC_RS | ETHTOOL_FEC_BASER;

	param->fec = 0;
	param->active_fec = 0;
	if (xeth_port_ks_supports(ks, FEC_NONE))
		param->fec |= ETHTOOL_FEC_OFF;
	if (xeth_port_ks_supports(ks, FEC_RS))
		param->fec |= ETHTOOL_FEC_RS;
	if (xeth_port_ks_supports(ks, FEC_BASER))
		param->fec |= ETHTOOL_FEC_BASER;
	if ((param->fec & fec_both) == fec_both)
		param->fec |= ETHTOOL_FEC_AUTO;
	if (!param->fec)
		param->fec = ETHTOOL_FEC_NONE;
	if (param->fec == ETHTOOL_FEC_NONE)
		param->active_fec = ETHTOOL_FEC_NONE;
	else if (xeth_port_ks_advertising(ks, FEC_NONE))
		param->active_fec = ETHTOOL_FEC_OFF;
	else if (xeth_port_ks_advertising(ks, FEC_RS))
		param->active_fec = xeth_port_ks_advertising(ks, FEC_BASER) ?
			ETHTOOL_FEC_AUTO : ETHTOOL_FEC_RS;
	else if (xeth_port_ks_advertising(ks, FEC_BASER))
		param->fec = ETHTOOL_FEC_BASER;
	return 0;
}

static int xeth_port_set_fecparam(struct net_device *nd,
				  struct ethtool_fecparam *param)
{
	struct xeth_port_priv *priv = netdev_priv(nd);
	struct ethtool_link_ksettings *ks = &priv->ksettings;
	switch (param->fec) {
	case ETHTOOL_FEC_AUTO:
		if (!ethtool_link_ksettings_test_link_mode(ks, supported,
							   FEC_RS) ||
		    !ethtool_link_ksettings_test_link_mode(ks, supported,
							   FEC_BASER)) {
			return -EOPNOTSUPP;
		}
		ethtool_link_ksettings_del_link_mode(ks, advertising,
						     FEC_NONE);
		ethtool_link_ksettings_add_link_mode(ks, advertising,
						     FEC_RS);
		ethtool_link_ksettings_add_link_mode(ks, advertising,
						     FEC_BASER);
		break;
	case ETHTOOL_FEC_OFF:
		if (!ethtool_link_ksettings_test_link_mode(ks, supported,
							   FEC_NONE)) {
			return -EOPNOTSUPP;
		}
		ethtool_link_ksettings_add_link_mode(ks, advertising,
						     FEC_NONE);
		ethtool_link_ksettings_del_link_mode(ks, advertising,
						     FEC_RS);
		ethtool_link_ksettings_del_link_mode(ks, advertising,
						     FEC_BASER);
		break;
	case ETHTOOL_FEC_RS:
		if (!ethtool_link_ksettings_test_link_mode(ks, supported,
							   FEC_RS)) {
			return -EOPNOTSUPP;
		}
		ethtool_link_ksettings_del_link_mode(ks, advertising,
						     FEC_NONE);
		ethtool_link_ksettings_add_link_mode(ks, advertising,
						     FEC_RS);
		ethtool_link_ksettings_del_link_mode(ks, advertising,
						     FEC_BASER);
		break;
	case ETHTOOL_FEC_BASER:
		if (!ethtool_link_ksettings_test_link_mode(ks, supported,
							   FEC_BASER)) {
			return -EOPNOTSUPP;
		}
		ethtool_link_ksettings_del_link_mode(ks, advertising,
						     FEC_NONE);
		ethtool_link_ksettings_del_link_mode(ks, advertising,
						     FEC_RS);
		ethtool_link_ksettings_add_link_mode(ks, advertising,
						     FEC_BASER);
		break;
	default:
		return -EINVAL;
	}
	return xeth_sbtx_et_settings(priv->proxy.mux, priv->proxy.xid, ks);
}

int xeth_port_get_module_info(struct net_device *nd,
			      struct ethtool_modinfo *emi)
{
	struct xeth_port_priv *priv = netdev_priv(nd);
	return priv->ext[0].qsfp ?
		xeth_qsfp_get_module_info(priv->ext[0].qsfp, emi) : -ENXIO;
}

int xeth_port_get_module_eeprom(struct net_device *nd,
				struct ethtool_eeprom *ee, u8 *data)
{
	struct xeth_port_priv *priv = netdev_priv(nd);
	return priv->ext[0].qsfp ?
		xeth_qsfp_get_module_eeprom(priv->ext[0].qsfp, ee, data) :
		-ENXIO;
}

static const struct ethtool_ops xeth_port_eto = {
	.get_drvinfo = xeth_port_get_drvinfo,
	.get_link = ethtool_op_get_link,
	.get_sset_count = xeth_port_get_sset_count,
	.get_strings = xeth_port_get_strings,
	.get_ethtool_stats = xeth_port_get_stats,
	.get_priv_flags = xeth_port_get_priv_flags,
	.set_priv_flags = xeth_port_set_priv_flags,
	.get_link_ksettings = xeth_port_get_link_ksettings,
	.set_link_ksettings = xeth_port_set_link_ksettings,
	.get_fecparam = xeth_port_get_fecparam,
	.set_fecparam = xeth_port_set_fecparam,
	.get_module_info = xeth_port_get_module_info,
	.get_module_eeprom = xeth_port_get_module_eeprom,
};

static const struct ethtool_ops xeth_subport_eto = {
	.get_drvinfo = xeth_subport_get_drvinfo,
	.get_link = ethtool_op_get_link,
	.get_link_ksettings = xeth_port_get_link_ksettings,
	.set_link_ksettings = xeth_port_set_link_ksettings,
};

static void xeth_port_setup(struct net_device *nd)
{
	struct xeth_port_priv *priv = netdev_priv(nd);

	xeth_proxy_setup(nd);
	netif_carrier_off(nd);
	priv->proxy.kind = XETH_DEV_KIND_PORT;
	nd->netdev_ops = &xeth_port_ndo;
	nd->needs_free_netdev = true;
	nd->priv_destructor = NULL;
	ether_setup(nd);
	nd->priv_flags &= ~IFF_TX_SKB_SHARING;
	nd->priv_flags |= IFF_LIVE_ADDR_CHANGE;
	nd->priv_flags |= IFF_NO_QUEUE;
	nd->priv_flags |= IFF_PHONY_HEADROOM;
	nd->priv_flags |= IFF_DONT_BRIDGE;
}

struct net_device *xeth_port_probe(struct net_device *mux,
				   int port, int subport)
{
	const struct xeth_platform *platform = xeth_mux_platform(mux);
	struct net_device *nd;
	struct xeth_port_priv *priv;
	char ifname[IFNAMSIZ];
	size_t sz;
	int err;

	xeth_platform_ifname(platform, ifname, port, subport);
	sz = sizeof(*priv);
	if (subport <= 0) {
		size_t stats = xeth_platform_port_et_stats(platform);
		sz += sizeof(struct xeth_port_ext);
		sz += sizeof(atomic64_t) * stats;
	}
	nd = alloc_netdev_mqs(sz, ifname, NET_NAME_ENUM,
			      xeth_port_setup, 1, 1);
	if (IS_ERR(nd))
		return nd;

	xeth_platform_hw_addr(platform, nd, port, subport);

	priv = netdev_priv(nd);
	priv->proxy.nd = nd;
	priv->proxy.mux = mux;
	priv->proxy.xid = xeth_platform_xid(platform, port, subport); 
	nd->min_mtu = priv->proxy.mux->min_mtu;
	nd->max_mtu = priv->proxy.mux->max_mtu;

	priv->port = port;
	priv->subport = subport;

	if (subport <= 0) {
		int i;
		size_t n = xeth_platform_port_et_stats(platform);
		for (i = 0; i < n; i++)
			atomic64_set(&priv->ext[0].stats[i], 0LL);
		nd->ethtool_ops = &xeth_port_eto;
		priv->ext[0].qsfp = xeth_platform_qsfp(platform, port);
		for (n = 0; platform->port_et_priv_flag_names[n]; n++);
		priv->ext[0].n_priv_flags = n;
	} else
		nd->ethtool_ops = &xeth_subport_eto;

	if (subport < 0)
		xeth_platform_port_ksettings(platform, &priv->ksettings);
	else
		xeth_platform_subport_ksettings(platform, &priv->ksettings);

	xeth_mux_add_proxy(&priv->proxy);

	rtnl_lock();
	err = xeth_debug_nd_err(nd, register_netdevice(nd));
	rtnl_unlock();

	if (err) {
		xeth_mux_del_proxy(&priv->proxy);
		free_netdev(nd);
		return ERR_PTR(err);
	}
	return nd;
}

static int xeth_port_validate(struct nlattr *tb[], struct nlattr *data[],
			      struct netlink_ext_ack *extack)
{
	if (!tb || !tb[IFLA_LINK]) {
		NL_SET_ERR_MSG(extack, "missing mux link");
		return -EINVAL;
	}
	if (data) {
		if (data[XETH_PORT_IFLA_XID]) {
			u16 xid = nla_get_u16(data[XETH_PORT_IFLA_XID]);
			if (xid == 0) {
				NL_SET_ERR_MSG(extack, "out-of-range XID");
				return -ERANGE;
			}
		}
	}
	return 0;
}

static int xeth_port_newlink(struct net *src_net, struct net_device *nd,
			     struct nlattr *tb[], struct nlattr *data[],
			     struct netlink_ext_ack *extack)
{
	struct xeth_port_priv *priv = netdev_priv(nd);
	u32 link = nla_get_u32(tb[IFLA_LINK]);
	int err;

	priv->proxy.nd = nd;
	priv->proxy.mux = dev_get_by_index_rcu(dev_net(nd), link);
	nd->min_mtu = priv->proxy.mux->min_mtu;
	nd->max_mtu = priv->proxy.mux->max_mtu;
	priv->port = -1;
	priv->subport = -1;

	if (data && data[XETH_PORT_IFLA_XID])
		priv->proxy.xid = nla_get_u16(data[XETH_PORT_IFLA_XID]);
	else
		for (priv->proxy.xid = 3999;
		     xeth_mux_proxy_of_xid(priv->proxy.mux, priv->proxy.xid);
		     priv->proxy.xid--);

	if (!tb || !tb[IFLA_IFNAME])
		scnprintf(nd->name, IFNAMSIZ, "%s%u",
			  xeth_port_drvname, priv->proxy.xid);

	xeth_mux_add_proxy(&priv->proxy);
	if (err = register_netdevice(nd), err) {
		xeth_mux_del_proxy(&priv->proxy);
		return err;
	}
	return xeth_sbtx_ifinfo(&priv->proxy, 0, XETH_IFINFO_REASON_NEW);
}

static void xeth_port_dellink(struct net_device *nd, struct list_head *unregq)
{
	struct xeth_port_priv *priv = netdev_priv(nd);
	xeth_mux_del_vlans(priv->proxy.mux, nd, unregq);
	unregister_netdevice_queue(nd, unregq);
}

static struct net *xeth_port_get_link_net(const struct net_device *nd)
{
	return dev_net(nd);
}

static const struct nla_policy xeth_port_nla_policy[] = {
	[XETH_PORT_IFLA_XID] = { .type = NLA_U16 },
};

struct rtnl_link_ops xeth_port_lnko = {
	.kind		= xeth_port_drvname,
	.priv_size	= sizeof(struct xeth_port_priv),
	.setup		= xeth_port_setup,
	.validate	= xeth_port_validate,
	.newlink	= xeth_port_newlink,
	.dellink	= xeth_port_dellink,
	.get_link_net	= xeth_port_get_link_net,
	.policy		= xeth_port_nla_policy,
	.maxtype	= XETH_PORT_N_IFLA - 1,
};
