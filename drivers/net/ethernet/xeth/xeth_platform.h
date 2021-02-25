/**
 * SPDX-License-Identifier: GPL-2.0
 * Copyright(c) 2018-2020 Platina Systems, Inc.
 *
 * Contact Information:
 * sw@platina.com
 * Platina Systems, 3180 Del La Cruz Blvd, Santa Clara, CA 95054
 */

#ifndef __NET_ETHERNET_PLATFORM_H
#define __NET_ETHERNET_PLATFORM_H

#include <linux/platform_device.h>
#include <linux/netdevice.h>
#include <linux/ethtool.h>
#include <linux/etherdevice.h>
#include <linux/i2c.h>
#include <uapi/linux/xeth.h>

extern struct platform_driver xeth_platform_driver;

struct xeth_platform {
	/**
	 * @links:
	 *	A NULL terminated list of mux lower netdevs discovered through
	 *	the following @probe.
	 */
	struct net_device * const *links;
	/**
	 * @port_et_priv_flag_names:
	 *	A NULL terminated list of ethtool priv flag names
	 */
	const char * const *port_et_priv_flag_names;
	/**
	 * @init: called before registering the mux and its ports.
	 */
	int (*init)(struct platform_device *);
	/**
	 * @uninit: called before unregistering mux and all of its uppers
	 */
	void (*uninit)(void);
	/**
	 * @ifname:
	 *	Name of indexed (port, subport) or mux if port is -1.
	 */
	void (*ifname)(char *ifname, int port, int subport);
	/**
	 * @hw_addr:
	 *	Assign HW addr (aka MAC) of given netdev at (port, subport)
	 *	or mux if port is -1.
	 */
	void (*hw_addr)(struct net_device *nd, int port, int subport);
	/**
	 * @xid:
	 *	Return mux id of (port, subport) or alloc from free list if
	 *	port is -1.
	 */
	u32 (*xid)(int port, int subport);
	/**
	 * @qsfp: returns i2c_client of port's QSFP or NULL if none.
	 */
	struct i2c_client *(*qsfp)(int port);
	/**
	 * @port_ksettings:
	 *	Assigns port's @ethtool_link_ksettings
	 */
	void (*port_ksettings)(struct ethtool_link_ksettings *);
	/**
	 * @subport_ksettings:
	 *	Assigns subport's @ethtool_link_ksettings
	 */
	void (*subport_ksettings)(struct ethtool_link_ksettings *);
	size_t (*port_et_stats)(void);
	size_t (*port_et_stat_named)(void);
	void (*port_et_stat_names)(char*);
	/**
	 * @encap: vlan or vpls
	 */
	enum xeth_encap encap;
	/**
	 * @ports:
	 *	An xeth platform must have 1 or more ports
	 */
	u8 ports;
};

ssize_t xeth_platform_subports(size_t port);

const struct xeth_platform *xeth_mux_platform(struct net_device *mux);
struct net_device *xeth_mux(const struct xeth_platform *, struct device *);

static inline int xeth_platform_init(const struct xeth_platform *platform,
				     struct platform_device *pd)
{
	return (platform && platform->init) ?  platform->init(pd) : 32;
}

static inline void xeth_platform_uninit(const struct xeth_platform *platform)
{
	if (platform && platform->uninit)
		platform->uninit();
}

static inline int xeth_platform_ports(const struct xeth_platform *platform)
{
	return (platform && platform->ports) ?  platform->ports : 32;
}

static inline void xeth_platform_ifname(const struct xeth_platform *platform,
					char *ifname, int port, int subport)
{
	if (platform->ifname)
		platform->ifname(ifname, port, subport);
	else if (port < 0)
		strcpy(ifname, "xeth-mux");
	else if (subport < 0)
		scnprintf(ifname, IFNAMSIZ, "xeth%d", port);
	else
		scnprintf(ifname, IFNAMSIZ, "xeth%d.%d", port, subport);
}

static inline void xeth_platform_hw_addr(const struct xeth_platform *platform,
					 struct net_device *nd,
					 int port, int subport)
{
	if (platform->hw_addr)
		platform->hw_addr(nd, port, subport);
	else
		eth_hw_addr_random(nd);
}

static inline u32 xeth_platform_xid(const struct xeth_platform *platform,
				    int port, int subport)
{
	int ports = platform ? platform->ports : 32;
	if (platform->xid)
		return platform->xid(port, subport);
	else if (port < 0)
		return 3000;
	else if (subport < 0)
		return 3999 - port;
	else
		return 3999 - port - (ports * subport);
}

static inline struct i2c_client *
xeth_platform_qsfp(const struct xeth_platform *platform, int port)
{
	return (platform && platform->qsfp) ? platform->qsfp(port) : NULL;
}

static inline void
xeth_platform_port_ksettings(const struct xeth_platform *platform,
			     struct ethtool_link_ksettings *ks)
{
	if (platform->port_ksettings)
		platform->port_ksettings(ks);
}

static inline void
xeth_platform_subport_ksettings(const struct xeth_platform *platform,
				struct ethtool_link_ksettings *ks)
{
	if (platform->subport_ksettings)
		platform->subport_ksettings(ks);
}

static inline size_t
xeth_platform_port_et_stats(const struct xeth_platform *platform)
{
	return platform->port_et_stats ? platform->port_et_stats() : 0;
}

static inline size_t
xeth_platform_port_et_stat_named(const struct xeth_platform *platform)
{
	return platform->port_et_stat_named ?
		platform->port_et_stat_named() : 0;
}

static inline void
xeth_platform_port_et_stat_names(const struct xeth_platform *platform,
				 char *buf)
{
	if (platform->port_et_stat_names)
		platform->port_et_stat_names(buf);
}

#endif /* __NET_ETHERNET_PLATFORM_H */
