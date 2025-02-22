/* routines that interface with the kernel's IPsec mechanism, for libreswan
 *
 * Copyright (C) 1997 Angelos D. Keromytis.
 * Copyright (C) 1998-2010  D. Hugh Redelmeier.
 * Copyright (C) 2003-2008 Michael Richardson <mcr@xelerance.com>
 * Copyright (C) 2007-2010 Paul Wouters <paul@xelerance.com>
 * Copyright (C) 2008-2010 David McCullough <david_mccullough@securecomputing.com>
 * Copyright (C) 2010 Bart Trojanowski <bart@jukie.net>
 * Copyright (C) 2009-2010 Tuomo Soini <tis@foobar.fi>
 * Copyright (C) 2010 Avesh Agarwal <avagarwa@redhat.com>
 * Copyright (C) 2010-2019 D. Hugh Redelmeier <hugh@mimosa.com>
 * Copyright (C) 2012-2015 Paul Wouters <paul@libreswan.org>
 * Copyright (C) 2013 Kim B. Heino <b@bbbs.net>
 * Copyright (C) 2016-2022 Andrew Cagney
 * Copyright (C) 2019 Paul Wouters <pwouters@redhat.com>
 * Copyright (C) 2017 Mayank Totale <mtotale@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.  See <https://www.gnu.org/licenses/gpl2.txt>.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 */

#include <errno.h>
#include <stdio.h>
#include <sys/wait.h>		/* WIFEXITED() et.al. */

#include "ip_info.h"

#include "defs.h"
#include "state.h"
#include "connections.h"
#include "log.h"
#include "kernel.h"
#include "kernel_xfrm_interface.h"
#include "iface.h"
#include "keys.h"		/* for pluto_pubkeys */
#include "secrets.h"		/* for struct pubkey_list */

/*
 * Remove all characters but [-_.0-9a-zA-Z] from a character string.
 * Truncates the result if it would be too long.
 */

static void jam_clean_xauth_username(struct jambuf *buf,
				     const char *src,
				     struct logger *logger)
{
	bool changed = false;
	const char *dst = jambuf_cursor(buf);
	while (*src != '\0') {
		if ((*src >= '0' && *src <= '9') ||
		    (*src >= 'a' && *src <= 'z') ||
		    (*src >= 'A' && *src <= 'Z') ||
		    *src == '_' || *src == '-' || *src == '.') {
			jam_char(buf, *src);
		} else {
			changed = true;
		}
		src++;
	}
	if (changed || !jambuf_ok(buf)) {
		llog(RC_LOG, logger,
			    "Warning: XAUTH username changed from '%s' to '%s'",
			    src, dst);
	}
}

/*
 * fmt_common_shell_out: form the command string
 *
 * note: this mutates *st by calling get_sa_bundle_info().
 */

static bool fmt_common_shell_out(char *buf,
				 size_t blen,
				 const struct connection *c,
				 const struct spd_route *sr,
				 struct state *st)
{
	struct jambuf jb = array_as_jambuf(buf, blen);
	const bool tunneling = LIN(POLICY_TUNNEL, c->policy);

	/* macros to jam definitions of various forms */
#	define JDstr(name, string)  jam(&jb, name "='%s' ", string)
#	define JDuint(name, u)  jam(&jb, name "=%u ", u)
#	define JDuint64(name, u)  jam(&jb, name "=%" PRIu64 " ", u)
#	define JDemitter(name, emitter)  { jam_string(&jb, name "='"); emitter; jam_string(&jb, "' "); }
#	define JDipaddr(name, addr)  JDemitter(name, { ip_address ta = addr; jam_address(&jb, &ta); } )

	JDstr("PLUTO_CONNECTION", c->name);
	JDstr("PLUTO_CONNECTION_TYPE", (tunneling ? "tunnel" : "transport"));
	JDstr("PLUTO_VIRT_INTERFACE", (c->xfrmi != NULL && c->xfrmi->name != NULL) ?
		c->xfrmi->name : "NULL");
	JDstr("PLUTO_INTERFACE", c->interface == NULL ? "NULL" : c->interface->ip_dev->id_rname);
	JDstr("PLUTO_XFRMI_ROUTE",  (c->xfrmi != NULL && c->xfrmi->if_id > 0) ? "yes" : "");

	if (address_is_specified(sr->local->host->nexthop)) {
		JDipaddr("PLUTO_NEXT_HOP", sr->local->host->nexthop);
	}

	JDipaddr("PLUTO_ME", sr->local->host->addr);
	JDemitter("PLUTO_MY_ID", jam_id_bytes(&jb, &c->local->host.id, jam_shell_quoted_bytes));
	jam(&jb, "PLUTO_CLIENT_FAMILY='ipv%d' ", selector_info(sr->local->client)->ip_version);
	JDemitter("PLUTO_MY_CLIENT", jam_selector_subnet(&jb, &sr->local->client));
	JDipaddr("PLUTO_MY_CLIENT_NET", selector_prefix(sr->local->client));
	JDipaddr("PLUTO_MY_CLIENT_MASK", selector_prefix_mask(sr->local->client));

	if (cidr_is_specified(c->local->config->child.host_vtiip)) {
		JDemitter("VTI_IP", jam_cidr(&jb, &c->local->config->child.host_vtiip));
	}

	if (cidr_is_specified(c->local->config->child.ifaceip)) {
		JDemitter("INTERFACE_IP", jam_cidr(&jb, &c->local->config->child.ifaceip));
	}

	JDuint("PLUTO_MY_PORT", sr->local->client.hport);
	JDuint("PLUTO_MY_PROTOCOL", sr->local->client.ipproto);
	JDuint("PLUTO_SA_REQID", c->child.reqid);
	JDstr("PLUTO_SA_TYPE",
		st == NULL ? "none" :
		st->st_esp.present ? "ESP" :
		st->st_ah.present ? "AH" :
		st->st_ipcomp.present ? "IPCOMP" :
		"unknown?");

	JDipaddr("PLUTO_PEER", sr->remote->host->addr);
	JDemitter("PLUTO_PEER_ID", jam_id_bytes(&jb, &c->remote->host.id, jam_shell_quoted_bytes));

	/* for transport mode, things are complicated */
	jam_string(&jb, "PLUTO_PEER_CLIENT='");
	if (!tunneling && st != NULL && LHAS(st->hidden_variables.st_nat_traversal, NATED_PEER)) {
		/* pexpect(selector_eq_address(sr->remote->client, sr->remote->host->addr)); */
		jam_address(&jb, &sr->remote->host->addr);
		jam(&jb, "/%d", address_type(&sr->local->host->addr)->mask_cnt/*32 or 128*/);
	} else {
		jam_selector_subnet(&jb, &sr->remote->client);
	}
	jam_string(&jb, "' ");

	JDipaddr("PLUTO_PEER_CLIENT_NET",
		(!tunneling && st != NULL && LHAS(st->hidden_variables.st_nat_traversal, NATED_PEER)) ?
			sr->remote->host->addr : selector_prefix(sr->remote->client));

	JDipaddr("PLUTO_PEER_CLIENT_MASK", selector_prefix_mask(sr->remote->client));
	JDuint("PLUTO_PEER_PORT", sr->remote->client.hport);
	JDuint("PLUTO_PEER_PROTOCOL", sr->remote->client.ipproto);

	jam_string(&jb, "PLUTO_PEER_CA='");
	for (struct pubkey_list *p = pluto_pubkeys; p != NULL; p = p->next) {
		struct pubkey *key = p->key;
		int pathlen;	/* value ignored */
		if (key->content.type == &pubkey_type_rsa &&
		    same_id(&c->remote->host.id, &key->id) &&
		    trusted_ca(key->issuer, ASN1(sr->remote->host->config->ca), &pathlen)) {
			jam_dn_or_null(&jb, key->issuer, "", jam_shell_quoted_bytes);
			break;
		}
	}
	jam_string(&jb, "' ");

	JDstr("PLUTO_STACK", kernel_ops->updown_name);

	if (c->metric != 0) {
		jam(&jb, "PLUTO_METRIC=%d ", c->metric);
	}

	if (c->connmtu != 0) {
		jam(&jb, "PLUTO_MTU=%d ", c->connmtu);
	}

	JDuint64("PLUTO_ADDTIME", st == NULL ? (uint64_t)0 : st->st_esp.add_time);
	JDemitter("PLUTO_CONN_POLICY",	jam_connection_policies(&jb, c));
	JDemitter("PLUTO_CONN_KIND", jam_enum(&jb, &connection_kind_names, c->kind));
	jam(&jb, "PLUTO_CONN_ADDRFAMILY='ipv%d' ", address_type(&sr->local->host->addr)->ip_version);
	JDuint("XAUTH_FAILED", (st != NULL && st->st_xauth_soft) ? 1 : 0);

	if (st != NULL && st->st_xauth_username[0] != '\0') {
		JDemitter("PLUTO_USERNAME", jam_clean_xauth_username(&jb, st->st_xauth_username, st->st_logger));
	}

	ip_address sourceip = spd_end_sourceip(sr->local);
	if (sourceip.is_set) {
		JDipaddr("PLUTO_MY_SOURCEIP", sourceip);
		if (st != NULL) {
			JDstr("PLUTO_MOBIKE_EVENT",
			      st->st_mobike_del_src_ip ? "yes" : "");
		}
	}

	JDuint("PLUTO_IS_PEER_CISCO", c->remotepeertype /* ??? kind of odd printing an enum with %u */);
	JDstr("PLUTO_PEER_DNS_INFO", (st != NULL && st->st_seen_cfg_dns != NULL) ? st->st_seen_cfg_dns : "");
	JDstr("PLUTO_PEER_DOMAIN_INFO", (st != NULL && st->st_seen_cfg_domains != NULL) ? st->st_seen_cfg_domains : "");
	JDstr("PLUTO_PEER_BANNER", (st != NULL && st->st_seen_cfg_banner != NULL) ? st->st_seen_cfg_banner : "");
	JDuint("PLUTO_CFG_SERVER", sr->local->host->config->modecfg.server);
	JDuint("PLUTO_CFG_CLIENT", sr->local->host->config->modecfg.client);
#ifdef HAVE_NM
	JDuint("PLUTO_NM_CONFIGURED", c->nmconfigured);
#endif

	struct ipsec_proto_info *const first_ipsec_proto =
		(st == NULL ? NULL :
		 st->st_esp.present ? &st->st_esp :
		 st->st_ah.present ? &st->st_ah :
		 st->st_ipcomp.present ? &st->st_ipcomp :
		 NULL);

	if (first_ipsec_proto != NULL) {
		/*
		 * note: this mutates *st by calling get_sa_bundle_info
		 *
		 * XXX: does the get_sa_bundle_info() call order matter? Should this
		 * be a single "atomic" call?
		 */
		if (get_ipsec_traffic(st, first_ipsec_proto, DIRECTION_INBOUND)) {
			JDuint64("PLUTO_INBYTES", first_ipsec_proto->inbound.bytes);
		}
		if (get_ipsec_traffic(st, first_ipsec_proto, DIRECTION_OUTBOUND)) {
			JDuint64("PLUTO_OUTBYTES", first_ipsec_proto->outbound.bytes);
		}
	}

	if (c->nflog_group != 0) {
		jam(&jb, "NFLOG=%d ", c->nflog_group);
	}

	if (c->sa_marks.in.val != 0) {
		jam(&jb, "CONNMARK_IN=%" PRIu32 "/%#08" PRIx32 " ",
		    c->sa_marks.in.val, c->sa_marks.in.mask);
	}
	if (c->sa_marks.out.val != 0 && c->xfrmi == NULL) {
		jam(&jb, "CONNMARK_OUT=%" PRIu32 "/%#08" PRIx32 " ",
		    c->sa_marks.out.val, c->sa_marks.out.mask);
	}
	if (c->xfrmi != NULL) {
		if (c->sa_marks.out.val != 0) {
			/* user configured XFRMI_SET_MARK (a.k.a. output mark) add it */
			jam(&jb, "PLUTO_XFRMI_FWMARK='%" PRIu32 "/%#08" PRIx32 "' ",
			    c->sa_marks.out.val, c->sa_marks.out.mask);
		} else if (address_in_selector_range(sr->remote->host->addr, sr->remote->client)) {
			jam(&jb, "PLUTO_XFRMI_FWMARK='%" PRIu32 "/0xffffffff' ",
			    c->xfrmi->if_id);
		} else {
			address_buf bpeer;
			selector_buf peerclient_str;
			dbg("not adding PLUTO_XFRMI_FWMARK. PLUTO_PEER=%s is not inside PLUTO_PEER_CLIENT=%s",
			    str_address(&sr->remote->host->addr, &bpeer),
			    str_selector_subnet_port(&sr->remote->client, &peerclient_str));
			jam(&jb, "PLUTO_XFRMI_FWMARK='' ");
		}
	}
	JDstr("VTI_IFACE", c->vti_iface ? c->vti_iface : "");
	JDstr("VTI_ROUTING", bool_str(c->vti_routing));
	JDstr("VTI_SHARED", bool_str(c->vti_shared));

	if (c->local->child.has_cat) {
		jam_string(&jb, "CAT='YES' ");
	}

	jam(&jb, "SPI_IN=0x%x SPI_OUT=0x%x " /* SPI_IN SPI_OUT */,
		first_ipsec_proto == NULL ? 0 : ntohl(first_ipsec_proto->outbound.spi),
		first_ipsec_proto == NULL ? 0 : ntohl(first_ipsec_proto->inbound.spi));

	return jambuf_ok(&jb);

#	undef JDstr
#	undef JDuint
#	undef JDuint64
#	undef JDemitter
#	undef JDipaddr
}

static bool invoke_command(const char *verb, const char *verb_suffix, const char *cmd,
			   struct logger *logger)
{
#	define CHUNK_WIDTH	80	/* units for cmd logging */
	if (DBGP(DBG_BASE)) {
		int slen = strlen(cmd);
		int i;

		DBG_log("executing %s%s: %s",
			verb, verb_suffix, cmd);
		DBG_log("popen cmd is %d chars long", slen);
		for (i = 0; i < slen; i += CHUNK_WIDTH)
			DBG_log("cmd(%4d):%.*s:", i,
				slen-i < CHUNK_WIDTH? slen-i : CHUNK_WIDTH,
				&cmd[i]);
	}
#	undef CHUNK_WIDTH


	{
		/*
		 * invoke the script, catching stderr and stdout
		 * It may be of concern that some file descriptors will
		 * be inherited.  For the ones under our control, we
		 * have done fcntl(fd, F_SETFD, FD_CLOEXEC) to prevent this.
		 * Any used by library routines (perhaps the resolver or
		 * syslog) will remain.
		 */
		FILE *f = popen(cmd, "r");

		if (f == NULL) {
#ifdef HAVE_BROKEN_POPEN
			/*
			 * See bug #1067  Angstrom Linux on a arm7 has no
			 * popen()
			 */
			if (errno == ENOSYS) {
				/*
				 * Try system(), though it will not give us
				 * output
				 */
				DBG_log("unable to popen(), falling back to system()");
				system(cmd);
				return true;
			}
#endif
			llog(RC_LOG_SERIOUS, logger,
				    "unable to popen %s%s command",
				    verb, verb_suffix);
			return false;
		}

		/* log any output */
		for (;; ) {
			/*
			 * if response doesn't fit in this buffer, it will
			 * be folded
			 */
			char resp[256];

			if (fgets(resp, sizeof(resp), f) == NULL) {
				if (ferror(f)) {
					llog_error(logger, errno,
						   "fgets failed on output of %s%s command",
						   verb, verb_suffix);
					pclose(f);
					return false;
				} else {
					passert(feof(f));
					break;
				}
			} else {
				char *e = resp + strlen(resp);

				if (e > resp && e[-1] == '\n')
					e[-1] = '\0'; /* trim trailing '\n' */
				llog(RC_LOG, logger, "%s%s output: %s", verb,
					    verb_suffix, resp);
			}
		}

		/* report on and react to return code */
		{
			int r = pclose(f);

			if (r == -1) {
				llog_error(logger, errno,
					   "pclose failed for %s%s command",
					   verb, verb_suffix);
				return false;
			} else if (WIFEXITED(r)) {
				if (WEXITSTATUS(r) != 0) {
					llog(RC_LOG_SERIOUS, logger,
						    "%s%s command exited with status %d",
						    verb, verb_suffix,
						    WEXITSTATUS(r));
					return false;
				}
			} else if (WIFSIGNALED(r)) {
				llog(RC_LOG_SERIOUS, logger,
					    "%s%s command exited with signal %d",
					    verb, verb_suffix, WTERMSIG(r));
				return false;
			} else {
				llog(RC_LOG_SERIOUS, logger,
					    "%s%s command exited with unknown status %d",
					    verb, verb_suffix, r);
				return false;
			}
		}
	}
	return true;
}

static bool do_updown_verb(const char *verb,
			   const struct connection *c,
			   const struct spd_route *sr,
			   struct state *st,
			   /* either st, or c's logger */
			   struct logger *logger)
{
	/*
	 * Figure out which verb suffix applies.
	 */
	const char *verb_suffix;

	{
		const struct ip_info *host_afi = address_info(sr->local->host->addr);
		const struct ip_info *child_afi = selector_info(sr->local->client);
		if (host_afi == NULL || child_afi == NULL) {
			llog_pexpect(logger, HERE, "unknown address family");
			return false;
		}

		const char *hs;
		switch (host_afi->af) {
		case AF_INET:
			hs = "-host";
			break;
		case AF_INET6:
			hs = "-host-v6";
			break;
		default:
			bad_case(host_afi->af);
		}

		const char *cs;
		switch (child_afi->af) {
		case AF_INET:
			cs = "-client"; /* really child; legacy name */
			break;
		case AF_INET6:
			cs = "-client-v6"; /* really child; legacy name */
			break;
		default:
			bad_case(child_afi->af);
		}

		verb_suffix = selector_range_eq_address(sr->local->client, sr->local->host->addr) ? hs : cs;
	}

	dbg("kernel: command executing %s%s", verb, verb_suffix);

	char common_shell_out_str[2048];
	if (!fmt_common_shell_out(common_shell_out_str,
				  sizeof(common_shell_out_str), c, sr,
				  st)) {
		llog(RC_LOG_SERIOUS, logger,
			    "%s%s command too long!", verb,
			    verb_suffix);
		return false;
	}

	/* must free */
	char *cmd = alloc_printf("2>&1 "      /* capture stderr along with stdout */
				 "PLUTO_VERB='%s%s' "
				 "%s"         /* other stuff */
				 "%s",        /* actual script */
				 verb, verb_suffix,
				 common_shell_out_str,
				 c->local->config->child.updown);
	if (cmd == NULL) {
		llog(RC_LOG_SERIOUS, logger,
			    "%s%s command too long!", verb,
			    verb_suffix);
		return false;
	}

	bool ok = invoke_command(verb, verb_suffix, cmd, logger);
	pfree(cmd);
	return ok;
}

bool do_updown(enum updown updown_verb,
	       const struct connection *c,
	       const struct spd_route *spd,
	       struct state *st,
	       /* either st, or c's logger */
	       struct logger *logger)
{
#if 0
	/*
	 * Depending on context, logging for either the connection or
	 * the state?
	 *
	 * The sec_label code violates this expectation somehow.
	 */
	PEXPECT(logger, ((c != NULL && c->logger == logger) ||
			 (st != NULL && st->st_logger == logger)));
#endif

	const char *verb;
	switch (updown_verb) {
#define C(E,N) case E: verb = N; break
		C(UPDOWN_PREPARE, "prepare");
		C(UPDOWN_ROUTE, "route");
		C(UPDOWN_UNROUTE, "unroute");
		C(UPDOWN_UP, "up");
		C(UPDOWN_DOWN, "down");
#ifdef HAVE_NM
		C(UPDOWN_DISCONNECT_NM, "disconnectNM");
#endif
#undef C
	default:
		bad_case(updown_verb);
	}

	/*
	 * Support for skipping updown, eg leftupdown=""
	 * Useful on busy servers that do not need to use updown for anything
	 */
	const char *updown = c->local->config->child.updown;
	if (updown == NULL) {
		ldbg(logger, "kernel: skipped updown %s command - disabled per policy", verb);
		return true;
	}
	if (spd != NULL && c->spd != NULL && c->spd->spd_next != NULL) {
		/* i.e., more selectors than just this */
		selector_pair_buf sb;
		llog(RC_LOG, logger, "running updown %s %s", verb,
		     str_selector_pair(&spd->local->client, &spd->remote->client, &sb));
	} else {
		ldbg(logger, "kernel: running updown command \"%s\" for verb %s ", updown, verb);
	}

	return do_updown_verb(verb, c, spd, st, logger);
}
