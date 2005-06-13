/*
 *  Empire - A multi-player, client/server Internet based war game.
 *  Copyright (C) 1986-2005, Dave Pare, Jeff Bailey, Thomas Ruschak,
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
 *  pr.c: Use to do output to a player
 * 
 *  Known contributors to this file:
 *     Dave Pare, 1986, 1989 
 *     Steve McClure, 1998-2000
 */
/*
 * The pr routine historically arranged for nonbuffered i/o
 * because stdio didn't used to automatically flush stdout before
 * it read something from stdin.  Now pr() prepends an "output id"
 * in front of each line of text, informing the user interface
 * what sort of item it is seeing; prompt, noecho prompt,
 * more input data, etc.
 */

#include <string.h>
#include <fcntl.h>
#include <ctype.h>
#include <stdarg.h>
#include "proto.h"
#include "misc.h"
#include "player.h"
#include "nat.h"
#include "empio.h"
#include "file.h"
#include "com.h"
#include "tel.h"
#include "server.h"
#include "prototypes.h"

static void outid(struct player *pl, int n);

/*VARARGS*/
void
pr(char *format, ...)
{
    char buf[4096];
    va_list ap;

    va_start(ap, format);
    (void)vsprintf(buf, format, ap);
    va_end(ap);
    if (player->flags & PF_UTF8)
	upr_player(player, C_DATA, buf);
    else
        pr_player(player, C_DATA, buf);
}

void
uprnf(char *buf /* buf is message text */)
{
    /*
     * Translate to ASCII if the client is not in UTF mode
     */
    if (!(player->flags & PF_UTF8))
	prtoascii(buf);

    pr_player(player, C_DATA, buf);
}

/*VARARGS*/
void
pr_id(struct player *p, int id, s_char *format, ...)
{
    s_char buf[4096];
    va_list ap;

    if (p->curid >= 0) {
	io_puts(p->iop, "\n");
	p->curid = -1;
    }
    va_start(ap, format);
    (void)vsprintf(buf, format, ap);
    va_end(ap);
    pr_player(p, id, buf);
}

void
pr_flash(struct player *pl, char *format
	 /* format is message text */, ...)
{
    char buf[4096]; /* buf is message text */
    va_list ap;

    if (pl->state != PS_PLAYING)
	return;
    va_start(ap, format);
    (void)vsprintf(buf, format, ap);
    va_end(ap);
    /*
     * Translate to ASCII if the client is not in UTF mode
     */
    if (!(pl->flags & PF_UTF8))
	prtoascii(buf);
    pr_player(pl, C_FLASH, buf);
    io_output(pl->iop, IO_NOWAIT);
}

void
pr_inform(struct player *pl, s_char *format, ...)
{
    s_char buf[4096];
    va_list ap;

    if (pl->state != PS_PLAYING)
	return;
    va_start(ap, format);
    (void)vsprintf(buf, format, ap);
    va_end(ap);
    pr_player(pl, C_INFORM, buf);
    io_output(pl->iop, IO_NOWAIT);
}

void
pr_wall(s_char *format, ...)
{
    s_char buf[4096];
    struct player *p;
    va_list ap;

    va_start(ap, format);
    (void)vsprintf(buf, format, ap);
    va_end(ap);
    for (p = player_next(0); p; p = player_next(p)) {
	if (p->state != PS_PLAYING)
	    continue;
	pr_player(p, C_FLASH, buf);
	io_output(p->iop, IO_NOWAIT);
    }
}

void
pr_player(struct player *pl, int id, s_char *buf)
{
    register s_char *p;
    register s_char *bp;
    register int len;

    bp = buf;
    while (*bp != '\0') {
	if (pl->curid != -1 && pl->curid != id) {
	    io_puts(pl->iop, "\n");
	    pl->curid = -1;
	}
	if (pl->curid == -1)
	    outid(pl, id);
	p = strchr(bp, '\n');
	if (p != 0) {
	    len = (p - bp) + 1;
	    if (pl->command && (pl->command->c_flags & C_MOD))
		io_write(pl->iop, bp, len, IO_NOWAIT);
	    else
		io_write(pl->iop, bp, len, IO_WAIT);
	    bp += len;
	    pl->curid = -1;
	} else {
	    len = io_puts(pl->iop, bp);
	    bp += len;
	}
    }
}

void
upr_player(struct player *pl, int id, char *buf
                      /* buf is message text */)
{
    register char *bp; /* bp is message text */
    register int standout = 0;
    char printbuf[2]; /* bp is message text */
    char ch;

    printbuf[0] = '\0';
    printbuf[1] = '\0';

    bp = buf;
    while ((ch = *bp++)) {
	if (pl->curid != -1 && pl->curid != id) {
	    io_puts(pl->iop, "\n");
	    pl->curid = -1;
	}
	if (pl->curid == -1)
	    outid(pl, id);

	if (ch & 0x80) {
	    if (standout == 0) {
		printbuf[0] = 0x0e;
		io_puts(pl->iop, printbuf);
		standout = 1;
	    }
	    ch &= 0x7f;
	} else {
	    if (standout == 1) {
		printbuf[0] = 0x0f;
		io_puts(pl->iop, printbuf);
		standout = 0;
	    }
	}
	if (ch == '\n') {
	    if (pl->command && (pl->command->c_flags & C_MOD))
		io_write(pl->iop, &ch, 1, IO_NOWAIT);
	    else
		io_write(pl->iop, &ch, 1, IO_WAIT);
	    pl->curid = -1;
	} else {
	    printbuf[0] = ch;
	    io_puts(pl->iop, printbuf);
	}
    }
}

/*
 * highlighted characters have hex 80 or'ed in
 * with them to designate their highlightedness
 */
void
pr_hilite(s_char *buf)
{
    register s_char *bp;
    register s_char c;
    s_char *p;

    p = malloc(strlen(buf) + 1);
    strcpy(p, buf);
    for (bp = p; 0 != (c = *bp); bp++)
	if (isprint(c))
	    *bp |= 0x80;
    pr(p);
    free(p);
}

/*
 * output hex code + space
 */
static void
outid(struct player *pl, int n)
{
    s_char c;
    s_char buf[3];

    if (n > C_LAST) {
	logerror("outid: %d not valid code\n", n);
	return;
    }
    if (n >= 10)
	c = 'a' - 10 + n;
    else
	c = '0' + n;
    buf[0] = c;
    buf[1] = ' ';
    buf[2] = '\0';
    io_puts(pl->iop, buf);
    pl->curid = n;
}

void
prredir(s_char *redir)
{
    pr_id(player, *redir == '>' ? C_REDIR : C_PIPE, "%s\n", redir);
}

void
prexec(s_char *file)
{
    pr_id(player, C_EXECUTE, "%s\n", file);
}

void
prprompt(int min, int btu)
{
    pr_id(player, C_PROMPT, "%d %d\n", min, btu);
}

int
prmptrd(char *prompt, char *str, int size)
{
    int r;
    char *cp;

    pr_id(player, C_FLUSH, "%s\n", prompt);
    if ((r = recvclient(str, size)) < 0)
	return r;
    time(&player->curup);
    if (*str == 0)
	return 1;
    for(cp = str; 0 != *cp; ++cp) {
	if ((*cp >= 0x0 && *cp < 0x20 && *cp != '\t') ||
	    *cp == 0x7f || *cp & 0x80)
	    *cp = '?';
    }
    return strlen(str);
}

int
uprmptrd(char *prompt, char *str /* str is message text */, int size)
{
    int r;
    char *cp; /* cp is message text */

    pr_id(player, C_FLUSH, "%s\n", prompt);
    if ((r = recvclient(str, size)) < 0)
	return r;
    time(&player->curup);
    if (*str == 0)
	return 1;
    
    for(cp = str; 0 != *cp; ++cp) {
	if ((*cp >= 0x0 && *cp < 0x20 && *cp != '\t') ||
	    *cp == 0x7f)
	    *cp = '?';
	else if (!(player->flags & PF_UTF8) && (*cp & 0x80))
	    *cp = '?';
    }
    return strlen(str);
}

void
prdate(void)
{
    time_t now;

    (void)time(&now);
    pr(ctime(&now));
}

/*
 * print x,y formatting as country
 */
void
prxy(s_char *format, coord x, coord y, natid country)
{
    s_char buf[255];
    struct natstr *np;

    np = getnatp(country);
    sprintf(buf, format, xrel(np, x), yrel(np, y));
    pr(buf);
}

/*VARARGS*/
void
PR(int cn, s_char *format, ...)
{
    /* XXX should really do this on a per-nation basis */
    static s_char longline[MAXNOC][512];
    int newline;
    va_list ap;
    s_char buf[1024];

    va_start(ap, format);
    (void)vsprintf(buf, format, ap);
    va_end(ap);
    newline = strrchr(buf, '\n') ? 1 : 0;
    strcat(longline[cn], buf);
    if (newline) {
	if (update_pending || (cn && cn != player->cnum))
	    typed_wu(0, cn, longline[cn], TEL_BULLETIN);
	else
	    pr_player(player, C_DATA, longline[cn]);
	longline[cn][0] = '\0';
    }
}

void
PRdate(natid cn)
{
    time_t now;

    (void)time(&now);
    PR(cn, ctime(&now));
}

void
pr_beep(void)
{
    struct natstr *np = getnatp(player->cnum);

    if (np->nat_flags & NF_BEEP)
	pr("\07");
}

void
mpr(int cn, s_char *format, ...)
{
    s_char buf[4096];
    va_list ap;

    va_start(ap, format);
    (void)vsprintf(buf, format, ap);
    va_end(ap);
    if (cn) {
	if (update_pending || cn != player->cnum)
	    typed_wu(0, cn, buf, TEL_BULLETIN);
	else
	    pr_player(player, C_DATA, buf);
    }
}

void
prtoascii(char *buf /* buf is message text */)
{
    char *pbuf; /* pbuf is message text */

    for(pbuf = buf; *pbuf != 0; pbuf++)
	if ((*pbuf & 0xc0) == 0xc0)
	    *pbuf = '?';
	else if (*pbuf & 0x80) {
	    memmove(pbuf,pbuf+1,strlen(pbuf)-1);
	    pbuf--;
	}
}

/*
 * Return byte-index of the N-th UTF-8 character in UTF-8 string S.
 * If S doesn't have that many characters, return its length instead.
 */
int
ufindpfx(char *s, int n)
{
    int i = 0;

    while (n && s[i])
    {
	if ((s[i++] & 0xc0) == 0xc0)
            while ((s[i] & 0xc0) == 0x80)
		i++;
        --n;
    }
    return i;
}
