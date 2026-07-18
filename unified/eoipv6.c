/*
 *	Linux NET3:	EoIPv6 (Ethernet over IPv6, MikroTik compatible) tunnel driver.
 *
 *	Authors: Boian Bonev (bbonev@ipacct.com)
 *
 *	This program is free software; you can redistribute it and/or
 *	modify it under the terms of the GNU General Public License
 *	as published by the Free Software Foundation; either version
 *	2 of the License, or (at your option) any later version.
 *
 *	The protocol (verified against live RouterOS captures): IPv6 next
 *	header 97 (the EtherIP number of RFC 3378, with a layout that
 *	violates the RFC), a 2 byte header carrying a 12 bit tunnel id and
 *	the version nibble 0x3 in the LOW nibble of the first octet, then
 *	the raw Ethernet frame.  There is no frame length field; frames
 *	are not padded to the Ethernet minimum; tunnels are demultiplexed
 *	by (source address, tunnel id); RouterOS sends hop limit 255,
 *	traffic class 0 and flow label 0.  See eoip_gre.h and the README
 *	protocol spec for the wire diagram.
 *
 *	Unlike the IPv4 EoIP pair (eoip.ko + replacement gre.ko), this
 *	module is fully standalone: protocol 97 has no in-tree handler.
 *
 *	This is a unified source that builds on all supported kernels
 *	(3.2 .. 7.0+), written against the current kernel API with compat
 *	fences below; version boundaries verified against mainline
 *	release tags.
 */

#include <linux/version.h>

#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 2, 0)
#error "kernels older than 3.2 are not supported"
#endif

#include <linux/capability.h>
#include <linux/module.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/skbuff.h>
#include <linux/netdevice.h>
#include <linux/in6.h>
#include <linux/if_arp.h>
#include <linux/if_tunnel.h>
#include <linux/init.h>
#include <linux/etherdevice.h>
#include <linux/if_ether.h>
#include <linux/icmpv6.h>
#include <linux/u64_stats_sync.h>

#include <net/protocol.h>
#include <net/ipv6.h>
#include <net/ip6_route.h>
#include <net/addrconf.h>
#include <net/xfrm.h>
#include <net/net_namespace.h>
#include <net/netns/generic.h>
#include <net/rtnetlink.h>
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 10, 0)
#include <net/ip_tunnels.h>
#endif

#include "../eoip_version.h"
#include "eoip_gre.h"
#include "eoip_keepalive.h"

#if !IS_ENABLED(CONFIG_IPV6)
#error "eoipv6 requires CONFIG_IPV6"
#endif

/*
 * Backward compatibility shims (same set as eoip.c where applicable).
 */

#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 14, 0)
/* see the twin definition in eoip.c; the struct tag must be pcpu_tstats
 * to complete the forward declaration in old struct net_device
 */
#ifndef _EOIP_PCPU_TSTATS
#define _EOIP_PCPU_TSTATS
struct pcpu_tstats {
	u64			rx_packets;
	u64			rx_bytes;
	u64			tx_packets;
	u64			tx_bytes;
	struct u64_stats_sync	syncp;
};
#endif
#define pcpu_sw_netstats pcpu_tstats
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 13, 0)
#define u64_stats_init(syncp) do { } while (0)	/* zero init suffices */
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 16, 0)
#define ignore_df local_df	/* v3.16 renamed local_df */
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 3, 0)
#define strscpy strlcpy		/* v4.3 */
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 4, 0)
#define nf_reset_ct nf_reset	/* v5.4 */
#endif

#ifndef IPTUNNEL_ERR_TIMEO	/* lives in net/ipip.h before v3.10 */
#define IPTUNNEL_ERR_TIMEO	(30*HZ)
#endif

#ifndef ETH_MAX_MTU		/* v4.10 */
#define ETH_MAX_MTU	0xFFFFU
#endif

#ifndef DEV_STATS_INC		/* v6.1, plain increment before */
#define DEV_STATS_INC(dev, field) ((dev)->stats.field++)
#endif

#ifndef IPPROTO_ETHERIP
#define IPPROTO_ETHERIP	97	/* RFC 3378 number; absent from uapi in6.h */
#endif

/* all wire format knowledge lives in these two helpers */
static inline void eoip6_build_hdr(u8 *h, u32 tid)
{
	h[0] = (((tid >> 8) & 0xF) << 4) | EOIP6_VERSION;
	h[1] = tid & 0xFF;
}

static inline int eoip6_parse_hdr(const u8 *h, u32 *tid)
{
	if ((h[0] & 0x0F) != EOIP6_VERSION)
		return -1;
	*tid = ((u32)(h[0] >> 4) << 8) | h[1];
	return 0;
}

struct eoip6_parm {
	char		name[IFNAMSIZ];
	int		link;
	u32		tid;		/* 0..4095, host order, on the wire */
	u8		hop_limit;	/* 0 = RouterOS default 255 */
	u8		tclass;
	struct in6_addr	laddr;
	struct in6_addr	raddr;
};

struct eoip6_tunnel {
	struct eoip6_tunnel __rcu *next;
	struct net_device	*dev;
	struct eoip6_parm	parms;
	unsigned long		err_time;	/* time of last ICMP error */
	int			err_count;	/* number of ICMP errors seen */
	int			hlen;		/* outer ipv6hdr + EoIPv6 header */
	struct eoip_keepalive	ka;
};

#define eoip6_for_each_tunnel_rcu(t, start) \
	for (t = rcu_dereference(start); t; t = rcu_dereference(t->next))

static struct rtnl_link_ops eoip6_ops __read_mostly;
static int eoip6_tunnel_bind_dev(struct net_device *dev);

#define EOIP6_HASH_SIZE 16
#define HASH(tid) (((tid)^((tid)>>4))&0xF)

static int eoip6_net_id __read_mostly;
struct eoip6_net {
	struct eoip6_tunnel __rcu *tunnels[EOIP6_HASH_SIZE];
};

/*
 * Locking : hash tables are protected by RCU and RTNL
 */

/* RouterOS demultiplexes on (source address, tunnel id); an exactly
 * matching local address is preferred over a wildcard one
 */
static struct eoip6_tunnel *eoip6_tunnel_lookup(struct net_device *dev,
						const struct in6_addr *remote,
						const struct in6_addr *local,
						u32 tid)
{
	struct net *net = dev_net(dev);
	int link = dev->ifindex;
	unsigned int h0 = HASH(tid);
	struct eoip6_tunnel *t, *cand = NULL;
	struct eoip6_net *ign = net_generic(net, eoip6_net_id);
	int score, cand_score = 8;

	eoip6_for_each_tunnel_rcu(t, ign->tunnels[h0]) {
		if (tid != t->parms.tid ||
				!ipv6_addr_equal(remote, &t->parms.raddr) ||
				!(t->dev->flags & IFF_UP))
			continue;

		score = 0;
		if (!ipv6_addr_equal(local, &t->parms.laddr)) {
			if (!ipv6_addr_any(&t->parms.laddr))
				continue;	/* bound to another local address */
			score |= 4;
		}
		if (t->parms.link != link)
			score |= 1;
		if (t->dev->type != ARPHRD_ETHER)
			score |= 2;
		if (score == 0)
			return t;

		if (score < cand_score) {
			cand = t;
			cand_score = score;
		}
	}

	return cand;
}

static struct eoip6_tunnel __rcu **eoip6_bucket(struct eoip6_net *ign,
		struct eoip6_parm *parms)
{
	return &ign->tunnels[HASH(parms->tid)];
}

static void eoip6_tunnel_link(struct eoip6_net *ign, struct eoip6_tunnel *t)
{
	struct eoip6_tunnel __rcu **tp = eoip6_bucket(ign, &t->parms);

	rcu_assign_pointer(t->next, rtnl_dereference(*tp));
	rcu_assign_pointer(*tp, t);
}

static void eoip6_tunnel_unlink(struct eoip6_net *ign, struct eoip6_tunnel *t)
{
	struct eoip6_tunnel __rcu **tp;
	struct eoip6_tunnel *iter;

	for (tp = eoip6_bucket(ign, &t->parms);
			(iter = rtnl_dereference(*tp)) != NULL;
			tp = &iter->next) {
		if (t == iter) {
			rcu_assign_pointer(*tp, t->next);
			break;
		}
	}
}

static struct eoip6_tunnel *eoip6_tunnel_find(struct net *net,
						struct eoip6_parm *parms,
						int type)
{
	struct eoip6_tunnel *t;
	struct eoip6_tunnel __rcu **tp;
	struct eoip6_net *ign = net_generic(net, eoip6_net_id);

	for (tp = eoip6_bucket(ign, parms);
			(t = rtnl_dereference(*tp)) != NULL;
			tp = &t->next)
		if (ipv6_addr_equal(&parms->laddr, &t->parms.laddr) &&
			ipv6_addr_equal(&parms->raddr, &t->parms.raddr) &&
			parms->tid == t->parms.tid &&
			parms->link == t->parms.link &&
			type == t->dev->type)
			break;

	return t;
}

static void eoip6_if_uninit(struct net_device *dev)
{
	struct net *net = dev_net(dev);
	struct eoip6_net *ign = net_generic(net, eoip6_net_id);

	eoip6_tunnel_unlink(ign, netdev_priv(dev));
	dev_put(dev);
}

static int __eoip6_err(struct sk_buff *skb, struct inet6_skb_parm *opt,
			u8 type, u8 code, int offset, __be32 info)
{
	/* skb->data points at the quoted (offending) IPv6 header we sent;
	 * offset is where our 2 byte header sits within it
	 */
	const struct ipv6hdr *ip6h = (const struct ipv6hdr *)skb->data;
	struct eoip6_tunnel *t;
	u32 tid;

	switch (type) {
	case ICMPV6_DEST_UNREACH:
	case ICMPV6_TIME_EXCEED:
		break;
	default:
		/* peers without EoIPv6 support answer every packet with
		 * parameter problem (unrecognized next header) - that and
		 * everything else is deliberately ignored
		 */
		return 0;
	}

	if (!pskb_may_pull(skb, offset + EOIP6_HDR_LEN))
		return -ENOENT;
	ip6h = (const struct ipv6hdr *)skb->data;
	if (eoip6_parse_hdr(skb->data + offset, &tid))
		return -ENOENT;

	rcu_read_lock();
	/* the quoted saddr is our local address, its daddr our remote */
	t = eoip6_tunnel_lookup(skb->dev, &ip6h->daddr, &ip6h->saddr, tid);
	if (t) {
		if (time_before(jiffies, t->err_time + IPTUNNEL_ERR_TIMEO))
			t->err_count++;
		else
			t->err_count = 1;
		t->err_time = jiffies;
	}
	rcu_read_unlock();

	return 0;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 0, 0)
/* v5.0 changed the err_handler return type from void to int */
static int eoip6_err(struct sk_buff *skb, struct inet6_skb_parm *opt,
			u8 type, u8 code, int offset, __be32 info)
{
	return __eoip6_err(skb, opt, type, code, offset, info);
}
#else
static void eoip6_err(struct sk_buff *skb, struct inet6_skb_parm *opt,
			u8 type, u8 code, int offset, __be32 info)
{
	__eoip6_err(skb, opt, type, code, offset, info);
}
#endif

static void eoip6_rx_stats(struct net_device *dev, unsigned int len)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 10, 0)
	dev_sw_netstats_rx_add(dev, len);
#else
	struct pcpu_sw_netstats *tstats = this_cpu_ptr(dev->tstats);

	u64_stats_update_begin(&tstats->syncp);
	tstats->rx_packets++;
	tstats->rx_bytes += len;
	u64_stats_update_end(&tstats->syncp);
#endif
}

static void eoip6_tx_stats(struct net_device *dev, int pkt_len)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 11, 0)
	iptunnel_xmit_stats(dev, pkt_len);
#else
	if (pkt_len > 0) {
		struct pcpu_sw_netstats *tstats = this_cpu_ptr(dev->tstats);

		u64_stats_update_begin(&tstats->syncp);
		tstats->tx_bytes += pkt_len;
		tstats->tx_packets++;
		u64_stats_update_end(&tstats->syncp);
	} else {
		DEV_STATS_INC(dev, tx_errors);
		DEV_STATS_INC(dev, tx_aborted_errors);
	}
#endif
}

static int eoip6_rcv(struct sk_buff *skb)
{
	const struct ipv6hdr *ip6h;
	struct eoip6_tunnel *tunnel;
	u32 tid;
	u8 *h;

	/* outer fragments are already reassembled by the IPv6 core */
	if (!pskb_may_pull(skb, EOIP6_HDR_LEN))
		goto drop_nolock;

	ip6h = ipv6_hdr(skb);
	h = skb->data;

	if (eoip6_parse_hdr(h, &tid))
		goto drop_nolock;

	rcu_read_lock();

	tunnel = eoip6_tunnel_lookup(skb->dev, &ip6h->saddr, &ip6h->daddr, tid);
	if (tunnel) {
		if (skb->len == EOIP6_HDR_LEN) {
			/* MikroTik keepalive: a bare header, no frame */
			eoip_ka_recv(&tunnel->ka);
			rcu_read_unlock();
			consume_skb(skb);
			return 0;
		}

		if (!pskb_may_pull(skb, EOIP6_HDR_LEN + ETH_HLEN)) {
			DEV_STATS_INC(tunnel->dev, rx_length_errors);
			DEV_STATS_INC(tunnel->dev, rx_errors);
			goto drop;
		}
		h = skb->data;	/* pskb_may_pull may reallocate the head */

		secpath_reset(skb);

		pskb_pull(skb, EOIP6_HDR_LEN);
		skb_postpull_rcsum(skb, h, EOIP6_HDR_LEN);
		skb->pkt_type = PACKET_HOST;

		/* there is no length field to trim to (cf. the v4 module):
		 * the frame is exactly the remaining payload and frames
		 * are not padded on the wire
		 */
		skb->protocol = eth_type_trans(skb, tunnel->dev);
		skb_postpull_rcsum(skb, eth_hdr(skb), ETH_HLEN);

		eoip6_rx_stats(tunnel->dev, skb->len);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 12, 0)
		__skb_tunnel_rx(skb, tunnel->dev, dev_net(tunnel->dev));
#else
		__skb_tunnel_rx(skb, tunnel->dev);
#endif

		skb_reset_network_header(skb);

		netif_rx(skb);

		rcu_read_unlock();
		return 0;
	}
	icmpv6_send(skb, ICMPV6_DEST_UNREACH, ICMPV6_PORT_UNREACH, 0);

drop:
	rcu_read_unlock();
drop_nolock:
	kfree_skb(skb);
	return 0;
}

/* Route to the peer, prepend the outer IPv6 + EoIPv6 header and hand the
 * skb to ip6_local_out().  Consumes the skb.  An empty skb produces a
 * keepalive; tx byte/packet counters are only accounted for data.
 */
static void eoip6_xmit_skb(struct eoip6_tunnel *tunnel, struct sk_buff *skb)
{
	struct net_device *dev = tunnel->dev;
	struct eoip6_parm *p = &tunnel->parms;
	struct flowi6 fl6;
	struct dst_entry *dst;
	struct net_device *tdev;	/* Device to other host */
	struct ipv6hdr *ip6h;		/* Our new IPv6 header */
	unsigned int max_headroom;	/* The extra header space needed */
	int hlen = tunnel->hlen;
	int err;
	int pkt_len;
	unsigned int frame_size = skb->len;

	if (frame_size > 0xFFFF - EOIP6_HDR_LEN) {
		DEV_STATS_INC(dev, tx_dropped);
		dev_kfree_skb(skb);
		return;
	}

	memset(&fl6, 0, sizeof(fl6));
	fl6.daddr = p->raddr;
	fl6.saddr = p->laddr;
	fl6.flowi6_oif = p->link;
	fl6.flowi6_proto = IPPROTO_ETHERIP;

	dst = ip6_route_output(dev_net(dev), NULL, &fl6);
	if (dst->error) {
		dst_release(dst);
		DEV_STATS_INC(dev, tx_carrier_errors);
		goto tx_error;
	}
	tdev = dst->dev;

	if (tdev == dev) {
		dst_release(dst);
		DEV_STATS_INC(dev, collisions);
		goto tx_error;
	}

	if (ipv6_addr_any(&p->laddr) &&
			ipv6_dev_get_saddr(dev_net(dev), tdev, &fl6.daddr, 0,
				&fl6.saddr)) {
		dst_release(dst);
		DEV_STATS_INC(dev, tx_carrier_errors);
		goto tx_error;
	}

	max_headroom = LL_RESERVED_SPACE(tdev) + hlen + dst->header_len;

	if (skb_headroom(skb) < max_headroom || skb_shared(skb) ||
			(skb_cloned(skb) && !skb_clone_writable(skb, 0))) {
		struct sk_buff *new_skb;

		new_skb = skb_realloc_headroom(skb, max_headroom);
		if (!new_skb) {
			dst_release(dst);
			DEV_STATS_INC(dev, tx_dropped);
			dev_kfree_skb(skb);
			return;
		}
		if (skb->sk)
			skb_set_owner_w(new_skb, skb->sk);
		dev_kfree_skb(skb);
		skb = new_skb;
	}

	skb_reset_transport_header(skb);
	skb_push(skb, hlen);
	skb_reset_network_header(skb);
	memset(IP6CB(skb), 0, sizeof(struct inet6_skb_parm));
	skb->protocol = htons(ETH_P_IPV6);
	skb_dst_drop(skb);
	skb_dst_set(skb, dst);

	/*
	 *	Push down and install the EoIPv6 header.
	 */

	ip6h = ipv6_hdr(skb);
	*(__be32 *)ip6h = htonl(0x60000000 | ((u32)p->tclass << 20));
	ip6h->payload_len = htons(frame_size + EOIP6_HDR_LEN);
	ip6h->nexthdr = IPPROTO_ETHERIP;
	/* RouterOS sends hop limit 255 (observed on live captures) */
	ip6h->hop_limit = p->hop_limit ? p->hop_limit : 255;
	ip6h->saddr = fl6.saddr;
	ip6h->daddr = fl6.daddr;

	eoip6_build_hdr((u8 *)(ip6h + 1), p->tid);

	nf_reset_ct(skb);
	skb->ip_summed = CHECKSUM_NONE;
	/* IPv6 has no in-network fragmentation; let the local stack emit
	 * fragment extension headers for oversized frames
	 */
	skb->ignore_df = 1;

	pkt_len = skb->len - skb_transport_offset(skb);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 4, 0)
	err = ip6_local_out(dev_net(dev), NULL, skb);
#else
	err = ip6_local_out(skb);
#endif
	if (likely(net_xmit_eval(err) == 0)) {
		if (frame_size)
			eoip6_tx_stats(dev, pkt_len);
	} else if (frame_size)
		eoip6_tx_stats(dev, -1);

	return;

tx_error:
	DEV_STATS_INC(dev, tx_errors);
	dev_kfree_skb(skb);
}

static netdev_tx_t eoip6_if_xmit(struct sk_buff *skb, struct net_device *dev)
{
	struct eoip6_tunnel *tunnel = netdev_priv(dev);

	if (tunnel->err_count > 0) {
		if (time_before(jiffies,
				tunnel->err_time + IPTUNNEL_ERR_TIMEO)) {
			tunnel->err_count--;

			dst_link_failure(skb);
		} else
			tunnel->err_count = 0;
	}

	eoip6_xmit_skb(tunnel, skb);

	return NETDEV_TX_OK;
}

static void eoip6_ka_send(struct net_device *dev)
{
	struct eoip6_tunnel *tunnel = netdev_priv(dev);
	struct sk_buff *skb;
	unsigned int room = LL_MAX_HEADER + sizeof(struct ipv6hdr) + EOIP6_HDR_LEN;

	if (ipv6_addr_any(&tunnel->parms.raddr))
		return;

	skb = alloc_skb(room, GFP_ATOMIC);
	if (!skb)
		return;
	skb_reserve(skb, room);
	skb->dev = dev;
	skb->protocol = htons(ETH_P_IPV6);

	eoip6_xmit_skb(tunnel, skb);
}

static int eoip6_if_open(struct net_device *dev)
{
	struct eoip6_tunnel *tunnel = netdev_priv(dev);

	eoip_ka_open(&tunnel->ka);
	return 0;
}

static int eoip6_if_close(struct net_device *dev)
{
	struct eoip6_tunnel *tunnel = netdev_priv(dev);

	eoip_ka_stop(&tunnel->ka);
	return 0;
}

static int eoip6_tunnel_bind_dev(struct net_device *dev)
{
	struct net_device *tdev = NULL;
	struct eoip6_tunnel *tunnel = netdev_priv(dev);
	struct eoip6_parm *p = &tunnel->parms;
	int hlen = LL_MAX_HEADER;
	int mtu = ETH_DATA_LEN;
	int addend = sizeof(struct ipv6hdr) + EOIP6_HDR_LEN;

	/* Guess output device to choose reasonable mtu and needed_headroom */

	if (!ipv6_addr_any(&p->raddr)) {
		struct flowi6 fl6;
		struct dst_entry *dst;

		memset(&fl6, 0, sizeof(fl6));
		fl6.daddr = p->raddr;
		fl6.saddr = p->laddr;
		fl6.flowi6_oif = p->link;
		fl6.flowi6_proto = IPPROTO_ETHERIP;

		dst = ip6_route_output(dev_net(dev), NULL, &fl6);
		if (!dst->error)
			tdev = dst->dev;
		dst_release(dst);
	}

	if (!tdev && p->link)
		tdev = __dev_get_by_index(dev_net(dev), p->link);

	if (tdev) {
		hlen = tdev->hard_header_len + tdev->needed_headroom;
		mtu = tdev->mtu;
	}
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 1, 0)
	dev->iflink = p->link;
#endif

	dev->needed_headroom = addend + hlen;
	mtu -= dev->hard_header_len + addend;

	if (mtu < 68)
		mtu = 68;

	tunnel->hlen = addend;

	/* 1444 on a 1500 byte path, matching the RouterOS default */
	return mtu;
}

static int eoip6_if_change_mtu(struct net_device *dev, int new_mtu)
{
	struct eoip6_tunnel *tunnel = netdev_priv(dev);

	if (new_mtu < 68 ||
			new_mtu > 0xFFF8 - dev->hard_header_len - tunnel->hlen)
		return -EINVAL;
	dev->mtu = new_mtu;
	return 0;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 1, 0)
static int eoip6_get_iflink(const struct net_device *dev)
{
	const struct eoip6_tunnel *tunnel = netdev_priv(dev);

	return tunnel->parms.link;
}
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 7, 0)
/* since v6.7 the core allocates and frees dev->tstats
 * (dev->pcpu_stat_type == NETDEV_PCPU_STAT_TSTATS)
 */
static void eoip6_dev_free(struct net_device *dev)
{
	free_percpu(dev->tstats);
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 12, 0)
	free_netdev(dev);
#endif
}
#endif

static void eoip6_destroy_tunnels(struct eoip6_net *ign, struct list_head *head)
{
	int h;

	for (h = 0; h < EOIP6_HASH_SIZE; h++) {
		struct eoip6_tunnel *t;

		t = rtnl_dereference(ign->tunnels[h]);

		while (t != NULL) {
			unregister_netdevice_queue(t->dev, head);
			t = rtnl_dereference(t->next);
		}
	}
}

static int __net_init eoip6_init_net(struct net *net)
{
	return 0;
}

static void __net_exit eoip6_exit_net(struct net *net)
{
	struct eoip6_net *ign;
	LIST_HEAD(list);

	ign = net_generic(net, eoip6_net_id);
	rtnl_lock();
	eoip6_destroy_tunnels(ign, &list);
	unregister_netdevice_many(&list);
	rtnl_unlock();
}

static struct pernet_operations eoip6_net_ops = {
	.id = &eoip6_net_id,
	.init = eoip6_init_net,
	.exit = eoip6_exit_net,
	.size = sizeof(struct eoip6_net),
};

static int __eoip6_validate(struct nlattr *tb[], struct nlattr *data[])
{
	struct in6_addr daddr;

	if (tb[IFLA_ADDRESS]) {
		if (nla_len(tb[IFLA_ADDRESS]) != ETH_ALEN)
			return -EINVAL;
		if (!is_valid_ether_addr(nla_data(tb[IFLA_ADDRESS])))
			return -EADDRNOTAVAIL;
	}

	if (!data)
		return 0;

	if (data[IFLA_GRE_IKEY] &&
			nla_get_u32(data[IFLA_GRE_IKEY]) > EOIP6_TID_MAX)
		return -EINVAL;

	if (data[IFLA_GRE_REMOTE]) {
		nla_memcpy(&daddr, data[IFLA_GRE_REMOTE], sizeof(daddr));
		if (ipv6_addr_any(&daddr) || ipv6_addr_is_multicast(&daddr))
			return -EINVAL;
	}

	if (data[IFLA_EOIP_KA_INTERVAL] &&
			nla_get_u32(data[IFLA_EOIP_KA_INTERVAL]) > EOIP_KA_MAX_INTERVAL)
		return -EINVAL;

	return 0;
}

static void eoip6_netlink_parms(struct nlattr *data[],
				struct eoip6_parm *parms)
{
	memset(parms, 0, sizeof(*parms));

	if (!data)
		return;

	if (data[IFLA_GRE_LINK])
		parms->link = nla_get_u32(data[IFLA_GRE_LINK]);

	if (data[IFLA_GRE_IKEY])
		parms->tid = nla_get_u32(data[IFLA_GRE_IKEY]);

	if (data[IFLA_GRE_LOCAL])
		nla_memcpy(&parms->laddr, data[IFLA_GRE_LOCAL],
			sizeof(parms->laddr));

	if (data[IFLA_GRE_REMOTE])
		nla_memcpy(&parms->raddr, data[IFLA_GRE_REMOTE],
			sizeof(parms->raddr));

	if (data[IFLA_GRE_TTL])
		parms->hop_limit = nla_get_u8(data[IFLA_GRE_TTL]);

	if (data[IFLA_GRE_TOS])
		parms->tclass = nla_get_u8(data[IFLA_GRE_TOS]);
}

static int eoip6_if_init(struct net_device *dev)
{
	struct eoip6_tunnel *tunnel = netdev_priv(dev);

	tunnel->dev = dev;
	strscpy(tunnel->parms.name, dev->name, IFNAMSIZ);

	eoip6_tunnel_bind_dev(dev);
	eoip_ka_init(&tunnel->ka, dev, eoip6_ka_send);

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 7, 0)
	{
		int i;

		dev->tstats = alloc_percpu(struct pcpu_sw_netstats);
		if (!dev->tstats)
			return -ENOMEM;
		for_each_possible_cpu(i)
			u64_stats_init(&per_cpu_ptr(dev->tstats, i)->syncp);
	}
#endif

	/* balanced by dev_put() in ndo_uninit; taking the reference here
	 * keeps it balanced even when register_netdevice() fails midway
	 */
	dev_hold(dev);

	return 0;
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 11, 0)
/* .ndo_get_stats64 = dev_get_tstats64 replaces this since v5.11 */
static struct net_device_stats *eoip6_if_get_stats(struct net_device *dev)
{
	u64 rx_packets = 0, rx_bytes = 0;
	u64 tx_packets = 0, tx_bytes = 0;
	int i;

	for_each_possible_cpu(i) {
		const struct pcpu_sw_netstats *tstats =
			per_cpu_ptr(dev->tstats, i);
		u64 rxp, rxb, txp, txb;
		unsigned int start;

		do {
			start = u64_stats_fetch_begin(&tstats->syncp);
			rxp = tstats->rx_packets;
			rxb = tstats->rx_bytes;
			txp = tstats->tx_packets;
			txb = tstats->tx_bytes;
		} while (u64_stats_fetch_retry(&tstats->syncp, start));

		rx_packets += rxp;
		rx_bytes += rxb;
		tx_packets += txp;
		tx_bytes += txb;
	}
	dev->stats.rx_packets = rx_packets;
	dev->stats.rx_bytes = rx_bytes;
	dev->stats.tx_packets = tx_packets;
	dev->stats.tx_bytes = tx_bytes;
	return &dev->stats;
}
#endif

static const struct net_device_ops eoip6_netdev_ops = {
	.ndo_init = eoip6_if_init,
	.ndo_uninit = eoip6_if_uninit,
	.ndo_open = eoip6_if_open,
	.ndo_stop = eoip6_if_close,
	.ndo_start_xmit = eoip6_if_xmit,
	.ndo_set_mac_address = eth_mac_addr,
	.ndo_validate_addr = eth_validate_addr,
	.ndo_change_mtu = eoip6_if_change_mtu,
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 11, 0)
	.ndo_get_stats64 = dev_get_tstats64,
#else
	.ndo_get_stats = eoip6_if_get_stats,
#endif
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 1, 0)
	.ndo_get_iflink = eoip6_get_iflink,
#endif
};

static void eoip6_setup(struct net_device *dev)
{
	ether_setup(dev);

	dev->netdev_ops = &eoip6_netdev_ops;

	/* no queue on the virtual interface: transmission is synchronous
	 * into ip6_local_out() and queueing happens on the underlying
	 * physical interface
	 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 3, 0)
	dev->priv_flags |= IFF_NO_QUEUE;
#else
	dev->tx_queue_len = 0;	/* selects noqueue before v4.3 */
#endif

	/* lockless transmit, we do not generate output sequences */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 12, 0)
	dev->lltx = true;
#else
	dev->features |= NETIF_F_LLTX;
#endif

	/* the tunnel is bound to the netns it was created in */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 15, 0)
	dev->netns_immutable = true;
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(6, 12, 0)
	dev->netns_local = true;
#else
	dev->features |= NETIF_F_NETNS_LOCAL;
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 7, 0)
	dev->pcpu_stat_type = NETDEV_PCPU_STAT_TSTATS;
	dev->needs_free_netdev = true;
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(4, 12, 0)
	dev->needs_free_netdev = true;
	dev->priv_destructor = eoip6_dev_free;
#else
	dev->destructor = eoip6_dev_free;
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 10, 0)
	dev->max_mtu = ETH_MAX_MTU;
#endif
}

static int __eoip6_newlink(struct net_device *dev,
			struct nlattr *tb[], struct nlattr *data[])
{
	struct eoip6_tunnel *nt = netdev_priv(dev);
	struct net *net = dev_net(dev);
	struct eoip6_net *ign = net_generic(net, eoip6_net_id);
	int mtu;
	int err;

	eoip6_netlink_parms(data, &nt->parms);

	/* keepalive is off unless requested (RouterOS defaults to 10s,10) */
	if (data && data[IFLA_EOIP_KA_INTERVAL])
		nt->ka.interval = nla_get_u32(data[IFLA_EOIP_KA_INTERVAL]);
	if (data && data[IFLA_EOIP_KA_RETRIES])
		nt->ka.retries = nla_get_u8(data[IFLA_EOIP_KA_RETRIES]);

	if (eoip6_tunnel_find(net, &nt->parms, dev->type))
		return -EEXIST;

	if (!tb[IFLA_ADDRESS]) {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 15, 0)
		u8 addr[ETH_ALEN];

		eth_random_addr(addr);
		dev_addr_set(dev, addr);
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(3, 6, 0)
		eth_random_addr(dev->dev_addr);
#else
		random_ether_addr(dev->dev_addr);
#endif
	}

	mtu = eoip6_tunnel_bind_dev(dev);
	if (!tb[IFLA_MTU])
		dev->mtu = mtu;

	err = register_netdevice(dev);
	if (err)
		return err;

	eoip6_tunnel_link(ign, nt);

	return 0;
}

static int __eoip6_changelink(struct net_device *dev,
			struct nlattr *tb[], struct nlattr *data[])
{
	struct eoip6_tunnel *t, *nt = netdev_priv(dev);
	struct net *net = dev_net(dev);
	struct eoip6_net *ign = net_generic(net, eoip6_net_id);
	struct eoip6_parm p;

	eoip6_netlink_parms(data, &p);

	t = eoip6_tunnel_find(net, &p, dev->type);

	if (t) {
		if (t->dev != dev)
			return -EEXIST;
	} else {
		t = nt;

		eoip6_tunnel_unlink(ign, t);
		t->parms.laddr = p.laddr;
		t->parms.raddr = p.raddr;
		t->parms.tid = p.tid;
		eoip6_tunnel_link(ign, t);
		netdev_state_change(dev);
	}

	t->parms.hop_limit = p.hop_limit;
	t->parms.tclass = p.tclass;

	if (t->parms.link != p.link) {
		t->parms.link = p.link;
		eoip6_tunnel_bind_dev(dev);
		netdev_state_change(dev);
	}

	/* absent attributes leave the keepalive configuration unchanged so
	 * that a set request from an older tool does not disable it
	 */
	if (data && (data[IFLA_EOIP_KA_INTERVAL] || data[IFLA_EOIP_KA_RETRIES])) {
		u32 interval = data[IFLA_EOIP_KA_INTERVAL] ?
			nla_get_u32(data[IFLA_EOIP_KA_INTERVAL]) : t->ka.interval;
		u8 retries = data[IFLA_EOIP_KA_RETRIES] ?
			nla_get_u8(data[IFLA_EOIP_KA_RETRIES]) : t->ka.retries;

		if (interval != t->ka.interval || retries != t->ka.retries)
			eoip_ka_config(&t->ka, interval, retries);
	}

	return 0;
}

/*
 * rtnl_link_ops callback signatures changed in v4.13 (extack) and
 * v6.15 (rtnl_newlink_params); thin wrappers around the common code
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 15, 0)
static int eoip6_newlink(struct net_device *dev,
			struct rtnl_newlink_params *params,
			struct netlink_ext_ack *extack)
{
	return __eoip6_newlink(dev, params->tb, params->data);
}
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(4, 13, 0)
static int eoip6_newlink(struct net *src_net, struct net_device *dev,
			struct nlattr *tb[], struct nlattr *data[],
			struct netlink_ext_ack *extack)
{
	return __eoip6_newlink(dev, tb, data);
}
#else
static int eoip6_newlink(struct net *src_net, struct net_device *dev,
			struct nlattr *tb[], struct nlattr *data[])
{
	return __eoip6_newlink(dev, tb, data);
}
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 13, 0)
static int eoip6_changelink(struct net_device *dev, struct nlattr *tb[],
			struct nlattr *data[],
			struct netlink_ext_ack *extack)
{
	return __eoip6_changelink(dev, tb, data);
}

static int eoip6_validate(struct nlattr *tb[], struct nlattr *data[],
			struct netlink_ext_ack *extack)
{
	return __eoip6_validate(tb, data);
}
#else
static int eoip6_changelink(struct net_device *dev, struct nlattr *tb[],
			struct nlattr *data[])
{
	return __eoip6_changelink(dev, tb, data);
}

static int eoip6_validate(struct nlattr *tb[], struct nlattr *data[])
{
	return __eoip6_validate(tb, data);
}
#endif

static size_t eoip6_get_size(const struct net_device *dev)
{
	return
		/* IFLA_GRE_LINK */
		nla_total_size(4) +
		/* IFLA_GRE_IKEY */
		nla_total_size(4) +
		/* IFLA_GRE_LOCAL */
		nla_total_size(sizeof(struct in6_addr)) +
		/* IFLA_GRE_REMOTE */
		nla_total_size(sizeof(struct in6_addr)) +
		/* IFLA_GRE_TTL */
		nla_total_size(1) +
		/* IFLA_GRE_TOS */
		nla_total_size(1) +
		/* IFLA_EOIP_KA_INTERVAL */
		nla_total_size(4) +
		/* IFLA_EOIP_KA_RETRIES */
		nla_total_size(1) +
		0;
}

static int eoip6_fill_info(struct sk_buff *skb, const struct net_device *dev)
{
	struct eoip6_tunnel *t = netdev_priv(dev);
	struct eoip6_parm *p = &t->parms;

	/* plain 16 byte puts: nla_put_in6_addr only exists since v4.1 and
	 * is byte identical on the wire
	 */
	if (nla_put_u32(skb, IFLA_GRE_LINK, p->link) ||
		nla_put_u32(skb, IFLA_GRE_IKEY, p->tid) ||
		nla_put(skb, IFLA_GRE_LOCAL, sizeof(p->laddr), &p->laddr) ||
		nla_put(skb, IFLA_GRE_REMOTE, sizeof(p->raddr), &p->raddr) ||
		nla_put_u8(skb, IFLA_GRE_TTL, p->hop_limit) ||
		nla_put_u8(skb, IFLA_GRE_TOS, p->tclass) ||
		nla_put_u32(skb, IFLA_EOIP_KA_INTERVAL, t->ka.interval) ||
		nla_put_u8(skb, IFLA_EOIP_KA_RETRIES, t->ka.retries))
		goto nla_put_failure;

	return 0;

nla_put_failure:
	return -EMSGSIZE;
}

static const struct nla_policy eoip6_policy[IFLA_EOIP_ATTR_MAX + 1] = {
	[IFLA_GRE_LINK]		= { .type = NLA_U32 },
	[IFLA_GRE_IKEY]		= { .type = NLA_U32 },
	[IFLA_GRE_LOCAL]	= { .len = sizeof(struct in6_addr) },
	[IFLA_GRE_REMOTE]	= { .len = sizeof(struct in6_addr) },
	[IFLA_GRE_TTL]		= { .type = NLA_U8 },
	[IFLA_GRE_TOS]		= { .type = NLA_U8 },
	[IFLA_EOIP_KA_INTERVAL]	= { .type = NLA_U32 },
	[IFLA_EOIP_KA_RETRIES]	= { .type = NLA_U8 },
};

static struct rtnl_link_ops eoip6_ops __read_mostly = {
	.kind		= "eoipv6",
	.maxtype	= IFLA_EOIP_ATTR_MAX,
	.policy		= eoip6_policy,
	.priv_size	= sizeof(struct eoip6_tunnel),
	.setup		= eoip6_setup,
	.validate	= eoip6_validate,
	.newlink	= eoip6_newlink,
	.changelink	= eoip6_changelink,
	.get_size	= eoip6_get_size,
	.fill_info	= eoip6_fill_info,
};

static const struct inet6_protocol eoip6_protocol = {
	.handler	= eoip6_rcv,
	.err_handler	= eoip6_err,
	.flags		= INET6_PROTO_FINAL,
};

/*
 *	And now the modules code and kernel interface.
 */

static int __init eoip6_init(void)
{
	int err;

	pr_info("EoIPv6 tunneling driver v" EOIP_VERSION "\n");

	err = register_pernet_device(&eoip6_net_ops);
	if (err < 0)
		return err;

	err = inet6_add_protocol(&eoip6_protocol, IPPROTO_ETHERIP);
	if (err < 0) {
		pr_info("eoipv6 init: can't add protocol (proto %d already claimed)\n",
			IPPROTO_ETHERIP);
		goto add_proto_failed;
	}

	err = rtnl_link_register(&eoip6_ops);
	if (err < 0)
		goto eoip6_ops_failed;

out:
	return err;

eoip6_ops_failed:
	inet6_del_protocol(&eoip6_protocol, IPPROTO_ETHERIP);
add_proto_failed:
	unregister_pernet_device(&eoip6_net_ops);
	goto out;
}

static void __exit eoip6_fini(void)
{
	rtnl_link_unregister(&eoip6_ops);
	if (inet6_del_protocol(&eoip6_protocol, IPPROTO_ETHERIP) < 0)
		pr_info("eoipv6 close: can't remove protocol\n");
	unregister_pernet_device(&eoip6_net_ops);
}

module_init(eoip6_init);
module_exit(eoip6_fini);
MODULE_LICENSE("GPL");
MODULE_VERSION(EOIP_VERSION);
MODULE_DESCRIPTION("MikroTik compatible EoIPv6 (Ethernet over IPv6) tunnel driver");
MODULE_AUTHOR("Boian Bonev <bbonev@ipacct.com>");
MODULE_ALIAS_RTNL_LINK("eoipv6");
