/*
 * Retry (and retransmit), for libreswan
 *
 * Copyright (C) 2018 Andrew Cagney
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

#ifndef IKEV2_RETRY_H
#define IKEV2_RETRY_H

#include "monotime.h"

struct state;
struct ike_sa;

void event_v2_retransmit(struct state *st, monotime_t now);
void process_v2_ike_sa_established_request_timeout(struct ike_sa *ike, monotime_t now);

#endif
