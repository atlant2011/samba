/*
   Unix SMB/CIFS implementation.

   Generic Authentication Interface

   Copyright (C) Andrew Tridgell 2003
   Copyright (C) Andrew Bartlett <abartlet@samba.org> 2004-2006

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
#include "system/network.h"
#include <tevent.h>
#include "lib/tsocket/tsocket.h"
#include "lib/util/tevent_ntstatus.h"
#include "auth/gensec/gensec.h"

/*
  wrappers for the gensec function pointers
*/
_PUBLIC_ NTSTATUS gensec_unseal_packet(struct gensec_security *gensec_security,
			      uint8_t *data, size_t length,
			      const uint8_t *whole_pdu, size_t pdu_length,
			      const DATA_BLOB *sig)
{
	if (!gensec_security->ops->unseal_packet) {
		return NT_STATUS_NOT_IMPLEMENTED;
	}
	if (!gensec_have_feature(gensec_security, GENSEC_FEATURE_SEAL)) {
		return NT_STATUS_INVALID_PARAMETER;
	}

	return gensec_security->ops->unseal_packet(gensec_security,
						   data, length,
						   whole_pdu, pdu_length,
						   sig);
}

_PUBLIC_ NTSTATUS gensec_check_packet(struct gensec_security *gensec_security,
			     const uint8_t *data, size_t length,
			     const uint8_t *whole_pdu, size_t pdu_length,
			     const DATA_BLOB *sig)
{
	if (!gensec_security->ops->check_packet) {
		return NT_STATUS_NOT_IMPLEMENTED;
	}
	if (!gensec_have_feature(gensec_security, GENSEC_FEATURE_SIGN)) {
		return NT_STATUS_INVALID_PARAMETER;
	}

	return gensec_security->ops->check_packet(gensec_security, data, length, whole_pdu, pdu_length, sig);
}

_PUBLIC_ NTSTATUS gensec_seal_packet(struct gensec_security *gensec_security,
			    TALLOC_CTX *mem_ctx,
			    uint8_t *data, size_t length,
			    const uint8_t *whole_pdu, size_t pdu_length,
			    DATA_BLOB *sig)
{
	if (!gensec_security->ops->seal_packet) {
		return NT_STATUS_NOT_IMPLEMENTED;
	}
	if (!gensec_have_feature(gensec_security, GENSEC_FEATURE_SEAL)) {
		return NT_STATUS_INVALID_PARAMETER;
	}

	return gensec_security->ops->seal_packet(gensec_security, mem_ctx, data, length, whole_pdu, pdu_length, sig);
}

_PUBLIC_ NTSTATUS gensec_sign_packet(struct gensec_security *gensec_security,
			    TALLOC_CTX *mem_ctx,
			    const uint8_t *data, size_t length,
			    const uint8_t *whole_pdu, size_t pdu_length,
			    DATA_BLOB *sig)
{
	if (!gensec_security->ops->sign_packet) {
		return NT_STATUS_NOT_IMPLEMENTED;
	}
	if (!gensec_have_feature(gensec_security, GENSEC_FEATURE_SIGN)) {
		return NT_STATUS_INVALID_PARAMETER;
	}

	return gensec_security->ops->sign_packet(gensec_security, mem_ctx, data, length, whole_pdu, pdu_length, sig);
}

_PUBLIC_ size_t gensec_sig_size(struct gensec_security *gensec_security, size_t data_size)
{
	if (!gensec_security->ops->sig_size) {
		return 0;
	}
	if (!gensec_have_feature(gensec_security, GENSEC_FEATURE_SIGN)) {
		return 0;
	}

	return gensec_security->ops->sig_size(gensec_security, data_size);
}

size_t gensec_max_wrapped_size(struct gensec_security *gensec_security)
{
	if (!gensec_security->ops->max_wrapped_size) {
		return (1 << 17);
	}

	return gensec_security->ops->max_wrapped_size(gensec_security);
}

size_t gensec_max_input_size(struct gensec_security *gensec_security)
{
	if (!gensec_security->ops->max_input_size) {
		return (1 << 17) - gensec_sig_size(gensec_security, 1 << 17);
	}

	return gensec_security->ops->max_input_size(gensec_security);
}

_PUBLIC_ NTSTATUS gensec_wrap(struct gensec_security *gensec_security,
		     TALLOC_CTX *mem_ctx,
		     const DATA_BLOB *in,
		     DATA_BLOB *out)
{
	if (!gensec_security->ops->wrap) {
		return NT_STATUS_NOT_IMPLEMENTED;
	}
	return gensec_security->ops->wrap(gensec_security, mem_ctx, in, out);
}

_PUBLIC_ NTSTATUS gensec_unwrap(struct gensec_security *gensec_security,
		       TALLOC_CTX *mem_ctx,
		       const DATA_BLOB *in,
		       DATA_BLOB *out)
{
	if (!gensec_security->ops->unwrap) {
		return NT_STATUS_NOT_IMPLEMENTED;
	}
	return gensec_security->ops->unwrap(gensec_security, mem_ctx, in, out);
}

_PUBLIC_ NTSTATUS gensec_session_key(struct gensec_security *gensec_security,
				     TALLOC_CTX *mem_ctx,
				     DATA_BLOB *session_key)
{
	if (!gensec_security->ops->session_key) {
		return NT_STATUS_NOT_IMPLEMENTED;
	}
	if (!gensec_have_feature(gensec_security, GENSEC_FEATURE_SESSION_KEY)) {
		return NT_STATUS_NO_USER_SESSION_KEY;
	}

	return gensec_security->ops->session_key(gensec_security, mem_ctx, session_key);
}

/**
 * Return the credentials of a logged on user, including session keys
 * etc.
 *
 * Only valid after a successful authentication
 *
 * May only be called once per authentication.
 *
 */

_PUBLIC_ NTSTATUS gensec_session_info(struct gensec_security *gensec_security,
				      TALLOC_CTX *mem_ctx,
				      struct auth_session_info **session_info)
{
	if (!gensec_security->ops->session_info) {
		return NT_STATUS_NOT_IMPLEMENTED;
	}
	return gensec_security->ops->session_info(gensec_security, mem_ctx, session_info);
}

/**
 * Next state function for the GENSEC state machine
 *
 * @param gensec_security GENSEC State
 * @param out_mem_ctx The TALLOC_CTX for *out to be allocated on
 * @param in The request, as a DATA_BLOB
 * @param out The reply, as an talloc()ed DATA_BLOB, on *out_mem_ctx
 * @return Error, MORE_PROCESSING_REQUIRED if a reply is sent,
 *                or NT_STATUS_OK if the user is authenticated.
 */

_PUBLIC_ NTSTATUS gensec_update(struct gensec_security *gensec_security, TALLOC_CTX *out_mem_ctx,
		       const DATA_BLOB in, DATA_BLOB *out)
{
	return gensec_security->ops->update(gensec_security, out_mem_ctx, in, out);
}

struct gensec_update_state {
	struct tevent_immediate *im;
	struct gensec_security *gensec_security;
	DATA_BLOB in;
	DATA_BLOB out;
};

static void gensec_update_async_trigger(struct tevent_context *ctx,
					struct tevent_immediate *im,
					void *private_data);
/**
 * Next state function for the GENSEC state machine async version
 *
 * @param mem_ctx The memory context for the request
 * @param ev The event context for the request
 * @param gensec_security GENSEC State
 * @param in The request, as a DATA_BLOB
 *
 * @return The request handle or NULL on no memory failure
 */

_PUBLIC_ struct tevent_req *gensec_update_send(TALLOC_CTX *mem_ctx,
					       struct tevent_context *ev,
					       struct gensec_security *gensec_security,
					       const DATA_BLOB in)
{
	struct tevent_req *req;
	struct gensec_update_state *state = NULL;

	req = tevent_req_create(mem_ctx, &state,
				struct gensec_update_state);
	if (req == NULL) {
		return NULL;
	}

	state->gensec_security		= gensec_security;
	state->in			= in;
	state->out			= data_blob(NULL, 0);
	state->im			= tevent_create_immediate(state);
	if (tevent_req_nomem(state->im, req)) {
		return tevent_req_post(req, ev);
	}

	tevent_schedule_immediate(state->im, ev,
				  gensec_update_async_trigger,
				  req);

	return req;
}

static void gensec_update_async_trigger(struct tevent_context *ctx,
					struct tevent_immediate *im,
					void *private_data)
{
	struct tevent_req *req =
		talloc_get_type_abort(private_data, struct tevent_req);
	struct gensec_update_state *state =
		tevent_req_data(req, struct gensec_update_state);
	NTSTATUS status;

	status = gensec_update(state->gensec_security, state,
			       state->in, &state->out);
	if (tevent_req_nterror(req, status)) {
		return;
	}

	tevent_req_done(req);
}

/**
 * Next state function for the GENSEC state machine
 *
 * @param req request state
 * @param out_mem_ctx The TALLOC_CTX for *out to be allocated on
 * @param out The reply, as an talloc()ed DATA_BLOB, on *out_mem_ctx
 * @return Error, MORE_PROCESSING_REQUIRED if a reply is sent,
 *                or NT_STATUS_OK if the user is authenticated.
 */
_PUBLIC_ NTSTATUS gensec_update_recv(struct tevent_req *req,
				     TALLOC_CTX *out_mem_ctx,
				     DATA_BLOB *out)
{
	struct gensec_update_state *state =
		tevent_req_data(req, struct gensec_update_state);
	NTSTATUS status;

	if (tevent_req_is_nterror(req, &status)) {
		if (!NT_STATUS_EQUAL(status, NT_STATUS_MORE_PROCESSING_REQUIRED)) {
			tevent_req_received(req);
			return status;
		}
	} else {
		status = NT_STATUS_OK;
	}

	*out = state->out;
	talloc_steal(out_mem_ctx, out->data);

	tevent_req_received(req);
	return status;
}

/**
 * Set the requirement for a certain feature on the connection
 *
 */

_PUBLIC_ void gensec_want_feature(struct gensec_security *gensec_security,
			 uint32_t feature)
{
	if (!gensec_security->ops || !gensec_security->ops->want_feature) {
		gensec_security->want_features |= feature;
		return;
	}
	gensec_security->ops->want_feature(gensec_security, feature);
}

/**
 * Check the requirement for a certain feature on the connection
 *
 */

_PUBLIC_ bool gensec_have_feature(struct gensec_security *gensec_security,
			 uint32_t feature)
{
	if (!gensec_security->ops->have_feature) {
		return false;
	}

	/* We might 'have' features that we don't 'want', because the
	 * other end demanded them, or we can't neotiate them off */
	return gensec_security->ops->have_feature(gensec_security, feature);
}

/**
 * Return the credentials structure associated with a GENSEC context
 *
 */

_PUBLIC_ struct cli_credentials *gensec_get_credentials(struct gensec_security *gensec_security)
{
	if (!gensec_security) {
		return NULL;
	}
	return gensec_security->credentials;
}

/**
 * Set the target service (such as 'http' or 'host') on a GENSEC context - ensures it is talloc()ed
 *
 */

_PUBLIC_ NTSTATUS gensec_set_target_service(struct gensec_security *gensec_security, const char *service)
{
	gensec_security->target.service = talloc_strdup(gensec_security, service);
	if (!gensec_security->target.service) {
		return NT_STATUS_NO_MEMORY;
	}
	return NT_STATUS_OK;
}

_PUBLIC_ const char *gensec_get_target_service(struct gensec_security *gensec_security)
{
	if (gensec_security->target.service) {
		return gensec_security->target.service;
	}

	return "host";
}

/**
 * Set the target hostname (suitable for kerberos resolutation) on a GENSEC context - ensures it is talloc()ed
 *
 */

_PUBLIC_ NTSTATUS gensec_set_target_hostname(struct gensec_security *gensec_security, const char *hostname)
{
	gensec_security->target.hostname = talloc_strdup(gensec_security, hostname);
	if (hostname && !gensec_security->target.hostname) {
		return NT_STATUS_NO_MEMORY;
	}
	return NT_STATUS_OK;
}

_PUBLIC_ const char *gensec_get_target_hostname(struct gensec_security *gensec_security)
{
	/* We allow the target hostname to be overriden for testing purposes */
	if (gensec_security->settings->target_hostname) {
		return gensec_security->settings->target_hostname;
	}

	if (gensec_security->target.hostname) {
		return gensec_security->target.hostname;
	}

	/* We could add use the 'set sockaddr' call, and do a reverse
	 * lookup, but this would be both insecure (compromising the
	 * way kerberos works) and add DNS timeouts */
	return NULL;
}

/**
 * Set (and talloc_reference) local and peer socket addresses onto a socket
 * context on the GENSEC context.
 *
 * This is so that kerberos can include these addresses in
 * cryptographic tokens, to avoid certain attacks.
 */

/**
 * @brief Set the local gensec address.
 *
 * @param  gensec_security   The gensec security context to use.
 *
 * @param  remote       The local address to set.
 *
 * @return              On success NT_STATUS_OK is returned or an NT_STATUS
 *                      error.
 */
_PUBLIC_ NTSTATUS gensec_set_local_address(struct gensec_security *gensec_security,
		const struct tsocket_address *local)
{
	TALLOC_FREE(gensec_security->local_addr);

	if (local == NULL) {
		return NT_STATUS_OK;
	}

	gensec_security->local_addr = tsocket_address_copy(local, gensec_security);
	if (gensec_security->local_addr == NULL) {
		return NT_STATUS_NO_MEMORY;
	}

	return NT_STATUS_OK;
}

/**
 * @brief Set the remote gensec address.
 *
 * @param  gensec_security   The gensec security context to use.
 *
 * @param  remote       The remote address to set.
 *
 * @return              On success NT_STATUS_OK is returned or an NT_STATUS
 *                      error.
 */
_PUBLIC_ NTSTATUS gensec_set_remote_address(struct gensec_security *gensec_security,
		const struct tsocket_address *remote)
{
	TALLOC_FREE(gensec_security->remote_addr);

	if (remote == NULL) {
		return NT_STATUS_OK;
	}

	gensec_security->remote_addr = tsocket_address_copy(remote, gensec_security);
	if (gensec_security->remote_addr == NULL) {
		return NT_STATUS_NO_MEMORY;
	}

	return NT_STATUS_OK;
}

/**
 * @brief Get the local address from a gensec security context.
 *
 * @param  gensec_security   The security context to get the address from.
 *
 * @return              The address as tsocket_address which could be NULL if
 *                      no address is set.
 */
_PUBLIC_ const struct tsocket_address *gensec_get_local_address(struct gensec_security *gensec_security)
{
	if (gensec_security == NULL) {
		return NULL;
	}
	return gensec_security->local_addr;
}

/**
 * @brief Get the remote address from a gensec security context.
 *
 * @param  gensec_security   The security context to get the address from.
 *
 * @return              The address as tsocket_address which could be NULL if
 *                      no address is set.
 */
_PUBLIC_ const struct tsocket_address *gensec_get_remote_address(struct gensec_security *gensec_security)
{
	if (gensec_security == NULL) {
		return NULL;
	}
	return gensec_security->remote_addr;
}

/**
 * Set the target principal (assuming it it known, say from the SPNEGO reply)
 *  - ensures it is talloc()ed
 *
 */

_PUBLIC_ NTSTATUS gensec_set_target_principal(struct gensec_security *gensec_security, const char *principal)
{
	gensec_security->target.principal = talloc_strdup(gensec_security, principal);
	if (!gensec_security->target.principal) {
		return NT_STATUS_NO_MEMORY;
	}
	return NT_STATUS_OK;
}

const char *gensec_get_target_principal(struct gensec_security *gensec_security)
{
	if (gensec_security->target.principal) {
		return gensec_security->target.principal;
	}

	return NULL;
}
