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

static int eoip_add(int excl,char *name,uint16_t tunnelid,uint32_t sip,uint32_t dip,uint32_t link,uint8_t ttl,uint8_t tos) {
	struct {
		struct nlmsghdr msg;
		struct ifinfomsg ifi;
		uint8_t buf[1024];
	} req;
	struct rtnl_handle rth = { .fd = -1 };
	struct rtattr *lnfo;
	struct rtattr *data;

	memset(&req, 0, sizeof(req));
	req.msg.nlmsg_type = RTM_NEWLINK;
	req.msg.nlmsg_len = NLMSG_LENGTH(sizeof(struct ifinfomsg));
	req.msg.nlmsg_flags = NLM_F_REQUEST | NLM_F_CREATE | (excl ? NLM_F_EXCL : 0);
	req.ifi.ifi_family = AF_UNSPEC;

	addattr_l(&req.msg, sizeof(req), IFLA_IFNAME, name, strlen(name));
	lnfo = NLMSG_TAIL(&req.msg);
	addattr_l(&req.msg, sizeof(req), IFLA_LINKINFO, NULL, 0);
	addattr_l(&req.msg, sizeof(req), IFLA_INFO_KIND, "eoip", strlen("eoip"));
	data = NLMSG_TAIL(&req.msg);
	addattr_l(&req.msg, sizeof(req), IFLA_INFO_DATA, NULL, 0);

	addattr32(&req.msg, sizeof(req), IFLA_GRE_IKEY, tunnelid);
	addattr32(&req.msg, sizeof(req), IFLA_GRE_LOCAL, sip);
	addattr32(&req.msg, sizeof(req), IFLA_GRE_REMOTE, dip);
	addattr32(&req.msg, sizeof(req), IFLA_GRE_LINK, link);
	addattr8(&req.msg, sizeof(req), IFLA_GRE_TTL, ttl);
	addattr8(&req.msg, sizeof(req), IFLA_GRE_TOS, tos);

	data->rta_len = (void *)NLMSG_TAIL(&req.msg) - (void *)data;
	lnfo->rta_len = (void *)NLMSG_TAIL(&req.msg) - (void *)lnfo;


	if (rtnl_open(&rth, 0)) {
		fprintf(stderr, "cannot open netlink\n");
		return 1;
	}

	if (rtnl_talk(&rth, &req.msg, 0, 0, NULL) < 0) {
		fprintf(stderr, "failed to talk to netlink!\n");
		return -1;
	}

	rtnl_close(&rth);
	return 0;
}

typedef enum {
	E_AMBIGUOUS = -1,
	E_NONE = 0,
	C_LIST,
	C_ADD,
	C_SET,
	P_LOCAL,
	P_REMOTE,
	P_TUNNELID,
	P_TTL,
	P_TOS,
	P_LINK,
	P_NAME,
} e_cmd;

typedef struct {
	char *cmd;
	e_cmd cid;
} s_cmd;

static s_cmd cmds[] = {
	{ .cmd = "list", .cid = C_LIST, },
	{ .cmd = "show", .cid = C_LIST, },
	{ .cmd = "add", .cid = C_ADD, },
	{ .cmd = "new", .cid = C_ADD, },
	{ .cmd = "set", .cid = C_SET, },
	{ .cmd = "change", .cid = C_SET, },
	{ .cmd = NULL, .cid = 0, },
};

static s_cmd prms[] = {
	{ .cmd = "local", .cid = P_LOCAL, },
	{ .cmd = "remote", .cid = P_REMOTE, },
	{ .cmd = "tunnel-id", .cid = P_TUNNELID, },
	{ .cmd = "tid", .cid = P_TUNNELID, },
	{ .cmd = "ttl", .cid = P_TTL, },
	{ .cmd = "tos", .cid = P_TOS, },
	{ .cmd = "link", .cid = P_LINK, },
	{ .cmd = "name", .cid = P_NAME, },
	{ .cmd = NULL, .cid = 0, },
};

static e_cmd find_cmd(s_cmd *l, char *pcmd) {
	int cnt=0;
	int len=0;
	e_cmd match=E_NONE;

	while (l && pcmd && l->cmd) {
		char *ha = l->cmd;
		char *ne = pcmd;
		int clen = 0;

		while (*ha && *ne && *ha == *ne)
			ha++, ne++, clen++;

		if (!*ne) {
			if (clen && clen > len) {
				cnt = 0;
				len = clen;
				match = l->cid;
			}
			if (clen && clen == len)
				cnt++;
		}
		l++;
	}
	if (cnt == 1)
		return match;
	return cnt ? E_AMBIGUOUS : E_NONE;
}

static void usage(char *me) {
	printf("usage:\n"
		"\t%s add [name <name>] tunnel-id <id> [local <ip>] remote <ip> [ttl <ttl>] [tos <tos>] [link <ifindex|ifname>]\n"
		"\t%s set  name <name>  tunnel-id <id> [local <ip>] remote <ip> [ttl <ttl>] [tos <tos>] [link <ifindex|ifname>]\n"
		"\t%s list\n",
		me,me,me);
}

int main(int argc,char **argv) {
	if (argc >= 1) {
		e_cmd c = (argc == 1) ? C_LIST : find_cmd(cmds, argv[1]);
		int excl = 1;

		switch (c) {
			dafeutl:
			default:
				usage(argv[0]);
				return 0;
			case C_LIST:
				if (argc > 2)
					goto dafeutl;
				list();
				return 0;
			case C_SET:
				excl = 0;
			case C_ADD: {
				char ifname[IFNAMSIZ + 1] = "";
				uint32_t sip = htonl(0), dip = htonl(0);
				uint32_t tid = ~0;
				uint8_t ttl=0, tos=0;
				uint32_t link = 0;
				int i=2;

				while (i < argc) {
					e_cmd p = find_cmd(prms, argv[i]);
					int noarg=!((i + 1) < argc);
					struct in_addr iad;

					switch (p) {
						default:
							break;
						case E_NONE:
							printf("unknown option: %s\n", argv[i]);
							return 0;
						case E_AMBIGUOUS:
							printf("option is ambiguous: %s\n", argv[i]);
							return 0;
					}

					if (noarg) {
						printf("option: %s requires an argument\n", argv[i]);
						return 0;
					}

					switch (p) {
						default:
							printf("BUG!\n");
							return 0;
						case P_NAME:
							ifname[(sizeof ifname)-1] = 0;
							strncpy(ifname,argv[i + 1], IFNAMSIZ);
							break;
						case P_TTL:
							ttl=atoi(argv[i + 1]);
							break;
						case P_TOS:
							tos=atoi(argv[i + 1]);
							break;
						case P_LINK:
							// convert ifname to ifinidex, also support numeric arg
							link=if_nametoindex(argv[i + 1]);
							if (!link)
								link=atoi(argv[i + 1]);
							if (!link) {
								printf("invald interface name/index: %s\n", argv[i + 1]);
								return 0;
							}
							break;
						case P_TUNNELID:
							tid=atoi(argv[i + 1]);
							break;
						case P_LOCAL:
							if (!inet_aton(argv[i + 1], &iad)) {
								printf("invald ip address: %s\n", argv[i + 1]);
								return 0;
							}
							sip = iad.s_addr;
							break;
						case P_REMOTE:
							if (!inet_aton(argv[i + 1], &iad)) {
								printf("invald ip address: %s\n", argv[i + 1]);
								return 0;
							}
							dip = iad.s_addr;
							break;
					}
					i += 2;
				}
				if (tid > 0xffff) {
					if (tid == ~0)
						printf("tunnel-id is mandatory parameter\n");
					else
						printf("invalid tunnel-id value: %d\n",tid);
					return 0;
				}
				// tunnel id is in host byte order, addresses are in net byte order
				eoip_add(excl,ifname,tid,sip,dip,link,ttl,tos);
				return 0;
			}
		}
	}
	return 0;
}
