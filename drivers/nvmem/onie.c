/* SPDX-License-Identifier: GPL-2.0
 * Copyright(c) 2018-2020 Platina Systems, Inc.
 *
 * Contact Information:
 * sw@platina.com
 * Platina Systems, 3180 Del La Cruz Blvd, Santa Clara, CA 95054
 */

#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/crc32.h>
#include <asm/unaligned.h>
#include <linux/of_device.h>
#include <linux/acpi.h>
#include <linux/platform_device.h>
#include <linux/nvmem-consumer.h>
#include <linux/nvmem-provider.h>
#include <linux/etherdevice.h>
#include <linux/glob.h>

#define onie_pr_debug(format, args...)					\
do {									\
	pr_debug("%s:" format "\n", __func__, ##args);			\
} while(0)

#define onie_pr_err(format, args...)					\
do {									\
	pr_err("%s:" format "\n", __func__, ##args);			\
} while(0)

#define onie_pr_if_err(expr)						\
({									\
	int _err = (expr);						\
	if (_err < 0)							\
		onie_pr_err("%s == %d", #expr, _err);			\
	(_err);								\
})

#define onie_pr_if_ptr_err(expr)					\
({									\
	void *_ptr = (expr);						\
	if (!_ptr) {							\
		onie_pr_err("%s == nil", #expr);			\
		_ptr = ERR_PTR(-ENOMEM);				\
	} else if (IS_ERR(_ptr))					\
		onie_pr_err("%s == %ld", #expr, PTR_ERR(_ptr));		\
	(_ptr);								\
})

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Platina Systems");
MODULE_DESCRIPTION("a platform driver for ONIE format NVMEM");

static const struct of_device_id onie_of_match[] = {
	{ .compatible = "linux,onie", },
	{ /* END OF LIST */ },
};
MODULE_DEVICE_TABLE(of, onie_of_match);

static const char onie_driver_name[] = "nvmem_onie";
static int onie_probe(struct platform_device *);
static int onie_remove(struct platform_device *);

static struct platform_driver onie_platform_driver = {
	.driver = {
		.name = onie_driver_name,
		.of_match_table = onie_of_match,
	},
	.probe = onie_probe,
	.remove = onie_remove,
};

module_platform_driver(onie_platform_driver);

enum onie_max {
	onie_max_data	= 2048,
	onie_max_tlv	=  255,
};

enum onie_type {
	onie_type_nvmem_name		= 0x11,
	onie_type_nvmem_cache		= 0x12,
	onie_type_product_name		= 0x21,
	onie_type_part_number		= 0x22,
	onie_type_serial_number		= 0x23,
	onie_type_mac_base		= 0x24,
	onie_type_manufacture_date	= 0x25,
	onie_type_device_version	= 0x26,
	onie_type_label_revision	= 0x27,
	onie_type_platform_name		= 0x28,
	onie_type_onie_version		= 0x29,
	onie_type_num_macs		= 0x2a,
	onie_type_manufacturer		= 0x2b,
	onie_type_country_code		= 0x2c,
	onie_type_vendor		= 0x2d,
	onie_type_diag_version		= 0x2e,
	onie_type_service_tag		= 0x2f,
	onie_type_vendor_extension	= 0xfd,
	onie_type_crc			= 0xfe,
};

struct onie_priv {
	struct platform_device *pd;
	struct nvmem_cell *cell;
	struct {
		struct	mutex	mutex;
		u8	data[onie_max_data];
		size_t	n;
	} cache;
	struct {
		struct	mutex	mutex;
		u8	data[onie_max_data];
	} writeback;
};

static ssize_t onie_cache_unlock(struct onie_priv *priv, ssize_t n)
{
	mutex_unlock(&priv->cache.mutex);
	return n;
}

static const struct attribute_group *onie_attr_groups[];

static const char onie_header_id[] = "TlvInfo";

enum { onie_header_version = 1 };

enum onie_sz {
	onie_sz_header_id	= sizeof(onie_header_id),
	onie_sz_header_version	= sizeof(u8),
	onie_sz_header_length	= sizeof(u16),
	onie_sz_header		= onie_sz_header_id + onie_sz_header_version +
		onie_sz_header_length,
	onie_sz_tlv_type	= sizeof(u8),
	onie_sz_tlv_length	= sizeof(u8),
	onie_sz_crc		= sizeof(u32),
	onie_sz_mac		= 6,
};

enum onie_min {
	onie_min_tlv	= onie_sz_tlv_type + onie_sz_tlv_length,
	onie_min_data	= onie_sz_header + onie_min_tlv + onie_sz_crc,
};

struct __attribute__((packed)) onie_header {
	u8	id[onie_sz_header_id];
	u8	version;
	u8	length[onie_sz_header_length];
};

struct onie_tlv {
	u8 t;
	u8 l;
	u8 v[];
};

struct __attribute__((packed)) onie_data {
	struct onie_header header;
	struct onie_tlv tlv;
};

static u16 onie_u16(const void *p)
{
	return get_unaligned_be16(p);
}

static u32 onie_u32(const void *p)
{
	return get_unaligned_be32(p);
}

static void onie_set_u16(u16 u, void *p)
{
	put_unaligned_be16(u, p);
}

static void onie_set_u32(u32 u, void *p)
{
	put_unaligned_be32(u, p);
}

static struct onie_tlv *onie_next(struct onie_tlv *tlv)
{
	return (struct onie_tlv *)((u8*)tlv + onie_min_tlv + tlv->l);
}

static u32 onie_crc(u8 *data, size_t sz)
{
	return crc32_le(~0, data, sz) ^ ~0;
}

static void onie_append_crc(u8 *data, size_t sz)
{
	onie_set_u32(onie_crc(data, sz), data + sz);
}

static void onie_reset_header(struct onie_header *h)
{
	strcpy(h->id, onie_header_id);
	h->version = onie_header_version;
	onie_set_u16(0, h->length);
}

static void onie_reset_crc(struct onie_tlv *tlv)
{
	tlv->t = onie_type_crc;
	tlv->l = onie_sz_crc;
	memset(tlv->v, 0, onie_sz_crc);
}

/**
 * onie_cache_validate() - verify ONIE ID, Version, and CRC.
 *
 * Return:
 * * -EBADR	- sz && sz < onie_min_data
 * * -EIDRM	- invalid ID
 * * -EINVAL	- invalid Version
 * * -EFBIG	- header length > max
 * * -EBADF	- CRC mismatch
 * * >0		- total ONIE data length
 */
static ssize_t onie_cache_validate(void *data)
{
	struct onie_header *h = data;
	size_t tlvsz, fullsz, crcsz;
	u32 crc_read, crc_calc;
	ssize_t n;

	n = onie_pr_if_err(strcmp(onie_header_id, data) ? -EIDRM : 0);
	if (n)
		return n;
	n = onie_pr_if_err(h->version != onie_header_version ? -EINVAL : 0);
	if (n)
		return n;
	tlvsz = onie_u16(h->length);
	fullsz = sizeof(*h) + tlvsz;
	n = onie_pr_if_err(fullsz > onie_max_data ? -EFBIG : 0);
	if (n)
		return n;
	crcsz = fullsz - onie_sz_crc;
	crc_read = onie_u32(data + crcsz);
	crc_calc = onie_crc(data, crcsz);
#if 0
	onie_pr_debug("crc: 0x%08x vs. 0x%08x\n", crc_read, crc_calc);
	onie_pr_debug("crc32_le:0: 0x%08x\n", crc32_le(0, data, sz));
	onie_pr_debug("crc32_be:0: 0x%08x\n", crc32_be(0, data, sz));
	onie_pr_debug("crc32_le:~0: 0x%08x\n", crc32_le(~0, data, sz));
	onie_pr_debug("crc32_be:~0: 0x%08x\n", crc32_be(~0, data, sz));
	onie_pr_debug("crc32_le:0:^~0: 0x%08x\n", crc32_le(0, data, sz)^~0);
	onie_pr_debug("crc32_be:0:^~0: 0x%08x\n", crc32_be(0, data, sz)^~0);
	onie_pr_debug("crc32_le:~0:^~0: 0x%08x\n", crc32_le(~0, data, sz)^~0);
	onie_pr_debug("crc32_be:~0:^~0: 0x%08x\n", crc32_be(~0, data, sz)^~0);
#endif
	n = onie_pr_if_err(crc_read != crc_calc) ? -EBADF : fullsz;
	return n;
}

static void onie_cache_init(void *data)
{
	struct onie_header *h = data;
	struct onie_data *od = (struct onie_data *)data;
	struct onie_tlv *crc_tlv = &od->tlv;
	u16 tl;

	strcpy(h->id, onie_header_id);
	h->version = onie_header_version;
	tl = onie_sz_header + sizeof(*crc_tlv) + onie_sz_crc;
	onie_set_u16(tl, h->length);
	onie_reset_crc(crc_tlv);
	onie_append_crc(data, tl - onie_sz_crc);
	onie_pr_if_err(onie_cache_validate(data));
}

static void onie_cell_put(void *cell)
{
	nvmem_cell_put(cell);
}

static ssize_t onie_cell_validate(struct onie_priv *priv)
{
	const char *name = priv->pd->name;
	void *data;
	int err;

	mutex_lock(&priv->cache.mutex);
	if (IS_ERR(priv->cell))
		return onie_cache_unlock(priv, PTR_ERR(priv->cell));
	if (priv->cell)
		return onie_cache_unlock(priv, priv->cache.n);
	priv->cell = nvmem_cell_get(&priv->pd->dev, "onie-data");
	if (IS_ERR(priv->cell)) {
		err = PTR_ERR(priv->cell);
		if (err == -EPROBE_DEFER)
			priv->cell = NULL;
		else
			onie_pr_err("nvmem_cell_get(%s): %d", name, err);
		return onie_cache_unlock(priv, err);
	} else if (!priv->cell) {
		onie_pr_err("nvmem_cell_get(%s): nil", name);
		return onie_cache_unlock(priv, -ENXIO);
	}
	data = nvmem_cell_read(priv->cell, &priv->cache.n);
	if (IS_ERR(data)) {
		err = PTR_ERR(data);
		onie_pr_err("nvmem_cell_read(%s): %d", name, err);
		onie_cell_put(priv->cell);
		priv->cell = data;
		return onie_cache_unlock(priv, err);
	}
	memcpy(priv->cache.data, data, priv->cache.n);
	kfree(data);
	err = devm_add_action(&priv->pd->dev, onie_cell_put, priv->cell);
	if (err) {
		onie_pr_err("devm_add_action(put, %s)): %d", name, err);
		onie_cell_put(priv->cell);
		priv->cell = ERR_PTR(err);
		return onie_cache_unlock(priv, err);
	}
	priv->cache.n = onie_cache_validate(priv->cache.data);
	if (priv->cache.n < 0)
		onie_cache_init(priv->cache.data);
	return onie_cache_unlock(priv, priv->cache.n);
}

/**
 * onie_get_tlv() - get cached ONIE NVMEM value.
 * @priv: onie drvdata
 * @t: &enum onie_type
 * @l: sizeof destination
 * @v: destination buffer
 *
 * This expects these @l sized destinations per @t type::
 *
 *	onie_max_tlv	onie_type_product_name,
 *			onie_type_part_number,
 *			onie_type_serial_number,
 *			onie_type_manufacture_date,
 *			onie_type_label_revision,
 *			onie_type_platform_name,
 *			onie_type_onie_version,
 *			onie_type_manufacturer,
 *			onie_type_country_code,
 *			onie_type_vendor,
 *			onie_type_diag_version,
 *			onie_type_service_tag
 *
 *	ETH_ALEN	onie_type_mac_base
 *
 *	sizeof(u8)	onie_type_device_version
 *
 *	sizeof(u16)	onie_type_num_macs
 *
 *	sizeof(u32)	onie_type_crc
 *
 *	onie_max_data	onie_type_vendor_extension
 *
 * Return:
 * * -ENOMSG	- type @t unavailable
 * * -EINVAL	- size @l insufficient for value
 * * >=0	- value length
 */
static ssize_t onie_get_tlv(struct onie_priv *priv,
			    enum onie_type t, size_t l, u8 *v)
{
	struct onie_data *data = (struct onie_data *)priv->cache.data;
	struct onie_tlv *tlv;
	ssize_t n;
	u16 hl;
	u8 *end;

	n = onie_cell_validate(priv);
	if (n < 0)
		return onie_cache_unlock(priv, n);
	mutex_lock(&priv->cache.mutex);
	n = -ENOMSG;
	hl = onie_u16(data->header.length);
	if (!hl)
		return onie_cache_unlock(priv, -ENOMSG);
	end = priv->cache.data + sizeof(data->header) + hl;
	for (tlv = &data->tlv; (u8*)tlv < end; tlv = onie_next(tlv))
		if (tlv->t == t) {
			if (n < 0)
				n = 0;
			if (tlv->l) {
				if (l < n + tlv->l)
					return onie_cache_unlock(priv, -EINVAL);
				memcpy(v, tlv->v, tlv->l);
				n += tlv->l;
				v += tlv->l;
			} else	/* may have 0 length values */
				n = 0;
			if (t != onie_type_vendor_extension)
				return onie_cache_unlock(priv, n);
		}
	return onie_cache_unlock(priv, n);
}

static struct onie_tlv *onie_insert_tlv(struct onie_tlv *dst, enum onie_type t,
					size_t l, const u8 *v, size_t *phl,
					u8 *over)
{
	while (l) {
		size_t n = (l < onie_max_tlv) ? l : onie_max_tlv;
		if ((u8*)dst + sizeof(*dst) + n > over)
			return NULL;
		dst->t = t;
		dst->l = n;
		memcpy(dst->v, v, n);
		dst = onie_next(dst);
		v += n;
		l -= n;
		*phl += sizeof(*dst) + n;
	}
	return dst;
}

/**
 * onie_set_tlv() - set ONIE NVMEM value.
 * @priv: onie drvdata
 * @t: &enum onie_type
 * @l: sizeof destination
 * @v: destination buffer
 *
 * This expects @l sized destinations per @t type as @onie_get_tlv.
 *
 * Return:
 * * -ERANGE	- @v exceeds range of type @t
 * * -EINVAL	- size @l insufficient for value
 * * >=0	- value length
 */
static int onie_set_tlv(struct onie_priv *priv,
			enum onie_type t, size_t l, const u8 *v)
{
	struct onie_header *cache_h,*wb_h;
	struct onie_tlv *cache_tlv, *wb_tlv;
	u8 *over;
	size_t tl, hl = 0;

	cache_h = (struct onie_header *)priv->cache.data;
	wb_h = (struct onie_header *)priv->writeback.data;
	cache_tlv = (struct onie_tlv *)(priv->cache.data + sizeof(*cache_h));
	wb_tlv = (struct onie_tlv *)(priv->writeback.data + sizeof(*wb_h));
	over = priv->writeback.data + onie_max_data;

	mutex_lock(&priv->cache.mutex);
	mutex_lock(&priv->writeback.mutex);
	onie_reset_header(wb_h);
	if (onie_u16(cache_h->length) != 0)
		for (; cache_tlv->t != onie_type_crc;
		     cache_tlv = onie_next(cache_tlv)) {
			if (cache_tlv->t != t) {
				size_t n = sizeof(*cache_tlv) + cache_tlv->l;
				memcpy(wb_tlv, cache_tlv, n);
				wb_tlv = onie_next(wb_tlv);
				hl += n;
				continue;
			}
			wb_tlv = onie_insert_tlv(wb_tlv, t, l, v, &hl, over);
			if (!wb_tlv) {
				mutex_unlock(&priv->writeback.mutex);
				mutex_unlock(&priv->cache.mutex);
				return -EINVAL;
			} else
				l = 0;
		}
	if (l) {
		wb_tlv = onie_insert_tlv(wb_tlv, t, l, v, &hl, over);
		if (!wb_tlv) {
			mutex_unlock(&priv->writeback.mutex);
			mutex_unlock(&priv->cache.mutex);
			return -EINVAL;
		}
	}
	onie_reset_crc(wb_tlv);
	hl += sizeof(*wb_tlv) + onie_sz_crc;
	onie_set_u16(hl, wb_h->length);
	tl = sizeof(*wb_h) + hl;
	onie_append_crc(priv->writeback.data, tl - onie_sz_crc);
	memcpy(priv->cache.data, priv->writeback.data, tl);
	mutex_unlock(&priv->cache.mutex);
	mutex_unlock(&priv->writeback.mutex);
	return 0;
}

static int onie_probe(struct platform_device *pd)
{
	struct onie_priv *priv;

	priv = devm_kzalloc(&pd->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;
	priv->pd = pd;
	mutex_init(&priv->cache.mutex);
	mutex_init(&priv->writeback.mutex);
	platform_set_drvdata(pd, priv);
	onie_cell_validate(priv);
	return devm_device_add_groups(&pd->dev, onie_attr_groups);
}

static int onie_remove(struct platform_device *pd)
{
	platform_set_drvdata(pd, NULL);
	return 0;
}

static ssize_t onie_show_default(struct device *dev,
				 enum onie_type t,
				 char *buf)
{
	struct onie_priv *priv = dev_get_drvdata(dev);
	ssize_t n = onie_get_tlv(priv, t, PAGE_SIZE, buf);
	if (n == 1 && buf[0] == '\0')
		n = 0;
	return n;
}

#define onie_show(NAME)							\
static ssize_t onie_show_##NAME(struct device *dev,			\
				struct device_attribute *attr,		\
				char *buf)				\
{									\
	return onie_show_default(dev, onie_type_##NAME, buf);		\
}

onie_show(product_name)
onie_show(part_number)
onie_show(serial_number)
onie_show(manufacture_date)
onie_show(label_revision)
onie_show(platform_name)
onie_show(onie_version)
onie_show(manufacturer)
onie_show(country_code)
onie_show(vendor)
onie_show(diag_version)
onie_show(service_tag)
onie_show(vendor_extension)

static ssize_t onie_show_cache(struct device *dev,
			       struct device_attribute *attr,
			       char *buf)
{
	struct onie_priv *priv = dev_get_drvdata(dev);
	mutex_lock(&priv->cache.mutex);
	memcpy(buf, priv->cache.data, onie_max_data);
	mutex_unlock(&priv->cache.mutex);
	return onie_max_data;
}

static ssize_t onie_show_mac_base(struct device *dev,
				  struct device_attribute *attr,
				  char *buf)
{
	struct onie_priv *priv = dev_get_drvdata(dev);
	u8 v[onie_sz_mac];
	ssize_t n = onie_get_tlv(priv, onie_type_mac_base, onie_sz_mac, v);
	if (n == onie_sz_mac)
		n = scnprintf(buf, PAGE_SIZE, "%pM", v);
	else if (n > 0)
		n = -EFBIG;
	return n;
}

static ssize_t onie_show_device_version(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	struct onie_priv *priv = dev_get_drvdata(dev);
	ssize_t n = onie_get_tlv(priv, onie_type_device_version,
				 sizeof(u8), buf);
	if (n == sizeof(u8)) {
		u8 dv = buf[0];
		n = scnprintf(buf, PAGE_SIZE, "%u", dv);
	} else if (n > 0)
		n = -EFBIG;
	return n;
}

static ssize_t onie_show_num_macs(struct device *dev,
				  struct device_attribute *attr,
				  char *buf)
{
	struct onie_priv *priv = dev_get_drvdata(dev);
	ssize_t n = onie_get_tlv(priv, onie_type_num_macs, sizeof(u16), buf);
	if (n == sizeof(u16)) {
		u16 macs = be16_to_cpu(*(u16*)buf);
		n = scnprintf(buf, PAGE_SIZE, "%u", macs);
	} else if (n > 0)
		n = -EFBIG;
	return n;
}

static ssize_t onie_show_crc(struct device *dev,
			     struct device_attribute *attr,
			     char *buf)
{
	struct onie_priv *priv = dev_get_drvdata(dev);
	ssize_t n = onie_get_tlv(priv, onie_type_crc, onie_sz_crc, buf);
	if (n == onie_sz_crc) {
		u32 crc = be32_to_cpu(*(u32*)buf);
		n = scnprintf(buf, PAGE_SIZE, "0x%08x", crc);
	} else if (n > 0)
		n = -EFBIG;
	return n;
}

static ssize_t onie_store_default(struct device *dev,
				  enum onie_type t, size_t sz, const char *buf)
{
	struct onie_priv *priv = dev_get_drvdata(dev);
	ssize_t l = (sz > 0 && buf[sz-1] == '\n') ? sz-1 : sz;
	int err = onie_set_tlv(priv, t, l, (u8*)(buf));
	return err ? err : sz;
}

#define onie_store(NAME)						\
static ssize_t onie_store_##NAME(struct device *dev,			\
				 struct device_attribute *attr,		\
				 const char *buf, size_t sz)		\
{									\
	return onie_store_default(dev, onie_type_##NAME, sz, buf);	\
}

onie_store(product_name)
onie_store(part_number)
onie_store(serial_number)
onie_store(manufacture_date)
onie_store(label_revision)
onie_store(platform_name)
onie_store(onie_version)
onie_store(manufacturer)
onie_store(country_code)
onie_store(vendor)
onie_store(diag_version)
onie_store(service_tag)
onie_store(vendor_extension)

static ssize_t onie_store_cache(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t sz)
{
	struct onie_priv *priv = dev_get_drvdata(dev);
	if (sz >= onie_max_data)
		return -ERANGE;
	mutex_lock(&priv->cache.mutex);
	memcpy(priv->cache.data, buf, sz);
	mutex_unlock(&priv->cache.mutex);
	return sz;
}

static ssize_t onie_store_mac_base(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf, size_t sz)
{
	struct onie_priv *priv = dev_get_drvdata(dev);
	unsigned int uv[onie_sz_mac];
	u8 v[onie_sz_mac];
	int i, err;

	err = sscanf(buf, "%x:%x:%x:%x:%x:%x%*c",
		     &uv[0], &uv[1], &uv[2], &uv[3], &uv[4], &uv[5]);
	if (err < 0)
		return err;
	if (err != onie_sz_mac)
		return -EINVAL;
	for (i = 0; i < onie_sz_mac; i++)
		v[i] = uv[i] & U8_MAX;
	err = onie_set_tlv(priv, onie_type_mac_base, onie_sz_mac, v);
	return err ? err : sz;
}

static ssize_t onie_store_num_macs(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf, size_t sz)
{
	struct onie_priv *priv = dev_get_drvdata(dev);
	unsigned int uv;
	u8 v[sizeof(u16)];
	int err;

	err = kstrtouint(buf, 0, &uv);
	if (err < 0)
		return err;
	if (uv > U16_MAX)
		return -ERANGE;
	*(u16*)v = cpu_to_be16((u16)uv);
	err = onie_set_tlv(priv, onie_type_num_macs, sizeof(u16), v);
	return err ? err : sz;
}

static ssize_t onie_store_device_version(struct device *dev,
					 struct device_attribute *attr,
					 const char *buf, size_t sz)
{
	struct onie_priv *priv = dev_get_drvdata(dev);
	unsigned int uv;
	u8 v[sizeof(u8)];
	int err;

	err = kstrtouint(buf, 0, &uv);
	if (err < 0)
		return err;
	if (uv > U8_MAX)
		return -ERANGE;
	v[0] = uv & U8_MAX;
	err = onie_set_tlv(priv, onie_type_device_version, sizeof(u8), v);
	return err ? err : sz;
}

static ssize_t onie_store_crc(struct device *dev,
			      struct device_attribute *attr,
			      const char *buf, size_t sz)
{
	struct onie_priv *priv = dev_get_drvdata(dev);
	ssize_t n;

	n = onie_cell_validate(priv);
	if (n <= 0)
		return n;
	if (IS_ERR(priv->cell))
		n = PTR_ERR(priv->cell);
	else
		n = nvmem_cell_write(priv->cell, priv->cache.data, n);
	return (n < 0) ? n : sz;
}

#define onie_attr(NAME) \
struct device_attribute onie_attr_##NAME = \
	__ATTR(NAME, 0644, onie_show_##NAME, onie_store_##NAME)

static onie_attr(cache);
static onie_attr(product_name);
static onie_attr(part_number);
static onie_attr(serial_number);
static onie_attr(mac_base);
static onie_attr(manufacture_date);
static onie_attr(device_version);
static onie_attr(label_revision);
static onie_attr(platform_name);
static onie_attr(onie_version);
static onie_attr(num_macs);
static onie_attr(manufacturer);
static onie_attr(country_code);
static onie_attr(vendor);
static onie_attr(diag_version);
static onie_attr(service_tag);
static onie_attr(vendor_extension);
static onie_attr(crc);

static struct attribute *onie_attrs[] = {
	&onie_attr_cache.attr,
	&onie_attr_product_name.attr,
	&onie_attr_part_number.attr,
	&onie_attr_serial_number.attr,
	&onie_attr_mac_base.attr,
	&onie_attr_manufacture_date.attr,
	&onie_attr_device_version.attr,
	&onie_attr_label_revision.attr,
	&onie_attr_platform_name.attr,
	&onie_attr_onie_version.attr,
	&onie_attr_num_macs.attr,
	&onie_attr_manufacturer.attr,
	&onie_attr_country_code.attr,
	&onie_attr_vendor.attr,
	&onie_attr_diag_version.attr,
	&onie_attr_service_tag.attr,
	&onie_attr_vendor_extension.attr,
	&onie_attr_crc.attr,
	NULL,
};

static const struct attribute_group onie_attr_group = {
	.attrs = onie_attrs,
};

static const struct attribute_group *onie_attr_groups[] = {
	&onie_attr_group,
	NULL,
};
