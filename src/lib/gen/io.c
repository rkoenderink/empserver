/*
 *  Empire - A multi-player, client/server Internet based war game.
 *  Copyright (C) 1986-2000, Dave Pare, Jeff Bailey, Thomas Ruschak,
 *                           Ken Stevens, Steve McClure
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 *  ---
 *
 *  See the "LEGAL", "LICENSE", "CREDITS" and "README" files for all the
 *  related information and legal notices. It is expected that any future
 *  projects/authors will amend these files as needed.
 *
 *  ---
 *
 *  io.c: Arrange for input and output on a file descriptor to be queued.
 * 
 *  Known contributors to this file:
 *      Doug Hay, 1998
 *      Steve McClure, 1998
 */

/*
 * Arrange for input and output on a file descriptor
 * to be queued.  Provide main loop -- a mechanism for
 * blocking across all registered file descriptors, and
 * reading or writing when appropriate.
 */

#include <errno.h>
#include <sys/types.h>
#if !defined(_WIN32)
#include <sys/uio.h>
#include <sys/file.h>
#include <unistd.h>		/* close read shutdown select */
#include <sys/socket.h>
#endif
#include <time.h>
#include <fcntl.h>
#include <stdlib.h>		/* malloc calloc free */

#if defined(_WIN32)
#include <winsock.h>
#endif

#include "misc.h"
#include "queue.h"
#include "ioqueue.h"
#include "io_mask.h"
#include "empio.h"
#include "gen.h"		/* getfdtablesize */

#include "empthread.h"

extern struct player *player;	/* XXX */

static struct iop **io_list;
static struct io_mask *iom;
static int fdmax;		/* largest file descriptor seen */
static fd_set newoutput;

struct iop {
    int fd;
    struct ioqueue *input;
    struct ioqueue *output;
    int flags;
    s_char *assoc;
    int bufsize;
    int (*notify) ();
};

void
io_init(void)
{
    iom = iom_create(IO_READ | IO_WRITE);
    io_list = (struct iop **)calloc(getfdtablesize(), sizeof(*io_list));
    fdmax = 0;
    FD_ZERO(&newoutput);
}

struct iop *
io_open(int fd, int flags, int bufsize, int (*notify) (void),
	s_char *assoc)
{
    struct iop *iop;

    if (io_list[fd] != 0) {
	/* already exists */
	return 0;
    }
    flags = flags & (IO_READ | IO_WRITE | IO_NBLOCK | IO_NEWSOCK);
    if ((flags & (IO_READ | IO_WRITE)) == 0)
	return 0;
    iop = (struct iop *)malloc(sizeof(struct iop));
    iop->fd = fd;
    iop->input = 0;
    iop->output = 0;
    iop->flags = 0;
    iop->bufsize = bufsize;
    if ((flags & IO_READ) && (flags & IO_NEWSOCK) == 0)
	iop->input = ioq_create(bufsize);
    if ((flags & IO_WRITE) && (flags & IO_NEWSOCK) == 0)
	iop->output = ioq_create(bufsize);
    if (flags & IO_NBLOCK)
	io_noblocking(iop, 1);
    iop->flags = flags;
    iop->assoc = assoc;
    iop->notify = notify;
    io_list[fd] = iop;
    iom_set(iom, flags, fd);
    if (fd > fdmax) fdmax = fd;
    return iop;
}

void
io_close(struct iop *iop)
{

    if (iop->input != 0)
	ioq_destroy(iop->input);
    if (iop->output != 0)
	ioq_destroy(iop->output);
    iom_clear(iom, iop->flags, iop->fd);
    FD_CLR(iop->fd, &newoutput);
    io_list[iop->fd] = 0;
#if !defined(_WIN32)
    (void)close(iop->fd);
#else
    closesocket(iop->fd);
#endif
    free((s_char *)iop);
}

int
io_input(struct iop *iop, int waitforinput)
{
    s_char buf[IO_BUFSIZE];
    int cc;

    /* Not a read IOP */
    if ((iop->flags & IO_READ) == 0)
	return -1;
    /* IOP is markes as in error. */
    if (iop->flags & IO_ERROR)
	return -1;
    /* Wait for the file to have input. */
    if (waitforinput) {
	empth_select(iop->fd, EMPTH_FD_READ);
    }
#if !defined(_WIN32)
    /* Do the actual read. */
    cc = read(iop->fd, buf, sizeof(buf));
    if (cc < 0) {
	/* would block, so nothing to read. */
	if (errno == EWOULDBLOCK)
	    return 0;

	/* Some form of file error occurred... */
	iop->flags |= IO_ERROR;
	iom_clear(iom, IO_READ, iop->fd);
	return -1;
    }
#else
    cc = recv(iop->fd, buf, sizeof(buf), 0);
    if (cc == SOCKET_ERROR) {
	int err = WSAGetLastError();
	/* Hmm, it would block.  file is opened noblock, soooooo.. */
	if (err == WSAEWOULDBLOCK)
	    return 0;

	/* Some form of file error occurred... */
	iop->flags |= IO_ERROR;
	iom_clear(iom, IO_READ, iop->fd);
	return -1;
    }
#endif

    /* We eof'd */
    if (cc == 0) {
	iop->flags |= IO_EOF;
	return 0;
    }

    /* Append the input to the IOQ. */
    ioq_append(iop->input, buf, cc);
    return cc;
}

int
io_inputwaiting(struct iop *iop)
{
    return ioq_qsize(iop->input);
}

int
io_outputwaiting(struct iop *iop)
{
    return ioq_qsize(iop->output);
}

int
io_output(struct iop *iop, int waitforoutput)
{
#if !defined(_WIN32)
    struct iovec iov[16];
#else
    s_char buf[IO_BUFSIZE];
#endif
    int cc;
    int n;
    int remain;

    /* If there is no output waiting. */
    if (!io_outputwaiting(iop))
	return 0;

    /* bit clear */
    FD_CLR(iop->fd, &newoutput);

    /* If the iop is not write enabled. */
    if ((iop->flags & IO_WRITE) == 0)
	return -1;

    /* If the io is marked as in error... */
    if (iop->flags & IO_ERROR)
	return -1;

    /* This is the same test as io_outputwaiting.... */
    if (ioq_qsize(iop->output) == 0)
	return 0;

#if !defined(_WIN32)
    /* make the iov point to the data in the queue. */
    /* I.E., each of the elements in the queue. */
    /* returns the number of elements in the iov. */
    n = ioq_makeiov(iop->output, iov, IO_BUFSIZE);
#else
    /* Make a buffer containing the output to write. */
    n = ioq_makebuf(iop->output, buf, sizeof(buf));
#endif

    if (n <= 0) {
	/* If we got no elements, we have no output.... */
	iom_clear(iom, IO_WRITE, iop->fd);
	return 0;
    }

    /* wait for the file to be output ready. */
    if (waitforoutput != IO_NOWAIT) {
	/* This waits for the file to be ready for writing, */
	/* and lets other threads run. */
	empth_select(iop->fd, EMPTH_FD_WRITE);
    }

    /* Do the actual write. */
#if !defined(_WIN32)
    cc = writev(iop->fd, iov, n);

    /* if it failed.... */
    if (cc < 0) {
	/* Hmm, it would block.  file is opened noblock, soooooo.. */
	if (errno == EWOULDBLOCK) {
	    /* If there are remaining bytes, set the IO as remaining.. */
	    remain = ioq_qsize(iop->output);
	    if (remain > 0)
		iom_set(iom, IO_WRITE, iop->fd);
	    return remain;
	}
	iop->flags |= IO_ERROR;
	iom_clear(iom, IO_WRITE, iop->fd);
	return -1;
    }
#else
    cc = send(iop->fd, buf, n, 0);

    /* if it failed.... */
    if (cc == SOCKET_ERROR) {
	int err = WSAGetLastError();
	/* Hmm, it would block.  file is opened noblock, soooooo.. */
	if (err == WSAEWOULDBLOCK) {
	    /* If there are remaining bytes, set the IO as remaining.. */
	    remain = ioq_qsize(iop->output);
	    if (remain > 0)
		iom_set(iom, IO_WRITE, iop->fd);
	    return remain;
	}
	iop->flags |= IO_ERROR;
	iom_clear(iom, IO_WRITE, iop->fd);
	return -1;
    }
#endif


    /* If no bytes were written, something happened..  Like an EOF. */
#ifndef	hpux
    if (cc == 0) {
	iop->flags |= IO_EOF;
	return 0;
    }
#else
    if (cc == 0) {
	remain = ioq_qsize(iop->output);
	if (remain > 0)
	    iom_set(iom, IO_WRITE, iop->fd);
	return remain;
    }
#endif /* hpux */

    /* Remove the number of written bytes from the queue. */
    ioq_dequeue(iop->output, cc);

    /* If the queue has stuff remaining, set it still needing output. */
    remain = ioq_qsize(iop->output);
    if (remain == 0) {
	iom_clear(iom, IO_WRITE, iop->fd);
    } else {
	iom_set(iom, IO_WRITE, iop->fd);
    }
    return cc;
}

int
io_select(struct timeval *tv)
{
    fd_set *readmask;
    fd_set *writemask;
    int n;
    int maxfd;
    int fd;
    struct iop *iop;

    iom_getmask(iom, &maxfd, &readmask, &writemask);
    n = select(maxfd + 1, readmask, writemask, NULL, tv);
    if (n <= 0) {
	if (errno == EINTR)
	    return 0;
	return -1;
    }
    for (fd = 0; fd <= maxfd; ++fd) {
	if (!FD_ISSET(fd, readmask)) continue;
	iop = io_list[fd];
	if ((iop->flags & IO_NEWSOCK) == 0)
	    (void)io_input(iop, IO_NOWAIT);
	if (iop->notify != 0)
	    iop->notify(iop, IO_READ, iop->assoc);
	FD_CLR(fd, readmask);
    }
    for (fd = 0; fd <= maxfd; ++fd) {
	if (!FD_ISSET(fd, writemask)) continue;
	iop = io_list[fd];
	if (io_output(iop, IO_NOWAIT) < 0 && iop->notify != 0)
	    iop->notify(iop, IO_WRITE, iop->assoc);
	FD_CLR(fd, writemask);
    }
    return n;
}

void
io_flush(int doWait)
{
    int fd;
    struct iop *iop;

    for (fd = 0; fd <= fdmax; ++fd) {
	if (!FD_ISSET(fd, &newoutput)) continue;
	iop = io_list[fd];
	if (io_output(iop, doWait) < 0 && iop->notify != 0)
	    iop->notify(iop, IO_WRITE, iop->assoc);
    }
}

int
io_peek(struct iop *iop, s_char *buf, int nbytes)
{
    if ((iop->flags & IO_READ) == 0)
	return -1;
    return ioq_peek(iop->input, buf, nbytes);
}

int
io_read(struct iop *iop, s_char *buf, int nbytes)
{
    int cc;

    if ((iop->flags & IO_READ) == 0)
	return -1;
    cc = ioq_peek(iop->input, buf, nbytes);
    if (cc > 0)
	ioq_dequeue(iop->input, cc);
    return cc;
}

int
io_write(struct iop *iop, s_char *buf, int nbytes, int doWait)
{
    int len;

    if ((iop->flags & IO_WRITE) == 0)
	return -1;
    ioq_append(iop->output, buf, nbytes);
    FD_SET(iop->fd, &newoutput);
    len = ioq_qsize(iop->output);
    if (len > iop->bufsize) {
	if (doWait) {
	    io_output_all(iop);
	} else {
	    /* only try a write every BUFSIZE characters */
	    if (((len - nbytes) % iop->bufsize) < (len % iop->bufsize))
		io_output(iop, 0);
	}
    }
    return nbytes;
}

int
io_output_all(struct iop *iop)
{
    int n;

    while ((n = io_output(iop, IO_NOWAIT)) > 0) {
	empth_select(iop->fd, EMPTH_FD_WRITE);
    }
    return n;
}

int
io_gets(struct iop *iop, s_char *buf, int nbytes)
{
    if ((iop->flags & IO_READ) == 0)
	return -1;
    return ioq_gets(iop->input, buf, nbytes);
}

int
io_puts(struct iop *iop, s_char *buf)
{
    if ((iop->flags & IO_WRITE) == 0)
	return -1;
    FD_SET(iop->fd, &newoutput);
    return ioq_puts(iop->output, buf);
}

int
io_shutdown(struct iop *iop, int flags)
{
    flags &= (IO_READ | IO_WRITE);
    if ((iop->flags & flags) != flags)
	return -1;
    if (flags & IO_READ) {
	shutdown(iop->fd, 0);
	ioq_drain(iop->input);
    }
    if (flags & IO_WRITE) {
	shutdown(iop->fd, 1);
	ioq_drain(iop->output);
    }
    return 0;
}

int
io_noblocking(struct iop *iop, int value)
{
#if !defined(_WIN32)
    int flags;

    flags = fcntl(iop->fd, F_GETFL, 0);
    if (flags < 0)
	return -1;
    if (value == 0)
	flags &= ~FNDELAY;
    else
	flags |= FNDELAY;
    if (fcntl(iop->fd, F_SETFL, flags) < 0)
	return -1;
#else
    u_long arg = value;
    ioctlsocket(iop->fd, FIONBIO, &arg);
#endif
    if (value == 0)
	iop->flags &= ~IO_NBLOCK;
    else
	iop->flags |= IO_NBLOCK;
    return 0;
}

int
io_conn(struct iop *iop)
{
    return (iop->flags & IO_CONN);
}

int
io_error(struct iop *iop)
{
    return (iop->flags & IO_ERROR);
}

int
io_eof(struct iop *iop)
{
    return (iop->flags & IO_EOF);
}

int
io_fileno(struct iop *iop)
{
    return iop->fd;
}

struct iop *
io_iopfromfd(int fd)
{
    return io_list[fd];
}
