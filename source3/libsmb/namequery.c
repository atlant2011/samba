/*
   Unix SMB/CIFS implementation.
   name query routines
   Copyright (C) Andrew Tridgell 1994-1998
   Copyright (C) Jeremy Allison 2007.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "includes.h"
#include "../lib/util/tevent_ntstatus.h"
#include "libads/sitename_cache.h"
#include "libads/dns.h"
#include "../libcli/netlogon/netlogon.h"
#include "lib/async_req/async_sock.h"
#include "libsmb/nmblib.h"

/* nmbd.c sets this to True. */
bool global_in_nmbd = False;

/****************************
 * SERVER AFFINITY ROUTINES *
 ****************************/

 /* Server affinity is the concept of preferring the last domain
    controller with whom you had a successful conversation */

/****************************************************************************
****************************************************************************/
#define SAFKEY_FMT	"SAF/DOMAIN/%s"
#define SAF_TTL		900
#define SAFJOINKEY_FMT	"SAFJOIN/DOMAIN/%s"
#define SAFJOIN_TTL	3600

static char *saf_key(TALLOC_CTX *mem_ctx, const char *domain)
{
	return talloc_asprintf_strupper_m(mem_ctx, SAFKEY_FMT, domain);
}

static char *saf_join_key(TALLOC_CTX *mem_ctx, const char *domain)
{
	return talloc_asprintf_strupper_m(mem_ctx, SAFJOINKEY_FMT, domain);
}

/****************************************************************************
****************************************************************************/

bool saf_store( const char *domain, const char *servername )
{
	char *key;
	time_t expire;
	bool ret = False;

	if ( !domain || !servername ) {
		DEBUG(2,("saf_store: "
			"Refusing to store empty domain or servername!\n"));
		return False;
	}

	if ( (strlen(domain) == 0) || (strlen(servername) == 0) ) {
		DEBUG(0,("saf_store: "
			"refusing to store 0 length domain or servername!\n"));
		return False;
	}

	key = saf_key(talloc_tos(), domain);
	if (key == NULL) {
		DEBUG(1, ("saf_key() failed\n"));
		return false;
	}
	expire = time( NULL ) + lp_parm_int(-1, "saf","ttl", SAF_TTL);

	DEBUG(10,("saf_store: domain = [%s], server = [%s], expire = [%u]\n",
		domain, servername, (unsigned int)expire ));

	ret = gencache_set( key, servername, expire );

	TALLOC_FREE( key );

	return ret;
}

bool saf_join_store( const char *domain, const char *servername )
{
	char *key;
	time_t expire;
	bool ret = False;

	if ( !domain || !servername ) {
		DEBUG(2,("saf_join_store: Refusing to store empty domain or servername!\n"));
		return False;
	}

	if ( (strlen(domain) == 0) || (strlen(servername) == 0) ) {
		DEBUG(0,("saf_join_store: refusing to store 0 length domain or servername!\n"));
		return False;
	}

	key = saf_join_key(talloc_tos(), domain);
	if (key == NULL) {
		DEBUG(1, ("saf_join_key() failed\n"));
		return false;
	}
	expire = time( NULL ) + lp_parm_int(-1, "saf","join ttl", SAFJOIN_TTL);

	DEBUG(10,("saf_join_store: domain = [%s], server = [%s], expire = [%u]\n",
		domain, servername, (unsigned int)expire ));

	ret = gencache_set( key, servername, expire );

	TALLOC_FREE( key );

	return ret;
}

bool saf_delete( const char *domain )
{
	char *key;
	bool ret = False;

	if ( !domain ) {
		DEBUG(2,("saf_delete: Refusing to delete empty domain\n"));
		return False;
	}

	key = saf_join_key(talloc_tos(), domain);
	if (key == NULL) {
		DEBUG(1, ("saf_join_key() failed\n"));
		return false;
	}
	ret = gencache_del(key);
	TALLOC_FREE(key);

	if (ret) {
		DEBUG(10,("saf_delete[join]: domain = [%s]\n", domain ));
	}

	key = saf_key(talloc_tos(), domain);
	if (key == NULL) {
		DEBUG(1, ("saf_key() failed\n"));
		return false;
	}
	ret = gencache_del(key);
	TALLOC_FREE(key);

	if (ret) {
		DEBUG(10,("saf_delete: domain = [%s]\n", domain ));
	}

	return ret;
}

/****************************************************************************
****************************************************************************/

char *saf_fetch( const char *domain )
{
	char *server = NULL;
	time_t timeout;
	bool ret = False;
	char *key = NULL;

	if ( !domain || strlen(domain) == 0) {
		DEBUG(2,("saf_fetch: Empty domain name!\n"));
		return NULL;
	}

	key = saf_join_key(talloc_tos(), domain);
	if (key == NULL) {
		DEBUG(1, ("saf_join_key() failed\n"));
		return NULL;
	}

	ret = gencache_get( key, &server, &timeout );

	TALLOC_FREE( key );

	if ( ret ) {
		DEBUG(5,("saf_fetch[join]: Returning \"%s\" for \"%s\" domain\n",
			server, domain ));
		return server;
	}

	key = saf_key(talloc_tos(), domain);
	if (key == NULL) {
		DEBUG(1, ("saf_key() failed\n"));
		return NULL;
	}

	ret = gencache_get( key, &server, &timeout );

	TALLOC_FREE( key );

	if ( !ret ) {
		DEBUG(5,("saf_fetch: failed to find server for \"%s\" domain\n",
					domain ));
	} else {
		DEBUG(5,("saf_fetch: Returning \"%s\" for \"%s\" domain\n",
			server, domain ));
	}

	return server;
}

static void set_socket_addr_v4(struct sockaddr_storage *addr)
{
	if (!interpret_string_addr(addr, lp_socket_address(),
				   AI_NUMERICHOST|AI_PASSIVE)) {
		zero_sockaddr(addr);
	}
	if (addr->ss_family != AF_INET) {
		zero_sockaddr(addr);
	}
}

static struct in_addr my_socket_addr_v4(void)
{
	struct sockaddr_storage my_addr;
	struct sockaddr_in *in_addr = (struct sockaddr_in *)((char *)&my_addr);

	set_socket_addr_v4(&my_addr);
	return in_addr->sin_addr;
}

/****************************************************************************
 Generate a random trn_id.
****************************************************************************/

static int generate_trn_id(void)
{
	uint16 id;

	generate_random_buffer((uint8 *)&id, sizeof(id));

	return id % (unsigned)0x7FFF;
}

/****************************************************************************
 Parse a node status response into an array of structures.
****************************************************************************/

static struct node_status *parse_node_status(TALLOC_CTX *mem_ctx, char *p,
				int *num_names,
				struct node_status_extra *extra)
{
	struct node_status *ret;
	int i;

	*num_names = CVAL(p,0);

	if (*num_names == 0)
		return NULL;

	ret = talloc_array(mem_ctx, struct node_status,*num_names);
	if (!ret)
		return NULL;

	p++;
	for (i=0;i< *num_names;i++) {
		StrnCpy(ret[i].name,p,15);
		trim_char(ret[i].name,'\0',' ');
		ret[i].type = CVAL(p,15);
		ret[i].flags = p[16];
		p += 18;
		DEBUG(10, ("%s#%02x: flags = 0x%02x\n", ret[i].name,
			   ret[i].type, ret[i].flags));
	}
	/*
	 * Also, pick up the MAC address ...
	 */
	if (extra) {
		memcpy(&extra->mac_addr, p, 6); /* Fill in the mac addr */
	}
	return ret;
}

struct sock_packet_read_state {
	struct tevent_context *ev;
	enum packet_type type;
	int trn_id;

	struct nb_packet_reader *reader;
	struct tevent_req *reader_req;

	int sock;
	struct tevent_req *socket_req;
	uint8_t buf[1024];
	struct sockaddr_storage addr;
	socklen_t addr_len;

	bool (*validator)(struct packet_struct *p,
			  void *private_data);
	void *private_data;

	struct packet_struct *packet;
};

static int sock_packet_read_state_destructor(struct sock_packet_read_state *s);
static void sock_packet_read_got_packet(struct tevent_req *subreq);
static void sock_packet_read_got_socket(struct tevent_req *subreq);

static struct tevent_req *sock_packet_read_send(
	TALLOC_CTX *mem_ctx,
	struct tevent_context *ev,
	int sock, /* dgram socket */
	struct nb_packet_reader *reader,
	enum packet_type type,
	int trn_id,
	bool (*validator)(struct packet_struct *p, void *private_data),
	void *private_data)
{
	struct tevent_req *req;
	struct sock_packet_read_state *state;

	req = tevent_req_create(mem_ctx, &state,
				struct sock_packet_read_state);
	if (req == NULL) {
		return NULL;
	}
	talloc_set_destructor(state, sock_packet_read_state_destructor);
	state->ev = ev;
	state->reader = reader;
	state->sock = sock;
	state->type = type;
	state->trn_id = trn_id;
	state->validator = validator;
	state->private_data = private_data;

	if (reader != NULL) {
		state->reader_req = nb_packet_read_send(state, ev, reader);
		if (tevent_req_nomem(state->reader_req, req)) {
			return tevent_req_post(req, ev);
		}
		tevent_req_set_callback(
			state->reader_req, sock_packet_read_got_packet, req);
	}

	state->addr_len = sizeof(state->addr);
	state->socket_req = recvfrom_send(state, ev, sock,
					  state->buf, sizeof(state->buf), 0,
					  &state->addr, &state->addr_len);
	if (tevent_req_nomem(state->socket_req, req)) {
		return tevent_req_post(req, ev);
	}
	tevent_req_set_callback(state->socket_req, sock_packet_read_got_socket,
				req);

	return req;
}

static int sock_packet_read_state_destructor(struct sock_packet_read_state *s)
{
	if (s->packet != NULL) {
		free_packet(s->packet);
		s->packet = NULL;
	}
	return 0;
}

static void sock_packet_read_got_packet(struct tevent_req *subreq)
{
	struct tevent_req *req = tevent_req_callback_data(
		subreq, struct tevent_req);
	struct sock_packet_read_state *state = tevent_req_data(
		req, struct sock_packet_read_state);
	NTSTATUS status;

	status = nb_packet_read_recv(subreq, &state->packet);

	TALLOC_FREE(state->reader_req);

	if (!NT_STATUS_IS_OK(status)) {
		if (state->socket_req != NULL) {
			/*
			 * Still waiting for socket
			 */
			return;
		}
		/*
		 * Both socket and packet reader failed
		 */
		tevent_req_nterror(req, status);
		return;
	}

	if ((state->validator != NULL) &&
	    !state->validator(state->packet, state->private_data)) {
		DEBUG(10, ("validator failed\n"));

		free_packet(state->packet);
		state->packet = NULL;

		state->reader_req = nb_packet_read_send(state, state->ev,
							state->reader);
		if (tevent_req_nomem(state->reader_req, req)) {
			return;
		}
		tevent_req_set_callback(
			state->reader_req, sock_packet_read_got_packet, req);
		return;
	}

	TALLOC_FREE(state->socket_req);
	tevent_req_done(req);
}

static void sock_packet_read_got_socket(struct tevent_req *subreq)
{
	struct tevent_req *req = tevent_req_callback_data(
		subreq, struct tevent_req);
	struct sock_packet_read_state *state = tevent_req_data(
		req, struct sock_packet_read_state);
	struct sockaddr_in *in_addr;
	ssize_t received;
	int err;

	received = recvfrom_recv(subreq, &err);

	TALLOC_FREE(state->socket_req);

	if (received == -1) {
		if (state->reader_req != NULL) {
			/*
			 * Still waiting for reader
			 */
			return;
		}
		/*
		 * Both socket and reader failed
		 */
		tevent_req_nterror(req, map_nt_error_from_unix(err));
		return;
	}
	if (state->addr.ss_family != AF_INET) {
		goto retry;
	}
	in_addr = (struct sockaddr_in *)(void *)&state->addr;

	state->packet = parse_packet((char *)state->buf, received, state->type,
				     in_addr->sin_addr, in_addr->sin_port);
	if (state->packet == NULL) {
		DEBUG(10, ("parse_packet failed\n"));
		goto retry;
	}
	if ((state->trn_id != -1) &&
	    (state->trn_id != packet_trn_id(state->packet))) {
		DEBUG(10, ("Expected transaction id %d, got %d\n",
			   state->trn_id, packet_trn_id(state->packet)));
		goto retry;
	}

	if ((state->validator != NULL) &&
	    !state->validator(state->packet, state->private_data)) {
		DEBUG(10, ("validator failed\n"));
		goto retry;
	}

	tevent_req_done(req);
	return;

retry:
	if (state->packet != NULL) {
		free_packet(state->packet);
		state->packet = NULL;
	}
	state->socket_req = recvfrom_send(state, state->ev, state->sock,
					  state->buf, sizeof(state->buf), 0,
					  &state->addr, &state->addr_len);
	if (tevent_req_nomem(state->socket_req, req)) {
		return;
	}
	tevent_req_set_callback(state->socket_req, sock_packet_read_got_socket,
				req);
}

static NTSTATUS sock_packet_read_recv(struct tevent_req *req,
				      struct packet_struct **ppacket)
{
	struct sock_packet_read_state *state = tevent_req_data(
		req, struct sock_packet_read_state);
	NTSTATUS status;

	if (tevent_req_is_nterror(req, &status)) {
		return status;
	}
	*ppacket = state->packet;
	state->packet = NULL;
	return NT_STATUS_OK;
}

struct nb_trans_state {
	struct tevent_context *ev;
	int sock;
	struct nb_packet_reader *reader;

	const struct sockaddr_storage *dst_addr;
	uint8_t *buf;
	size_t buflen;
	enum packet_type type;
	int trn_id;

	bool (*validator)(struct packet_struct *p,
			  void *private_data);
	void *private_data;

	struct packet_struct *packet;
};

static int nb_trans_state_destructor(struct nb_trans_state *s);
static void nb_trans_got_reader(struct tevent_req *subreq);
static void nb_trans_done(struct tevent_req *subreq);
static void nb_trans_sent(struct tevent_req *subreq);
static void nb_trans_send_next(struct tevent_req *subreq);

static struct tevent_req *nb_trans_send(
	TALLOC_CTX *mem_ctx,
	struct tevent_context *ev,
	const struct sockaddr_storage *my_addr,
	const struct sockaddr_storage *dst_addr,
	bool bcast,
	uint8_t *buf, size_t buflen,
	enum packet_type type, int trn_id,
	bool (*validator)(struct packet_struct *p,
			  void *private_data),
	void *private_data)
{
	struct tevent_req *req, *subreq;
	struct nb_trans_state *state;

	req = tevent_req_create(mem_ctx, &state, struct nb_trans_state);
	if (req == NULL) {
		return NULL;
	}
	talloc_set_destructor(state, nb_trans_state_destructor);
	state->ev = ev;
	state->dst_addr = dst_addr;
	state->buf = buf;
	state->buflen = buflen;
	state->type = type;
	state->trn_id = trn_id;
	state->validator = validator;
	state->private_data = private_data;

	state->sock = open_socket_in(SOCK_DGRAM, 0, 3, my_addr, True);
	if (state->sock == -1) {
		tevent_req_nterror(req, map_nt_error_from_unix(errno));
		DEBUG(10, ("open_socket_in failed: %s\n", strerror(errno)));
		return tevent_req_post(req, ev);
	}

	if (bcast) {
		set_socket_options(state->sock,"SO_BROADCAST");
	}

	subreq = nb_packet_reader_send(state, ev, type, state->trn_id, NULL);
	if (tevent_req_nomem(subreq, req)) {
		return tevent_req_post(req, ev);
	}
	tevent_req_set_callback(subreq, nb_trans_got_reader, req);
	return req;
}

static int nb_trans_state_destructor(struct nb_trans_state *s)
{
	if (s->sock != -1) {
		close(s->sock);
		s->sock = -1;
	}
	if (s->packet != NULL) {
		free_packet(s->packet);
		s->packet = NULL;
	}
	return 0;
}

static void nb_trans_got_reader(struct tevent_req *subreq)
{
	struct tevent_req *req = tevent_req_callback_data(
		subreq, struct tevent_req);
	struct nb_trans_state *state = tevent_req_data(
		req, struct nb_trans_state);
	NTSTATUS status;

	status = nb_packet_reader_recv(subreq, state, &state->reader);
	TALLOC_FREE(subreq);

	if (!NT_STATUS_IS_OK(status)) {
		DEBUG(10, ("nmbd not around\n"));
		state->reader = NULL;
	}

	subreq = sock_packet_read_send(
		state, state->ev, state->sock,
		state->reader, state->type, state->trn_id,
		state->validator, state->private_data);
	if (tevent_req_nomem(subreq, req)) {
		return;
	}
	tevent_req_set_callback(subreq, nb_trans_done, req);

	subreq = sendto_send(state, state->ev, state->sock,
			     state->buf, state->buflen, 0, state->dst_addr);
	if (tevent_req_nomem(subreq, req)) {
		return;
	}
	tevent_req_set_callback(subreq, nb_trans_sent, req);
}

static void nb_trans_sent(struct tevent_req *subreq)
{
	struct tevent_req *req = tevent_req_callback_data(
		subreq, struct tevent_req);
	struct nb_trans_state *state = tevent_req_data(
		req, struct nb_trans_state);
	ssize_t sent;
	int err;

	sent = sendto_recv(subreq, &err);
	TALLOC_FREE(subreq);
	if (sent == -1) {
		DEBUG(10, ("sendto failed: %s\n", strerror(err)));
		tevent_req_nterror(req, map_nt_error_from_unix(err));
		return;
	}
	subreq = tevent_wakeup_send(state, state->ev,
				    timeval_current_ofs(1, 0));
	if (tevent_req_nomem(subreq, req)) {
		return;
	}
	tevent_req_set_callback(subreq, nb_trans_send_next, req);
}

static void nb_trans_send_next(struct tevent_req *subreq)
{
	struct tevent_req *req = tevent_req_callback_data(
		subreq, struct tevent_req);
	struct nb_trans_state *state = tevent_req_data(
		req, struct nb_trans_state);
	bool ret;

	ret = tevent_wakeup_recv(subreq);
	TALLOC_FREE(subreq);
	if (!ret) {
		tevent_req_nterror(req, NT_STATUS_INTERNAL_ERROR);
		return;
	}
	subreq = sendto_send(state, state->ev, state->sock,
			     state->buf, state->buflen, 0, state->dst_addr);
	if (tevent_req_nomem(subreq, req)) {
		return;
	}
	tevent_req_set_callback(subreq, nb_trans_sent, req);
}

static void nb_trans_done(struct tevent_req *subreq)
{
	struct tevent_req *req = tevent_req_callback_data(
		subreq, struct tevent_req);
	struct nb_trans_state *state = tevent_req_data(
		req, struct nb_trans_state);
	NTSTATUS status;

	status = sock_packet_read_recv(subreq, &state->packet);
	TALLOC_FREE(subreq);
	if (tevent_req_nterror(req, status)) {
		return;
	}
	tevent_req_done(req);
}

static NTSTATUS nb_trans_recv(struct tevent_req *req,
			      struct packet_struct **ppacket)
{
	struct nb_trans_state *state = tevent_req_data(
		req, struct nb_trans_state);
	NTSTATUS status;

	if (tevent_req_is_nterror(req, &status)) {
		return status;
	}
	*ppacket = state->packet;
	state->packet = NULL;
	return NT_STATUS_OK;
}

/****************************************************************************
 Do a NBT node status query on an open socket and return an array of
 structures holding the returned names or NULL if the query failed.
**************************************************************************/

struct node_status_query_state {
	struct sockaddr_storage my_addr;
	struct sockaddr_storage addr;
	uint8_t buf[1024];
	ssize_t buflen;
	struct packet_struct *packet;
};

static int node_status_query_state_destructor(
	struct node_status_query_state *s);
static bool node_status_query_validator(struct packet_struct *p,
					void *private_data);
static void node_status_query_done(struct tevent_req *subreq);

struct tevent_req *node_status_query_send(TALLOC_CTX *mem_ctx,
					  struct tevent_context *ev,
					  struct nmb_name *name,
					  const struct sockaddr_storage *addr)
{
	struct tevent_req *req, *subreq;
	struct node_status_query_state *state;
	struct packet_struct p;
	struct nmb_packet *nmb = &p.packet.nmb;
	struct sockaddr_in *in_addr;

	req = tevent_req_create(mem_ctx, &state,
				struct node_status_query_state);
	if (req == NULL) {
		return NULL;
	}
	talloc_set_destructor(state, node_status_query_state_destructor);

	if (addr->ss_family != AF_INET) {
		/* Can't do node status to IPv6 */
		tevent_req_nterror(req, NT_STATUS_INVALID_ADDRESS);
		return tevent_req_post(req, ev);
	}

	state->addr = *addr;
	in_addr = (struct sockaddr_in *)(void *)&state->addr;
	in_addr->sin_port = htons(NMB_PORT);

	set_socket_addr_v4(&state->my_addr);

	ZERO_STRUCT(p);
	nmb->header.name_trn_id = generate_trn_id();
	nmb->header.opcode = 0;
	nmb->header.response = false;
	nmb->header.nm_flags.bcast = false;
	nmb->header.nm_flags.recursion_available = false;
	nmb->header.nm_flags.recursion_desired = false;
	nmb->header.nm_flags.trunc = false;
	nmb->header.nm_flags.authoritative = false;
	nmb->header.rcode = 0;
	nmb->header.qdcount = 1;
	nmb->header.ancount = 0;
	nmb->header.nscount = 0;
	nmb->header.arcount = 0;
	nmb->question.question_name = *name;
	nmb->question.question_type = 0x21;
	nmb->question.question_class = 0x1;

	state->buflen = build_packet((char *)state->buf, sizeof(state->buf),
				     &p);
	if (state->buflen == 0) {
		tevent_req_nterror(req, NT_STATUS_INTERNAL_ERROR);
		DEBUG(10, ("build_packet failed\n"));
		return tevent_req_post(req, ev);
	}

	subreq = nb_trans_send(state, ev, &state->my_addr, &state->addr, false,
			       state->buf, state->buflen,
			       NMB_PACKET, nmb->header.name_trn_id,
			       node_status_query_validator, NULL);
	if (tevent_req_nomem(subreq, req)) {
		DEBUG(10, ("nb_trans_send failed\n"));
		return tevent_req_post(req, ev);
	}
	if (!tevent_req_set_endtime(req, ev, timeval_current_ofs(10, 0))) {
		return tevent_req_post(req, ev);
	}
	tevent_req_set_callback(subreq, node_status_query_done, req);
	return req;
}

static bool node_status_query_validator(struct packet_struct *p,
					void *private_data)
{
	struct nmb_packet *nmb = &p->packet.nmb;
	debug_nmb_packet(p);

	if (nmb->header.opcode != 0 ||
	    nmb->header.nm_flags.bcast ||
	    nmb->header.rcode ||
	    !nmb->header.ancount ||
	    nmb->answers->rr_type != 0x21) {
		/*
		 * XXXX what do we do with this? could be a redirect,
		 * but we'll discard it for the moment
		 */
		return false;
	}
	return true;
}

static int node_status_query_state_destructor(
	struct node_status_query_state *s)
{
	if (s->packet != NULL) {
		free_packet(s->packet);
		s->packet = NULL;
	}
	return 0;
}

static void node_status_query_done(struct tevent_req *subreq)
{
	struct tevent_req *req = tevent_req_callback_data(
		subreq, struct tevent_req);
	struct node_status_query_state *state = tevent_req_data(
		req, struct node_status_query_state);
	NTSTATUS status;

	status = nb_trans_recv(subreq, &state->packet);
	TALLOC_FREE(subreq);
	if (tevent_req_nterror(req, status)) {
		return;
	}
	tevent_req_done(req);
}

NTSTATUS node_status_query_recv(struct tevent_req *req, TALLOC_CTX *mem_ctx,
				struct node_status **pnode_status,
				int *pnum_names,
				struct node_status_extra *extra)
{
	struct node_status_query_state *state = tevent_req_data(
		req, struct node_status_query_state);
	struct node_status *node_status;
	int num_names;
	NTSTATUS status;

	if (tevent_req_is_nterror(req, &status)) {
		return status;
	}
	node_status = parse_node_status(
		mem_ctx, &state->packet->packet.nmb.answers->rdata[0],
		&num_names, extra);
	if (node_status == NULL) {
		return NT_STATUS_NO_MEMORY;
	}
	*pnode_status = node_status;
	*pnum_names = num_names;
	return NT_STATUS_OK;
}

NTSTATUS node_status_query(TALLOC_CTX *mem_ctx, struct nmb_name *name,
			   const struct sockaddr_storage *addr,
			   struct node_status **pnode_status,
			   int *pnum_names,
			   struct node_status_extra *extra)
{
	TALLOC_CTX *frame = talloc_stackframe();
	struct tevent_context *ev;
	struct tevent_req *req;
	NTSTATUS status = NT_STATUS_NO_MEMORY;

	ev = tevent_context_init(frame);
	if (ev == NULL) {
		goto fail;
	}
	req = node_status_query_send(ev, ev, name, addr);
	if (req == NULL) {
		goto fail;
	}
	if (!tevent_req_poll_ntstatus(req, ev, &status)) {
		goto fail;
	}
	status = node_status_query_recv(req, mem_ctx, pnode_status,
					pnum_names, extra);
 fail:
	TALLOC_FREE(frame);
	return status;
}

/****************************************************************************
 Find the first type XX name in a node status reply - used for finding
 a servers name given its IP. Return the matched name in *name.
**************************************************************************/

bool name_status_find(const char *q_name,
			int q_type,
			int type,
			const struct sockaddr_storage *to_ss,
			fstring name)
{
	char addr[INET6_ADDRSTRLEN];
	struct sockaddr_storage ss;
	struct node_status *addrs = NULL;
	struct nmb_name nname;
	int count, i;
	bool result = false;
	NTSTATUS status;

	if (lp_disable_netbios()) {
		DEBUG(5,("name_status_find(%s#%02x): netbios is disabled\n",
					q_name, q_type));
		return False;
	}

	print_sockaddr(addr, sizeof(addr), to_ss);

	DEBUG(10, ("name_status_find: looking up %s#%02x at %s\n", q_name,
		   q_type, addr));

	/* Check the cache first. */

	if (namecache_status_fetch(q_name, q_type, type, to_ss, name)) {
		return True;
	}

	if (to_ss->ss_family != AF_INET) {
		/* Can't do node status to IPv6 */
		return false;
	}

	set_socket_addr_v4(&ss);

	/* W2K PDC's seem not to respond to '*'#0. JRA */
	make_nmb_name(&nname, q_name, q_type);
	status = node_status_query(talloc_tos(), &nname, to_ss,
				   &addrs, &count, NULL);
	if (!NT_STATUS_IS_OK(status)) {
		goto done;
	}

	for (i=0;i<count;i++) {
                /* Find first one of the requested type that's not a GROUP. */
		if (addrs[i].type == type && ! (addrs[i].flags & 0x80))
			break;
	}
	if (i == count)
		goto done;

	pull_ascii_nstring(name, sizeof(fstring), addrs[i].name);

	/* Store the result in the cache. */
	/* but don't store an entry for 0x1c names here.  Here we have
	   a single host and DOMAIN<0x1c> names should be a list of hosts */

	if ( q_type != 0x1c ) {
		namecache_status_store(q_name, q_type, type, to_ss, name);
	}

	result = true;

 done:
	TALLOC_FREE(addrs);

	DEBUG(10, ("name_status_find: name %sfound", result ? "" : "not "));

	if (result)
		DEBUGADD(10, (", name %s ip address is %s", name, addr));

	DEBUG(10, ("\n"));

	return result;
}

/*
  comparison function used by sort_addr_list
*/

static int addr_compare(const struct sockaddr_storage *ss1,
			const struct sockaddr_storage *ss2)
{
	int max_bits1=0, max_bits2=0;
	int num_interfaces = iface_count();
	int i;

	/* Sort IPv4 addresses first. */
	if (ss1->ss_family != ss2->ss_family) {
		if (ss2->ss_family == AF_INET) {
			return 1;
		} else {
			return -1;
		}
	}

	/* Here we know both addresses are of the same
	 * family. */

	for (i=0;i<num_interfaces;i++) {
		const struct sockaddr_storage *pss = iface_n_bcast(i);
		const unsigned char *p_ss1 = NULL;
		const unsigned char *p_ss2 = NULL;
		const unsigned char *p_if = NULL;
		size_t len = 0;
		int bits1, bits2;

		if (pss->ss_family != ss1->ss_family) {
			/* Ignore interfaces of the wrong type. */
			continue;
		}
		if (pss->ss_family == AF_INET) {
			p_if = (const unsigned char *)
				&((const struct sockaddr_in *)pss)->sin_addr;
			p_ss1 = (const unsigned char *)
				&((const struct sockaddr_in *)ss1)->sin_addr;
			p_ss2 = (const unsigned char *)
				&((const struct sockaddr_in *)ss2)->sin_addr;
			len = 4;
		}
#if defined(HAVE_IPV6)
		if (pss->ss_family == AF_INET6) {
			p_if = (const unsigned char *)
				&((const struct sockaddr_in6 *)pss)->sin6_addr;
			p_ss1 = (const unsigned char *)
				&((const struct sockaddr_in6 *)ss1)->sin6_addr;
			p_ss2 = (const unsigned char *)
				&((const struct sockaddr_in6 *)ss2)->sin6_addr;
			len = 16;
		}
#endif
		if (!p_ss1 || !p_ss2 || !p_if || len == 0) {
			continue;
		}
		bits1 = matching_len_bits(p_ss1, p_if, len);
		bits2 = matching_len_bits(p_ss2, p_if, len);
		max_bits1 = MAX(bits1, max_bits1);
		max_bits2 = MAX(bits2, max_bits2);
	}

	/* Bias towards directly reachable IPs */
	if (iface_local((const struct sockaddr *)ss1)) {
		if (ss1->ss_family == AF_INET) {
			max_bits1 += 32;
		} else {
			max_bits1 += 128;
		}
	}
	if (iface_local((const struct sockaddr *)ss2)) {
		if (ss2->ss_family == AF_INET) {
			max_bits2 += 32;
		} else {
			max_bits2 += 128;
		}
	}
	return max_bits2 - max_bits1;
}

/*******************************************************************
 compare 2 ldap IPs by nearness to our interfaces - used in qsort
*******************************************************************/

int ip_service_compare(struct ip_service *ss1, struct ip_service *ss2)
{
	int result;

	if ((result = addr_compare(&ss1->ss, &ss2->ss)) != 0) {
		return result;
	}

	if (ss1->port > ss2->port) {
		return 1;
	}

	if (ss1->port < ss2->port) {
		return -1;
	}

	return 0;
}

/*
  sort an IP list so that names that are close to one of our interfaces
  are at the top. This prevents the problem where a WINS server returns an IP
  that is not reachable from our subnet as the first match
*/

static void sort_addr_list(struct sockaddr_storage *sslist, int count)
{
	if (count <= 1) {
		return;
	}

	TYPESAFE_QSORT(sslist, count, addr_compare);
}

static void sort_service_list(struct ip_service *servlist, int count)
{
	if (count <= 1) {
		return;
	}

	TYPESAFE_QSORT(servlist, count, ip_service_compare);
}

/**********************************************************************
 Remove any duplicate address/port pairs in the list
 *********************************************************************/

static int remove_duplicate_addrs2(struct ip_service *iplist, int count )
{
	int i, j;

	DEBUG(10,("remove_duplicate_addrs2: "
			"looking for duplicate address/port pairs\n"));

	/* one loop to remove duplicates */
	for ( i=0; i<count; i++ ) {
		if ( is_zero_addr(&iplist[i].ss)) {
			continue;
		}

		for ( j=i+1; j<count; j++ ) {
			if (sockaddr_equal((struct sockaddr *)(void *)&iplist[i].ss,
					   (struct sockaddr *)(void *)&iplist[j].ss) &&
					iplist[i].port == iplist[j].port) {
				zero_sockaddr(&iplist[j].ss);
			}
		}
	}

	/* one loop to clean up any holes we left */
	/* first ip should never be a zero_ip() */
	for (i = 0; i<count; ) {
		if (is_zero_addr(&iplist[i].ss) ) {
			if (i != count-1) {
				memmove(&iplist[i], &iplist[i+1],
					(count - i - 1)*sizeof(iplist[i]));
			}
			count--;
			continue;
		}
		i++;
	}

	return count;
}

static bool prioritize_ipv4_list(struct ip_service *iplist, int count)
{
	TALLOC_CTX *frame = talloc_stackframe();
	struct ip_service *iplist_new = talloc_array(frame, struct ip_service, count);
	int i, j;

	if (iplist_new == NULL) {
		TALLOC_FREE(frame);
		return false;
	}

	j = 0;

	/* Copy IPv4 first. */
	for (i = 0; i < count; i++) {
		if (iplist[i].ss.ss_family == AF_INET) {
			iplist_new[j++] = iplist[i];
		}
	}

	/* Copy IPv6. */
	for (i = 0; i < count; i++) {
		if (iplist[i].ss.ss_family != AF_INET) {
			iplist_new[j++] = iplist[i];
		}
	}

	memcpy(iplist, iplist_new, sizeof(struct ip_service)*count);
	TALLOC_FREE(frame);
	return true;
}

/****************************************************************************
 Do a netbios name query to find someones IP.
 Returns an array of IP addresses or NULL if none.
 *count will be set to the number of addresses returned.
 *timed_out is set if we failed by timing out
****************************************************************************/

struct name_query_state {
	struct sockaddr_storage my_addr;
	struct sockaddr_storage addr;
	bool bcast;


	uint8_t buf[1024];
	ssize_t buflen;

	NTSTATUS validate_error;
	uint8_t flags;

	struct sockaddr_storage *addrs;
	int num_addrs;
};

static bool name_query_validator(struct packet_struct *p, void *private_data);
static void name_query_done(struct tevent_req *subreq);

struct tevent_req *name_query_send(TALLOC_CTX *mem_ctx,
				   struct tevent_context *ev,
				   const char *name, int name_type,
				   bool bcast, bool recurse,
				   const struct sockaddr_storage *addr)
{
	struct tevent_req *req, *subreq;
	struct name_query_state *state;
	struct packet_struct p;
	struct nmb_packet *nmb = &p.packet.nmb;
	struct sockaddr_in *in_addr;

	req = tevent_req_create(mem_ctx, &state, struct name_query_state);
	if (req == NULL) {
		return NULL;
	}
	state->bcast = bcast;

	if (addr->ss_family != AF_INET) {
		/* Can't do node status to IPv6 */
		tevent_req_nterror(req, NT_STATUS_INVALID_ADDRESS);
		return tevent_req_post(req, ev);
	}

	if (lp_disable_netbios()) {
		DEBUG(5,("name_query(%s#%02x): netbios is disabled\n",
					name, name_type));
		tevent_req_nterror(req, NT_STATUS_NOT_SUPPORTED);
		return tevent_req_post(req, ev);
	}

	state->addr = *addr;
	in_addr = (struct sockaddr_in *)(void *)&state->addr;
	in_addr->sin_port = htons(NMB_PORT);

	set_socket_addr_v4(&state->my_addr);

	ZERO_STRUCT(p);
	nmb->header.name_trn_id = generate_trn_id();
	nmb->header.opcode = 0;
	nmb->header.response = false;
	nmb->header.nm_flags.bcast = bcast;
	nmb->header.nm_flags.recursion_available = false;
	nmb->header.nm_flags.recursion_desired = recurse;
	nmb->header.nm_flags.trunc = false;
	nmb->header.nm_flags.authoritative = false;
	nmb->header.rcode = 0;
	nmb->header.qdcount = 1;
	nmb->header.ancount = 0;
	nmb->header.nscount = 0;
	nmb->header.arcount = 0;

	make_nmb_name(&nmb->question.question_name,name,name_type);

	nmb->question.question_type = 0x20;
	nmb->question.question_class = 0x1;

	state->buflen = build_packet((char *)state->buf, sizeof(state->buf),
				     &p);
	if (state->buflen == 0) {
		tevent_req_nterror(req, NT_STATUS_INTERNAL_ERROR);
		DEBUG(10, ("build_packet failed\n"));
		return tevent_req_post(req, ev);
	}

	subreq = nb_trans_send(state, ev, &state->my_addr, &state->addr, bcast,
			       state->buf, state->buflen,
			       NMB_PACKET, nmb->header.name_trn_id,
			       name_query_validator, state);
	if (tevent_req_nomem(subreq, req)) {
		DEBUG(10, ("nb_trans_send failed\n"));
		return tevent_req_post(req, ev);
	}
	tevent_req_set_callback(subreq, name_query_done, req);
	return req;
}

static bool name_query_validator(struct packet_struct *p, void *private_data)
{
	struct name_query_state *state = talloc_get_type_abort(
		private_data, struct name_query_state);
	struct nmb_packet *nmb = &p->packet.nmb;
	struct sockaddr_storage *tmp_addrs;
	bool got_unique_netbios_name = false;
	int i;

	debug_nmb_packet(p);

	/*
	 * If we get a Negative Name Query Response from a WINS
	 * server, we should report it and give up.
	 */
	if( 0 == nmb->header.opcode	/* A query response   */
	    && !state->bcast		/* from a WINS server */
	    && nmb->header.rcode	/* Error returned     */
		) {

		if( DEBUGLVL( 3 ) ) {
			/* Only executed if DEBUGLEVEL >= 3 */
			dbgtext( "Negative name query "
				 "response, rcode 0x%02x: ",
				 nmb->header.rcode );
			switch( nmb->header.rcode ) {
			case 0x01:
				dbgtext("Request was invalidly formatted.\n");
				break;
			case 0x02:
				dbgtext("Problem with NBNS, cannot process "
					"name.\n");
				break;
			case 0x03:
				dbgtext("The name requested does not "
					"exist.\n");
				break;
			case 0x04:
				dbgtext("Unsupported request error.\n");
				break;
			case 0x05:
				dbgtext("Query refused error.\n");
				break;
			default:
				dbgtext("Unrecognized error code.\n" );
				break;
			}
		}

		/*
		 * We accept this packet as valid, but tell the upper
		 * layers that it's a negative response.
		 */
		state->validate_error = NT_STATUS_NOT_FOUND;
		return true;
	}

	if (nmb->header.opcode != 0 ||
	    nmb->header.nm_flags.bcast ||
	    nmb->header.rcode ||
	    !nmb->header.ancount) {
		/*
		 * XXXX what do we do with this? Could be a redirect,
		 * but we'll discard it for the moment.
		 */
		return false;
	}

	tmp_addrs = talloc_realloc(
		state, state->addrs, struct sockaddr_storage,
		state->num_addrs + nmb->answers->rdlength/6);
	if (tmp_addrs == NULL) {
		state->validate_error = NT_STATUS_NO_MEMORY;
		return true;
	}
	state->addrs = tmp_addrs;

	DEBUG(2,("Got a positive name query response "
		 "from %s ( ", inet_ntoa(p->ip)));

	for (i=0; i<nmb->answers->rdlength/6; i++) {
		uint16_t flags;
		struct in_addr ip;
		struct sockaddr_storage addr;
		int j;

		flags = RSVAL(&nmb->answers->rdata[i*6], 0);
		got_unique_netbios_name |= ((flags & 0x8000) == 0);

		putip((char *)&ip,&nmb->answers->rdata[2+i*6]);
		in_addr_to_sockaddr_storage(&addr, ip);

		for (j=0; j<state->num_addrs; j++) {
			if (sockaddr_equal(
				    (struct sockaddr *)(void *)&addr,
				    (struct sockaddr *)(void *)&state->addrs[j])) {
				break;
			}
		}
		if (j < state->num_addrs) {
			/* Already got it */
			continue;
		}

		DEBUGADD(2,("%s ",inet_ntoa(ip)));

		state->addrs[state->num_addrs] = addr;
		state->num_addrs += 1;
	}
	DEBUGADD(2,(")\n"));

	/* We add the flags back ... */
	if (nmb->header.response)
		state->flags |= NM_FLAGS_RS;
	if (nmb->header.nm_flags.authoritative)
		state->flags |= NM_FLAGS_AA;
	if (nmb->header.nm_flags.trunc)
		state->flags |= NM_FLAGS_TC;
	if (nmb->header.nm_flags.recursion_desired)
		state->flags |= NM_FLAGS_RD;
	if (nmb->header.nm_flags.recursion_available)
		state->flags |= NM_FLAGS_RA;
	if (nmb->header.nm_flags.bcast)
		state->flags |= NM_FLAGS_B;

	if (state->bcast) {
		/*
		 * We have to collect all entries coming in from broadcast
		 * queries. If we got a unique name, we're done.
		 */
		return got_unique_netbios_name;
	}
	/*
	 * WINS responses are accepted when they are received
	 */
	return true;
}

static void name_query_done(struct tevent_req *subreq)
{
	struct tevent_req *req = tevent_req_callback_data(
		subreq, struct tevent_req);
	struct name_query_state *state = tevent_req_data(
		req, struct name_query_state);
	NTSTATUS status;
	struct packet_struct *p = NULL;

	status = nb_trans_recv(subreq, &p);
	TALLOC_FREE(subreq);
	if (tevent_req_nterror(req, status)) {
		return;
	}
	if (!NT_STATUS_IS_OK(state->validate_error)) {
		tevent_req_nterror(req, state->validate_error);
		return;
	}
	if (p != NULL) {
		/*
		 * Free the packet here, we've collected the response in the
		 * validator
		 */
		free_packet(p);
	}
	tevent_req_done(req);
}

NTSTATUS name_query_recv(struct tevent_req *req, TALLOC_CTX *mem_ctx,
			 struct sockaddr_storage **addrs, int *num_addrs,
			 uint8_t *flags)
{
	struct name_query_state *state = tevent_req_data(
		req, struct name_query_state);
	NTSTATUS status;

	if (tevent_req_is_nterror(req, &status)) {
		if (state->bcast &&
		    NT_STATUS_EQUAL(status, NT_STATUS_IO_TIMEOUT)) {
			/*
			 * In the broadcast case we collect replies until the
			 * timeout.
			 */
			status = NT_STATUS_OK;
		}
		if (!NT_STATUS_IS_OK(status)) {
			return status;
		}
	}
	if (state->num_addrs == 0) {
		return NT_STATUS_NOT_FOUND;
	}
	*addrs = talloc_move(mem_ctx, &state->addrs);
	sort_addr_list(*addrs, state->num_addrs);
	*num_addrs = state->num_addrs;
	if (flags != NULL) {
		*flags = state->flags;
	}
	return NT_STATUS_OK;
}

NTSTATUS name_query(const char *name, int name_type,
		    bool bcast, bool recurse,
		    const struct sockaddr_storage *to_ss,
		    TALLOC_CTX *mem_ctx,
		    struct sockaddr_storage **addrs,
		    int *num_addrs, uint8_t *flags)
{
	TALLOC_CTX *frame = talloc_stackframe();
	struct tevent_context *ev;
	struct tevent_req *req;
	struct timeval timeout;
	NTSTATUS status = NT_STATUS_NO_MEMORY;

	ev = tevent_context_init(frame);
	if (ev == NULL) {
		goto fail;
	}
	req = name_query_send(ev, ev, name, name_type, bcast, recurse, to_ss);
	if (req == NULL) {
		goto fail;
	}
	if (bcast) {
		timeout = timeval_current_ofs(0, 250000);
	} else {
		timeout = timeval_current_ofs(2, 0);
	}
	if (!tevent_req_set_endtime(req, ev, timeout)) {
		goto fail;
	}
	if (!tevent_req_poll_ntstatus(req, ev, &status)) {
		goto fail;
	}
	status = name_query_recv(req, mem_ctx, addrs, num_addrs, flags);
 fail:
	TALLOC_FREE(frame);
	return status;
}

/********************************************************
 convert an array if struct sockaddr_storage to struct ip_service
 return false on failure.  Port is set to PORT_NONE;
*********************************************************/

static bool convert_ss2service(struct ip_service **return_iplist,
		const struct sockaddr_storage *ss_list,
		int count)
{
	int i;

	if ( count==0 || !ss_list )
		return False;

	/* copy the ip address; port will be PORT_NONE */
	if ((*return_iplist = SMB_MALLOC_ARRAY(struct ip_service, count)) ==
			NULL) {
		DEBUG(0,("convert_ip2service: malloc failed "
			"for %d enetries!\n", count ));
		return False;
	}

	for ( i=0; i<count; i++ ) {
		(*return_iplist)[i].ss   = ss_list[i];
		(*return_iplist)[i].port = PORT_NONE;
	}

	return true;
}

struct name_queries_state {
	struct tevent_context *ev;
	const char *name;
	int name_type;
	bool bcast;
	bool recurse;
	const struct sockaddr_storage *addrs;
	int num_addrs;
	int wait_msec;
	int timeout_msec;

	struct tevent_req **subreqs;
	int num_received;
	int num_sent;

	int received_index;
	struct sockaddr_storage *result_addrs;
	int num_result_addrs;
	uint8_t flags;
};

static void name_queries_done(struct tevent_req *subreq);
static void name_queries_next(struct tevent_req *subreq);

/*
 * Send a name query to multiple destinations with a wait time in between
 */

static struct tevent_req *name_queries_send(
	TALLOC_CTX *mem_ctx, struct tevent_context *ev,
	const char *name, int name_type,
	bool bcast, bool recurse,
	const struct sockaddr_storage *addrs,
	int num_addrs, int wait_msec, int timeout_msec)
{
	struct tevent_req *req, *subreq;
	struct name_queries_state *state;

	req = tevent_req_create(mem_ctx, &state,
				struct name_queries_state);
	if (req == NULL) {
		return NULL;
	}
	state->ev = ev;
	state->name = name;
	state->name_type = name_type;
	state->bcast = bcast;
	state->recurse = recurse;
	state->addrs = addrs;
	state->num_addrs = num_addrs;
	state->wait_msec = wait_msec;
	state->timeout_msec = timeout_msec;

	state->subreqs = talloc_zero_array(
		state, struct tevent_req *, num_addrs);
	if (tevent_req_nomem(state->subreqs, req)) {
		return tevent_req_post(req, ev);
	}
	state->num_sent = 0;

	subreq = name_query_send(
		state->subreqs, state->ev, name, name_type, bcast, recurse,
		&state->addrs[state->num_sent]);
	if (tevent_req_nomem(subreq, req)) {
		return tevent_req_post(req, ev);
	}
	if (!tevent_req_set_endtime(
		    subreq, state->ev,
		    timeval_current_ofs(0, state->timeout_msec * 1000))) {
		tevent_req_oom(req);
		return tevent_req_post(req, ev);
	}
	tevent_req_set_callback(subreq, name_queries_done, req);

	state->subreqs[state->num_sent] = subreq;
	state->num_sent += 1;

	if (state->num_sent < state->num_addrs) {
		subreq = tevent_wakeup_send(
			state, state->ev,
			timeval_current_ofs(0, state->wait_msec * 1000));
		if (tevent_req_nomem(subreq, req)) {
			return tevent_req_post(req, ev);
		}
		tevent_req_set_callback(subreq, name_queries_next, req);
	}
	return req;
}

static void name_queries_done(struct tevent_req *subreq)
{
	struct tevent_req *req = tevent_req_callback_data(
		subreq, struct tevent_req);
	struct name_queries_state *state = tevent_req_data(
		req, struct name_queries_state);
	int i;
	NTSTATUS status;

	status = name_query_recv(subreq, state, &state->result_addrs,
				 &state->num_result_addrs, &state->flags);

	for (i=0; i<state->num_sent; i++) {
		if (state->subreqs[i] == subreq) {
			break;
		}
	}
	if (i == state->num_sent) {
		tevent_req_nterror(req, NT_STATUS_INTERNAL_ERROR);
		return;
	}
	TALLOC_FREE(state->subreqs[i]);

	state->num_received += 1;

	if (!NT_STATUS_IS_OK(status)) {

		if (state->num_received >= state->num_addrs) {
			tevent_req_nterror(req, status);
			return;
		}
		/*
		 * Still outstanding requests, just wait
		 */
		return;
	}
	state->received_index = i;
	tevent_req_done(req);
}

static void name_queries_next(struct tevent_req *subreq)
{
	struct tevent_req *req = tevent_req_callback_data(
		subreq, struct tevent_req);
	struct name_queries_state *state = tevent_req_data(
		req, struct name_queries_state);

	if (!tevent_wakeup_recv(subreq)) {
		tevent_req_nterror(req, NT_STATUS_INTERNAL_ERROR);
		return;
	}

	subreq = name_query_send(
		state->subreqs, state->ev,
		state->name, state->name_type, state->bcast, state->recurse,
		&state->addrs[state->num_sent]);
	if (tevent_req_nomem(subreq, req)) {
		return;
	}
	tevent_req_set_callback(subreq, name_queries_done, req);
	if (!tevent_req_set_endtime(
		    subreq, state->ev,
		    timeval_current_ofs(0, state->timeout_msec * 1000))) {
		tevent_req_oom(req);
		return;
	}
	state->subreqs[state->num_sent] = subreq;
	state->num_sent += 1;

	if (state->num_sent < state->num_addrs) {
		subreq = tevent_wakeup_send(
			state, state->ev,
			timeval_current_ofs(0, state->wait_msec * 1000));
		if (tevent_req_nomem(subreq, req)) {
			return;
		}
		tevent_req_set_callback(subreq, name_queries_next, req);
	}
}

static NTSTATUS name_queries_recv(struct tevent_req *req, TALLOC_CTX *mem_ctx,
				  struct sockaddr_storage **result_addrs,
				  int *num_result_addrs, uint8_t *flags,
				  int *received_index)
{
	struct name_queries_state *state = tevent_req_data(
		req, struct name_queries_state);
	NTSTATUS status;

	if (tevent_req_is_nterror(req, &status)) {
		return status;
	}

	if (result_addrs != NULL) {
		*result_addrs = talloc_move(mem_ctx, &state->result_addrs);
	}
	if (num_result_addrs != NULL) {
		*num_result_addrs = state->num_result_addrs;
	}
	if (flags != NULL) {
		*flags = state->flags;
	}
	if (received_index != NULL) {
		*received_index = state->received_index;
	}
	return NT_STATUS_OK;
}

/********************************************************
 Resolve via "bcast" method.
*********************************************************/

struct name_resolve_bcast_state {
	struct sockaddr_storage *addrs;
	int num_addrs;
};

static void name_resolve_bcast_done(struct tevent_req *subreq);

struct tevent_req *name_resolve_bcast_send(TALLOC_CTX *mem_ctx,
					   struct tevent_context *ev,
					   const char *name,
					   int name_type)
{
	struct tevent_req *req, *subreq;
	struct name_resolve_bcast_state *state;
	struct sockaddr_storage *bcast_addrs;
	int i, num_addrs, num_bcast_addrs;

	req = tevent_req_create(mem_ctx, &state,
				struct name_resolve_bcast_state);
	if (req == NULL) {
		return NULL;
	}

	if (lp_disable_netbios()) {
		DEBUG(5, ("name_resolve_bcast(%s#%02x): netbios is disabled\n",
			  name, name_type));
		tevent_req_nterror(req, NT_STATUS_INVALID_PARAMETER);
		return tevent_req_post(req, ev);
	}

	/*
	 * "bcast" means do a broadcast lookup on all the local interfaces.
	 */

	DEBUG(3, ("name_resolve_bcast: Attempting broadcast lookup "
		  "for name %s<0x%x>\n", name, name_type));

	num_addrs = iface_count();
	bcast_addrs = talloc_array(state, struct sockaddr_storage, num_addrs);
	if (tevent_req_nomem(bcast_addrs, req)) {
		return tevent_req_post(req, ev);
	}

	/*
	 * Lookup the name on all the interfaces, return on
	 * the first successful match.
	 */
	num_bcast_addrs = 0;

	for (i=0; i<num_addrs; i++) {
		const struct sockaddr_storage *pss = iface_n_bcast(i);

		if (pss->ss_family != AF_INET) {
			continue;
		}
		bcast_addrs[num_bcast_addrs] = *pss;
		num_bcast_addrs += 1;
	}

	subreq = name_queries_send(state, ev, name, name_type, true, true,
				   bcast_addrs, num_bcast_addrs, 0, 1000);
	if (tevent_req_nomem(subreq, req)) {
		return tevent_req_post(req, ev);
	}
	tevent_req_set_callback(subreq, name_resolve_bcast_done, req);
	return req;
}

static void name_resolve_bcast_done(struct tevent_req *subreq)
{
	struct tevent_req *req = tevent_req_callback_data(
		subreq, struct tevent_req);
	struct name_resolve_bcast_state *state = tevent_req_data(
		req, struct name_resolve_bcast_state);
	NTSTATUS status;

	status = name_queries_recv(subreq, state,
				   &state->addrs, &state->num_addrs,
				   NULL, NULL);
	TALLOC_FREE(subreq);
	if (tevent_req_nterror(req, status)) {
		return;
	}
	tevent_req_done(req);
}

NTSTATUS name_resolve_bcast_recv(struct tevent_req *req, TALLOC_CTX *mem_ctx,
				 struct sockaddr_storage **addrs,
				 int *num_addrs)
{
	struct name_resolve_bcast_state *state = tevent_req_data(
		req, struct name_resolve_bcast_state);
	NTSTATUS status;

	if (tevent_req_is_nterror(req, &status)) {
		return status;
	}
	*addrs = talloc_move(mem_ctx, &state->addrs);
	*num_addrs = state->num_addrs;
	return NT_STATUS_OK;
}

NTSTATUS name_resolve_bcast(const char *name,
			int name_type,
			TALLOC_CTX *mem_ctx,
			struct sockaddr_storage **return_iplist,
			int *return_count)
{
	TALLOC_CTX *frame = talloc_stackframe();
	struct event_context *ev;
	struct tevent_req *req;
	NTSTATUS status = NT_STATUS_NO_MEMORY;

	ev = event_context_init(frame);
	if (ev == NULL) {
		goto fail;
	}
	req = name_resolve_bcast_send(frame, ev, name, name_type);
	if (req == NULL) {
		goto fail;
	}
	if (!tevent_req_poll_ntstatus(req, ev, &status)) {
		goto fail;
	}
	status = name_resolve_bcast_recv(req, mem_ctx, return_iplist,
					 return_count);
 fail:
	TALLOC_FREE(frame);
	return status;
}

struct query_wins_list_state {
	struct tevent_context *ev;
	const char *name;
	uint8_t name_type;
	struct in_addr *servers;
	uint32_t num_servers;
	struct sockaddr_storage server;
	uint32_t num_sent;

	struct sockaddr_storage *addrs;
	int num_addrs;
	uint8_t flags;
};

static void query_wins_list_done(struct tevent_req *subreq);

/*
 * Query a list of (replicating) wins servers in sequence, call them
 * dead if they don't reply
 */

static struct tevent_req *query_wins_list_send(
	TALLOC_CTX *mem_ctx, struct tevent_context *ev,
	struct in_addr src_ip, const char *name, uint8_t name_type,
	struct in_addr *servers, int num_servers)
{
	struct tevent_req *req, *subreq;
	struct query_wins_list_state *state;

	req = tevent_req_create(mem_ctx, &state,
				struct query_wins_list_state);
	if (req == NULL) {
		return NULL;
	}
	state->ev = ev;
	state->name = name;
	state->name_type = name_type;
	state->servers = servers;
	state->num_servers = num_servers;

	if (state->num_servers == 0) {
		tevent_req_nterror(req, NT_STATUS_NOT_FOUND);
		return tevent_req_post(req, ev);
	}

	in_addr_to_sockaddr_storage(
		&state->server, state->servers[state->num_sent]);

	subreq = name_query_send(state, state->ev,
				 state->name, state->name_type,
				 false, true, &state->server);
	state->num_sent += 1;
	if (tevent_req_nomem(subreq, req)) {
		return tevent_req_post(req, ev);
	}
	if (!tevent_req_set_endtime(subreq, state->ev,
				    timeval_current_ofs(2, 0))) {
		tevent_req_oom(req);
		return tevent_req_post(req, ev);
	}
	tevent_req_set_callback(subreq, query_wins_list_done, req);
	return req;
}

static void query_wins_list_done(struct tevent_req *subreq)
{
	struct tevent_req *req = tevent_req_callback_data(
		subreq, struct tevent_req);
	struct query_wins_list_state *state = tevent_req_data(
		req, struct query_wins_list_state);
	NTSTATUS status;

	status = name_query_recv(subreq, state,
				 &state->addrs, &state->num_addrs,
				 &state->flags);
	TALLOC_FREE(subreq);
	if (NT_STATUS_IS_OK(status)) {
		tevent_req_done(req);
		return;
	}
	if (!NT_STATUS_EQUAL(status, NT_STATUS_IO_TIMEOUT)) {
		tevent_req_nterror(req, status);
		return;
	}
	wins_srv_died(state->servers[state->num_sent-1],
		      my_socket_addr_v4());

	if (state->num_sent == state->num_servers) {
		tevent_req_nterror(req, NT_STATUS_NOT_FOUND);
		return;
	}

	in_addr_to_sockaddr_storage(
		&state->server, state->servers[state->num_sent]);

	subreq = name_query_send(state, state->ev,
				 state->name, state->name_type,
				 false, true, &state->server);
	state->num_sent += 1;
	if (tevent_req_nomem(subreq, req)) {
		return;
	}
	if (!tevent_req_set_endtime(subreq, state->ev,
				    timeval_current_ofs(2, 0))) {
		tevent_req_oom(req);
		return;
	}
	tevent_req_set_callback(subreq, query_wins_list_done, req);
}

static NTSTATUS query_wins_list_recv(struct tevent_req *req,
				     TALLOC_CTX *mem_ctx,
				     struct sockaddr_storage **addrs,
				     int *num_addrs,
				     uint8_t *flags)
{
	struct query_wins_list_state *state = tevent_req_data(
		req, struct query_wins_list_state);
	NTSTATUS status;

	if (tevent_req_is_nterror(req, &status)) {
		return status;
	}
	if (addrs != NULL) {
		*addrs = talloc_move(mem_ctx, &state->addrs);
	}
	if (num_addrs != NULL) {
		*num_addrs = state->num_addrs;
	}
	if (flags != NULL) {
		*flags = state->flags;
	}
	return NT_STATUS_OK;
}

struct resolve_wins_state {
	int num_sent;
	int num_received;

	struct sockaddr_storage *addrs;
	int num_addrs;
	uint8_t flags;
};

static void resolve_wins_done(struct tevent_req *subreq);

struct tevent_req *resolve_wins_send(TALLOC_CTX *mem_ctx,
				     struct tevent_context *ev,
				     const char *name,
				     int name_type)
{
	struct tevent_req *req, *subreq;
	struct resolve_wins_state *state;
	char **wins_tags = NULL;
	struct sockaddr_storage src_ss;
	struct in_addr src_ip;
	int i, num_wins_tags;

	req = tevent_req_create(mem_ctx, &state,
				struct resolve_wins_state);
	if (req == NULL) {
		return NULL;
	}

	if (wins_srv_count() < 1) {
		DEBUG(3,("resolve_wins: WINS server resolution selected "
			"and no WINS servers listed.\n"));
		tevent_req_nterror(req, NT_STATUS_INVALID_PARAMETER);
		goto fail;
	}

	/* the address we will be sending from */
	if (!interpret_string_addr(&src_ss, lp_socket_address(),
				AI_NUMERICHOST|AI_PASSIVE)) {
		zero_sockaddr(&src_ss);
	}

	if (src_ss.ss_family != AF_INET) {
		char addr[INET6_ADDRSTRLEN];
		print_sockaddr(addr, sizeof(addr), &src_ss);
		DEBUG(3,("resolve_wins: cannot receive WINS replies "
			"on IPv6 address %s\n",
			addr));
		tevent_req_nterror(req, NT_STATUS_INVALID_PARAMETER);
		goto fail;
	}

	src_ip = ((const struct sockaddr_in *)(void *)&src_ss)->sin_addr;

	wins_tags = wins_srv_tags();
	if (wins_tags == NULL) {
		tevent_req_nterror(req, NT_STATUS_INVALID_PARAMETER);
		goto fail;
	}

	num_wins_tags = 0;
	while (wins_tags[num_wins_tags] != NULL) {
		num_wins_tags += 1;
	}

	for (i=0; i<num_wins_tags; i++) {
		int num_servers, num_alive;
		struct in_addr *servers, *alive;
		int j;

		if (!wins_server_tag_ips(wins_tags[i], talloc_tos(),
					 &servers, &num_servers)) {
			DEBUG(10, ("wins_server_tag_ips failed for tag %s\n",
				   wins_tags[i]));
			continue;
		}

		alive = talloc_array(state, struct in_addr, num_servers);
		if (tevent_req_nomem(alive, req)) {
			goto fail;
		}

		num_alive = 0;
		for (j=0; j<num_servers; j++) {
			struct in_addr wins_ip = servers[j];

			if (global_in_nmbd && ismyip_v4(wins_ip)) {
				/* yikes! we'll loop forever */
				continue;
			}
			/* skip any that have been unresponsive lately */
			if (wins_srv_is_dead(wins_ip, src_ip)) {
				continue;
			}
			DEBUG(3, ("resolve_wins: using WINS server %s "
				 "and tag '%s'\n",
				  inet_ntoa(wins_ip), wins_tags[i]));
			alive[num_alive] = wins_ip;
			num_alive += 1;
		}
		TALLOC_FREE(servers);

		if (num_alive == 0) {
			continue;
		}

		subreq = query_wins_list_send(
			state, ev, src_ip, name, name_type,
			alive, num_alive);
		if (tevent_req_nomem(subreq, req)) {
			goto fail;
		}
		tevent_req_set_callback(subreq, resolve_wins_done, req);
		state->num_sent += 1;
	}

	if (state->num_sent == 0) {
		tevent_req_nterror(req, NT_STATUS_NOT_FOUND);
		goto fail;
	}

	wins_srv_tags_free(wins_tags);
	return req;
fail:
	wins_srv_tags_free(wins_tags);
	return tevent_req_post(req, ev);
}

static void resolve_wins_done(struct tevent_req *subreq)
{
	struct tevent_req *req = tevent_req_callback_data(
		subreq, struct tevent_req);
	struct resolve_wins_state *state = tevent_req_data(
		req, struct resolve_wins_state);
	NTSTATUS status;

	status = query_wins_list_recv(subreq, state, &state->addrs,
				      &state->num_addrs, &state->flags);
	if (NT_STATUS_IS_OK(status)) {
		tevent_req_done(req);
		return;
	}

	state->num_received += 1;

	if (state->num_received < state->num_sent) {
		/*
		 * Wait for the others
		 */
		return;
	}
	tevent_req_nterror(req, status);
}

NTSTATUS resolve_wins_recv(struct tevent_req *req, TALLOC_CTX *mem_ctx,
			   struct sockaddr_storage **addrs,
			   int *num_addrs, uint8_t *flags)
{
	struct resolve_wins_state *state = tevent_req_data(
		req, struct resolve_wins_state);
	NTSTATUS status;

	if (tevent_req_is_nterror(req, &status)) {
		return status;
	}
	if (addrs != NULL) {
		*addrs = talloc_move(mem_ctx, &state->addrs);
	}
	if (num_addrs != NULL) {
		*num_addrs = state->num_addrs;
	}
	if (flags != NULL) {
		*flags = state->flags;
	}
	return NT_STATUS_OK;
}

/********************************************************
 Resolve via "wins" method.
*********************************************************/

NTSTATUS resolve_wins(const char *name,
		int name_type,
		TALLOC_CTX *mem_ctx,
		struct sockaddr_storage **return_iplist,
		int *return_count)
{
	struct tevent_context *ev;
	struct tevent_req *req;
	NTSTATUS status = NT_STATUS_NO_MEMORY;

	ev = tevent_context_init(talloc_tos());
	if (ev == NULL) {
		goto fail;
	}
	req = resolve_wins_send(ev, ev, name, name_type);
	if (req == NULL) {
		goto fail;
	}
	if (!tevent_req_poll_ntstatus(req, ev, &status)) {
		goto fail;
	}
	status = resolve_wins_recv(req, mem_ctx, return_iplist, return_count,
				   NULL);
fail:
	TALLOC_FREE(ev);
	return status;
}

/********************************************************
 Resolve via "lmhosts" method.
*********************************************************/

static NTSTATUS resolve_lmhosts(const char *name, int name_type,
				struct ip_service **return_iplist,
				int *return_count)
{
	/*
	 * "lmhosts" means parse the local lmhosts file.
	 */
	struct sockaddr_storage *ss_list;
	NTSTATUS status = NT_STATUS_DOMAIN_CONTROLLER_NOT_FOUND;
	TALLOC_CTX *ctx = NULL;

	*return_iplist = NULL;
	*return_count = 0;

	DEBUG(3,("resolve_lmhosts: "
		"Attempting lmhosts lookup for name %s<0x%x>\n",
		name, name_type));

	ctx = talloc_init("resolve_lmhosts");
	if (!ctx) {
		return NT_STATUS_NO_MEMORY;
	}

	status = resolve_lmhosts_file_as_sockaddr(get_dyn_LMHOSTSFILE(), 
						  name, name_type, 
						  ctx, 
						  &ss_list, 
						  return_count);
	if (NT_STATUS_IS_OK(status)) {
		if (convert_ss2service(return_iplist, 
				       ss_list,
				       *return_count)) {
			talloc_free(ctx);
			return NT_STATUS_OK;
		} else {
			talloc_free(ctx);
			return NT_STATUS_NO_MEMORY;
		}
	}
	talloc_free(ctx);
	return status;
}


/********************************************************
 Resolve via "hosts" method.
*********************************************************/

static NTSTATUS resolve_hosts(const char *name, int name_type,
			      struct ip_service **return_iplist,
			      int *return_count)
{
	/*
	 * "host" means do a localhost, or dns lookup.
	 */
	struct addrinfo hints;
	struct addrinfo *ailist = NULL;
	struct addrinfo *res = NULL;
	int ret = -1;
	int i = 0;
	const char *dns_hosts_file;

	if ( name_type != 0x20 && name_type != 0x0) {
		DEBUG(5, ("resolve_hosts: not appropriate "
			"for name type <0x%x>\n",
			name_type));
		return NT_STATUS_INVALID_PARAMETER;
	}

	*return_iplist = NULL;
	*return_count = 0;

	DEBUG(3,("resolve_hosts: Attempting host lookup for name %s<0x%x>\n",
				name, name_type));

	ZERO_STRUCT(hints);
	/* By default make sure it supports TCP. */
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_ADDRCONFIG;

#if !defined(HAVE_IPV6)
	/* Unless we have IPv6, we really only want IPv4 addresses back. */
	hints.ai_family = AF_INET;
#endif

	dns_hosts_file = lp_parm_const_string(-1, "resolv", "host file", NULL);
	if (dns_hosts_file) {
		struct sockaddr_storage *ss_list;
		NTSTATUS status;
		TALLOC_CTX *ctx = talloc_stackframe();
		if (!ctx) {
			return NT_STATUS_NO_MEMORY;
		}

		status = resolve_dns_hosts_file_as_sockaddr(dns_hosts_file, name, false,
							    ctx, &ss_list, return_count);
		if (NT_STATUS_IS_OK(status)) {
			if (convert_ss2service(return_iplist,
					       ss_list,
					       *return_count)) {
				talloc_free(ctx);
				return NT_STATUS_OK;
			} else {
				talloc_free(ctx);
				return NT_STATUS_NO_MEMORY;
			}
		}
		talloc_free(ctx);
		return NT_STATUS_UNSUCCESSFUL;
	}

	ret = getaddrinfo(name,
			NULL,
			&hints,
			&ailist);
	if (ret) {
		DEBUG(3,("resolve_hosts: getaddrinfo failed for name %s [%s]\n",
			name,
			gai_strerror(ret) ));
	}

	for (res = ailist; res; res = res->ai_next) {
		struct sockaddr_storage ss;

		if (!res->ai_addr || res->ai_addrlen == 0) {
			continue;
		}

		ZERO_STRUCT(ss);
		memcpy(&ss, res->ai_addr, res->ai_addrlen);

		*return_count += 1;

		*return_iplist = SMB_REALLOC_ARRAY(*return_iplist,
						struct ip_service,
						*return_count);
		if (!*return_iplist) {
			DEBUG(3,("resolve_hosts: malloc fail !\n"));
			freeaddrinfo(ailist);
			return NT_STATUS_NO_MEMORY;
		}
		(*return_iplist)[i].ss = ss;
		(*return_iplist)[i].port = PORT_NONE;
		i++;
	}
	if (ailist) {
		freeaddrinfo(ailist);
	}
	if (*return_count) {
		return NT_STATUS_OK;
	}
	return NT_STATUS_UNSUCCESSFUL;
}

/********************************************************
 Resolve via "ADS" method.
*********************************************************/

/* Special name type used to cause a _kerberos DNS lookup. */
#define KDC_NAME_TYPE 0xDCDC

static NTSTATUS resolve_ads(const char *name,
			    int name_type,
			    const char *sitename,
			    struct ip_service **return_iplist,
			    int *return_count)
{
	int 			i, j;
	NTSTATUS  		status;
	TALLOC_CTX		*ctx;
	struct dns_rr_srv	*dcs = NULL;
	int			numdcs = 0;
	int			numaddrs = 0;

	if ((name_type != 0x1c) && (name_type != KDC_NAME_TYPE) &&
	    (name_type != 0x1b)) {
		return NT_STATUS_INVALID_PARAMETER;
	}

	if ( (ctx = talloc_init("resolve_ads")) == NULL ) {
		DEBUG(0,("resolve_ads: talloc_init() failed!\n"));
		return NT_STATUS_NO_MEMORY;
	}

	/* The DNS code needs fixing to find IPv6 addresses... JRA. */

	switch (name_type) {
		case 0x1b:
			DEBUG(5,("resolve_ads: Attempting to resolve "
				 "PDC for %s using DNS\n", name));
			status = ads_dns_query_pdc(ctx, name, &dcs, &numdcs);
			break;

		case 0x1c:
			DEBUG(5,("resolve_ads: Attempting to resolve "
				 "DCs for %s using DNS\n", name));
			status = ads_dns_query_dcs(ctx, name, sitename, &dcs,
						   &numdcs);
			break;
		case KDC_NAME_TYPE:
			DEBUG(5,("resolve_ads: Attempting to resolve "
				 "KDCs for %s using DNS\n", name));
			status = ads_dns_query_kdcs(ctx, name, sitename, &dcs,
						    &numdcs);
			break;
		default:
			status = NT_STATUS_INVALID_PARAMETER;
			break;
	}

	if ( !NT_STATUS_IS_OK( status ) ) {
		talloc_destroy(ctx);
		return status;
	}

	for (i=0;i<numdcs;i++) {
		numaddrs += MAX(dcs[i].num_ips,1);
	}

	if ((*return_iplist = SMB_MALLOC_ARRAY(struct ip_service, numaddrs)) ==
			NULL ) {
		DEBUG(0,("resolve_ads: malloc failed for %d entries\n",
					numaddrs ));
		talloc_destroy(ctx);
		return NT_STATUS_NO_MEMORY;
	}

	/* now unroll the list of IP addresses */

	*return_count = 0;
	i = 0;
	j = 0;
	while ( i < numdcs && (*return_count<numaddrs) ) {
		struct ip_service *r = &(*return_iplist)[*return_count];

		r->port = dcs[i].port;

		/* If we don't have an IP list for a name, lookup it up */

		if (!dcs[i].ss_s) {
			interpret_string_addr(&r->ss, dcs[i].hostname, 0);
			i++;
			j = 0;
		} else {
			/* use the IP addresses from the SRV sresponse */

			if ( j >= dcs[i].num_ips ) {
				i++;
				j = 0;
				continue;
			}

			r->ss = dcs[i].ss_s[j];
			j++;
		}

		/* make sure it is a valid IP.  I considered checking the
		 * negative connection cache, but this is the wrong place
		 * for it. Maybe only as a hack. After think about it, if
		 * all of the IP addresses returned from DNS are dead, what
		 * hope does a netbios name lookup have ? The standard reason
		 * for falling back to netbios lookups is that our DNS server
		 * doesn't know anything about the DC's   -- jerry */

		if (!is_zero_addr(&r->ss)) {
			(*return_count)++;
		}
	}

	talloc_destroy(ctx);
	return NT_STATUS_OK;
}

/*******************************************************************
 Internal interface to resolve a name into an IP address.
 Use this function if the string is either an IP address, DNS
 or host name or NetBIOS name. This uses the name switch in the
 smb.conf to determine the order of name resolution.

 Added support for ip addr/port to support ADS ldap servers.
 the only place we currently care about the port is in the
 resolve_hosts() when looking up DC's via SRV RR entries in DNS
**********************************************************************/

NTSTATUS internal_resolve_name(const char *name,
			        int name_type,
				const char *sitename,
				struct ip_service **return_iplist,
				int *return_count,
				const char *resolve_order)
{
	char *tok;
	const char *ptr;
	NTSTATUS status = NT_STATUS_UNSUCCESSFUL;
	int i;
	TALLOC_CTX *frame = NULL;

	*return_iplist = NULL;
	*return_count = 0;

	DEBUG(10, ("internal_resolve_name: looking up %s#%x (sitename %s)\n",
			name, name_type, sitename ? sitename : "(null)"));

	if (is_ipaddress(name)) {
		if ((*return_iplist = SMB_MALLOC_P(struct ip_service)) ==
				NULL) {
			DEBUG(0,("internal_resolve_name: malloc fail !\n"));
			return NT_STATUS_NO_MEMORY;
		}

		/* ignore the port here */
		(*return_iplist)->port = PORT_NONE;

		/* if it's in the form of an IP address then get the lib to interpret it */
		if (!interpret_string_addr(&(*return_iplist)->ss,
					name, AI_NUMERICHOST)) {
			DEBUG(1,("internal_resolve_name: interpret_string_addr "
				"failed on %s\n",
				name));
			SAFE_FREE(*return_iplist);
			return NT_STATUS_INVALID_PARAMETER;
		}
		*return_count = 1;
		return NT_STATUS_OK;
	}

	/* Check name cache */

	if (namecache_fetch(name, name_type, return_iplist, return_count)) {
		/* This could be a negative response */
		if (*return_count > 0) {
			return NT_STATUS_OK;
		} else {
			return NT_STATUS_UNSUCCESSFUL;
		}
	}

	/* set the name resolution order */

	if (strcmp( resolve_order, "NULL") == 0) {
		DEBUG(8,("internal_resolve_name: all lookups disabled\n"));
		return NT_STATUS_INVALID_PARAMETER;
	}

	if (!resolve_order[0]) {
		ptr = "host";
	} else {
		ptr = resolve_order;
	}

	/* iterate through the name resolution backends */

	frame = talloc_stackframe();
	while (next_token_talloc(frame, &ptr, &tok, LIST_SEP)) {
		if((strequal(tok, "host") || strequal(tok, "hosts"))) {
			status = resolve_hosts(name, name_type, return_iplist,
					       return_count);
			if (NT_STATUS_IS_OK(status)) {
				goto done;
			}
		} else if(strequal( tok, "kdc")) {
			/* deal with KDC_NAME_TYPE names here.
			 * This will result in a SRV record lookup */
			status = resolve_ads(name, KDC_NAME_TYPE, sitename,
					     return_iplist, return_count);
			if (NT_STATUS_IS_OK(status)) {
				/* Ensure we don't namecache
				 * this with the KDC port. */
				name_type = KDC_NAME_TYPE;
				goto done;
			}
		} else if(strequal( tok, "ads")) {
			/* deal with 0x1c and 0x1b names here.
			 * This will result in a SRV record lookup */
			status = resolve_ads(name, name_type, sitename,
					     return_iplist, return_count);
			if (NT_STATUS_IS_OK(status)) {
				goto done;
			}
		} else if(strequal( tok, "lmhosts")) {
			status = resolve_lmhosts(name, name_type,
						 return_iplist, return_count);
			if (NT_STATUS_IS_OK(status)) {
				goto done;
			}
		} else if(strequal( tok, "wins")) {
			/* don't resolve 1D via WINS */
			struct sockaddr_storage *ss_list;
			if (name_type != 0x1D) {
				status = resolve_wins(name, name_type,
						      talloc_tos(),
						      &ss_list,
						      return_count);
				if (NT_STATUS_IS_OK(status)) {
					if (!convert_ss2service(return_iplist,
								ss_list,
								*return_count)) {
						status = NT_STATUS_NO_MEMORY;
					}
					goto done;
				}
			}
		} else if(strequal( tok, "bcast")) {
			struct sockaddr_storage *ss_list;
			status = name_resolve_bcast(
				name, name_type, talloc_tos(),
				&ss_list, return_count);
			if (NT_STATUS_IS_OK(status)) {
				if (!convert_ss2service(return_iplist,
							ss_list,
							*return_count)) {
					status = NT_STATUS_NO_MEMORY;
				}
				goto done;
			}
		} else {
			DEBUG(0,("resolve_name: unknown name switch type %s\n",
				tok));
		}
	}

	/* All of the resolve_* functions above have returned false. */

	TALLOC_FREE(frame);
	SAFE_FREE(*return_iplist);
	*return_count = 0;

	return NT_STATUS_UNSUCCESSFUL;

  done:

	/* Remove duplicate entries.  Some queries, notably #1c (domain
	controllers) return the PDC in iplist[0] and then all domain
	controllers including the PDC in iplist[1..n].  Iterating over
	the iplist when the PDC is down will cause two sets of timeouts. */

	if ( *return_count ) {
		*return_count = remove_duplicate_addrs2(*return_iplist,
					*return_count );
	}

	/* Save in name cache */
	if ( DEBUGLEVEL >= 100 ) {
		for (i = 0; i < *return_count && DEBUGLEVEL == 100; i++) {
			char addr[INET6_ADDRSTRLEN];
			print_sockaddr(addr, sizeof(addr),
					&(*return_iplist)[i].ss);
			DEBUG(100, ("Storing name %s of type %d (%s:%d)\n",
					name,
					name_type,
					addr,
					(*return_iplist)[i].port));
		}
	}

	namecache_store(name, name_type, *return_count, *return_iplist);

	/* Display some debugging info */

	if ( DEBUGLEVEL >= 10 ) {
		DEBUG(10, ("internal_resolve_name: returning %d addresses: ",
					*return_count));

		for (i = 0; i < *return_count; i++) {
			char addr[INET6_ADDRSTRLEN];
			print_sockaddr(addr, sizeof(addr),
					&(*return_iplist)[i].ss);
			DEBUGADD(10, ("%s:%d ",
					addr,
					(*return_iplist)[i].port));
		}
		DEBUG(10, ("\n"));
	}

	TALLOC_FREE(frame);
	return status;
}

/********************************************************
 Internal interface to resolve a name into one IP address.
 Use this function if the string is either an IP address, DNS
 or host name or NetBIOS name. This uses the name switch in the
 smb.conf to determine the order of name resolution.
*********************************************************/

bool resolve_name(const char *name,
		struct sockaddr_storage *return_ss,
		int name_type,
		bool prefer_ipv4)
{
	struct ip_service *ss_list = NULL;
	char *sitename = NULL;
	int count = 0;
	NTSTATUS status;

	if (is_ipaddress(name)) {
		return interpret_string_addr(return_ss, name, AI_NUMERICHOST);
	}

	sitename = sitename_fetch(lp_realm()); /* wild guess */

	status = internal_resolve_name(name, name_type, sitename,
				       &ss_list, &count,
				       lp_name_resolve_order());
	if (NT_STATUS_IS_OK(status)) {
		int i;

		if (prefer_ipv4) {
			for (i=0; i<count; i++) {
				if (!is_zero_addr(&ss_list[i].ss) &&
				    !is_broadcast_addr((struct sockaddr *)(void *)&ss_list[i].ss) &&
						(ss_list[i].ss.ss_family == AF_INET)) {
					*return_ss = ss_list[i].ss;
					SAFE_FREE(ss_list);
					SAFE_FREE(sitename);
					return True;
				}
			}
		}

		/* only return valid addresses for TCP connections */
		for (i=0; i<count; i++) {
			if (!is_zero_addr(&ss_list[i].ss) &&
			    !is_broadcast_addr((struct sockaddr *)(void *)&ss_list[i].ss)) {
				*return_ss = ss_list[i].ss;
				SAFE_FREE(ss_list);
				SAFE_FREE(sitename);
				return True;
			}
		}
	}

	SAFE_FREE(ss_list);
	SAFE_FREE(sitename);
	return False;
}

/********************************************************
 Internal interface to resolve a name into a list of IP addresses.
 Use this function if the string is either an IP address, DNS
 or host name or NetBIOS name. This uses the name switch in the
 smb.conf to determine the order of name resolution.
*********************************************************/

NTSTATUS resolve_name_list(TALLOC_CTX *ctx,
		const char *name,
		int name_type,
		struct sockaddr_storage **return_ss_arr,
		unsigned int *p_num_entries)
{
	struct ip_service *ss_list = NULL;
	char *sitename = NULL;
	int count = 0;
	int i;
	unsigned int num_entries;
	NTSTATUS status;

	*p_num_entries = 0;
	*return_ss_arr = NULL;

	if (is_ipaddress(name)) {
		*return_ss_arr = talloc(ctx, struct sockaddr_storage);
		if (!*return_ss_arr) {
			return NT_STATUS_NO_MEMORY;
		}
		if (!interpret_string_addr(*return_ss_arr, name, AI_NUMERICHOST)) {
			TALLOC_FREE(*return_ss_arr);
			return NT_STATUS_BAD_NETWORK_NAME;
		}
		*p_num_entries = 1;
		return NT_STATUS_OK;
	}

	sitename = sitename_fetch(lp_realm()); /* wild guess */

	status = internal_resolve_name(name, name_type, sitename,
						  &ss_list, &count,
						  lp_name_resolve_order());
	SAFE_FREE(sitename);

	if (!NT_STATUS_IS_OK(status)) {
		return status;
	}

	/* only return valid addresses for TCP connections */
	for (i=0, num_entries = 0; i<count; i++) {
		if (!is_zero_addr(&ss_list[i].ss) &&
		    !is_broadcast_addr((struct sockaddr *)(void *)&ss_list[i].ss)) {
			num_entries++;
		}
	}
	if (num_entries == 0) {
		SAFE_FREE(ss_list);
		return NT_STATUS_BAD_NETWORK_NAME;
	}

	*return_ss_arr = talloc_array(ctx,
				struct sockaddr_storage,
				num_entries);
	if (!(*return_ss_arr)) {
		SAFE_FREE(ss_list);
		return NT_STATUS_NO_MEMORY;
	}

	for (i=0, num_entries = 0; i<count; i++) {
		if (!is_zero_addr(&ss_list[i].ss) &&
		    !is_broadcast_addr((struct sockaddr *)(void *)&ss_list[i].ss)) {
			(*return_ss_arr)[num_entries++] = ss_list[i].ss;
		}
	}

	status = NT_STATUS_OK;
	*p_num_entries = num_entries;

	SAFE_FREE(ss_list);
	return NT_STATUS_OK;
}

/********************************************************
 Find the IP address of the master browser or DMB for a workgroup.
*********************************************************/

bool find_master_ip(const char *group, struct sockaddr_storage *master_ss)
{
	struct ip_service *ip_list = NULL;
	int count = 0;
	NTSTATUS status;

	if (lp_disable_netbios()) {
		DEBUG(5,("find_master_ip(%s): netbios is disabled\n", group));
		return false;
	}

	status = internal_resolve_name(group, 0x1D, NULL, &ip_list, &count,
				       lp_name_resolve_order());
	if (NT_STATUS_IS_OK(status)) {
		*master_ss = ip_list[0].ss;
		SAFE_FREE(ip_list);
		return true;
	}

	status = internal_resolve_name(group, 0x1B, NULL, &ip_list, &count,
				       lp_name_resolve_order());
	if (NT_STATUS_IS_OK(status)) {
		*master_ss = ip_list[0].ss;
		SAFE_FREE(ip_list);
		return true;
	}

	SAFE_FREE(ip_list);
	return false;
}

/********************************************************
 Get the IP address list of the primary domain controller
 for a domain.
*********************************************************/

bool get_pdc_ip(const char *domain, struct sockaddr_storage *pss)
{
	struct ip_service *ip_list = NULL;
	int count = 0;
	NTSTATUS status = NT_STATUS_DOMAIN_CONTROLLER_NOT_FOUND;

	/* Look up #1B name */

	if (lp_security() == SEC_ADS) {
		status = internal_resolve_name(domain, 0x1b, NULL, &ip_list,
					       &count, "ads");
	}

	if (!NT_STATUS_IS_OK(status) || count == 0) {
		status = internal_resolve_name(domain, 0x1b, NULL, &ip_list,
					       &count,
					       lp_name_resolve_order());
		if (!NT_STATUS_IS_OK(status)) {
			return false;
		}
	}

	/* if we get more than 1 IP back we have to assume it is a
	   multi-homed PDC and not a mess up */

	if ( count > 1 ) {
		DEBUG(6,("get_pdc_ip: PDC has %d IP addresses!\n", count));
		sort_service_list(ip_list, count);
	}

	*pss = ip_list[0].ss;
	SAFE_FREE(ip_list);
	return true;
}

/* Private enum type for lookups. */

enum dc_lookup_type { DC_NORMAL_LOOKUP, DC_ADS_ONLY, DC_KDC_ONLY };

/********************************************************
 Get the IP address list of the domain controllers for
 a domain.
*********************************************************/

static NTSTATUS get_dc_list(const char *domain,
			const char *sitename,
			struct ip_service **ip_list,
			int *count,
			enum dc_lookup_type lookup_type,
			bool *ordered)
{
	char *resolve_order = NULL;
	char *saf_servername = NULL;
	char *pserver = NULL;
	const char *p;
	char *port_str = NULL;
	int port;
	char *name;
	int num_addresses = 0;
	int  local_count, i, j;
	struct ip_service *return_iplist = NULL;
	struct ip_service *auto_ip_list = NULL;
	bool done_auto_lookup = false;
	int auto_count = 0;
	NTSTATUS status;
	TALLOC_CTX *ctx = talloc_init("get_dc_list");

	*ip_list = NULL;
	*count = 0;

	if (!ctx) {
		return NT_STATUS_NO_MEMORY;
	}

	*ordered = False;

	/* if we are restricted to solely using DNS for looking
	   up a domain controller, make sure that host lookups
	   are enabled for the 'name resolve order'.  If host lookups
	   are disabled and ads_only is True, then set the string to
	   NULL. */

	resolve_order = talloc_strdup(ctx, lp_name_resolve_order());
	if (!resolve_order) {
		status = NT_STATUS_NO_MEMORY;
		goto out;
	}
	strlower_m(resolve_order);
	if (lookup_type == DC_ADS_ONLY)  {
		if (strstr( resolve_order, "host")) {
			resolve_order = talloc_strdup(ctx, "ads");

			/* DNS SRV lookups used by the ads resolver
			   are already sorted by priority and weight */
			*ordered = true;
		} else {
                        resolve_order = talloc_strdup(ctx, "NULL");
		}
	} else if (lookup_type == DC_KDC_ONLY) {
		/* DNS SRV lookups used by the ads/kdc resolver
		   are already sorted by priority and weight */
		*ordered = true;
		resolve_order = talloc_strdup(ctx, "kdc");
	}
	if (!resolve_order) {
		status = NT_STATUS_NO_MEMORY;
		goto out;
	}

	/* fetch the server we have affinity for.  Add the
	   'password server' list to a search for our domain controllers */

	saf_servername = saf_fetch( domain);

	if (strequal(domain, lp_workgroup()) || strequal(domain, lp_realm())) {
		pserver = talloc_asprintf(ctx, "%s, %s",
			saf_servername ? saf_servername : "",
			lp_passwordserver());
	} else {
		pserver = talloc_asprintf(ctx, "%s, *",
			saf_servername ? saf_servername : "");
	}

	SAFE_FREE(saf_servername);
	if (!pserver) {
		status = NT_STATUS_NO_MEMORY;
		goto out;
	}

	/* if we are starting from scratch, just lookup DOMAIN<0x1c> */

	if (!*pserver ) {
		DEBUG(10,("get_dc_list: no preferred domain controllers.\n"));
		status = internal_resolve_name(domain, 0x1C, sitename, ip_list,
					     count, resolve_order);
		goto out;
	}

	DEBUG(3,("get_dc_list: preferred server list: \"%s\"\n", pserver ));

	/*
	 * if '*' appears in the "password server" list then add
	 * an auto lookup to the list of manually configured
	 * DC's.  If any DC is listed by name, then the list should be
	 * considered to be ordered
	 */

	p = pserver;
	while (next_token_talloc(ctx, &p, &name, LIST_SEP)) {
		if (!done_auto_lookup && strequal(name, "*")) {
			status = internal_resolve_name(domain, 0x1C, sitename,
						       &auto_ip_list,
						       &auto_count,
						       resolve_order);
			if (NT_STATUS_IS_OK(status)) {
				num_addresses += auto_count;
			}
			done_auto_lookup = true;
			DEBUG(8,("Adding %d DC's from auto lookup\n",
						auto_count));
		} else  {
			num_addresses++;
		}
	}

	/* if we have no addresses and haven't done the auto lookup, then
	   just return the list of DC's.  Or maybe we just failed. */

	if ((num_addresses == 0)) {
		if (done_auto_lookup) {
			DEBUG(4,("get_dc_list: no servers found\n"));
			status = NT_STATUS_NO_LOGON_SERVERS;
			goto out;
		}
		status = internal_resolve_name(domain, 0x1C, sitename, ip_list,
					     count, resolve_order);
		goto out;
	}

	if ((return_iplist = SMB_MALLOC_ARRAY(struct ip_service,
					num_addresses)) == NULL) {
		DEBUG(3,("get_dc_list: malloc fail !\n"));
		status = NT_STATUS_NO_MEMORY;
		goto out;
	}

	p = pserver;
	local_count = 0;

	/* fill in the return list now with real IP's */

	while ((local_count<num_addresses) &&
			next_token_talloc(ctx, &p, &name, LIST_SEP)) {
		struct sockaddr_storage name_ss;

		/* copy any addersses from the auto lookup */

		if (strequal(name, "*")) {
			for (j=0; j<auto_count; j++) {
				char addr[INET6_ADDRSTRLEN];
				print_sockaddr(addr,
						sizeof(addr),
						&auto_ip_list[j].ss);
				/* Check for and don't copy any
				 * known bad DC IP's. */
				if(!NT_STATUS_IS_OK(check_negative_conn_cache(
						domain,
						addr))) {
					DEBUG(5,("get_dc_list: "
						"negative entry %s removed "
						"from DC list\n",
						addr));
					continue;
				}
				return_iplist[local_count].ss =
					auto_ip_list[j].ss;
				return_iplist[local_count].port =
					auto_ip_list[j].port;
				local_count++;
			}
			continue;
		}

		/* added support for address:port syntax for ads
		 * (not that I think anyone will ever run the LDAP
		 * server in an AD domain on something other than
		 * port 389 */

		port = (lp_security() == SEC_ADS) ? LDAP_PORT : PORT_NONE;
		if ((port_str=strchr(name, ':')) != NULL) {
			*port_str = '\0';
			port_str++;
			port = atoi(port_str);
		}

		/* explicit lookup; resolve_name() will
		 * handle names & IP addresses */
		if (resolve_name( name, &name_ss, 0x20, true )) {
			char addr[INET6_ADDRSTRLEN];
			print_sockaddr(addr,
					sizeof(addr),
					&name_ss);

			/* Check for and don't copy any known bad DC IP's. */
			if( !NT_STATUS_IS_OK(check_negative_conn_cache(domain,
							addr)) ) {
				DEBUG(5,("get_dc_list: negative entry %s "
					"removed from DC list\n",
					name ));
				continue;
			}

			return_iplist[local_count].ss = name_ss;
			return_iplist[local_count].port = port;
			local_count++;
			*ordered = true;
		}
	}

	/* need to remove duplicates in the list if we have any
	   explicit password servers */

	if (local_count) {
		local_count = remove_duplicate_addrs2(return_iplist,
				local_count );
	}

	/* For DC's we always prioritize IPv4 due to W2K3 not
	 * supporting LDAP, KRB5 or CLDAP over IPv6. */

	if (local_count && return_iplist) {
		prioritize_ipv4_list(return_iplist, local_count);
	}

	if ( DEBUGLEVEL >= 4 ) {
		DEBUG(4,("get_dc_list: returning %d ip addresses "
				"in an %sordered list\n",
				local_count,
				*ordered ? "":"un"));
		DEBUG(4,("get_dc_list: "));
		for ( i=0; i<local_count; i++ ) {
			char addr[INET6_ADDRSTRLEN];
			print_sockaddr(addr,
					sizeof(addr),
					&return_iplist[i].ss);
			DEBUGADD(4,("%s:%d ", addr, return_iplist[i].port ));
		}
		DEBUGADD(4,("\n"));
	}

	*ip_list = return_iplist;
	*count = local_count;

	status = ( *count != 0 ? NT_STATUS_OK : NT_STATUS_NO_LOGON_SERVERS );

  out:

	if (!NT_STATUS_IS_OK(status)) {
		SAFE_FREE(return_iplist);
		*ip_list = NULL;
		*count = 0;
	}

	SAFE_FREE(auto_ip_list);
	TALLOC_FREE(ctx);
	return status;
}

/*********************************************************************
 Small wrapper function to get the DC list and sort it if neccessary.
*********************************************************************/

NTSTATUS get_sorted_dc_list( const char *domain,
			const char *sitename,
			struct ip_service **ip_list,
			int *count,
			bool ads_only )
{
	bool ordered = false;
	NTSTATUS status;
	enum dc_lookup_type lookup_type = DC_NORMAL_LOOKUP;

	*ip_list = NULL;
	*count = 0;

	DEBUG(8,("get_sorted_dc_list: attempting lookup "
		"for name %s (sitename %s) using [%s]\n",
		domain,
		sitename ? sitename : "NULL",
		(ads_only ? "ads" : lp_name_resolve_order())));

	if (ads_only) {
		lookup_type = DC_ADS_ONLY;
	}

	status = get_dc_list(domain, sitename, ip_list,
			count, lookup_type, &ordered);
	if (NT_STATUS_EQUAL(status, NT_STATUS_NO_LOGON_SERVERS)
	    && sitename) {
		DEBUG(3,("get_sorted_dc_list: no server for name %s available"
			 " in site %s, fallback to all servers\n",
			 domain, sitename));
		status = get_dc_list(domain, NULL, ip_list,
				     count, lookup_type, &ordered);
	}

	if (!NT_STATUS_IS_OK(status)) {
		SAFE_FREE(*ip_list);
		*count = 0;
		return status;
	}

	/* only sort if we don't already have an ordered list */
	if (!ordered) {
		sort_service_list(*ip_list, *count);
	}

	return NT_STATUS_OK;
}

/*********************************************************************
 Get the KDC list - re-use all the logic in get_dc_list.
*********************************************************************/

NTSTATUS get_kdc_list( const char *realm,
			const char *sitename,
			struct ip_service **ip_list,
			int *count)
{
	bool ordered;
	NTSTATUS status;

	*count = 0;
	*ip_list = NULL;

	status = get_dc_list(realm, sitename, ip_list,
			count, DC_KDC_ONLY, &ordered);

	if (!NT_STATUS_IS_OK(status)) {
		SAFE_FREE(*ip_list);
		*count = 0;
		return status;
	}

	/* only sort if we don't already have an ordered list */
	if ( !ordered ) {
		sort_service_list(*ip_list, *count);
	}

	return NT_STATUS_OK;
}
