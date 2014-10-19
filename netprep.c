// netprep.c - prepare net from base

/*
   This file is part of Tripover, a broad-search journey planner.

   Copyright (C) 2014 Joris van der Geer.

   This work is licensed under the Creative Commons Attribution-NonCommercial-NoDerivatives 4.0 International License.
   To view a copy of this license, visit http://creativecommons.org/licenses/by-nc-nd/4.0/
 */

#include <string.h>

#include "base.h"
#include "cfg.h"
#include "mem.h"
#include "math.h"

static ub4 msgfile;
#include "msg.h"

#include "util.h"
#include "bitfields.h"
#include "netbase.h"
#include "netio.h"
#include "netprep.h"
#include "net.h"

void ininetprep(void)
{
  msgfile = setmsgfile(__FILE__);
  iniassert();
}

int prepnet(netbase *basenet)
{
  struct network *net = getnet();
  struct portbase *bports,*bpp;
  struct hopbase *bhops,*bhp;
  struct port *ports,*pdep,*parr,*pp;
  struct hop *hops,*hp;
  ub4 portcnt,hopcnt,dep,arr,ndep,narr,nodep,noarr,nodeparr,nudep,nuarr;
  ub4 nlen,n,routeid;
  enum txkind kind;
  ub4 hop,port;
  ub4 variants,varmask;

  hopcnt = basenet->hopcnt;
  portcnt = basenet->portcnt;
  if (portcnt == 0 || hopcnt == 0) return 1;

  bports = basenet->ports;
  bhops = basenet->hops;

  ports = alloc(portcnt,struct port,0,"ports",portcnt);
  hops = alloc(hopcnt,struct hop,0,"hops",hopcnt);

  for (port = 0; port < portcnt; port++) {
    pp = ports + port;
    bpp = bports + port;
    pp->id = pp->allid = port;
    nlen = bpp->namelen;
    if (nlen) {
      memcpy(pp->name,bpp->name,nlen);
      pp->namelen = nlen;
    }
    pp->lat = bpp->lat;
    pp->lon = bpp->lon;
    pp->rlat = bpp->rlat;
    pp->rlon = bpp->rlon;
    pp->utcofs = bpp->utcofs;
  }

  for (hop = 0; hop < hopcnt; hop++) {
    hp = hops + hop;
    hp->id = hop;
    bhp = bhops + hop;
    nlen = bhp->namelen;
    if (nlen) {
      memcpy(hp->name,bhp->name,nlen);
      hp->namelen = nlen;
    }
    dep = bhp->dep;
    arr = bhp->arr;
    routeid = bhp->routeid;

    hp->dep = dep;
    hp->arr = arr;
    hp->routeid = routeid;

    pdep = ports + dep;
    parr = ports + arr;

    kind = bhp->kind;
    hp->kind = kind;
    switch(kind) {
    case Walk: pdep->nwalkdep++; parr->nwalkarr++; break;
    case Air: break;
    case Rail: break;
    case Bus: break;
    case Unknown: info(0,"hop %s has unknown transport mode", hp->name); break;
    }

  }

  net->allportcnt = portcnt;
  net->allhopcnt = hopcnt;
  net->allports = ports;
  net->allhops = hops;

  net->maxrouteid = basenet->maxrouteid;
  net->maxvariants = variants = basenet->maxvariants;
  net->routevarmask = varmask = basenet->routevarmask;

// mark local links
  ub4 undx,nvdep,nvarr,nveq;

  for (hop = 0; hop < hopcnt; hop++) {
    hp = hops + hop;

    dep = hp->dep;
    arr = hp->arr;
    error_ge(dep,portcnt);
    error_ge(arr,portcnt);
    pdep = ports + dep;
    parr = ports + arr;
    ndep = pdep->ndep;
    narr = parr->narr;

    pdep->ndep = ndep+1;
    parr->narr = narr+1;

    if (hp->kind == Walk) continue;

    routeid = hp->routeid;

    nudep = pdep->nudep;
    nvdep = pdep->nvdep;
    nuarr = parr->nuarr;
    nvarr = parr->nvarr;

    if (nudep == 0) {
      pdep->deps[0] = arr; pdep->nudep = pdep->nvdep = 1; pdep->drids[0] = routeid;
    } else if (nudep < Nlocal) {
      undx = nveq = 0;
      while (undx < nudep && pdep->deps[undx] != arr) {
        if (routeid != hi32 && (routeid & varmask) == (pdep->drids[undx++] & varmask) ) nveq = 1;
      }
      if (undx == nudep) {
        pdep->deps[undx] = arr; pdep->nudep = undx + 1;
        pdep->drids[undx] = routeid;
        if (nveq == 0 || routeid == hi32) pdep->nvdep = nvdep + 1;
      }
    }
    if (nuarr == 0) {
      parr->arrs[0] = dep; parr->nuarr = parr->nvarr = 1; parr->arids[0] = routeid;
    } else if (nuarr < Nlocal) {
      undx = nveq = 0;
      while (undx < nuarr && parr->arrs[undx] != dep) {
        if (routeid != hi32 && (routeid & varmask) == (parr->arids[undx++] & varmask) ) nveq = 1;
      }
      if (undx == nuarr) {
        parr->arrs[undx] = dep; parr->nuarr = undx + 1;
        parr->arids[undx] = routeid;
        if (nveq == 0 || routeid == hi32) parr->nvarr = nvarr + 1;
      }
    }

  }
  nodep = noarr = nodeparr = 0;

  ub4 constats[256];
  aclear(constats);

  for (port = 0; port < portcnt; port++) {
    pp = ports + port;
    ndep = pp->ndep; narr = pp->narr;
    if (ndep == 0 && narr == 0) { info(0,"port %u has no connections - %s",port,pp->name); nodeparr++; }
    else if (ndep == 0) { info(0,"port %u has no deps - %s",port,pp->name); nodep++; }
    else if (narr == 0) { info(0,"port %u has no arrs - %s",port,pp->name); noarr++; }
    if (ndep < 16 && narr < 16) constats[(ndep << 4) | narr]++;
  }
  info(0,"%u of %u ports without departures",nodep,portcnt);
  info(0,"%u of %u ports without arrivals",noarr,portcnt);
  info(0,"%u of %u ports without connection",nodeparr,portcnt);
  for (ndep = 0; ndep < 4; ndep++) {
    for (narr = 0; narr < 4; narr++) {
      n = constats[(ndep << 4) | narr];
      if (n || ndep < 2 || narr < 2) info(0,"%u ports with %u deps + %u arrs", n,ndep,narr);
    }
  }
  return 0;
}
