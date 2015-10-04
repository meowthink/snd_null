# $FreeBSD$

.PATH: ${.CURDIR}/../../../../dev/sound

KMOD=	snd_null
SRCS=	device_if.h bus_if.h
SRCS+=	null.c

.include <bsd.kmod.mk>
