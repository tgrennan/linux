/**
 * SPDX-License-Identifier: GPL-2.0
 * Copyright(c) 2018-2020 Platina Systems, Inc.
 *
 * Contact Information:
 * sw@platina.com
 * Platina Systems, 3180 Del La Cruz Blvd, Santa Clara, CA 95054
 */

#include "xeth_platform.h"
#include "xeth_mux.h"
#include "xeth_port.h"
#include "xeth_qsfp.h"
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/of_device.h>

/* Driver Data indices */
enum xeth_platform_dd {
	xeth_platform_dd_platina_mk1,
	xeth_platform_dd_platina_mk1alpha,
};

extern struct xeth_platform xeth_platina_mk1_platform;
extern struct xeth_platform xeth_platina_mk1alpha_platform;

static const struct xeth_platform * const xeth_platforms[] = {
	[xeth_platform_dd_platina_mk1] = &xeth_platina_mk1_platform,
	[xeth_platform_dd_platina_mk1alpha] = &xeth_platina_mk1alpha_platform,
};

static const struct platform_device_id xeth_platform_device_ids[] = {
	{
		.name = "platina-mk1",
		.driver_data = xeth_platform_dd_platina_mk1,
	},
	{
		.name = "platina-mk1alpha",
		.driver_data = xeth_platform_dd_platina_mk1alpha,
	},
	{},
};

MODULE_DEVICE_TABLE(platform, xeth_platform_device_ids);

static const struct of_device_id xeth_platform_of_match[] = {
	{
		.compatible = "linux,platina-mk1",
		.data = &xeth_platina_mk1_platform,
	},
	{
		.compatible = "linux,platina-mk1alpha",
		.data = &xeth_platina_mk1alpha_platform,
	},
	{},
};

MODULE_DEVICE_TABLE(of, xeth_platform_of_match);

int xeth_platform_provision[512], xeth_platform_provisioned;
module_param_array_named(provision, xeth_platform_provision, int,
			 &xeth_platform_provisioned, 0644);
MODULE_PARM_DESC(provision, " 1 (default), 2, or 4 subports per port");

ssize_t xeth_platform_subports(size_t port)
{
	return port < ARRAY_SIZE(xeth_platform_provision) ?
		xeth_platform_provision[port] : -EINVAL;
}

static inline ssize_t
xeth_platform_show_port_provision(struct device *dev,
				  struct device_attribute *attr,
				  char *buf)
{
	int port;
	for (port = 0;
	     port < ARRAY_SIZE(xeth_platform_provision) &&
	     xeth_platform_provision[port];
	     port++)
		buf[port] = xeth_platform_provision[port] + '0';
	return port;
}

static struct device_attribute xeth_platform_provision_attr = {
	.attr.name = "provision",
	.attr.mode = VERIFY_OCTAL_PERMISSIONS(0444),
	.show = xeth_platform_show_port_provision,
};

static int xeth_platform_probe(struct platform_device *pd)
{
	const struct xeth_platform *platform;
	struct net_device *mux;
	int err, port, subport;

	platform = of_device_get_match_data(&pd->dev);
	if (!platform && pd->id_entry)
		platform = xeth_platforms[pd->id_entry->driver_data];
	if (!platform) {
		pr_err("%s: no match\n", pd->name);
		return -EINVAL;
	}

	err = device_create_file(&pd->dev, &xeth_platform_provision_attr);
	if (err)
		return err;

	err = xeth_platform_init(platform, pd);
	if (err) {
		device_remove_file(&pd->dev, &xeth_platform_provision_attr);
		return err;
	}

	mux = xeth_mux(platform, &pd->dev);
	if (IS_ERR(mux)) {
		xeth_platform_uninit(platform);
		device_remove_file(&pd->dev, &xeth_platform_provision_attr);
		return PTR_ERR(mux);
	}

	platform_set_drvdata(pd, mux);

	for (port = 0; port < xeth_platform_ports(platform); port++)
		switch (xeth_platform_provision[port]) {
		case 1:
			xeth_port(mux, port, -1);
			break;
		case 2:
		case 4:
			for (subport = 0;
			     subport < xeth_platform_provision[port];
			     subport++)
				xeth_port(mux, port, subport);
			break;
		default:
			xeth_platform_provision[port] = 1;
			xeth_port(mux, port, -1);
		}

	return 0;
}

static int xeth_platform_remove(struct platform_device *pd)
{
	struct net_device *mux;
	LIST_HEAD(q);

	mux = platform_get_drvdata(pd);
	if (!mux)
		return 0;
	platform_set_drvdata(pd, NULL);

	device_remove_file(&pd->dev, &xeth_platform_provision_attr);
	xeth_platform_uninit(xeth_mux_platform(mux));

	rtnl_lock();
	xeth_mux_lnko.dellink(mux, &q);
	unregister_netdevice_many(&q);
	rtnl_unlock();
	rcu_barrier();

	return 0;
}

struct platform_driver xeth_platform_driver = {
	.driver		= {
		.name = KBUILD_MODNAME,
		.of_match_table = xeth_platform_of_match,
	},
	.probe		= xeth_platform_probe,
	.remove		= xeth_platform_remove,
	.id_table	= xeth_platform_device_ids,
};
