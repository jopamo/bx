/*
 * test-listen: Test program to listen to a TCP port
 *
 * Copyright (C)     -2025 Craig Small <csmall@dropbear.xyz>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <err.h>

#define LISTEN_BACKLOG 10

int main(int argc, char *argv[])
{
    int skt;
    struct sockaddr_in sin, sname;
    socklen_t sname_len;

    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    memset(&sname, 0, sizeof(sname));
    sname_len = sizeof(sname);

    if ( (skt = socket(AF_INET, SOCK_STREAM, 0)) < 0)
        err(EXIT_FAILURE, "Creating socket");

    if (bind( skt, (struct sockaddr*)&sin, sizeof(sin)) < 0)
        err(EXIT_FAILURE, "Binding socket");

    if (listen(skt, LISTEN_BACKLOG) < 0)
        err(EXIT_FAILURE, "Listen to socket");

    if (getsockname(skt, (struct sockaddr*)&sname, &sname_len) < 0)
        err(EXIT_FAILURE, "get socket name");
    printf("Success: port is %d\n", ntohs(sname.sin_port));

    while(1)
    {
        int cskt;
        struct sockaddr_in peer_sin;
        socklen_t peer_sin_size = sizeof peer_sin;

        if ( (cskt = accept(skt, (struct sockaddr*) &peer_sin, &peer_sin_size)) < 0)
            err(EXIT_FAILURE, "accept socket");
        close(cskt);
    }
    return EXIT_SUCCESS;
}
