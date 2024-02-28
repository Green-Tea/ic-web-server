#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include "pcsa_net.h"

#define LISTEN_QUEUE 5
#define BUFSIZE 8192

int open_listenfd(char *port)
{
    struct addrinfo hints;
    struct addrinfo *listp;

    memset(&hints, 0, sizeof(struct addrinfo));
    /* Look to accept connect on any IP addr using this port no */
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE | AI_ADDRCONFIG | AI_NUMERICSERV;
    int retCode = getaddrinfo(NULL, port, &hints, &listp);

    if (retCode < 0)
    {
        fprintf(stderr, "Error: %s\n", gai_strerror(retCode));
        exit(-1);
    }

    int listenFd;
    struct addrinfo *p;
    for (p = listp; p != NULL; p = p->ai_next)
    {
        if ((listenFd = socket(p->ai_family, p->ai_socktype, p->ai_protocol)) < 0)
            continue; /* This option doesn't work; try next */

        int optVal = 1;
        /* Alleviate "Address already in use" by allowing reuse */
        setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR,
                   (const void *)&optVal, sizeof(int));

        if (bind(listenFd, p->ai_addr, p->ai_addrlen) == 0)
            break; /* Yay, success */

        close(listenFd); /* Bind failed, close this, then next */
    }

    freeaddrinfo(listp);

    if (!p)
        return -1; /* None of them worked. Meh */

    /* Make it ready to accept incoming requests */
    if (listen(listenFd, LISTEN_QUEUE) < 0)
    {
        close(listenFd);
        return -1;
    }

    /* All good, return the file descriptor */
    return listenFd;
}

int open_clientfd(char *hostname, char *port)
{
    struct addrinfo hints;
    struct addrinfo *listp;

    memset(&hints, 0, sizeof(struct addrinfo));
    /* Look to accept connect on any IP addr using this port no */
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_ADDRCONFIG | AI_NUMERICSERV;
    int retCode = getaddrinfo(hostname, port, &hints, &listp);

    if (retCode < 0)
    {
        fprintf(stderr, "Error: %s\n", gai_strerror(retCode));
        exit(-1);
    }

    int clientFd;
    struct addrinfo *p;
    for (p = listp; p != NULL; p = p->ai_next)
    {
        if ((clientFd = socket(p->ai_family, p->ai_socktype, p->ai_protocol)) < 0)
            continue; /* This option doesn't work; try next */

        /* Try connecting */
        if (connect(clientFd, p->ai_addr, p->ai_addrlen) != -1)
            break; /* Yay, success */

        close(clientFd); /* Bind failed, close this, then next */
    }

    freeaddrinfo(listp);

    if (!p)
        return -1; /* None of them worked. Meh */

    return clientFd;
}

void write_all(int connFd, char *buf, size_t len)
{
    size_t toWrite = len;

    while (toWrite > 0)
    {
        ssize_t numWritten = write(connFd, buf, toWrite);
        if (numWritten < 0)
        {
            fprintf(stderr, "Meh, can't write\n");
            return;
        }
        toWrite -= numWritten;
        buf += numWritten;
    }
}

ssize_t read_line(int connFd, char *usrbuf, size_t maxlen)
{
    enum
    {
        STATE_START = 0,
        STATE_CR,
        STATE_CRLF
    };

    int i = 0, state;
    char ch;
    memset(usrbuf, 0, 8192);
    int n;
    int max = maxlen - 1;

    state = STATE_START;
    while (state != STATE_CRLF)
    {
        char expected = 0;

        if (i == max)
        {
            break;
        }

        if ((n = read(connFd, &ch, 1)) < 0)
        {
            break;
        }

        usrbuf[i++] = ch;

        switch (state)
        {
        case STATE_START:
            expected = '\r';
            break;
        case STATE_CR:
            expected = '\n';
            break;
        default:
            state = STATE_START;
            continue;
        }

        if (ch == expected)
        {
            state++;
        }
        else
        {
            state = STATE_START;
        }
    }
    usrbuf[i] = '\0';
    return i;
}
