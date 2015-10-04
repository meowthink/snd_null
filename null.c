/*-
 * Copyright (c) 2007-2009 Ariff Abdullah <ariff@FreeBSD.org>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHERIN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THEPOSSIBILITY OF
 * SUCH DAMAGE.
 */

#ifdef HAVE_KERNEL_OPTION_HEADERS
#include "opt_snd.h"
#endif

#include <dev/sound/pcm/sound.h>
#include <sys/sysctl.h>
#include "mixer_if.h"

SND_DECLARE_FILE("$FreeBSD$");

#define	SNDNULL_DESC		"NULL Audio"

#define	SNDNULL_RATE_MIN	8000
#define	SNDNULL_RATE_MAX	192000

#define	SNDNULL_RATE_DEFAULT	51200
#define	SNDNULL_FMT_DEFAULT	SND_FORMAT(AFMT_S16_LE, 2, 0)
#define	SNDNULL_FMTSTR_DEFAULT	"s16le:2.0"

#define	SNDNULL_NPCHAN		1
#define	SNDNULL_NRCHAN		1
#define	SNDNULL_MAXCHAN		(SNDNULL_NPCHAN + SNDNULL_NRCHAN)

#define	SNDNULL_BUFSZ_MIN	4096
#define	SNDNULL_BUFSZ_MAX	65536
#define	SNDNULL_BUFSZ_DEFAULT	4096

#define	SNDNULL_BLKCNT_MIN	2
#define	SNDNULL_BLKCNT_MAX	512
#define	SNDNULL_BLKCNT_DEFAULT	SNDNULL_BLKCNT_MIN

#define	SNDNULL_LOCK(sc)	snd_mtxlock((sc)->lock)
#define	SNDNULL_UNLOCK(sc)	snd_mtxunlock((sc)->lock)

struct sndnull_info;

struct sndnull_chinfo {
	struct snd_dbuf *buffer;
	struct pcm_channel *channel;
	struct pcmchan_caps *caps;
	struct sndnull_info *parent;
	uint32_t ptr, intrcnt;
	int dir, active;
};

struct sndnull_info {
	device_t dev;
	struct sndnull_chinfo ch[SNDNULL_MAXCHAN];
	struct pcmchan_caps caps;
	uint32_t bufsz;
	uint32_t blkcnt;
	uint32_t fmtlist[2];
	struct mtx *lock;
	uint8_t *ringbuffer;
	int chnum;

	struct callout poll_timer;
	int poll_ticks, polling;
};

static void *
sndnull_chan_init(kobj_t obj, void *devinfo, struct snd_dbuf *b,
    struct pcm_channel *c, int dir)
{
	struct sndnull_info *sc = devinfo;
	struct sndnull_chinfo *ch;

	SNDNULL_LOCK(sc);

	ch = &sc->ch[sc->chnum++];
	ch->buffer = b;
	ch->parent = sc;
	ch->channel = c;
	ch->dir = dir;
	ch->caps = &sc->caps;

	SNDNULL_UNLOCK(sc);

	if (sndbuf_setup(ch->buffer, sc->ringbuffer, sc->bufsz) == -1)
		return (NULL);

	return (ch);
}

static int
sndnull_chan_setformat(kobj_t obj, void *data, uint32_t format)
{
	struct sndnull_chinfo *ch = data;

	if (ch->caps->fmtlist[0] != format)
		return (EINVAL);

	return (0);
}

static uint32_t
sndnull_chan_setspeed(kobj_t obj, void *data, uint32_t spd)
{
	struct sndnull_chinfo *ch = data;

	if (spd < ch->caps->minspeed)
		spd = ch->caps->minspeed;
	if (spd > ch->caps->maxspeed)
		spd = ch->caps->maxspeed;

	return (spd);
}

static int
sndnull_chan_setfragments(kobj_t obj, void *data, uint32_t blksz,
    uint32_t blkcnt)
{
	struct sndnull_chinfo *ch = data;
	struct sndnull_info *sc = ch->parent;

	blkcnt = sc->blkcnt;
	blksz = sndbuf_getmaxsize(ch->buffer) / blkcnt;
	blksz -= blksz % sndbuf_getalign(ch->buffer);

	if ((sndbuf_getblksz(ch->buffer) != blksz ||
	    sndbuf_getblkcnt(ch->buffer) != blkcnt) &&
	    sndbuf_resize(ch->buffer, blkcnt, blksz) != 0)
		device_printf(sc->dev, "%s: failed blksz=%u blkcnt=%u\n",
		    __func__, blksz, blkcnt);

	return (0);
}

static uint32_t
sndnull_chan_setblocksize(kobj_t obj, void *data, uint32_t blksz)
{
	struct sndnull_chinfo *ch = data;
	struct sndnull_info *sc = ch->parent;

	sndnull_chan_setfragments(obj, data, blksz, sc->blkcnt);

	return (sndbuf_getblksz(ch->buffer));
}

#define	SNDNULL_CHAN_ACTIVE(ch)		((ch)->active != 0)

static __inline int
sndnull_anychan_active(struct sndnull_info *sc)
{
	int i;

	for (i = 0; i < sc->chnum; i++) {
		if (SNDNULL_CHAN_ACTIVE(&sc->ch[i]))
			return (1);
	}

	return (0);
}

static void
sndnull_poll_callback(void *arg)
{
	struct sndnull_info *sc = arg;
	struct sndnull_chinfo *ch;
	int i;

	if (sc == NULL)
		return;

	SNDNULL_LOCK(sc);

	if (!sndnull_anychan_active(sc)) {
		SNDNULL_UNLOCK(sc);
		return;
	}

	for (i = 0; i < sc->chnum; i++) {
		ch = &sc->ch[i];
		if (SNDNULL_CHAN_ACTIVE(ch)) {
			ch->ptr += sndbuf_getblksz(ch->buffer);
			ch->ptr %= sndbuf_getsize(ch->buffer);
			ch->intrcnt += 1;
			SNDNULL_UNLOCK(sc);
			chn_intr(ch->channel);
			SNDNULL_LOCK(sc);
		}
	}

	callout_reset(&sc->poll_timer, sc->poll_ticks,
	    sndnull_poll_callback, sc);

	SNDNULL_UNLOCK(sc);
}

static int
sndnull_chan_trigger(kobj_t obj, void *data, int go)
{
	struct sndnull_chinfo *ch = data;
	struct sndnull_info *sc = ch->parent;
	int pollticks;

	if (!PCMTRIG_COMMON(go))
		return (0);

	SNDNULL_LOCK(sc);

	switch (go) {
	case PCMTRIG_START:
		if (!sndnull_anychan_active(sc)) {
			pollticks = ((uint64_t)hz *
			    sndbuf_getblksz(ch->buffer)) /
			    ((uint64_t)sndbuf_getalign(ch->buffer) *
			    sndbuf_getspd(ch->buffer));
device_printf(sc->dev,"PCMTRIG_START: %d * %d / %d / %d = %d\n",
hz,sndbuf_getblksz(ch->buffer),sndbuf_getalign(ch->buffer), sndbuf_getspd(ch->buffer), pollticks);
			if (pollticks < 1)
				pollticks = 1;
			sc->poll_ticks = pollticks;
			callout_reset(&sc->poll_timer, 1,
			    sndnull_poll_callback, sc);
			if (bootverbose)
				device_printf(sc->dev,
				    "PCMTRIG_START: pollticks=%d\n",
				    pollticks);
		}
		if (ch->dir == PCMDIR_REC)
			memset(sc->ringbuffer, sndbuf_zerodata(
			    sndbuf_getfmt(ch->buffer)),
			    sndbuf_getmaxsize(ch->buffer));
		ch->ptr = 0;
		ch->intrcnt = 0;
		ch->active = 1;
		break;
	case PCMTRIG_STOP:
	case PCMTRIG_ABORT:
		ch->active = 0;
		if (!sndnull_anychan_active(sc))
			callout_stop(&sc->poll_timer);
		if (ch->dir == PCMDIR_PLAY)
			memset(sc->ringbuffer, sndbuf_zerodata(
			    sndbuf_getfmt(ch->buffer)),
			    sndbuf_getmaxsize(ch->buffer));
		break;
	default:
		break;
	}

	SNDNULL_UNLOCK(sc);

	return (0);
}

static uint32_t
sndnull_chan_getptr(kobj_t obj, void *data)
{
	struct sndnull_chinfo *ch = data;
	struct sndnull_info *sc = ch->parent;
	uint32_t ptr;

	SNDNULL_LOCK(sc);
	ptr = (SNDNULL_CHAN_ACTIVE(ch)) ? ch->ptr : 0;
	SNDNULL_UNLOCK(sc);

	return (ptr);
}

static struct pcmchan_caps *
sndnull_chan_getcaps(kobj_t obj, void *data)
{

	return (((struct sndnull_chinfo *)data)->caps);
}

static kobj_method_t sndnull_chan_methods[] = {
	KOBJMETHOD(channel_init,		sndnull_chan_init),
	KOBJMETHOD(channel_setformat,		sndnull_chan_setformat),
	KOBJMETHOD(channel_setspeed,		sndnull_chan_setspeed),
	KOBJMETHOD(channel_setblocksize,	sndnull_chan_setblocksize),
	KOBJMETHOD(channel_setfragments,	sndnull_chan_setfragments),
	KOBJMETHOD(channel_trigger,		sndnull_chan_trigger),
	KOBJMETHOD(channel_getptr,		sndnull_chan_getptr),
	KOBJMETHOD(channel_getcaps,		sndnull_chan_getcaps),
	KOBJMETHOD_END
};
CHANNEL_DECLARE(sndnull_chan);

static const struct {
	int ctl;
	int rec;
} sndnull_mixer_ctls[SOUND_MIXER_NRDEVICES] = {
	[SOUND_MIXER_VOLUME]	= { 1, 0 },
	[SOUND_MIXER_BASS]	= { 1, 0 },
	[SOUND_MIXER_TREBLE]	= { 1, 0 },
	[SOUND_MIXER_SYNTH]	= { 1, 1 },
	[SOUND_MIXER_PCM]	= { 1, 1 },
	[SOUND_MIXER_SPEAKER]	= { 1, 0 },
	[SOUND_MIXER_LINE]	= { 1, 1 },
	[SOUND_MIXER_MIC]	= { 1, 1 },
	[SOUND_MIXER_CD]	= { 1, 1 },
	[SOUND_MIXER_IMIX]	= { 1, 1 },
	[SOUND_MIXER_RECLEV]	= { 1, 0 },
};

static int
sndnull_mixer_init(struct snd_mixer *m)
{
	uint32_t mask, recmask;
	int i;

	mask = 0;
	recmask = 0;
	for (i = 0; i < SOUND_MIXER_NRDEVICES; i++) {
		if (sndnull_mixer_ctls[i].ctl != 0)
			mask |= 1 << i;
		if (sndnull_mixer_ctls[i].rec != 0)
			recmask |= 1 << i;
	}

	mix_setdevs(m, mask);
	mix_setrecdevs(m, recmask);

	return (0);
}

static int
sndnull_mixer_set(struct snd_mixer *m, unsigned dev, unsigned left,
    unsigned right)
{

	if (!(dev < SOUND_MIXER_NRDEVICES && sndnull_mixer_ctls[dev].ctl != 0))
		return (-1);

	return (left | (right << 8));
}

static uint32_t
sndnull_mixer_setrecsrc(struct snd_mixer *m, uint32_t src)
{
	uint32_t recsrc;
	int i;

	recsrc = src;

	if (recsrc & SOUND_MASK_IMIX)
		recsrc &= SOUND_MASK_IMIX;
	else {
		for (i = 0; i < SOUND_MIXER_NRDEVICES; i++) {
			if (sndnull_mixer_ctls[i].rec == 0)
				recsrc &= ~(1 << i);
		}
	}

	return (recsrc);
}

static kobj_method_t sndnull_mixer_methods[] = {
	KOBJMETHOD(mixer_init,		sndnull_mixer_init),
	KOBJMETHOD(mixer_set,		sndnull_mixer_set),
	KOBJMETHOD(mixer_setrecsrc,	sndnull_mixer_setrecsrc),
	KOBJMETHOD_END
};
MIXER_DECLARE(sndnull_mixer);

static int
sysctl_sndnull_rate(SYSCTL_HANDLER_ARGS)
{
	struct sndnull_info *sc;
	device_t dev;
	int err, val;

	dev = oidp->oid_arg1;

	sc = pcm_getdevinfo(dev);
	if (sc == NULL)
		return (EINVAL);

	SNDNULL_LOCK(sc);
	val = sc->caps.maxspeed;
	SNDNULL_UNLOCK(sc);

	err = sysctl_handle_int(oidp, &val, 0, req);

	if (err != 0 || req->newptr == NULL)
		return (err);

	if (val < SNDNULL_RATE_MIN)
		val = SNDNULL_RATE_MIN;
	if (val > SNDNULL_RATE_MAX)
		val = SNDNULL_RATE_MAX;

	SNDNULL_LOCK(sc);
	if (sndnull_anychan_active(sc))
		err = EBUSY;
	else {
		sc->caps.minspeed = (uint32_t)val;
		sc->caps.maxspeed = sc->caps.minspeed;
	}
	SNDNULL_UNLOCK(sc);

	return (err);
}

static int
sysctl_sndnull_format(SYSCTL_HANDLER_ARGS)
{
	struct sndnull_info *sc;
	device_t dev;
	int err;
	char fmtstr[AFMTSTR_LEN];
	uint32_t fmt;

	dev = oidp->oid_arg1;

	sc = pcm_getdevinfo(dev);
	if (sc == NULL)
		return (EINVAL);

	SNDNULL_LOCK(sc);
	fmt = sc->fmtlist[0];
	if (snd_afmt2str(fmt, fmtstr, sizeof(fmtstr)) != fmt)
		strlcpy(fmtstr, SNDNULL_FMTSTR_DEFAULT, sizeof(fmtstr));
	SNDNULL_UNLOCK(sc);

	err = sysctl_handle_string(oidp, fmtstr, sizeof(fmtstr), req);

	if (err != 0 || req->newptr == NULL)
		return (err);

	fmt = snd_str2afmt(fmtstr);
	if (fmt == 0)
		return (EINVAL);

	SNDNULL_LOCK(sc);
	if (fmt != sc->fmtlist[0]) {
		if (sndnull_anychan_active(sc))
			err = EBUSY;
		else
			sc->fmtlist[0] = fmt;
	}
	SNDNULL_UNLOCK(sc);

	return (err);
}

static device_t sndnull_dev = NULL;

static void
sndnull_dev_identify(driver_t *driver, device_t parent)
{

	if (sndnull_dev == NULL)
		sndnull_dev = BUS_ADD_CHILD(parent, 0, "pcm", -1);
}

static int
sndnull_dev_probe(device_t dev)
{

	if (dev != NULL && dev == sndnull_dev) {
		device_set_desc(dev, SNDNULL_DESC);
		return (BUS_PROBE_DEFAULT);
	}

	return (ENXIO);
}

static int
sndnull_dev_attach(device_t dev)
{
	struct sndnull_info *sc;
	char status[SND_STATUSLEN];
	int i;

	sc = malloc(sizeof(*sc), M_DEVBUF, M_WAITOK | M_ZERO);

	sc->lock = snd_mtxcreate(device_get_nameunit(dev), "snd_null softc");
	sc->dev = dev;

	callout_init(&sc->poll_timer, CALLOUT_MPSAFE);
	sc->poll_ticks = 1;

	sc->caps.minspeed = SNDNULL_RATE_DEFAULT;
	sc->caps.maxspeed = SNDNULL_RATE_DEFAULT;
	sc->fmtlist[0] = SNDNULL_FMT_DEFAULT;
	sc->fmtlist[1] = 0;
	sc->caps.fmtlist = sc->fmtlist;

	sc->bufsz = pcm_getbuffersize(dev, SNDNULL_BUFSZ_MIN,
	    SNDNULL_BUFSZ_DEFAULT, SNDNULL_BUFSZ_MAX);
	sc->blkcnt = SNDNULL_BLKCNT_DEFAULT;

	sc->ringbuffer = malloc(sc->bufsz, M_DEVBUF, M_WAITOK | M_ZERO);

	if (mixer_init(dev, &sndnull_mixer_class, sc) != 0)
		device_printf(dev, "mixer_init() failed\n");

	if (pcm_register(dev, sc, SNDNULL_NPCHAN, SNDNULL_NRCHAN))
		return (ENXIO);

	for (i = 0; i < SNDNULL_NPCHAN; i++)
		pcm_addchan(dev, PCMDIR_PLAY, &sndnull_chan_class, sc);
	for (i = 0; i < SNDNULL_NRCHAN; i++)
		pcm_addchan(dev, PCMDIR_REC, &sndnull_chan_class, sc);

	snprintf(status, SND_STATUSLEN, "at %s %s",
	    device_get_nameunit(device_get_parent(dev)),
	    PCM_KLDSTRING(snd_null));

	pcm_setflags(dev, pcm_getflags(dev) | SD_F_MPSAFE);
	pcm_setstatus(dev, status);

	SYSCTL_ADD_PROC(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO,
	    "rate", CTLTYPE_INT | CTLFLAG_RW, dev, sizeof(dev),
	    sysctl_sndnull_rate, "I", "runtime rate");
	SYSCTL_ADD_PROC(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO,
	    "format", CTLTYPE_STRING | CTLFLAG_RW, dev, sizeof(dev),
	    sysctl_sndnull_format, "A", "runtime format");

	return (0);
}

static void
sndnull_release_resources(struct sndnull_info *sc)
{

	if (sc == NULL)
		return;
	if (sc->chnum != 0) {
		SNDNULL_LOCK(sc);
		callout_stop(&sc->poll_timer);
		SNDNULL_UNLOCK(sc);
		callout_drain(&sc->poll_timer);
	}
	if (sc->ringbuffer != NULL) {
		free(sc->ringbuffer, M_DEVBUF);
		sc->ringbuffer = NULL;
	}
	if (sc->lock != NULL) {
		snd_mtxfree(sc->lock);
		sc->lock = NULL;
	}
	free(sc, M_DEVBUF);
}

static int
sndnull_dev_detach(device_t dev)
{
	struct sndnull_info *sc;
	int err;

	sc = pcm_getdevinfo(dev);
	if (sc != NULL) {
		err = pcm_unregister(dev);
		if (err != 0)
			return (err);
		sndnull_release_resources(sc);
	}

	return (0);
}

static device_method_t sndnull_methods[] = {
	DEVMETHOD(device_identify,	sndnull_dev_identify),
	DEVMETHOD(device_probe,		sndnull_dev_probe),
	DEVMETHOD(device_attach,	sndnull_dev_attach),
	DEVMETHOD(device_detach,	sndnull_dev_detach),
	{ 0, 0 }
};

static driver_t sndnull_driver = {
	"pcm",
	sndnull_methods,
	PCM_SOFTC_SIZE,
};

static int
sndnull_modevent(module_t mod, int type, void *data)
{

	switch (type) {
	case MOD_UNLOAD:
		if (sndnull_dev != NULL)
			device_delete_child(device_get_parent(sndnull_dev),
			    sndnull_dev);
		sndnull_dev = NULL;
	case MOD_LOAD:
		return (0);
		break;
	default:
		break;
	}

	return (ENOTSUP);
}

DRIVER_MODULE(snd_null, nexus, sndnull_driver, pcm_devclass, sndnull_modevent,
    0);
MODULE_DEPEND(snd_null, sound, SOUND_MINVER, SOUND_PREFVER, SOUND_MAXVER);
MODULE_VERSION(snd_null, 1);

