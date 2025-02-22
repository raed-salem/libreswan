/* Connection database indexed by serialno, for libreswan
 *
 * Copyright (C) 2020 Andrew Cagney <cagney@gnu.org>
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

#include "connection_db.h"
#include "connections.h"
#include "log.h"
#include "hash_table.h"
#include "refcnt.h"
#include "virtual_ip.h"		/* for virtual_ip_addref() */

/*
 * A table hashed by serialno.
 */

static void jam_connection_serialno(struct jambuf *buf, const struct connection *c)
{
	jam_connection(buf, c);
	jam(buf, " "PRI_CO, pri_co(c->serialno));
}

static hash_t hash_connection_serialno(const co_serial_t *serialno)
{
	return hash_thing(*serialno, zero_hash);
}

HASH_TABLE(connection, serialno, .serialno, STATE_TABLE_SIZE);

struct connection *connection_by_serialno(co_serial_t serialno)
{
	/*
	 * Note that since SOS_NOBODY is never hashed, a lookup of
	 * SOS_NOBODY always returns NULL.
	 */
	struct connection *c;
	hash_t hash = hash_connection_serialno(&serialno);
	struct list_head *bucket = hash_table_bucket(&connection_serialno_hash_table, hash);
	FOR_EACH_LIST_ENTRY_NEW2OLD(bucket, c) {
		if (c->serialno == serialno) {
			return c;
		}
	}
	return NULL;
}

/*
 * An ID hash table.
 */

static hash_t hash_connection_that_id(const struct id *id)
{
	hash_t hash = zero_hash;
	if (id->kind != ID_NONE) {
		shunk_t body;
		enum ike_id_type type = id_to_payload(id, &unset_address/*ignored*/, &body);
		hash = hash_thing(type, hash);
		hash = hash_hunk(body, hash);
	}
	return hash;
}

static void jam_connection_that_id(struct jambuf *buf, const struct connection *c)
{
	jam_connection_serialno(buf, c);
	jam(buf, ": that_id=");
	jam_id_bytes(buf, &c->remote->host.id, jam_sanitized_bytes);
}

HASH_TABLE(connection, that_id, .remote->host.id, STATE_TABLE_SIZE);

void rehash_db_connection_that_id(struct connection *c)
{
	id_buf idb;
	dbg("%s() rehashing "PRI_CO" that_id=%s",
	    __func__, pri_co(c->serialno), str_id(&c->remote->host.id, &idb));
	rehash_table_entry(&connection_that_id_hash_table, c);
}

void replace_connection_that_id(struct connection *c, const struct id *src)
{
	struct id *dst = &c->remote->host.id;
	passert(dst->name.ptr == NULL || dst->name.ptr != src->name.ptr);
	free_id_content(dst);
	*dst = clone_id(src, "replaing connection id");
	rehash_db_connection_that_id(c);
}

/*
 * SPD_ROUTE database.
 */

static void jam_spd_route(struct jambuf *buf, const struct spd_route *sr)
{
	jam_connection(buf, sr->connection);
	jam_string(buf, " ");
	jam_selector_pair(buf, &sr->local->client, &sr->remote->client);
}

static void jam_spd_route_remote_client(struct jambuf *buf, const struct spd_route *sr)
{
	jam_spd_route(buf, sr);
}

static hash_t hash_spd_route_remote_client(const ip_selector *sr)
{
	return hash_thing(sr->bytes, zero_hash);
}

void spd_route_db_add_connection(struct connection *c)
{
	for (struct spd_route *spd = c->spd; spd != NULL; spd = spd->spd_next) {
		spd_route_db_add(spd);
	}
}

HASH_TABLE(spd_route, remote_client, .remote->client, STATE_TABLE_SIZE);

HASH_DB(spd_route, &spd_route_remote_client_hash_table);

void rehash_db_spd_route_remote_client(struct spd_route *sr)
{
	rehash_table_entry(&spd_route_remote_client_hash_table, sr);
}

static struct list_head *spd_route_filter_head(struct spd_route_filter *filter)
{
	/* select list head */
	if (filter->remote_client_range != NULL) {
		selector_buf sb;
		dbg("FOR_EACH_SPD_ROUTE[remote_client_range=%s]... in "PRI_WHERE,
		    str_selector(filter->remote_client_range, &sb), pri_where(filter->where));
		hash_t hash = hash_spd_route_remote_client(filter->remote_client_range);
		return hash_table_bucket(&spd_route_remote_client_hash_table, hash);
	}

	/* else other queries? */
	dbg("FOR_EACH_SPD_ROUTE_... in "PRI_WHERE, pri_where(filter->where));
	return &spd_route_db_list_head;
}

static bool matches_spd_route_filter(struct spd_route *spd, struct spd_route_filter *filter)
{
	if (filter->remote_client_range != NULL &&
	    !selector_range_eq_selector_range(*filter->remote_client_range, spd->remote->client)) {
		return false;
	}
	return true;
}

bool next_spd_route(enum chrono order, struct spd_route_filter *filter)
{
	if (filter->internal == NULL) {
		/*
		 * Advance to first entry of the circular list (if the
		 * list is entry it ends up back on HEAD which has no
		 * data).
		 */
		filter->internal = spd_route_filter_head(filter)->head.next[order];
	}
	/* Walk list until an entry matches */
	filter->spd = NULL;
	for (struct list_entry *entry = filter->internal;
	     entry->data != NULL /* head has DATA == NULL */;
	     entry = entry->next[order]) {
		struct spd_route *spd = (struct spd_route *) entry->data;
		if (matches_spd_route_filter(spd, filter)) {
			/* save connection; but step off current entry */
			filter->internal = entry->next[order];
			filter->count++;
			LDBGP_JAMBUF(DBG_BASE, &global_logger, buf) {
				jam_string(buf, "  found ");
				jam_spd_route(buf, spd);
			}
			filter->spd = spd;
			return true;
		}
	}
	dbg("  matches: %d", filter->count);
	return false;
}

/*
 * Maintain the contents of the hash tables.
 */

HASH_DB(connection,
	&connection_serialno_hash_table,
	&connection_that_id_hash_table);

/*
 * Allocate connections.
 */

void finish_connection(struct connection *c, const char *name,
		       struct connection *t,
		       lset_t debugging, struct fd *whackfd,
		       where_t where)
{
	/* announce it (before code below logs its address) */
	dbg_alloc(name, c, where);

	c->name = clone_str(name, __func__);
	c->logger = alloc_logger(c, &logger_connection_vec,
				 debugging, whackfd, where);

	/*
	 * If there's no template to use as a reference point, LOCAL
	 * and REMOTE are disoriented so distinguishing one as local
	 * and the other as remote is pretty much meaningless.
	 *
	 * Somewhat arbitrarially (as in this is the way it's always
	 * been) start with:
	 *
	 *    LEFT == LOCAL / THIS
	 *    RIGHT == REMOTE / THAT
	 *
	 * Needed by the hash table code that expects .that->host.id
	 * to work.
	 */

	enum left_right local = (t == NULL ? LEFT_END : t->local->config->index);
	enum left_right remote = (t == NULL ? RIGHT_END : t->remote->config->index);
	c->local = &c->end[local];	/* this; clone must update */
	c->remote = &c->end[remote];	/* that; clone must update */

	/* somewhat oriented can start hashing */
	connection_db_init_connection(c);

	/*
	 * Update counter, set serialno and add to serialno list.
	 *
	 * The connection will be hashed after the caller has finished
	 * populating it.
	 */
	static co_serial_t connection_serialno;
	connection_serialno++;
	passert(connection_serialno > 0); /* can't overflow */
	c->serialno = connection_serialno;
	c->serial_from = (t != NULL ? t->serialno : UNSET_CO_SERIAL);
}

struct connection *alloc_connection(const char *name,
				    lset_t debugging, struct fd *whackfd,
				    where_t where)
{
	struct connection *c = alloc_thing(struct connection, where->func);
	finish_connection(c, name, NULL/*no template*/,
			  debugging, whackfd, where);

	/*
	 * Allocate the configuration - only allocated on root
	 * connection; connection instances (clones) inherit these
	 * pointers.
	 */

	struct config *config = alloc_thing(struct config, "root config");
	c->config = c->root_config = config;

	FOR_EACH_THING(lr, LEFT_END, RIGHT_END) {
		/* "left" or "right" */
		const char *leftright =
			(lr == LEFT_END ? "left" :
			 lr == RIGHT_END ? "right" :
			 NULL);
		passert(leftright != NULL);
		struct connection_end *end = &c->end[lr];
		struct config_end *end_config = &config->end[lr];
		end_config->leftright = leftright;
		end_config->index = lr;
		end_config->host.leftright = leftright;
		end_config->child.leftright = leftright;
		end->config = end_config;
		end->host.config = &end_config->host;
		end->child.config = &end_config->child;
	}

	return c;
}

struct spd_route *append_spd(struct connection *c, struct spd_route ***spd_end)
{
	struct spd_route *spd = alloc_thing(struct spd_route, "spd_route");
	/* append; too much redirection */
	passert(**spd_end == NULL);
	**spd_end = spd;
	*spd_end = &spd->spd_next;
	passert(**spd_end == NULL);
	/* back link */
	spd->connection = c;
	/* local link */
	spd->local = &spd->end[c->local->config->index];	/*clone must update*/
	spd->remote = &spd->end[c->remote->config->index];	/*clone must update*/
	FOR_EACH_THING(end, LEFT_END, RIGHT_END) {
		spd->end[end].config = c->end[end].config;
		spd->end[end].host = &c->end[end].host;		/*clone must update*/
		spd->end[end].child = &c->end[end].child;	/*clone must update*/
	}
	/* db; will be updated */
	spd_route_db_init_spd_route(spd);
	return spd;
}

/*
 * See also {new2old,old2new}_state()
 */

static struct list_head *connection_filter_head(struct connection_filter *filter)
{
	if (filter->that_id_eq != NULL) {
		id_buf idb;
		dbg("FOR_EACH_CONNECTION[that_id_eq=%s].... in "PRI_WHERE,
		    str_id(filter->that_id_eq, &idb), pri_where(filter->where));
		hash_t hash = hash_connection_that_id(filter->that_id_eq);
		return hash_table_bucket(&connection_that_id_hash_table, hash);
	}

	dbg("FOR_EACH_CONNECTION_.... in "PRI_WHERE, pri_where(filter->where));
	return &connection_db_list_head;
}

static bool matches_connection_filter(struct connection *c, struct connection_filter *filter)
{
	if (filter->kind != 0 && filter->kind != c->kind) {
		return false;
	}
	if (filter->name != NULL && !streq(filter->name, c->name)) {
		return false;
	}
	if (filter->this_id_eq != NULL && !id_eq(filter->this_id_eq, &c->local->host.id)) {
		return false;
	}
	if (filter->that_id_eq != NULL && !id_eq(filter->that_id_eq, &c->remote->host.id)) {
		return false;
	}
	return true; /* sure */
}

static bool next_connection(enum chrono adv, struct connection_filter *filter)
{
	if (filter->internal == NULL) {
		/*
		 * Advance to first entry of the circular list (if the
		 * list is entry it ends up back on HEAD which has no
		 * data).
		 */
		filter->internal = connection_filter_head(filter)->head.next[adv];
	}
	/* Walk list until an entry matches */
	filter->c = NULL;
	for (struct list_entry *entry = filter->internal;
	     entry->data != NULL /* head has DATA == NULL */;
	     entry = entry->next[adv]) {
		struct connection *c = (struct connection *) entry->data;
		if (matches_connection_filter(c, filter)) {
			/* save connection; but step off current entry */
			filter->internal = entry->next[adv];
			filter->count++;
			LDBGP_JAMBUF(DBG_BASE, &global_logger, buf) {
				jam_string(buf, "  found ");
				jam_connection(buf, c);
			}
			filter->c = c;
			return true;
		}
	}
	dbg("  matches: %d", filter->count);
	return false;
}

bool next_connection_old2new(struct connection_filter *filter)
{
	return next_connection(OLD2NEW, filter);
}

bool next_connection_new2old(struct connection_filter *filter)
{
	return next_connection(NEW2OLD, filter);
}
