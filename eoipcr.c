#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <net/if_arp.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <linux/ip.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/if_tunnel.h>

#include "libnetlink.h"

int print_link(const struct sockaddr_nl *who, struct nlmsghdr *n, void *arg) {
	struct ndmsg *r = NLMSG_DATA(n);
	int len = n->nlmsg_len;
	struct rtattr *ifattr[IFLA_MAX+1];
	struct rtattr *ifinfo[IFLA_INFO_MAX+1];
	struct rtattr *ifgreo[IFLA_GRE_MAX+1];
	const char *ifname;
	const char *kind;
	int link=0;
	int tunid=0;
	struct in_addr sip,dip;
	uint8_t ttl=0,tos=0;
	struct ifinfomsg *ifi;

	sip.s_addr=dip.s_addr=htonl(0);

	if (n->nlmsg_type != RTM_NEWLINK && n->nlmsg_type != RTM_DELLINK) {
		fprintf(stderr, "Not RTM_xxxLINK: %08x %08x %08x\n", n->nlmsg_len, n->nlmsg_type, n->nlmsg_flags);
		return 0;
	}
	len -= NLMSG_LENGTH(sizeof(*r));
	if (len < 0) {
		fprintf(stderr, "BUG: wrong nlmsg len %d\n", len);
		return -1;
	}

	ifi=(void *)r;

	parse_rtattr(ifattr, IFLA_MAX, IFLA_RTA(r), n->nlmsg_len - NLMSG_LENGTH(sizeof(*r)));
	if (ifattr[IFLA_IFNAME])
		ifname=rta_getattr_str(ifattr[IFLA_IFNAME]);
	else
		ifname="";
	if (ifattr[IFLA_LINKINFO])
		parse_rtattr(ifinfo, IFLA_INFO_MAX, (void *)rta_getattr_str(ifattr[IFLA_LINKINFO]), ifattr[IFLA_LINKINFO]->rta_len);
	else
		memset(ifinfo,0,sizeof ifinfo);
	if (ifinfo[IFLA_INFO_KIND])
		kind=rta_getattr_str(ifinfo[IFLA_INFO_KIND]);
	else
		kind="";
	if (!strcmp(kind,"eoip") && ifinfo[IFLA_INFO_DATA]) {
		char ts[IFNAMSIZ],td[IFNAMSIZ];

		parse_rtattr(ifgreo, IFLA_GRE_MAX, (void *)rta_getattr_str(ifinfo[IFLA_INFO_DATA]), ifinfo[IFLA_INFO_DATA]->rta_len);
		if (ifgreo[IFLA_GRE_LINK])
			link=rta_getattr_u32(ifgreo[IFLA_GRE_LINK]);
		if (ifgreo[IFLA_GRE_TOS])
			tos=rta_getattr_u8(ifgreo[IFLA_GRE_TOS]);
		if (ifgreo[IFLA_GRE_TTL])
			ttl=rta_getattr_u8(ifgreo[IFLA_GRE_TTL]);
		if (ifgreo[IFLA_GRE_LOCAL])
			sip.s_addr=rta_getattr_u32(ifgreo[IFLA_GRE_LOCAL]);
		if (ifgreo[IFLA_GRE_REMOTE])
			dip.s_addr=rta_getattr_u32(ifgreo[IFLA_GRE_REMOTE]);
		if (ifgreo[IFLA_GRE_IKEY])
			tunid=rta_getattr_u32(ifgreo[IFLA_GRE_IKEY]);
		strcpy(ts,inet_ntoa(sip));
		strcpy(td,inet_ntoa(dip));
		printf("%d: %s@%d: link/%s %s remote %s tunnel-id %d ttl %d tos %d\n",ifi->ifi_index,ifname,link,kind,ts,td,tunid,ttl,tos);
	}

	return 0;
}

void list(void) {
	struct rtnl_handle rth;

	if (rtnl_open(&rth,0)) {
		perror("Cannot open rtnetlink");
		return;
	}
	if (rtnl_wilddump_request(&rth, AF_UNSPEC, RTM_GETLINK) < 0) {
		perror("Cannot send dump request");
		return;
	}

	if (rtnl_dump_filter(&rth, print_link, NULL) < 0) {
		fprintf(stderr, "Dump terminated\n");
		return;
	}
	rtnl_close(&rth);
}

int rtnetlink_request(struct nlmsghdr *msg, int buflen, struct sockaddr_nl *adr) {
	int rsk;
	int n;

	/* Use a private socket to avoid having to keep state for a sequence number. */
	rsk = socket(PF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
	if (rsk < 0)
		return -1;
	n = sendto(rsk, msg, msg->nlmsg_len, 0, (struct sockaddr *)adr, sizeof(struct sockaddr_nl));
	if (errno)
		perror("in send");
	close(rsk);
	if (n < 0)
		return -1;
	return 0;
}

int eoip_add(char *name,uint32_t tunnelid,uint32_t sip,uint32_t dip) {
	struct {
		struct nlmsghdr msg;
		struct ifinfomsg ifi;
		struct rtattr a_name;
		char ifname[IFNAMSIZ];
		struct rtattr a_lnfo;
		struct rtattr a_kind;
		char kind[8];
		struct rtattr a_data;
		struct rtattr a_ikey;
		uint32_t ikey;
		struct rtattr a_sa;
		uint32_t sa;
		struct rtattr a_da;
		uint32_t da;
		struct rtattr a_link;
		uint32_t link;
		struct rtattr a_ttl;
		uint8_t ttl;
		uint8_t ttlpad[3];
		struct rtattr a_tos;
		uint8_t tos;
		uint8_t tospad[3];
		uint32_t dummy[50];
	} req = {
		.msg = {
			.nlmsg_len = 0, // fix me later
			.nlmsg_type = RTM_NEWLINK,
			.nlmsg_flags = NLM_F_REQUEST|NLM_F_CREATE,
		},
		.ifi = {
			.ifi_family = AF_UNSPEC,
			.ifi_index = 0,
		},
		.a_name = {
			.rta_len = IFNAMSIZ + sizeof(struct rtattr),
			.rta_type = IFLA_IFNAME,
		},
		.ifname="",
		.a_lnfo = {
			.rta_len = 0, // fix me later
			.rta_type = IFLA_LINKINFO,
		},
		.a_kind = {
			.rta_len = 8 + sizeof(struct rtattr),
			.rta_type = IFLA_INFO_KIND,
		},
		.kind="eoip",
		.a_data = {
			.rta_len = 0, // fix me later
			.rta_type = IFLA_INFO_DATA,
		},
		.a_ikey = {
			.rta_len = 4 + sizeof(struct rtattr),
			.rta_type = IFLA_GRE_IKEY,
		},
		.ikey=4321,
		.a_sa = {
			.rta_len = 4 + sizeof(struct rtattr),
			.rta_type = IFLA_GRE_LOCAL,
		},
		.sa=htonl(0),
		.a_da = {
			.rta_len = 4 + sizeof(struct rtattr),
			.rta_type = IFLA_GRE_REMOTE,
		},
		.da=htonl(0),
		.a_link = {
			.rta_len = 4 + sizeof(struct rtattr),
			.rta_type = IFLA_GRE_LINK,
		},
		.link=0,
		.a_ttl = {
			.rta_len = 4 + sizeof(struct rtattr),
			.rta_type = IFLA_GRE_TTL,
		},
		.ttl=0,
		.a_tos = {
			.rta_len = 4 + sizeof(struct rtattr),
			.rta_type = IFLA_GRE_TOS,
		},
		.tos=0,
	};
	struct sockaddr_nl adr = {
		.nl_family = AF_NETLINK,
	};

	req.msg.nlmsg_len=NLMSG_LENGTH((char *)&req.dummy-(char *)&req);

	req.a_name.rta_len=(char *)&req.a_lnfo-(char *)&req.a_name;
	req.a_lnfo.rta_len=(char *)&req.dummy-(char *)&req.a_lnfo;
	req.a_kind.rta_len=(char *)&req.a_data-(char *)&req.a_kind;
	req.a_data.rta_len=(char *)&req.dummy-(char *)&req.a_data;
	req.a_ikey.rta_len=(char *)&req.a_sa-(char *)&req.a_ikey;
	req.a_sa.rta_len=(char *)&req.a_da-(char *)&req.a_sa;
	req.a_da.rta_len=(char *)&req.a_link-(char *)&req.a_da;
	req.a_link.rta_len=(char *)&req.a_ttl-(char *)&req.a_link;
	req.a_ttl.rta_len=(char *)&req.a_tos-(char *)&req.a_ttl;
	req.a_tos.rta_len=(char *)&req.dummy-(char *)&req.a_tos;

	req.sa=sip;
	req.da=dip;
	strcpy(req.ifname,name);
	req.ikey=tunnelid;
	if (0) {
		unsigned char *p=(unsigned char *)&req;
		int l=(char *)&req.dummy-(char *)&req;
		int i;

		printf("req size: %d\n",req.msg.nlmsg_len);
		printf("name size: %d\n",req.a_name.rta_len);
		printf("lnfo size: %d\n",req.a_lnfo.rta_len);
		printf("kind size: %d\n",req.a_kind.rta_len);
		printf("data size: %d\n",req.a_data.rta_len);
		printf("ikey size: %d\n",req.a_ikey.rta_len);
		printf("sadr size: %d\n",req.a_sa.rta_len);
		printf("dadr size: %d\n",req.a_da.rta_len);

		printf("packet size: %d, data dump:",l);

		for (i=0;i<l;i++)
			printf("%s%02x",(!(i%16))?"\n":" ",p[i]);
		fflush(stdout);
	}

	if (rtnetlink_request(&req.msg, sizeof req, &adr) < 0) {
		perror("error in netlink request");
		return -1;
	}
	return 0;
}

int main(int argc,char **argv) {
	if (!((argc == 5) || (argc == 2 && !strcmp("list",argv[1])))) {
		printf("usage:\n\t%s name tunnel-id source-ip dest-ip\n\t%s list\n",argv[0],argv[0]);
		return 0;
	}
	if (argc == 2 && !strcmp("list",argv[1])) { // list interfaces
		list();
		return 0;
	}
	if (argc == 5) { // add interface
		struct in_addr sipa,dipa;

		if (!inet_aton(argv[3],&sipa)) {
			printf("%s is not a valid ip\n",argv[3]);
			return 0;
		}
		if (!inet_aton(argv[4],&dipa)) {
			printf("%s is not a valid ip\n",argv[4]);
			return 0;
		}
		// tunnel id is in host byte order, addresses are in net byte order
		eoip_add(argv[1],atoi(argv[2]),sipa.s_addr,dipa.s_addr);
		return 0;
	}
	return 0;
}
