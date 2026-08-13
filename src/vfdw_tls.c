/*-------------------------------------------------------------------------
 *
 * vfdw_tls.c
 *		TLS transport, with real hostname verification.
 *
 * libvalkey's own TLS helper is not sufficient here. As of 0.5.0 it wires
 * valkeyTLSOptions.server_name into SSL_set_tlsext_host_name only - that is
 * SNI - and never calls X509_VERIFY_PARAM_set1_host. The result is chain
 * validation without identity checking: a certificate signed by a trusted CA
 * but issued for a different host is accepted. That is precisely the case
 * hostname verification exists to catch, so tls_verify = 'full' cannot be
 * built on it.
 *
 * We therefore construct the SSL_CTX and SSL ourselves, set the verification
 * parameters, and hand the finished SSL to valkeyInitiateTLS() - the entry
 * point libvalkey documents for callers managing their own context.
 *
 * Copyright (c) 2026, The valkey_fdw contributors
 *
 * Released under the PostgreSQL License; see the LICENSE file.
 *
 * IDENTIFICATION
 *		src/vfdw_tls.c
 *
 *-------------------------------------------------------------------------
 */
#include "vfdw_tls.h"

#include <arpa/inet.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include <valkey/tls.h>

#include "utils/builtins.h"

#include "vfdw_error.h"

/* Mirrors the tls_verify enum in the option table. */
#define VFDW_TLS_VERIFY_FULL	0
#define VFDW_TLS_VERIFY_CA		1
#define VFDW_TLS_VERIFY_NONE	2

static bool vfdw_openssl_ready = false;

/*
 * Why the peer certificate was rejected, or X509_V_OK.
 *
 * OpenSSL's generic "certificate verify failed" is the same string for a
 * wrong hostname, a missing trust anchor and an expired certificate, so an
 * error carrying only that cannot say which check did the rejecting - and a
 * test asserting it cannot tell an expiry regression from a CA-loading one.
 * The specific reason exists only inside the verification callback, so it is
 * captured there.
 *
 * A backend-lifetime static rather than something hung off the connection:
 * the handshake completes lazily, during the first read or write, long after
 * vfdw_tls_attach has handed the SSL to libvalkey and lost its own handle on
 * it. Safe because a backend is single-threaded and one connection is opened
 * at a time; it is reset at the start of every attach so a stale reason from
 * an earlier connection can never be reported against a later one.
 *
 * Only the FIRST failure is kept. With SSL_VERIFY_PEER the callback returning
 * 0 aborts the walk, so there is only one - but under tls_verify 'none' the
 * callback still runs and its answer is ignored, and recording the last would
 * let a later, vaguer error overwrite the specific one.
 */
static int vfdw_tls_verify_error = X509_V_OK;

static int
vfdw_tls_verify_cb(int preverify_ok, X509_STORE_CTX *store)
{
	if (!preverify_ok && vfdw_tls_verify_error == X509_V_OK)
		vfdw_tls_verify_error = X509_STORE_CTX_get_error(store);

	return preverify_ok;
}

/*
 * The advice that fits the check which actually refused.
 *
 * One hint for every failure was still misdirection after the DETAIL learned
 * to name the check: "Check tls_ca_file, and that the server certificate
 * names the host" points at the two things that are CORRECT when a
 * certificate has merely expired, and sends the reader to re-examine a CA
 * file and a hostname that were never the problem.
 *
 * The default stays deliberately broad. These cases are the ones the test
 * suite can produce and a deployment is likely to hit; the rest of
 * X509_V_ERR_* is long, and a confidently wrong specific hint would be worse
 * than an honest general one.
 */
static const char *
vfdw_tls_verify_hint(int err)
{
	switch (err)
	{
		case X509_V_ERR_HOSTNAME_MISMATCH:
		case X509_V_ERR_IP_ADDRESS_MISMATCH:
			return "The chain is trusted; it is the name that does not match. "
				"Check the host option, or set tls_sni to a name the "
				"certificate carries.";

		case X509_V_ERR_CERT_HAS_EXPIRED:
		case X509_V_ERR_CERT_NOT_YET_VALID:
			return "The certificate is trusted and correctly named, but not "
				"valid now. Check its validity window and the clock at both "
				"ends.";

		case X509_V_ERR_DEPTH_ZERO_SELF_SIGNED_CERT:
		case X509_V_ERR_SELF_SIGNED_CERT_IN_CHAIN:
		case X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT:
		case X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT_LOCALLY:
		case X509_V_ERR_UNABLE_TO_VERIFY_LEAF_SIGNATURE:
			return "Nothing anchors this chain to a CA we trust. Check that "
				"tls_ca_file names the CA which signed the server "
				"certificate.";

		default:
			return "Check tls_ca_file, and that the server certificate names "
				"the host in the server options.";
	}
}

const char *
vfdw_tls_verify_reason(const char **hint)
{
	int			err = vfdw_tls_verify_error;

	/*
	 * Taken, not read: a reason belongs to one handshake, and leaving it
	 * behind would attach it to whatever failed next.
	 */
	vfdw_tls_verify_error = X509_V_OK;

	/*
	 * The hint is always set, including when there is no verification reason
	 * at all, so the caller has something to print for a handshake that failed
	 * for some other cause.
	 */
	*hint = vfdw_tls_verify_hint(err);

	return err == X509_V_OK ? NULL : X509_verify_cert_error_string(err);
}

/*
 * Pull OpenSSL's most recent error into palloc'd memory.
 *
 * Always returns something printable: an empty queue still needs a message,
 * and "" in a DETAIL is worse than saying so.
 */
char *
vfdw_tls_take_error(void)
{
	unsigned long code = ERR_get_error();
	char		buf[256];

	if (code == 0)
		return NULL;

	ERR_error_string_n(code, buf, sizeof(buf));
	ERR_clear_error();
	return pstrdup(buf);
}

/*
 * As above, but for contexts that must print something: an empty DETAIL is
 * worse than saying the queue was empty.
 */
static char *
vfdw_tls_last_error(void)
{
	char	   *reason = vfdw_tls_take_error();

	return reason != NULL ? reason : pstrdup("no OpenSSL error was recorded");
}

/*
 * Fail while we are the sole owner of ssl and ctx.
 *
 * Copy the message, release, then report - the same ordering rule the
 * connection layer follows. Reporting first would leak both objects, since
 * ereport does not return.
 */
static void
vfdw_tls_fail(SSL *ssl, SSL_CTX *ctx, const char *what)
{
	char	   *detail = vfdw_tls_last_error();

	if (ssl != NULL)
		SSL_free(ssl);
	if (ctx != NULL)
		SSL_CTX_free(ctx);

	vfdw_ereport(ERROR, ERRCODE_CONNECTION_FAILURE, what, detail);
	pg_unreachable();
}

static bool
vfdw_is_ip_literal(const char *host)
{
	unsigned char buf[sizeof(struct in6_addr)];

	return inet_pton(AF_INET, host, buf) == 1 ||
		inet_pton(AF_INET6, host, buf) == 1;
}

/*
 * Build the client context: protocol floor, trust anchors, client identity.
 */
static SSL_CTX *
vfdw_tls_make_context(const VfdwServerOptions *opts)
{
	SSL_CTX    *ctx;

	ctx = SSL_CTX_new(TLS_client_method());
	if (ctx == NULL)
		vfdw_tls_fail(NULL, NULL, "could not create a TLS context");

	/*
	 * TLS 1.0 and 1.1 are deprecated and no longer meaningfully secure. Refuse
	 * them rather than leaving the floor at whatever the platform defaults to.
	 */
	if (!SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION))
		vfdw_tls_fail(NULL, ctx, "could not set the minimum TLS version");

	if (opts->tls_ca_file != NULL)
	{
		if (!SSL_CTX_load_verify_locations(ctx, opts->tls_ca_file, NULL))
			vfdw_tls_fail(NULL, ctx, "could not load the CA certificate file");
	}
	else if (opts->tls_verify != VFDW_TLS_VERIFY_NONE)
	{
		/*
		 * With no explicit CA and verification on, fall back to the system
		 * trust store rather than silently trusting nothing - failing to load
		 * it would otherwise reject every certificate for a reason the user
		 * cannot see.
		 */
		if (!SSL_CTX_set_default_verify_paths(ctx))
			vfdw_tls_fail(NULL, ctx, "could not load the system CA store");
	}

	if (opts->tls_cert_file != NULL)
	{
		if (!SSL_CTX_use_certificate_chain_file(ctx, opts->tls_cert_file))
			vfdw_tls_fail(NULL, ctx, "could not load the client certificate");

		if (opts->tls_key_file == NULL)
		{
			SSL_CTX_free(ctx);
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("tls_cert_file requires tls_key_file")));
		}

		if (!SSL_CTX_use_PrivateKey_file(ctx, opts->tls_key_file,
										 SSL_FILETYPE_PEM))
			vfdw_tls_fail(NULL, ctx, "could not load the client private key");

		if (!SSL_CTX_check_private_key(ctx))
			vfdw_tls_fail(NULL, ctx,
						  "the client private key does not match the certificate");
	}

	SSL_CTX_set_verify(ctx,
					   opts->tls_verify == VFDW_TLS_VERIFY_NONE
					   ? SSL_VERIFY_NONE : SSL_VERIFY_PEER,
					   vfdw_tls_verify_cb);

	return ctx;
}

/*
 * Apply identity checking to the SSL handle.
 *
 * This is the part libvalkey does not do, and the reason this module exists.
 * Under tls_verify = 'full' the peer certificate must name the host we asked
 * for; under 'ca' the chain must be trusted but the name is not checked.
 */
static void
vfdw_tls_set_identity(SSL *ssl, SSL_CTX *ctx, const VfdwServerOptions *opts)
{
	const char *peer_name = opts->tls_sni != NULL ? opts->tls_sni : opts->host;

	/*
	 * SNI carries a host name, never an address literal, so an IP host is
	 * simply not sent. Verification below still pins to it.
	 */
	if (!vfdw_is_ip_literal(peer_name))
	{
		if (!SSL_set_tlsext_host_name(ssl, peer_name))
			vfdw_tls_fail(ssl, ctx, "could not set the TLS server name");
	}

	if (opts->tls_verify != VFDW_TLS_VERIFY_FULL)
		return;

	{
		X509_VERIFY_PARAM *param = SSL_get0_param(ssl);
		int			ok;

		/*
		 * A wildcard may stand for a whole label but not part of one:
		 * *.example.com is acceptable, w*.example.com is not.
		 */
		X509_VERIFY_PARAM_set_hostflags(param,
										X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);

		if (vfdw_is_ip_literal(peer_name))
			ok = X509_VERIFY_PARAM_set1_ip_asc(param, peer_name);
		else
			ok = X509_VERIFY_PARAM_set1_host(param, peer_name, 0);

		if (!ok)
			vfdw_tls_fail(ssl, ctx, "could not enable TLS hostname verification");
	}
}

void
vfdw_tls_attach(valkeyContext *conn, const VfdwServerOptions *opts)
{
	SSL_CTX    *ctx;
	SSL		   *ssl;

	if (!vfdw_openssl_ready)
	{
		if (valkeyInitOpenSSL() != VALKEY_OK)
			ereport(ERROR,
					(errcode(ERRCODE_CONNECTION_FAILURE),
					 errmsg("could not initialise OpenSSL")));
		vfdw_openssl_ready = true;
	}

	ERR_clear_error();
	vfdw_tls_verify_error = X509_V_OK;

	ctx = vfdw_tls_make_context(opts);

	ssl = SSL_new(ctx);
	if (ssl == NULL)
		vfdw_tls_fail(NULL, ctx, "could not create a TLS session");

	/*
	 * SSL_new took a reference, so the context now outlives this call through
	 * the session and is released with it. Dropping ours here means there is
	 * one fewer thing to unwind on every path below.
	 */
	SSL_CTX_free(ctx);
	ctx = NULL;

	vfdw_tls_set_identity(ssl, NULL, opts);

	/*
	 * valkeyInitiateTLS sets the socket on the session itself. On failure it
	 * releases its own wrapper but not the session, so that is ours to free -
	 * and the message has to be copied out of the context before anything is
	 * released.
	 */
	if (valkeyInitiateTLS(conn, ssl) != VALKEY_OK)
	{
		char	   *detail = pstrdup(conn->errstr);

		SSL_free(ssl);
		vfdw_ereport(ERROR, ERRCODE_CONNECTION_FAILURE,
					 "TLS handshake with Valkey failed", detail);
	}
}
