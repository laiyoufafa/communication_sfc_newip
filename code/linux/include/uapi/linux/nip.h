/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * Copyright (c) 2022 Huawei Device Co., Ltd.
 *
 * Based on include/uapi/linux/ipv6.h
 * Based on include/uapi/linux/in6.h
 */
#ifndef _UAPI_NEWIP_H
#define _UAPI_NEWIP_H

#include <asm/byteorder.h>
#include <linux/libc-compat.h>
#include <linux/types.h>
#include "nip_addr.h"
#include <linux/if.h>

struct nip_ifreq {
	struct nip_addr ifrn_addr;
	int ifrn_ifindex;
};

/* The following structure must be larger than V4. System calls use V4.
 * If the definition is smaller than V4, the read process will have memory overruns
 * v4: include\linux\socket.h --> sockaddr (16Byte)
 */
#define POD_SOCKADDR_SIZE 3
#pragma pack(1)
struct sockaddr_nin {
	unsigned short sin_family; /* [2Byte] AF_NINET */
	unsigned short sin_port;   /* [2Byte] Transport layer port, big-endian */
	struct nip_addr sin_addr;  /* [9Byte] NIP address */

	unsigned char sin_zero[POD_SOCKADDR_SIZE]; /* [3Byte] Byte alignment */
};
#pragma pack()

struct nip_devreq {
	char	nip_ifr_name[IFNAMSIZ];	/* if name, e.g. "eth0", "wlan0" */

	union {
		struct sockaddr_nin addr;
		short flags;
	} devreq;
};

#define nip_dev_addr devreq.addr	/* nip address */
#define nip_dev_flags devreq.flags	/* net device flags */

#endif /*_UAPI_NEWIP_H*/
