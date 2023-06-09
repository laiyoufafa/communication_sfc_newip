// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2022 Huawei Device Co., Ltd.
 *
 * NewIP INET
 * An implementation of the TCP/IP protocol suite for the LINUX
 * operating system. NewIP INET is implemented using the  BSD Socket
 * interface as the means of communication with the user level.
 *
 * Implementation of the Transmission Control Protocol(TCP).
 *
 * Based on net/ipv4/tcp_timer.c
 */
#define pr_fmt(fmt) "NIP-TCP: " fmt

#include <net/tcp_nip.h>
#include <linux/module.h>
#include "tcp_nip_parameter.h"

/**
 *  tcp_nip_orphan_retries() - Returns maximal number of retries on an orphaned socket
 *  @sk:    Pointer to the current socket.
 *  @alive: bool, socket alive state
 */
static int tcp_nip_orphan_retries(struct sock *sk, bool alive)
{
	int retries = sock_net(sk)->ipv4.sysctl_tcp_orphan_retries; /* May be zero. */

	/* We know from an ICMP that something is wrong. */
	if (sk->sk_err_soft && !alive)
		retries = 0;

	/* However, if socket sent something recently, select some safe
	 * number of retries. 8 corresponds to >100 seconds with minimal
	 * RTO of 200msec.
	 */
	if (retries == 0 && alive)
		retries = 8;
	return retries;
}

void tcp_nip_delack_timer_handler(struct sock *sk)
{
	struct inet_connection_sock *icsk = inet_csk(sk);

	if (((1 << sk->sk_state) & (TCPF_CLOSE | TCPF_LISTEN)) ||
	    !(icsk->icsk_ack.pending & ICSK_ACK_TIMER))
		goto out;

	if (time_after(icsk->icsk_ack.timeout, jiffies)) {
		sk_reset_timer(sk, &icsk->icsk_delack_timer, icsk->icsk_ack.timeout);
		goto out;
	}
	icsk->icsk_ack.pending &= ~ICSK_ACK_TIMER;

	if (inet_csk_ack_scheduled(sk)) {
		icsk->icsk_ack.ato      = TCP_ATO_MIN;
		tcp_mstamp_refresh(tcp_sk(sk));
		tcp_nip_send_ack(sk);
		__NET_INC_STATS(sock_net(sk), LINUX_MIB_DELAYEDACKS);
	}

out:;
}

static void tcp_nip_write_err(struct sock *sk)
{
	sk->sk_err = sk->sk_err_soft ? : ETIMEDOUT;
	sk->sk_error_report(sk);
	/* Releasing TCP Resources */
	tcp_nip_done(sk);
	__NET_INC_STATS(sock_net(sk), LINUX_MIB_TCPABORTONTIMEOUT);
}

static void tcp_nip_delack_timer(struct timer_list *t)
{
	struct inet_connection_sock *icsk =
			from_timer(icsk, t, icsk_delack_timer);
	struct sock *sk = &icsk->icsk_inet.sk;

	bh_lock_sock(sk);
	if (!sock_owned_by_user(sk)) {
		tcp_nip_delack_timer_handler(sk);
	} else {
		__NET_INC_STATS(sock_net(sk), LINUX_MIB_DELAYEDACKLOCKED);
		/* deleguate our work to tcp_release_cb() */
		if (!test_and_set_bit(TCP_NIP_DELACK_TIMER_DEFERRED, &sk->sk_tsq_flags))
			sock_hold(sk);
	}
	bh_unlock_sock(sk);
	sock_put(sk);
}

static bool retransmits_nip_timed_out(struct sock *sk,
				      unsigned int boundary,
				      unsigned int timeout,
				      bool syn_set)
{
	/* Newip does not support the calculation of the timeout period based on the timestamp.
	 * Currently, it determines whether the timeout period is based on
	 * the retransmission times
	 */
	DEBUG("%s: icsk->retransmits=%u, boundary=%u", __func__,
	      inet_csk(sk)->icsk_retransmits, boundary);
	return inet_csk(sk)->icsk_retransmits > boundary;
}

static int tcp_nip_write_timeout(struct sock *sk)
{
	struct inet_connection_sock *icsk = inet_csk(sk);
	struct net *net = sock_net(sk);
	int retry_until;
	bool syn_set = false;

	if ((1 << sk->sk_state) & (TCPF_SYN_SENT | TCPF_SYN_RECV)) {
		retry_until = icsk->icsk_syn_retries ? : net->ipv4.sysctl_tcp_syn_retries;
		syn_set = true;
	} else {
		retry_until = net->ipv4.sysctl_tcp_retries2;
		if (sock_flag(sk, SOCK_DEAD)) {
			const bool alive = icsk->icsk_rto < TCP_RTO_MAX;

			/* In the case of SOCK_DEAD, the retry_until value is smaller */
			retry_until = tcp_nip_orphan_retries(sk, alive);
		}
	}

	if (retransmits_nip_timed_out(sk, retry_until,
				      syn_set ? 0 : icsk->icsk_user_timeout, syn_set)) {
		DEBUG("%s: tcp retransmit time out!!!", __func__);
		tcp_nip_write_err(sk);
		return 1;
	}
	return 0;
}

void tcp_nip_retransmit_timer(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct tcp_nip_common *ntp = &tcp_nip_sk(sk)->common;
	struct inet_connection_sock *icsk = inet_csk(sk);
	struct sk_buff *skb = tcp_write_queue_head(sk);
	struct tcp_skb_cb *scb = TCP_SKB_CB(skb);
	u32 icsk_rto_last;

	if (!tp->packets_out)
		return;

	if (tcp_nip_write_queue_empty(sk))
		return;

	tp->tlp_high_seq = 0;

	if (tcp_nip_write_timeout(sk))
		return;

	if (tcp_nip_retransmit_skb(sk, skb, 1) > 0) {
		if (!icsk->icsk_retransmits)
			icsk->icsk_retransmits = 1;
		inet_csk_reset_xmit_timer(sk, ICSK_TIME_RETRANS,
					  min(icsk->icsk_rto, TCP_RESOURCE_PROBE_INTERVAL),
					  TCP_RTO_MAX);

		SSTHRESH_DBG("%s seq %u retransmit fail, win=%u, rto=%u, pkt_out=%u",
			     __func__, scb->seq, ntp->nip_ssthresh,
			     icsk->icsk_rto, tp->packets_out);
		return;
	}
	icsk->icsk_backoff++;
	icsk->icsk_retransmits++;

	icsk_rto_last = icsk->icsk_rto;
	icsk->icsk_rto = min(icsk->icsk_rto << 1, TCP_RTO_MAX);

	SSTHRESH_DBG("%s seq %u, reset win %u to %u, rto %u to %u, pkt_out=%u",
		     __func__, scb->seq, ntp->nip_ssthresh, g_ssthresh_low,
		     icsk_rto_last, icsk->icsk_rto, tp->packets_out);

	ntp->nip_ssthresh = g_ssthresh_low;

	inet_csk_reset_xmit_timer(sk, ICSK_TIME_RETRANS, icsk->icsk_rto, TCP_RTO_MAX);
}

void tcp_nip_probe_timer(struct sock *sk)
{
	struct inet_connection_sock *icsk = inet_csk(sk);
	struct tcp_sock *tp = tcp_sk(sk);
	int max_probes;
	int icsk_backoff;
	int icsk_probes_out;

	if (tp->packets_out || !tcp_nip_send_head(sk)) {
		icsk->icsk_probes_out = 0;
		icsk->icsk_backoff = 0;  // V4 no modified this line
		DEBUG("%s packets_out(%u) not 0 or send_head is NULL, cancel probe0 timer.",
		      __func__, tp->packets_out);
		return;
	}

	/* default: sock_net(sk)->ipv4.sysctl_tcp_retries2 */
	max_probes = g_nip_probe_max; /* fix session auto close */

	if (sock_flag(sk, SOCK_DEAD)) {
		const bool alive = inet_csk_rto_backoff(icsk, TCP_RTO_MAX) < TCP_RTO_MAX;

		max_probes = tcp_nip_orphan_retries(sk, alive);
		DEBUG("%s sock dead, icsk_backoff=%u, max_probes=%u, alive=%u",
		      __func__, icsk->icsk_backoff, max_probes, alive);
		if (!alive && icsk->icsk_backoff >= max_probes) {
			DEBUG("%s will close session, icsk_backoff=%u, max_probes=%u",
			      __func__, icsk->icsk_backoff, max_probes);
			goto abort;
		}
	}

	if (icsk->icsk_probes_out >= max_probes) {
abort:		icsk_backoff = icsk->icsk_backoff;
		icsk_probes_out = icsk->icsk_probes_out;
		DEBUG("%s close session, icsk_probes_out=%u, icsk_backoff=%u, max_probes=%u",
		      __func__, icsk_probes_out, icsk_backoff, max_probes);
		tcp_nip_write_err(sk);
	} else {
		icsk_backoff = icsk->icsk_backoff;
		icsk_probes_out = icsk->icsk_probes_out;
		DEBUG("%s will send probe0, icsk_probes_out=%u, icsk_backoff=%u, max_probes=%u",
		      __func__, icsk_probes_out, icsk_backoff, max_probes);
		/* Only send another probe if we didn't close things up. */
		tcp_nip_send_probe0(sk);
	}
}

void tcp_nip_write_timer_handler(struct sock *sk)
{
	struct inet_connection_sock *icsk = inet_csk(sk);
	int event;

	if (((1 << sk->sk_state) & (TCPF_CLOSE | TCPF_LISTEN)) || !icsk->icsk_pending)
		return;

	if (time_after(icsk->icsk_timeout, jiffies)) {
		sk_reset_timer(sk, &icsk->icsk_retransmit_timer, icsk->icsk_timeout);
		return;
	}
	tcp_mstamp_refresh(tcp_sk(sk));
	event = icsk->icsk_pending;

	switch (event) {
	case ICSK_TIME_RETRANS:
		icsk->icsk_pending = 0;
		tcp_nip_retransmit_timer(sk);
		break;
	case ICSK_TIME_PROBE0:
		icsk->icsk_pending = 0;
		tcp_nip_probe_timer(sk);
		break;
	default:
		break;
	}
}

static void tcp_nip_write_timer(struct timer_list *t)
{
	struct inet_connection_sock *icsk =
			from_timer(icsk, t, icsk_retransmit_timer);
	struct sock *sk = &icsk->icsk_inet.sk;

	bh_lock_sock(sk);
	if (!sock_owned_by_user(sk)) {
		tcp_nip_write_timer_handler(sk);
	} else {
		/* delegate our work to tcp_release_cb() */
		if (!test_and_set_bit(TCP_NIP_WRITE_TIMER_DEFERRED, &sk->sk_tsq_flags))
			sock_hold(sk);
	}
	bh_unlock_sock(sk);
	sock_put(sk);
}

static bool tcp_nip_keepalive_is_timeout(struct sock *sk, u32 elapsed)
{
	struct inet_connection_sock *icsk = inet_csk(sk);
	struct tcp_sock *tp = tcp_sk(sk);
	struct tcp_nip_common *ntp = &tcp_nip_sk(sk)->common;
	u32 keepalive_time = keepalive_time_when(tp);
	bool is_timeout = false;

	/* keepalive set by setsockopt */
	if (keepalive_time > HZ) {
		/* If the TCP_USER_TIMEOUT option is enabled, use that
		 * to determine when to timeout instead.
		 */
		if ((icsk->icsk_user_timeout != 0 &&
		     elapsed >= msecs_to_jiffies(icsk->icsk_user_timeout) &&
		     ntp->nip_keepalive_out > 0) ||
		     (icsk->icsk_user_timeout == 0 &&
		      ntp->nip_keepalive_out >= keepalive_probes(tp))) {
			DEBUG("%s normal keepalive timeout, keepalive_out=%u.",
			      __func__, ntp->nip_keepalive_out);
			tcp_nip_write_err(sk);
			is_timeout = true;
		}
	}

	return is_timeout;
}

static void tcp_nip_keepalive_timer(struct timer_list *t)
{
	struct sock *sk = from_timer(sk, t, sk_timer);
	struct tcp_sock *tp = tcp_sk(sk);
	struct tcp_nip_common *ntp = &tcp_nip_sk(sk)->common;
	u32 elapsed;

	/* Only process if socket is not in use. */
	bh_lock_sock(sk);
	if (sock_owned_by_user(sk)) {
		/* Try again later. */
		inet_csk_reset_keepalive_timer(sk, HZ / TCP_NIP_KEEPALIVE_CYCLE_MS_DIVISOR);
		goto out;
	}

	if (sk->sk_state == TCP_LISTEN) {
		DEBUG("%s: keepalive on a LISTEN", __func__);
		goto out;
	}
	tcp_mstamp_refresh(tp);
	/* 2022-02-18
	 * NewIP TCP doesn't have TIME_WAIT state, so socket in TCP_CLOSING
	 * uses keepalive timer to release socket.
	 */
	if ((sk->sk_state == TCP_FIN_WAIT2 || sk->sk_state == TCP_CLOSING) &&
	    sock_flag(sk, SOCK_DEAD)) {
		DEBUG("%s: finish wait, close sock, sk_state=%u", __func__, sk->sk_state);
		goto death;
	}

	if (!sock_flag(sk, SOCK_KEEPOPEN) ||
	    ((1 << sk->sk_state) & (TCPF_CLOSE | TCPF_SYN_SENT)))
		goto out;

	elapsed = keepalive_time_when(tp);

	/* It is alive without keepalive 8) */
	if (tp->packets_out || !tcp_write_queue_empty(sk))
		goto resched;

	elapsed = keepalive_time_elapsed(tp);
	if (elapsed >= keepalive_time_when(tp)) {
		if (tcp_nip_keepalive_is_timeout(sk, elapsed))
			goto out;

		if (tcp_nip_write_wakeup(sk, LINUX_MIB_TCPKEEPALIVE) <= 0) {
			ntp->nip_keepalive_out++;
			ntp->idle_ka_probes_out++;
			elapsed = keepalive_intvl_when(tp);
		} else {
			/* If keepalive was lost due to local congestion,
			 * try harder.
			 */
			elapsed = TCP_RESOURCE_PROBE_INTERVAL;
		}
	} else {
		/* It is tp->rcv_tstamp + keepalive_time_when(tp) */
		elapsed = keepalive_time_when(tp) - elapsed;
	}

	sk_mem_reclaim(sk);

resched:
	inet_csk_reset_keepalive_timer(sk, elapsed);
	goto out;

death:
	tcp_nip_done(sk);

out:
	tcp_nip_keepalive_disable(sk);
	bh_unlock_sock(sk);
	sock_put(sk);
}

void tcp_nip_init_xmit_timers(struct sock *sk)
{
	inet_csk_init_xmit_timers(sk, &tcp_nip_write_timer, &tcp_nip_delack_timer,
				  &tcp_nip_keepalive_timer);
}

void tcp_nip_clear_xmit_timers(struct sock *sk)
{
	inet_csk_clear_xmit_timers(sk);
}
