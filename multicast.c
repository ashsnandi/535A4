#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include <time.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include "multicast.h"

mcast_t *multicast_init(char *mcast_addr, int sport, int rport)
{
    mcast_t *m = (mcast_t *)calloc(1, sizeof(mcast_t));

    m->sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (m->sock < 0)
    {
        perror("socket");
        exit(1);
    }
    int optval = 1;
#ifdef SO_REUSEPORT
    setsockopt(m->sock, SOL_SOCKET, SO_REUSEPORT, &optval, sizeof(optval));
#endif
    setsockopt(m->sock, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

    unsigned char ttl = 1;
    setsockopt(m->sock, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));

    unsigned char loop = 1;
    setsockopt(m->sock, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));

    bzero((char *)&(m->addr), sizeof(m->addr));
    m->addr.sin_family = AF_INET;
    m->addr.sin_addr.s_addr = inet_addr(mcast_addr);
    m->addr.sin_port = htons(sport);
    m->addrlen = sizeof(m->addr);

    bzero((char *)&(m->my_addr), sizeof(m->my_addr));
    m->my_addr.sin_family = AF_INET;
    m->my_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    m->my_addr.sin_port = htons(rport);
    m->my_addrlen = sizeof(m->my_addr);

    m->mreq.imr_multiaddr.s_addr = inet_addr(mcast_addr);
    m->mreq.imr_interface.s_addr = htonl(INADDR_ANY);

    memset(m->fds, 0, sizeof(m->fds));
    m->fds[0].fd = m->sock;
    m->fds[0].events = POLLIN;
    m->nfds = 1;
    return m;
}

int multicast_send(mcast_t *m, void *msg, int msglen)
{
    int cnt = sendto(m->sock, msg, msglen, 0, (struct sockaddr *)&(m->addr), m->addrlen);
    if (cnt < 0)
    {
        perror("sendto:");
        exit(1);
    }

    struct sockaddr_in local_addr;
    memset(&local_addr, 0, sizeof(local_addr));
    local_addr.sin_family = AF_INET;
    local_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    local_addr.sin_port = m->addr.sin_port;

    sendto(m->sock, msg, msglen, 0, (struct sockaddr *)&local_addr, sizeof(local_addr));

    return cnt;
}

void multicast_setup_recv(mcast_t *m)
{
    if (bind(m->sock, (struct sockaddr *)&(m->my_addr), sizeof(m->my_addr)) < 0)
    {
        perror("bind");
        exit(1);
    }
    if (setsockopt(m->sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &(m->mreq), sizeof(m->mreq)) < 0)
    {
        perror("setsockopt mreq");
        exit(1);
    }

    struct ip_mreq loopback_mreq = m->mreq;
    loopback_mreq.imr_interface.s_addr = inet_addr("127.0.0.1");
    if (setsockopt(m->sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &loopback_mreq, sizeof(loopback_mreq)) < 0)
    {
        if (errno != EADDRINUSE)
        {
            perror("setsockopt loopback mreq");
        }
    }
}

int multicast_receive(mcast_t *m, void *buf, int bufsize)
{
    int cnt = recvfrom(m->sock, buf, bufsize, 0, (struct sockaddr *)&(m->my_addr), &(m->my_addrlen));
    if (cnt < 0)
    {
        perror("recvfrom");
        exit(1);
    }
    return cnt;
}

int multicast_check_receive(mcast_t *m)
{

    int rc = poll(m->fds, m->nfds, 1000);
    if (rc < 0) {
        perror("poll");
        exit(1);
    }
    return rc;
}

void multicast_destroy(mcast_t *m)
{
    close(m->sock);
    free(m);
}
