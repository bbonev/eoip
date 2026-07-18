/*
 *	Linux NET3:	EoIP (Ethernet over IP, MikroTik compatible) tunnel driver.
 *
 *	Based on the code from ip_gre.c
 *
 *	Authors: Alexey Kuznetsov (kuznet@ms2.inr.ac.ru)
 *	Authors: Boian Bonev (bbonev@ipacct.com)
 *
 *	This program is free software; you can redistribute it and/or
 *	modify it under the terms of the GNU General Public License
 *	as published by the Free Software Foundation; either version
 *	2 of the License, or (at your option) any later version.
 *
 *	This is a unified source that builds on all supported kernels
 *	(3.2 .. 7.0+).  It is written against the current kernel API;
 *	older kernels are handled by the compat block below.  Version
 *	boundaries were verified against mainline release tags; where a
 *	distro stable tree is known to have backported an API change,
 *	the fence deviates from mainline and says so in a comment.
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
#include <linux/in.h>
#include <linux/if_arp.h>
#include <linux/if_tunnel.h>
#include <linux/init.h>
#include <linux/inetdevice.h>
#include <linux/netfilter_ipv4.h>
#include <linux/etherdevice.h>
#include <linux/if_ether.h>
#include <linux/u64_stats_sync.h>

#include <net/sock.h>
#include <net/ip.h>
#include <net/icmp.h>
#include <net/protocol.h>
#include <net/checksum.h>
#include <net/dsfield.h>
#include <net/xfrm.h>
#include <net/net_namespace.h>
#include <net/netns/generic.h>
#include <net/rtnetlink.h>
#include <net/route.h>
#include <net/flow.h>
#include <net/gre.h>
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 10, 0)
/* v3.10 moved the tunnel definitions from net/ipip.h here; we need it
 * only for iptunnel_xmit_stats() and struct ip_tunnel_parm[_kern]
 */
#include <net/ip_tunnels.h>
#endif
#ifdef flowi4_dscp		/* v6.18 replaced flowi4_tos with flowi4_dscp */
#include <net/inet_dscp.h>
#endif

#include "../eoip_version.h"
#include "eoip_gre.h"
#include "eoip_keepalive.h"

/*
 * Backward compatibility shims.  Everything below the shims is written
 * against the current kernel API.
 */

#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 14, 0)
/* until v3.14 the per-cpu tunnel stats struct was declared per driver;
 * the name pcpu_tstats must match the forward declaration used by the
 * tstats member of struct net_device.  The layout mirrors the later
 * pcpu_sw_netstats so one stats implementation serves all kernels.
 */
struct pcpu_tstats {
	u64			rx_packets;
	u64			rx_bytes;
	u64			tx_packets;
	u64			tx_bytes;
	struct u64_stats_sync	syncp;
};
#define pcpu_sw_netstats pcpu_tstats
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 13, 0)
/* v3.13 added u64_stats_init() for lockdep; the zero initialization
 * from alloc_percpu() is sufficient on kernels that predate it
 */
#define u64_stats_init(syncp) do { } while (0)
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 3, 0)
#define strscpy strlcpy		/* v4.3 */
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 4, 0)
#define nf_reset_ct nf_reset	/* v5.4 */
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 10, 0)
#define ip_tunnel_parm_kern ip_tunnel_parm	/* v6.10 */
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

#ifndef RT_TOS			/* removed after the dscp_t conversion */
#define RT_TOS(tos)	((tos) & IPTOS_TOS_MASK)
#endif

/*
 * The tunnel state is private to this driver and deliberately not
 * struct ip_tunnel: the layout of the kernel one keeps changing and we
 * use none of the ip_tunnel core infrastructure.
 */
struct eoip_tunnel {
	struct eoip_tunnel __rcu *next;
	struct net_device	*dev;
	struct ip_tunnel_parm_kern parms;
	unsigned long		err_time;	/* time of last ICMP error */
	int			err_count;	/* number of ICMP errors seen */
	int			hlen;		/* outer iph + EoIP header */
	struct eoip_keepalive	ka;
};

#define eoip_for_each_tunnel_rcu(t, start) \
	for (t = rcu_dereference(start); t; t = rcu_dereference(t->next))

static struct rtnl_link_ops eoip_ops __read_mostly;
static int eoip_tunnel_bind_dev(struct net_device *dev);

#define EOIP_HASH_SIZE 16
#define HASH(addr) (((__force u32)addr^((__force u32)addr>>4))&0xF)

static int eoip_net_id __read_mostly;
struct eoip_net {
	struct eoip_tunnel __rcu *tunnels[EOIP_HASH_SIZE];
};

/*
 * Locking : hash tables are protected by RCU and RTNL
 */

/* Given src, dst and tunnel id, find appropriate for input tunnel. */

static struct eoip_tunnel *eoip_tunnel_lookup(struct net_device *dev,
						__be32 remote, __be32 local,
						u32 key)
{
	struct net *net = dev_net(dev);
	int link = dev->ifindex;
	unsigned int h0 = HASH(key);
	struct eoip_tunnel *t, *cand = NULL;
	struct eoip_net *ign = net_generic(net, eoip_net_id);
	int score, cand_score = 4;

	eoip_for_each_tunnel_rcu(t, ign->tunnels[h0]) {
		if (local != t->parms.iph.saddr ||
				remote != t->parms.iph.daddr ||
				key != (__force u32)t->parms.i_key ||
				!(t->dev->flags & IFF_UP))
			continue;

		score = 0;
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

static struct eoip_tunnel __rcu **eoip_bucket(struct eoip_net *ign,
		struct ip_tunnel_parm_kern *parms)
{
	unsigned int h = HASH((__force u32)parms->i_key);

	return &ign->tunnels[h];
}

static void eoip_tunnel_link(struct eoip_net *ign, struct eoip_tunnel *t)
{
	struct eoip_tunnel __rcu **tp = eoip_bucket(ign, &t->parms);

	rcu_assign_pointer(t->next, rtnl_dereference(*tp));
	rcu_assign_pointer(*tp, t);
}

static void eoip_tunnel_unlink(struct eoip_net *ign, struct eoip_tunnel *t)
{
	struct eoip_tunnel __rcu **tp;
	struct eoip_tunnel *iter;

	for (tp = eoip_bucket(ign, &t->parms);
			(iter = rtnl_dereference(*tp)) != NULL;
			tp = &iter->next) {
		if (t == iter) {
			rcu_assign_pointer(*tp, t->next);
			break;
		}
	}
}

static struct eoip_tunnel *eoip_tunnel_find(struct net *net,
						struct ip_tunnel_parm_kern *parms,
						int type)
{
	__be32 remote = parms->iph.daddr;
	__be32 local = parms->iph.saddr;
	__be32 key = parms->i_key;
	int link = parms->link;
	struct eoip_tunnel *t;
	struct eoip_tunnel __rcu **tp;
	struct eoip_net *ign = net_generic(net, eoip_net_id);

	for (tp = eoip_bucket(ign, parms);
			(t = rtnl_dereference(*tp)) != NULL;
			tp = &t->next)
		if (local == t->parms.iph.saddr &&
			remote == t->parms.iph.daddr &&
			key == t->parms.i_key &&
			link == t->parms.link &&
			type == t->dev->type)
			break;

	return t;
}

static void eoip_if_uninit(struct net_device *dev)
{
	struct net *net = dev_net(dev);
	struct eoip_net *ign = net_generic(net, eoip_net_id);

	eoip_tunnel_unlink(ign, netdev_priv(dev));
	dev_put(dev);
}

static void eoip_err(struct sk_buff *skb, u32 info)
{
	/* skb->data points at the outer IP header; the gre demux has
	 * already verified the EoIP magic before dispatching here, so
	 * the EoIP header (magic[4] len[2] tid[2], tid little endian)
	 * follows it directly.  ICMP is required to return at least 8
	 * bytes of payload, which is exactly the full EoIP header.
	 */
	const struct iphdr *iph = (const struct iphdr *)skb->data;
	const int type = icmp_hdr(skb)->type;
	const int code = icmp_hdr(skb)->code;
	struct eoip_tunnel *t;
	u32 key;

	if (skb_headlen(skb) < (iph->ihl<<2) + EOIP_HDR_LEN)
		return;
	key = le16_to_cpu(*(__le16 *)(skb->data + (iph->ihl<<2) + 6));

	switch (type) {
	default:
	case ICMP_PARAMETERPROB:
		return;

	case ICMP_DEST_UNREACH:
		switch (code) {
		case ICMP_SR_FAILED:
		case ICMP_PORT_UNREACH:
			/* Impossible event. */
			return;
		case ICMP_FRAG_NEEDED:
			/* Soft state for pmtu is maintained by IP core. */
			return;
		default:
			/* All others are translated to HOST_UNREACH.
			   rfc2003 contains "deep thoughts" about NET_UNREACH,
			   I believe they are just ether pollution. --ANK
			 */
			break;
		}
		break;
	case ICMP_TIME_EXCEEDED:
		if (code != ICMP_EXC_TTL)
			return;
		break;
	}

	rcu_read_lock();
	t = eoip_tunnel_lookup(skb->dev, iph->daddr, iph->saddr, key);
	if (t == NULL || t->parms.iph.daddr == 0 ||
			ipv4_is_multicast(t->parms.iph.daddr))
		goto out;

	if (t->parms.iph.ttl == 0 && type == ICMP_TIME_EXCEEDED)
		goto out;

	if (time_before(jiffies, t->err_time + IPTUNNEL_ERR_TIMEO))
		t->err_count++;
	else
		t->err_count = 1;
	t->err_time = jiffies;
out:
	rcu_read_unlock();
}

static void eoip_rx_stats(struct net_device *dev, unsigned int len)
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

static int eoip_rcv(struct sk_buff *skb)
{
	const struct iphdr *iph;
	u8 *h;
	struct eoip_tunnel *tunnel;
	unsigned int frame_len;
	u32 key;

	if (!pskb_may_pull(skb, EOIP_HDR_LEN))
		goto drop_nolock;

	iph = ip_hdr(skb);
	h = skb->data;

	/* the demux verified the magic in h[0..3]; the tunnel id is
	 * little endian on the wire, the frame length is big endian
	 */
	key = le16_to_cpu(*(__le16 *)(h + 6));
	frame_len = ntohs(*(__be16 *)(h + 4));

	rcu_read_lock();

	tunnel = eoip_tunnel_lookup(skb->dev, iph->saddr, iph->daddr, key);
	if (tunnel) {
		if (frame_len == 0) {
			/* MikroTik keepalive: refresh peer liveness,
			 * never deliver it as a frame
			 */
			eoip_ka_recv(&tunnel->ka);
			rcu_read_unlock();
			consume_skb(skb);
			return 0;
		}

		if (!pskb_may_pull(skb, EOIP_HDR_LEN + ETH_HLEN)) {
			DEV_STATS_INC(tunnel->dev, rx_length_errors);
			DEV_STATS_INC(tunnel->dev, rx_errors);
			goto drop;
		}
		h = skb->data;	/* pskb_may_pull may reallocate the head */

		secpath_reset(skb);

		pskb_pull(skb, EOIP_HDR_LEN);
		skb_postpull_rcsum(skb, h, EOIP_HDR_LEN);
		skb->pkt_type = PACKET_HOST;
#ifdef CONFIG_NET_IPGRE_BROADCAST
		if (ipv4_is_multicast(ip_hdr(skb)->daddr)) {
			/* Looped back packet, drop it! */
			if (rt_is_output_route(skb_rtable(skb)))
				goto drop;
			DEV_STATS_INC(tunnel->dev, multicast);
			skb->pkt_type = PACKET_BROADCAST;
		}
#endif

		/* Warning: All skb pointers will be invalidated! */
		if (!pskb_may_pull(skb, ETH_HLEN)) {
			DEV_STATS_INC(tunnel->dev, rx_length_errors);
			DEV_STATS_INC(tunnel->dev, rx_errors);
			goto drop;
		}

		/* the advertised frame length is authoritative: drop
		 * truncated or absurd frames, trim padding beyond it
		 */
		if (frame_len < ETH_HLEN || skb->len < frame_len) {
			DEV_STATS_INC(tunnel->dev, rx_length_errors);
			DEV_STATS_INC(tunnel->dev, rx_errors);
			goto drop;
		}
		if (skb->len > frame_len)
			pskb_trim_rcsum(skb, frame_len);

		skb->protocol = eth_type_trans(skb, tunnel->dev);
		skb_postpull_rcsum(skb, eth_hdr(skb), ETH_HLEN);

		eoip_rx_stats(tunnel->dev, skb->len);

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
	icmp_send(skb, ICMP_DEST_UNREACH, ICMP_PORT_UNREACH, 0);

drop:
	rcu_read_unlock();
drop_nolock:
	kfree_skb(skb);
	return 0;
}

static void eoip_tx_stats(struct net_device *dev, int pkt_len)
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

/* Route to the peer, prepend the outer IP + EoIP header (frame_size is
 * the value of the EoIP length field: the encapsulated frame length for
 * data, 0 for a keepalive) and hand the skb to ip_local_out().
 * Consumes the skb.  Error counters are always accounted; the tx byte
 * and packet counters only for data - keepalives are tunnel overhead,
 * not interface traffic.
 */
static void eoip_xmit_skb(struct eoip_tunnel *tunnel, struct sk_buff *skb,
			u16 frame_size)
{
	struct net_device *dev = tunnel->dev;
	const struct iphdr *tiph = &tunnel->parms.iph;
	struct flowi4 fl4;
	struct rtable *rt;			/* Route to the other host */
	struct net_device *tdev;	/* Device to other host */
	struct iphdr *iph;			/* Our new IP header */
	unsigned int max_headroom;	/* The extra header space needed */
	int hlen = tunnel->hlen;
	int err;
	int pkt_len;

	IPCB(skb)->flags = 0;

	memset(&fl4, 0, sizeof(fl4));
	fl4.daddr = tiph->daddr;
	fl4.saddr = tiph->saddr;
#ifdef flowi4_dscp
	fl4.flowi4_dscp = inet_dsfield_to_dscp(tiph->tos);
#else
	fl4.flowi4_tos = RT_TOS(tiph->tos);
#endif
	fl4.flowi4_proto = IPPROTO_GRE;
	fl4.fl4_gre_key = tunnel->parms.o_key;
	fl4.flowi4_oif = tunnel->parms.link;

	rt = ip_route_output_key(dev_net(dev), &fl4);
	if (IS_ERR(rt)) {
		DEV_STATS_INC(dev, tx_carrier_errors);
		goto tx_error;
	}
	tdev = rt->dst.dev;

	if (tdev == dev) {
		ip_rt_put(rt);
		DEV_STATS_INC(dev, collisions);
		goto tx_error;
	}

	max_headroom = LL_RESERVED_SPACE(tdev) + hlen + rt->dst.header_len;

	if (skb_headroom(skb) < max_headroom || skb_shared(skb) ||
			(skb_cloned(skb) && !skb_clone_writable(skb, 0))) {
		struct sk_buff *new_skb;

		new_skb = skb_realloc_headroom(skb, max_headroom);
		if (!new_skb) {
			ip_rt_put(rt);
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
	memset(&(IPCB(skb)->opt), 0, sizeof(IPCB(skb)->opt));
	IPCB(skb)->flags &= ~(IPSKB_XFRM_TUNNEL_SIZE | IPSKB_XFRM_TRANSFORMED |
			IPSKB_REROUTED);
	skb_dst_drop(skb);
	skb_dst_set(skb, &rt->dst);

	/*
	 *	Push down and install the EoIP header.
	 */

	iph = ip_hdr(skb);
	iph->version = 4;
	iph->ihl = sizeof(struct iphdr) >> 2;
	iph->frag_off = 0;
	iph->protocol = IPPROTO_GRE;
	iph->tos = tiph->tos;
	iph->daddr = fl4.daddr;
	iph->saddr = fl4.saddr;
	iph->ttl = tiph->ttl;

	if (iph->ttl == 0)
		iph->ttl = ip4_dst_hoplimit(&rt->dst);

	memcpy(iph + 1, EOIP_MAGIC, 4);
	((__be16 *)(iph + 1))[2] = htons(frame_size);
	((__le16 *)(iph + 1))[3] = cpu_to_le16((__force u32)tunnel->parms.i_key);

	nf_reset_ct(skb);

	pkt_len = skb->len - skb_transport_offset(skb);
	skb->ip_summed = CHECKSUM_NONE;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 1, 0) || \
	(LINUX_VERSION_CODE >= KERNEL_VERSION(3, 16, 7) && \
	 LINUX_VERSION_CODE < KERNEL_VERSION(3, 17, 0))
	/* mainline v4.1 form; also backported to the 3.16.y stable
	 * series (early vanilla 3.16 needs the two argument form below)
	 */
	ip_select_ident(dev_net(dev), skb, NULL);
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(3, 16, 0)
	ip_select_ident(skb, NULL);
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(3, 12, 0)
	ip_select_ident(skb, &rt->dst, NULL);
#else
	ip_select_ident(iph, &rt->dst, NULL);
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 4, 0)
	err = ip_local_out(dev_net(dev), NULL, skb);
#else
	err = ip_local_out(skb);
#endif
	if (likely(net_xmit_eval(err) == 0)) {
		if (frame_size)
			eoip_tx_stats(dev, pkt_len);
	} else if (frame_size)
		eoip_tx_stats(dev, -1);

	return;

tx_error:
	DEV_STATS_INC(dev, tx_errors);
	dev_kfree_skb(skb);
}

static netdev_tx_t eoip_if_xmit(struct sk_buff *skb, struct net_device *dev)
{
	struct eoip_tunnel *tunnel = netdev_priv(dev);
	uint16_t frame_size = skb->len;

	if (tunnel->err_count > 0) {
		if (time_before(jiffies,
				tunnel->err_time + IPTUNNEL_ERR_TIMEO)) {
			tunnel->err_count--;

			dst_link_failure(skb);
		} else
			tunnel->err_count = 0;
	}

	eoip_xmit_skb(tunnel, skb, frame_size);

	return NETDEV_TX_OK;
}

static void eoip_ka_send(struct net_device *dev)
{
	struct eoip_tunnel *tunnel = netdev_priv(dev);
	struct sk_buff *skb;
	unsigned int room = LL_MAX_HEADER + sizeof(struct iphdr) + EOIP_HDR_LEN;

	if (!tunnel->parms.iph.daddr)
		return;

	skb = alloc_skb(room, GFP_ATOMIC);
	if (!skb)
		return;
	skb_reserve(skb, room);
	skb->dev = dev;
	skb->protocol = htons(ETH_P_IP);

	eoip_xmit_skb(tunnel, skb, 0);
}

static int eoip_if_open(struct net_device *dev)
{
	struct eoip_tunnel *tunnel = netdev_priv(dev);

	eoip_ka_open(&tunnel->ka);
	return 0;
}

static int eoip_if_close(struct net_device *dev)
{
	struct eoip_tunnel *tunnel = netdev_priv(dev);

	eoip_ka_stop(&tunnel->ka);
	return 0;
}

static int eoip_tunnel_bind_dev(struct net_device *dev)
{
	struct net_device *tdev = NULL;
	struct eoip_tunnel *tunnel = netdev_priv(dev);
	const struct iphdr *iph = &tunnel->parms.iph;
	int hlen = LL_MAX_HEADER;
	int mtu = ETH_DATA_LEN;
	int addend = sizeof(struct iphdr) + EOIP_HDR_LEN;

	/* Guess output device to choose reasonable mtu and needed_headroom */

	if (iph->daddr) {
		struct flowi4 fl4;
		struct rtable *rt;

		memset(&fl4, 0, sizeof(fl4));
		fl4.daddr = iph->daddr;
		fl4.saddr = iph->saddr;
#ifdef flowi4_dscp
		fl4.flowi4_dscp = inet_dsfield_to_dscp(iph->tos);
#else
		fl4.flowi4_tos = RT_TOS(iph->tos);
#endif
		fl4.flowi4_proto = IPPROTO_GRE;
		fl4.fl4_gre_key = tunnel->parms.o_key;
		fl4.flowi4_oif = tunnel->parms.link;
		rt = ip_route_output_key(dev_net(dev), &fl4);
		if (!IS_ERR(rt)) {
			tdev = rt->dst.dev;
			ip_rt_put(rt);
		}
	}

	if (!tdev && tunnel->parms.link)
		tdev = __dev_get_by_index(dev_net(dev), tunnel->parms.link);

	if (tdev) {
		hlen = tdev->hard_header_len + tdev->needed_headroom;
		mtu = tdev->mtu;
	}
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 1, 0)
	dev->iflink = tunnel->parms.link;
#endif

	dev->needed_headroom = addend + hlen;
	mtu -= dev->hard_header_len + addend;

	if (mtu < 68)
		mtu = 68;

	tunnel->hlen = addend;

	return mtu;
}

static int eoip_if_change_mtu(struct net_device *dev, int new_mtu)
{
	struct eoip_tunnel *tunnel = netdev_priv(dev);

	if (new_mtu < 68 ||
			new_mtu > 0xFFF8 - dev->hard_header_len - tunnel->hlen)
		return -EINVAL;
	dev->mtu = new_mtu;
	return 0;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 1, 0)
static int eoip_get_iflink(const struct net_device *dev)
{
	const struct eoip_tunnel *tunnel = netdev_priv(dev);

	return tunnel->parms.link;
}
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 7, 0)
/* since v6.7 the core allocates and frees dev->tstats
 * (dev->pcpu_stat_type == NETDEV_PCPU_STAT_TSTATS)
 */
static void eoip_dev_free(struct net_device *dev)
{
	free_percpu(dev->tstats);
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 12, 0)
	free_netdev(dev);
#endif
}
#endif

static const struct gre_protocol eoip_protocol = {
	.handler = eoip_rcv,
	.err_handler = eoip_err,
};

static void eoip_destroy_tunnels(struct eoip_net *ign, struct list_head *head)
{
	int h;

	for (h = 0; h < EOIP_HASH_SIZE; h++) {
		struct eoip_tunnel *t;

		t = rtnl_dereference(ign->tunnels[h]);

		while (t != NULL) {
			unregister_netdevice_queue(t->dev, head);
			t = rtnl_dereference(t->next);
		}
	}
}

static int __net_init eoip_init_net(struct net *net)
{
	return 0;
}

static void __net_exit eoip_exit_net(struct net *net)
{
	struct eoip_net *ign;
	LIST_HEAD(list);

	ign = net_generic(net, eoip_net_id);
	rtnl_lock();
	eoip_destroy_tunnels(ign, &list);
	unregister_netdevice_many(&list);
	rtnl_unlock();
}

static struct pernet_operations eoip_net_ops = {
	.id = &eoip_net_id,
	.init = eoip_init_net,
	.exit = eoip_exit_net,
	.size = sizeof(struct eoip_net),
};

static int __eoip_validate(struct nlattr *tb[], struct nlattr *data[])
{
	__be32 daddr;

	if (tb[IFLA_ADDRESS]) {
		if (nla_len(tb[IFLA_ADDRESS]) != ETH_ALEN)
			return -EINVAL;
		if (!is_valid_ether_addr(nla_data(tb[IFLA_ADDRESS])))
			return -EADDRNOTAVAIL;
	}

	if (!data)
		return 0;

	if (data[IFLA_GRE_REMOTE]) {
		memcpy(&daddr, nla_data(data[IFLA_GRE_REMOTE]), 4);
		if (!daddr)
			return -EINVAL;
	}

	if (data[IFLA_EOIP_KA_INTERVAL] &&
			nla_get_u32(data[IFLA_EOIP_KA_INTERVAL]) > EOIP_KA_MAX_INTERVAL)
		return -EINVAL;

	return 0;
}

static void eoip_netlink_parms(struct nlattr *data[],
				struct ip_tunnel_parm_kern *parms)
{
	memset(parms, 0, sizeof(*parms));

	parms->iph.protocol = IPPROTO_GRE;

	if (!data)
		return;

	if (data[IFLA_GRE_LINK])
		parms->link = nla_get_u32(data[IFLA_GRE_LINK]);

	if (data[IFLA_GRE_IKEY])
		parms->i_key = nla_get_be32(data[IFLA_GRE_IKEY]);

	if (data[IFLA_GRE_LOCAL])
		parms->iph.saddr = nla_get_be32(data[IFLA_GRE_LOCAL]);

	if (data[IFLA_GRE_REMOTE])
		parms->iph.daddr = nla_get_be32(data[IFLA_GRE_REMOTE]);

	if (data[IFLA_GRE_TTL])
		parms->iph.ttl = nla_get_u8(data[IFLA_GRE_TTL]);

	if (data[IFLA_GRE_TOS])
		parms->iph.tos = nla_get_u8(data[IFLA_GRE_TOS]);
}

static int eoip_if_init(struct net_device *dev)
{
	struct eoip_tunnel *tunnel = netdev_priv(dev);

	tunnel->dev = dev;
	strscpy(tunnel->parms.name, dev->name, IFNAMSIZ);

	eoip_tunnel_bind_dev(dev);
	eoip_ka_init(&tunnel->ka, dev, eoip_ka_send);

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
static struct net_device_stats *eoip_if_get_stats(struct net_device *dev)
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

static const struct net_device_ops eoip_netdev_ops = {
	.ndo_init = eoip_if_init,
	.ndo_uninit = eoip_if_uninit,
	.ndo_open = eoip_if_open,
	.ndo_stop = eoip_if_close,
	.ndo_start_xmit = eoip_if_xmit,
	.ndo_set_mac_address = eth_mac_addr,
	.ndo_validate_addr = eth_validate_addr,
	.ndo_change_mtu = eoip_if_change_mtu,
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 11, 0)
	.ndo_get_stats64 = dev_get_tstats64,
#else
	.ndo_get_stats = eoip_if_get_stats,
#endif
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 1, 0)
	.ndo_get_iflink = eoip_get_iflink,
#endif
};

static void eoip_setup(struct net_device *dev)
{
	ether_setup(dev);

	dev->netdev_ops = &eoip_netdev_ops;

	/* no queue on the virtual interface: transmission is synchronous
	 * into ip_local_out() and queueing happens on the underlying
	 * physical interface; an extra qdisc here only adds latency
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
	dev->priv_destructor = eoip_dev_free;
#else
	dev->destructor = eoip_dev_free;
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 10, 0)
	dev->max_mtu = ETH_MAX_MTU;
#endif
}

static int __eoip_newlink(struct net_device *dev,
			struct nlattr *tb[], struct nlattr *data[])
{
	struct eoip_tunnel *nt = netdev_priv(dev);
	struct net *net = dev_net(dev);
	struct eoip_net *ign = net_generic(net, eoip_net_id);
	int mtu;
	int err;

	eoip_netlink_parms(data, &nt->parms);

	/* keepalive is off unless requested (RouterOS defaults to 10s,10) */
	if (data && data[IFLA_EOIP_KA_INTERVAL])
		nt->ka.interval = nla_get_u32(data[IFLA_EOIP_KA_INTERVAL]);
	if (data && data[IFLA_EOIP_KA_RETRIES])
		nt->ka.retries = nla_get_u8(data[IFLA_EOIP_KA_RETRIES]);

	if (eoip_tunnel_find(net, &nt->parms, dev->type))
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

	mtu = eoip_tunnel_bind_dev(dev);
	if (!tb[IFLA_MTU])
		dev->mtu = mtu;

	err = register_netdevice(dev);
	if (err)
		return err;

	eoip_tunnel_link(ign, nt);

	return 0;
}

static int __eoip_changelink(struct net_device *dev,
			struct nlattr *tb[], struct nlattr *data[])
{
	struct eoip_tunnel *t, *nt = netdev_priv(dev);
	struct net *net = dev_net(dev);
	struct eoip_net *ign = net_generic(net, eoip_net_id);
	struct ip_tunnel_parm_kern p;

	eoip_netlink_parms(data, &p);

	t = eoip_tunnel_find(net, &p, dev->type);

	if (t) {
		if (t->dev != dev)
			return -EEXIST;
	} else {
		t = nt;

		eoip_tunnel_unlink(ign, t);
		t->parms.iph.saddr = p.iph.saddr;
		t->parms.iph.daddr = p.iph.daddr;
		t->parms.i_key = p.i_key;
		eoip_tunnel_link(ign, t);
		netdev_state_change(dev);
	}

	t->parms.iph.ttl = p.iph.ttl;
	t->parms.iph.tos = p.iph.tos;
	t->parms.iph.frag_off = p.iph.frag_off;

	if (t->parms.link != p.link) {
		t->parms.link = p.link;
		eoip_tunnel_bind_dev(dev);
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
static int eoip_newlink(struct net_device *dev,
			struct rtnl_newlink_params *params,
			struct netlink_ext_ack *extack)
{
	return __eoip_newlink(dev, params->tb, params->data);
}
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(4, 13, 0)
static int eoip_newlink(struct net *src_net, struct net_device *dev,
			struct nlattr *tb[], struct nlattr *data[],
			struct netlink_ext_ack *extack)
{
	return __eoip_newlink(dev, tb, data);
}
#else
static int eoip_newlink(struct net *src_net, struct net_device *dev,
			struct nlattr *tb[], struct nlattr *data[])
{
	return __eoip_newlink(dev, tb, data);
}
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 13, 0)
static int eoip_changelink(struct net_device *dev, struct nlattr *tb[],
			struct nlattr *data[],
			struct netlink_ext_ack *extack)
{
	return __eoip_changelink(dev, tb, data);
}

static int eoip_validate(struct nlattr *tb[], struct nlattr *data[],
			struct netlink_ext_ack *extack)
{
	return __eoip_validate(tb, data);
}
#else
static int eoip_changelink(struct net_device *dev, struct nlattr *tb[],
			struct nlattr *data[])
{
	return __eoip_changelink(dev, tb, data);
}

static int eoip_validate(struct nlattr *tb[], struct nlattr *data[])
{
	return __eoip_validate(tb, data);
}
#endif

static size_t eoip_get_size(const struct net_device *dev)
{
	return
		/* IFLA_GRE_LINK */
		nla_total_size(4) +
		/* IFLA_GRE_IKEY */
		nla_total_size(4) +
		/* IFLA_GRE_LOCAL */
		nla_total_size(4) +
		/* IFLA_GRE_REMOTE */
		nla_total_size(4) +
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

static int eoip_fill_info(struct sk_buff *skb, const struct net_device *dev)
{
	struct eoip_tunnel *t = netdev_priv(dev);
	struct ip_tunnel_parm_kern *p = &t->parms;

	if (nla_put_u32(skb, IFLA_GRE_LINK, p->link) ||
		nla_put_be32(skb, IFLA_GRE_IKEY, p->i_key) ||
		nla_put_be32(skb, IFLA_GRE_LOCAL, p->iph.saddr) ||
		nla_put_be32(skb, IFLA_GRE_REMOTE, p->iph.daddr) ||
		nla_put_u8(skb, IFLA_GRE_TTL, p->iph.ttl) ||
		nla_put_u8(skb, IFLA_GRE_TOS, p->iph.tos) ||
		nla_put_u32(skb, IFLA_EOIP_KA_INTERVAL, t->ka.interval) ||
		nla_put_u8(skb, IFLA_EOIP_KA_RETRIES, t->ka.retries))
		goto nla_put_failure;

	return 0;

nla_put_failure:
	return -EMSGSIZE;
}

static const struct nla_policy eoip_policy[IFLA_EOIP_ATTR_MAX + 1] = {
	[IFLA_GRE_LINK]		= { .type = NLA_U32 },
	[IFLA_GRE_IKEY]		= { .type = NLA_U32 },
	[IFLA_GRE_LOCAL]	= { .len = 4 },
	[IFLA_GRE_REMOTE]	= { .len = 4 },
	[IFLA_GRE_TTL]		= { .type = NLA_U8 },
	[IFLA_GRE_TOS]		= { .type = NLA_U8 },
	[IFLA_EOIP_KA_INTERVAL]	= { .type = NLA_U32 },
	[IFLA_EOIP_KA_RETRIES]	= { .type = NLA_U8 },
};

static struct rtnl_link_ops eoip_ops __read_mostly = {
	.kind		= "eoip",
	.maxtype	= IFLA_EOIP_ATTR_MAX,
	.policy		= eoip_policy,
	.priv_size	= sizeof(struct eoip_tunnel),
	.setup		= eoip_setup,
	.validate	= eoip_validate,
	.newlink	= eoip_newlink,
	.changelink	= eoip_changelink,
	.get_size	= eoip_get_size,
	.fill_info	= eoip_fill_info,
};

/*
 *	And now the modules code and kernel interface.
 */

static int __init eoip_init(void)
{
	int err;

	pr_info("EoIP (IPv4) tunneling driver v" EOIP_VERSION "\n");

	err = register_pernet_device(&eoip_net_ops);
	if (err < 0)
		return err;

	err = gre_add_protocol(&eoip_protocol, GREPROTO_NONSTD_EOIP);
	if (err < 0) {
		pr_info("eoip init: can't add protocol\n");
		goto add_proto_failed;
	}

	err = rtnl_link_register(&eoip_ops);
	if (err < 0)
		goto eoip_ops_failed;

out:
	return err;

eoip_ops_failed:
	gre_del_protocol(&eoip_protocol, GREPROTO_NONSTD_EOIP);
add_proto_failed:
	unregister_pernet_device(&eoip_net_ops);
	goto out;
}

static void __exit eoip_fini(void)
{
	rtnl_link_unregister(&eoip_ops);
	if (gre_del_protocol(&eoip_protocol, GREPROTO_NONSTD_EOIP) < 0)
		pr_info("eoip close: can't remove protocol\n");
	unregister_pernet_device(&eoip_net_ops);
}

module_init(eoip_init);
module_exit(eoip_fini);
MODULE_LICENSE("GPL");
MODULE_VERSION(EOIP_VERSION);
MODULE_DESCRIPTION("MikroTik compatible EoIP (Ethernet over IP) tunnel driver");
MODULE_AUTHOR("Boian Bonev <bbonev@ipacct.com>");
MODULE_ALIAS_RTNL_LINK("eoip");
