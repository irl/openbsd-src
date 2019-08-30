
/* Structures and declarations for KISS. */

#ifndef _NET_IF_KISS_H_
#define _NET_IF_KISS_H_

/* TODO: Some of these may need to be public, some private, split it up? */

#ifdef KISS_DEBUG
#define DPRINTFN(n, x)	do { if (kissdebug > (n)) printf x; } while (0)
int kissdebug = 0;
#else
#define DPRINTFN(n, x)
#endif
#define DPRINTF(x)	DPRINTFN(0, x)

#define KISSMAX		1512 /* chosen arbitrarily; TODO: do better */

#define KISSFEND	0xC0 /* frame end */
#define KISSFESC	0xDB /* frame escape */
#define KISSTFEND	0xDC /* transposed frame end */
#define KISSTFESC	0xDD /* transposed frame escape */

#define KISSCMD_DATA		0x00
#define KISSCMD_TXDELAY		0x01 /* in 10 ms units */
#define KISSCMD_PERSIST		0x02 /* used for CSMA */
#define KISSCMD_SLOTTIME	0x03 /* in 10 ms units */
#define KISSCMD_TXTAIL		0x04 /* in 10 ms units */
#define KISSCMD_FULLDUPLEX	0x05 /* 0=half, anything else=full */
#define KISSCMD_SETHARDWARE	0x06 /* this is not implemented anywhere here */
#define KISSCMD_RETURN		0xFF /* on all ports */

struct kiss_softc {
	/* network interface */
	struct arpcom		sc_ac;
#define sc_if			sc_ac.ac_if
	unsigned int		sc_dead;
	unsigned int		sc_promisc;
	struct ifmedia		sc_media;

	/* line discipline */
	void			*sc_devp; /* tty */
	char			cbuf[KISSMAX];	/* receive buffer */
	int			pos; /* position in rcv buffer */
	int			newfrm; /* new frame started; no command yet */
	int			data; /* data command was recieved */
	int			port; /* port index of current recvd data */
	int			fesc; /* fesc was recvd, next byte transposed */

	pid_t	sc_xfer; /* used in transferring unit */
	LIST_ENTRY(kiss_softc) sc_list; /* all kiss interfaces */
};

struct kiss_softc *kissalloc(pid_t pid);
void kissoutput(struct tty *tp, struct mbuf *m);
void kissregister(struct kiss_softc *sc);

#endif /* _NET_IF_KISS_H_ */
