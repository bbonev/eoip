/*
 *	MikroTik compatible tunnel keepalive engine, shared by the EoIP
 *	(eoip.c) and EoIPv6 (eoip6.c) drivers.
 *
 *	The wire format of a keepalive differs per family (an EoIP header
 *	with zero frame length vs a bare 2 byte EoIPv6 header) and is
 *	produced by the send callback; the state machine is common and
 *	matches the RouterOS behavior:
 *
 *	 - each side independently transmits one keepalive every interval;
 *	   keepalives are not echoed and there is no reply
 *	 - only received keepalives count as peer liveness, data does not
 *	 - after interval * retries without a received keepalive the
 *	   carrier is dropped; transmission continues while the peer is
 *	   considered down, which is what makes both ends auto-recover;
 *	   carrier returns on the first received keepalive
 *	 - interval 0 disables keepalive entirely (the default),
 *	   retries 0 selects send-only mode (carrier never dropped)
 *
 *	Concurrency: interval/retries are written only under RTNL with the
 *	timer synchronously stopped first and read from softirq via
 *	READ_ONCE(); rx_time is written by softirq RX and read by the
 *	timer; the timer is armed only by eoip_ka_start() (RTNL) and by
 *	its own callback, so timer_delete_sync() under RTNL fully
 *	quiesces it.  ndo_stop precedes ndo_uninit/free on every teardown
 *	path, which keeps the callback's netdev reference valid.
 *
 *	This program is free software; you can redistribute it and/or
 *	modify it under the terms of the GNU General Public License
 *	as published by the Free Software Foundation; either version
 *	2 of the License, or (at your option) any later version.
 */

#ifndef _EOIP_KEEPALIVE_H
#define _EOIP_KEEPALIVE_H

#include <linux/version.h>
#include <linux/timer.h>
#include <linux/jiffies.h>
#include <linux/netdevice.h>

#include "eoip_proto.h"

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 2, 0)
/* v6.2 added timer_delete_sync(); the del_timer_sync() wrapper it
 * replaces was removed in v6.15
 */
#define timer_delete_sync del_timer_sync
#endif

struct eoip_keepalive {
	struct timer_list	timer;
	struct net_device	*dev;
	void			(*send)(struct net_device *dev);
	unsigned long		rx_time;	/* jiffies of last keepalive rx */
	u32			interval;	/* seconds; 0 = keepalive off */
	u8			retries;	/* 0 = send-only, no carrier drop */
};

static void __eoip_ka_timer(struct eoip_keepalive *ka)
{
	struct net_device *dev = ka->dev;
	u32 interval = READ_ONCE(ka->interval);
	u8 retries = READ_ONCE(ka->retries);

	if (!netif_running(dev) || !interval)
		return;		/* raced with stop/reconfig; do not re-arm */

	ka->send(dev);

	if (retries && time_after(jiffies, READ_ONCE(ka->rx_time) +
			(unsigned long)interval * retries * HZ))
		netif_carrier_off(dev);

	mod_timer(&ka->timer, jiffies + (unsigned long)interval * HZ);
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 14, 0)
/* v4.14 introduced timer_setup(); the callback receives the timer */
static void eoip_ka_timer(struct timer_list *t)
{
	__eoip_ka_timer(container_of(t, struct eoip_keepalive, timer));
}
#else
static void eoip_ka_timer(unsigned long data)
{
	__eoip_ka_timer((struct eoip_keepalive *)data);
}
#endif

/* ndo_init; never arms the timer */
static void eoip_ka_init(struct eoip_keepalive *ka, struct net_device *dev,
			void (*send)(struct net_device *dev))
{
	ka->dev = dev;
	ka->send = send;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 14, 0)
	timer_setup(&ka->timer, eoip_ka_timer, 0);
#else
	setup_timer(&ka->timer, eoip_ka_timer, (unsigned long)ka);
#endif
}

/* start/refresh keepalive on an up device; caller holds RTNL */
static void eoip_ka_start(struct eoip_keepalive *ka)
{
	/* WRITE_ONCE: the RX softirq may store rx_time concurrently */
	WRITE_ONCE(ka->rx_time, jiffies);	/* full grace period */
	netif_carrier_on(ka->dev);
	ka->send(ka->dev);	/* announce ourselves at once */
	mod_timer(&ka->timer, jiffies + (unsigned long)ka->interval * HZ);
}

/* ndo_open */
static void eoip_ka_open(struct eoip_keepalive *ka)
{
	if (ka->interval)
		eoip_ka_start(ka);
	else
		netif_carrier_on(ka->dev);	/* un-stick a monitored-down carrier */
}

/* ndo_stop; process context under RTNL */
static void eoip_ka_stop(struct eoip_keepalive *ka)
{
	timer_delete_sync(&ka->timer);
}

/* apply configuration; caller holds RTNL; safe whether up or down */
static void eoip_ka_config(struct eoip_keepalive *ka, u32 interval, u8 retries)
{
	timer_delete_sync(&ka->timer);
	WRITE_ONCE(ka->interval, interval);
	WRITE_ONCE(ka->retries, retries);
	if (!interval || !retries)
		netif_carrier_on(ka->dev);	/* not monitoring: carrier pinned on */
	if (interval && netif_running(ka->dev))
		eoip_ka_start(ka);
}

/* softirq RX of a keepalive matching this tunnel */
static void eoip_ka_recv(struct eoip_keepalive *ka)
{
	if (!READ_ONCE(ka->interval) || !READ_ONCE(ka->retries))
		return;		/* off or send-only: carrier not managed by rx */
	WRITE_ONCE(ka->rx_time, jiffies);
	if (!netif_carrier_ok(ka->dev))
		netif_carrier_on(ka->dev);
}

#endif
