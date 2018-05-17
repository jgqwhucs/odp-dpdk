/* Copyright (c) 2013-2018, Linaro Limited
 * Copyright (c) 2013, Nokia Solutions and Networks
 * All rights reserved.
 *
 * SPDX-License-Identifier:     BSD-3-Clause
 */

#include "config.h"

#include <odp_posix_extensions.h>

#include <sys/socket.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <linux/if_packet.h>
#include <linux/filter.h>
#include <ctype.h>
#include <fcntl.h>
#include <unistd.h>
#include <bits/wordsize.h>
#include <net/ethernet.h>
#include <netinet/ip.h>
#include <arpa/inet.h>
#include <stdint.h>
#include <string.h>
#include <net/if.h>
#include <inttypes.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <sys/syscall.h>
#include <linux/ethtool.h>
#include <linux/sockios.h>

#include <odp_api.h>
#include <odp_packet_socket.h>
#include <odp_packet_internal.h>
#include <odp_packet_io_internal.h>
#include <odp_align_internal.h>
#include <odp_debug_internal.h>
#include <odp_classification_datamodel.h>
#include <odp_classification_inlines.h>
#include <odp_classification_internal.h>
#include <odp/api/hints.h>

#include <protocols/eth.h>
#include <protocols/ip.h>

#define MAX_SEGS          CONFIG_PACKET_MAX_SEGS
#define PACKET_JUMBO_LEN  (9 * 1024)

static int disable_pktio; /** !0 this pktio disabled, 0 enabled */

static int sock_stats_reset(pktio_entry_t *pktio_entry);

/** Provide a sendmmsg wrapper for systems with no libc or kernel support.
 *  As it is implemented as a weak symbol, it has zero effect on systems
 *  with both.
 */
int sendmmsg(int fd, struct mmsghdr *vmessages, unsigned int vlen,
	     int flags) __attribute__((weak));
int sendmmsg(int fd, struct mmsghdr *vmessages, unsigned int vlen, int flags)
{
#ifdef SYS_sendmmsg
	return syscall(SYS_sendmmsg, fd, vmessages, vlen, flags);
#else
	/* Emulate sendmmsg using sendmsg.
	 * Note: this emulated version does break sendmmsg promise
	 * that for blocking calls all the messages will be handled
	 * so it's not a good general purpose sendmmsg emulator,
	 * but for our purposes it suffices.
	 */
	ssize_t ret;

	if (vlen) {
		ret = sendmsg(fd, &vmessages->msg_hdr, flags);

		if (ret != -1) {
			vmessages->msg_len = ret;
			return 1;
		}
	}

	return -1;

#endif
}

/** Eth buffer start offset from u32-aligned address to make sure the following
 * header (e.g. IP) starts at a 32-bit aligned address.
 */
#define ETHBUF_OFFSET (ODP_ALIGN_ROUNDUP(_ODP_ETHHDR_LEN, sizeof(uint32_t)) \
				- _ODP_ETHHDR_LEN)

/** Round up buffer address to get a properly aliged eth buffer, i.e. aligned
 * so that the next header always starts at a 32bit aligned address.
 */
#define ETHBUF_ALIGN(buf_ptr) ((uint8_t *)ODP_ALIGN_ROUNDUP_PTR((buf_ptr), \
				sizeof(uint32_t)) + ETHBUF_OFFSET)

/**
 * ODP_PACKET_SOCKET_MMSG:
 * ODP_PACKET_SOCKET_MMAP:
 * ODP_PACKET_NETMAP:
 */
int mac_addr_get_fd(int fd, const char *name, unsigned char mac_dst[])
{
	struct ifreq ethreq;
	int ret;

	memset(&ethreq, 0, sizeof(ethreq));
	snprintf(ethreq.ifr_name, IF_NAMESIZE, "%s", name);
	ret = ioctl(fd, SIOCGIFHWADDR, &ethreq);
	if (ret != 0) {
		__odp_errno = errno;
		ODP_ERR("ioctl(SIOCGIFHWADDR): %s: \"%s\".\n", strerror(errno),
			ethreq.ifr_name);
		return -1;
	}

	memcpy(mac_dst, (unsigned char *)ethreq.ifr_ifru.ifru_hwaddr.sa_data,
	       ETH_ALEN);
	return 0;
}

/*
 * ODP_PACKET_SOCKET_MMSG:
 * ODP_PACKET_SOCKET_MMAP:
 * ODP_PACKET_NETMAP:
 */
uint32_t mtu_get_fd(int fd, const char *name)
{
	struct ifreq ifr;
	int ret;

	snprintf(ifr.ifr_name, IF_NAMESIZE, "%s", name);
	ret = ioctl(fd, SIOCGIFMTU, &ifr);
	if (ret < 0) {
		__odp_errno = errno;
		ODP_DBG("ioctl(SIOCGIFMTU): %s: \"%s\".\n", strerror(errno),
			ifr.ifr_name);
		return 0;
	}
	return ifr.ifr_mtu + _ODP_ETHHDR_LEN;
}

/*
 * ODP_PACKET_SOCKET_MMSG:
 * ODP_PACKET_SOCKET_MMAP:
 * ODP_PACKET_NETMAP:
 */
int promisc_mode_set_fd(int fd, const char *name, int enable)
{
	struct ifreq ifr;
	int ret;

	snprintf(ifr.ifr_name, IF_NAMESIZE, "%s", name);
	ret = ioctl(fd, SIOCGIFFLAGS, &ifr);
	if (ret < 0) {
		__odp_errno = errno;
		ODP_DBG("ioctl(SIOCGIFFLAGS): %s: \"%s\".\n", strerror(errno),
			ifr.ifr_name);
		return -1;
	}

	if (enable)
		ifr.ifr_flags |= IFF_PROMISC;
	else
		ifr.ifr_flags &= ~(IFF_PROMISC);

	ret = ioctl(fd, SIOCSIFFLAGS, &ifr);
	if (ret < 0) {
		__odp_errno = errno;
		ODP_DBG("ioctl(SIOCSIFFLAGS): %s: \"%s\".\n", strerror(errno),
			ifr.ifr_name);
		return -1;
	}
	return 0;
}

/*
 * ODP_PACKET_SOCKET_MMSG:
 * ODP_PACKET_SOCKET_MMAP:
 * ODP_PACKET_NETMAP:
 */
int promisc_mode_get_fd(int fd, const char *name)
{
	struct ifreq ifr;
	int ret;

	snprintf(ifr.ifr_name, IF_NAMESIZE, "%s", name);
	ret = ioctl(fd, SIOCGIFFLAGS, &ifr);
	if (ret < 0) {
		__odp_errno = errno;
		ODP_DBG("ioctl(SIOCGIFFLAGS): %s: \"%s\".\n", strerror(errno),
			ifr.ifr_name);
		return -1;
	}

	return !!(ifr.ifr_flags & IFF_PROMISC);
}

int link_status_fd(int fd, const char *name)
{
	struct ifreq ifr;
	int ret;

	snprintf(ifr.ifr_name, IF_NAMESIZE, "%s", name);
	ret = ioctl(fd, SIOCGIFFLAGS, &ifr);
	if (ret < 0) {
		__odp_errno = errno;
		ODP_DBG("ioctl(SIOCGIFFLAGS): %s: \"%s\".\n", strerror(errno),
			ifr.ifr_name);
		return -1;
	}

	return !!(ifr.ifr_flags & IFF_RUNNING);
}

/**
 * Get enabled hash options of a packet socket
 *
 * @param fd              Socket file descriptor
 * @param name            Interface name
 * @param flow_type       Packet flow type
 * @param options[out]    Enabled hash options
 *
 * @retval 0 on success
 * @retval <0 on failure
 */
static inline int get_rss_hash_options(int fd, const char *name,
				       uint32_t flow_type, uint64_t *options)
{
	struct ifreq ifr;
	struct ethtool_rxnfc rsscmd;

	memset(&ifr, 0, sizeof(ifr));
	memset(&rsscmd, 0, sizeof(rsscmd));
	*options = 0;

	snprintf(ifr.ifr_name, IF_NAMESIZE, "%s", name);

	rsscmd.cmd = ETHTOOL_GRXFH;
	rsscmd.flow_type = flow_type;

	ifr.ifr_data = (caddr_t)&rsscmd;

	if (ioctl(fd, SIOCETHTOOL, &ifr) < 0)
		return -1;

	*options = rsscmd.data;
	return 0;
}

int rss_conf_get_fd(int fd, const char *name,
		    odp_pktin_hash_proto_t *hash_proto)
{
	uint64_t options;
	int rss_enabled = 0;

	memset(hash_proto, 0, sizeof(odp_pktin_hash_proto_t));

	get_rss_hash_options(fd, name, IPV4_FLOW, &options);
	if ((options & RXH_IP_SRC) && (options & RXH_IP_DST)) {
		hash_proto->proto.ipv4 = 1;
		rss_enabled++;
	}
	get_rss_hash_options(fd, name, TCP_V4_FLOW, &options);
	if ((options & RXH_IP_SRC) && (options & RXH_IP_DST) &&
	    (options & RXH_L4_B_0_1) && (options & RXH_L4_B_2_3)) {
		hash_proto->proto.ipv4_tcp = 1;
		rss_enabled++;
	}
	get_rss_hash_options(fd, name, UDP_V4_FLOW, &options);
	if ((options & RXH_IP_SRC) && (options & RXH_IP_DST) &&
	    (options & RXH_L4_B_0_1) && (options & RXH_L4_B_2_3)) {
		hash_proto->proto.ipv4_udp = 1;
		rss_enabled++;
	}
	get_rss_hash_options(fd, name, IPV6_FLOW, &options);
	if ((options & RXH_IP_SRC) && (options & RXH_IP_DST)) {
		hash_proto->proto.ipv6 = 1;
		rss_enabled++;
	}
	get_rss_hash_options(fd, name, TCP_V6_FLOW, &options);
	if ((options & RXH_IP_SRC) && (options & RXH_IP_DST) &&
	    (options & RXH_L4_B_0_1) && (options & RXH_L4_B_2_3)) {
		hash_proto->proto.ipv6_tcp = 1;
		rss_enabled++;
	}
	get_rss_hash_options(fd, name, UDP_V6_FLOW, &options);
	if ((options & RXH_IP_SRC) && (options & RXH_IP_DST) &&
	    (options & RXH_L4_B_0_1) && (options & RXH_L4_B_2_3)) {
		hash_proto->proto.ipv6_udp = 1;
		rss_enabled++;
	}
	return rss_enabled;
}

/**
 * Set hash options of a packet socket
 *
 * @param fd              Socket file descriptor
 * @param name            Interface name
 * @param flow_type       Packet flow type
 * @param options         Hash options
 *
 * @retval 0 on success
 * @retval <0 on failure
 */
static inline int set_rss_hash(int fd, const char *name,
			       uint32_t flow_type, uint64_t options)
{
	struct ifreq ifr;
	struct ethtool_rxnfc rsscmd;

	memset(&rsscmd, 0, sizeof(rsscmd));

	snprintf(ifr.ifr_name, IF_NAMESIZE, "%s", name);

	rsscmd.cmd = ETHTOOL_SRXFH;
	rsscmd.flow_type = flow_type;
	rsscmd.data = options;

	ifr.ifr_data = (caddr_t)&rsscmd;

	if (ioctl(fd, SIOCETHTOOL, &ifr) < 0)
		return -1;

	return 0;
}

int rss_conf_set_fd(int fd, const char *name,
		    const odp_pktin_hash_proto_t *hash_proto)
{
	uint64_t options;
	odp_pktin_hash_proto_t cur_hash;

	/* Compare to currently set hash protocols */
	rss_conf_get_fd(fd, name, &cur_hash);

	if (hash_proto->proto.ipv4_udp && !cur_hash.proto.ipv4_udp) {
		options = RXH_IP_SRC | RXH_IP_DST | RXH_L4_B_0_1 | RXH_L4_B_2_3;
		if (set_rss_hash(fd, name, UDP_V4_FLOW, options))
			return -1;
	}
	if (hash_proto->proto.ipv4_tcp && !cur_hash.proto.ipv4_tcp) {
		options = RXH_IP_SRC | RXH_IP_DST | RXH_L4_B_0_1 | RXH_L4_B_2_3;
		if (set_rss_hash(fd, name, TCP_V4_FLOW, options))
			return -1;
	}
	if (hash_proto->proto.ipv6_udp && !cur_hash.proto.ipv6_udp) {
		options = RXH_IP_SRC | RXH_IP_DST | RXH_L4_B_0_1 | RXH_L4_B_2_3;
		if (set_rss_hash(fd, name, UDP_V6_FLOW, options))
			return -1;
	}
	if (hash_proto->proto.ipv6_tcp && !cur_hash.proto.ipv6_tcp) {
		options = RXH_IP_SRC | RXH_IP_DST | RXH_L4_B_0_1 | RXH_L4_B_2_3;
		if (set_rss_hash(fd, name, TCP_V6_FLOW, options))
			return -1;
	}
	if (hash_proto->proto.ipv4 && !cur_hash.proto.ipv4) {
		options = RXH_IP_SRC | RXH_IP_DST;
		if (set_rss_hash(fd, name, IPV4_FLOW, options))
			return -1;
	}
	if (hash_proto->proto.ipv6 && !cur_hash.proto.ipv6) {
		options = RXH_IP_SRC | RXH_IP_DST;
		if (set_rss_hash(fd, name, IPV6_FLOW, options))
			return -1;
	}
	return 0;
}

int rss_conf_get_supported_fd(int fd, const char *name,
			      odp_pktin_hash_proto_t *hash_proto)
{
	uint64_t options;
	int rss_supported = 0;

	memset(hash_proto, 0, sizeof(odp_pktin_hash_proto_t));

	if (!get_rss_hash_options(fd, name, IPV4_FLOW, &options)) {
		if (!set_rss_hash(fd, name, IPV4_FLOW, options)) {
			hash_proto->proto.ipv4 = 1;
			rss_supported++;
		}
	}
	if (!get_rss_hash_options(fd, name, TCP_V4_FLOW, &options)) {
		if (!set_rss_hash(fd, name, TCP_V4_FLOW, options)) {
			hash_proto->proto.ipv4_tcp = 1;
			rss_supported++;
		}
	}
	if (!get_rss_hash_options(fd, name, UDP_V4_FLOW, &options)) {
		if (!set_rss_hash(fd, name, UDP_V4_FLOW, options)) {
			hash_proto->proto.ipv4_udp = 1;
			rss_supported++;
		}
	}
	if (!get_rss_hash_options(fd, name, IPV6_FLOW, &options)) {
		if (!set_rss_hash(fd, name, IPV6_FLOW, options)) {
			hash_proto->proto.ipv6 = 1;
			rss_supported++;
		}
	}
	if (!get_rss_hash_options(fd, name, TCP_V6_FLOW, &options)) {
		if (!set_rss_hash(fd, name, TCP_V6_FLOW, options)) {
			hash_proto->proto.ipv6_tcp = 1;
			rss_supported++;
		}
	}
	if (!get_rss_hash_options(fd, name, UDP_V6_FLOW, &options)) {
		if (!set_rss_hash(fd, name, UDP_V6_FLOW, options)) {
			hash_proto->proto.ipv6_udp = 1;
			rss_supported++;
		}
	}
	return rss_supported;
}

void rss_conf_print(const odp_pktin_hash_proto_t *hash_proto)
{	int max_len = 512;
	char str[max_len];
	int len = 0;
	int n = max_len - 1;

	len += snprintf(&str[len], n - len, " rss conf\n");

	if (hash_proto->proto.ipv4)
		len += snprintf(&str[len], n - len,
				"    IPV4\n");
	if (hash_proto->proto.ipv4_tcp)
		len += snprintf(&str[len], n - len,
				"    IPV4 TCP\n");
	if (hash_proto->proto.ipv4_udp)
		len += snprintf(&str[len], n - len,
				"    IPV4 UDP\n");
	if (hash_proto->proto.ipv6)
		len += snprintf(&str[len], n - len,
				"    IPV6\n");
	if (hash_proto->proto.ipv6_tcp)
		len += snprintf(&str[len], n - len,
				"    IPV6 TCP\n");
	if (hash_proto->proto.ipv6_udp)
		len += snprintf(&str[len], n - len,
				"    IPV6 UDP\n");
	str[len] = '\0';

	ODP_PRINT("%s\n", str);
}

/*
 * ODP_PACKET_SOCKET_MMSG:
 */
static int sock_close(pktio_entry_t *pktio_entry)
{
	pkt_sock_t *pkt_sock = &pktio_entry->s.pkt_sock;
	if (pkt_sock->sockfd != -1 && close(pkt_sock->sockfd) != 0) {
		__odp_errno = errno;
		ODP_ERR("close(sockfd): %s\n", strerror(errno));
		return -1;
	}

	return 0;
}

/*
 * ODP_PACKET_SOCKET_MMSG:
 */
static int sock_setup_pkt(pktio_entry_t *pktio_entry, const char *netdev,
			  odp_pool_t pool)
{
	int sockfd;
	int err;
	unsigned int if_idx;
	struct ifreq ethreq;
	struct sockaddr_ll sa_ll;
	char shm_name[ODP_SHM_NAME_LEN];
	pkt_sock_t *pkt_sock = &pktio_entry->s.pkt_sock;
	odp_pktio_stats_t cur_stats;

	/* Init pktio entry */
	memset(pkt_sock, 0, sizeof(*pkt_sock));
	/* set sockfd to -1, because a valid socked might be initialized to 0 */
	pkt_sock->sockfd = -1;

	if (pool == ODP_POOL_INVALID)
		return -1;
	pkt_sock->pool = pool;
	snprintf(shm_name, ODP_SHM_NAME_LEN, "%s-%s", "pktio", netdev);
	shm_name[ODP_SHM_NAME_LEN - 1] = '\0';

	sockfd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
	if (sockfd == -1) {
		__odp_errno = errno;
		ODP_ERR("socket(): %s\n", strerror(errno));
		goto error;
	}
	pkt_sock->sockfd = sockfd;

	/* get if index */
	memset(&ethreq, 0, sizeof(struct ifreq));
	snprintf(ethreq.ifr_name, IF_NAMESIZE, "%s", netdev);
	err = ioctl(sockfd, SIOCGIFINDEX, &ethreq);
	if (err != 0) {
		__odp_errno = errno;
		ODP_ERR("ioctl(SIOCGIFINDEX): %s: \"%s\".\n", strerror(errno),
			ethreq.ifr_name);
		goto error;
	}
	if_idx = ethreq.ifr_ifindex;

	err = mac_addr_get_fd(sockfd, netdev, pkt_sock->if_mac);
	if (err != 0)
		goto error;

	pkt_sock->mtu = mtu_get_fd(sockfd, netdev);
	if (!pkt_sock->mtu)
		goto error;

	/* bind socket to if */
	memset(&sa_ll, 0, sizeof(sa_ll));
	sa_ll.sll_family = AF_PACKET;
	sa_ll.sll_ifindex = if_idx;
	sa_ll.sll_protocol = htons(ETH_P_ALL);
	if (bind(sockfd, (struct sockaddr *)&sa_ll, sizeof(sa_ll)) < 0) {
		__odp_errno = errno;
		ODP_ERR("bind(to IF): %s\n", strerror(errno));
		goto error;
	}

	err = ethtool_stats_get_fd(pktio_entry->s.pkt_sock.sockfd,
				   pktio_entry->s.name,
				   &cur_stats);
	if (err != 0) {
		err = sysfs_stats(pktio_entry, &cur_stats);
		if (err != 0) {
			pktio_entry->s.stats_type = STATS_UNSUPPORTED;
			ODP_DBG("pktio: %s unsupported stats\n",
				pktio_entry->s.name);
		} else {
		pktio_entry->s.stats_type = STATS_SYSFS;
		}
	} else {
		pktio_entry->s.stats_type = STATS_ETHTOOL;
	}

	err = sock_stats_reset(pktio_entry);
	if (err != 0)
		goto error;

	return 0;

error:
	sock_close(pktio_entry);

	return -1;
}

/*
 * ODP_PACKET_SOCKET_MMSG:
 */
static int sock_mmsg_open(odp_pktio_t id ODP_UNUSED,
			  pktio_entry_t *pktio_entry,
			  const char *devname, odp_pool_t pool)
{
	if (disable_pktio)
		return -1;
	return sock_setup_pkt(pktio_entry, devname, pool);
}

static uint32_t _rx_pkt_to_iovec(odp_packet_t pkt,
				 struct iovec iovecs[MAX_SEGS])
{
	odp_packet_seg_t seg = odp_packet_first_seg(pkt);
	uint32_t seg_count = odp_packet_num_segs(pkt);
	uint32_t seg_id = 0;
	uint32_t iov_count = 0;
	uint8_t *ptr;
	uint32_t seglen;

	for (seg_id = 0; seg_id < seg_count; ++seg_id) {
		ptr    = odp_packet_seg_data(pkt, seg);
		seglen = odp_packet_seg_data_len(pkt, seg);

		if (ptr) {
			iovecs[iov_count].iov_base = ptr;
			iovecs[iov_count].iov_len = seglen;
			iov_count++;
		}
		seg = odp_packet_next_seg(pkt, seg);
	}

	return iov_count;
}

/*
 * ODP_PACKET_SOCKET_MMSG:
 */
static int sock_mmsg_recv(pktio_entry_t *pktio_entry, int index ODP_UNUSED,
			  odp_packet_t pkt_table[], int num)
{
	pkt_sock_t *pkt_sock = &pktio_entry->s.pkt_sock;
	odp_pool_t pool = pkt_sock->pool;
	odp_time_t ts_val;
	odp_time_t *ts = NULL;
	const int sockfd = pkt_sock->sockfd;
	struct mmsghdr msgvec[num];
	struct iovec iovecs[num][MAX_SEGS];
	int nb_rx = 0;
	int nb_pkts;
	int recv_msgs;
	int i;

	odp_ticketlock_lock(&pktio_entry->s.rxl);

	if (pktio_entry->s.config.pktin.bit.ts_all ||
	    pktio_entry->s.config.pktin.bit.ts_ptp)
		ts = &ts_val;

	memset(msgvec, 0, sizeof(msgvec));

	nb_pkts = packet_alloc_multi(pool, pkt_sock->mtu, pkt_table, num);
	for (i = 0; i < nb_pkts; i++) {
		msgvec[i].msg_hdr.msg_iovlen =
			_rx_pkt_to_iovec(pkt_table[i], iovecs[i]);
		msgvec[i].msg_hdr.msg_iov = iovecs[i];
	}

	recv_msgs = recvmmsg(sockfd, msgvec, nb_pkts, MSG_DONTWAIT, NULL);

	if (ts != NULL)
		ts_val = odp_time_global();

	for (i = 0; i < recv_msgs; i++) {
		void *base = msgvec[i].msg_hdr.msg_iov->iov_base;
		struct ethhdr *eth_hdr = base;
		odp_packet_t pkt = pkt_table[i];
		odp_packet_hdr_t *pkt_hdr = packet_hdr(pkt);
		uint16_t pkt_len = msgvec[i].msg_len;
		int ret;

		if (odp_unlikely(msgvec[i].msg_hdr.msg_flags & MSG_TRUNC)) {
			odp_packet_free(pkt);
			ODP_DBG("dropped truncated packet\n");
			continue;
		}
		if (pktio_cls_enabled(pktio_entry)) {
			uint16_t seg_len =  pkt_len;

			if (msgvec[i].msg_hdr.msg_iov->iov_len < pkt_len)
				seg_len = msgvec[i].msg_hdr.msg_iov->iov_len;

			if (cls_classify_packet(pktio_entry, base, pkt_len,
						seg_len, &pool, pkt_hdr,
						true)) {
				ODP_ERR("cls_classify_packet failed");
				odp_packet_free(pkt);
				continue;
			}
		}

		/* Don't receive packets sent by ourselves */
		if (odp_unlikely(ethaddrs_equal(pkt_sock->if_mac,
						eth_hdr->h_source))) {
			odp_packet_free(pkt);
			continue;
		}

		ret = odp_packet_trunc_tail(&pkt, odp_packet_len(pkt) - pkt_len,
					    NULL, NULL);
		if (ret < 0) {
			ODP_ERR("trunk_tail failed");
			odp_packet_free(pkt);
			continue;
		}

		pkt_hdr->input = pktio_entry->s.handle;

		if (!pktio_cls_enabled(pktio_entry))
			packet_parse_layer(pkt_hdr,
					   pktio_entry->s.config.parser.layer);

		pkt_hdr->input = pktio_entry->s.handle;
		packet_set_ts(pkt_hdr, ts);

		pkt_table[nb_rx++] = pkt;
	}

	/* Free unused pkt buffers */
	for (; i < nb_pkts; i++)
		odp_packet_free(pkt_table[i]);

	odp_ticketlock_unlock(&pktio_entry->s.rxl);

	return nb_rx;
}

static int sock_fd_set(pktio_entry_t *pktio_entry, int index ODP_UNUSED,
		       fd_set *readfds)
{
	pkt_sock_t *pkt_sock = &pktio_entry->s.pkt_sock;
	const int sockfd = pkt_sock->sockfd;

	FD_SET(sockfd, readfds);
	return sockfd;
}

static int sock_recv_tmo(pktio_entry_t *pktio_entry, int index,
			 odp_packet_t pkt_table[], int num, uint64_t usecs)
{
	struct timeval timeout;
	int ret;
	int maxfd;
	fd_set readfds;

	ret = sock_mmsg_recv(pktio_entry, index, pkt_table, num);
	if (ret != 0)
		return ret;

	timeout.tv_sec = usecs / (1000 * 1000);
	timeout.tv_usec = usecs - timeout.tv_sec * (1000ULL * 1000ULL);

	FD_ZERO(&readfds);
	maxfd = sock_fd_set(pktio_entry, index, &readfds);

	if (select(maxfd + 1, &readfds, NULL, NULL, &timeout) == 0)
		return 0;

	return sock_mmsg_recv(pktio_entry, index, pkt_table, num);
}

static int sock_recv_mq_tmo(pktio_entry_t *pktio_entry[], int index[],
			    int num_q, odp_packet_t pkt_table[], int num,
			    unsigned *from, uint64_t usecs)
{
	struct timeval timeout;
	int i;
	int ret;
	int maxfd = -1, maxfd2;
	fd_set readfds;

	for (i = 0; i < num_q; i++) {
		ret = sock_mmsg_recv(pktio_entry[i], index[i], pkt_table, num);

		if (ret > 0 && from)
			*from = i;

		if (ret != 0)
			return ret;
	}

	timeout.tv_sec = usecs / (1000 * 1000);
	timeout.tv_usec = usecs - timeout.tv_sec * (1000ULL * 1000ULL);

	FD_ZERO(&readfds);

	for (i = 0; i < num_q; i++) {
		maxfd2 = sock_fd_set(pktio_entry[i], index[i], &readfds);
		if (maxfd2 > maxfd)
			maxfd = maxfd2;
	}

	if (select(maxfd + 1, &readfds, NULL, NULL, &timeout) == 0)
		return 0;

	for (i = 0; i < num_q; i++) {
		ret = sock_mmsg_recv(pktio_entry[i], index[i], pkt_table, num);

		if (ret > 0 && from)
			*from = i;

		if (ret != 0)
			return ret;
	}

	return 0;
}

static uint32_t _tx_pkt_to_iovec(odp_packet_t pkt,
				 struct iovec iovecs[MAX_SEGS])
{
	uint32_t pkt_len = odp_packet_len(pkt);
	uint32_t offset = 0;
	uint32_t iov_count = 0;

	while (offset < pkt_len) {
		uint32_t seglen;

		iovecs[iov_count].iov_base = odp_packet_offset(pkt, offset,
				&seglen, NULL);
		iovecs[iov_count].iov_len = seglen;
		iov_count++;
		offset += seglen;
	}
	return iov_count;
}

/*
 * ODP_PACKET_SOCKET_MMSG:
 */
static int sock_mmsg_send(pktio_entry_t *pktio_entry, int index ODP_UNUSED,
			  const odp_packet_t pkt_table[], int num)
{
	pkt_sock_t *pkt_sock = &pktio_entry->s.pkt_sock;
	struct mmsghdr msgvec[num];
	struct iovec iovecs[num][MAX_SEGS];
	int ret;
	int sockfd;
	int n, i;

	odp_ticketlock_lock(&pktio_entry->s.txl);

	sockfd = pkt_sock->sockfd;
	memset(msgvec, 0, sizeof(msgvec));

	for (i = 0; i < num; i++) {
		msgvec[i].msg_hdr.msg_iov = iovecs[i];
		msgvec[i].msg_hdr.msg_iovlen = _tx_pkt_to_iovec(pkt_table[i],
				iovecs[i]);
	}

	for (i = 0; i < num; ) {
		ret = sendmmsg(sockfd, &msgvec[i], num - i, MSG_DONTWAIT);
		if (odp_unlikely(ret <= -1)) {
			if (i == 0 && SOCK_ERR_REPORT(errno)) {
				__odp_errno = errno;
				ODP_ERR("sendmmsg(): %s\n", strerror(errno));
				odp_ticketlock_unlock(&pktio_entry->s.txl);
				return -1;
			}
			break;
		}

		i += ret;
	}

	odp_ticketlock_unlock(&pktio_entry->s.txl);

	for (n = 0; n < i; ++n)
		odp_packet_free(pkt_table[n]);

	return i;
}

/*
 * ODP_PACKET_SOCKET_MMSG:
 */
static uint32_t sock_mtu_get(pktio_entry_t *pktio_entry)
{
	return pktio_entry->s.pkt_sock.mtu;
}

/*
 * ODP_PACKET_SOCKET_MMSG:
 */
static int sock_mac_addr_get(pktio_entry_t *pktio_entry,
			     void *mac_addr)
{
	memcpy(mac_addr, pktio_entry->s.pkt_sock.if_mac, ETH_ALEN);
	return ETH_ALEN;
}

/*
 * ODP_PACKET_SOCKET_MMSG:
 */
static int sock_promisc_mode_set(pktio_entry_t *pktio_entry,
				 odp_bool_t enable)
{
	return promisc_mode_set_fd(pktio_entry->s.pkt_sock.sockfd,
				   pktio_entry->s.name, enable);
}

/*
 * ODP_PACKET_SOCKET_MMSG:
 */
static int sock_promisc_mode_get(pktio_entry_t *pktio_entry)
{
	return promisc_mode_get_fd(pktio_entry->s.pkt_sock.sockfd,
				   pktio_entry->s.name);
}

static int sock_link_status(pktio_entry_t *pktio_entry)
{
	return link_status_fd(pktio_entry->s.pkt_sock.sockfd,
			      pktio_entry->s.name);
}

static int sock_capability(pktio_entry_t *pktio_entry ODP_UNUSED,
			   odp_pktio_capability_t *capa)
{
	memset(capa, 0, sizeof(odp_pktio_capability_t));

	capa->max_input_queues  = 1;
	capa->max_output_queues = 1;
	capa->set_op.op.promisc_mode = 1;

	odp_pktio_config_init(&capa->config);
	capa->config.pktin.bit.ts_all = 1;
	capa->config.pktin.bit.ts_ptp = 1;
	return 0;
}

static int sock_stats(pktio_entry_t *pktio_entry,
		      odp_pktio_stats_t *stats)
{
	if (pktio_entry->s.stats_type == STATS_UNSUPPORTED) {
		memset(stats, 0, sizeof(*stats));
		return 0;
	}

	return sock_stats_fd(pktio_entry,
			     stats,
			     pktio_entry->s.pkt_sock.sockfd);
}

static int sock_stats_reset(pktio_entry_t *pktio_entry)
{
	if (pktio_entry->s.stats_type == STATS_UNSUPPORTED) {
		memset(&pktio_entry->s.stats, 0,
		       sizeof(odp_pktio_stats_t));
		return 0;
	}

	return sock_stats_reset_fd(pktio_entry,
				   pktio_entry->s.pkt_sock.sockfd);
}

static int sock_init_global(void)
{
	if (getenv("ODP_PKTIO_DISABLE_SOCKET_MMSG")) {
		ODP_PRINT("PKTIO: socket mmsg skipped,"
			  " enabled export ODP_PKTIO_DISABLE_SOCKET_MMSG=1.\n");
		disable_pktio = 1;
	} else {
		ODP_PRINT("PKTIO: initialized socket mmsg,"
			  "use export ODP_PKTIO_DISABLE_SOCKET_MMSG=1 to disable.\n");
	}
	return 0;
}

const pktio_if_ops_t sock_mmsg_pktio_ops = {
	.name = "socket",
	.print = NULL,
	.init_global = sock_init_global,
	.init_local = NULL,
	.term = NULL,
	.open = sock_mmsg_open,
	.close = sock_close,
	.start = NULL,
	.stop = NULL,
	.stats = sock_stats,
	.stats_reset = sock_stats_reset,
	.recv = sock_mmsg_recv,
	.recv_tmo = sock_recv_tmo,
	.recv_mq_tmo = sock_recv_mq_tmo,
	.fd_set = sock_fd_set,
	.send = sock_mmsg_send,
	.mtu_get = sock_mtu_get,
	.promisc_mode_set = sock_promisc_mode_set,
	.promisc_mode_get = sock_promisc_mode_get,
	.mac_get = sock_mac_addr_get,
	.mac_set = NULL,
	.link_status = sock_link_status,
	.capability = sock_capability,
	.pktin_ts_res = NULL,
	.pktin_ts_from_ns = NULL,
	.config = NULL,
	.input_queues_config = NULL,
	.output_queues_config = NULL,
};
