// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (c) 2022 Huawei Device Co., Ltd.
 *
 * NewIP input
 * Linux NewIP INET implementation
 *
 * Based on net/ipv6/ip6_input.c
 */
#define pr_fmt(fmt) "NIP-INPUT: " fmt

#include <linux/errno.h>
#include <linux/types.h>
#include <linux/socket.h>
#include <linux/net.h>
#include <linux/netdevice.h>
#include <linux/nip.h>

#include <net/sock.h>
#include <net/protocol.h>
#include <net/dst_metadata.h>
#include <net/transp_nip.h>
#include <net/nip_route.h>
#include <net/nip.h>

#include "nip_hdr.h"

static int _nip_update_recv_skb_len(struct sk_buff *skb,
				    struct nip_hdr_decap *niph)
{
	if (!niph->include_total_len)
		return 0;

	if (niph->total_len > skb->len) {
		DEBUG("%s: total_len(%u) is bigger than skb_len(%u), Drop a packet.",
		      __func__, niph->total_len, skb->len);
		return NET_RX_DROP;
	}

	skb->len = niph->total_len;
	return 0;
}

static int nip_rcv_finish(struct sk_buff *skb)
{
	struct net *net = dev_net(skb->dev);
	void (*edemux)(struct sk_buff *skb) = NULL;

	/* set /proc/sys/net/ipv4/ip_early_demux to change sysctl_ip_early_demux,
	 * which is used by ipv4, ipv6 and newip
	 */
	if (net->ipv4.sysctl_ip_early_demux && !skb_dst(skb) && !skb->sk) {
		const struct ninet_protocol *ipprot;

		DEBUG("%s: try to early demux skb, nexthdr=0x%x.", __func__, NIPCB(skb)->nexthdr);
		ipprot = rcu_dereference(ninet_protos[NIPCB(skb)->nexthdr]);
		if (ipprot)
			edemux = READ_ONCE(ipprot->early_demux);
		if (edemux)
			edemux(skb);
	}

	/* nip_route_input will set nip_null_entry
	 * instead of NULL in skb when looking up failed.
	 */
	if (!skb_valid_dst(skb))
		nip_route_input(skb);

	return dst_input(skb);
}

int nip_rcv(struct sk_buff *skb, struct net_device *dev,
	    struct packet_type *pt, struct net_device *orig_dev)
{
	int offset = 0;
	struct nip_hdr_decap niph = {0};

	if (skb->pkt_type == PACKET_OTHERHOST) {
		kfree_skb(skb);
		return NET_RX_DROP;
	}

	skb = skb_share_check(skb, GFP_ATOMIC);
	if (!skb)
		goto out;

	memset(NIPCB(skb), 0, sizeof(struct ninet_skb_parm));
	offset = nip_hdr_parse(skb->data, skb->len, &niph);
	if (offset <= 0) {
		DEBUG("%s check in failure, errcode=%d, Drop a packet.(nexthdr=%u, hdr_len=%u)",
		      __func__, offset, niph.nexthdr, niph.hdr_len);
		goto drop;
	}

	if (niph.nexthdr != IPPROTO_UDP && niph.nexthdr != IPPROTO_TCP &&
	    niph.nexthdr != IPPROTO_NIP_ICMP) {
		DEBUG("%s nexthdr(%u) invalid, Drop a packet.", __func__, niph.nexthdr);
		goto drop;
	}

	niph.total_len = ntohs(niph.total_len);
	NIPCB(skb)->dstaddr = niph.daddr;
	NIPCB(skb)->srcaddr = niph.saddr;
	NIPCB(skb)->nexthdr = niph.nexthdr;
	skb->transport_header = skb->network_header + offset;
	skb_orphan(skb);

	/* SKB refreshes the length after replication */
	if (_nip_update_recv_skb_len(skb, &niph))
		goto drop;

	return nip_rcv_finish(skb);
drop:
	kfree_skb(skb);
out:
	return NET_RX_DROP;
}

/* Deliver the packet to transport layer,
 * including TCP, UDP and ICMP.
 * Caller must hold rcu.
 */
void nip_protocol_deliver_rcu(struct sk_buff *skb)
{
	const struct ninet_protocol *ipprot;

	if (!pskb_pull(skb, skb_transport_offset(skb)))
		goto discard;

	ipprot = rcu_dereference(ninet_protos[NIPCB(skb)->nexthdr]);
	if (ipprot) {
		ipprot->handler(skb);
	} else {
		kfree_skb(skb);
		DEBUG("not found transport protol, drop this packet!");
	}
	return;

discard:
	kfree_skb(skb);
}

/* Generally called by dst_input */
int nip_input(struct sk_buff *skb)
{
	rcu_read_lock();
	nip_protocol_deliver_rcu(skb);
	rcu_read_unlock();

	return 0;
}
