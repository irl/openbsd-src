/*
 * iclcd - intentification compliant with license conditions daemon
 *
 * Written by Iain R. Learmonth <irl@fsfe.org> for the public domain.
 */

#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <syslog.h>
#include <stdarg.h>
#include <signal.h>

#include <sys/times.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <net/bpf.h>
#include <sys/socket.h>
#include <net/if.h>

__dead void
fatal(char* msg)
{
	syslog(LOG_DAEMON | LOG_EMERG,
	    "iclcd hit fatal error; you might want to turn off your radio");
	syslog(LOG_DAEMON | LOG_ERR,
	    "%s", msg);
	exit(1);
}

__dead void
usage(void)
{
	extern char *__progname;

	fprintf(stderr, "usage: %s [-D] [-i interface] [-p period]\n",
	    __progname);
	exit(1);
}

void
signal_handler(int sig)
{
	switch(sig) {
	case SIGHUP:
		syslog(LOG_DAEMON | LOG_INFO, "caught hangup signal");
		break;
	case SIGTERM:
		syslog(LOG_DAEMON | LOG_EMERG,
		    "caught terminate signal, shutting down");
		exit(0);
		break;
	}
}

char *
mycallsign()
{
	FILE    *mcp;
	char    *call, *nl;
	size_t  callsize = 0;
	ssize_t calllen;

	call = NULL;

	if ((mcp = fopen("/etc/mycallsign", "r")) == NULL)
		fatal("could not open /etc/mycallsign");
	if ((calllen = getline(&call, &callsize, mcp)) != -1) {
		if ((nl = strchr(call, '\n')) != NULL)
			nl[0] = '\0';
		return call;
	}
	fatal("could not read callsign from /etc/mycallsign");
}

int
composeframe(char *buf, char *call)
{
	int framelen, calllen;

	const char header[] = {
	  0xff, 0xff, 0xff, 0xff, 0xff, 0xff, // broadcast destination
	  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // source filled by bpf
	  0x44, 0x00,                         // ethertype
	  0x00                                // version
	};

	memcpy(buf, header, 15);
	framelen = 15; // prefix is 15 bytes
	calllen = strlcpy(&buf[14], call, 25);
	if (calllen > 25) {
		fatal("callsign in /etc/mycallsign too long");
	}
	framelen = framelen + calllen - 1; // remove the NUL from end of call
	return framelen;
}

int
openbpf(char *interface)
{
	char buf[11];
	int bpf, iflen;
	struct ifreq bound_if;
	
	for (int i = 0; i < 99; i++)
	{
		sprintf(buf, "/dev/bpf%i", i);
		bpf = open(buf, O_RDWR);
	
		if (bpf != -1)
			break;
	}

	iflen = strlen(interface);
	if (strlcpy(bound_if.ifr_name, interface, sizeof(bound_if.ifr_name))
	    < iflen)
		fatal("interface name too long");

	if(ioctl(bpf, BIOCSETIF, &bound_if) > 0)
		return -1;

	return bpf;
}

void
daemonize()
{
	int i;
	// TODO: should this use daemon(3) instead? maybe it cuts this down
	if (getppid() == 1)
		return; /* already a daemon */
	i = fork();
	if (i<0)
		exit(1); /* fork error */
	if (i>0)
		exit(0); /* parent exits */
	/* child (daemon) continues */
	setsid(); /* obtain a new process group */
	for (i = getdtablesize(); i >= 0; --i)
		close(i); /* close all descriptors */
	i = open("/dev/null", O_RDWR);
	dup(i);
	dup(i); /* handle standard I/O */
	signal(SIGCHLD, SIG_IGN); /* ignore child */
	signal(SIGTSTP, SIG_IGN); /* ignore tty signals */
	signal(SIGTTOU, SIG_IGN);
	signal(SIGTTIN, SIG_IGN);
	signal(SIGHUP, signal_handler); /* catch hangup signal */
	signal(SIGTERM, signal_handler); /* catch kill signal */
}

int
main(int argc, char **argv)
{
	int bpf, daemon, period;
	char ch, *interface;

	/* option defaults */
	daemon = 1;
	period = 60;
	interface = "kiss0"; // default

	while ((ch = getopt(argc, argv, "i:p:D")) != -1) {
		switch (ch) {
		case 'i':
			interface = optarg;
			break;
		case 'p':
			period = atoi(optarg);
			break;
		case 'D':
			daemon = 0;
			break;
		default:
			usage();
			break;
		}
	}
	argc -= optind;
	argv += optind;

	/* Check for root privileges. */
	if (geteuid())
		fatal("need root privileges");

	if (daemon)
		daemonize();

	if ((bpf = openbpf(interface)) == -1)
		fatal("failed to open bpf interface");

	char framebuf[40];
	char *call = mycallsign();
	int framelen = composeframe(framebuf, call);
	int unslept;

	syslog(LOG_DAEMON | LOG_INFO,
	    "started up (interface %s, callsign: %s)", interface, call);

	for (;;) {
		if (write(bpf, &framebuf, framelen) != framelen) {
			syslog(LOG_DAEMON | LOG_EMERG,
			    "failed to send ident frame, might want to unplug");
			syslog(LOG_DAEMON | LOG_ERR,
			    "failure reason: %m");
		}
		unslept = period;
		while (unslept > 0)
			unslept = sleep(unslept);
	}
}
