/* dnsmasq is Copyright (c) 2000-2007 Simon Kelley

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 dated June, 1991, or
   (at your option) version 3 dated 29 June, 2007.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "dnsmasq.h"
#include <rtk/options.h>
#ifdef CTC_DNS_SPEED_LIMIT
#include <rtk/mib.h>
#include <rtk/sysconfig.h>
#endif

static struct frec *lookup_frec(unsigned short id, unsigned int crc);
static struct frec *lookup_frec_by_sender(unsigned short id,
					  union mysockaddr *addr,
					  unsigned int crc);
static unsigned short get_id(int force, unsigned short force_id, unsigned int crc);
static void free_frec(struct frec *f);
static struct randfd *allocate_rfd(int family);
#ifdef SUPPORT_WEB_REDIRECT
static void add_new_server_for_unknown_domain(char *namebuf);
static int check_reply_sanity(HEADER *header, time_t now);
#endif

/* Send a UDP packet with its source address set as "source"
   unless nowild is true, when we just send it with the kernel default */
static void send_from(int fd, int nowild, char *packet, size_t len,
		      union mysockaddr *to, struct all_addr *source,
		      unsigned int iface)
{
  struct msghdr msg;
  struct iovec iov[1];
  union {
    struct cmsghdr align; /* this ensures alignment */
#if defined(HAVE_LINUX_NETWORK)
    char control[CMSG_SPACE(sizeof(struct in_pktinfo))];
#elif defined(IP_SENDSRCADDR)
    char control[CMSG_SPACE(sizeof(struct in_addr))];
#endif
#ifdef HAVE_IPV6
    char control6[CMSG_SPACE(sizeof(struct in6_pktinfo))];
#endif
  } control_u;

  iov[0].iov_base = packet;
  iov[0].iov_len = len;

  msg.msg_control = NULL;
  msg.msg_controllen = 0;
  msg.msg_flags = 0;
  msg.msg_name = to;
  msg.msg_namelen = sa_len(to);
  msg.msg_iov = iov;
  msg.msg_iovlen = 1;

  if (!nowild)
    {
      struct cmsghdr *cmptr;
      msg.msg_control = &control_u;
      msg.msg_controllen = sizeof(control_u);
      cmptr = CMSG_FIRSTHDR(&msg);

      if (to->sa.sa_family == AF_INET)
	{
#if defined(HAVE_LINUX_NETWORK)
	  struct in_pktinfo *pkt = (struct in_pktinfo *)CMSG_DATA(cmptr);
	  pkt->ipi_ifindex = 0;
	  pkt->ipi_spec_dst = source->addr.addr4;
	  msg.msg_controllen = cmptr->cmsg_len = CMSG_LEN(sizeof(struct in_pktinfo));
	  cmptr->cmsg_level = SOL_IP;
	  cmptr->cmsg_type = IP_PKTINFO;
#elif defined(IP_SENDSRCADDR)
	  struct in_addr *a = (struct in_addr *)CMSG_DATA(cmptr);
	  *a = source->addr.addr4;
	  msg.msg_controllen = cmptr->cmsg_len = CMSG_LEN(sizeof(struct in_addr));
	  cmptr->cmsg_level = IPPROTO_IP;
	  cmptr->cmsg_type = IP_SENDSRCADDR;
#endif
	}
      else
#ifdef HAVE_IPV6
	{
	  struct in6_pktinfo *pkt = (struct in6_pktinfo *)CMSG_DATA(cmptr);
	  pkt->ipi6_ifindex = iface; /* Need iface for IPv6 to handle link-local addrs */
	  pkt->ipi6_addr = source->addr.addr6;
	  msg.msg_controllen = cmptr->cmsg_len = CMSG_LEN(sizeof(struct in6_pktinfo));
	  cmptr->cmsg_type = IPV6_PKTINFO;
	  cmptr->cmsg_level = IPV6_LEVEL;
	}
#else
      iface = 0; /* eliminate warning */
#endif
    }

 retry:
  if (sendmsg(fd, &msg, 0) == -1)
    {
      /* certain Linux kernels seem to object to setting the source address in the IPv6 stack
	 by returning EINVAL from sendmsg. In that case, try again without setting the
	 source address, since it will nearly alway be correct anyway.  IPv6 stinks. */
      if (errno == EINVAL && msg.msg_controllen)
	{
	  msg.msg_controllen = 0;
	  goto retry;
	}
      if (retry_send())
	goto retry;
    }
}

static unsigned short search_servers(time_t now, struct all_addr **addrpp,
				     unsigned short qtype, char *qdomain, int *type, char **domain)

{
  /* If the query ends in the domain in one of our servers, set
     domain to point to that name. We find the largest match to allow both
     domain.org and sub.domain.org to exist. */

  unsigned int namelen = strlen(qdomain);
  unsigned int matchlen = 0;
  struct server *serv;
  unsigned short flags = 0;

  for (serv = daemon->servers; serv; serv=serv->next)
    /* domain matches take priority over NODOTS matches */
    if ((serv->flags & SERV_FOR_NODOTS) && *type != SERV_HAS_DOMAIN && !strchr(qdomain, '.') && namelen != 0)
      {
	unsigned short sflag = serv->addr.sa.sa_family == AF_INET ? F_IPV4 : F_IPV6;
	*type = SERV_FOR_NODOTS;
	if (serv->flags & SERV_NO_ADDR)
	  flags = F_NXDOMAIN;
	else if (serv->flags & SERV_LITERAL_ADDRESS)
	  {
	    if (sflag & qtype)
	      {
		flags = sflag;
		if (serv->addr.sa.sa_family == AF_INET)
		  *addrpp = (struct all_addr *)&serv->addr.in.sin_addr;
#ifdef HAVE_IPV6
		else
		  *addrpp = (struct all_addr *)&serv->addr.in6.sin6_addr;
#endif
	      }
	    else if (!flags || (flags & F_NXDOMAIN))
	      flags = F_NOERR;
	  }
      }
    else if (serv->flags & SERV_HAS_DOMAIN)
      {
	unsigned int domainlen = strlen(serv->domain);
	char *matchstart = qdomain + namelen - domainlen;
	if (namelen >= domainlen &&
	    hostname_isequal(matchstart, serv->domain) &&
	    domainlen >= matchlen &&
	    (domainlen == 0 || namelen == domainlen || *(serv->domain) == '.' || *(matchstart-1) == '.' ))
	  {
	    unsigned short sflag = serv->addr.sa.sa_family == AF_INET ? F_IPV4 : F_IPV6;
	    *type = SERV_HAS_DOMAIN;
	    *domain = serv->domain;
	    matchlen = domainlen;
	    if (serv->flags & SERV_NO_ADDR)
	      flags = F_NXDOMAIN;
	    else if (serv->flags & SERV_LITERAL_ADDRESS)
	      {
		if (sflag & qtype)
		  {
		    flags = sflag;
		    if (serv->addr.sa.sa_family == AF_INET)
		      *addrpp = (struct all_addr *)&serv->addr.in.sin_addr;
#ifdef HAVE_IPV6
		    else
		      *addrpp = (struct all_addr *)&serv->addr.in6.sin6_addr;
#endif
		  }
		else if (!flags || (flags & F_NXDOMAIN))
		  flags = F_NOERR;
	      }
	  }
      }

  if (flags == 0 && !(qtype & F_BIGNAME) &&
      (daemon->options & OPT_NODOTS_LOCAL) && !strchr(qdomain, '.') && namelen != 0)
    /* don't forward simple names, make exception for NS queries and empty name. */
    flags = F_NXDOMAIN;

  if (flags == F_NXDOMAIN && check_for_local_domain(qdomain, now))
    flags = F_NOERR;

  if (flags)
    {
      int logflags = 0;

      if (flags == F_NXDOMAIN || flags == F_NOERR)
	logflags = F_NEG | qtype;

      log_query(logflags | flags | F_CONFIG | F_FORWARD, qdomain, *addrpp, NULL);
    }

  return  flags;
}

static int forward_query(int udpfd, union mysockaddr *udpaddr,
			 struct all_addr *dst_addr, unsigned int dst_iface,
			 HEADER *header, size_t plen, time_t now, struct frec *forward)
{
  char *domain = NULL;
  int type = 0;
  struct all_addr *addrp = NULL;
  unsigned int crc = questions_crc(header, plen, daemon->namebuff);
  unsigned short flags = 0;
  unsigned short gotname = extract_request(header, plen, daemon->namebuff, NULL);
  struct server *start = NULL;

  /* may be no servers available. */
  if (!daemon->servers)
    forward = NULL;
  else if (forward || (forward = lookup_frec_by_sender(ntohs(header->id), udpaddr, crc)))
    {
      /* retry on existing query, send to all available servers  */
      domain = forward->sentto->domain;
      forward->sentto->failed_queries++;
      if (!(daemon->options & OPT_ORDER))
	{
	  forward->forwardall = 1;
	  daemon->last_server = NULL;
	}
      type = forward->sentto->flags & SERV_TYPE;
      if (!(start = forward->sentto->next))
	start = daemon->servers; /* at end of list, recycle */
      header->id = htons(forward->new_id);
    }
  else
    {
      if (gotname)
	flags = search_servers(now, &addrp, gotname, daemon->namebuff, &type, &domain);

      if (!flags && !(forward = get_new_frec(now, NULL)))
	/* table full - server failure. */
	flags = F_NEG;

      if (forward)
	{
	  /* force unchanging id for signed packets */
	  int is_sign;
	  find_pseudoheader(header, plen, NULL, NULL, &is_sign);

	  forward->source = *udpaddr;
	  forward->dest = *dst_addr;
	  forward->iface = dst_iface;
	  forward->orig_id = ntohs(header->id);
	  forward->new_id = get_id(is_sign, forward->orig_id, crc);
	  forward->fd = udpfd;
	  forward->crc = crc;
	  forward->forwardall = 0;
	  header->id = htons(forward->new_id);

	  /* In strict_order mode, or when using domain specific servers
	     always try servers in the order specified in resolv.conf,
	     otherwise, use the one last known to work. */

	  if (type != 0  || (daemon->options & OPT_ORDER))
	    start = daemon->servers;
	  else if (!(start = daemon->last_server))
	    {
	      start = daemon->servers;
	      forward->forwardall = 1;
	    }
	}
    }

  /* check for send errors here (no route to host)
     if we fail to send to all nameservers, send back an error
     packet straight away (helps modem users when offline)  */

  if (!flags && forward)
    {
      struct server *firstsentto = start;
      int forwarded = 0;

      while (1)
	{
	  /* only send to servers dealing with our domain.
	     domain may be NULL, in which case server->domain
	     must be NULL also. */

	  if (type == (start->flags & SERV_TYPE) &&
	      (type != SERV_HAS_DOMAIN || hostname_isequal(domain, start->domain)) &&
	      !(start->flags & SERV_LITERAL_ADDRESS))
	    {
	      int fd;

	      /* find server socket to use, may need to get random one. */
	      if (start->sfd)
		fd = start->sfd->fd;
	      else
		{
#ifdef HAVE_IPV6
		  if (start->addr.sa.sa_family == AF_INET6)
		    {
		      if (!forward->rfd6 &&
			  !(forward->rfd6 = allocate_rfd(AF_INET6)))
			break;
		      daemon->rfd_save = forward->rfd6;
		      fd = forward->rfd6->fd;
		    }
		  else
#endif
		    {
		      if (!forward->rfd4 &&
			  !(forward->rfd4 = allocate_rfd(AF_INET)))
			break;
		      daemon->rfd_save = forward->rfd4;
		      fd = forward->rfd4->fd;
		    }
		}

	      if (sendto(fd, (char *)header, plen, 0,
			 &start->addr.sa,
			 sa_len(&start->addr)) == -1)
		{
		  if (retry_send())
		    continue;
		}
	      else
		{
		  /* Keep info in case we want to re-send this packet */
		  daemon->srv_save = start;
		  daemon->packet_len = plen;

		  if (!gotname)
		    strcpy(daemon->namebuff, "query");
		  if (start->addr.sa.sa_family == AF_INET)
		    log_query(F_SERVER | F_IPV4 | F_FORWARD, daemon->namebuff,
			      (struct all_addr *)&start->addr.in.sin_addr, NULL);
#ifdef HAVE_IPV6
		  else
		    log_query(F_SERVER | F_IPV6 | F_FORWARD, daemon->namebuff,
			      (struct all_addr *)&start->addr.in6.sin6_addr, NULL);
#endif
		  start->queries++;
		  forwarded = 1;
		  forward->sentto = start;
		  if (!forward->forwardall)
		    break;
		  forward->forwardall++;
		}
	    }

	  if (!(start = start->next))
 	    start = daemon->servers;

	  if (start == firstsentto)
	    break;
	}

      if (forwarded)
	return 1;

      /* could not send on, prepare to return */
      header->id = htons(forward->orig_id);
      free_frec(forward); /* cancel */
    }

  /* could not send on, return empty answer or address if known for whole domain */
  if (udpfd != -1)
    {
      plen = setup_reply(header, plen, addrp, flags, daemon->local_ttl);
      send_from(udpfd, daemon->options & OPT_NOWILD, (char *)header, plen, udpaddr, dst_addr, dst_iface);
    }

  return 0;
}

static size_t process_reply(HEADER *header, time_t now,
			    struct server *server, size_t n)
{
  unsigned char *pheader, *sizep;
  int munged = 0, is_sign;
  size_t plen;

  /* If upstream is advertising a larger UDP packet size
	 than we allow, trim it so that we don't get overlarge
	 requests for the client. We can't do this for signed packets. */

  if ((pheader = find_pseudoheader(header, n, &plen, &sizep, &is_sign)) && !is_sign)
    {
      unsigned short udpsz;
      unsigned char *psave = sizep;

      GETSHORT(udpsz, sizep);
      if (udpsz > daemon->edns_pktsz)
	PUTSHORT(daemon->edns_pktsz, psave);
    }

  if (header->opcode != QUERY || (header->rcode != NOERROR && header->rcode != NXDOMAIN))
    return n;

  /* Complain loudly if the upstream server is non-recursive. */
  if (!header->ra && header->rcode == NOERROR && ntohs(header->ancount) == 0 &&
      server && !(server->flags & SERV_WARNED_RECURSIVE))
    {
      prettyprint_addr(&server->addr, daemon->namebuff);
      my_syslog(LOG_WARNING, _("nameserver %s refused to do a recursive query"), daemon->namebuff);
      if (!(daemon->options & OPT_LOG))
	server->flags |= SERV_WARNED_RECURSIVE;
    }

  if (daemon->bogus_addr && header->rcode != NXDOMAIN &&
      check_for_bogus_wildcard(header, n, daemon->namebuff, daemon->bogus_addr, now))
    {
      munged = 1;
      header->rcode = NXDOMAIN;
      header->aa = 0;
    }
  else
    {
      if (header->rcode == NXDOMAIN &&
	  extract_request(header, n, daemon->namebuff, NULL) &&
	  check_for_local_domain(daemon->namebuff, now))
	{
	  /* if we forwarded a query for a locally known name (because it was for
	     an unknown type) and the answer is NXDOMAIN, convert that to NODATA,
	     since we know that the domain exists, even if upstream doesn't */
	  munged = 1;
	  header->aa = 1;
	  header->rcode = NOERROR;
	}

      if (extract_addresses(header, n, daemon->namebuff, now))
	{
	  my_syslog(LOG_WARNING, _("possible DNS-rebind attack detected"));
	  munged = 1;
	}
    }

  /* do this after extract_addresses. Ensure NODATA reply and remove
     nameserver info. */

  if (munged)
    {
      header->ancount = htons(0);
      header->nscount = htons(0);
      header->arcount = htons(0);
    }

  /* the bogus-nxdomain stuff, doctor and NXDOMAIN->NODATA munging can all elide
     sections of the packet. Find the new length here and put back pseudoheader
     if it was removed. */
  return resize_packet(header, n, pheader, plen);
}

/* sets new last_server */
void reply_query(int fd, int family, time_t now)
{
  /* packet from peer server, extract data for cache, and send to
     original requester */
  HEADER *header;
  union mysockaddr serveraddr;
  struct frec *forward;
  socklen_t addrlen = sizeof(serveraddr);
  ssize_t n = recvfrom(fd, daemon->packet, daemon->edns_pktsz, 0, &serveraddr.sa, &addrlen);
  size_t nn;
  struct server *server;

  /* packet buffer overwritten */
  daemon->srv_save = NULL;

  /* Determine the address of the server replying  so that we can mark that as good */
  serveraddr.sa.sa_family = family;
#ifdef HAVE_IPV6
  if (serveraddr.sa.sa_family == AF_INET6)
    serveraddr.in6.sin6_flowinfo = 0;
#endif

  /* spoof check: answer must come from known server, */
  for (server = daemon->servers; server; server = server->next)
    if (!(server->flags & (SERV_LITERAL_ADDRESS | SERV_NO_ADDR)) &&
	sockaddr_isequal(&server->addr, &serveraddr))
      break;

  header = (HEADER *)daemon->packet;

  if (!server ||
      n < (int)sizeof(HEADER) || !header->qr ||
      !(forward = lookup_frec(ntohs(header->id), questions_crc(header, n, daemon->namebuff))))
    return;

  server = forward->sentto;

  // Mason Yu. dns_45/46
  //if ((header->rcode == SERVFAIL || header->rcode == REFUSED) &&
  if ((header->rcode == SERVFAIL || header->rcode == REFUSED || header->rcode == NOTIMP) &&
      !(daemon->options & OPT_ORDER) &&
      forward->forwardall == 0)
    /* for broken servers, attempt to send to another one. */
    {
      unsigned char *pheader;
      size_t plen;
      int is_sign;

      /* recreate query from reply */
      pheader = find_pseudoheader(header, (size_t)n, &plen, NULL, &is_sign);
      if (!is_sign)
	{
	  header->ancount = htons(0);
	  header->nscount = htons(0);
	  header->arcount = htons(0);
	  if ((nn = resize_packet(header, (size_t)n, pheader, plen)))
	    {
	      header->qr = 0;
	      header->tc = 0;
	      forward_query(-1, NULL, NULL, 0, header, nn, now, forward);
	      return;
	    }
	}
    }

  if ((forward->sentto->flags & SERV_TYPE) == 0)
    {
      // Mason Yu. dns_45/46
      //if (header->rcode == SERVFAIL || header->rcode == REFUSED)
      if (header->rcode == SERVFAIL || header->rcode == REFUSED || header->rcode == NOTIMP)
	server = NULL;
      else
	{
	  struct server *last_server;

	  /* find good server by address if possible, otherwise assume the last one we sent to */
	  for (last_server = daemon->servers; last_server; last_server = last_server->next)
	    if (!(last_server->flags & (SERV_LITERAL_ADDRESS | SERV_HAS_DOMAIN | SERV_FOR_NODOTS | SERV_NO_ADDR)) &&
		sockaddr_isequal(&last_server->addr, &serveraddr))
	      {
		server = last_server;
		break;
	      }
	}
      if (!(daemon->options & OPT_ALL_SERVERS))
	daemon->last_server = server;
    }

  /* If the answer is an error, keep the forward record in place in case
     we get a good reply from another server. Kill it when we've
     had replies from all to avoid filling the forwarding table when
     everything is broken */
  if (forward->forwardall == 0 || --forward->forwardall == 1 ||
    // Mason Yu. dns_45/46
    //(header->rcode != REFUSED && header->rcode != SERVFAIL))
    (header->rcode != REFUSED && header->rcode != SERVFAIL && header->rcode != NOTIMP))
    {
      if ((nn = process_reply(header, now, server, (size_t)n)))
	{
#ifdef SUPPORT_WEB_REDIRECT
		if (!check_reply_sanity(header, now))
	  	{
	  	  add_new_server_for_unknown_domain(daemon->namebuff);
	  	}
		else
		{
#endif
		  header->id = htons(forward->orig_id);
		  header->ra = 1; /* recursion if available */
		  send_from(forward->fd, daemon->options & OPT_NOWILD, daemon->packet, nn,
			    &forward->source, &forward->dest, forward->iface);
#ifdef SUPPORT_WEB_REDIRECT
		}
#endif
	}
      free_frec(forward); /* cancel */
    }
}

#ifdef CTC_DNS_SPEED_LIMIT
time_t last_reset_time = 0;
struct dns_log *dns_logs = NULL;
unsigned int a_query_count = 0, aaaa_query_count = 0;

struct dns_log
{
	char *domain;
	unsigned int a_count;
	unsigned int aaaa_count;
	struct dns_log *next;
};

void reset_dns_logs()
{
	if(dns_logs)
	{
		struct dns_log *current = NULL;
		current = dns_logs;

		while(current)
		{
			dns_logs = dns_logs->next;

			if(current->domain)
				free(current->domain);

			free(current);
			current = dns_logs;
		}
	}

	a_query_count = 0;
	aaaa_query_count = 0;
}

static char *get_sld(char *name)
{
	char *sld = rindex(name, '.');

	// dot not found
	if(sld == NULL)
		return name;

	sld--;
	while(sld != name && *(sld-1) != '.')
		sld--;

	return sld;
}

struct dns_log *log_dns_query(unsigned short type, char *name)
{
	struct dns_log *log = NULL;
	char *sld = NULL;

	if(name == NULL)
		return NULL;

	sld = get_sld(name);
	if(sld == NULL)
		return NULL;

	log = dns_logs;
	while(log)
	{
		if(strstr(log->domain, sld))
			break;
		log = log->next;
	}

	if(log == NULL)
	{
		log = malloc(sizeof(struct dns_log));
		memset(log, 0, sizeof(struct dns_log));

		log->domain = strdup(sld);

		// Add to list head
		log->next = dns_logs;
		dns_logs = log;
	}

	if(type == 1)
		log->a_count++;
	else if(type == 28)
		log->aaaa_count++;

	a_query_count++;
	aaaa_query_count++;

	return log;
}

int is_dns_limit_overflow(struct dns_log *log)
{
	int total = mib_chain_total(DNS_LIMIT_DOMAIN_TBL);
	int i;
	MIB_CE_DNS_LIMIT_DOMAIN_T entry;

	if(total == 0)
		return 0;

	for(i = 0 ; i < total ; i++)
	{
		if(mib_chain_get(DNS_LIMIT_DOMAIN_TBL, i, &entry) == 0)
			continue;

		if(strcmp(entry.domain, "ALL") == 0)
		{
			if(a_query_count > entry.limit || aaaa_query_count > entry.limit)
			{
				fprintf(stderr, "[dns_speed_limit] domain=%s\n", entry.domain);
				return 1;
			}
		}
		else if(strstr(entry.domain, log->domain))
		{
			if(log->a_count > entry.limit || log->aaaa_count > entry.limit)
			{
				fprintf(stderr, "[dns_speed_limit] domain %s qeuery is overflow\n", entry.domain);
				return 1;
			}
		}
	}

	return 0;
}

static void add_dns_limit_dev_info(char *name, union mysockaddr *addr, int family)
{
	int total = mib_chain_total(DNS_LIMIT_DEV_INFO_TBL);
	MIB_CE_DNS_LIMIT_DEV_INFO_T entry;

	if(name == NULL || addr == NULL)
		return;

	if(total == DNS_LIMIT_MAX)
		mib_chain_delete(DNS_LIMIT_DEV_INFO_TBL, 0);

	
	bzero(&entry, sizeof(entry));
	strcpy(entry.domain, name);

	if(family== AF_INET)
	{
		int s;
		struct arpreq arpreq;
		struct sockaddr_in *sin;
		char buff[ADDRSTRLEN] = {0};

		entry.ip_ver = IPVER_IPV4;
		memcpy(entry.ip_addr, &addr->in.sin_addr, IP_ADDR_LEN);

		inet_ntop(AF_INET, &addr->in.sin_addr, buff, ADDRSTRLEN);
		fprintf(stderr, "add %s into dev info\n", buff);

		// get mac address
		memset(&arpreq, 0, sizeof(struct arpreq));
		sin = (struct sockaddr_in *) &arpreq.arp_pa;
		sin->sin_family = AF_INET;
		memcpy(&sin->sin_addr, &addr->in.sin_addr, IP_ADDR_LEN);
		strcpy(arpreq.arp_dev, "br0");

		s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
		if (s < 0)
		{
			perror("[dns_speed_limit] socket");
			return;
		}
		if (ioctl(s, SIOCGARP, &arpreq) < 0)
		{
			perror("[dns_speed_limit] ioctl");
			close(s);
			return;
		}

		if (!(arpreq.arp_flags & ATF_COM))
		{
			close(s);
			return ;
		}

		memcpy(entry.mac, arpreq.arp_ha.sa_data, MAC_ADDR_LEN);
		mib_chain_add(DNS_LIMIT_DEV_INFO_TBL, &entry);

		close(s);
	}
	else if(family == AF_INET6)
	{
		fprintf(stderr, "[dns_speed_limit] FIXME: do not support IPv6\n");
	}
	else
		return;

	return;
}

int handle_dns_speed_limit(unsigned short type, char *name, union mysockaddr *addr, int family)
{
	time_t current_time = 0;
	struct dns_log *log = NULL;

	if(type != 1 && type != 28)
		return 0;

	if(name == NULL)
		return 0;

	current_time = time(NULL);
	if(current_time - last_reset_time >= 60)
	{
		reset_dns_logs();
		last_reset_time = current_time;
	}

	log = log_dns_query(type, name);

	if(log && is_dns_limit_overflow(log))
	{
		unsigned char action;

		add_dns_limit_dev_info(log->domain, addr, family);
		mib_get(MIB_DNS_LIMIT_ACTION, &action);
		if(action == DNS_LIMIT_ACTION_DROP)
			return -1;
	}
	return 0;
}
#endif

void receive_query(struct listener *listen, time_t now)
{
  HEADER *header = (HEADER *)daemon->packet;
  union mysockaddr source_addr;
  unsigned short type;
  struct all_addr dst_addr;
  struct in_addr netmask, dst_addr_4;
  size_t m;
  ssize_t n;
  int if_index = 0;
  struct iovec iov[1];
  struct msghdr msg;
  struct cmsghdr *cmptr;
  union {
    struct cmsghdr align; /* this ensures alignment */
#ifdef HAVE_IPV6
    char control6[CMSG_SPACE(sizeof(struct in6_pktinfo))];
#endif
#if defined(HAVE_LINUX_NETWORK)
    char control[CMSG_SPACE(sizeof(struct in_pktinfo))];
#elif defined(IP_RECVDSTADDR) && defined(HAVE_SOLARIS_NETWORK)
    char control[CMSG_SPACE(sizeof(struct in_addr)) +
		 CMSG_SPACE(sizeof(unsigned int))];
#elif defined(IP_RECVDSTADDR)
    char control[CMSG_SPACE(sizeof(struct in_addr)) +
		 CMSG_SPACE(sizeof(struct sockaddr_dl))];
#endif
  } control_u;

  /* packet buffer overwritten */
  daemon->srv_save = NULL;

  if (listen->family == AF_INET && (daemon->options & OPT_NOWILD))
    {
      dst_addr_4 = listen->iface->addr.in.sin_addr;
      netmask = listen->iface->netmask;
    }
  else
    {
      dst_addr_4.s_addr = 0;
      netmask.s_addr = 0;
    }

  iov[0].iov_base = daemon->packet;
  iov[0].iov_len = daemon->edns_pktsz;

  msg.msg_control = control_u.control;
  msg.msg_controllen = sizeof(control_u);
  msg.msg_flags = 0;
  msg.msg_name = &source_addr;
  msg.msg_namelen = sizeof(source_addr);
  msg.msg_iov = iov;
  msg.msg_iovlen = 1;

  if ((n = recvmsg(listen->fd, &msg, 0)) == -1)
    return;

  if (n < (int)sizeof(HEADER) ||
      (msg.msg_flags & MSG_TRUNC) ||
      header->qr)
    return;

  source_addr.sa.sa_family = listen->family;
#ifdef HAVE_IPV6
  if (listen->family == AF_INET6)
    source_addr.in6.sin6_flowinfo = 0;
#endif

  if (!(daemon->options & OPT_NOWILD))
    {
      struct ifreq ifr;

      if (msg.msg_controllen < sizeof(struct cmsghdr))
	return;

#if defined(HAVE_LINUX_NETWORK)
      if (listen->family == AF_INET)
	for (cmptr = CMSG_FIRSTHDR(&msg); cmptr; cmptr = CMSG_NXTHDR(&msg, cmptr))
	  if (cmptr->cmsg_level == SOL_IP && cmptr->cmsg_type == IP_PKTINFO)
	    {
	      dst_addr_4 = dst_addr.addr.addr4 = ((struct in_pktinfo *)CMSG_DATA(cmptr))->ipi_spec_dst;
	      if_index = ((struct in_pktinfo *)CMSG_DATA(cmptr))->ipi_ifindex;
	    }
#elif defined(IP_RECVDSTADDR) && defined(IP_RECVIF)
      if (listen->family == AF_INET)
	{
	  for (cmptr = CMSG_FIRSTHDR(&msg); cmptr; cmptr = CMSG_NXTHDR(&msg, cmptr))
	    if (cmptr->cmsg_level == IPPROTO_IP && cmptr->cmsg_type == IP_RECVDSTADDR)
	      dst_addr_4 = dst_addr.addr.addr4 = *((struct in_addr *)CMSG_DATA(cmptr));
	    else if (cmptr->cmsg_level == IPPROTO_IP && cmptr->cmsg_type == IP_RECVIF)
#ifdef HAVE_SOLARIS_NETWORK
	      if_index = *((unsigned int *)CMSG_DATA(cmptr));
#else
	      if_index = ((struct sockaddr_dl *)CMSG_DATA(cmptr))->sdl_index;
#endif
	}
#endif

#ifdef HAVE_IPV6
      if (listen->family == AF_INET6)
	{
	  for (cmptr = CMSG_FIRSTHDR(&msg); cmptr; cmptr = CMSG_NXTHDR(&msg, cmptr))
	    if (cmptr->cmsg_level == IPV6_LEVEL && cmptr->cmsg_type == IPV6_PKTINFO)
	      {
		dst_addr.addr.addr6 = ((struct in6_pktinfo *)CMSG_DATA(cmptr))->ipi6_addr;
		if_index =((struct in6_pktinfo *)CMSG_DATA(cmptr))->ipi6_ifindex;
	      }
	}
#endif

      /* enforce available interface configuration */

      if (if_index == 0)
	return;

#ifdef SIOCGIFNAME
      ifr.ifr_ifindex = if_index;
      if (ioctl(listen->fd, SIOCGIFNAME, &ifr) == -1)
	return;
#else
      if (!if_indextoname(if_index, ifr.ifr_name))
	return;
#endif

      if (!iface_check(listen->family, &dst_addr, &ifr, &if_index))
	return;

      if (listen->family == AF_INET &&
	  (daemon->options & OPT_LOCALISE) &&
	  ioctl(listen->fd, SIOCGIFNETMASK, &ifr) == -1)
	return;

      netmask = ((struct sockaddr_in *) &ifr.ifr_addr)->sin_addr;
    }

  if (extract_request(header, (size_t)n, daemon->namebuff, &type))
    {
      char types[20];

#ifdef CTC_DNS_SPEED_LIMIT
      if(handle_dns_speed_limit(type, daemon->namebuff, &source_addr, listen->family))
	  	return; //drop if not return 0
#endif

      querystr(types, type);

      if (listen->family == AF_INET)
	log_query(F_QUERY | F_IPV4 | F_FORWARD, daemon->namebuff,
		  (struct all_addr *)&source_addr.in.sin_addr, types);
#ifdef HAVE_IPV6
      else
	log_query(F_QUERY | F_IPV6 | F_FORWARD, daemon->namebuff,
		  (struct all_addr *)&source_addr.in6.sin6_addr, types);
#endif
    }
	else
	{
		// Mason Yu. // dns_tcp_150
		if (header->opcode == STATUS)
		{
			//printf("This is a status requests, return\n");
			return;
		}
	}

  m = answer_request (header, ((char *) header) + PACKETSZ, (size_t)n,
		      dst_addr_4, netmask, now);
  if (m >= 1)
    {
      send_from(listen->fd, daemon->options & OPT_NOWILD, (char *)header,
		m, &source_addr, &dst_addr, if_index);
      daemon->local_answer++;
    }
  else if (forward_query(listen->fd, &source_addr, &dst_addr, if_index,
			 header, (size_t)n, now, NULL))
    daemon->queries_forwarded++;
  else
    daemon->local_answer++;
}

// Added by Mason Yu
//do a nonblocking connect
//  return -1 on a system call error, 0 on success
//  sa - host to connect to, filled by caller
//  sock - the socket to connect
//  timeout - how long to wait to connect
inline int
conn_nonb(struct server *last_server, int sock, int timeout)
{
    int flags = 0, error = 0, ret = 0;
    fd_set  rset, wset;
    socklen_t   len = sizeof(error);
    struct timeval  ts;

    ts.tv_sec = timeout;
    ts.tv_usec = 0;

    //clear out descriptor sets for select
    //add socket to the descriptor sets
    FD_ZERO(&rset);
    FD_SET(sock, &rset);
    wset = rset;    //structure assignment ok

    //set socket nonblocking flag
    if( (flags = fcntl(sock, F_GETFL, 0)) < 0)
        return -1;

    if(fcntl(sock, F_SETFL, flags | O_NONBLOCK) < 0)
        return -1;

    //initiate non-blocking connect
	if( (ret = connect(sock, &last_server->addr.sa, sa_len(&last_server->addr))) < 0 )
        if (errno != EINPROGRESS)
            return -1;

    if(ret == 0)    //then connect succeeded right away
        goto done;

    //we are waiting for connect to complete now
    if( (ret = select(sock + 1, &rset, &wset, NULL, (timeout) ? &ts : NULL)) < 0)
        return -1;
    if(ret == 0){   //we had a timeout
        errno = ETIMEDOUT;
        return -1;
    }

    //we had a positivite return so a descriptor is ready
    if (FD_ISSET(sock, &rset) || FD_ISSET(sock, &wset)){
        if(getsockopt(sock, SOL_SOCKET, SO_ERROR, &error, &len) < 0)
            return -1;
    }else
        return -1;

    if(error){  //check if we had a socket error
        errno = error;
        return -1;
    }

done:
// Mason Yu. dns_tcp_60
#if 0
    //put socket back in blocking mode
    if(fcntl(sock, F_SETFL, flags) < 0)
        return -1;
#endif
    return 0;
}

/* The daemon forks before calling this: it should deal with one connection,
   blocking as neccessary, and then return. Note, need to be a bit careful
   about resources for debug mode, when the fork is suppressed: that's
   done by the caller. */
unsigned char *tcp_request(int confd, time_t now,
			   struct in_addr local_addr, struct in_addr netmask)
{
  int size = 0;
  size_t m;
  unsigned short qtype, gotname;
  unsigned char c1, c2;
  /* Max TCP packet + slop */
  unsigned char *packet = whine_malloc(65536 + MAXDNAME + RRFIXEDSZ);
  HEADER *header;
  struct server *last_server;

  while (1)
    {
      if (!packet ||
	  !read_write(confd, &c1, 1, 1) || !read_write(confd, &c2, 1, 1) ||
	  !(size = c1 << 8 | c2) ||
	  !read_write(confd, packet, size, 1))
       	return packet;

      if (size < (int)sizeof(HEADER))
	continue;

      header = (HEADER *)packet;

      if ((gotname = extract_request(header, (unsigned int)size, daemon->namebuff, &qtype)))
	{
	  union mysockaddr peer_addr;
	  socklen_t peer_len = sizeof(union mysockaddr);

	  if (getpeername(confd, (struct sockaddr *)&peer_addr, &peer_len) != -1)
	    {
	      char types[20];

	      querystr(types, qtype);

	      if (peer_addr.sa.sa_family == AF_INET)
		log_query(F_QUERY | F_IPV4 | F_FORWARD, daemon->namebuff,
			  (struct all_addr *)&peer_addr.in.sin_addr, types);
#ifdef HAVE_IPV6
	      else
		log_query(F_QUERY | F_IPV6 | F_FORWARD, daemon->namebuff,
			  (struct all_addr *)&peer_addr.in6.sin6_addr, types);
#endif
	    }
	}

	if (gotname==0)
	{
		// Mason Yu. // dns_tcp_150
		if (header->opcode == STATUS)
		{
			//printf("This is a TCP status requests, return\n");
			return packet;
		}
	}

      /* m > 0 if answered from cache */
      m = answer_request(header, ((char *) header) + 65536, (unsigned int)size,
			 local_addr, netmask, now);

      /* Do this by steam now we're not in the select() loop */
      check_log_writer(NULL);

      if (m == 0)
	{
	  unsigned short flags = 0;
	  struct all_addr *addrp = NULL;
	  int type = 0;
	  char *domain = NULL;
	  // Mason Yu.  dns_tcp_45/46
	  int query_all=0;

	  if (gotname)
	    flags = search_servers(now, &addrp, gotname, daemon->namebuff, &type, &domain);

	  if (type != 0  || (daemon->options & OPT_ORDER) || !daemon->last_server)
	    last_server = daemon->servers;
	  else
	    last_server = daemon->last_server;

	  if (!flags && last_server)
	    {
	      struct server *firstsendto = NULL;
	      unsigned int crc = questions_crc(header, (unsigned int)size, daemon->namebuff);

	      /* Loop round available servers until we succeed in connecting to one.
	         Note that this code subtley ensures that consecutive queries on this connection
	         which can go to the same server, do so. */
	      while (1)
		{
		  if (!firstsendto)
		    firstsendto = last_server;
		  else
		    {
		      if (!(last_server = last_server->next))
			last_server = daemon->servers;

		      if (last_server == firstsendto)
			break;
		    }

		  /* server for wrong domain */
		  if (type != (last_server->flags & SERV_TYPE) ||
		      (type == SERV_HAS_DOMAIN && !hostname_isequal(domain, last_server->domain)))
		    continue;

		  if ((last_server->tcpfd == -1) &&
		      (last_server->tcpfd = socket(last_server->addr.sa.sa_family, SOCK_STREAM, 0)) != -1 &&
		      (!local_bind(last_server->tcpfd,  &last_server->source_addr, last_server->interface, 1) ||
		       connect(last_server->tcpfd, &last_server->addr.sa, sa_len(&last_server->addr)) == -1))
		    {
		      close(last_server->tcpfd);
		      last_server->tcpfd = -1;
		    }

		  if (last_server->tcpfd == -1)
		    continue;

          // Mason Yu. dns_tcp_45/46
	      if (query_all == 1)
	      {
                /* for broken servers, attempt to send to another one. */
                unsigned char *pheader;
                size_t plen, nn;
                int is_sign;

                /* recreate query from reply */
                pheader = find_pseudoheader(header, (size_t)size, &plen, NULL, &is_sign);
                if (!is_sign)
	           {
	             header->ancount = htons(0);
	             header->nscount = htons(0);
	             header->arcount = htons(0);
	             if ((nn = resize_packet(header, (size_t)size, pheader, plen)))
	              {
	                header->qr = 0;
	                header->tc = 0;
                    size = nn;
                    packet = (unsigned char *)header;
	              }
	           }
	      }

		  c1 = size >> 8;
		  c2 = size;

		  if (!read_write(last_server->tcpfd, &c1, 1, 0) ||
		      !read_write(last_server->tcpfd, &c2, 1, 0) ||
		      !read_write(last_server->tcpfd, packet, size, 0) ||
		      !read_write(last_server->tcpfd, &c1, 1, 1) ||
		      !read_write(last_server->tcpfd, &c2, 1, 1))
		    {
		      close(last_server->tcpfd);
		      last_server->tcpfd = -1;
		      continue;
		    }

		  m = (c1 << 8) | c2;
		  if (!read_write(last_server->tcpfd, packet, m, 1))
		  {
		    // Mason Yu. dns_tcp_60
		    continue;
		    //return packet;
		  }

		  if (!gotname)
		    strcpy(daemon->namebuff, "query");
		  if (last_server->addr.sa.sa_family == AF_INET)
		    log_query(F_SERVER | F_IPV4 | F_FORWARD, daemon->namebuff,
			      (struct all_addr *)&last_server->addr.in.sin_addr, NULL);
#ifdef HAVE_IPV6
		  else
		    log_query(F_SERVER | F_IPV6 | F_FORWARD, daemon->namebuff,
			      (struct all_addr *)&last_server->addr.in6.sin6_addr, NULL);
#endif

		  /* There's no point in updating the cache, since this process will exit and
		     lose the information after a few queries. We make this call for the alias and
		     bogus-nxdomain side-effects. */
		  /* If the crc of the question section doesn't match the crc we sent, then
		     someone might be attempting to insert bogus values into the cache by
		     sending replies containing questions and bogus answers. */
		  if (crc == questions_crc(header, (unsigned int)m, daemon->namebuff))
		    m = process_reply(header, now, last_server, (unsigned int)m);

		  // Mason Yu. dns_tcp_45/46
		  //break;
		  if (header->opcode == QUERY && ((header->rcode == SERVFAIL || header->rcode == REFUSED || header->rcode == NOTIMP)))
		  {
		     my_syslog(LOG_INFO, _("tcp_request:Get TCP reply with error from %s"), inet_ntoa(last_server->addr.in.sin_addr));
		     query_all = 1;
		  }

		  // Mason Yu. dns_tcp_45/46
		  if (header->opcode == QUERY && (header->rcode == NOERROR || (header->rcode != SERVFAIL && header->rcode != REFUSED && header->rcode != NOTIMP)))
		  {
		     //printf("tcp_request: Get TCP reply with NOERROR from %s", inet_ntoa(last_server->addr.in.sin_addr));
		     break;
		  }
		}
		}

	  /* In case of local answer or no connections made. */
	  if (m == 0)
	    m = setup_reply(header, (unsigned int)size, addrp, flags, daemon->local_ttl);
	}

      check_log_writer(NULL);

      c1 = m>>8;
      c2 = m;
      if (!read_write(confd, &c1, 1, 0) ||
	  !read_write(confd, &c2, 1, 0) ||
	  !read_write(confd, packet, m, 0))
	return packet;
    }
}

static struct frec *allocate_frec(time_t now)
{
  struct frec *f;

  if ((f = (struct frec *)whine_malloc(sizeof(struct frec))))
    {
      f->next = daemon->frec_list;
      f->time = now;
      f->sentto = NULL;
      f->rfd4 = NULL;
#ifdef HAVE_IPV6
      f->rfd6 = NULL;
#endif
      daemon->frec_list = f;
    }

  return f;
}

static struct randfd *allocate_rfd(int family)
{
  static int finger = 0;
  int i;

  /* limit the number of sockets we have open to avoid starvation of
     (eg) TFTP. Once we have a reasonable number, randomness should be OK */

  for (i = 0; i < RANDOM_SOCKS; i++)
    if (daemon->randomsocks[i].refcount == 0 &&
	(daemon->randomsocks[i].fd = random_sock(family)) != -1)
      {
	daemon->randomsocks[i].refcount = 1;
	daemon->randomsocks[i].family = family;
	return &daemon->randomsocks[i];
      }

  /* No free ones, grab an existing one */
  for (i = 0; i < RANDOM_SOCKS; i++)
    {
      int j = (i+finger) % RANDOM_SOCKS;
      if (daemon->randomsocks[j].family == family && daemon->randomsocks[j].refcount != 0xffff)
	{
	  finger = j;
	  daemon->randomsocks[j].refcount++;
	  return &daemon->randomsocks[j];
	}
    }

  return NULL; /* doom */
}

static void free_frec(struct frec *f)
{
  if (f->rfd4 && --(f->rfd4->refcount) == 0)
    close(f->rfd4->fd);

  f->rfd4 = NULL;
  f->sentto = NULL;

#ifdef HAVE_IPV6
  if (f->rfd6 && --(f->rfd6->refcount) == 0)
    close(f->rfd6->fd);

  f->rfd6 = NULL;
#endif
}

/* if wait==NULL return a free or older than TIMEOUT record.
   else return *wait zero if one available, or *wait is delay to
   when the oldest in-use record will expire. Impose an absolute
   limit of 4*TIMEOUT before we wipe things (for random sockets) */
struct frec *get_new_frec(time_t now, int *wait)
{
  struct frec *f, *oldest, *target;
  int count;

  if (wait)
    *wait = 0;

  for (f = daemon->frec_list, oldest = NULL, target =  NULL, count = 0; f; f = f->next, count++)
    if (!f->sentto)
      target = f;
    else
      {
	if (difftime(now, f->time) >= 4*TIMEOUT)
	  {
	    free_frec(f);
	    target = f;
	  }

	if (!oldest || difftime(f->time, oldest->time) <= 0)
	  oldest = f;
      }

  if (target)
    {
      target->time = now;
      return target;
    }

  /* can't find empty one, use oldest if there is one
     and it's older than timeout */
  if (oldest && ((int)difftime(now, oldest->time)) >= TIMEOUT)
    {
      /* keep stuff for twice timeout if we can by allocating a new
	 record instead */
      if (difftime(now, oldest->time) < 2*TIMEOUT &&
	  count <= daemon->ftabsize &&
	  (f = allocate_frec(now)))
	return f;

      if (!wait)
	{
	  free_frec(oldest);
	  oldest->time = now;
	}
      return oldest;
    }

  /* none available, calculate time 'till oldest record expires */
  if (count > daemon->ftabsize)
    {
      if (oldest && wait)
	*wait = oldest->time + (time_t)TIMEOUT - now;
      return NULL;
    }

  if (!(f = allocate_frec(now)) && wait)
    /* wait one second on malloc failure */
    *wait = 1;

  return f; /* OK if malloc fails and this is NULL */
}

/* crc is all-ones if not known. */
static struct frec *lookup_frec(unsigned short id, unsigned int crc)
{
  struct frec *f;

  for(f = daemon->frec_list; f; f = f->next)
    if (f->sentto && f->new_id == id &&
	(f->crc == crc || crc == 0xffffffff))
      return f;

  return NULL;
}

static struct frec *lookup_frec_by_sender(unsigned short id,
					  union mysockaddr *addr,
					  unsigned int crc)
{
  struct frec *f;

  // Change the criterion to check if the packet carry  the same URL and source IP only for CDrouter test on cdrouter_app_25/26. Mason Yu
  for(f = daemon->frec_list; f; f = f->next)
  {
    if (f->sentto &&
	// Mason Yu. cdrouter_app_25/26
	f->orig_id == id &&
	f->crc == crc &&
	// Mason Yu. cdrouter_app_25/26
	sockaddr_isequal(&f->source, addr))
	//sockaddr_isequal_2(&f->source, addr))
      return f;
  }
  return NULL;
}

/* A server record is going away, remove references to it */
void server_gone(struct server *server)
{
  struct frec *f;

  for (f = daemon->frec_list; f; f = f->next)
    if (f->sentto && f->sentto == server)
      free_frec(f);

  if (daemon->last_server == server)
    daemon->last_server = NULL;

  if (daemon->srv_save == server)
    daemon->srv_save = NULL;
}

/* return unique random ids.
   For signed packets we can't change the ID without breaking the
   signing, so we keep the same one. In this case force is set, and this
   routine degenerates into killing any conflicting forward record. */
static unsigned short get_id(int force, unsigned short force_id, unsigned int crc)
{
  unsigned short ret = 0;

  if (force)
    {
      struct frec *f = lookup_frec(force_id, crc);
      if (f)
	free_frec(f); /* free */
      ret = force_id;
    }
  else do
    ret = rand16();
  while (lookup_frec(ret, crc));

  return ret;
}

#ifdef SUPPORT_WEB_REDIRECT
static void add_new_server_for_unknown_domain(char *namebuf)
{
	struct server *serv;
	char *domain;
	int source_port = 0, serv_port = NAMESERVER_PORT;
	unsigned int redirectIP;

	mib_get(MIB_REDIRECT_IP, (void *)&redirectIP);
	domain = safe_malloc(strlen(namebuf)+1); /* NULL if strlen is zero */
	if (domain) strcpy(domain, namebuf);
	
	serv = safe_malloc(sizeof(struct server));
	memset(serv, 0, sizeof(struct server));
	serv->next = NULL;
	serv->domain = domain;
	serv->flags = domain ? SERV_HAS_DOMAIN : SERV_FOR_NODOTS;

	serv->flags |= SERV_LITERAL_ADDRESS;

	if ((serv->addr.in.sin_addr.s_addr = redirectIP) != (in_addr_t) -1)
	{
		serv->addr.in.sin_port = htons(serv_port);	
		serv->source_addr.in.sin_port = htons(source_port); 
		serv->addr.sa.sa_family = serv->source_addr.sa.sa_family = AF_INET;
#ifdef HAVE_SOCKADDR_SA_LEN
		serv->source_addr.in.sin_len = serv->addr.in.sin_len = sizeof(struct sockaddr_in);
#endif
		serv->source_addr.in.sin_addr.s_addr = INADDR_ANY;
	}

	serv->next = daemon->servers;
	daemon->servers = serv;
}

static int check_reply_sanity(HEADER *header, time_t now)
{
	unsigned char itmsAddr;
	struct crec *crecp;
	
	if (header->rcode == NXDOMAIN)/*unkonw domain name*/
		return 0;

	if (header->rcode == NOERROR)
	{
		mib_get(MIB_ITMS_ADDR, (void *)&itmsAddr);
		
		crecp = cache_find_by_name(NULL, daemon->namebuff, now, F_IPV4 | F_IPV6);
		if ((crecp != NULL) && (crecp->flags & F_FORWARD))
		{
			if (crecp->addr.addr.addr.addr4.s_addr == itmsAddr)
				return 0;
		}
	}
	
	return 1;
}
#endif

