#	$OpenBSD: Makefile,v 1.2 2014/07/08 15:53:52 jsing Exp $

.include "../../Makefile.inc"

PROG=	bntest
LDADD=	${CRYPTO_INT}
DPADD=	${LIBCRYPTO}
WARNINGS=	Yes
CFLAGS+=	-DLIBRESSL_INTERNAL -Werror

.include <bsd.regress.mk>
