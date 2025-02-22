/* Process IKE_SA_INIT payload, for libreswan
 *
 * Copyright (C) 2017-2019 Andrew Cagney <cagney@gnu.org>
 * Copyright (C) 2018 Sahana Prasad <sahana.prasad07@gmail.com>
 * Copyright (C) 2020 Yulia Kuzovkova <ukuzovkova@gmail.com>
 */

#ifndef IKEV2_IKE_SA_INIT_H
#define IKEV2_IKE_SA_INIT_H

struct msg_digest;
struct ike_sa;

void process_v2_IKE_SA_INIT(struct msg_digest *md);

extern void initiate_v2_IKE_SA_INIT_request(struct connection *c,
					    struct state *predecessor,
					    lset_t policy,
					    unsigned long try,
					    const threadtime_t *inception,
					    shunk_t sec_label,
					    bool background, struct logger *logger);

extern void process_v2_request_no_skeyseed(struct ike_sa *ike, struct msg_digest *md);
extern ikev2_state_transition_fn process_v2_IKE_SA_INIT_request;
extern ikev2_state_transition_fn process_v2_IKE_SA_INIT_response;
extern ikev2_state_transition_fn process_v2_IKE_SA_INIT_response_v2N_INVALID_KE_PAYLOAD;

bool record_v2_IKE_SA_INIT_request(struct ike_sa *ike);

void process_v2_ike_sa_init_request_timeout(struct ike_sa *ike, monotime_t now);

#endif
