/* $Id: pfpinhole.c,v 1.29 2020/05/10 22:22:50 nanard Exp $ */
/* vim: tabstop=4 shiftwidth=4 noexpandtab
 * MiniUPnP project
 * http://miniupnp.free.fr/ or https://miniupnp.tuxfamily.org/
 * (c) 2012-2020 Thomas Bernard
 * This software is subject to the conditions detailed
 * in the LICENCE file provided within the distribution */

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/param.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#ifdef __DragonFly__
#include <net/pf/pfvar.h>
#else
#ifdef __APPLE__
#define PRIVATE 1
#endif
#include <net/pfvar.h>
#endif
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <string.h>
#include <syslog.h>
#include <stdio.h>
#include <stdlib.h>
#ifdef USE_LIBPFCTL
#include <libpfctl.h>
#endif

#include "config.h"
#include "pfpinhole.h"
#include "../upnpglobalvars.h"
#include "../macros.h"
#include "../upnputils.h"

/* the pass rules created by add_pinhole() are as follow :
 *
 * pass in quick on ep0 inet6 proto udp
 *   from any to dead:beef::42:42 port = 8080
 *   flags S/SA keep state
 *   label "pinhole-2 ts-4321000"
 *
 * with the label "pinhole-$uid ts-$timestamp: $description"
 */

#ifdef ENABLE_UPNPPINHOLE

/* /dev/pf when opened */
extern int dev;

static int next_uid = 1;

#define PINEHOLE_LABEL_FORMAT "pinhole-%d ts-%u: %s"
#define PINEHOLE_LABEL_FORMAT_SKIPDESC "pinhole-%d ts-%u: %*s"

int add_pinhole(const char * ifname,
                const char * rem_host, unsigned short rem_port,
                const char * int_client, unsigned short int_port,
                int proto, const char * desc, unsigned int timestamp)
{
	int uid;
	struct pfioc_rule pcr;
#ifndef PF_NEWSTYLE
	struct pfioc_pooladdr pp;
#endif

	if(dev<0) {
		syslog(LOG_ERR, "pf device is not open");
		return -1;
	}
	memset(&pcr, 0, sizeof(pcr));
	strlcpy(pcr.anchor, anchor_name, MAXPATHLEN);

#ifndef PF_NEWSTYLE
	memset(&pp, 0, sizeof(pp));
	strlcpy(pp.anchor, anchor_name, MAXPATHLEN);
	if(ioctl(dev, DIOCBEGINADDRS, &pp) < 0) {
		syslog(LOG_ERR, "ioctl(dev, DIOCBEGINADDRS, ...): %m");
		return -1;
	} else {
		pcr.pool_ticket = pp.ticket;
#else
	{
#endif
		pcr.rule.direction = PF_IN;
		pcr.rule.action = PF_PASS;
		pcr.rule.af = AF_INET6;
#ifdef PF_NEWSTYLE
		pcr.rule.nat.addr.type = PF_ADDR_NONE;
		pcr.rule.rdr.addr.type = PF_ADDR_NONE;
#endif
#ifdef USE_IFNAME_IN_RULES
		if(ifname)
			strlcpy(pcr.rule.ifname, ifname, IFNAMSIZ);
#endif
		pcr.rule.proto = proto;

		pcr.rule.quick = 1;/*(GETFLAG(PFNOQUICKRULESMASK))?0:1;*/
		pcr.rule.log = (GETFLAG(LOGPACKETSMASK))?1:0;	/*logpackets;*/
/* see the discussion on the forum :
 * http://miniupnp.tuxfamily.org/forum/viewtopic.php?p=638 */
		pcr.rule.flags = TH_SYN;
		pcr.rule.flagset = (TH_SYN|TH_ACK);
#ifdef PFRULE_HAS_RTABLEID
		pcr.rule.rtableid = -1;	/* first appeared in OpenBSD 4.0 */
#endif
#ifdef PFRULE_HAS_ONRDOMAIN
		pcr.rule.onrdomain = -1;	/* first appeared in OpenBSD 5.0 */
#endif
		pcr.rule.keep_state = 1;
		uid = next_uid;
		snprintf(pcr.rule.label, PF_RULE_LABEL_SIZE,
		         PINEHOLE_LABEL_FORMAT, uid, timestamp, desc);
		if(queue)
			strlcpy(pcr.rule.qname, queue, PF_QNAME_SIZE);
		if(tag)
			strlcpy(pcr.rule.tagname, tag, PF_TAG_NAME_SIZE);

		if(rem_port) {
			pcr.rule.src.port_op = PF_OP_EQ;
			pcr.rule.src.port[0] = htons(rem_port);
		}
		if(rem_host && rem_host[0] != '\0' && rem_host[0] != '*') {
			pcr.rule.src.addr.type = PF_ADDR_ADDRMASK;
			if(inet_pton(AF_INET6, rem_host, &pcr.rule.src.addr.v.a.addr.v6) != 1) {
				syslog(LOG_ERR, "inet_pton(%s) failed", rem_host);
			}
			memset(&pcr.rule.src.addr.v.a.mask.addr8, 255, 16);
		}

		pcr.rule.dst.port_op = PF_OP_EQ;
		pcr.rule.dst.port[0] = htons(int_port);
		pcr.rule.dst.addr.type = PF_ADDR_ADDRMASK;
		if(inet_pton(AF_INET6, int_client, &pcr.rule.dst.addr.v.a.addr.v6) != 1) {
			syslog(LOG_ERR, "inet_pton(%s) failed", int_client);
		}
		memset(&pcr.rule.dst.addr.v.a.mask.addr8, 255, 16);

		if(ifname)
			strlcpy(pcr.rule.ifname, ifname, IFNAMSIZ);

		pcr.action = PF_CHANGE_GET_TICKET;
		if(ioctl(dev, DIOCCHANGERULE, &pcr) < 0) {
			syslog(LOG_ERR, "ioctl(dev, DIOCCHANGERULE, ...) PF_CHANGE_GET_TICKET: %m");
			return -1;
		} else {
			pcr.action = PF_CHANGE_ADD_TAIL;
			if(ioctl(dev, DIOCCHANGERULE, &pcr) < 0) {
				syslog(LOG_ERR, "ioctl(dev, DIOCCHANGERULE, ...) PF_CHANGE_ADD_TAIL: %m");
				return -1;
			}
		}
	}

	if(++next_uid >= 65535) {
		next_uid = 1;
	}
	return uid;
}

int find_pinhole(const char * ifname,
                 const char * rem_host, unsigned short rem_port,
                 const char * int_client, unsigned short int_port,
                 int proto,
                 char *desc, int desc_len, unsigned int * timestamp)
{
	int uid;
	unsigned int ts;
	int i, n;
	struct pfioc_rule pr;
#ifdef USE_LIBPFCTL
	struct pfctl_rule rule;
	struct pfctl_rule *r = &rule;
#else
	struct pf_rule *r = &pr.rule;
#endif
	struct in6_addr saddr;
	struct in6_addr daddr;
	UNUSED(ifname);

	if(dev<0) {
		syslog(LOG_ERR, "pf device is not open");
		return -1;
	}
	if(rem_host && (rem_host[0] != '\0')) {
		inet_pton(AF_INET6, rem_host, &saddr);
	} else {
		memset(&saddr, 0, sizeof(struct in6_addr));
	}
	inet_pton(AF_INET6, int_client, &daddr);
	memset(&pr, 0, sizeof(pr));
	strlcpy(pr.anchor, anchor_name, MAXPATHLEN);
#ifndef PF_NEWSTYLE
	pr.rule.action = PF_PASS;
#endif
	if(ioctl(dev, DIOCGETRULES, &pr) < 0) {
		syslog(LOG_ERR, "ioctl(dev, DIOCGETRULES, ...): %m");
		return -1;
	}
	n = pr.nr;
	for(i=0; i<n; i++) {
		pr.nr = i;
#ifdef USE_LIBPFCTL
		if(pfctl_get_rule(dev, i, pr.ticket, pr.anchor, pr.action, &rule, pr.anchor_call) < 0) {
#else
		if(ioctl(dev, DIOCGETRULE, &pr) < 0) {
#endif
			syslog(LOG_ERR, "ioctl(dev, DIOCGETRULE): %m");
			return -1;
		}
		if((proto == r->proto) && (rem_port == ntohs(r->src.port[0]))
		   && (0 == memcmp(&saddr, &r->src.addr.v.a.addr.v6, sizeof(struct in6_addr)))
		   && (int_port == ntohs(r->dst.port[0])) &&
		   (0 == memcmp(&daddr, &r->dst.addr.v.a.addr.v6, sizeof(struct in6_addr)))) {
#ifdef USE_LIBPFCTL
			if(sscanf(r->label[0], PINEHOLE_LABEL_FORMAT_SKIPDESC, &uid, &ts) != 2) {
#else
			if(sscanf(r->label, PINEHOLE_LABEL_FORMAT_SKIPDESC, &uid, &ts) != 2) {
#endif
				syslog(LOG_DEBUG, "rule with label '%s' is not a IGD pinhole", rule.label[0]);
				continue;
			}
			if(timestamp) *timestamp = ts;
			if(desc) {
#ifdef USE_LIBPFCTL
				char * p = strchr(r->label[0], ':');
#else
				char * p = strchr(r->label, ':');
#endif
				if(p) {
					p += 2;
					strlcpy(desc, p, desc_len);
				}
			}
			return uid;
		}
	}
	return -2;
}

int delete_pinhole(unsigned short uid)
{
	int i, n;
	struct pfioc_rule pr;
#ifdef USE_LIBPFCTL
	struct pfctl_rule rule;
	struct pfctl_rule *r = &rule;
#else
	struct pf_rule *r = &pr.rule;
#endif
	char label_start[PF_RULE_LABEL_SIZE];
	char tmp_label[PF_RULE_LABEL_SIZE];

	if(dev<0) {
		syslog(LOG_ERR, "pf device is not open");
		return -1;
	}
	snprintf(label_start, sizeof(label_start),
	         "pinhole-%hu", uid);
	memset(&pr, 0, sizeof(pr));
	strlcpy(pr.anchor, anchor_name, MAXPATHLEN);
#ifndef PF_NEWSTYLE
	pr.rule.action = PF_PASS;
#endif
	if(ioctl(dev, DIOCGETRULES, &pr) < 0) {
		syslog(LOG_ERR, "ioctl(dev, DIOCGETRULES, ...): %m");
		return -1;
	}
	n = pr.nr;
	for(i=0; i<n; i++) {
		pr.nr = i;
#ifdef USE_LIBPFCTL
		if(pfctl_get_rule(dev, i, pr.ticket, pr.anchor, pr.action, &rule, pr.anchor_call) < 0) {
#else
		if(ioctl(dev, DIOCGETRULE, &pr) < 0) {
#endif
			syslog(LOG_ERR, "ioctl(dev, DIOCGETRULE): %m");
			return -1;
		}
#ifdef USE_LIBPFCTL
		strlcpy(tmp_label, r->label[0], sizeof(tmp_label));
#else
		strlcpy(tmp_label, r->label, sizeof(tmp_label));
#endif
		strtok(tmp_label, " ");
		if(0 == strcmp(tmp_label, label_start)) {
			pr.action = PF_CHANGE_GET_TICKET;
			if(ioctl(dev, DIOCCHANGERULE, &pr) < 0) {
				syslog(LOG_ERR, "ioctl(dev, DIOCCHANGERULE, ...) PF_CHANGE_GET_TICKET: %m");
				return -1;
			}
			pr.action = PF_CHANGE_REMOVE;
			pr.nr = i;
			if(ioctl(dev, DIOCCHANGERULE, &pr) < 0) {
				syslog(LOG_ERR, "ioctl(dev, DIOCCHANGERULE, ...) PF_CHANGE_REMOVE: %m");
				return -1;
			}
			return 0;
		}
	}
	/* not found */
	return -2;
}

int
get_pinhole_info(unsigned short uid,
                 char * rem_host, int rem_hostlen, unsigned short * rem_port,
                 char * int_client, int int_clientlen, unsigned short * int_port,
                 int * proto, char * desc, int desclen,
                 unsigned int * timestamp,
                 u_int64_t * packets, u_int64_t * bytes)
{
	int i, n;
	struct pfioc_rule pr;
#ifdef USE_LIBPFCTL
	struct pfctl_rule rule;
	struct pfctl_rule *r = &rule;
#else
	struct pf_rule *r = &pr.rule;
#endif
	char label_start[PF_RULE_LABEL_SIZE];
	char tmp_label[PF_RULE_LABEL_SIZE];
	char * p;

	if(dev<0) {
		syslog(LOG_ERR, "pf device is not open");
		return -1;
	}
	snprintf(label_start, sizeof(label_start),
	         "pinhole-%hu", uid);
	memset(&pr, 0, sizeof(pr));
	strlcpy(pr.anchor, anchor_name, MAXPATHLEN);
#ifndef PF_NEWSTYLE
	pr.rule.action = PF_PASS;
#endif
	if(ioctl(dev, DIOCGETRULES, &pr) < 0) {
		syslog(LOG_ERR, "ioctl(dev, DIOCGETRULES, ...): %m");
		return -1;
	}
	n = pr.nr;
	for(i=0; i<n; i++) {
		pr.nr = i;
#ifdef USE_LIBPFCTL
		if(pfctl_get_rule(dev, i, pr.ticket, pr.anchor, pr.action, &rule, pr.anchor_call) < 0) {
#else
		if(ioctl(dev, DIOCGETRULE, &pr) < 0) {
#endif
			syslog(LOG_ERR, "ioctl(dev, DIOCGETRULE): %m");
			return -1;
		}
#ifdef USE_LIBPFCTL
		strlcpy(tmp_label, r->label[0], sizeof(tmp_label));
#else
		strlcpy(tmp_label, r->label, sizeof(tmp_label));
#endif
		p = tmp_label;
		strsep(&p, " ");
		if(0 == strcmp(tmp_label, label_start)) {
			if(rem_host && (inet_ntop(AF_INET6, &r->src.addr.v.a.addr.v6, rem_host, rem_hostlen) == NULL)) {
				return -1;
			}
			if(rem_port)
				*rem_port = ntohs(r->src.port[0]);
			if(int_client && (inet_ntop(AF_INET6, &r->dst.addr.v.a.addr.v6, int_client, int_clientlen) == NULL)) {
				return -1;
			}
			if(int_port)
				*int_port = ntohs(r->dst.port[0]);
			if(proto)
				*proto = r->proto;
			if(timestamp)
				sscanf(p, "ts-%u", timestamp);
			if(desc) {
				strsep(&p, " ");
				if(p) {
					strlcpy(desc, p, desclen);
				} else {
					desc[0] = '\0';
				}
			}
#ifdef PFRULE_INOUT_COUNTS
			if(packets)
				*packets = r->packets[0] + r->packets[1];
			if(bytes)
				*bytes = r->bytes[0] + r->bytes[1];
#else
			if(packets)
				*packets = r->packets;
			if(bytes)
				*bytes = r->bytes;
#endif
			return 0;
		}
	}
	/* not found */
	return -2;
}

int update_pinhole(unsigned short uid, unsigned int timestamp)
{
	/* TODO :
	 * As it is not possible to change rule label, we should :
	 * 1 - delete
	 * 2 - Add new
	 * the stats of the rule will then be reset :( */
	UNUSED(uid); UNUSED(timestamp);
	return -42; /* not implemented */
}

/* return the number of rules removed
 * or a negative integer in case of error */
int clean_pinhole_list(unsigned int * next_timestamp)
{
	int i;
	struct pfioc_rule pr;
#ifdef USE_LIBPFCTL
	struct pfctl_rule rule;
	struct pfctl_rule *r = &rule;
#else
	struct pf_rule *r = &pr.rule;
#endif
	time_t current_time;
	unsigned int ts;
	int uid;
	unsigned int min_ts = UINT_MAX;
	int min_uid = INT_MAX, max_uid = -1;
	int n = 0;

	if(dev<0) {
		syslog(LOG_ERR, "pf device is not open");
		return -1;
	}
	current_time = upnp_time();
	memset(&pr, 0, sizeof(pr));
	strlcpy(pr.anchor, anchor_name, MAXPATHLEN);
#ifndef PF_NEWSTYLE
	pr.rule.action = PF_PASS;
#endif
	if(ioctl(dev, DIOCGETRULES, &pr) < 0) {
		syslog(LOG_ERR, "ioctl(dev, DIOCGETRULES, ...): %m");
		return -1;
	}
	for(i = pr.nr - 1; i >= 0; i--) {
		pr.nr = i;
#ifdef USE_LIBPFCTL
		if(pfctl_get_rule(dev, i, pr.ticket, pr.anchor, pr.action, &rule, pr.anchor_call) < 0) {
#else
		if(ioctl(dev, DIOCGETRULE, &pr) < 0) {
#endif
			syslog(LOG_ERR, "ioctl(dev, DIOCGETRULE): %m");
			return -1;
		}
#ifdef USE_LIBPFCTL
		if(sscanf(r->label[0], PINEHOLE_LABEL_FORMAT_SKIPDESC, &uid, &ts) != 2) {
			syslog(LOG_DEBUG, "rule with label '%s' is not a IGD pinhole", r->label[0]);
#else
		if(sscanf(r->label, PINEHOLE_LABEL_FORMAT_SKIPDESC, &uid, &ts) != 2) {
			syslog(LOG_DEBUG, "rule with label '%s' is not a IGD pinhole", r->label);
#endif
			continue;
		}
		if(ts <= (unsigned int)current_time) {
#ifdef USE_LIBPFCTL
			syslog(LOG_INFO, "removing expired pinhole '%s'", r->label[0]);
#else
			syslog(LOG_INFO, "removing expired pinhole '%s'", r->label);
#endif
			pr.action = PF_CHANGE_GET_TICKET;
			if(ioctl(dev, DIOCCHANGERULE, &pr) < 0) {
				syslog(LOG_ERR, "ioctl(dev, DIOCCHANGERULE, ...) PF_CHANGE_GET_TICKET: %m");
				return -1;
			}
			pr.action = PF_CHANGE_REMOVE;
			pr.nr = i;
			if(ioctl(dev, DIOCCHANGERULE, &pr) < 0) {
				syslog(LOG_ERR, "ioctl(dev, DIOCCHANGERULE, ...) PF_CHANGE_REMOVE: %m");
				return -1;
			}
			n++;
#ifndef PF_NEWSTYLE
			pr.rule.action = PF_PASS;
#endif
			if(ioctl(dev, DIOCGETRULES, &pr) < 0) {
				syslog(LOG_ERR, "ioctl(dev, DIOCGETRULES, ...): %m");
				return -1;
			}
		} else {
			if(uid > max_uid)
				max_uid = uid;
			else if(uid < min_uid)
				min_uid = uid;
			if(ts < min_ts)
				min_ts = ts;
		}
	}
	if(next_timestamp && (min_ts != UINT_MAX))
		*next_timestamp = min_ts;
	if(max_uid > 0) {
		if(((min_uid - 32000) <= next_uid) && (next_uid <= max_uid)) {
			next_uid = max_uid + 1;
		}
		if(next_uid >= 65535) {
			next_uid = 1;
		}
	}
	return n;	/* number of rules removed */
}

#endif /* ENABLE_UPNPPINHOLE */
