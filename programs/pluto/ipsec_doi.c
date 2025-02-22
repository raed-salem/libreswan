/*
 * IPsec DOI and Oakley resolution routines
 *
 * Copyright (C) 1997 Angelos D. Keromytis.
 * Copyright (C) 1998-2002,2010-2017 D. Hugh Redelmeier <hugh@mimosa.com>
 * Copyright (C) 2003-2006  Michael Richardson <mcr@xelerance.com>
 * Copyright (C) 2003-2011 Paul Wouters <paul@xelerance.com>
 * Copyright (C) 2010-2011 Tuomo Soini <tis@foobar.fi>
 * Copyright (C) 2009 Avesh Agarwal <avagarwa@redhat.com>
 * Copyright (C) 2012-2018 Paul Wouters <pwouters@redhat.com>
 * Copyright (C) 2013 David McCullough <ucdevel@gmail.com>
 * Copyright (C) 2013 Matt Rogers <mrogers@redhat.com>
 * Copyright (C) 2014-2019 Andrew Cagney <cagney@gnu.org>
 * Copyright (C) 2017-2018 Antony Antony <antony@phenome.org>
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
 *
 */

#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <resolv.h>

#include "sysdep.h"
#include "constants.h"
#include "defs.h"
#include "state.h"
#include "id.h"
#include "x509.h"
#include "certs.h"
#include "connections.h"        /* needs id.h */
#include "packet.h"
#include "keys.h"
#include "demux.h"      /* needs packet.h */
#include "kernel.h"     /* needs connections.h */
#include "log.h"
#include "server.h"
#include "timer.h"
#include "rnd.h"
#include "ipsec_doi.h"  /* needs demux.h and state.h */
#include "ikev1_quick.h"
#include "whack.h"
#include "fetch.h"
#include "asn1.h"
#include "crypto.h"
#include "secrets.h"
#include "crypt_dh.h"
#include "ike_alg.h"
#include "ike_alg_integ.h"
#include "ike_alg_encrypt.h"
#include "kernel_alg.h"
#include "plutoalg.h"
#include "ikev1.h"
#include "ikev1_continuations.h"
#include "ikev2.h"
#include "ikev2_send.h"
#include "ikev1_xauth.h"
#include "ip_info.h"
#include "nat_traversal.h"
#include "ikev1_dpd.h"
#include "pluto_x509.h"
#include "ip_address.h"
#include "pluto_stats.h"
#include "chunk.h"
#include "pending.h"
#include "iface.h"
#include "ikev2_delete.h"	/* for record_v2_delete(); but call is dying */
#include "orient.h"
#include "initiate.h"
#include "ikev2_ike_sa_init.h"

/*
 * Start from policy in (ipsec) state, not connection.  This ensures
 * that rekeying doesn't downgrade security.  I admit that this
 * doesn't capture everything.
 */
lset_t capture_child_rekey_policy(struct state *st)
{
	lset_t policy = st->st_policy;

	if (st->st_pfs_group != NULL)
		policy |= POLICY_PFS;
	if (st->st_ah.present) {
		policy |= POLICY_AUTHENTICATE;
		if (st->st_ah.attrs.mode == ENCAPSULATION_MODE_TUNNEL)
			policy |= POLICY_TUNNEL;
	}
	if (st->st_esp.present &&
	    st->st_esp.attrs.transattrs.ta_encrypt != &ike_alg_encrypt_null) {
		policy |= POLICY_ENCRYPT;
		if (st->st_esp.attrs.mode == ENCAPSULATION_MODE_TUNNEL)
			policy |= POLICY_TUNNEL;
	}
	if (st->st_ipcomp.present) {
		policy |= POLICY_COMPRESS;
		if (st->st_ipcomp.attrs.mode == ENCAPSULATION_MODE_TUNNEL)
			policy |= POLICY_TUNNEL;
	}

	return policy;
}

void initialize_new_state(struct state *st,
			  lset_t policy,
			  int try)
{
	/*
	 * reset our choice of interface
	 *
	 * XXX: why? suspect this has the side effect of restoring /
	 * updating connection's ends?
	 */
	struct connection *c = st->st_connection;
	pexpect(oriented(c));
	iface_endpoint_delref(&c->interface);
	orient(c, st->st_logger);
	pexpect(st->st_interface == NULL); /* no-leak */
	st->st_interface = iface_endpoint_addref(c->interface);
	passert(st->st_interface != NULL);
	st->st_remote_endpoint = endpoint_from_address_protocol_port(c->remote->host.addr,
								     c->interface->io->protocol,
								     ip_hport(c->spd->remote->host->port));
	endpoint_buf eb;
	dbg("in %s with remote endpoint set to %s",
	    __func__, str_endpoint(&st->st_remote_endpoint, &eb));

	st->st_policy = policy & ~POLICY_IPSEC_MASK;        /* clear bits */
	st->st_try = try;

	for (const struct spd_route *sr = c->spd;
	     sr != NULL; sr = sr->spd_next) {
		if (sr->local->host->config->xauth.client) {
			if (sr->local->host->config->xauth.username != NULL) {
				jam_str(st->st_xauth_username,
					sizeof(st->st_xauth_username),
					sr->local->host->config->xauth.username);
				break;
			}
		}
	}

	binlog_refresh_state(st);
}

void jam_child_sa_details(struct jambuf *buf, struct state *st)
{
	struct connection *const c = st->st_connection;
	const char *ini = "{";

	if (st->st_esp.present) {
		jam_string(buf, ini);
		ini = " ";
		bool nat = (st->hidden_variables.st_nat_traversal & NAT_T_DETECTED) != 0;
		bool tfc = c->sa_tfcpad != 0 && !st->st_seen_no_tfc;
		bool esn = st->st_esp.attrs.transattrs.esn_enabled;
		bool tcp = st->st_interface->io->protocol == &ip_protocol_tcp;

		if (nat)
			dbg("NAT-T: NAT Traversal detected - their IKE port is '%d'",
			     c->spd->remote->host->port);

		dbg("NAT-T: encaps is '%s'",
		     c->encaps == yna_auto ? "auto" : bool_str(c->encaps == yna_yes));

		jam(buf, "ESP%s%s%s=>0x%08" PRIx32 " <0x%08" PRIx32 "",
		    tcp ? "inTCP" : nat ? "inUDP" : "",
		    esn ? "/ESN" : "",
		    tfc ? "/TFC" : "",
		    ntohl(st->st_esp.outbound.spi),
		    ntohl(st->st_esp.inbound.spi));
		jam(buf, " xfrm=%s", st->st_esp.attrs.transattrs.ta_encrypt->common.fqn);
		/* log keylen when it is required and/or "interesting" */
		if (!st->st_esp.attrs.transattrs.ta_encrypt->keylen_omitted ||
		    (st->st_esp.attrs.transattrs.enckeylen != 0 &&
		     st->st_esp.attrs.transattrs.enckeylen != st->st_esp.attrs.transattrs.ta_encrypt->keydeflen)) {
			jam(buf, "_%u", st->st_esp.attrs.transattrs.enckeylen);
		}
		jam(buf, "-%s", st->st_esp.attrs.transattrs.ta_integ->common.fqn);

		if ((st->st_ike_version == IKEv2) && st->st_pfs_group != NULL) {
			jam_string(buf, "-");
			jam_string(buf, st->st_pfs_group->common.fqn);
		}
	}

	if (st->st_ah.present) {
		jam_string(buf, ini);
		ini = " ";
		jam(buf, "AH%s=>0x%08" PRIx32 " <0x%08" PRIx32 " xfrm=%s",
		    st->st_ah.attrs.transattrs.esn_enabled ? "/ESN" : "",
		    ntohl(st->st_ah.outbound.spi),
		    ntohl(st->st_ah.inbound.spi),
		    st->st_ah.attrs.transattrs.ta_integ->common.fqn);
	}

	if (st->st_ipcomp.present) {
		jam_string(buf, ini);
		ini = " ";
		jam(buf, "IPCOMP=>0x%08" PRIx32 " <0x%08" PRIx32,
		    ntohl(st->st_ipcomp.outbound.spi),
		    ntohl(st->st_ipcomp.inbound.spi));
	}

	if (address_is_specified(st->hidden_variables.st_nat_oa)) {
		jam_string(buf, ini);
		ini = " ";
		jam_string(buf, "NATOA=");
		jam_address_sensitive(buf, &st->hidden_variables.st_nat_oa);
	}

	if (address_is_specified(st->hidden_variables.st_natd)) {
		jam_string(buf, ini);
		ini = " ";
		jam_string(buf, "NATD=");
		jam_address_sensitive(buf, &st->hidden_variables.st_natd);
		jam(buf, ":%d", endpoint_hport(st->st_remote_endpoint));
	}

	jam_string(buf, ini);
	ini = " ";
	jam_string(buf, "DPD=");
	if (st->st_ike_version == IKEv1 && !st->hidden_variables.st_peer_supports_dpd) {
		jam_string(buf, "unsupported");
	} else if (dpd_active_locally(st->st_connection)) {
		jam_string(buf, "active");
	} else {
		jam_string(buf, "passive");
	}

	if (st->st_xauth_username[0] != '\0') {
		jam_string(buf, ini);
		ini = " ";
		jam_string(buf, "username=");
		jam_string(buf, st->st_xauth_username);
	}

	jam_string(buf, "}");
}

void jam_parent_sa_details(struct jambuf *buf, struct state *st)
{
	passert(st->st_oakley.ta_encrypt != NULL);
	passert(st->st_oakley.ta_prf != NULL);
	passert(st->st_oakley.ta_dh != NULL);

	jam_string(buf, "{");

	if (st->st_ike_version == IKEv1) {
		jam(buf, "auth=");
		jam_enum_short(buf, &oakley_auth_names, st->st_oakley.auth);
		jam(buf, " ");
	}

	jam(buf, "cipher=%s", st->st_oakley.ta_encrypt->common.fqn);
	if (st->st_oakley.enckeylen > 0) {
		/* XXX: also check omit key? */
		jam(buf, "_%d", st->st_oakley.enckeylen);
	}

	/*
	 * Note: for IKEv1 and AEAD encrypters,
	 * st->st_oakley.ta_integ is 'none'!
	 */
	jam_string(buf, " integ=");
	if (st->st_ike_version == IKEv2) {
		if (st->st_oakley.ta_integ == &ike_alg_integ_none) {
			jam_string(buf, "n/a");
		} else {
			jam_string(buf, st->st_oakley.ta_integ->common.fqn);
		}
	} else {
		/*
		 * For IKEv1, since the INTEG algorithm is potentially
		 * (always?) NULL.  Display the PRF.  The choice and
		 * behaviour are historic.
		 */
		jam_string(buf, st->st_oakley.ta_prf->common.fqn);
	}

	if (st->st_ike_version == IKEv2) {
		jam(buf, " prf=%s", st->st_oakley.ta_prf->common.fqn);
	}

	jam(buf, " group=%s}", st->st_oakley.ta_dh->common.fqn);
}
