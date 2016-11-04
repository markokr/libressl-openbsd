/* $OpenBSD$ */

/*
 * Copyright (c) 2016 Joel Sing <jsing@openbsd.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <limits.h>

#include <openssl/ssl.h>
#include <openssl/x509.h>

#include "ssl_locl.h"

#include "bytestring.h"

#define SSLASN1_TAG	(CBS_ASN1_CONSTRUCTED | CBS_ASN1_CONTEXT_SPECIFIC)
#define SSLASN1_TIME_TAG		(SSLASN1_TAG | 1)
#define SSLASN1_TIMEOUT_TAG		(SSLASN1_TAG | 2)
#define SSLASN1_PEER_CERT_TAG		(SSLASN1_TAG | 3)
#define SSLASN1_SESSION_ID_CTX_TAG	(SSLASN1_TAG | 4)
#define SSLASN1_VERIFY_RESULT_TAG	(SSLASN1_TAG | 5)
#define SSLASN1_HOSTNAME_TAG		(SSLASN1_TAG | 6)
#define SSLASN1_LIFETIME_TAG		(SSLASN1_TAG | 9)
#define SSLASN1_TICKET_TAG		(SSLASN1_TAG | 10)

static uint64_t
time_max(void)
{
	if (sizeof(time_t) == sizeof(int32_t))
		return INT32_MAX;
	if (sizeof(time_t) == sizeof(int64_t))
		return INT64_MAX;
	return 0;
}

int
i2d_SSL_SESSION(SSL_SESSION *s, unsigned char **pp)
{
	CBB cbb, session, cipher_suite, session_id, master_key, time, timeout;
	CBB peer_cert, sidctx, verify_result, hostname, lifetime, ticket;
	CBB value;
	unsigned char *data = NULL, *peer_cert_bytes = NULL;
	int len, rv = -1;
	size_t data_len;
	uint16_t cid;

	if (s == NULL)
		return (0);

	if (s->cipher == NULL && s->cipher_id == 0)
		return (0);

	if (!CBB_init(&cbb, 0))
		goto err;

	if (!CBB_add_asn1(&cbb, &session, CBS_ASN1_SEQUENCE))
		goto err;

	/* Session ASN1 version. */
	if (!CBB_add_asn1_uint64(&session, SSL_SESSION_ASN1_VERSION))
		goto err;

	/* TLS/SSL protocol version. */
	if (s->ssl_version < 0)
		goto err;
	if (!CBB_add_asn1_uint64(&session, s->ssl_version))
		goto err;

	/* Cipher suite ID. */
	/* XXX - require cipher to be non-NULL or always/only use cipher_id. */
	cid = (uint16_t)(s->cipher_id & 0xffff);
	if (s->cipher != NULL)
		cid = ssl3_cipher_get_value(s->cipher);
	if (!CBB_add_asn1(&session, &cipher_suite, CBS_ASN1_OCTETSTRING))
		goto err;
	if (!CBB_add_u16(&cipher_suite, cid))
		goto err;

	/* Session ID. */
	if (!CBB_add_asn1(&session, &session_id, CBS_ASN1_OCTETSTRING))
		goto err;
	if (!CBB_add_bytes(&session_id, s->session_id, s->session_id_length))
		goto err;

	/* Master key. */
	if (!CBB_add_asn1(&session, &master_key, CBS_ASN1_OCTETSTRING))
		goto err;
	if (!CBB_add_bytes(&master_key, s->master_key, s->master_key_length))
		goto err;

	/* Time [1]. */
	if (s->time != 0) {
		if (s->time < 0)
			goto err;
		if (!CBB_add_asn1(&session, &time, SSLASN1_TIME_TAG))
			goto err;
		if (!CBB_add_asn1_uint64(&time, s->time))
			goto err;
	}

	/* Timeout [2]. */
	if (s->timeout != 0) {
		if (s->timeout < 0)
			goto err;
		if (!CBB_add_asn1(&session, &timeout, SSLASN1_TIMEOUT_TAG))
			goto err;
		if (!CBB_add_asn1_uint64(&timeout, s->timeout))
			goto err;
	}

	/* Peer certificate [3]. */
	if (s->peer != NULL) {
		if (!CBB_add_asn1(&session, &peer_cert, SSLASN1_PEER_CERT_TAG))
			goto err;
		if (!CBB_add_asn1(&peer_cert, &value, CBS_ASN1_OCTETSTRING))
			goto err;
		if ((len = i2d_X509(s->peer, &peer_cert_bytes)) <= 0)
			goto err;
		if (!CBB_add_bytes(&value, peer_cert_bytes, len))
			goto err;
	}

	/* Session ID context [4]. */
	/* XXX - Actually handle this as optional? */
	if (!CBB_add_asn1(&session, &sidctx, SSLASN1_SESSION_ID_CTX_TAG))
		goto err;
	if (!CBB_add_asn1(&sidctx, &value, CBS_ASN1_OCTETSTRING))
		goto err;
	if (!CBB_add_bytes(&value, s->sid_ctx, s->sid_ctx_length))
		goto err;

	/* Verify result [5]. */
	if (s->verify_result != X509_V_OK) {
		if (s->verify_result < 0)
			goto err;
		if (!CBB_add_asn1(&session, &verify_result,
		    SSLASN1_VERIFY_RESULT_TAG))
			goto err;
		if (!CBB_add_asn1_uint64(&verify_result, s->verify_result))
			goto err;
	}

	/* Hostname [6]. */
	if (s->tlsext_hostname != NULL) {
		if (!CBB_add_asn1(&session, &hostname, SSLASN1_HOSTNAME_TAG))
			goto err;
		if (!CBB_add_asn1(&hostname, &value, CBS_ASN1_OCTETSTRING))
			goto err;
		if (!CBB_add_bytes(&value, s->tlsext_hostname,
		    strlen(s->tlsext_hostname)))
			goto err;
	}

	/* PSK identity hint [7]. */
	/* PSK identity [8]. */

	/* Ticket lifetime hint [9]. */
	if (s->tlsext_tick_lifetime_hint > 0) {
		if (!CBB_add_asn1(&session, &lifetime, SSLASN1_LIFETIME_TAG))
			goto err;
		if (!CBB_add_asn1_uint64(&lifetime,
		    s->tlsext_tick_lifetime_hint))
			goto err;
	}

	/* Ticket [10]. */
	if (s->tlsext_tick) {
		if (!CBB_add_asn1(&session, &ticket, SSLASN1_TICKET_TAG))
			goto err;
		if (!CBB_add_asn1(&ticket, &value, CBS_ASN1_OCTETSTRING))
			goto err;
		if (!CBB_add_bytes(&value, s->tlsext_tick, s->tlsext_ticklen))
			goto err;
	}

	/* Compression method [11]. */
	/* SRP username [12]. */

	if (!CBB_finish(&cbb, &data, &data_len))
		goto err;

	if (data_len > INT_MAX)
		goto err;

	if (pp != NULL) {
		if (*pp == NULL) {
			*pp = data;
			data = NULL;
		} else {
			memcpy(*pp, data, data_len);
		}
	}

	rv = (int)data_len;

 err:
	CBB_cleanup(&session);
	free(peer_cert_bytes);
	free(data);

	return rv;
}

SSL_SESSION *
d2i_SSL_SESSION(SSL_SESSION **a, const unsigned char **pp, long length)
{
	CBS cbs, session, cipher_suite, session_id, master_key, peer_cert;
	CBS hostname, ticket;
	uint64_t version, tls_version, stime, timeout, verify_result, lifetime;
	const unsigned char *peer_cert_bytes;
	uint16_t cipher_value;
	SSL_SESSION *s = NULL;
	size_t data_len;
	int present;

	if (a != NULL)
		s = *a;

	if (s == NULL) {
		if ((s = SSL_SESSION_new()) == NULL) {
			SSLerr(SSL_F_D2I_SSL_SESSION, ERR_R_MALLOC_FAILURE);
			goto err;
		}
	}

	CBS_init(&cbs, *pp, length);

	if (!CBS_get_asn1(&cbs, &session, CBS_ASN1_SEQUENCE))
		goto err;

	/* Session ASN1 version. */
	if (!CBS_get_asn1_uint64(&session, &version))
		goto err;
	if (version != SSL_SESSION_ASN1_VERSION)
		goto err;

	/* TLS/SSL Protocol Version. */
	if (!CBS_get_asn1_uint64(&session, &tls_version))
		goto err;
	if (tls_version > INT_MAX)
		goto err;
	s->ssl_version = (int)tls_version;

	/* Cipher suite. */
	if (!CBS_get_asn1(&session, &cipher_suite, CBS_ASN1_OCTETSTRING))
		goto err;
	if (!CBS_get_u16(&cipher_suite, &cipher_value))
		goto err;
	if (CBS_len(&cipher_suite) != 0)
		goto err;

	/* XXX - populate cipher instead? */
	s->cipher = NULL;
	s->cipher_id = SSL3_CK_ID | cipher_value;

	/* Session ID. */
	if (!CBS_get_asn1(&session, &session_id, CBS_ASN1_OCTETSTRING))
		goto err;
	if (!CBS_write_bytes(&session_id, s->session_id, sizeof(s->session_id),
	    &data_len))
		goto err;
	if (data_len > UINT_MAX)
		goto err;
	s->session_id_length = (unsigned int)data_len;

	/* Master key. */
	if (!CBS_get_asn1(&session, &master_key, CBS_ASN1_OCTETSTRING))
		goto err;
	if (!CBS_write_bytes(&master_key, s->master_key, sizeof(s->master_key),
	    &data_len))
		goto err;
	if (data_len > INT_MAX)
		goto err;
	s->master_key_length = (int)data_len;

	/* Time [1]. */
	s->time = time(NULL);
	if (!CBS_get_optional_asn1_uint64(&session, &stime, SSLASN1_TIME_TAG,
	    0))
		goto err;
	if (stime > time_max())
		goto err;
	if (stime != 0)
		s->time = (time_t)stime;

	/* Timeout [2]. */
	s->timeout = 3;
	if (!CBS_get_optional_asn1_uint64(&session, &timeout,
	    SSLASN1_TIMEOUT_TAG, 0))
		goto err;
	if (timeout > LONG_MAX)
		goto err;
	if (timeout != 0)
		s->timeout = (long)timeout;
	
	/* Peer certificate [3]. */
	X509_free(s->peer);
	s->peer = NULL;
	if (!CBS_get_optional_asn1_octet_string(&session, &peer_cert, &present,
	    SSLASN1_PEER_CERT_TAG))
		goto err;
	if (present) {
		data_len = CBS_len(&peer_cert);
		if (data_len > LONG_MAX)
			goto err;
		peer_cert_bytes = CBS_data(&peer_cert);
		if (d2i_X509(&s->peer, &peer_cert_bytes,
		    (long)data_len) == NULL)
			goto err;
	}

	/* Session ID context [4]. */
	s->sid_ctx_length = 0;
	if (!CBS_get_optional_asn1_octet_string(&session, &session_id, &present,
	    SSLASN1_SESSION_ID_CTX_TAG))
		goto err;
	if (present) {
		if (!CBS_write_bytes(&session_id, (uint8_t *)&s->sid_ctx,
		    sizeof(s->sid_ctx), &data_len))
			goto err;
		if (data_len > UINT_MAX)
			goto err;
		s->sid_ctx_length = (unsigned int)data_len;
	}

	/* Verify result [5]. */
	s->verify_result = X509_V_OK;
	if (!CBS_get_optional_asn1_uint64(&session, &verify_result,
	    SSLASN1_VERIFY_RESULT_TAG, X509_V_OK))
		goto err;
	if (verify_result > LONG_MAX)
		goto err;
	s->verify_result = (long)verify_result;

	/* Hostname [6]. */
	free(s->tlsext_hostname);
	s->tlsext_hostname = NULL;
	if (!CBS_get_optional_asn1_octet_string(&session, &hostname, &present,
	    SSLASN1_HOSTNAME_TAG))
		goto err;
	if (present) {
		if (CBS_contains_zero_byte(&hostname))
			goto err;
		if (!CBS_strdup(&hostname, &s->tlsext_hostname))
			goto err;
	}
	
	/* PSK identity hint [7]. */
	/* PSK identity [8]. */

	/* Ticket lifetime [9]. */
	s->tlsext_tick_lifetime_hint = 0;
	/* XXX - tlsext_ticklen is not yet set... */
	if (s->tlsext_ticklen > 0 && s->session_id_length > 0)
		s->tlsext_tick_lifetime_hint = -1;
	if (!CBS_get_optional_asn1_uint64(&session, &lifetime,
	    SSLASN1_LIFETIME_TAG, 0))
		goto err;
	if (lifetime > LONG_MAX)
		goto err;
	if (lifetime > 0)
		s->tlsext_tick_lifetime_hint = (long)lifetime;

	/* Ticket [10]. */
	free(s->tlsext_tick);
	s->tlsext_tick = NULL;
	if (!CBS_get_optional_asn1_octet_string(&session, &ticket, &present,
	    SSLASN1_TICKET_TAG))
		goto err;
	if (present) {
		if (!CBS_stow(&ticket, &s->tlsext_tick, &s->tlsext_ticklen))
			goto err;
	}

	/* Compression method [11]. */
	/* SRP username [12]. */

	*pp = CBS_data(&cbs);

	if (a != NULL)
		*a = s;

	return (s);

err:
	ERR_asprintf_error_data("offset=%d", (int)(CBS_data(&cbs) - *pp));

	if (s != NULL && (a == NULL || *a != s))
		SSL_SESSION_free(s);

	return (NULL);
}
