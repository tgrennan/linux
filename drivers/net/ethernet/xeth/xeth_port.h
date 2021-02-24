/**
 * SPDX-License-Identifier: GPL-2.0
 * Copyright(c) 2018-2020 Platina Systems, Inc.
 *
 * Contact Information:
 * sw@platina.com
 * Platina Systems, 3180 Del La Cruz Blvd, Santa Clara, CA 95054
 */

#ifndef __NET_ETHERNET_XETH_PORT_H
#define __NET_ETHERNET_XETH_PORT_H

#include <linux/platform_device.h>
#include <linux/netdevice.h>
#include <net/rtnetlink.h>

struct xeth_port_et_stat {
	struct mutex mutex;
	struct device *dev;
	struct device_attribute attr;
	size_t max, named;
	char *names;	/* ETH_GSTRING_LEN sized strings */
};

extern struct rtnl_link_ops xeth_port_lnko;

extern const struct net_device_ops xeth_port_ndo;

static inline bool is_xeth_port(struct net_device *nd)
{
	return nd->netdev_ops == &xeth_port_ndo;
}

struct net_device *xeth_port_probe(struct net_device *mux,
				   int port, int subport);

int xeth_port_of(struct net_device *nd);
int xeth_port_subport(struct net_device *nd);

u32 xeth_port_ethtool_priv_flags(struct net_device *nd);
const struct ethtool_link_ksettings *
	xeth_port_ethtool_ksettings(struct net_device *nd);
void xeth_port_ethtool_stat(struct net_device *nd, u32 index, u64 count);
void xeth_port_link_stat(struct net_device *nd, u32 index, u64 count);
void xeth_port_speed(struct net_device *nd, u32 mbps);

void xeth_port_reset_ethtool_stats(struct net_device *);

static inline void xeth_port_et_stat_lock(struct xeth_port_et_stat *st)
{
	mutex_lock(&st->mutex);
}

static inline void xeth_port_et_stat_unlock(struct xeth_port_et_stat *st)
{
	mutex_unlock(&st->mutex);
}

static inline size_t xeth_port_et_stat_named(struct xeth_port_et_stat *st)
{
	size_t n;
	xeth_port_et_stat_lock(st);
	n = st->named;
	xeth_port_et_stat_unlock(st);
	return n;
}

static inline void xeth_port_et_stat_names(struct xeth_port_et_stat *st,
					   char *buf)
{
	xeth_port_et_stat_lock(st);
	memcpy(buf, st->names, st->named * ETH_GSTRING_LEN);
	xeth_port_et_stat_unlock(st);
}

static inline ssize_t
xeth_port_et_stat_show_name(struct device *dev, struct device_attribute *attr,
			    char *buf)
{
	struct xeth_port_et_stat *st =
		container_of(attr, struct xeth_port_et_stat, attr);
	ssize_t n = 0;

	xeth_port_et_stat_lock(st);
	if (st->named) {
		n = (st->named - 1) * ETH_GSTRING_LEN;
		n = strlcpy(buf, &st->names[n], ETH_GSTRING_LEN);
	}
	xeth_port_et_stat_unlock(st);
	return n;
}

static inline ssize_t
xeth_port_et_stat_store_name(struct device *dev, struct device_attribute *attr,
			     const char *buf, size_t sz)
{
	struct xeth_port_et_stat *st =
		container_of(attr, struct xeth_port_et_stat, attr);
	int i, begin, end;

	if (!sz || buf[0] == '\n') {
		xeth_port_et_stat_lock(st);
		st->named = 0;
		xeth_port_et_stat_unlock(st);
		return sz;
	}
	xeth_port_et_stat_lock(st);
	if (st->named >= st->max) {
		xeth_port_et_stat_unlock(st);
		return -EINVAL;
	}
	begin = st->named * ETH_GSTRING_LEN;
	end = begin + ETH_GSTRING_LEN;
	for (i = begin; i < end; i++)
		if (buf[i] == '\n' || i == sz) {
			st->names[i] = '\0';
			break;
		} else
			st->names[i] = buf[i];
	st->named++;
	xeth_port_et_stat_unlock(st);
	return sz;
}

static inline int xeth_port_et_stat_init(struct xeth_port_et_stat *st,
					 struct device *dev)
{
	int err;

	if (st->names || !st->max)
		return 0;
	mutex_init(&st->mutex);
	st->attr.attr.name = "stat_name";
	st->attr.attr.mode = VERIFY_OCTAL_PERMISSIONS(0644);
	st->attr.show = xeth_port_et_stat_show_name;
	st->attr.store = xeth_port_et_stat_store_name;
	st->names = devm_kzalloc(dev, ETH_GSTRING_LEN * st->max, GFP_KERNEL);
	if (!st->names)
		return -ENOMEM;
	err = device_create_file(dev, &st->attr);
	if (!err)
		st->dev = dev;
	return err;
}

static inline void xeth_port_et_stat_uninit(struct xeth_port_et_stat *st)
{
	if (st->dev) {
		device_remove_file(st->dev, &st->attr);
		st->dev = NULL;
	}
}

#endif /* __NET_ETHERNET_XETH_PORT_H */
