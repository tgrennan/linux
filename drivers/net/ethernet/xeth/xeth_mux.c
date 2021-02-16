/**
 * SPDX-License-Identifier: GPL-2.0
 * Copyright(c) 2018-2020 Platina Systems, Inc.
 *
 * Contact Information:
 * sw@platina.com
 * Platina Systems, 3180 Del La Cruz Blvd, Santa Clara, CA 95054
 */

#include "xeth_mux.h"
#include "xeth_link_stat.h"
#include "xeth_nb.h"
#include "xeth_proxy.h"
#include "xeth_sbrx.h"
#include "xeth_sbtx.h"
#include "xeth_port.h"
#include "xeth_bridge.h"
#include "xeth_vlan.h"
#include "xeth_debug.h"
#include "xeth_version.h"
#include <linux/xeth.h>
#include <linux/if_vlan.h>
#include <net/sock.h>
#include <linux/un.h>
#include <linux/i2c.h>

static const char xeth_mux_drvname[] = "xeth-mux";

enum {
	xeth_mux_proxy_hash_bits = 4,
	xeth_mux_proxy_hash_bkts = 1 << xeth_mux_proxy_hash_bits,
	xeth_mux_link_hash_bits = 4,
	xeth_mux_link_hash_bkts = 1 << xeth_mux_link_hash_bits,
};

struct xeth_mux_priv {
	struct net_device *nd;
	struct xeth_nb nb;
	struct task_struct *main;
	struct platform_device *xeth;
	struct net_device *link[xeth_mux_link_hash_bkts];
	struct {
		struct mutex mutex;
		struct xeth_proxy __rcu	*last;
		struct hlist_head __rcu	hls[xeth_mux_proxy_hash_bkts];
		struct list_head __rcu ports, vlans, bridges, lags, lbs;
	} proxy;
	atomic64_t counters[xeth_mux_n_counters];
	atomic64_t link_stats[XETH_N_LINK_STAT];
	volatile unsigned long flags;
	struct {
		spinlock_t mutex;
		struct socket *conn;
		struct list_head free, tx;
		void *rx;
	} sb;
	enum xeth_encap encap;
};

static void xeth_mux_priv_init(struct xeth_mux_priv *priv)
{
	int i;

	mutex_init(&priv->proxy.mutex);
	spin_lock_init(&priv->sb.mutex);

	for (i = 0; i < xeth_mux_proxy_hash_bkts; i++)
		INIT_HLIST_HEAD(&priv->proxy.hls[i]);
	INIT_LIST_HEAD_RCU(&priv->proxy.ports);
	INIT_LIST_HEAD_RCU(&priv->proxy.vlans);
	INIT_LIST_HEAD_RCU(&priv->proxy.bridges);
	INIT_LIST_HEAD_RCU(&priv->proxy.lags);
	INIT_LIST_HEAD_RCU(&priv->proxy.lbs);

	INIT_LIST_HEAD(&priv->sb.free);
	INIT_LIST_HEAD(&priv->sb.tx);
}

static void xeth_mux_lock_proxy(struct xeth_mux_priv *priv)
{
	mutex_lock(&priv->proxy.mutex);
}

static void xeth_mux_unlock_proxy(struct xeth_mux_priv *priv)
{
	mutex_unlock(&priv->proxy.mutex);
}

static void xeth_mux_lock_sb(struct xeth_mux_priv *priv)
{
	spin_lock(&priv->sb.mutex);
}

static void xeth_mux_unlock_sb(struct xeth_mux_priv *priv)
{
	spin_unlock(&priv->sb.mutex);
}

struct xeth_nb *xeth_mux_nb(struct net_device *mux)
{
	struct xeth_mux_priv *priv = netdev_priv(mux);
	return &priv->nb;
}

struct net_device *xeth_mux_of_nb(struct xeth_nb *nb)
{
	struct xeth_mux_priv *priv =
		xeth_debug_container_of(nb, struct xeth_mux_priv, nb);
	return priv->nd;
}

enum xeth_encap xeth_mux_encap(struct net_device *nd)
{
	struct xeth_mux_priv *priv = netdev_priv(nd);
	return priv->encap;
}

struct xeth_proxy *xeth_mux_priv_proxy(struct xeth_mux_priv *priv,
				       struct xeth_proxy *proxy)
{
	rcu_read_unlock();
	rcu_assign_pointer(priv->proxy.last, proxy);
	return proxy;
}

struct xeth_proxy *xeth_mux_proxy_of_xid(struct net_device *mux, u32 xid)
{
	struct xeth_mux_priv *priv = netdev_priv(mux);
	struct xeth_proxy *proxy;
	unsigned bkt;

	rcu_read_lock();
	proxy = rcu_dereference(priv->proxy.last);
	if (proxy && proxy->xid == xid)
		return xeth_mux_priv_proxy(priv, proxy);
	bkt = hash_min(xid, xeth_mux_proxy_hash_bits);
	hlist_for_each_entry_rcu(proxy, &priv->proxy.hls[bkt], node)
		if (proxy->xid == xid)
			return xeth_mux_priv_proxy(priv, proxy);
	return xeth_mux_priv_proxy(priv, NULL);
}

struct xeth_proxy *xeth_mux_proxy_of_nd(struct net_device *mux,
					struct net_device *nd)
{
	struct xeth_mux_priv *priv = netdev_priv(mux);
	struct xeth_proxy *proxy;
	unsigned bkt;

	rcu_read_lock();
	proxy = rcu_dereference(priv->proxy.last);
	if (proxy && proxy->nd == nd)
		return xeth_mux_priv_proxy(priv, proxy);
	for (bkt = 0; bkt < xeth_mux_proxy_hash_bkts; bkt++)
		hlist_for_each_entry_rcu(proxy, &priv->proxy.hls[bkt], node)
			if (proxy->nd == nd)
				return xeth_mux_priv_proxy(priv, proxy);
	return xeth_mux_priv_proxy(priv, NULL);
}

void xeth_mux_add_proxy(struct xeth_proxy *proxy)
{
	struct xeth_mux_priv *priv = netdev_priv(proxy->mux);
	unsigned bkt;
	
	bkt = hash_min(proxy->xid, xeth_mux_proxy_hash_bits);
	xeth_mux_lock_proxy(priv);
	hlist_add_head_rcu(&proxy->node, &priv->proxy.hls[bkt]);
	switch (proxy->kind) {
	case XETH_DEV_KIND_PORT:
		list_add_rcu(&proxy->kin, &priv->proxy.ports);
		break;
	case XETH_DEV_KIND_VLAN:
		list_add_rcu(&proxy->kin, &priv->proxy.vlans);
		break;
	case XETH_DEV_KIND_BRIDGE:
		list_add_rcu(&proxy->kin, &priv->proxy.bridges);
		break;
	case XETH_DEV_KIND_LAG:
		list_add_rcu(&proxy->kin, &priv->proxy.lags);
		break;
	case XETH_DEV_KIND_LB:
		list_add_rcu(&proxy->kin, &priv->proxy.lbs);
		break;
	case XETH_DEV_KIND_UNSPEC:
	default:
		pr_err("%s:%s: invalid kind: 0x%x", __func__,
		       netdev_name(proxy->nd), proxy->kind);
	}
	xeth_mux_unlock_proxy(priv);
}

void xeth_mux_del_proxy(struct xeth_proxy *proxy)
{
	struct xeth_mux_priv *priv = netdev_priv(proxy->mux);

	xeth_mux_lock_proxy(priv);
	rcu_assign_pointer(priv->proxy.last, NULL);
	hlist_del_rcu(&proxy->node);
	list_del(&proxy->kin);
	xeth_mux_unlock_proxy(priv);
	synchronize_rcu();
}

static void xeth_mux_reset_all_link_stats(struct net_device *mux)
{
	struct xeth_mux_priv *priv = netdev_priv(mux);
	struct xeth_proxy *proxy;

	xeth_link_stat_init(priv->link_stats);
	list_for_each_entry_rcu(proxy, &priv->proxy.ports, kin)
		xeth_proxy_reset_link_stats(proxy);
	list_for_each_entry_rcu(proxy, &priv->proxy.vlans, kin)
		xeth_proxy_reset_link_stats(proxy);
	list_for_each_entry_rcu(proxy, &priv->proxy.bridges, kin)
		xeth_proxy_reset_link_stats(proxy);
	list_for_each_entry_rcu(proxy, &priv->proxy.lags, kin)
		xeth_proxy_reset_link_stats(proxy);
	list_for_each_entry_rcu(proxy, &priv->proxy.lbs, kin)
		xeth_proxy_reset_link_stats(proxy);
}

void xeth_mux_change_carrier(struct net_device *mux, struct net_device *nd,
			     bool on)
{
	struct xeth_mux_priv *priv = netdev_priv(mux);
	void (*change_carrier)(struct net_device *dev) =
		on ? netif_carrier_on : netif_carrier_off;
	struct xeth_proxy *proxy;

	change_carrier(nd);
	list_for_each_entry_rcu(proxy, &priv->proxy.vlans, kin)
		if (xeth_vlan_has_link(proxy->nd, nd))
			change_carrier(proxy->nd);
}

void xeth_mux_check_lower_carrier(struct net_device *mux)
{
	struct net_device *lower;
	struct list_head *lowers;
	bool carrier = true;

	netdev_for_each_lower_dev(mux, lower, lowers)
		if (!netif_carrier_ok(lower))
			carrier = false;
	if (carrier) {
		if (!netif_carrier_ok(mux))
			netif_carrier_on(mux);
	} else if (netif_carrier_ok(mux))
		netif_carrier_off(mux);
}

void xeth_mux_del_vlans(struct net_device *mux, struct net_device *nd,
			struct list_head *unregq)
{
	struct xeth_mux_priv *priv = netdev_priv(mux);
	struct xeth_proxy *proxy;

	list_for_each_entry_rcu(proxy, &priv->proxy.vlans, kin)
		if (xeth_vlan_has_link(proxy->nd, nd))
			unregister_netdevice_queue(proxy->nd, unregq);
}

void xeth_mux_dump_all_ifinfo(struct net_device *mux)
{
	struct xeth_mux_priv *priv = netdev_priv(mux);
	struct xeth_proxy *proxy;

	list_for_each_entry_rcu(proxy, &priv->proxy.lbs, kin)
		xeth_proxy_dump_ifinfo(proxy);
	list_for_each_entry_rcu(proxy, &priv->proxy.ports, kin)
		xeth_proxy_dump_ifinfo(proxy);
	list_for_each_entry_rcu(proxy, &priv->proxy.lags, kin)
		xeth_proxy_dump_ifinfo(proxy);
	list_for_each_entry_rcu(proxy, &priv->proxy.vlans, kin)
		xeth_proxy_dump_ifinfo(proxy);
	list_for_each_entry_rcu(proxy, &priv->proxy.bridges, kin)
		xeth_proxy_dump_ifinfo(proxy);
}

static void xeth_mux_drop_all_port_carrier(struct net_device *mux)
{
	struct xeth_mux_priv *priv = netdev_priv(mux);
	struct xeth_proxy *proxy;

	list_for_each_entry_rcu(proxy, &priv->proxy.ports, kin)
		netif_carrier_off(proxy->nd);
}

static void xeth_mux_reset_all_port_ethtool_stats(struct net_device *mux)
{
	struct xeth_mux_priv *priv = netdev_priv(mux);
	struct xeth_proxy *proxy;

	list_for_each_entry_rcu(proxy, &priv->proxy.ports, kin)
		xeth_port_reset_ethtool_stats(proxy->nd);
}

atomic64_t *xeth_mux_counters(struct net_device *mux)
{
	struct xeth_mux_priv *priv = netdev_priv(mux);
	return priv->counters;
}

volatile unsigned long *xeth_mux_flags(struct net_device *mux)
{
	struct xeth_mux_priv *priv = netdev_priv(mux);
	return &priv->flags;
}

static const struct ethtool_ops xeth_mux_ethtool_ops;
static rx_handler_result_t xeth_mux_demux(struct sk_buff **pskb);
static void xeth_mux_demux_vlan(struct net_device *mux, struct sk_buff *skb);

static void xeth_mux_setup(struct net_device *mux)
{
	struct xeth_mux_priv *priv = netdev_priv(mux);

	xeth_mux_priv_init(priv);

	xeth_mux_counter_init(priv->counters);
	xeth_link_stat_init(priv->link_stats);

	mux->netdev_ops = &xeth_mux_ndo;
	mux->ethtool_ops = &xeth_mux_ethtool_ops;
	mux->needs_free_netdev = true;
	mux->priv_destructor = NULL;
	ether_setup(mux);
	mux->flags |= IFF_MASTER;
	mux->priv_flags |= IFF_DONT_BRIDGE;
	mux->priv_flags |= IFF_NO_QUEUE;
	mux->priv_flags &= ~IFF_TX_SKB_SHARING;
	mux->min_mtu = ETH_MIN_MTU;
	mux->max_mtu = ETH_MAX_MTU - VLAN_HLEN;
	mux->mtu = XETH_SIZEOF_JUMBO_FRAME - VLAN_HLEN;

	/* FIXME should we netif_keep_dst(nd) ? */
}

struct net_device *xeth_mux_probe(struct platform_device *xeth)
{
	struct net_device *mux;
	struct xeth_mux_priv *priv;
	char ifname[IFNAMSIZ];
	int err;

	xeth_vendor_ifname(xeth, ifname, -1, -1);
	mux = alloc_netdev_mqs(sizeof(*priv), ifname, NET_NAME_ENUM,
			       xeth_mux_setup, xeth_vendor_n_txqs(xeth),
			       xeth_vendor_n_rxqs(xeth));
	if (!mux || IS_ERR(mux))
		return mux;

	xeth_vendor_hw_addr(xeth, mux, -1, -1);

	priv = netdev_priv(mux);
	priv->nd = mux;
	priv->xeth = xeth;
	priv->encap = xeth_vendor_encap(xeth);

	rtnl_lock();
	err = register_netdevice(mux);
	rtnl_unlock();

	if (err) {
		free_netdev(mux);
		mux = ERR_PTR(err);
	}

	return mux;
}

static int xeth_mux_validate(struct nlattr *tb[], struct nlattr *data[],
			      struct netlink_ext_ack *extack)
{
	if (!tb || !tb[IFLA_LINK]) {
		NL_SET_ERR_MSG(extack, "missing link");
		return -EINVAL;
	}
	if (tb && tb[IFLA_ADDRESS]) {
		NL_SET_ERR_MSG(extack, "cannot set mac addr");
		return -EOPNOTSUPP;
	}
	return 0;
}

static int xeth_mux_service_sbrx(void *data)
{
	struct net_device *mux = data;
	struct xeth_mux_priv *priv = netdev_priv(mux);
	struct timeval tv = {
		.tv_sec = 0,
		.tv_usec = 10000,
	};
	int err;

	xeth_mux_set_sbrx_task(mux);
	err = kernel_setsockopt(priv->sb.conn, SOL_SOCKET, SO_RCVTIMEO_NEW,
				(char *)&tv, sizeof(tv));
	if (err)
		goto xeth_mux_service_sbrx_exit;

	allow_signal(SIGKILL);
	while (!err && !kthread_should_stop() && !signal_pending(current))
		err = xeth_sbrx(mux, priv->sb.conn, priv->sb.rx);
	xeth_nb_stop_netevent(mux);
	xeth_nb_stop_fib(mux);
	xeth_nb_stop_inetaddr(mux);
	xeth_nb_stop_netdevice(mux);
xeth_mux_service_sbrx_exit:
	xeth_mux_clear_sbrx_task(mux);
	return err;
}

static struct task_struct *xeth_mux_fork_sbrx(struct net_device *mux)
{
	struct task_struct *t;

	t = kthread_run(xeth_mux_service_sbrx, mux, "%s-rx", mux->name);
	return IS_ERR(t) ? NULL :  t;
}

struct xeth_sbtxb *xeth_mux_alloc_sbtxb(struct net_device *mux, size_t len)
{
	struct xeth_mux_priv *priv = netdev_priv(mux);
	struct xeth_sbtxb *sbtxb, *tmp;
	size_t sz;
	
	xeth_mux_lock_sb(priv);
	list_for_each_entry_safe(sbtxb, tmp, &priv->sb.free, list)
		if (sbtxb->sz >= len) {
			list_del(&sbtxb->list);
			xeth_mux_unlock_sb(priv);
			xeth_mux_dec_sbtx_free(mux);
			sbtxb->len = len;
			xeth_sbtxb_zero(sbtxb);
			return sbtxb;
		}
	xeth_mux_unlock_sb(priv);
	sz = ALIGN(xeth_sbtxb_size + len, 1024);
	sbtxb = devm_kzalloc(&mux->dev, sz, GFP_KERNEL);
	sbtxb->len = len;
	sbtxb->sz = sz - xeth_sbtxb_size;
	return sbtxb;
}

static void xeth_mux_append_sbtxb(struct net_device *mux,
				  struct xeth_sbtxb *sbtxb)
{
	struct xeth_mux_priv *priv = netdev_priv(mux);

	xeth_mux_lock_sb(priv);
	list_add_tail(&sbtxb->list, &priv->sb.tx);
	xeth_mux_unlock_sb(priv);
	xeth_mux_inc_sbtx_queued(mux);
}

static void xeth_mux_prepend_sbtxb(struct net_device *mux,
				   struct xeth_sbtxb *sbtxb)
{
	struct xeth_mux_priv *priv = netdev_priv(mux);

	xeth_mux_lock_sb(priv);
	list_add(&sbtxb->list, &priv->sb.tx);
	xeth_mux_unlock_sb(priv);
	xeth_mux_inc_sbtx_queued(mux);
}

static struct xeth_sbtxb *xeth_mux_pop_sbtxb(struct net_device *mux)
{
	struct xeth_mux_priv *priv = netdev_priv(mux);
	struct xeth_sbtxb *sbtxb;

	xeth_mux_lock_sb(priv);
	sbtxb = list_first_entry_or_null(&priv->sb.tx,
					 struct xeth_sbtxb, list);
	if (sbtxb) {
		list_del(&sbtxb->list);
		xeth_mux_dec_sbtx_queued(mux);
	}
	xeth_mux_unlock_sb(priv);
	return sbtxb;
}

static void xeth_mux_free_sbtxb(struct net_device *mux,
				struct xeth_sbtxb *sbtxb)
{
	struct xeth_mux_priv *priv = netdev_priv(mux);

	xeth_mux_lock_sb(priv);
	list_add_tail(&sbtxb->list, &priv->sb.free);
	xeth_mux_unlock_sb(priv);
	xeth_mux_inc_sbtx_free(mux);
}

void xeth_mux_queue_sbtx(struct net_device *mux, struct xeth_sbtxb *sbtxb)
{
	if (xeth_mux_has_sb_connection(mux))
		xeth_mux_append_sbtxb(mux, sbtxb);
	else
		xeth_mux_free_sbtxb(mux, sbtxb);
}

static int xeth_mux_sbtx(struct net_device *mux, struct xeth_sbtxb *sbtxb)
{
	struct xeth_mux_priv *priv = netdev_priv(mux);
	struct kvec iov = {
		.iov_base = xeth_sbtxb_data(sbtxb),
		.iov_len  = sbtxb->len,
	};
	struct msghdr msg = {
		.msg_flags = MSG_DONTWAIT,
	};
	int n;

	n = kernel_sendmsg(priv->sb.conn, &msg, &iov, 1, iov.iov_len);
	if (n == -EAGAIN) {
		xeth_mux_prepend_sbtxb(mux, sbtxb);
		xeth_mux_inc_sbtx_retries(mux);
		return n;
	}
	xeth_mux_free_sbtxb(mux, sbtxb);
	if (n > 0) {
		xeth_mux_inc_sbtx_msgs(mux);
		return 0;
	}
	return n < 0 ? n : 1; /* 1 indicates EOF */
}

static int xeth_mux_service_sbtx(struct net_device *mux)
{
	struct xeth_mux_priv *priv = netdev_priv(mux);
	const unsigned int maxms = 320;
	const unsigned int minms = 10;
	unsigned int ms = minms;
	int err = 0;
	struct xeth_sbtxb *sbtxb, *tmp;

	while (!err && xeth_mux_has_sbrx_task(mux) &&
	       !kthread_should_stop() && !signal_pending(current)) {
		xeth_mux_inc_sbtx_ticks(mux);
		sbtxb = xeth_mux_pop_sbtxb(mux);
		if (sbtxb) {
			ms = minms;
			err = xeth_mux_sbtx(mux, sbtxb);
			if (err == -EAGAIN) {
				err = 0;
				msleep(ms);
			}
		} else {
			msleep(ms);
			if (ms < maxms)
				ms *= 2;
		}
	}

	xeth_mux_lock_sb(priv);
	list_for_each_entry_safe(sbtxb, tmp, &priv->sb.tx, list) {
		list_del(&sbtxb->list);
		xeth_mux_dec_sbtx_queued(mux);
		list_add_tail(&sbtxb->list, &priv->sb.free);
		xeth_mux_inc_sbtx_free(mux);
	}
	xeth_mux_unlock_sb(priv);
	xeth_debug_err(xeth_mux_get_sbtx_queued(mux) > 0);

	return err;
}

static int xeth_mux_main(void *data)
{
	struct net_device *mux = data;
	struct xeth_mux_priv *priv = netdev_priv(mux);
	const int backlog = 3;
	struct socket *ln = NULL;
	struct sockaddr_un addr;
	char name[TASK_COMM_LEN];
	int n, err;

	if (!priv->sb.rx) {
		priv->sb.rx = devm_kzalloc(&mux->dev, XETH_SIZEOF_JUMBO_FRAME,
					   GFP_KERNEL);
		if (!priv->sb.rx)
			return -ENOMEM;
	}

	xeth_mux_set_main_task(mux);
	get_task_comm(name, current);
	allow_signal(SIGKILL);

	priv->sb.conn = NULL;
	memset(&addr, 0, sizeof(struct sockaddr_un));
	addr.sun_family = AF_UNIX;
	/* Note: This is an abstract namespace w/ addr.sun_path[0] == 0 */
	n = sizeof(sa_family_t) + 1 +
		scnprintf(addr.sun_path+1, UNIX_PATH_MAX-1, "%s", mux->name);

	err = sock_create_kern(current->nsproxy->net_ns,
			       AF_UNIX, SOCK_SEQPACKET, 0, &ln);
	if (err)
		goto xeth_mux_main_exit;
	SOCK_INODE(ln)->i_mode &= ~(S_IRWXG | S_IRWXO);
	err = kernel_bind(ln, (struct sockaddr *)&addr, n);
	if (err)
		goto xeth_mux_main_exit;
	err = kernel_listen(ln, backlog);
	if (err)
		goto xeth_mux_main_exit;
	xeth_mux_clear_sb_connection(mux);
	xeth_mux_set_sb_listen(mux);
	while(!err && !kthread_should_stop() && !signal_pending(current)) {
		err = kernel_accept(ln, &priv->sb.conn, O_NONBLOCK);
		if (err == -EAGAIN) {
			err = 0;
			msleep_interruptible(100);
			schedule();
			continue;
		}
		if (!err) {
			struct task_struct *sbrx;
			xeth_mux_set_sb_connection(mux);
			sbrx = xeth_mux_fork_sbrx(mux);
			if (sbrx) {
				xeth_mux_drop_all_port_carrier(mux);
				xeth_mux_reset_all_link_stats(mux);
				xeth_mux_reset_all_port_ethtool_stats(mux);
				xeth_debug_nd_err(mux,
						  xeth_mux_service_sbtx(mux));
				if (xeth_mux_has_sbrx_task(mux)) {
					kthread_stop(sbrx);
					while (xeth_mux_has_sbrx_task(mux)) {
						msleep_interruptible(100);
						schedule();
					}
				}
				xeth_mux_drop_all_port_carrier(mux);
			}
			sock_release(priv->sb.conn);
			priv->sb.conn = NULL;
			xeth_mux_clear_sb_connection(mux);
		}
	}
	rcu_barrier();
	xeth_mux_clear_sb_listen(mux);
xeth_mux_main_exit:
	if (ln)
		sock_release(ln);
	xeth_mux_clear_main_task(mux);
	return err;
}

static void xeth_mux_rehash_link_ht(struct net_device *mux)
{
	struct xeth_mux_priv *priv = netdev_priv(mux);
	struct net_device *lower;
	struct list_head *lowers;
	int i, n = 1;

	netdev_for_each_lower_dev(mux, lower, lowers) {
		for (i = n - 1; i < xeth_mux_link_hash_bkts; i += n)
			priv->link[i] = lower;
		n++;
	}
}

static int xeth_mux_del_lower(struct net_device *mux, struct net_device *lower)
{
	lower->flags &= ~IFF_SLAVE;
	netdev_upper_dev_unlink(lower, mux);
	netdev_rx_handler_unregister(lower);
	dev_set_promiscuity(lower, -1);
	dev_put(lower);
	return 0;
}

static int xeth_mux_add_lower(struct net_device *mux, struct net_device *lower,
			      struct netlink_ext_ack *extack)
{
	int (*change_mtu_op)(struct net_device *dev, int new_mtu) =
		lower->netdev_ops->ndo_change_mtu;
	int err;

	err = dev_set_promiscuity(lower, 1);
	if (err < 0)
		return err;

	if (change_mtu_op) {
		err = change_mtu_op(lower, XETH_SIZEOF_JUMBO_FRAME);
		if (err)
			return err;
	}
	if (lower == dev_net(mux)->loopback_dev)
		return -EOPNOTSUPP;

	if (netdev_is_rx_handler_busy(lower))
		return rtnl_dereference(lower->rx_handler) != xeth_mux_demux ?
			-EBUSY : 0;

	err = netdev_rx_handler_register(lower, xeth_mux_demux, mux);
	if (err)
		return err;

	lower->flags |= IFF_SLAVE;
	err = netdev_master_upper_dev_link(lower, mux, NULL, NULL, extack);
	if (err)
		lower->flags &= ~IFF_SLAVE;
	else
		xeth_mux_rehash_link_ht(mux);

	if (err)
		netdev_rx_handler_unregister(lower);
	return err;
}

static int xeth_mux_add_lowers(struct net_device *mux)
{
	struct xeth_mux_priv *priv = netdev_priv(mux);
	struct net_device *link; 
	const char * const * const *links;
	int i, aka, err = 0;

	if (!priv->xeth)
		return -EINVAL;
	links = xeth_vendor_links(priv->xeth);
	for (i = 0; !err && links[i]; i++) {
		for (aka = 0; true; aka++) {
			if (!links[i][aka])
				return -ENODEV;
			link = dev_get_by_name(&init_net, links[i][aka]);
			if (link) {
				err = xeth_mux_add_lower(mux, link, NULL);
				if (err)
					dev_put(link);
				break;
			}
		}
	}
	return err;
}

static bool xeth_mux_has_links(struct net_device *mux)
{
	struct net_device *lower;
	struct list_head *lowers;

	netdev_for_each_lower_dev(mux, lower, lowers)
		return true;
	return false;
}

static int xeth_mux_init(struct net_device *mux)
{
	struct xeth_mux_priv *priv = netdev_priv(mux);

	if (priv->xeth)
		xeth_mux_add_lowers(mux);

	priv->main = kthread_run(xeth_mux_main, mux, "%s", mux->name);
	return IS_ERR(priv->main) ?  PTR_ERR(priv->main) : 0;
}

static void xeth_mux_uninit(struct net_device *mux)
{
	struct xeth_mux_priv *priv = netdev_priv(mux);
	struct net_device *lower;
	struct list_head *lowers;
	int i;

	if (xeth_mux_has_main_task(mux)) {
		kthread_stop(priv->main);
		priv->main = NULL;
		while (xeth_mux_has_main_task(mux)) ;
	}

	netdev_for_each_lower_dev(mux, lower, lowers)
		xeth_mux_del_lower(mux, lower);
	for (i = 0; i < xeth_mux_link_hash_bkts; i++)
		priv->link[i] = NULL;
}

/* The mux's newlink will bind a single lower link, however, with platform
 * probed drivers we retry binding lowers on open (aka. admin up) in case
 * we failed to find them at init.
 */
static int xeth_mux_open(struct net_device *mux)
{
	struct xeth_mux_priv *priv = netdev_priv(mux);
	struct net_device *lower;
	struct list_head *lowers;
	int err;

	if (!xeth_mux_has_links(mux)) {
		if (!priv->xeth)
			return -EINVAL;
		if (err = xeth_mux_add_lowers(mux), err)
			return err;
	}

	netdev_for_each_lower_dev(mux, lower, lowers)
		if (!(lower->flags & IFF_UP))
			xeth_debug_nd_err(lower, dev_open(lower, NULL));

	xeth_mux_check_lower_carrier(mux);

	return 0;
}

static int xeth_mux_stop(struct net_device *mux)
{
	struct net_device *lower;
	struct list_head *lowers;

	if (netif_carrier_ok(mux))
		netif_carrier_off(mux);
	netdev_for_each_lower_dev(mux, lower, lowers)
		dev_close(lower);
	return 0;
}

static int xeth_mux_link_hash_vlan(struct sk_buff *skb)
{
	u16 tci;
	return vlan_get_tag(skb, &tci) ? 0 : tci & 1;
}

static void xeth_mux_vlan_exception(struct net_device *mux, struct sk_buff *skb)
{
	struct xeth_mux_priv *priv = netdev_priv(mux);
	atomic64_t *counters = priv->counters;
	struct vlan_ethhdr *veh = (struct vlan_ethhdr *)skb->data;

	__be16 h_vlan_proto = veh->h_vlan_proto;
	u16 tci = be16_to_cpu(veh->h_vlan_TCI);
	__be16 h_vlan_encapsulated_proto =
		veh->h_vlan_encapsulated_proto;
	xeth_mux_inc__ex_frames(counters);
	xeth_mux_add__ex_bytes(counters, skb->len);
	eth_type_trans(skb, mux);
	skb->vlan_proto = h_vlan_proto;
	skb->vlan_tci = tci & ~VLAN_PRIO_MASK;
	skb->protocol = h_vlan_encapsulated_proto;
	skb_pull_inline(skb, VLAN_HLEN);
	xeth_mux_demux_vlan(mux, skb);
}

static bool xeth_mux_was_vlan_exception(struct net_device *mux,
					struct sk_buff *skb)
{
	const u16 expri = 7 << VLAN_PRIO_SHIFT;
	struct vlan_ethhdr *veh = (struct vlan_ethhdr *)skb->data;

	if (eth_type_vlan(veh->h_vlan_proto)) {
		u16 pri = be16_to_cpu(veh->h_vlan_TCI) & VLAN_PRIO_MASK;
		if (pri == expri) {
			xeth_mux_vlan_exception(mux, skb);
			return true;
		}
	}
	return false;
}

static netdev_tx_t xeth_mux_vlan_xmit(struct sk_buff *skb,
				      struct net_device *mux)
{
	struct xeth_mux_priv *priv = netdev_priv(mux);
	atomic64_t *ls = priv->link_stats;
	struct net_device *link;

	if (xeth_mux_was_vlan_exception(mux, skb))
		return NETDEV_TX_OK;
	link = priv->link[xeth_mux_link_hash_vlan(skb)];
	if (link) {
		if (link->flags & IFF_UP) {
			skb->dev = link;
			no_xeth_debug_skb(skb);
			if (dev_queue_xmit(skb)) {
				xeth_inc_TX_DROPPED(ls);
			} else {
				xeth_inc_TX_PACKETS(ls);
				xeth_add_TX_BYTES(ls, skb->len);
			}
		} else {
			xeth_inc_TX_ERRORS(ls);
			xeth_inc_TX_HEARTBEAT_ERRORS(ls);
			kfree_skb(skb);
		}
	} else {
		xeth_inc_TX_ERRORS(ls);
		xeth_inc_TX_ABORTED_ERRORS(ls);
		kfree_skb(skb);
	}
	return NETDEV_TX_OK;
}

static netdev_tx_t xeth_mux_xmit(struct sk_buff *skb, struct net_device *mux)
{
	struct xeth_mux_priv *priv = netdev_priv(mux);
	switch (priv->encap) {
	case XETH_ENCAP_VLAN:
		return xeth_mux_vlan_xmit(skb, mux);
	case XETH_ENCAP_VPLS:
		/* FIXME vpls */
		break;
	}
	xeth_inc_TX_DROPPED(priv->link_stats);
	kfree_skb(skb);
	return NETDEV_TX_OK;
}

static netdev_tx_t xeth_mux_vlan_encap_xmit(struct sk_buff *skb,
					    struct net_device *nd)
{
	struct xeth_proxy *proxy = netdev_priv(nd);
	struct xeth_mux_priv *priv = netdev_priv(proxy->mux);
	u16 tpid = cpu_to_be16(ETH_P_8021Q);

	if (proxy->kind == XETH_DEV_KIND_VLAN) {
		u16 vid = proxy->xid >> XETH_ENCAP_VLAN_VID_BIT;
		skb = vlan_insert_tag_set_proto(skb, tpid, vid);
		if (skb) {
			tpid = cpu_to_be16(ETH_P_8021AD);
			vid = proxy->xid & XETH_ENCAP_VLAN_VID_MASK;
			skb = vlan_insert_tag_set_proto(skb, tpid, vid);
		}
	} else {
		u16 vid = proxy->xid & XETH_ENCAP_VLAN_VID_MASK;
		skb = vlan_insert_tag_set_proto(skb, tpid, vid);
	}
	if (skb) {
		skb->dev = proxy->mux;
		if (proxy->mux->flags & IFF_UP) {
			dev_queue_xmit(skb);
		} else {
			atomic64_t *ls = priv->link_stats;
			xeth_inc_TX_ERRORS(ls);
			xeth_inc_TX_CARRIER_ERRORS(ls);
			kfree_skb_list(skb);
		}
	}
	return NETDEV_TX_OK;
}

netdev_tx_t xeth_mux_encap_xmit(struct sk_buff *skb, struct net_device *nd)
{
	struct xeth_proxy *proxy = netdev_priv(nd);
	switch (xeth_mux_encap(proxy->mux)) {
	case XETH_ENCAP_VLAN:
		return xeth_mux_vlan_encap_xmit(skb, nd);
	case XETH_ENCAP_VPLS:
		/* FIXME vpls */
		break;
	}
	xeth_inc_TX_DROPPED(proxy->link_stats);
	kfree_skb(skb);
	return NETDEV_TX_OK;
}

static void xeth_mux_get_stats64(struct net_device *mux,
				 struct rtnl_link_stats64 *dst)
{
	struct xeth_mux_priv *priv = netdev_priv(mux);
	xeth_link_stats(dst, priv->link_stats);
}

static void xeth_mux_demux_vlan(struct net_device *mux, struct sk_buff *skb)
{
	struct xeth_mux_priv *priv = netdev_priv(mux);
	atomic64_t *ls = priv->link_stats;
	struct xeth_proxy *proxy = NULL;
	u32 xid;

	skb->priority =
		(typeof(skb->priority))(skb->vlan_tci >> VLAN_PRIO_SHIFT);
	xid = skb->vlan_tci & VLAN_VID_MASK;
	if (eth_type_vlan(skb->protocol)) {
		__be16 tci = *(__be16*)(skb->data);
		__be16 proto = *(__be16*)(skb->data+2);
		xid |= (u32)(be16_to_cpu(tci) & VLAN_VID_MASK) <<
			XETH_ENCAP_VLAN_VID_BIT;
		skb->protocol = proto;
		skb_pull_inline(skb, VLAN_HLEN);
	}
	proxy = xeth_mux_proxy_of_xid(mux, xid);
	if (proxy) {
		struct ethhdr *eth;
		unsigned char *mac = skb_mac_header(skb);
		skb_push(skb, ETH_HLEN);
		memmove(skb->data, mac, 2*ETH_ALEN);
		eth = (typeof(eth))skb->data;
		eth->h_proto = skb->protocol;
		skb->vlan_proto = 0;
		skb->vlan_tci = 0;
		if (dev_forward_skb(proxy->nd, skb) == NET_RX_SUCCESS) {
			xeth_inc_RX_PACKETS(ls);
			xeth_add_RX_BYTES(ls, skb->len);
		} else
			xeth_inc_RX_DROPPED(ls);
	} else {
		no_xeth_debug("no proxy for xid %d; tci 0x%x",
			xid, skb->vlan_tci);
		xeth_inc_RX_ERRORS(ls);
		xeth_inc_RX_NOHANDLER(ls);
		dev_kfree_skb(skb);
	}
}

static rx_handler_result_t xeth_mux_demux(struct sk_buff **pskb)
{
	struct sk_buff *skb = *pskb;
	struct net_device *mux = rcu_dereference(skb->dev->rx_handler_data);
	struct xeth_mux_priv *priv = netdev_priv(mux);
	atomic64_t *ls = priv->link_stats;

	if (eth_type_vlan(skb->vlan_proto)) {
		xeth_mux_demux_vlan(mux, skb);
	} else {
		/* FIXME vpls */
		xeth_inc_RX_ERRORS(ls);
		xeth_inc_RX_FRAME_ERRORS(ls);
		dev_kfree_skb(skb);
	}

	return RX_HANDLER_CONSUMED;
}

const struct net_device_ops xeth_mux_ndo = {
	.ndo_init	= xeth_mux_init,
	.ndo_uninit	= xeth_mux_uninit,
	.ndo_open	= xeth_mux_open,
	.ndo_stop	= xeth_mux_stop,
	.ndo_start_xmit	= xeth_mux_xmit,
	.ndo_get_stats64= xeth_mux_get_stats64,
};

static void xeth_mux_eto_get_drvinfo(struct net_device *nd,
				     struct ethtool_drvinfo *drvinfo)
{
	strlcpy(drvinfo->driver, xeth_mux_drvname, sizeof(drvinfo->driver));
	strlcpy(drvinfo->version, XETH_VERSION, sizeof(drvinfo->version));
	strlcpy(drvinfo->fw_version, "n/a", ETHTOOL_FWVERS_LEN);
	strlcpy(drvinfo->erom_version, "n/a", ETHTOOL_EROMVERS_LEN);
	strlcpy(drvinfo->bus_info, "n/a", ETHTOOL_BUSINFO_LEN);
	drvinfo->n_priv_flags = xeth_mux_n_flags;
	drvinfo->n_stats = xeth_mux_n_counters;
}

static int xeth_mux_eto_get_sset_count(struct net_device *nd, int sset)
{
	switch (sset) {
	case ETH_SS_TEST:
		return 0;
	case ETH_SS_STATS:
		return xeth_mux_n_counters;
	case ETH_SS_PRIV_FLAGS:
		return xeth_mux_n_flags;
	default:
		return -EOPNOTSUPP;
	}
}

static void xeth_mux_eto_get_strings(struct net_device *nd, u32 sset, u8 *data)
{
	static const char *const counter[] = { xeth_mux_counter_names() };
	static const char *const flag[] = { xeth_mux_flag_names() };
	char *p = (char *)data;
	int i;

	switch (sset) {
	case ETH_SS_TEST:
		break;
	case ETH_SS_STATS:
		for (i = 0; counter[i]; i++, p += ETH_GSTRING_LEN)
			strlcpy(p, counter[i], ETH_GSTRING_LEN);
		break;
	case ETH_SS_PRIV_FLAGS:
		for (i = 0; flag[i]; i++, p += ETH_GSTRING_LEN)
			strlcpy(p, flag[i], ETH_GSTRING_LEN);
		break;
	}
}

static void xeth_mux_eto_get_stats(struct net_device *mux,
				   struct ethtool_stats *stats,
				   u64 *data)
{
	struct xeth_mux_priv *priv = netdev_priv(mux);
	enum xeth_mux_counter c;
	for (c = 0; c < xeth_mux_n_counters; c++)
		*data++ = atomic64_read(&priv->counters[c]);
}

static u32 xeth_mux_eto_get_priv_flags(struct net_device *mux)
{
	struct xeth_mux_priv *priv = netdev_priv(mux);
	u32 flags;
	barrier();
	flags = priv->flags;
	return flags;
}

static const struct ethtool_ops xeth_mux_ethtool_ops = {
	.get_drvinfo = xeth_mux_eto_get_drvinfo,
	.get_link = ethtool_op_get_link,
	.get_sset_count = xeth_mux_eto_get_sset_count,
	.get_strings = xeth_mux_eto_get_strings,
	.get_ethtool_stats = xeth_mux_eto_get_stats,
	.get_priv_flags = xeth_mux_eto_get_priv_flags,
};

void xeth_mux_dellink(struct net_device *mux, struct list_head *unregq)
{
	struct xeth_mux_priv *priv = netdev_priv(mux);
	struct xeth_proxy *proxy;

	if (IS_ERR_OR_NULL(mux) || mux->reg_state != NETREG_REGISTERED)
		return;

	list_for_each_entry_rcu(proxy, &priv->proxy.bridges, kin)
		unregister_netdevice_queue(proxy->nd, unregq);
	list_for_each_entry_rcu(proxy, &priv->proxy.vlans, kin)
		unregister_netdevice_queue(proxy->nd, unregq);
	list_for_each_entry_rcu(proxy, &priv->proxy.lags, kin)
		unregister_netdevice_queue(proxy->nd, unregq);
	list_for_each_entry_rcu(proxy, &priv->proxy.ports, kin)
		unregister_netdevice_queue(proxy->nd, unregq);
	list_for_each_entry_rcu(proxy, &priv->proxy.lbs, kin)
		unregister_netdevice_queue(proxy->nd, unregq);
	unregister_netdevice_queue(mux, unregq);
}

static struct net *xeth_mux_get_link_net(const struct net_device *mux)
{
	return dev_net(mux);
}

struct rtnl_link_ops xeth_mux_lnko = {
	.kind		= xeth_mux_drvname,
	.priv_size	= sizeof(struct xeth_mux_priv),
	.setup		= xeth_mux_setup,
	.validate	= xeth_mux_validate,
#if 0	/* FIXME ip link add type xeth-mux */
	.newlink	= xeth_mux_newlink,
#endif
	.dellink	= xeth_mux_dellink,
	.get_link_net	= xeth_mux_get_link_net,
};
