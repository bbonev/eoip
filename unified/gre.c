// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *	GRE over IPv4 demultiplexer driver, extended with dispatch of the
 *	non-standard EoIP pseudo-GRE header.
 *
 *	Authors: Dmitry Kozlov (xeb@mail.ru)
 *	EoIP extension: Boian Bonev (bbonev@ipacct.com)
 *
 *	This is a drop-in replacement for the kernel's gre.ko: it keeps
 *	the exported API of every supported kernel generation so that the
 *	in-tree users (ip_gre, ip6_gre, pptp) keep working, and adds the
 *	EoIP protocol slots consumed by eoip.ko.
 *
 *	Unified source for kernels 3.2 .. 7.0+; written against the
 *	current kernel API with compat fences for older generations:
 *	 - v3.2  .. v3.10: plain demultiplexer (net/ipv4/gre.c era)
 *	 - v3.11 .. v4.2:  + gre_cisco_* / gre_build_header infrastructure
 *	 - v4.3  .. v4.6:  plain demultiplexer again
 *	 - v4.7+:          + gre_parse_header()
 *	Version boundaries verified against mainline release tags; fences
 *	that deviate towards a widely backported stable change say so in
 *	a comment.
 *
 *	Note: on v3.11..v3.15 the in-tree gre.ko also contained the GSO
 *	offload glue (gre_offload.o); this standalone replacement does
 *	not, so standard GRE loses GSO there.  EoIP is unaffected.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/version.h>

#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 2, 0)
#error "kernels older than 3.2 are not supported"
#endif

#include <linux/module.h>
#include <linux/if.h>
#include <linux/icmp.h>
#include <linux/kernel.h>
#include <linux/kmod.h>
#include <linux/skbuff.h>
#include <linux/in.h>
#include <linux/ip.h>
#include <linux/netdevice.h>
#include <linux/if_tunnel.h>
#include <linux/spinlock.h>
#include <net/protocol.h>
#include <net/gre.h>
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 10, 0)
#include <net/ip_tunnels.h>
#endif
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 0, 0) || \
	(LINUX_VERSION_CODE >= KERNEL_VERSION(4, 19, 20) && \
	 LINUX_VERSION_CODE < KERNEL_VERSION(4, 20, 0))
#define EOIP_HAVE_ERSPAN_LOOKUP 1	/* v5.0, backported to 4.19.y stable */
#include <net/erspan.h>
#endif

#include <net/icmp.h>
#include <net/route.h>
#include <net/xfrm.h>

#include "eoip_gre.h"

/* handle protocols with a non-standard GRE header by ids that do not
 * overlap with possible standard GRE protocol versions (0x00 - 0x7f)
 */
#define GREPROTO_CNT \
	(GREPROTO_MAX + GREPROTO_NONSTD_MAX - GREPROTO_NONSTD_BASE)

static const struct gre_protocol __rcu *gre_proto[GREPROTO_CNT] __read_mostly;

static int gre_proto_slot(u8 version)
{
	if (version < GREPROTO_MAX)
		return version;
	if (version >= GREPROTO_NONSTD_BASE && version < GREPROTO_NONSTD_MAX)
		return GREPROTO_MAX + version - GREPROTO_NONSTD_BASE;
	return -1;
}

int gre_add_protocol(const struct gre_protocol *proto, u8 version)
{
	int slot = gre_proto_slot(version);

	if (slot < 0)
		return -EINVAL;

	return (cmpxchg((const struct gre_protocol **)&gre_proto[slot], NULL, proto) == NULL) ?
		0 : -EBUSY;
}
EXPORT_SYMBOL_GPL(gre_add_protocol);

int gre_del_protocol(const struct gre_protocol *proto, u8 version)
{
	int slot = gre_proto_slot(version);
	int ret;

	if (slot < 0)
		return -EINVAL;

	ret = (cmpxchg((const struct gre_protocol **)&gre_proto[slot], proto, NULL) == proto) ?
		0 : -EBUSY;

	if (ret)
		return ret;

	synchronize_rcu();
	return 0;
}
EXPORT_SYMBOL_GPL(gre_del_protocol);

static bool eoip_hdr_present(const u8 *p)
{
	return p[0] == 0x20 && p[1] == 0x01 && p[2] == 0x64 && p[3] == 0x00;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 11, 0) && \
	LINUX_VERSION_CODE < KERNEL_VERSION(4, 3, 0)
/*
 * The gre_cisco_* dispatch layer and gre_build_header() lived in the
 * demultiplexer between v3.11 and v4.2 and are used by ip_gre/ip6_gre
 * of those kernels.
 */
static struct gre_cisco_protocol __rcu *gre_cisco_proto_list[GRE_IP_PROTO_MAX];

void gre_build_header(struct sk_buff *skb, const struct tnl_ptk_info *tpi,
		      int hdr_len)
{
	struct gre_base_hdr *greh;

	skb_push(skb, hdr_len);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 16, 0)
	skb_reset_transport_header(skb);
#endif
	greh = (struct gre_base_hdr *)skb->data;
	greh->flags = tnl_flags_to_gre_flags(tpi->flags);
	greh->protocol = tpi->proto;

	if (tpi->flags&(TUNNEL_KEY|TUNNEL_CSUM|TUNNEL_SEQ)) {
		__be32 *ptr = (__be32 *)(((u8 *)greh) + hdr_len - 4);

		if (tpi->flags&TUNNEL_SEQ) {
			*ptr = tpi->seq;
			ptr--;
		}
		if (tpi->flags&TUNNEL_KEY) {
			*ptr = tpi->key;
			ptr--;
		}
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 16, 0)
		if (tpi->flags&TUNNEL_CSUM &&
		    !(skb_shinfo(skb)->gso_type &
		      (SKB_GSO_GRE|SKB_GSO_GRE_CSUM))) {
#else
		if (tpi->flags&TUNNEL_CSUM &&
		    !(skb_shinfo(skb)->gso_type & SKB_GSO_GRE)) {
#endif
			*ptr = 0;
			*(__sum16 *)ptr = csum_fold(skb_checksum(skb, 0,
								 skb->len, 0));
		}
	}
}
EXPORT_SYMBOL_GPL(gre_build_header);

#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 16, 0)
/* replaced by skb_checksum_simple_validate() in v3.16 */
static __sum16 check_checksum(struct sk_buff *skb)
{
	__sum16 csum = 0;

	switch (skb->ip_summed) {
	case CHECKSUM_COMPLETE:
		csum = csum_fold(skb->csum);

		if (!csum)
			break;
		/* Fall through. */

	case CHECKSUM_NONE:
		skb->csum = 0;
		csum = __skb_checksum_complete(skb);
		skb->ip_summed = CHECKSUM_COMPLETE;
		break;
	}

	return csum;
}
#endif

static int parse_gre_header(struct sk_buff *skb, struct tnl_ptk_info *tpi,
			    bool *csum_err)
{
	unsigned int ip_hlen = ip_hdrlen(skb);
	const struct gre_base_hdr *greh;
	__be32 *options;
	int hdr_len;

	if (unlikely(!pskb_may_pull(skb, sizeof(struct gre_base_hdr))))
		return -EINVAL;

	greh = (struct gre_base_hdr *)(skb_network_header(skb) + ip_hlen);
	if (unlikely(greh->flags & (GRE_VERSION | GRE_ROUTING)))
		return -EINVAL;

	tpi->flags = gre_flags_to_tnl_flags(greh->flags);
	hdr_len = ip_gre_calc_hlen(tpi->flags);

	if (!pskb_may_pull(skb, hdr_len))
		return -EINVAL;

	greh = (struct gre_base_hdr *)(skb_network_header(skb) + ip_hlen);
	tpi->proto = greh->protocol;

	options = (__be32 *)(greh + 1);
	if (greh->flags & GRE_CSUM) {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 16, 0)
		if (skb_checksum_simple_validate(skb)) {
#else
		if (check_checksum(skb)) {
#endif
			*csum_err = true;
			return -EINVAL;
		}
		options++;
	}

	if (greh->flags & GRE_KEY) {
		tpi->key = *options;
		options++;
	} else
		tpi->key = 0;

	if (unlikely(greh->flags & GRE_SEQ)) {
		tpi->seq = *options;
		options++;
	} else
		tpi->seq = 0;

	/* WCCP version 1 and 2 protocol decoding.
	 * - Change protocol to IP
	 * - When dealing with WCCPv2, Skip extra 4 bytes in GRE header
	 */
	if (greh->flags == 0 && tpi->proto == htons(ETH_P_WCCP)) {
		tpi->proto = htons(ETH_P_IP);
		if ((*(u8 *)options & 0xF0) != 0x40) {
			hdr_len += 4;
			if (!pskb_may_pull(skb, hdr_len))
				return -EINVAL;
		}
	}

	return iptunnel_pull_header(skb, hdr_len, tpi->proto);
}

static int gre_cisco_rcv(struct sk_buff *skb)
{
	struct tnl_ptk_info tpi;
	int i;
	bool csum_err = false;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 16, 0)
#ifdef CONFIG_NET_IPGRE_BROADCAST
	if (ipv4_is_multicast(ip_hdr(skb)->daddr)) {
		/* Looped back packet, drop it! */
		if (rt_is_output_route(skb_rtable(skb)))
			goto drop;
	}
#endif
#endif

	if (parse_gre_header(skb, &tpi, &csum_err) < 0)
		goto drop;

	rcu_read_lock();
	for (i = 0; i < GRE_IP_PROTO_MAX; i++) {
		struct gre_cisco_protocol *proto;
		int ret;

		proto = rcu_dereference(gre_cisco_proto_list[i]);
		if (!proto)
			continue;
		ret = proto->handler(skb, &tpi);
		if (ret == PACKET_RCVD) {
			rcu_read_unlock();
			return 0;
		}
	}
	rcu_read_unlock();

	icmp_send(skb, ICMP_DEST_UNREACH, ICMP_PORT_UNREACH, 0);
drop:
	kfree_skb(skb);
	return 0;
}

static void gre_cisco_err(struct sk_buff *skb, u32 info)
{
	/* All the routers (except for Linux) return only
	 * 8 bytes of packet payload. It means, that precise relaying of
	 * ICMP in the real Internet is absolutely infeasible.
	 *
	 * Moreover, Cisco "wise men" put GRE key to the third word
	 * in GRE header. It makes impossible maintaining even soft
	 * state for keyed
	 * GRE tunnels with enabled checksum. Tell them "thank you".
	 *
	 * Well, I wonder, rfc1812 was written by Cisco employee,
	 * what the hell these idiots break standards established
	 * by themselves???
	 */

	const int type = icmp_hdr(skb)->type;
	const int code = icmp_hdr(skb)->code;
	struct tnl_ptk_info tpi;
	bool csum_err = false;
	int i;

	if (parse_gre_header(skb, &tpi, &csum_err)) {
		if (!csum_err)		/* ignore csum errors. */
			return;
	}

	if (type == ICMP_DEST_UNREACH && code == ICMP_FRAG_NEEDED) {
		ipv4_update_pmtu(skb, dev_net(skb->dev), info,
				skb->dev->ifindex, 0, IPPROTO_GRE, 0);
		return;
	}
	if (type == ICMP_REDIRECT) {
		ipv4_redirect(skb, dev_net(skb->dev), skb->dev->ifindex, 0,
				IPPROTO_GRE, 0);
		return;
	}

	rcu_read_lock();
	for (i = 0; i < GRE_IP_PROTO_MAX; i++) {
		struct gre_cisco_protocol *proto;

		proto = rcu_dereference(gre_cisco_proto_list[i]);
		if (!proto)
			continue;

		if (proto->err_handler(skb, info, &tpi) == PACKET_RCVD)
			goto out;

	}
out:
	rcu_read_unlock();
}

int gre_cisco_register(struct gre_cisco_protocol *newp)
{
	struct gre_cisco_protocol **proto = (struct gre_cisco_protocol **)
					    &gre_cisco_proto_list[newp->priority];

	return (cmpxchg(proto, NULL, newp) == NULL) ? 0 : -EBUSY;
}
EXPORT_SYMBOL_GPL(gre_cisco_register);

int gre_cisco_unregister(struct gre_cisco_protocol *del_proto)
{
	struct gre_cisco_protocol **proto = (struct gre_cisco_protocol **)
					    &gre_cisco_proto_list[del_proto->priority];
	int ret;

	ret = (cmpxchg(proto, del_proto, NULL) == del_proto) ? 0 : -EINVAL;

	if (ret)
		return ret;

	synchronize_net();
	return 0;
}
EXPORT_SYMBOL_GPL(gre_cisco_unregister);

static const struct gre_protocol ipgre_protocol = {
	.handler     = gre_cisco_rcv,
	.err_handler = gre_cisco_err,
};
#endif /* v3.11 .. v4.2 gre_cisco era */

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 7, 0)
/* Fills in tpi and returns header length to be pulled.
 * Note that caller must use pskb_may_pull() before pulling GRE header.
 */
int gre_parse_header(struct sk_buff *skb, struct tnl_ptk_info *tpi,
		     bool *csum_err, __be16 proto, int nhs)
{
	const struct gre_base_hdr *greh;
	__be32 *options;
	int hdr_len;

	if (unlikely(!pskb_may_pull(skb, nhs + sizeof(struct gre_base_hdr))))
		return -EINVAL;

	greh = (struct gre_base_hdr *)(skb->data + nhs);
	if (unlikely(greh->flags & (GRE_VERSION | GRE_ROUTING)))
		return -EINVAL;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 10, 0)
	gre_flags_to_tnl_flags(tpi->flags, greh->flags);
#else
	tpi->flags = gre_flags_to_tnl_flags(greh->flags);
#endif
	hdr_len = gre_calc_hlen(tpi->flags);

	if (!pskb_may_pull(skb, nhs + hdr_len))
		return -EINVAL;

	greh = (struct gre_base_hdr *)(skb->data + nhs);
	tpi->proto = greh->protocol;

	options = (__be32 *)(greh + 1);
	if (greh->flags & GRE_CSUM) {
		if (!skb_checksum_simple_validate(skb)) {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 3, 0)
			skb_checksum_try_convert(skb, IPPROTO_GRE,
						 null_compute_pseudo);
#else
			skb_checksum_try_convert(skb, IPPROTO_GRE, 0,
						 null_compute_pseudo);
#endif
		} else if (csum_err) {
			*csum_err = true;
			return -EINVAL;
		}

		options++;
	}

	if (greh->flags & GRE_KEY) {
		tpi->key = *options;
		options++;
	} else {
		tpi->key = 0;
	}
	if (unlikely(greh->flags & GRE_SEQ)) {
		tpi->seq = *options;
		options++;
	} else {
		tpi->seq = 0;
	}
	/* WCCP version 1 and 2 protocol decoding.
	 * - Change protocol to IPv4/IPv6
	 * - When dealing with WCCPv2, Skip extra 4 bytes in GRE header
	 *
	 * The bounds-safe skb_header_pointer() form (mainline v5.6) is
	 * used for all kernel versions.
	 */
	if (greh->flags == 0 && tpi->proto == htons(ETH_P_WCCP)) {
		u8 _val, *val;

		val = skb_header_pointer(skb, nhs + hdr_len,
					 sizeof(_val), &_val);
		if (!val)
			return -EINVAL;
		tpi->proto = proto;
		if ((*val & 0xF0) != 0x40)
			hdr_len += 4;
	}
	tpi->hdr_len = hdr_len;

#ifdef EOIP_HAVE_ERSPAN_LOOKUP
	/* ERSPAN ver 1 and 2 protocol sets GRE key field
	 * to 0 and sets the configured key in the
	 * inner erspan header field
	 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 11, 0)
	if ((greh->protocol == htons(ETH_P_ERSPAN) && hdr_len != 4) ||
	    greh->protocol == htons(ETH_P_ERSPAN2)) {
#else
	if (greh->protocol == htons(ETH_P_ERSPAN) ||
	    greh->protocol == htons(ETH_P_ERSPAN2)) {
#endif
		struct erspan_base_hdr *ershdr;

		if (!pskb_may_pull(skb, nhs + hdr_len + sizeof(*ershdr)))
			return -EINVAL;

		ershdr = (struct erspan_base_hdr *)(skb->data + nhs + hdr_len);
		tpi->key = cpu_to_be32(get_session_id(ershdr));
	}
#endif

	return hdr_len;
}
EXPORT_SYMBOL(gre_parse_header);
#endif /* >= v4.7 */

static int gre_rcv(struct sk_buff *skb)
{
	const struct gre_protocol *proto;
	u8 ver;
	int ret;

	/* the EoIP pseudo-GRE header is 8 octets with a fixed magic and
	 * may carry an empty frame (keepalive), so it is recognized
	 * before the standard 12 octet check
	 */
	if (pskb_may_pull(skb, EOIP_HDR_LEN) && eoip_hdr_present(skb->data))
		ver = GREPROTO_MAX + GREPROTO_NONSTD_EOIP - GREPROTO_NONSTD_BASE;
	else {
		if (!pskb_may_pull(skb, 12))
			goto drop;

		ver = skb->data[1]&0x7f;
		if (ver >= GREPROTO_MAX)
			goto drop;
	}

	rcu_read_lock();
	proto = rcu_dereference(gre_proto[ver]);
	if (!proto || !proto->handler)
		goto drop_unlock;
	ret = proto->handler(skb);
	rcu_read_unlock();
	return ret;

drop_unlock:
	rcu_read_unlock();
drop:
	kfree_skb(skb);
	return NET_RX_DROP;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 0, 0)
static int gre_err(struct sk_buff *skb, u32 info)
#else
static void gre_err(struct sk_buff *skb, u32 info)
#endif
{
	const struct gre_protocol *proto;
	const struct iphdr *iph = (const struct iphdr *)skb->data;
	u8 ver;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 0, 0)
	int err = 0;
#define GRE_ERR_RET(x) return (x)
#else
#define GRE_ERR_RET(x) return
#endif

	/* ICMP returns at least 8 octets past the inner IP header,
	 * exactly the size of the EoIP pseudo-GRE header
	 */
	if (eoip_hdr_present(skb->data + (iph->ihl<<2)))
		ver = GREPROTO_MAX + GREPROTO_NONSTD_EOIP - GREPROTO_NONSTD_BASE;
	else {
		ver = skb->data[(iph->ihl<<2) + 1]&0x7f;
		if (ver >= GREPROTO_MAX)
			GRE_ERR_RET(-EINVAL);
	}

	rcu_read_lock();
	proto = rcu_dereference(gre_proto[ver]);
	if (proto && proto->err_handler)
		proto->err_handler(skb, info);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 0, 0)
	else
		err = -EPROTONOSUPPORT;
#endif
	rcu_read_unlock();

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 0, 0)
	return err;
#endif
#undef GRE_ERR_RET
}

static const struct net_protocol net_gre_protocol = {
	.handler     = gre_rcv,
	.err_handler = gre_err,
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 14, 0)
	.netns_ok    = 1,
#endif
};

static int __init gre_init(void)
{
	pr_info("GRE over IPv4 demultiplexer driver (EoIP extended)\n");

	if (inet_add_protocol(&net_gre_protocol, IPPROTO_GRE) < 0) {
		pr_err("can't add protocol\n");
		return -EAGAIN;
	}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 11, 0) && \
	LINUX_VERSION_CODE < KERNEL_VERSION(4, 3, 0)
	if (gre_add_protocol(&ipgre_protocol, GREPROTO_CISCO) < 0) {
		pr_info("can't add ipgre protocol\n");
		inet_del_protocol(&net_gre_protocol, IPPROTO_GRE);
		return -EAGAIN;
	}
#endif

	return 0;
}

static void __exit gre_exit(void)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 11, 0) && \
	LINUX_VERSION_CODE < KERNEL_VERSION(4, 3, 0)
	gre_del_protocol(&ipgre_protocol, GREPROTO_CISCO);
#endif
	inet_del_protocol(&net_gre_protocol, IPPROTO_GRE);
}

module_init(gre_init);
module_exit(gre_exit);

MODULE_DESCRIPTION("GRE over IPv4 demultiplexer driver");
MODULE_AUTHOR("D. Kozlov <xeb@mail.ru>");
MODULE_LICENSE("GPL");
