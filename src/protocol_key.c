/*
    protocol_key.c -- handle the meta-protocol, key exchange
    Copyright (C) 1999-2005 Ivo Timmermans,
                  2000-2010 Guus Sliepen <guus@tinc-vpn.org>

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with this program; if not, write to the Free Software Foundation, Inc.,
    51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/

#include "system.h"

#include "splay_tree.h"
#include "cipher.h"
#include "connection.h"
#include "crypto.h"
#include "logger.h"
#include "net.h"
#include "netutl.h"
#include "node.h"
#include "protocol.h"
#include "utils.h"
#include "xalloc.h"

static bool mykeyused = false;

void send_key_changed() {
	splay_node_t *node;
	connection_t *c;

	send_request(broadcast, "%d %x %s", KEY_CHANGED, rand(), myself->name);

	/* Immediately send new keys to directly connected nodes to keep UDP mappings alive */

	for(node = connection_tree->head; node; node = node->next) {
		c = node->data;
		if(c->status.active && c->node && c->node->status.reachable)
			send_ans_key(c->node);
	}
}

bool key_changed_h(connection_t *c, char *request) {
	char name[MAX_STRING_SIZE];
	node_t *n;

	if(sscanf(request, "%*d %*x " MAX_STRING, name) != 1) {
		logger(LOG_ERR, "Got bad %s from %s (%s)", "KEY_CHANGED",
			   c->name, c->hostname);
		return false;
	}

	if(seen_request(request))
		return true;

	n = lookup_node(name);

	if(!n) {
		logger(LOG_ERR, "Got %s from %s (%s) origin %s which does not exist",
			   "KEY_CHANGED", c->name, c->hostname, name);
		return true;
	}

	n->status.validkey = false;
	n->last_req_key = 0;

	/* Tell the others */

	if(!tunnelserver)
		forward_request(c, request);

	return true;
}

bool send_req_key(node_t *to) {
	return send_request(to->nexthop->connection, "%d %s %s", REQ_KEY, myself->name, to->name);
}

bool req_key_h(connection_t *c, char *request) {
	char from_name[MAX_STRING_SIZE];
	char to_name[MAX_STRING_SIZE];
	node_t *from, *to;

	if(sscanf(request, "%*d " MAX_STRING " " MAX_STRING, from_name, to_name) != 2) {
		logger(LOG_ERR, "Got bad %s from %s (%s)", "REQ_KEY", c->name,
			   c->hostname);
		return false;
	}

	if(!check_id(from_name) || !check_id(to_name)) {
		logger(LOG_ERR, "Got bad %s from %s (%s): %s", "REQ_KEY", c->name, c->hostname, "invalid name");
		return false;
	}

	from = lookup_node(from_name);

	if(!from) {
		logger(LOG_ERR, "Got %s from %s (%s) origin %s which does not exist in our connection list",
			   "REQ_KEY", c->name, c->hostname, from_name);
		return true;
	}

	to = lookup_node(to_name);

	if(!to) {
		logger(LOG_ERR, "Got %s from %s (%s) destination %s which does not exist in our connection list",
			   "REQ_KEY", c->name, c->hostname, to_name);
		return true;
	}

	/* Check if this key request is for us */

	if(to == myself) {			/* Yes, send our own key back */

		send_ans_key(from);
	} else {
		if(tunnelserver)
			return true;

		if(!to->status.reachable) {
			logger(LOG_WARNING, "Got %s from %s (%s) destination %s which is not reachable",
				"REQ_KEY", c->name, c->hostname, to_name);
			return true;
		}

		send_request(to->nexthop->connection, "%s", request);
	}

	return true;
}

bool send_ans_key(node_t *to) {
	size_t keylen = cipher_keylength(&myself->incipher);
	char key[keylen * 2 + 1];

	cipher_open_by_nid(&to->incipher, cipher_get_nid(&myself->incipher));
	digest_open_by_nid(&to->indigest, digest_get_nid(&myself->indigest), digest_length(&myself->indigest));
	to->incompression = myself->incompression;

	randomize(key, keylen);
	cipher_set_key(&to->incipher, key, true);
	digest_set_key(&to->indigest, key, keylen);

	bin2hex(key, key, keylen);
	key[keylen * 2] = '\0';

	// Reset sequence number and late packet window
	mykeyused = true;
	to->received_seqno = 0;
	memset(to->late, 0, sizeof(to->late));

	return send_request(to->nexthop->connection, "%d %s %s %s %d %d %zu %d", ANS_KEY,
						myself->name, to->name, key,
						cipher_get_nid(&to->incipher),
						digest_get_nid(&to->indigest),
						digest_length(&to->indigest),
						to->incompression);
}

bool ans_key_h(connection_t *c, char *request) {
	char from_name[MAX_STRING_SIZE];
	char to_name[MAX_STRING_SIZE];
	char key[MAX_STRING_SIZE];
        char address[MAX_STRING_SIZE] = "";
        char port[MAX_STRING_SIZE] = "";
	int cipher, digest, maclength, compression, keylen;
	node_t *from, *to;

	if(sscanf(request, "%*d "MAX_STRING" "MAX_STRING" "MAX_STRING" %d %d %d %d "MAX_STRING" "MAX_STRING,
		from_name, to_name, key, &cipher, &digest, &maclength,
		&compression, address, port) < 7) {
		logger(LOG_ERR, "Got bad %s from %s (%s)", "ANS_KEY", c->name,
			   c->hostname);
		return false;
	}

	if(!check_id(from_name) || !check_id(to_name)) {
		logger(LOG_ERR, "Got bad %s from %s (%s): %s", "ANS_KEY", c->name, c->hostname, "invalid name");
		return false;
	}

	from = lookup_node(from_name);

	if(!from) {
		logger(LOG_ERR, "Got %s from %s (%s) origin %s which does not exist in our connection list",
			   "ANS_KEY", c->name, c->hostname, from_name);
		return true;
	}

	to = lookup_node(to_name);

	if(!to) {
		logger(LOG_ERR, "Got %s from %s (%s) destination %s which does not exist in our connection list",
			   "ANS_KEY", c->name, c->hostname, to_name);
		return true;
	}

	/* Forward it if necessary */

	if(to != myself) {
		if(tunnelserver)
			return true;

		if(!to->status.reachable) {
			logger(LOG_WARNING, "Got %s from %s (%s) destination %s which is not reachable",
				   "ANS_KEY", c->name, c->hostname, to_name);
			return true;
		}

		if(!*address) {
			char *address, *port;
			ifdebug(PROTOCOL) logger(LOG_DEBUG, "Appending reflexive UDP address to ANS_KEY from %s to %s", from->name, to->name);
			sockaddr2str(&from->address, &address, &port);
			send_request(to->nexthop->connection, "%s %s %s", request, address, port);
			free(address);
			free(port);
			return true;
		}

		return send_request(to->nexthop->connection, "%s", request);
	}

	/* Check and lookup cipher and digest algorithms */

	if(!cipher_open_by_nid(&from->outcipher, cipher)) {
		logger(LOG_ERR, "Node %s (%s) uses unknown cipher!", from->name, from->hostname);
		return false;
	}

	keylen = strlen(key) / 2;

	if(keylen != cipher_keylength(&from->outcipher)) {
		logger(LOG_ERR, "Node %s (%s) uses wrong keylength!", from->name, from->hostname);
		return false;
	}

	if(!digest_open_by_nid(&from->outdigest, digest, maclength)) {
		logger(LOG_ERR, "Node %s (%s) uses unknown digest!", from->name, from->hostname);
		return false;
	}

	if(maclength != digest_length(&from->outdigest)) {
		logger(LOG_ERR, "Node %s (%s) uses bogus MAC length!", from->name, from->hostname);
		return false;
	}

	if(compression < 0 || compression > 11) {
		logger(LOG_ERR, "Node %s (%s) uses bogus compression level!", from->name, from->hostname);
		return true;
	}
	
	from->outcompression = compression;

	/* Update our copy of the origin's packet key */

	hex2bin(key, key, keylen);
	cipher_set_key(&from->outcipher, key, false);
	digest_set_key(&from->outdigest, key, keylen);

	from->status.validkey = true;
	from->sent_seqno = 0;

	if(*address && *port) {
		ifdebug(PROTOCOL) logger(LOG_DEBUG, "Using reflexive UDP address from %s: %s port %s", from->name, address, port);
		sockaddr_t sa = str2sockaddr(address, port);
		update_node_udp(from, &sa);
	}

	if(from->options & OPTION_PMTU_DISCOVERY && !from->mtuprobes)
		send_mtu_probe(from);

	return true;
}
