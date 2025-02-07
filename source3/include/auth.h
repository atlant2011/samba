#ifndef _SMBAUTH_H_
#define _SMBAUTH_H_
/* 
   Unix SMB/CIFS implementation.
   Standardised Authentication types
   Copyright (C) Andrew Bartlett 2001

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

#include "../auth/common_auth.h"

struct gensec_security;

struct extra_auth_info {
	struct dom_sid user_sid;
	struct dom_sid pgid_sid;
};

struct auth_serversupplied_info {
	bool guest;
	bool system;

	struct security_unix_token utok;

	/* NT group information taken from the info3 structure */

	struct security_token *security_token;

	/* These are the intermediate session keys, as provided by a
	 * NETLOGON server and used by NTLMSSP to negotiate key
	 * exchange etc (which will provide the session_key in the
	 * auth_session_info).  It is usually the same as the keys in
	 * the info3, but is a variable length structure here to allow
	 * it to be omitted if the auth module does not know it.
	 */

	DATA_BLOB session_key;
	DATA_BLOB lm_session_key;

	struct netr_SamInfo3 *info3;

	/* this structure is filled *only* in pathological cases where the user
	 * sid or the primary group sid are not sids of the domain. Normally
	 * this happens only for unix accounts that have unix domain sids.
	 * This is checked only when info3.rid and/or info3.primary_gid are set
	 * to the special invalid value of 0xFFFFFFFF */
	struct extra_auth_info extra;

	/*
	 * This is a token from /etc/passwd and /etc/group
	 */
	bool nss_token;

	char *unix_name;
};

struct auth_context {
	DATA_BLOB challenge; 

	/* Who set this up in the first place? */ 
	const char *challenge_set_by; 

	bool challenge_may_be_modified;

	struct auth_methods *challenge_set_method; 
	/* What order are the various methods in?   Try to stop it changing under us */ 
	struct auth_methods *auth_method_list;	

	NTSTATUS (*get_ntlm_challenge)(struct auth_context *auth_context,
				       uint8_t chal[8]);
	NTSTATUS (*check_ntlm_password)(const struct auth_context *auth_context,
					const struct auth_usersupplied_info *user_info, 
					struct auth_serversupplied_info **server_info);
	NTSTATUS (*nt_status_squash)(NTSTATUS nt_status);

	NTSTATUS (*prepare_gensec)(TALLOC_CTX *mem_ctx,
				 struct gensec_security **gensec_context);
	NTSTATUS (*gensec_start_mech_by_oid)(struct gensec_security *gensec_context, const char *oid_string);
	NTSTATUS (*gensec_start_mech_by_authtype)(struct gensec_security *gensec_context, uint8_t auth_type, uint8_t auth_level);
};

typedef struct auth_methods
{
	struct auth_methods *prev, *next;
	const char *name; /* What name got this module */

	NTSTATUS (*auth)(const struct auth_context *auth_context,
			 void *my_private_data, 
			 TALLOC_CTX *mem_ctx,
			 const struct auth_usersupplied_info *user_info, 
			 struct auth_serversupplied_info **server_info);

	/* If you are using this interface, then you are probably
	 * getting something wrong.  This interface is only for
	 * security=server, and makes a number of compromises to allow
	 * that.  It is not compatible with being a PDC.  */
	DATA_BLOB (*get_chal)(const struct auth_context *auth_context,
			      void **my_private_data, 
			      TALLOC_CTX *mem_ctx);

	/* Optional methods allowing this module to provide a way to get a gensec context */
	NTSTATUS (*prepare_gensec)(TALLOC_CTX *mem_ctx,
				 struct gensec_security **gensec_context);
	NTSTATUS (*gensec_start_mech_by_oid)(struct gensec_security *gensec_context, const char *oid_string);
	NTSTATUS (*gensec_start_mech_by_authtype)(struct gensec_security *gensec_context, uint8_t auth_type, uint8_t auth_level);
	/* Used to keep tabs on things like the cli for SMB server authentication */
	void *private_data;

} auth_methods;

typedef NTSTATUS (*auth_init_function)(struct auth_context *, const char *, struct auth_methods **);

struct auth_init_function_entry {
	const char *name;
	/* Function to create a member of the authmethods list */

	auth_init_function init;

	struct auth_init_function_entry *prev, *next;
};

struct auth_ntlmssp_state;

/* Changed from 1 -> 2 to add the logon_parameters field. */
/* Changed from 2 -> 3 when we reworked many auth structures to use IDL or be in common with Samba4 */
#define AUTH_INTERFACE_VERSION 3

#include "auth/proto.h"

#endif /* _SMBAUTH_H_ */
