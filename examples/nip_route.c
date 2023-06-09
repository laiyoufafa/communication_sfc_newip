// SPDX-License-Identifier: BSD-3-Clause
/*
 * Copyright (c) 2022 Huawei Device Co., Ltd.
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above
 *    copyright notice, this list of conditions and the following
 *    disclaimer.
 * 2. Redistributions in binary form must reproduce the above
 *    copyright notice, this list of conditions and the following
 *    disclaimer in the documentation and/or other materials
 *    provided with the distribution.
 * 3. Neither the name of the copyright holder nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <stdint.h>
#include <linux/route.h>

#include "nip_uapi.h"
#include "nip_lib.h"
#include "newip_route.h"

/* get ifindex based on the device name
 * struct ifreq ifr;
 * struct nip_ifreq ifrn;
 * ioctl(fd, SIOGIFINDEX, &ifr);
 * ifr.ifr_ifindex; ===> ifindex
 */
int nip_route_add(int ifindex, struct nip_addr *dst_addr, struct nip_addr *gateway_addr,
		  uint8_t gateway_flag, int opt)
{
	int fd, ret;
	struct nip_rtmsg rt;

	fd = socket(AF_NINET, SOCK_DGRAM, 0);
	if (fd < 0)
		return -1;

	memset(&rt, 0, sizeof(rt));
	rt.rtmsg_ifindex = ifindex;
	rt.rtmsg_flags = RTF_UP;
	rt.rtmsg_dst = *dst_addr;

	if (gateway_flag) {
		rt.rtmsg_gateway = *gateway_addr;
		rt.rtmsg_flags |= RTF_GATEWAY;
	}

	ret = ioctl(fd, opt, &rt);
	if (ret < 0 && errno != EEXIST) { // ignore File Exists error
		close(fd);
		return -1;
	}

	close(fd);
	return 0;
}

void cmd_help(void)
{
	/* nip_route add 02   wlan0    (配置目的地址02设备路由，出口是wlan0)
	 * nip_route add 02   wlan0 03 (配置目的地址02设备路由，出口是wlan0，网关地址是03)
	 * nip_route add ff09 wlan0 03 (配置广播默认路由，      出口是wlan0，网关地址是03)
	 */
	printf("\n[cmd example]\n");
	printf("nip_route { add | del } <dst-addr> <netcard-name>\n");
	printf("nip_route { add | del } <dst-addr> <netcard-name> <gateway-addr>\n");
}

int main(int argc, char **argv_input)
{
	int ret;
	int opt;
	int ifindex = 0;
	uint8_t dst_addr_len;
	uint8_t gateway_addr_len;
	uint8_t gateway_flag = 0;
	char **argv = argv_input;
	char cmd[ARRAY_LEN];
	char dev[ARRAY_LEN];
	struct nip_addr dst_addr = {0};
	struct nip_addr gateway_addr = {0};

	if (argc != DEMO_INPUT_3 && argc != DEMO_INPUT_4) {
		printf("unsupport route cfg input, argc=%u.\n", argc);
		cmd_help();
		return -1;
	}

	/* 配置参数1解析: { add | del } */
	argv++;
	memset(cmd, 0, ARRAY_LEN);
	ret = sscanf(*argv, "%s", cmd);
	if (!strncmp(cmd, "add", NAME_WLAN_LEN)) {
		opt = SIOCADDRT;
	} else if (!strncmp(cmd, "del", NAME_WLAN_LEN)) {
		opt = SIOCDELRT;
	} else {
		printf("unsupport route cfg cmd-1, cmd=%s.\n", cmd);
		cmd_help();
		return -1;
	}

	/* 配置参数2解析: <dst-addr> */
	argv++;
	if (nip_get_addr(argv, &dst_addr)) {
		printf("unsupport route cfg cmd-2.\n");
		cmd_help();
		return 1;
	}

	/* 配置参数3解析: <netcard-name> */
	argv++;
	memset(dev, 0, ARRAY_LEN);
	ret = sscanf(*argv, "%s", dev);
	if (strncmp(dev, "wlan", NAME_WLAN_LEN)) {
		printf("unsupport route cfg cmd-3, cmd=%s.\n", dev);
		cmd_help();
		return -1;
	}
	ret = nip_get_ifindex(dev, &ifindex);
	if (ret != 0)
		return -1;

	/* 配置参数4解析: <gateway-addr> */
	if (argc == DEMO_INPUT_4) {
		argv++;
		if (nip_get_addr(argv, &gateway_addr)) {
			printf("unsupport route cfg cmd-4.\n");
			cmd_help();
			return 1;
		}
		gateway_flag = 1;
	}

	ret = nip_get_ifindex(dev, &ifindex);
	if (ret != 0) {
		printf("get %s ifindex fail, ret=%d.\n", dev, ret);
		return -1;
	}

	ret = nip_route_add(ifindex, &dst_addr, &gateway_addr, gateway_flag, opt);
	if (ret != 0) {
		printf("get %s ifindex fail, ret=%d.\n", dev, ret);
		return -1;
	}

	printf("%s (ifindex=%u) cfg route success.\n", dev, ifindex);
	return 0;
}

