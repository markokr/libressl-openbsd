#	$OpenBSD: Makefile,v 1.2 2014/06/01 11:11:45 miod Exp $

PROG=	bftest
LDADD=	-lcrypto
DPADD=	${LIBCRYPTO}
WARNINGS=	Yes
CFLAGS+=	-DLIBRESSL_INTERNAL -DLIBRESSL_INTERNAL -Werror

.include <bsd.regress.mk>
