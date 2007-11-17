/*
 *  Empire - A multi-player, client/server Internet based war game.
 *  Copyright (C) 1986-2007, Dave Pare, Jeff Bailey, Thomas Ruschak,
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
 *  See files README, COPYING and CREDITS in the root of the source
 *  tree for related information and legal notices.  It is expected
 *  that future projects/authors will amend these files as needed.
 *
 *  ---
 *
 *  servercmd.c: Change the state depending on the command from the server.
 * 
 *  Known contributors to this file:
 *     Dave Pare, 1989
 *     Steve McClure, 1998
 *     Ron Koenderink, 2005
 */

#include <config.h>

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

#include "ioqueue.h"
#include "misc.h"
#include "proto.h"
#include "tags.h"

static char num_teles[64];
static char the_prompt[1024];
static int mode;
static int nbtu;
static int nmin;
static FILE *redir_fp;
static FILE *pipe_fp;

static void prompt(FILE *auxfi);
static void doredir(char *p);
static void dopipe(char *p);
static void doexecute(char *p, FILE *auxfi);
static void output(int code, char *buf, FILE *auxfi);
static void screen(char *buf);

void
servercmd(struct ioqueue *ioq, FILE *auxfi)
{
    char buf[1024];
    char *p;
    static int code = -1;
    int eol;

    while (ioq_gets(ioq, buf, sizeof(buf), &eol)) {
	p = buf;
	if (code == -1) {
    	    if (isalpha(*buf))
		code = 10 + (*buf - 'a');
	    else
		code = *buf - '0';
	    while (*p && !isspace(*p))
		p++;
	    *p++ = 0;
	}
	/*
	 * FIXME
	 * C_REDIR, C_PIPE, and C_EXECUTE will not
	 * work with filename longer than one buffer
	 */
	switch (code) {
	case C_PROMPT:
	    if (sscanf(p, "%d %d", &nmin, &nbtu) != 2) {
		fprintf(stderr, "prompt: bad server prompt %s\n", p);
	    }
	    mode = code;
	    sprintf(the_prompt, "[%d:%d] Command : ", nmin, nbtu);
	    prompt(auxfi);
	    break;
	case C_REDIR:
	    if (eol)
		p[strlen(p) - 1] = '\0';
	    doredir(p);
	    break;
	case C_PIPE:
	    if (eol)
		p[strlen(p) - 1] = '\0';
	    dopipe(p);
	    break;
	case C_FLUSH:
	    mode = code;
	    if (eol)
		p[strlen(p) - 1] = '\0';
	    sprintf(the_prompt, "%s", p);
	    prompt(auxfi);
	    break;
	case C_EXECUTE:
	    if (eol)
		p[strlen(p) - 1] = '\0';
	    doexecute(p, auxfi);
	    break;
	case C_INFORM:
	    if (eol)
		p[strlen(p) - 1] = '\0';
	    if (*p) {
		p[strlen(p) - 1] = '\0';
		sprintf(num_teles, "(%s) ", p + 1);
		if (!redir_fp && !pipe_fp) {
		    putchar('\07');
		    prompt(NULL);
		}
	    } else
		*num_teles = '\0';
	    break;
	default:
	    output(code, p, auxfi);
	    break;
	}
	if (eol)
	    code = -1;
    }
}

static void
prompt(FILE *auxfi)
{
    if (mode == C_PROMPT) {
	if (redir_fp) {
	    (void)fclose(redir_fp);
	    redir_fp = NULL;
	} else if (pipe_fp) {
	    (void)pclose(pipe_fp);
	    pipe_fp = NULL;
	}
    }
    if (mode == C_PROMPT)
	printf("\n");
    printf("%s%s", num_teles, the_prompt);
    (void)fflush(stdout);
    if (auxfi) {
	fprintf(auxfi, "\n%s%s", num_teles, the_prompt);
	(void)fflush(auxfi);
    }
}

static char *
fname(char *s)
{
    char *beg, *end;

    for (beg = s; isspace(*(unsigned char *)beg); beg++) ;
    for (end = beg; !isspace(*(unsigned char *)end); end++) ;
    *end = 0;
    return beg;
}

/*
 * opens redir_fp if successful
 */
static void
doredir(char *p)
{
    char *tag;
    int mode;
    int fd;

    if (redir_fp) {
	(void)fclose(redir_fp);
	redir_fp = NULL;
    }

    if (*p++ != '>') {
	fprintf(stderr, "WARNING!  Weird redirection %s", p);
	return;
    }

    mode = O_WRONLY | O_CREAT;
    if (*p == '>') {
	mode |= O_APPEND;
	p++;
    } else if (*p == '!') {
	mode |= O_TRUNC;
	p++;
    } else
	mode |= O_EXCL;

    tag = gettag(p);
    p = fname(p);
    if (tag == NULL) {
	fprintf(stderr, "WARNING!  Server redirected output to file %s\n",
		p);
	return;
    }
    free(tag);

    if (*p == 0) {
	fprintf(stderr, "Redirection lacks a file name\n");
	return;
    }
    fd = open(p, mode, 0600);
    redir_fp = fd < 0 ? NULL : fdopen(fd, "w");
    if (!redir_fp) {
	fprintf(stderr, "Can't redirect to %s: %s\n",
		p, strerror(errno));
    }
}

/*
 * opens "pipe_fp" if successful
 */
static void
dopipe(char *p)
{
    char *tag;

    if (*p++ != '|') {
	fprintf(stderr, "WARNING!  Weird pipe %s", p);
	return;
    }

    tag = gettag(p);
    if (tag == NULL) {
	fprintf(stderr, "WARNING!  Server attempted to run: %s\n", p);
	return;
    }
    free(tag);

    for (; *p && isspace(*p); p++) ;
    if (*p == 0) {
	fprintf(stderr, "Redirection lacks a command\n");
	return;
    }
    if ((pipe_fp = popen(p, "w")) == NULL) {
	fprintf(stderr, "Can't redirect to pipe %s: %s\n",
		p, strerror(errno));
    }
}

static void
doexecute(char *p, FILE *auxfi)
{
    int fd;
    char *tag;

    tag = gettag(p);
    while (*p && isspace(*p))
	p++;
    if (tag == NULL) {
	fprintf(stderr,
		"WARNING!  Server attempted unauthorized read of file %s\n",
		p);
	return;
    }
    if (p == NULL) {
	fprintf(stderr, "Null file to execute\n");
	free(tag);
	return;
    }
    if ((fd = open(p, O_RDONLY, 0)) < 0) {
	fprintf(stderr, "Can't open execute file\n");
	perror(p);
	free(tag);
	return;
    }
    /* copies 4k at a time to the socket */
    while (termio(fd, sock, auxfi))	/*do copy */
	;
    /*
     * Some platforms don't send the eof (cntl-D) at the end of
     * copying a file.  If emp_client hangs at the end of an
     * execute, include the following line and notify wolfpack
     * of the platform you are using.
     * sendeof(sock);
     */
    close(fd);
    free(tag);
}

static void
output(int code, char *buf, FILE *auxfi)
{
    switch (code) {
    case C_NOECHO:
	/* not implemented; server doesn't send it */
	break;
    case C_ABORT:
	printf("Aborted\n");
	if (auxfi)
	    fprintf(auxfi, "Aborted\n");
	break;
    case C_CMDERR:
    case C_BADCMD:
	printf("Error; ");
	if (auxfi)
	    fprintf(auxfi, "Error; ");
	break;
    case C_EXIT:
	printf("Exit: ");
	if (auxfi)
	    fprintf(auxfi, "Exit: ");
	break;
    case C_FLASH:
	printf("\n");
	break;
    default:
	break;
    }
    if (auxfi) {
	fprintf(auxfi, "%s", buf);
    }

    if (redir_fp)
	fprintf(redir_fp, "%s", buf);
    else if (pipe_fp)
	fprintf(pipe_fp, "%s", buf);
    else {
	screen(buf);
    }
}

static void
screen(char *buf)
{
    char c;

    while ((c = *buf++)) {
	if (eight_bit_clean) {
	    if (c == 14)
		putso();
	    else if (c == 15)
		putse();
	    else
		putchar(c);
	} else if (c & 0x80) {
	    putso();
	    putchar(c & 0x7f);
	    putse();
	} else
	    putchar(c);
    }
}
