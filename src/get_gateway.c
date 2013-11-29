/*
 * ZMap Copyright 2013 Regents of the University of Michigan 
 * 
 * Licensed under the Apache License, Version 2.0 (the "License"); you may not
 * use this file except in compliance with the License. You may obtain a copy
 * of the License at http://www.apache.org/licenses/LICENSE-2.0
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <assert.h>

#include "../lib/includes.h"
#include "../lib/logger.h"


int get_hw_addr(struct in_addr *gw_ip, char *iface, unsigned char *hw_mac)
{
	int rc = 0;
	arp_t *arp;
	struct arp_entry entry;

	if (!gw_ip || !hw_mac) {
		return -1;
	}

	if ((arp = arp_open()) == NULL) {
		log_fatal("get_hw_addr", "failed to open arp table");
		return -1;
	}
	
	// Convert gateway ip to dnet struct format
	memset(&entry, 0, sizeof(struct arp_entry));
	entry.arp_pa.addr_type = ADDR_TYPE_IP;
    entry.arp_pa.addr_bits = IP_ADDR_BITS;
    entry.arp_pa.addr_ip = gw_ip->s_addr;
	
	if (arp_get(arp, &entry) < 0) {
		log_fatal("get_hw_addr", "failed to fetch arp entry");
		rc = -1;
		goto cleanup;
	} else {
		log_debug("get_hw_addr", "found ip %s at hw_addr %s", addr_ntoa(&entry.arp_pa),
			addr_ntoa(&entry.arp_ha));
		memcpy(hw_mac, &entry.arp_ha, 6);
	}

cleanup:
	arp_close(arp);
	return rc;
}


#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__)

int get_iface_ip(char *iface, struct in_addr *ip)
{
    assert(iface);
    struct ifaddrs *ifaddr, *ifa;
    if (getifaddrs(&ifaddr)) {
        log_fatal("get-iface-ip", "unable able to retrieve list of network interfaces: %s",
                        strerror(errno));
    }   
    for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == NULL || ifa->ifa_addr->sa_family != AF_INET) {
            continue;
        }   
        if (!strcmp(iface, ifa->ifa_name)) {
            struct sockaddr_in *sin = (struct sockaddr_in *)ifa->ifa_addr;
            ip->s_addr = sin->sin_addr.s_addr;
            log_debug("get-iface-ip", "ip address found for %s: %s",
                            iface, inet_ntoa(*ip));
            return EXIT_SUCCESS;
        }   
    }   
    log_fatal("get-iface-ip", "specified interface does not"
                    " exist or have an IPv4 address"); 
    return EXIT_FAILURE;
}

int get_default_gw(struct in_addr *gw, char *iface)
{
	log_warn("get-default-gw", "not yet implemented on bsd");
	return EXIT_FAILURE;
}	


#else // (linux)

#include <sys/ioctl.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <arpa/inet.h>


int read_nl_sock(int sock, char *buf, int buf_len)
{
	int msg_len = 0;
	char *pbuf = buf;
	do {
		int len = recv(sock, pbuf, buf_len - msg_len, 0);
		if (len <= 0) {
			log_debug("get-gw", "recv failed: %s", strerror(errno));
			return -1;
		}
		struct nlmsghdr *nlhdr = (struct nlmsghdr *)pbuf;
		if (NLMSG_OK(nlhdr, ((unsigned int)len)) == 0 || 
						nlhdr->nlmsg_type == NLMSG_ERROR) {
			log_debug("get-gw", "recv failed: %s", strerror(errno));
			return -1;
		}
		if (nlhdr->nlmsg_type == NLMSG_DONE) {
			break;
		} else {
			msg_len += len;
			pbuf += len;
		} 
		if ((nlhdr->nlmsg_flags & NLM_F_MULTI) == 0) {
			break;
		}
	} while (1);
	return msg_len;
}

int send_nl_req(uint16_t msg_type, uint32_t seq,
				void *payload, uint32_t payload_len)
{
	int sock = socket(PF_NETLINK, SOCK_DGRAM, NETLINK_ROUTE);
	if (sock < 0) {
		log_error("get-gw", "unable to get socket: %s", strerror(errno));
		return -1;
	}
	if (NLMSG_SPACE(payload_len) < payload_len) {
		// Integer overflow
		return -1;
	}
	struct nlmsghdr *nlmsg;
	nlmsg = malloc(NLMSG_SPACE(payload_len));
	if (!nlmsg) {
		return -1;
	}

	memset(nlmsg, 0, NLMSG_SPACE(payload_len));
	memcpy(NLMSG_DATA(nlmsg), payload, payload_len);
	nlmsg->nlmsg_type = msg_type;
	nlmsg->nlmsg_len = NLMSG_LENGTH(payload_len);
	nlmsg->nlmsg_flags = NLM_F_DUMP | NLM_F_REQUEST;
	nlmsg->nlmsg_seq = seq;
	nlmsg->nlmsg_pid = getpid();

	if (send(sock, nlmsg, nlmsg->nlmsg_len, 0) < 0) {
		log_error("get-gw", "failure sending: %s", strerror(errno));
		return -1;
	}
	free(nlmsg);
	return sock;
}

// gw and iface[IF_NAMESIZE] MUST be allocated
int get_default_gw(struct in_addr *gw, char *iface)
{
	struct rtmsg req;
	unsigned int nl_len;
	char buf[8192];
	struct nlmsghdr *nlhdr;

	if (!gw || !iface) {
		return -1;
	}

	// Send RTM_GETROUTE request
	memset(&req, 0, sizeof(req));
	int sock = send_nl_req(RTM_GETROUTE, 0, &req, sizeof(req));

	// Read responses
	nl_len = read_nl_sock(sock, buf, sizeof(buf));
	if (nl_len <= 0) {
		return -1;
	}

	// Parse responses
	nlhdr = (struct nlmsghdr *)buf;
	while (NLMSG_OK(nlhdr, nl_len)) {
		struct rtattr *rt_attr;
		struct rtmsg *rt_msg;
		int rt_len;
		int has_gw = 0;

		rt_msg = (struct rtmsg *) NLMSG_DATA(nlhdr);

		if ((rt_msg->rtm_family != AF_INET) || (rt_msg->rtm_table != RT_TABLE_MAIN)) {
			return -1;
		}

		rt_attr = (struct rtattr *) RTM_RTA(rt_msg);
		rt_len = RTM_PAYLOAD(nlhdr);
		while (RTA_OK(rt_attr, rt_len)) {
			switch (rt_attr->rta_type) {
			case RTA_OIF:
				if_indextoname(*(int *) RTA_DATA(rt_attr), iface); 
				break;
			case RTA_GATEWAY:
				gw->s_addr = *(unsigned int *) RTA_DATA(rt_attr); 
				has_gw = 1;
				break;
			}
			rt_attr = RTA_NEXT(rt_attr, rt_len);
		}
	
		if (has_gw) {
			return 0;
		}
		nlhdr = NLMSG_NEXT(nlhdr, nl_len);	
	}
	return -1;
}

int get_iface_ip(char *iface, struct in_addr *ip)
{
	int sock;
	struct ifreq ifr;
	sock = socket(AF_INET, SOCK_DGRAM, 0);
	if (sock < 0) {
		log_fatal("get-iface-ip", "failure opening socket: %s", strerror(errno));
	}
	ifr.ifr_addr.sa_family = AF_INET;
	strncpy(ifr.ifr_name, iface, IFNAMSIZ-1);

	if (ioctl(sock, SIOCGIFADDR, &ifr) < 0) {
		log_fatal("get-iface-ip", "ioctl failure: %s", strerror(errno));
		close(sock);
	}
	ip->s_addr =  &((struct sockaddr_in*)&ifr.ifr_addr)->sin_addr.s_addr;
	close(sock);
	return EXIT_SUCCESS;
}

#endif // end linux
