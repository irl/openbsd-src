/*	$OpenBSD$ */

/*
 * Copyright (c) 2006, 2007, 2008 Marc Balmer <mbalmer@openbsd.org>
 * Copyright (c) 2019 Iain R. Learmonth <irl@fsfe.org>
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

/*
 *  TODO:
 *
 *  - Set the interface as running in kissopen (and not in kissclose)
 *  - Packing incoming frames into mbufs and pass to interface specific handler
 *  - Fix double FEND errors that seem to be occuring
 */

/* A tty line discipline to communicate with a KISS terminal node controller. */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/malloc.h>
#include <sys/tty.h>
#include <sys/conf.h>
#include <sys/proc.h>
#include <sys/smr.h>

#include <net/if.h>
#include <net/if_dl.h>
#include <net/if_types.h>

#include <net/if_media.h>

#include <netinet/in.h>
#include <netinet/if_ether.h>

#include <net/if_kiss.h>

LIST_HEAD(, kiss_softc) kiss_softc_list;

static void kissreturn(struct tty *tp);
static void kissoutputdata(struct tty *tp, struct mbuf *m);

/* take over a tty and match with a kiss net interface. */
int
kissopen(dev_t dev, struct tty *tp, struct proc *p)
{
	struct kiss_softc *sc;
	int error;

	if (tp->t_line == KISSDISC)
		return (ENODEV);
	if ((error = suser(p)) != 0)
		return (error);
	if ((sc = kissalloc(p->p_p->ps_pid)) == NULL) {
		/* most likely there are no kissN interfaces */
		return ENXIO;
	}

	tp->t_sc = (caddr_t)sc;
	sc->sc_devp = (void*)tp;

	error = linesw[TTYDISC].l_open(dev, tp, p);
	// TODO: do we really need to call this?
	if (error) {
		tp->t_sc = NULL;
	}
	return (error);
}

/* clean up and set the tty back to termios. */
int
kissclose(struct tty *tp, int flags, struct proc *p)
{
	struct kiss_softc *sc = (struct kiss_softc *)tp->t_sc;
	sc->sc_devp = NULL;
	//free(sc, M_DEVBUF, sizeof(*sc));
	// TODO: the sc is owned by if_kiss.c, make sure we free it there
	kissreturn(tp); /* return TNC from KISS mode */
	tp->t_line = TTYDISC;	/* switch back to termios */
	tp->t_sc = NULL;
	return (linesw[TTYDISC].l_close(tp, flags, p));
	// TODO: do we really need to call this?
}

/* send command to tnc to exit kiss mode. */
static void
kissreturn(struct tty *tp)
{
	putc(KISSCMD_RETURN, &tp->t_outq);
}

/* tty input interrupt handler. collects kiss frames. */
int
kissinput(int c, struct tty *tp)
{
	struct kiss_softc *np = (struct kiss_softc *)tp->t_sc;

	if (np->newfrm) {
		int port = (c & 0xF0) >> 4;
		int command = c & 0x0F;

		np->newfrm = np->pos = 0;

		switch (command) {
		case KISSFEND:
			break;
		case KISSCMD_DATA:
			printf("data command received on port %d\n", port);
			np->port = port;
			np->data = 1;
			return 0;
		default:
			printf("unrecognised command received on port %d\n", port);
			return 0;
		}
	}

	if (np->fesc) {
		int escaped = 0;

		switch (c) {
		case KISSTFESC:
			escaped = KISSFESC;
			break;
		case KISSTFEND:
			escaped = KISSFEND;
			break;
		}

		if (escaped && np->pos < (KISSMAX - 1))
			np->cbuf[np->pos++] = escaped;

		/* TODO: handle invalid escape sequence */

		np->fesc = 0;
		return 0;
	}

	switch (c) {
	case KISSFEND:
		/* TODO: flush current frame into something */
		printf("got start of new frame, previous frame was %d bytes\n",
			np->pos);
		np->newfrm = 1;
		np->data = 0;
		np->fesc = 0;
		break;
	case KISSFESC:
		np->fesc = 1;
		break;
	default:
                printf(" input %d ", c);
		if (np->data && np->pos < (KISSMAX - 1))
			np->cbuf[np->pos++] = c;
		/* TODO: record error, frame too long */
		break;
	}

	return 0;
}

/* output a kiss frame with packet data from an mbuf. */
void
kissoutput(struct tty *tp, struct mbuf *m) {
	int s;

	smr_read_enter(); // TODO: what is this protecting?
	s = spltty();
	printf("kiss_enqueue packet with %d bytes\n", m->m_pkthdr.len);
	// transmit function to be broken out
	putc(KISSFEND, &tp->t_outq);
	putc(KISSCMD_DATA, &tp->t_outq); /* implicitly port 0 */
	for (;;) {
		kissoutputdata(tp, m);
		if ((m = m->m_next) == NULL)
			break;
	}
	putc(KISSFEND, &tp->t_outq);
	splx(s);
	ttstart(tp);
	smr_read_leave();

}

/* output data from a single packet, escaping as required. */
static void
kissoutputdata(struct tty *tp, struct mbuf *m)
{
	u_char *start, *stop, *cp;
	int len;

	start = mtod(m, u_char *);
	len = m->m_len;
	stop = start + len;
	for (cp = start; cp < stop; cp++) {
		switch(*cp) {
		case KISSFESC:
			putc(KISSTFESC, &tp->t_outq);
			break;
		case KISSFEND:
			putc(KISSTFEND, &tp->t_outq);
			break;
		default:
			putc(*cp, &tp->t_outq);
			break;
		}
	}
}


/* allocate the first available network interface. */
struct kiss_softc *
kissalloc(pid_t pid)
{
	struct kiss_softc *sc;

	NET_LOCK();
	LIST_FOREACH(sc, &kiss_softc_list, sc_list) {
		if (sc->sc_xfer == pid) {
			sc->sc_xfer = 0;
			NET_UNLOCK();
			return sc;
		}
	}
	LIST_FOREACH(sc, &kiss_softc_list, sc_list) {
		if (sc->sc_devp == NULL)
			break;
	}
	NET_UNLOCK();
	if (sc == NULL)
		return NULL;

	return sc;
}

/* register a kiss interface with the line discipline. */
void
kissregister(struct kiss_softc *sc)
{
	NET_LOCK();
	LIST_INSERT_HEAD(&kiss_softc_list, sc, sc_list);
	NET_UNLOCK();
}
