/*
 *   This program is is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or (at
 *   your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

/**
 * $Id$
 * @file ldap.c
 * @brief LDAP module library functions.
 *
 * @author Arran Cudbard-Bell <a.cudbardb@freeradius.org>
 * @copyright 2015 Arran Cudbard-Bell <a.cudbardb@freeradius.org>
 * @copyright 2013-2015 Network RADIUS SARL <info@networkradius.com>
 * @copyright 2013-2015 The FreeRADIUS Server Project.
 */

#include <freeradius-devel/rad_assert.h>

#include <stdarg.h>
#include <ctype.h>

#define LOG_PREFIX "rlm_ldap (%s) - "
#define LOG_PREFIX_ARGS inst->name

#include <freeradius-devel/radiusd.h>
#include <freeradius-devel/modules.h>
#include "rlm_ldap.h"

static const char specials[] = ",+\"\\<>;*=()";
static const char hextab[] = "0123456789abcdef";

FR_NAME_NUMBER const ldap_supported_extensions[] = {
	{ "bindname",	LDAP_DEREF_NEVER	},
	{ "x-bindpw",	LDAP_DEREF_SEARCHING	},

	{  NULL , -1 }
};

/** Prints information to the debug log on the current timeout settings
 *
 * There are so many different timers in LDAP it's often hard to debug
 * issues with them, hence the need for this function.
 */
void rlm_ldap_timeout_debug(rlm_ldap_t const *inst, REQUEST *request, ldap_handle_t const *conn,
			    struct timeval const *timeout, char const *prefix)
{
	struct timeval 	*net = NULL, *client = NULL;
	int		server = 0;

#ifdef LDAP_OPT_NETWORK_TIMEOUT
	if (ldap_get_option(conn->handle, LDAP_OPT_NETWORK_TIMEOUT, &net) != LDAP_OPT_SUCCESS) {
		ROPTIONAL(REDEBUG, ERROR, "Failed getting LDAP_OPT_NETWORK_TIMEOUT");
	}
#endif

#ifdef LDAP_OPT_TIMEOUT
	if (ldap_get_option(conn->handle, LDAP_OPT_TIMEOUT, &client) != LDAP_OPT_SUCCESS) {
		ROPTIONAL(REDEBUG, ERROR, "Failed getting LDAP_OPT_TIMEOUT");
	}
#endif

	if (ldap_get_option(conn->handle, LDAP_OPT_TIMELIMIT, &server) != LDAP_OPT_SUCCESS) {
		ROPTIONAL(REDEBUG, ERROR, "Failed getting LDAP_OPT_TIMELIMIT");
	}

	ROPTIONAL(RDEBUG4, DEBUG4, "%s: Timeout settings", prefix);

	if (timeout) {
		ROPTIONAL(RDEBUG4, DEBUG4, "Client side result timeout (ovr): %ld.%06ld",
			  (long)timeout->tv_sec, (long)timeout->tv_usec);
	} else {
		ROPTIONAL(RDEBUG4, DEBUG4, "Client side result timeout (ovr): unset");
	}

#ifdef LDAP_OPT_TIMEOUT
	if (client && (client->tv_sec != -1)) {
		ROPTIONAL(RDEBUG4, DEBUG4, "Client side result timeout (dfl): %ld.%06ld",
			  (long)client->tv_sec, (long)client->tv_usec);

	} else {
		ROPTIONAL(RDEBUG4, DEBUG4, "Client side result timeout (dfl): unset");
	}
#endif

#ifdef LDAP_OPT_NETWORK_TIMEOUT
	if (net && (net->tv_sec != -1)) {
		ROPTIONAL(RDEBUG4, DEBUG4, "Client side network I/O timeout : %ld.%06ld",
			  (long)net->tv_sec, (long)net->tv_usec);
	} else {
		ROPTIONAL(RDEBUG4, DEBUG4, "Client side network I/O timeout : unset");

	}
#endif
	ROPTIONAL(RDEBUG4, DEBUG4, "Server side result timeout      : %i", server);
	if (request) REXDENT();

	free(net);
	free(client);
}

/** Converts "bad" strings into ones which are safe for LDAP
 *
 * @note RFC 4515 says filter strings can only use the @verbatim \<hex><hex> @endverbatim
 *	format, whereas RFC 4514 indicates that some chars in DNs, may be escaped simply
 *	with a backslash. For simplicity, we always use the hex escape sequences.
 *	In other areas where we're doing DN comparison, the DNs need to be normalised first
 *	so that they both use only hex escape sequences.
 *
 * @note This is a callback for xlat operations.
 *
 * Will escape any characters in input strings that would cause the string to be interpreted
 * as part of a DN and or filter. Escape sequence is @verbatim \<hex><hex> @endverbatim.
 *
 * @param request The current request.
 * @param out Pointer to output buffer.
 * @param outlen Size of the output buffer.
 * @param in Raw unescaped string.
 * @param arg Any additional arguments (unused).
 */
size_t rlm_ldap_escape_func(UNUSED REQUEST *request, char *out, size_t outlen, char const *in, UNUSED void *arg)
{

	size_t left = outlen;

	if (*in && ((*in == ' ') || (*in == '#'))) goto encode;

	while (*in) {
		/*
		 *	Encode unsafe characters.
		 */
		if (memchr(specials, *in, sizeof(specials) - 1)) {
		encode:
			/*
			 *	Only 3 or less bytes available.
			 */
			if (left <= 3) break;

			*out++ = '\\';
			*out++ = hextab[(*in >> 4) & 0x0f];
			*out++ = hextab[*in & 0x0f];
			in++;
			left -= 3;

			continue;
		}

		if (left <= 1) break;

		/*
		 *	Doesn't need encoding
		 */
		*out++ = *in++;
		left--;
	}

	*out = '\0';

	return outlen - left;
}

/** Converts escaped DNs and filter strings into normal
 *
 * @note RFC 4515 says filter strings can only use the @verbatim \<hex><hex> @endverbatim
 *	format, whereas RFC 4514 indicates that some chars in DNs, may be escaped simply
 *	with a backslash..
 *
 * Will unescape any special characters in strings, or @verbatim \<hex><hex> @endverbatim
 * sequences.
 *
 * @param request The current request.
 * @param out Pointer to output buffer.
 * @param outlen Size of the output buffer.
 * @param in Escaped string string.
 * @param arg Any additional arguments (unused).
 */
size_t rlm_ldap_unescape_func(UNUSED REQUEST *request, char *out, size_t outlen, char const *in, UNUSED void *arg)
{
	char const *p;
	char *c1, *c2, c3;
	size_t	freespace = outlen;

	if (outlen <= 1) return 0;

	p = in;
	while (*p && (--freespace > 0)) {
		if (*p != '\\') {
		next:
			*out++ = *p++;
			continue;
		}

		p++;

		/* It's an escaped special, just remove the slash */
		if (memchr(specials, *in, sizeof(specials) - 1)) {
			*out++ = *p++;
			continue;
		}

		/* Is a hex sequence */
		if (!(c1 = memchr(hextab, tolower(p[0]), 16)) ||
		    !(c2 = memchr(hextab, tolower(p[1]), 16))) goto next;
		c3 = ((c1 - hextab) << 4) + (c2 - hextab);

		*out++ = c3;
		p += 2;
	}

	*out = '\0';

	return outlen - freespace;
}


/** Check whether a string looks like a DN
 *
 * @param[in] in Str to check.
 * @param[in] inlen Length of string to check.
 * @return
 *	- true if string looks like a DN.
 *	- false if string does not look like DN.
 */
bool rlm_ldap_is_dn(char const *in, size_t inlen)
{
	char const *p;

	char want = '=';
	bool too_soon = true;
	int comp = 1;

	for (p = in; inlen > 0; p++, inlen--) {
		if (p[0] == '\\') {
			char c;

			too_soon = false;

			/*
			 *	Invalid escape sequence, not a DN
			 */
			if (inlen < 2) return false;

			/*
			 *	Double backslash, consume two chars
			 */
			if (p[1] == '\\') {
				inlen--;
				p++;
				continue;
			}

			/*
			 *	Special, consume two chars
			 */
			switch (p[1]) {
			case ' ':
			case '#':
			case '=':
			case '"':
			case '+':
			case ',':
			case ';':
			case '<':
			case '>':
			case '\'':
				inlen -= 1;
				p += 1;
				continue;

			default:
				break;
			}

			/*
			 *	Invalid escape sequence, not a DN
			 */
			if (inlen < 3) return false;

			/*
			 *	Hex encoding, consume three chars
			 */
			if (fr_hex2bin((uint8_t *) &c, 1, p + 1, 2) == 1) {
				inlen -= 2;
				p += 2;
				continue;
			}

			/*
			 *	Invalid escape sequence, not a DN
			 */
			return false;
		}

		switch (*p) {
		case '=':
			if (too_soon || (*p != want)) return false;	/* Too soon after last , or = */
			want = ',';
			too_soon = true;
			break;

		case ',':
			if (too_soon || (*p != want)) return false;	/* Too soon after last , or = */
			want = '=';
			too_soon = true;
			comp++;
			break;

		default:
			too_soon = false;
			break;
		}
	}

	/*
	 *	If the string ended with , or =, or the number
	 *	of components was less than 2
	 *
	 *	i.e. we don't have <attr>=<val>,<attr>=<val>
	 */
	if (too_soon || (comp < 2)) return false;

	return true;
}

/** Convert a berval to a talloced string
 *
 * The ldap_get_values function is deprecated, and ldap_get_values_len
 * does not guarantee the berval buffers it returns are \0 terminated.
 *
 * For some cases this is fine, for others we require a \0 terminated
 * buffer (feeding DNs back into libldap for example).
 *
 * @param ctx to allocate in.
 * @param in Berval to copy.
 * @return \0 terminated buffer containing in->bv_val.
 */
char *rlm_ldap_berval_to_string(TALLOC_CTX *ctx, struct berval const *in)
{
	char *out;

	out = talloc_array(ctx, char, in->bv_len + 1);
	if (!out) return NULL;

	memcpy(out, in->bv_val, in->bv_len);
	out[in->bv_len] = '\0';

	return out;
}

/** Normalise escape sequences in a DN
 *
 * Characters in a DN can either be escaped as
 * @verbatim \<hex><hex> @endverbatim or @verbatim \<special> @endverbatim
 *
 * The LDAP directory chooses how characters are escaped, which can make
 * local comparisons of DNs difficult.
 *
 * Here we search for hex sequences that match special chars, and convert
 * them to the @verbatim \<special> @endverbatim form.
 *
 * @note the resulting output string will only ever be shorter than the
 *       input, so it's fine to use the same buffer for both out and in.
 *
 * @param out Where to write the normalised DN.
 * @param in The input DN.
 * @return The number of bytes written to out.
 */
size_t rlm_ldap_normalise_dn(char *out, char const *in)
{
	char const *p;
	char *o = out;

	for (p = in; *p != '\0'; p++) {
		if (p[0] == '\\') {
			char c;

			/*
			 *	Double backslashes get processed specially
			 */
			if (p[1] == '\\') {
				p += 1;
				*o++ = p[0];
				*o++ = p[1];
				continue;
			}

			/*
			 *	Hex encodings that have an alternative
			 *	special encoding, get rewritten to the
			 *	special encoding.
			 */
			if (fr_hex2bin((uint8_t *) &c, 1, p + 1, 2) == 1) {
				switch (c) {
				case ' ':
				case '#':
				case '=':
				case '"':
				case '+':
				case ',':
				case ';':
				case '<':
				case '>':
				case '\'':
					*o++ = '\\';
					*o++ = c;
					p += 2;
					continue;

				default:
					break;
				}
			}
		}
		*o++ = *p;
	}
	*o = '\0';

	return o - out;
}

/** Find the place at which the two DN strings diverge
 *
 * Returns the length of the non matching string in full.
 *
 * @param full DN.
 * @param part Partial DN as returned by ldap_parse_result.
 * @return
 *	- Length of the portion of full which wasn't matched
 *	- -1 on failure.
 */
static size_t rlm_ldap_common_dn(char const *full, char const *part)
{
	size_t f_len, p_len, i;

	if (!full) return -1;

	f_len = strlen(full);

	if (!part) return -1;

	p_len = strlen(part);
	if (!p_len) return f_len;

	if ((f_len < p_len) || !f_len) return -1;

	for (i = 0; i < p_len; i++) if (part[p_len - i] != full[f_len - i]) return -1;

	return f_len - p_len;
}

/** Combine and expand filters
 *
 * @param request Current request.
 * @param out Where to write the expanded string.
 * @param outlen Length of output buffer.
 * @param sub Array of subfilters (may contain NULLs).
 * @param sublen Number of potential subfilters in array.
 * @return length of expanded data.
 */
ssize_t rlm_ldap_xlat_filter(REQUEST *request, char const **sub, size_t sublen, char *out, size_t outlen)
{
	char buffer[LDAP_MAX_FILTER_STR_LEN + 1];
	char const *in = NULL;
	char *p = buffer;

	ssize_t len = 0;

	unsigned int i;
	int cnt = 0;

	/*
	 *	Figure out how many filter elements we need to integrate
	 */
	for (i = 0; i < sublen; i++) {
		if (sub[i] && *sub[i]) {
			in = sub[i];
			cnt++;
		}
	}

	if (!cnt) {
		out[0] = '\0';
		return 0;
	}

	if (cnt > 1) {
		if (outlen < 3) {
			goto oob;
		}

		p[len++] = '(';
		p[len++] = '&';

		for (i = 0; i < sublen; i++) {
			if (sub[i] && (*sub[i] != '\0')) {
				len += strlcpy(p + len, sub[i], outlen - len);

				if ((size_t) len >= outlen) {
					oob:
					REDEBUG("Out of buffer space creating filter");

					return -1;
				}
			}
		}

		if ((outlen - len) < 2) {
			goto oob;
		}

		p[len++] = ')';
		p[len] = '\0';

		in = buffer;
	}

	len = xlat_eval(out, outlen, request, in, rlm_ldap_escape_func, NULL);
	if (len < 0) {
		REDEBUG("Failed creating filter");

		return -1;
	}

	return len;
}

/** Return the error string associated with a handle
 *
 * @param conn to retrieve error from.
 * @return error string.
 */
char const *rlm_ldap_error_str(ldap_handle_t const *conn)
{
	int lib_errno;
	ldap_get_option(conn->handle, LDAP_OPT_ERROR_NUMBER, &lib_errno);
	if (lib_errno == LDAP_SUCCESS) {
		return "unknown";
	}

	return ldap_err2string(lib_errno);
}

/** Parse response from LDAP server dealing with any errors
 *
 * Should be called after an LDAP operation. Will check result of operation
 * and if it was successful, then attempt to retrieve and parse the result.
 *
 * Will also produce extended error output including any messages the server
 * sent, and information about partial DN matches.
 *
 * @param[in] inst	of LDAP module.
 * @param[in] conn	Current connection.
 * @param[in] msgid	returned from last operation. May be -1 if no result
 *			processing is required.
 * @param[in] dn	Last search or bind DN.
 * @param[in] timeout	Override the default result timeout.
 * @param[out] result	Where to write result, if NULL result will be freed.
 * @param[out] error	Where to write the error string, may be NULL, must
 *			not be freed.
 * @param[out] extra	Where to write additional error string to, may be NULL
 *			(faster) or must be freed (with talloc_free).
 * @return One of the LDAP_PROC_* (#ldap_rcode_t) values.
 */
ldap_rcode_t rlm_ldap_result(rlm_ldap_t const *inst,
			     ldap_handle_t const *conn,
			     int msgid,
			     char const *dn,
			     struct timeval const *timeout,
			     LDAPMessage **result,
			     char const **error, char **extra)
{
	ldap_rcode_t status = LDAP_PROC_SUCCESS;

	int lib_errno = LDAP_SUCCESS;	// errno returned by the library.
	int srv_errno = LDAP_SUCCESS;	// errno in the result message.

	char *part_dn = NULL;		// Partial DN match.
	char *our_err = NULL;		// Our extended error message.
	char *srv_err = NULL;		// Server's extended error message.
	char *p, *a;

	bool freeit = false;		// Whether the message should be freed after being processed.
	int len;

	struct timeval tv;		// Holds timeout values.

	LDAPMessage *tmp_msg = NULL;	// Temporary message pointer storage if we weren't provided with one.

	char const *tmp_err;		// Temporary error pointer storage if we weren't provided with one.

	if (!error) error = &tmp_err;
	*error = NULL;

	if (extra) *extra = NULL;
	if (result) *result = NULL;

	/*
	 *	We always need the result, but our caller may not
	 */
	if (!result) {
		result = &tmp_msg;
		freeit = true;
	}

	/*
	 *	Check if there was an error sending the request
	 */
	ldap_get_option(conn->handle, LDAP_OPT_ERROR_NUMBER, &lib_errno);
	if (lib_errno != LDAP_SUCCESS) goto process_error;
	if (msgid < 0) return LDAP_SUCCESS;	/* No msgid and no error, return now */

	if (!timeout) {
		tv.tv_sec = inst->res_timeout;
		tv.tv_usec = 0;
	} else {
		tv = *timeout;
	}

	/*
	 *	Now retrieve the result and check for errors
	 *	ldap_result returns -1 on failure, and 0 on timeout
	 */
	lib_errno = ldap_result(conn->handle, msgid, 1, &tv, result);
	if (lib_errno == 0) {
		lib_errno = LDAP_TIMEOUT;

		goto process_error;
	}

	if (lib_errno == -1) {
		ldap_get_option(conn->handle, LDAP_OPT_ERROR_NUMBER, &lib_errno);

		goto process_error;
	}

	/*
	 *	Parse the result and check for errors sent by the server
	 */
	lib_errno = ldap_parse_result(conn->handle, *result,
				      &srv_errno,
				      extra ? &part_dn : NULL,
				      extra ? &srv_err : NULL,
				      NULL, NULL, freeit);
	if (freeit) *result = NULL;

	if (lib_errno != LDAP_SUCCESS) {
		ldap_get_option(conn->handle, LDAP_OPT_ERROR_NUMBER, &lib_errno);
		goto process_error;
	}

process_error:
	if ((lib_errno == LDAP_SUCCESS) && (srv_errno != LDAP_SUCCESS)) {
		lib_errno = srv_errno;
	} else if ((lib_errno != LDAP_SUCCESS) && (srv_errno == LDAP_SUCCESS)) {
		srv_errno = lib_errno;
	}

	switch (lib_errno) {
	case LDAP_SUCCESS:
		*error = "Success";
		break;

	case LDAP_SASL_BIND_IN_PROGRESS:
		*error = "Continuing";
		status = LDAP_PROC_CONTINUE;
		break;

	case LDAP_NO_SUCH_OBJECT:
		*error = "The specified DN wasn't found";
		status = LDAP_PROC_BAD_DN;

		if (!extra) break;

		/*
		 *	Build our own internal diagnostic string
		 */
		len = rlm_ldap_common_dn(dn, part_dn);
		if (len < 0) break;

		our_err = talloc_typed_asprintf(conn, "Match stopped here: [%.*s]%s", len, dn, part_dn ? part_dn : "");
		goto error_string;

	case LDAP_INSUFFICIENT_ACCESS:
		*error = "Insufficient access. Check the identity and password configuration directives";
		status = LDAP_PROC_NOT_PERMITTED;
		break;

	case LDAP_UNWILLING_TO_PERFORM:
		*error = "Server was unwilling to perform";
		status = LDAP_PROC_NOT_PERMITTED;
		break;

	case LDAP_FILTER_ERROR:
		*error = "Bad search filter";
		status = LDAP_PROC_ERROR;
		break;

	case LDAP_TIMEOUT:
		*error = "Timed out while waiting for server to respond";
		goto timeout;

	case LDAP_TIMELIMIT_EXCEEDED:
		*error = "Time limit exceeded";
	timeout:
		trigger_exec(NULL, inst->cs, "modules.ldap.timeout", true, NULL);
		/* FALL-THROUGH */

	case LDAP_BUSY:
	case LDAP_UNAVAILABLE:
	case LDAP_SERVER_DOWN:
		status = LDAP_PROC_RETRY;
		goto error_string;

	case LDAP_INVALID_CREDENTIALS:
	case LDAP_CONSTRAINT_VIOLATION:
		status = LDAP_PROC_REJECT;
		goto error_string;

	case LDAP_OPERATIONS_ERROR:
		*error = "Please set 'chase_referrals=yes' and 'rebind=yes'. See the ldap module configuration "
			 "for details.";

		/* FALL-THROUGH */
	default:
		status = LDAP_PROC_ERROR;

	error_string:
		if (!*error) *error = ldap_err2string(lib_errno);

		if (!extra || ((lib_errno == srv_errno) && !our_err && !srv_err)) break;

		/*
		 *	Output the error codes from the library and server
		 */
		p = talloc_zero_array(conn, char, 1);
		if (!p) break;

		if (lib_errno != srv_errno) {
			a = talloc_asprintf_append(p, "LDAP lib error: %s (%u), srv error: %s (%u). ",
						   ldap_err2string(lib_errno), lib_errno,
						   ldap_err2string(srv_errno), srv_errno);
			if (!a) {
				talloc_free(p);
				break;
			}

			p = a;
		}

		if (our_err) {
			a = talloc_asprintf_append_buffer(p, "%s. ", our_err);
			if (!a) {
				talloc_free(p);
				break;
			}

			p = a;
		}

		if (srv_err) {
			a = talloc_asprintf_append_buffer(p, "Server said: %s. ", srv_err);
			if (!a) {
				talloc_free(p);
				break;
			}

			p = a;
		}

		*extra = p;

		break;
	}

	/*
	 *	Cleanup memory
	 */
	if (srv_err) ldap_memfree(srv_err);
	if (part_dn) ldap_memfree(part_dn);

	talloc_free(our_err);

	if ((status < 0) && *result) {
		ldap_msgfree(*result);
		*result = NULL;
	}

	return status;
}

/** Bind to the LDAP directory as a user
 *
 * Performs a simple bind to the LDAP directory, and handles any errors that occur.
 *
 * @param[in] inst		rlm_ldap configuration.
 * @param[in] request		Current request, this may be NULL, in which case all
 *				debug logging is done with radlog.
 * @param[in,out] pconn		to use. May change as this function calls functions
 *				which auto re-connect.
 * @param[in] dn		of the user, may be NULL to bind anonymously.
 * @param[in] password		of the user, may be NULL if no password is specified.
 * @param[in] sasl		mechanism to use for bind, and additional parameters.
 * @param[in] retry		whether we're rebinding an existing connection.
 * @param[in] timeout		Maximum time bind is allowed to take.
 * @param[in] serverctrls	Only used for SASL binds.  May be NULL.
 * @param[in] clientctrls	Search controls for sasl_bind.
 *				Only used for SASL binds. May be NULL.
 * @return One of the LDAP_PROC_* (#ldap_rcode_t) values.
 */
ldap_rcode_t rlm_ldap_bind(rlm_ldap_t const *inst,
			   REQUEST *request,
			   ldap_handle_t **pconn,
			   char const *dn, char const *password,
#ifdef WITH_SASL
			   ldap_sasl const *sasl,
#else
			   NDEBUG_UNUSED ldap_sasl const *sasl,
#endif
			   bool retry,
			   struct timeval const *timeout,
			   LDAPControl **serverctrls, LDAPControl **clientctrls)
{
	ldap_rcode_t		status = LDAP_PROC_ERROR;
	ldap_pool_inst_t const	*pool_inst = (*pconn)->pool_inst;

	int			msgid = -1;

	char const		*error = NULL;
	char 			*extra = NULL;

	int 			i, num;
	int			conn_available;

	rad_assert(*pconn && (*pconn)->handle);
	rad_assert(!retry || inst->pool);

#ifndef WITH_SASL
	rad_assert(!sasl || !sasl->mech);
#endif

	if (DEBUG_ENABLED4 || (request && RDEBUG_ENABLED4)) {
		rlm_ldap_timeout_debug(inst, request, *pconn, timeout, __FUNCTION__);
	}

	/*
	 *	Bind as anonymous user
	 */
	if (!dn) dn = "";

	/*
	 *	Pool isn't available during module instantiation
	 */
	conn_available = inst->pool ? fr_connection_pool_state(inst->pool)->num : 0;

	/*
	 *	For sanity, for when no connections are viable,
	 *	and we can't make a new one.
	 */
	num = retry ? conn_available : 0;
	for (i = num; i >= 0; i--) {
#ifdef WITH_SASL
		if (sasl && sasl->mech) {
			status = rlm_ldap_sasl_interactive(inst, request, *pconn, dn, password, sasl,
							   serverctrls, clientctrls, timeout, &error, &extra);
		} else
#endif
		{
			int ret;
			struct berval cred;

			if (password) {
				memcpy(&cred.bv_val, &password, sizeof(cred.bv_val));
				cred.bv_len = talloc_array_length(password) - 1;
			} else {
				cred.bv_val = NULL;
				cred.bv_len = 0;
			}

			/*
			 *	Yes, confusingly named.  This is the simple version
			 *	of the SASL bind function that should always be
			 *	available.
			 */
			ret = ldap_sasl_bind((*pconn)->handle, dn, LDAP_SASL_SIMPLE, &cred,
					     serverctrls, clientctrls, &msgid);

			/* We got a valid message ID */
			if ((ret == 0) && (msgid >= 0)) ROPTIONAL(RDEBUG2, DEBUG2, "Waiting for bind result...");

			status = rlm_ldap_result(inst, *pconn, msgid, dn, timeout, NULL, &error, &extra);
		}

		switch (status) {
		case LDAP_PROC_SUCCESS:
			ROPTIONAL(RDEBUG, DEBUG, "Bind successful");
			break;

		case LDAP_PROC_NOT_PERMITTED:
			ROPTIONAL(REDEBUG, ERROR, "Bind was not permitted: %s", error);
			LDAP_EXTRA_DEBUG();

			break;

		case LDAP_PROC_REJECT:
			ROPTIONAL(REDEBUG, ERROR, "Bind credentials incorrect: %s", error);
			LDAP_EXTRA_DEBUG();

			break;

		case LDAP_PROC_RETRY:
			if (retry) {
				*pconn = fr_connection_reconnect(inst->pool, request, *pconn);
				if (*pconn) {
					ROPTIONAL(RWDEBUG, WARN, "Bind with %s to %s failed: %s. Got new socket, "
						  "retrying...", *dn ? dn : "(anonymous)",
						  pool_inst->server, error);

					talloc_free(extra); /* don't leak debug info */

					continue;
				}
			};
			status = LDAP_PROC_ERROR;

			/*
			 *	Were not allowed to retry, or there are no more
			 *	sockets, treat this as a hard failure.
			 */
			/* FALL-THROUGH */
		default:
			ROPTIONAL(REDEBUG, ERROR, "Bind with %s to %s failed: %s", *dn ? dn : "(anonymous)",
				  pool_inst->server, error);
			LDAP_EXTRA_DEBUG();

			break;
		}

		break;
	}

	if (retry && (i < 0)) {
		ROPTIONAL(REDEBUG, ERROR, "Hit reconnection limit");
		status = LDAP_PROC_ERROR;
	}

	talloc_free(extra);

	return status; /* caller closes the connection */
}

/** Search for something in the LDAP directory
 *
 * Binds as the administrative user and performs a search, dealing with any errors.
 *
 * @param[out] result Where to store the result. Must be freed with ldap_msgfree if LDAP_PROC_SUCCESS is returned.
 *	May be NULL in which case result will be automatically freed after use.
 * @param[in] inst rlm_ldap configuration.
 * @param[in] request Current request.
 * @param[in,out] pconn to use. May change as this function calls functions which auto re-connect.
 * @param[in] dn to use as base for the search.
 * @param[in] scope to use (LDAP_SCOPE_BASE, LDAP_SCOPE_ONE, LDAP_SCOPE_SUB).
 * @param[in] filter to use, should be pre-escaped.
 * @param[in] attrs to retrieve.
 * @param[in] serverctrls Search controls to pass to the server.  May be NULL.
 * @param[in] clientctrls Search controls for ldap_search.  May be NULL.
 * @return One of the LDAP_PROC_* (#ldap_rcode_t) values.
 */
ldap_rcode_t rlm_ldap_search(LDAPMessage **result, rlm_ldap_t const *inst, REQUEST *request,
			     ldap_handle_t **pconn,
			     char const *dn, int scope, char const *filter, char const * const *attrs,
			     LDAPControl **serverctrls, LDAPControl **clientctrls)
{
	ldap_rcode_t	status = LDAP_PROC_ERROR;
	LDAPMessage	*our_result = NULL;

	int		msgid;		// Message id returned by
					// ldap_search_ext.

	int		count = 0;	// Number of results we got.

	struct timeval	tv;		// Holds timeout values.

	char const 	*error = NULL;
	char		*extra = NULL;

	int 		i;

	int		conn_available;

	LDAPControl	*our_serverctrls[LDAP_MAX_CONTROLS];
	LDAPControl	*our_clientctrls[LDAP_MAX_CONTROLS];

	rlm_ldap_control_merge(our_serverctrls, our_clientctrls,
			       sizeof(our_serverctrls) / sizeof(*our_serverctrls),
			       sizeof(our_clientctrls) / sizeof(*our_clientctrls),
			       *pconn, serverctrls, clientctrls);

	rad_assert(*pconn && (*pconn)->handle);

	if (DEBUG_ENABLED4 || (request && RDEBUG_ENABLED4)) {
		rlm_ldap_timeout_debug(inst, request, *pconn, NULL, __FUNCTION__);
	}

	/*
	 *	OpenLDAP library doesn't declare attrs array as const, but
	 *	it really should be *sigh*.
	 */
	char **search_attrs;
	memcpy(&search_attrs, &attrs, sizeof(attrs));

	/*
	 *	Do all searches as the admin user.
	 */
	if ((*pconn)->rebound) {
		status = rlm_ldap_bind(inst, request, pconn, (*pconn)->pool_inst->admin_identity,
				       (*pconn)->pool_inst->admin_password, &(*pconn)->pool_inst->admin_sasl, true,
				       NULL, NULL, NULL);
		if (status != LDAP_PROC_SUCCESS) {
			return LDAP_PROC_ERROR;
		}

		rad_assert(*pconn);

		(*pconn)->rebound = false;
	}

	if (filter) {
		ROPTIONAL(RDEBUG, DEBUG, "Performing search in \"%s\" with filter \"%s\", scope \"%s\"", dn, filter,
			  fr_int2str(ldap_scope, scope, "<INVALID>"));
	} else {
		ROPTIONAL(RDEBUG, DEBUG, "Performing unfiltered search in \"%s\", scope \"%s\"", dn,
			  fr_int2str(ldap_scope, scope, "<INVALID>"));
	}
	/*
	 *	If LDAP search produced an error it should also be logged
	 *	to the ld. result should pick it up without us
	 *	having to pass it explicitly.
	 */
	memset(&tv, 0, sizeof(tv));
	tv.tv_sec = inst->res_timeout;

	/*
	 *	Pool isn't available during module instantiation
	 */
	conn_available = inst->pool ? fr_connection_pool_state(inst->pool)->num : 0;

	/*
	 *	For sanity, for when no connections are viable,
	 *	and we can't make a new one.
	 */
	for (i = conn_available; i >= 0; i--) {
		(void) ldap_search_ext((*pconn)->handle, dn, scope, filter, search_attrs,
				       0, our_serverctrls, our_clientctrls, &tv, 0, &msgid);

		ROPTIONAL(RDEBUG, DEBUG, "Waiting for search result...");
		status = rlm_ldap_result(inst, *pconn, msgid, dn, NULL, &our_result, &error, &extra);
		switch (status) {
		case LDAP_PROC_SUCCESS:
			break;

		/*
		 *	Invalid DN isn't a failure when searching.
		 *	The DN may be xlat expanded so may point directly
		 *	to an LDAP object. If that can't be located, it's
		 *	the same as notfound.
		 */
		case LDAP_PROC_BAD_DN:
			ROPTIONAL(RDEBUG, DEBUG, "%s", error);
			if (extra) ROPTIONAL(RDEBUG, DEBUG, "%s", extra);
			break;

		case LDAP_PROC_RETRY:
			*pconn = fr_connection_reconnect(inst->pool, request, *pconn);
			if (*pconn) {
				ROPTIONAL(RWDEBUG, WARN, "Search failed: %s. Got new socket, retrying...", error);

				talloc_free(extra); /* don't leak debug info */

				continue;
			}

			status = LDAP_PROC_ERROR;

			/* FALL-THROUGH */
		default:
			ROPTIONAL(REDEBUG, ERROR, "Failed performing search: %s", error);
			if (extra) ROPTIONAL(REDEBUG, ERROR, "%s", extra);

			goto finish;
		}

		break;
	}

	if (i < 0) {
		ROPTIONAL(REDEBUG, ERROR, "Hit reconnection limit");
		status = LDAP_PROC_ERROR;

		goto finish;
	}

	count = ldap_count_entries((*pconn)->handle, our_result);
	if (count < 0) {
		ROPTIONAL(REDEBUG, ERROR, "Error counting results: %s", rlm_ldap_error_str(*pconn));
		status = LDAP_PROC_ERROR;

		ldap_msgfree(our_result);
		our_result = NULL;
	} else if (count == 0) {
		ROPTIONAL(RDEBUG, DEBUG, "Search returned no results");
		status = LDAP_PROC_NO_RESULT;

		ldap_msgfree(our_result);
		our_result = NULL;
	}

finish:
	talloc_free(extra);

	/*
	 *	We always need to get the result to count entries, but the caller
	 *	may not of requested one. If that's the case, free it, else write
	 *	it to where our caller said.
	 */
	if (!result) {
		if (our_result) ldap_msgfree(our_result);
	} else {
		*result = our_result;
	}

	return status;
}

/** Modify something in the LDAP directory
 *
 * Binds as the administrative user and attempts to modify an LDAP object.
 *
 * @param[in] inst rlm_ldap configuration.
 * @param[in] request Current request.
 * @param[in,out] pconn to use. May change as this function calls functions which auto re-connect.
 * @param[in] dn of the object to modify.
 * @param[in] mods to make, see 'man ldap_modify' for more information.
 * @param[in] serverctrls Search controls to pass to the server.  May be NULL.
 * @param[in] clientctrls Search controls for ldap_modify.  May be NULL.
 * @return One of the LDAP_PROC_* (#ldap_rcode_t) values.
 */
ldap_rcode_t rlm_ldap_modify(rlm_ldap_t const *inst, REQUEST *request, ldap_handle_t **pconn,
			     char const *dn, LDAPMod *mods[],
			     LDAPControl **serverctrls, LDAPControl **clientctrls)
{
	ldap_rcode_t	status = LDAP_PROC_ERROR;

	int		msgid;		// Message id returned by ldap_search_ext.

	char const 	*error = NULL;
	char		*extra = NULL;

	int 		i;

	LDAPControl	*our_serverctrls[LDAP_MAX_CONTROLS];
	LDAPControl	*our_clientctrls[LDAP_MAX_CONTROLS];

	int		conn_available = 0;

	rlm_ldap_control_merge(our_serverctrls, our_clientctrls,
			       sizeof(our_serverctrls) / sizeof(*our_serverctrls),
			       sizeof(our_clientctrls) / sizeof(*our_clientctrls),
			       *pconn, serverctrls, clientctrls);

	rad_assert(*pconn && (*pconn)->handle);

	if (RDEBUG_ENABLED4) rlm_ldap_timeout_debug(inst, request, *pconn, NULL, __FUNCTION__);

	/*
	 *	Perform all modifications as the admin user.
	 */
	if ((*pconn)->rebound) {
		status = rlm_ldap_bind(inst, request, pconn, (*pconn)->pool_inst->admin_identity,
				       (*pconn)->pool_inst->admin_password, &(*pconn)->pool_inst->admin_sasl, true,
				       NULL, NULL, NULL);
		if (status != LDAP_PROC_SUCCESS) {
			return LDAP_PROC_ERROR;
		}

		rad_assert(*pconn);

		(*pconn)->rebound = false;
	}

	/*
	 *	Pool isn't available during module instantiation
	 */
	conn_available = inst->pool ? fr_connection_pool_state(inst->pool)->num : 0;

	/*
	 *	For sanity, for when no connections are viable,
	 *	and we can't make a new one.
	 */
	for (i = conn_available; i >= 0; i--) {
		RDEBUG2("Modifying object with DN \"%s\"", dn);
		(void) ldap_modify_ext((*pconn)->handle, dn, mods, our_serverctrls, our_clientctrls, &msgid);

		RDEBUG2("Waiting for modify result...");
		status = rlm_ldap_result(inst, *pconn, msgid, dn, NULL, NULL, &error, &extra);
		switch (status) {
		case LDAP_PROC_SUCCESS:
			break;

		case LDAP_PROC_RETRY:
			*pconn = fr_connection_reconnect(inst->pool, request, *pconn);
			if (*pconn) {
				RWDEBUG("Modify failed: %s. Got new socket, retrying...", error);

				talloc_free(extra); /* don't leak debug info */
				continue;
			}

			status = LDAP_PROC_ERROR;

			/* FALL-THROUGH */
		default:
			REDEBUG("Failed modifying object: %s", error);
			REDEBUG("%s", extra);

			goto finish;
		}

		break;
	}

	if (i < 0) {
		ROPTIONAL(REDEBUG, ERROR, "Hit reconnection limit");
		status = LDAP_PROC_ERROR;
	}

finish:
	talloc_free(extra);

	return status;
}

/** Retrieve the DN of a user object
 *
 * Retrieves the DN of a user and adds it to the control list as LDAP-UserDN. Will also retrieve any
 * attributes passed and return the result in *result.
 *
 * This potentially allows for all authorization and authentication checks to be performed in one
 * ldap search operation, which is a big bonus given the number of crappy, slow *cough*AD*cough*
 * LDAP directory servers out there.
 *
 * @param[in] inst rlm_ldap configuration.
 * @param[in] request Current request.
 * @param[in,out] pconn to use. May change as this function calls functions which auto re-connect.
 * @param[in] attrs Additional attributes to retrieve, may be NULL.
 * @param[in] force Query even if the User-DN already exists.
 * @param[out] result Where to write the result, may be NULL in which case result is discarded.
 * @param[out] rcode The status of the operation, one of the RLM_MODULE_* codes.
 * @return The user's DN or NULL on error.
 */
char const *rlm_ldap_find_user(rlm_ldap_t const *inst, REQUEST *request, ldap_handle_t **pconn,
			       char const *attrs[], bool force, LDAPMessage **result, rlm_rcode_t *rcode)
{
	static char const *tmp_attrs[] = { NULL };

	ldap_rcode_t	status;
	VALUE_PAIR	*vp = NULL;
	LDAPMessage	*tmp_msg = NULL, *entry = NULL;
	int		ldap_errno;
	int		cnt;
	char		*dn = NULL;
	char const	*filter = NULL;
	char	    	filter_buff[LDAP_MAX_FILTER_STR_LEN];
	char const	*base_dn;
	char	    	base_dn_buff[LDAP_MAX_DN_STR_LEN];
	LDAPControl	*serverctrls[] = { inst->userobj_sort_ctrl, NULL };

	bool freeit = false;					//!< Whether the message should
								//!< be freed after being processed.

	*rcode = RLM_MODULE_FAIL;

	if (!result) {
		result = &tmp_msg;
		freeit = true;
	}
	*result = NULL;

	if (!attrs) {
		memset(&attrs, 0, sizeof(tmp_attrs));
	}

	/*
	 *	If the caller isn't looking for the result we can just return the current userdn value.
	 */
	if (!force) {
		vp = fr_pair_find_by_child_num(request->control, fr_dict_root(fr_dict_internal),
					       PW_LDAP_USERDN, TAG_ANY);
		if (vp) {
			RDEBUG("Using user DN from request \"%s\"", vp->vp_strvalue);
			*rcode = RLM_MODULE_OK;
			return vp->vp_strvalue;
		}
	}

	/*
	 *	Perform all searches as the admin user.
	 */
	if ((*pconn)->rebound) {
		status = rlm_ldap_bind(inst, request, pconn, (*pconn)->pool_inst->admin_identity,
				       (*pconn)->pool_inst->admin_password, &(*pconn)->pool_inst->admin_sasl, true,
				       NULL, NULL, NULL);
		if (status != LDAP_PROC_SUCCESS) {
			*rcode = RLM_MODULE_FAIL;
			return NULL;
		}

		rad_assert(*pconn);

		(*pconn)->rebound = false;
	}

	if (inst->userobj_filter) {
		if (tmpl_expand(&filter, filter_buff, sizeof(filter_buff), request, inst->userobj_filter,
				rlm_ldap_escape_func, NULL) < 0) {
			REDEBUG("Unable to create filter");
			*rcode = RLM_MODULE_INVALID;

			return NULL;
		}
	}

	if (tmpl_expand(&base_dn, base_dn_buff, sizeof(base_dn_buff), request,
			inst->userobj_base_dn, rlm_ldap_escape_func, NULL) < 0) {
		REDEBUG("Unable to create base_dn");
		*rcode = RLM_MODULE_INVALID;

		return NULL;
	}

	status = rlm_ldap_search(result, inst, request, pconn, base_dn,
				 inst->userobj_scope, filter, attrs, serverctrls, NULL);
	switch (status) {
	case LDAP_PROC_SUCCESS:
		break;

	case LDAP_PROC_BAD_DN:
	case LDAP_PROC_NO_RESULT:
		*rcode = RLM_MODULE_NOTFOUND;
		return NULL;

	default:
		*rcode = RLM_MODULE_FAIL;
		return NULL;
	}

	rad_assert(*pconn);

	/*
	 *	Forbid the use of unsorted search results that
	 *	contain multiple entries, as it's a potential
	 *	security issue, and likely non deterministic.
	 */
	if (!inst->userobj_sort_ctrl) {
		cnt = ldap_count_entries((*pconn)->handle, *result);
		if (cnt > 1) {
			REDEBUG("Ambiguous search result, returned %i unsorted entries (should return 1 or 0).  "
				"Enable sorting, or specify a more restrictive base_dn, filter or scope", cnt);
			REDEBUG("The following entries were returned:");
			RINDENT();
			for (entry = ldap_first_entry((*pconn)->handle, *result);
			     entry;
			     entry = ldap_next_entry((*pconn)->handle, entry)) {
				dn = ldap_get_dn((*pconn)->handle, entry);
				REDEBUG("%s", dn);
				ldap_memfree(dn);
			}
			REXDENT();
			*rcode = RLM_MODULE_INVALID;
			goto finish;
		}
	}

	entry = ldap_first_entry((*pconn)->handle, *result);
	if (!entry) {
		ldap_get_option((*pconn)->handle, LDAP_OPT_RESULT_CODE, &ldap_errno);
		REDEBUG("Failed retrieving entry: %s",
			ldap_err2string(ldap_errno));

		goto finish;
	}

	dn = ldap_get_dn((*pconn)->handle, entry);
	if (!dn) {
		ldap_get_option((*pconn)->handle, LDAP_OPT_RESULT_CODE, &ldap_errno);
		REDEBUG("Retrieving object DN from entry failed: %s", ldap_err2string(ldap_errno));

		goto finish;
	}
	rlm_ldap_normalise_dn(dn, dn);

	/*
	 *	We can't use fr_pair_make here to copy the value into the
	 *	attribute, as the dn must be copied into the attribute
	 *	verbatim (without de-escaping).
	 *
	 *	Special chars are pre-escaped by libldap, and because
	 *	we pass the string back to libldap we must not alter it.
	 */
	RDEBUG("User object found at DN \"%s\"", dn);
	vp = fr_pair_make(request, &request->control, "LDAP-UserDN", NULL, T_OP_EQ);
	if (vp) {
		fr_pair_value_strcpy(vp, dn);
		*rcode = RLM_MODULE_OK;
	}
	ldap_memfree(dn);

finish:
	if ((freeit || (*rcode != RLM_MODULE_OK)) && *result) {
		ldap_msgfree(*result);
		*result = NULL;
	}

	return vp ? vp->vp_strvalue : NULL;
}

/** Check for presence of access attribute in result
 *
 * @param[in] inst rlm_ldap configuration.
 * @param[in] request Current request.
 * @param[in] conn used to retrieve access attributes.
 * @param[in] entry retrieved by rlm_ldap_find_user or rlm_ldap_search.
 * @return
 *	- #RLM_MODULE_USERLOCK if the user was denied access.
 *	- #RLM_MODULE_OK otherwise.
 */
rlm_rcode_t rlm_ldap_check_access(rlm_ldap_t const *inst, REQUEST *request,
				  ldap_handle_t const *conn, LDAPMessage *entry)
{
	rlm_rcode_t rcode = RLM_MODULE_OK;
	struct berval **values = NULL;

	values = ldap_get_values_len(conn->handle, entry, inst->userobj_access_attr);
	if (values) {
		if (inst->access_positive) {
			if ((values[0]->bv_len >= 5) && (strncasecmp(values[0]->bv_val, "false", 5) == 0)) {
				RDEBUG("\"%s\" attribute exists but is set to 'false' - user locked out",
				       inst->userobj_access_attr);
				rcode = RLM_MODULE_USERLOCK;
			}
			/* RLM_MODULE_OK set above... */
		} else if ((values[0]->bv_len < 5) || (strncasecmp(values[0]->bv_val, "false", 5) != 0)) {
			RDEBUG("\"%s\" attribute exists - user locked out", inst->userobj_access_attr);
			rcode = RLM_MODULE_USERLOCK;
		}
		ldap_value_free_len(values);
	} else if (inst->access_positive) {
		RDEBUG("No \"%s\" attribute - user locked out", inst->userobj_access_attr);
		rcode = RLM_MODULE_USERLOCK;
	}

	return rcode;
}

/** Verify we got a password from the search
 *
 * Checks to see if after the LDAP to RADIUS mapping has been completed that a reference password.
 *
 * @param inst rlm_ldap configuration.
 * @param request Current request.
 * @param conn the connection handle
 */
void rlm_ldap_check_reply(rlm_ldap_t const *inst, REQUEST *request, ldap_handle_t const *conn)
{
       /*
	*	More warning messages for people who can't be bothered to read the documentation.
	*
	*	Expect_password is set when we process the mapping, and is only true if there was a mapping between
	*	an LDAP attribute and a password reference attribute in the control list.
	*/
	if (!inst->expect_password || (rad_debug_lvl < L_DBG_LVL_2)) return;

	if (!fr_pair_find_by_child_num(request->control,
				       fr_dict_root(fr_dict_internal), PW_CLEARTEXT_PASSWORD, TAG_ANY) &&
	    !fr_pair_find_by_child_num(request->control,
	    			       fr_dict_root(fr_dict_internal), PW_NT_PASSWORD, TAG_ANY) &&
	    !fr_pair_find_by_child_num(request->control,
	    			       fr_dict_root(fr_dict_internal), PW_USER_PASSWORD, TAG_ANY) &&
	    !fr_pair_find_by_child_num(request->control,
	    			       fr_dict_root(fr_dict_internal), PW_PASSWORD_WITH_HEADER, TAG_ANY) &&
	    !fr_pair_find_by_child_num(request->control,
	    			       fr_dict_root(fr_dict_internal), PW_CRYPT_PASSWORD, TAG_ANY)) {
		switch (conn->directory->type) {
		case LDAP_DIRECTORY_ACTIVE_DIRECTORY:
			RWDEBUG("!!! Found map between LDAP attribute and a FreeRADIUS password attribute");
			RWDEBUG("!!! Active Directory does not allow passwords to be read via LDAP");
			RWDEBUG("!!! Remove the password map and either:");
			RWDEBUG("!!!  - Configure authentication via ntlm_auth (mschapv2 only)");
			RWDEBUG("!!!  - Configure authentication via wbclient (mschapv2 only)");
			RWDEBUG("!!!  - Bind as the user by listing %s in the authenticate section, and",
				inst->name);
			RWDEBUG("!!!	setting attribute &control:Auth-Type := '%s' in the authorize section",
				inst->name);
			RWDEBUG("!!!    (pap only)");

			break;

		case LDAP_DIRECTORY_EDIRECTORY:
			RWDEBUG("!!! Found map between LDAP attribute and a FreeRADIUS password attribute");
			RWDEBUG("!!! eDirectory does not allow passwords to be retrieved via LDAP search");
			RWDEBUG("!!! Remove the password map and either:");
			RWDEBUG("!!!  - Set 'edir = yes' and enable the universal password feature on your ");
			RWDEBUG("!!!    eDir server (recommended)");
			RWDEBUG("!!!  - Bind as the user by listing %s in the authenticate section, and",
				inst->name);
			RWDEBUG("!!!	setting attribute &control:Auth-Type := '%s' in the authorize section",
				inst->name);
			RWDEBUG("!!!    (pap only)");
			break;

		default:
			if (!conn->pool_inst->admin_identity) {
				RWDEBUG("!!! Found map between LDAP attribute and a FreeRADIUS password attribute");
				RWDEBUG("!!! but no password attribute found in search result");
				RWDEBUG("!!! Either:");
				RWDEBUG("!!!  - Ensure the user object contains a password attribute, and that ");
				RWDEBUG("!!!    \"%s\" has permission to read that password attribute (recommended)",
					conn->pool_inst->admin_identity);
				RWDEBUG("!!!  - Bind as the user by listing %s in the authenticate section, and",
					inst->name);
				RWDEBUG("!!!	setting attribute &control:Auth-Type := '%s' in the authorize section",
					inst->name);
				RWDEBUG("!!!    (pap only)");
			} else {
				RWDEBUG("!!! No \"known good\" password added");
				RWDEBUG("!!! but no password attribute found in search result");
				RWDEBUG("!!! Either:");
				RWDEBUG("!!!  - Ensure the user object contains a password attribute, and that ");
				RWDEBUG("!!!    'identity' is set to the DN of an account that has permission to read");
				RWDEBUG("!!!    that password attribute");
				RWDEBUG("!!!  - Bind as the user by listing %s in the authenticate section, and",
					inst->name);
				RWDEBUG("!!!	setting attribute &control:Auth-Type := '%s' in the authorize section",
					inst->name);
				RWDEBUG("!!!    (pap only)");
			}
			break;
		}
	}
}

#if LDAP_SET_REBIND_PROC_ARGS == 3
/** Callback for OpenLDAP to rebind and chase referrals
 *
 * Called by OpenLDAP when it receives a referral and has to rebind.
 *
 * @param handle to rebind.
 * @param url to bind to.
 * @param request that triggered the rebind.
 * @param msgid that triggered the rebind.
 * @param ctx rlm_ldap configuration.
 */
static int rlm_ldap_rebind(LDAP *handle, LDAP_CONST char *url, UNUSED ber_tag_t request, UNUSED ber_int_t msgid,
			   void *ctx)
{
	ldap_rcode_t			status;
	ldap_handle_t			*conn = talloc_get_type_abort(ctx, ldap_handle_t);
	ldap_pool_inst_t const		*pool_inst = conn->pool_inst;
	rlm_ldap_t const		*inst = conn->inst;

	char const			*admin_identity = NULL;
	char const			*admin_password = NULL;

	int				ldap_errno;

	conn->referred = true;
	conn->rebound = true;	/* not really, but oh well... */
	rad_assert(handle == conn->handle);

	DEBUG("Rebinding to URL %s", url);

#  ifdef HAVE_LDAP_URL_PARSE
	/*
	 *	Use bindname and x-bindpw extensions to get the bind credentials
	 *	SASL mech is inherited from the module that defined the connection
	 *	pool.
	 */
	if (pool_inst->use_referral_credentials) {
		LDAPURLDesc	*ldap_url;
		int		ret;
		char		**ext;

		ret = ldap_url_parse(url, &ldap_url);
		if (ret != LDAP_SUCCESS) {
			ERROR("Failed parsing LDAP URL \"%s\": %s", url, ldap_err2string(ret));
			return -1;
		}

		/*
		 *	If there are no extensions, OpenLDAP doesn't
		 *	bother allocating an array.
		 */
		for (ext = ldap_url->lud_exts; ext && *ext; ext++) {
			char const *p;
			bool critical = false;

			p = *ext;

			if (*p == '!') {
				critical = true;
				p++;
			}

			/*
			 *	LDAP Parse URL unescapes the extensions for us
			 */
			switch (fr_substr2int(ldap_supported_extensions, p, LDAP_EXT_UNSUPPORTED, -1)) {
			case LDAP_EXT_BINDNAME:
				p = strchr(p, '=');
				if (!p) {
				bad_ext:
					ERROR("Failed parsing extension \"%s\": "
					      "No attribute/value delimiter '='", *ext);
					ldap_free_urldesc(ldap_url);
					return LDAP_OTHER;
				}
				admin_identity = p + 1;
				break;

			case LDAP_EXT_BINDPW:
				p = strchr(p, '=');
				if (!p) goto bad_ext;
				admin_password = p + 1;
				break;

			default:
				if (critical) {
					ERROR("Failed parsing critical extension \"%s\": "
					      "Not supported by rlm_ldap", *ext);
					ldap_free_urldesc(ldap_url);
					return LDAP_OTHER;
				}
				DEBUG2("Skipping unsupported extension \"%s\"", *ext);
				continue;
			}
		}
		ldap_free_urldesc(ldap_url);
	} else
#  endif
	{
		admin_identity = pool_inst->admin_identity;
		admin_password = pool_inst->admin_password;
	}

	status = rlm_ldap_bind(inst, NULL, &conn, admin_identity, admin_password,
			       &conn->pool_inst->admin_sasl, false, NULL, NULL, NULL);
	if (status != LDAP_PROC_SUCCESS) {
		ldap_get_option(handle, LDAP_OPT_ERROR_NUMBER, &ldap_errno);

		return ldap_errno;
	}

	return LDAP_SUCCESS;
}
#endif

int rlm_ldap_global_init(rlm_ldap_t *inst)
{
	int ldap_errno;

	rad_assert(inst); /* clang scan */

#define do_ldap_global_option(_option, _name, _value) \
	if (ldap_set_option(NULL, _option, _value) != LDAP_OPT_SUCCESS) { \
		ldap_get_option(NULL, LDAP_OPT_ERROR_NUMBER, &ldap_errno); \
		ERROR("Failed setting global option %s: %s", _name, \
			 (ldap_errno != LDAP_SUCCESS) ? ldap_err2string(ldap_errno) : "Unknown error"); \
		return -1;\
	}

#define maybe_ldap_global_option(_option, _name, _value) \
	if (_value) do_ldap_global_option(_option, _name, _value)

#ifdef LDAP_OPT_DEBUG_LEVEL
	/*
	 *	Can't use do_ldap_global_option
	 */
	if (inst->ldap_debug) do_ldap_global_option(LDAP_OPT_DEBUG_LEVEL, "ldap_debug", &(inst->ldap_debug));
#endif

#ifdef LDAP_OPT_X_TLS_RANDOM_FILE
	/*
	 *	OpenLDAP will error out if we attempt to set
	 *	this on a handle. Presumably it's global in
	 *	OpenSSL too.
	 */
	maybe_ldap_global_option(LDAP_OPT_X_TLS_RANDOM_FILE, "random_file", inst->tls_random_file);
#endif
	return 0;
}

/** Close and delete a connection
 *
 * Unbinds the LDAP connection, informing the server and freeing any memory, then releases the memory used by the
 * connection handle.
 *
 * @param conn to destroy.
 * @return always indicates success.
 */
static int _mod_conn_free(ldap_handle_t *conn)
{
	rlm_ldap_t const *inst = conn->inst;

	rad_assert(conn->handle);

	rlm_ldap_control_clear(conn);

#ifdef HAVE_LDAP_UNBIND_EXT_S
	LDAPControl	*our_serverctrls[LDAP_MAX_CONTROLS];
	LDAPControl	*our_clientctrls[LDAP_MAX_CONTROLS];

	rlm_ldap_control_merge(our_serverctrls, our_clientctrls,
			       sizeof(our_serverctrls) / sizeof(*our_serverctrls),
			       sizeof(our_clientctrls) / sizeof(*our_clientctrls),
			       conn, NULL, NULL);

	DEBUG3("Closing libldap handle %p", conn->handle);
	ldap_unbind_ext_s(conn->handle, our_serverctrls, our_clientctrls);
#else
	DEBUG3("Closing libldap handle %p", conn->handle);
	ldap_unbind_s(conn->handle);
#endif
	conn->handle = NULL;

	return 0;
}

/** Create and return a new connection
 *
 * Create a new ldap connection and allocate memory for a new rlm_handle_t
 */
void *mod_conn_create(TALLOC_CTX *ctx, void *instance, struct timeval const *timeout)
{
	ldap_rcode_t status;

	int ldap_errno, ldap_version;

	rlm_ldap_t const	*inst = talloc_get_type_abort(instance, rlm_ldap_t);
	ldap_pool_inst_t const	*pool_inst = &inst->pool_inst;
	ldap_handle_t		*conn;
	LDAP			*handle = NULL;

	DEBUG("Connecting to %s", pool_inst->server);
#ifdef HAVE_LDAP_INITIALIZE
	ldap_errno = ldap_initialize(&handle, pool_inst->server);
	if (ldap_errno != LDAP_SUCCESS) {
		ERROR("ldap_initialize failed: %s", ldap_err2string(ldap_errno));
		return NULL;
	}
#else
	handle = ldap_init(pool_inst->server, pool_inst->port);
	if (!handle) {
		ERROR("ldap_init failed");
		return NULL;
	}
#endif

	/*
	 *	Allocate memory for the handle.
	 */
	conn = talloc_zero(ctx, ldap_handle_t);
	if (!conn) return NULL;

	conn->inst = inst;
	conn->pool_inst = pool_inst;
	conn->handle = handle;
	conn->rebound = false;
	conn->referred = false;
	talloc_set_destructor(conn, _mod_conn_free);

	DEBUG3("New libldap handle %p", conn->handle);

	/*
	 *	We now have a connection structure, but no actual connection.
	 *
	 *	Set a bunch of LDAP options, using common code.
	 */
#define do_ldap_option(_option, _name, _value) \
	if (ldap_set_option(conn->handle, _option, _value) != LDAP_OPT_SUCCESS) { \
		ldap_get_option(conn->handle, LDAP_OPT_ERROR_NUMBER, &ldap_errno); \
		ERROR("Failed setting connection option %s: %s", _name, \
			 (ldap_errno != LDAP_SUCCESS) ? ldap_err2string(ldap_errno) : "Unknown error"); \
		goto error;\
	}

#define maybe_ldap_option(_option, _name, _value) \
	if (_value) do_ldap_option(_option, _name, _value)

	/*
	 *	Leave "dereference" unset to use the OpenLDAP default.
	 */
	if (pool_inst->dereference_str) {
		do_ldap_option(LDAP_OPT_DEREF, "dereference", &(pool_inst->dereference));
	}

	/*
	 *	Leave "chase_referrals" unset to use the OpenLDAP default.
	 */
	if (!pool_inst->chase_referrals_unset) {
		if (pool_inst->chase_referrals) {
			do_ldap_option(LDAP_OPT_REFERRALS, "chase_referrals", LDAP_OPT_ON);

			if (pool_inst->rebind == true) {
#if LDAP_SET_REBIND_PROC_ARGS == 3
				ldap_set_rebind_proc(conn->handle, rlm_ldap_rebind, conn);
#endif
			}
		} else {
			do_ldap_option(LDAP_OPT_REFERRALS, "chase_referrals", LDAP_OPT_OFF);
		}
	}

#ifdef LDAP_OPT_NETWORK_TIMEOUT
	/*
	 *	A value of zero results in an pool_instant failure.
	 *
	 *	When most people specify zero they mean infinite.
	 *
	 *	libldap requires tv_sec to be -1 to mean that.
	 */
	{
		struct timeval ldap_timeout = *timeout;

		if ((ldap_timeout.tv_usec == 0) && (ldap_timeout.tv_sec == 0)) ldap_timeout.tv_sec = -1;

		do_ldap_option(LDAP_OPT_NETWORK_TIMEOUT, "pool.connect_timeout", &ldap_timeout);
	}
#endif

	do_ldap_option(LDAP_OPT_TIMELIMIT, "srv_timelimit", &(pool_inst->srv_timelimit));

	ldap_version = LDAP_VERSION3;
	do_ldap_option(LDAP_OPT_PROTOCOL_VERSION, "ldap_version", &ldap_version);

#ifdef LDAP_OPT_X_KEEPALIVE_IDLE
	do_ldap_option(LDAP_OPT_X_KEEPALIVE_IDLE, "keepalive_idle", &(pool_inst->keepalive_idle));
#endif

#ifdef LDAP_OPT_X_KEEPALIVE_PROBES
	do_ldap_option(LDAP_OPT_X_KEEPALIVE_PROBES, "keepalive_probes", &(pool_inst->keepalive_probes));
#endif

#ifdef LDAP_OPT_X_KEEPALIVE_INTERVAL
	do_ldap_option(LDAP_OPT_X_KEEPALIVE_INTERVAL, "keepalive_interval", &(pool_inst->keepalive_interval));
#endif

#ifdef HAVE_LDAP_START_TLS_S
	/*
	 *	Set all of the TLS options
	 */
	if (pool_inst->tls_mode) do_ldap_option(LDAP_OPT_X_TLS, "tls_mode", &(pool_inst->tls_mode));

	maybe_ldap_option(LDAP_OPT_X_TLS_CACERTFILE, "ca_file", pool_inst->tls_ca_file);
	maybe_ldap_option(LDAP_OPT_X_TLS_CACERTDIR, "ca_path", pool_inst->tls_ca_path);

	/*
	 *	Set certificate options
	 */
	maybe_ldap_option(LDAP_OPT_X_TLS_CERTFILE, "certificate_file", pool_inst->tls_certificate_file);
	maybe_ldap_option(LDAP_OPT_X_TLS_KEYFILE, "private_key_file", pool_inst->tls_private_key_file);

#  ifdef LDAP_OPT_X_TLS_NEVER
	if (pool_inst->tls_require_cert_str) {
		do_ldap_option(LDAP_OPT_X_TLS_REQUIRE_CERT, "require_cert", &pool_inst->tls_require_cert);
	}
#  endif

	/*
	 *	Counter intuitively the TLS context appears to need to be initialised
	 *	after all the TLS options are set on the handle.
	 */
#  ifdef LDAP_OPT_X_TLS_NEWCTX
	{
		/* Always use the new TLS configuration context */
		int is_server = 0;
		do_ldap_option(LDAP_OPT_X_TLS_NEWCTX, "new TLS context", &is_server);
	}
#  endif

	/*
	 *	And finally start the TLS code.
	 */
	if (pool_inst->start_tls) {
		if (pool_inst->port == 636) {
			WARN("Told to Start TLS on LDAPS port this will probably fail, please correct the "
			     "configuration");
		}

		if (ldap_start_tls_s(conn->handle, NULL, NULL) != LDAP_SUCCESS) {
			ldap_get_option(conn->handle, LDAP_OPT_ERROR_NUMBER, &ldap_errno);

			ERROR("Could not start TLS: %s", ldap_err2string(ldap_errno));
			goto error;
		}
	}
#endif /* HAVE_LDAP_START_TLS_S */

	status = rlm_ldap_bind(inst, NULL, &conn, conn->pool_inst->admin_identity, conn->pool_inst->admin_password,
			       &(conn->pool_inst->admin_sasl), false, timeout, NULL, NULL);
	if (status != LDAP_PROC_SUCCESS) goto error;

	/*
	 *	Only error out on memory allocation errors
	 */
	if (rlm_ldap_directory_alloc(conn, &conn->directory, inst, &conn) < 0) goto error;

	return conn;

error:
	talloc_free(conn);

	return NULL;
}

/** Gets an LDAP socket from the connection pool
 *
 * Retrieve a socket from the connection pool, or NULL on error (of if no sockets are available).
 *
 * @param inst rlm_ldap configuration.
 * @param request Current request (may be NULL).
 */

ldap_handle_t *mod_conn_get(rlm_ldap_t const *inst, REQUEST *request)
{
	ldap_handle_t *conn;

	conn = fr_connection_get(inst->pool, request);

	rad_assert(!conn || conn->pool_inst);

#ifdef LDAP_CONTROL_X_SESSION_TRACKING
	/*
	 *	Add optional session tracking controls,
	 *	that contain values of some attributes
	 *	in the request.
	 */
	if ((conn != NULL) && (request != NULL) && conn->pool_inst->session_tracking) {
		if (rlm_ldap_control_add_session_tracking(conn, request) < 0) {
			fr_connection_release(inst->pool, request, conn);
			return NULL;
		}
	}
#endif
	return conn;
}

/** Frees an LDAP socket back to the connection pool
 *
 * If the socket was rebound chasing a referral onto another server then we destroy it.
 * If the socket was rebound to another user on the same server, we let the next caller rebind it.
 *
 * @param inst rlm_ldap configuration.
 * @param request The current request.
 * @param conn to release.
 */
void mod_conn_release(rlm_ldap_t const *inst, REQUEST *request, ldap_handle_t *conn)
{
	/*
	 *	Could have already been free'd due to a previous error.
	 */
	if (!conn) return;

	/*
	 *	Clear any client/server controls associated with the connection.
	 */
	rlm_ldap_control_clear(conn);

	/*
	 *	We chased a referral to another server.
	 *
	 *	This connection is no longer part of the pool which is
	 *	connected to and bound to the configured server.
	 *	Close it.
	 *
	 *	Note that we do NOT close it if it was bound to another user.
	 *	Instead, we let the next caller do the rebind.
	 */
	if (conn->referred) {
		fr_connection_close(inst->pool, request, conn);
		return;
	}

	fr_connection_release(inst->pool, request, conn);
	return;
}
