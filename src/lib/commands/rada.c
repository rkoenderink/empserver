/*
 *  Empire - A multi-player, client/server Internet based war game.
 *  Copyright (C) 1986-2006, Dave Pare, Jeff Bailey, Thomas Ruschak,
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
 *  rada.c: Do radar from a ship/unit/sector
 * 
 *  Known contributors to this file:
 *     
 */

#include <config.h>

#include <ctype.h>
#include "commands.h"
#include "optlist.h"
#include "empobj.h"

int
rada(void)
{
    char *cp;
    double tf;
    double tech;
    struct nstr_item ni;
    struct nstr_sect ns;
    union empobj_storage item;
    char buf[1024];
    short type;

    type = player->argp[0][0] == 'l' ? EF_LAND : EF_SHIP;

    cp = getstarg(player->argp[1],
	type == EF_SHIP ? "Radar from (ship # or sector(s)) : " :
	"Radar from (unit # or sector(s)) : ", buf);
		      
    if (cp == 0)
	return RET_SYN;
    switch (sarg_type(cp)) {
    case NS_AREA:
	if (!snxtsct(&ns, cp))
	    return RET_SYN;
	tech = tfact(player->cnum, 8.0);
	if (tech > WORLD_Y / 4.0)
	    tech = WORLD_Y / 4.0;
	if (tech > WORLD_X / 8.0)
	    tech = WORLD_X / 8.0;
	while (nxtsct(&ns, &item.sect)) {
	    if (item.sect.sct_type != SCT_RADAR)
		continue;
	    if (!player->owner)
		continue;
	    radmap(item.sect.sct_x, item.sect.sct_y, item.sect.sct_effic,
		   (int)(tech * 2.0), 0.0);
	}
	break;
    case NS_LIST:
    case NS_GROUP:
	/* assumes a NS_LIST return is a unit no */
	if (!snxtitem(&ni, type, cp)) {
	    pr("Specify at least one %s\n",
		type == EF_SHIP ? "ship" : "unit");
	    return RET_SYN;
	}
	while (nxtitem(&ni, &item)) {
	    if (!player->owner)
		continue;
	    tf = 0.0;
	    if (type == EF_SHIP) {
		if (mchr[(int)item.ship.shp_type].m_flags & M_SONAR)
		    tf = techfact(item.ship.shp_tech, 1.0);
		tech = techfact(item.ship.shp_tech,
		    mchr[(int)item.ship.shp_type].m_vrnge);
	    } else {
		if (!(lchr[(int)item.land.lnd_type].l_flags & L_RADAR)) {
		    pr("%s can't use radar!\n", prland(&item.land));
		    continue;
		}
		if (item.land.lnd_ship >= 0) {
		    pr("Units on ships can't use radar!\n");
		    continue;
		}
		tech = techfact(item.land.lnd_tech, item.land.lnd_spy);
	    }

	    pr("%s at ", obj_nameof(&item.gen));
	    if (tech > WORLD_Y / 2.0)
		tech = WORLD_Y / 2.0;
	    if (tech > WORLD_X / 4.0)
		tech = WORLD_X / 4.0;
	    radmap(item.gen.x, item.gen.y, item.gen.effic,
		   (int)tech, tf);
	}
	break;
    default:
	pr("Must use a %s or sector specifier\n",
	    type == EF_SHIP ? "ship" : "unit");
	return RET_SYN;
    }
    return RET_OK;
}
