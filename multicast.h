#ifndef __MULTICAST_H__
#define __MULTICAST_H__

#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <arpa/inet.h>
#include <sys/poll.h>

typedef struct _mcast_t
{
    struct sockaddr_in addr;
    struct sockaddr_in my_addr;
    unsigned int addrlen;
    unsigned int my_addrlen;
    int sock;
    struct ip_mreq mreq;
    struct pollfd fds[2];
    int nfds;
} mcast_t;

mcast_t *multicast_init(char *mcast_addr, int sport, int rport);
int multicast_send(mcast_t *m, void *msg, int msglen);
void multicast_setup_recv(mcast_t *m);
int multicast_receive(mcast_t *m, void *buf, int bufsize);
int multicast_check_receive(mcast_t *m);
void multicast_destroy(mcast_t *m);

#endif