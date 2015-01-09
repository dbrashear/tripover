// netprep.c - prepare net from base

/*
   This file is part of Tripover, a broad-search journey planner.

   Copyright (C) 2014-2015 Joris van der Geer.

   This work is licensed under the Creative Commons Attribution-NonCommercial-NoDerivatives 4.0 International License.
   To view a copy of this license, visit http://creativecommons.org/licenses/by-nc-nd/4.0/
 */

/*
  copy work lists from i/o based base lists

  partition ports into partitions by assigning hops a partition
  ports can be members of multiple partitions
  each partition thus includes member ports plus a placeholder for an aggregrated entire partition
  
  1 M ports in 1 K parts -> 1k parts of (1.xk + 1k) each
  x depends on partition criteria
  placeholders can have limited n-stop connectivity
  condense reduces 1M
 */

#include <string.h>
#include <stdlib.h>

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
#include "condense.h"

void ininetprep(void)
{
  msgfile = setmsgfile(__FILE__);
  iniassert();
}

static int marklocal(struct network *net)
{
  // mark local links
  ub4 ndep,narr,nudep,nuarr,nvdep,nvarr;
  struct hop *hp,*hops = net->hops;
  struct port *pdep,*parr,*ports = net->ports;
  ub4 hop,dep,arr;
  ub4 rrid;
  ub4 hopcnt = net->hopcnt;
  ub4 portcnt = net->portcnt;

  for (hop = 0; hop < hopcnt; hop++) {
    hp = hops + hop;

    dep = hp->dep;
    arr = hp->arr;
    error_ge(dep,portcnt);
    error_ge(arr,portcnt);
    error_eq(dep,arr);
    pdep = ports + dep;
    parr = ports + arr;

    ndep = pdep->ndep;
    narr = parr->narr;

    pdep->ndep = ndep+1;
    parr->narr = narr+1;

    if (hp->kind == Walk) continue;

    rrid = hp->rrid;

    nudep = pdep->nudep;
    nvdep = pdep->nvdep;
    nuarr = parr->nuarr;
    nvarr = parr->nvarr;

    if (ndep < Nlocal) {
      pdep->deps[ndep] = arr; pdep->drids[ndep] = rrid;
    }
    if (narr < Nlocal) {
      parr->arrs[narr] = dep; parr->arids[narr] = rrid;
    }

#if 0
    if (nudep == 0) {
      pdep->deps[0] = arr; pdep->nudep = pdep->nvdep = 1; pdep->drids[0] = rrid;
    } else if (nudep < Nlocal) {
      undx = nveq = 0;
      while (undx < nudep && pdep->deps[undx] != arr) {
        if (rrid != hi32 && (rrid & varmask) == (pdep->drids[undx] & varmask) ) nveq = 1;
        undx++;
      }
      if (undx == nudep) {
        pdep->deps[undx] = arr; pdep->nudep = undx + 1;
        pdep->drids[undx] = rrid;
        if (nveq == 0 || rrid == hi32) pdep->nvdep = nvdep + 1;
      }
    }
    if (nuarr == 0) {
      parr->arrs[0] = dep; parr->nuarr = parr->nvarr = 1; parr->arids[0] = rrid;
    } else if (nuarr < Nlocal) {
      undx = nveq = 0;
      while (undx < nuarr && parr->arrs[undx] != dep) {
        if (rrid != hi32 && (rrid & varmask) == (parr->arids[undx] & varmask) ) nveq = 1;
        undx++;
      }
      if (undx == nuarr) {
        parr->arrs[undx] = dep; parr->nuarr = undx + 1;
        parr->arids[undx] = rrid;
        if (nveq == 0 || rrid == hi32) parr->nvarr = nvarr + 1;
      }
    }
#endif

  }
  return 0;
}

struct hisort {
  ub4 cnt,rid1,rid2;
};

static int hicmp(const void *a,const void *b)
{
  struct hisort *aa = (struct hisort *)a;
  struct hisort *bb = (struct hisort *)b;

  if (aa->cnt < bb->cnt) return 1;
  else if (aa->cnt > bb->cnt) return -1;
  else return ((int)bb->rid1 - (int)aa->rid2);
}

int prepnet(netbase *basenet)
{
  struct network *net;
  struct gnetwork *gnet = getgnet();

  struct portbase *bports,*bpp;
  struct hopbase *bhops,*bhp;
  struct sidbase *bsids,*bsp;
  struct chainbase *bchains,*bcp;
  struct routebase *broutes,*brp;

  ub8 *bchainhops;
// todo events

  struct port *ports,*pports,*pdep,*parr,*pp,*gp;
  struct hop *hops,*phops,*hp,*ghp;
  struct sidtable *sids,*sp;
  struct chain *chains,*cp;
  struct route *routes,*rp;
  struct timepatbase *btp;
  struct timepat *tp;

  ub4 *portsbyhop;
  char *dname,*aname;

  ub4 bportcnt,portcnt,tportcnt;
  ub4 bhopcnt,hopcnt,nilhopcnt,pridcnt;
  ub4 dep,arr,depp,arrp;
  ub4 bsidcnt,sidcnt,bchaincnt,chaincnt,chainlen,chainhopcnt;
  ub4 bridcnt,ridcnt;
  ub4 nlen,cnt,acnt,dcnt,tcnt,sid,rid,rrid,part,tpart,xmaplen;
  ub4 pportcnt,zpportcnt,phopcnt,partcnt,part2,newpartcnt;
  enum txkind kind;
  ub4 hop,port,zport,zpport,chain,phop,pport;
  ub4 variants,varmask,hirrid;
  ub4 *hopcnts,*portcnts;
  ub4 *g2p,*p2g,*g2phop;
  ub4 *p2zp,*zp2p,*port2zport;
  struct partition *parts,*partp;
  ub4 *rrid2rid = basenet->rrid2rid;

  bhopcnt = basenet->hopcnt;
  bportcnt = basenet->portcnt;
  bsidcnt = basenet->sidcnt;
  bridcnt = basenet->ridcnt;
  bchaincnt = basenet->rawchaincnt;
  chainhopcnt = basenet->chainhopcnt;
  if (bportcnt == 0 || bhopcnt == 0) return error(0,"prepnet: %u ports, %u hops",bportcnt,bhopcnt);

  // filter but leave placeholder in gnet to make refs match
  ports = alloc(bportcnt,struct port,0,"ports",bportcnt);
  portcnt = 0;
  bports = basenet->ports;
  for (port = 0; port < bportcnt; port++) {
    bpp = bports + port;
    pp = ports + port;
    nlen = bpp->namelen;
    if (bpp->ndep == 0 && bpp->narr == 0) {
      info(0,"skip unconnected port %u %s",port,bpp->name);
      if (nlen) memcpy(pp->name,bpp->name,nlen);
      pp->namelen = nlen;
      continue;
    }

    // new ndep,narr filled lateron
    pp->valid = 1;
    pp->id = pp->allid = pp->gid = portcnt;
    pp->cid = bpp->cid;
    nlen = bpp->namelen;
    if (nlen) {
      memcpy(pp->name,bpp->name,nlen);
      pp->namelen = nlen;
    } else info(0,"port %u has no name", port);
    pp->lat = bpp->lat;
    error_z(pp->lat,port);
    pp->lon = bpp->lon;
    pp->rlat = bpp->rlat;
    pp->rlon = bpp->rlon;
    pp->utcofs = bpp->utcofs;
    portcnt++;
  }
  info(0,"%u from %u ports",portcnt,bportcnt);
  portcnt = bportcnt;

  sidcnt = bsidcnt;
  sids = alloc(sidcnt,struct sidtable,0,"sids",sidcnt);
  bsids = basenet->sids;
  for (sid = 0; sid < bsidcnt; sid++) {
    bsp = bsids + sid;
    sp = sids + sid;
    sp->sid = bsp->sid;
    sp->t0 = bsp->t0;
    sp->t1 = bsp->t1;
    nlen = bsp->namelen;
    if (nlen) {
      memcpy(sp->name,bsp->name,nlen);
      sp->namelen = nlen;
    }
  }
  info(0,"%u sids",sidcnt);

  ridcnt = bridcnt;
  routes = alloc(ridcnt,struct route,0,"routes",ridcnt);
  broutes = basenet->routes;
  for (rid = 0; rid < ridcnt; rid++) {
    brp = broutes + rid;
    rp = routes + rid;
    rp->rrid = brp->rrid;
    rp->chainofs = brp->chainofs;
    rp->chaincnt = brp->chaincnt;
    rp->hichainlen = brp->hichainlen;
    nlen = brp->namelen;
    if (nlen) {
      memcpy(rp->name,brp->name,nlen);
      rp->namelen = nlen;
    }
  }
  info(0,"%u routes",ridcnt);

  ub4 *gportsbyhop = alloc(bhopcnt * 2, ub4,0xff,"net portsbyhop",bhopcnt);
  ub4 *drids,*arids,*deps,*arrs;
  ub4 t0,t1,evcnt;

  hops = alloc(bhopcnt,struct hop,0,"hops",bhopcnt);
  hopcnt = 0;
  bhops = basenet->hops;
  for (hop = 0; hop < bhopcnt; hop++) {
    bhp = bhops + hop;
    dep = bhp->dep;
    arr = bhp->arr;
    error_ge(dep,portcnt);
    error_ge(arr,portcnt);
    if (dep == arr) {
      bpp = bports + dep;
      warning(0,"nil hop %u %s at %u",dep,bpp->name,bhp->cid);
      continue;
    } else if (bhp->valid == 0) {
      bpp = bports + dep;
      warning(0,"invalid hop %u %s at %u",dep,bpp->name,bhp->cid);
      continue;
    }

//    info(0,"hop %u %s at %u %u-%u",hop,bhp->name,bhp->cid,dep,arr);

    pdep = ports + dep;
    parr = ports + arr;
    error_z(pdep->valid,hop);
    error_z(parr->valid,hop);

    gportsbyhop[hop * 2] = dep;
    gportsbyhop[hop * 2 + 1] = arr;

    hp = hops + hop;
    nlen = bhp->namelen;
    if (nlen) {
      memcpy(hp->name,bhp->name,nlen);
      hp->namelen = nlen;
    }
    rrid = bhp->rrid;
    hp->rrid = rrid;
    rid = rrid2rid[rrid];
    hp->rid = rid;

    tp = &hp->tp;
    btp = &bhp->tp;
    t0 = btp->t0;
    t1 = btp->t1;
    evcnt = btp->evcnt;
//    warninfo(t1 == t0,0,"hop %u tt range %u-%u",hop,t0,t1);
    tp->utcofs = btp->utcofs;
    tp->tdays = btp->tdays;
    tp->gt0 = btp->gt0;
    tp->t0 = t0;
    tp->t1 = t1;
    tp->evcnt = evcnt;
    tp->genevcnt = btp->genevcnt;
    tp->evofs = btp->evofs;
    tp->dayofs = btp->dayofs;

    tp->lodur = btp->lodur;
    tp->hidur = btp->hidur;
    tp->midur = btp->midur;
    tp->duracc = btp->duracc;

    // mark local links
    pdep = ports + dep;
    parr = ports + arr;
    dcnt = pdep->ndep;
    deps = pdep->deps;
    drids = pdep->drids;
    if (dcnt == 0) {
      deps[0] = hop;
      drids[0] = rid;
      pdep->ndep = 1;
    } else if (dcnt == 1) {
      if (deps[0] != hop || drids[0] != rid) {
        deps[1] = hop;
        drids[1] = rid;
        pdep->ndep = 2;
      }
    } else if ( (deps[0] == hop && drids[0] == rid) || (deps[1] == hop && drids[1] == rid) ) ;
    else pdep->ndep = dcnt + 1;

    acnt = parr->narr;
    arrs = parr->arrs;
    arids = parr->arids;
    if (acnt == 0) {
      arrs[0] = hop;
      arids[0] = rid;
      parr->narr = 1;
    } else if (acnt == 1) {
      if (arrs[0] != hop || arids[0] != rid) {
        arrs[1] = hop;
        arids[1] = rid;
        parr->narr = 2;
      }
    } else if ( (arrs[0] == hop && arids[0] == rid) || (arrs[1] == hop && arids[1] == rid) ) ;
    else parr->narr = acnt + 1;

    kind = bhp->kind;
    hp->kind = kind;

    hp->dist = bhp->dist;

    hp->dep = dep;
    hp->arr = arr;
    hopcnt++;
  }
  if (hopcnt == 0) return error(0,"nil hops out of %u",bhopcnt);
  info(0,"%u from %u hops",hopcnt,bhopcnt);
  nilhopcnt = bhopcnt - hopcnt;
  hopcnt = bhopcnt;

  info0(0,"global connectivity");
  showconn(ports,portcnt,0);

  chains = alloc(bchaincnt,struct chain,0,"chains",bchaincnt);
  bchains = basenet->chains;
  bchainhops = basenet->chainhops;
  chaincnt = 0;
  for (chain = 0; chain < bchaincnt; chain++) {
    bcp = bchains + chain;
    cp = chains + chain;
    cnt = bcp->hopcnt;
    if (cnt < 3) { vrb(0,"skip dummy chain %u with %u hop\as",chain,cnt); continue; }
    cp->hopcnt = cnt;
    cp->rrid = bcp->rrid;
    cp->rid = bcp->rid;
    cp->hopofs = bcp->hopofs;
    chaincnt++;
  }
  info(0,"%u from %u chains",chaincnt,bchaincnt);
  chaincnt = bchaincnt;

  gnet->portcnt = portcnt;
  gnet->hopcnt = hopcnt;
  gnet->sidcnt = sidcnt;
  gnet->chaincnt = chaincnt;
  gnet->ridcnt = ridcnt;

  gnet->ports = ports;
  gnet->hops = hops;
  gnet->sids = sids;
  gnet->chains = chains;
  gnet->routes = routes;

  gnet->portsbyhop = gportsbyhop;

  gnet->routechains = basenet->routechains;
  gnet->chainhops = bchainhops;
  gnet->chainmets = basenet->chainmets;
  gnet->chainhopcnt = chainhopcnt;

  gnet->maxvariants = variants = basenet->maxvariants;
  gnet->routevarmask = varmask = basenet->routevarmask;
  gnet->hichainlen = basenet->hichainlen;

  gnet->eventmem = &basenet->eventmem;
  gnet->evmapmem = &basenet->evmapmem;
  gnet->events = basenet->events;
  gnet->evmaps = basenet->evmaps;

  // write reference for name lookup
  if (wrportrefs(basenet)) return 1;

  if (condense(gnet)) return 1;

  // prepare partitioning
  hopcnts = gnet->hopcnts;
  portcnts = gnet->portcnts;
  parts = gnet->parts;

  port2zport = gnet->port2zport;
  error_zp(port2zport,0);

  hirrid = basenet->hirrid;

/*
  each port has 1k list of memberships
  start with each port is in partno (portno or rid)
  repeat {
    for each link a->b  a.members += b.members and vice versa
    until list full or iter > x
  }
  mark portcnt per memberno
  mark himembers per port
  [rid1,rid] = number of ports in both rid1 and 2
  repeat merge highest combi until number of discrete rid2 = number of aimed parts
  optional, filter per port memberships with lower count
  plus separate partitions H with hi-conn or high-partmember ports
  such that each part has at least one port member of these H
  and each H has ports from each part
 */

  // todo configurable
  ub4 aimpartsize = 1500;
  ub4 aimcnt = max(3,portcnt / aimpartsize);

  // todo do not use Npart below for intermediate calcs
  error_ge(ridcnt,Npart);

  info(0,"partitioning %u ports from %u routes into estimated %u parts",portcnt,ridcnt,aimcnt);

  // todo: use initial part < rid, and inipartcnt * inipartcnt instead
  ub4 ridrid = ridcnt * ridcnt;

  // todo ub2 and inipartcnt aka 'rid'cnt instead of Npart
  ub4 *portparts = alloc(Npart * portcnt,ub4,0xff,"part portparts",portcnt);
  ub4 *lportparts = alloc(Npart * portcnt,ub4,0xff,"part portparts",portcnt);
  ub4 *mpp,*dmpp,*ampp,*lmpp;

  ub2 *ppm,*partportcnts = alloc(ridcnt * portcnt,ub2,0,"part partportcnts",portcnt);
  ub2 *lppm,*dppm,*appm,*lpartportcnts = alloc(ridcnt * portcnt,ub2,0,"part partportcnts",portcnt);
  ub4 *portsperpart = alloc(ridcnt,ub4,0,"part portsperpart",portcnt);

  ub4 *ridcnts = alloc(ridcnt,ub4,0,"part ridcnts",ridcnt);
  ub1 *ridhis = alloc(ridcnt,ub1,0,"part ridhis",ridcnt);
  ub4 *partmerges = alloc(ridcnt,ub4,0,"part partmerges",ridcnt);
  ub4 *rid2part = alloc(ridcnt,ub4,0,"part rid2part",ridcnt);

  ub4 hino,nhino;

  struct hisort *hisorts  = alloc(ridrid,struct hisort,0,"part",ridcnt);

  ub4 *partcnts = alloc(portcnt,ub4,0,"part partcnts",portcnt);
  ub4 *lpartcnts = alloc(portcnt,ub4,0,"part lpartcnts",portcnt);
  ub4 *itercnts = alloc(portcnt,ub4,0,"part itercnts",portcnt);

  ub4 *rid2cnts = alloc(ridrid,ub4,0,"part rid2cnts",ridcnt);
  ub4 *lrid2cnts = alloc(ridrid,ub4,0,"part lrid2cnts",ridcnt);

  ub4 mi,dmi,ami,cnt2,lcnt,sumcnt,orgacnt,orgdcnt,prvsumcnt,nstop,iter;
  ub4 rid1,rid2;
  ub4 ppartcnt,fullcnt;

  /* start with each port as member of rid any hop is on
   * todo: group into small number of nearby rids ?
   */
  for (hop = 0; hop < hopcnt; hop++) {
    hp = hops + hop;
    dep = hp->dep;
    arr = hp->arr;
    if (dep == arr) continue;

    rid = hp->rid;

    cnt = partcnts[dep];
    if (cnt < Npart - 1) {
      mpp = portparts + dep * Npart;
      mi = 0;
      while (mi < cnt && mpp[mi] != rid) mi++;
      if (mi == cnt) { mpp[mi] = rid; partcnts[dep] = cnt + 1; }
    }

    cnt = partcnts[arr];
    if (cnt == Npart - 1) continue;

    mpp = portparts + arr * Npart;
    mi = 0;
    while (mi < cnt && mpp[mi] != rid) mi++;
    if (mi == cnt) { mpp[mi] = rid; partcnts[arr] = cnt + 1; }
  }

  sumcnt = 0;
  for (port = 0; port < portcnt; port++) {
    pdep = ports + port;
//    info(0,"port %u cnt %u %s",port,partcnts[port],pdep->name);
    sumcnt += partcnts[port];
  }

  // check duplicates
  for (port = 0; port < portcnt; port++) {
    cnt = partcnts[port];
    mpp = portparts + port * Npart;

    for (mi = 0; mi < cnt; mi++) {
      rid = mpp[mi];
      error_ge(rid,ridcnt);
      for (ami = 0; ami < cnt; ami++) {
        if (ami != mi && mpp[ami] == rid) warning(0,"port %u dup rid %u at pos %u and %u",port,rid,mi,ami);
      }
    }
  }

  info(0,"pass 1, sum %u",sumcnt);

  // add members from each hop peer port, both directions
  nstop = prvsumcnt = iter = 0;

  struct eta eta;
  ub4 cntlimit = 0;

  while (iter < 10 && sumcnt < Npart * portcnt / 2) {

    for (hop = 0; hop < hopcnt; hop++) {

      progress(&eta,"assign initial parts hop \ah%u of \ah%u iter %u %u-stop sum \ah%u",hop,hopcnt,iter,nstop,sumcnt);

      hp = hops + hop;
      dep = hp->dep;
      arr = hp->arr;
      if (dep == arr) continue;

      dcnt = partcnts[dep];
      acnt = partcnts[arr];

      if (dcnt == Npart - 1 && acnt == Npart - 1) {
        info(0,"end initial part assign at hop %u dep %u arr %u",hop,dep,arr);
        break;
      } else if (dcnt == Npart - 1 || acnt == Npart - 1) {
        cntlimit++;
        if (cntlimit > hopcnt / 100) {
          info(0,"end initial part assign at hop %u dep %u arr %u",hop,dep,arr);
          break;
        }
      }

      dmpp = portparts + dep * Npart;
      ampp = portparts + arr * Npart;
      orgacnt = acnt;
      orgdcnt = dcnt;
      if (itercnts[dep] <= iter) {
        dmi = 0;
        while (dmi < dcnt && acnt < Npart - 1) {
          rid = dmpp[dmi++];
          error_ge(rid,ridcnt);
          ami = 0;
          while (ami < acnt && ampp[ami] != rid) ami++;
          if (ami == acnt) ampp[acnt++] = rid;
        }
        if (acnt > orgacnt) {
          partcnts[arr] = acnt;
          itercnts[dep]++;
        }
      }

      if (itercnts[arr] <= iter) {
        ami = 0;
        while (ami < orgacnt && dcnt < Npart - 1) {
          rid = ampp[ami++];
          error_ge(rid,ridcnt);
          dmi = 0;
          while (dmi < dcnt && dmpp[dmi] != rid) dmi++;
          if (dmi == dcnt) dmpp[dcnt++] = rid;
        }
        if (dcnt > orgdcnt) {
          partcnts[dep] = dcnt;
          itercnts[arr]++;
        }
      }
    } // each hop

    prvsumcnt = sumcnt;
    sumcnt = 0;
    for (port = 0; port < portcnt; port++) {
      sumcnt += partcnts[port];
//      info(0,"cnt %u iter %u",partcnts[port],itercnts[port]);
    }
    if (sumcnt == prvsumcnt) break;
    iter++;

    // use this lower connectivity to estimate resulting ports per part
    if (iter == 1) {
      memcpy(lportparts,portparts,portcnt * Npart * sizeof(ub4));
      memcpy(lpartcnts,partcnts,portcnt * sizeof(ub4));
    }
  }

  ub4 partstats[Npart / 4];
  ub4 iv,cumcnt,partivs = Elemcnt(partstats) - 1;
  ub4 partstats2[4096];
  ub4 partiv2s = Elemcnt(partstats2) - 1;

  aclear(partstats);

  // check duplicates
  for (port = 0; port < portcnt; port++) {
    cnt = partcnts[port];
    mpp = portparts + port * Npart;

    for (mi = 0; mi < cnt; mi++) {
      rid = mpp[mi];
      error_ge(rid,ridcnt);
      for (ami = 0; ami < cnt; ami++) {
        if (ami != mi && mpp[ami] == rid) info(0,"port %u dup rid %u at pos %u and %u",port,rid,mi,ami);
      }
    }
  }

//  mark portcnt per memberno and stat
  for (port = 0; port < portcnt; port++) {
    progress(&eta,"port %u of %u iter 0 pass 1 parts %u",port,portcnt,ridcnt);
    cnt = partcnts[port];
    mpp = portparts + port * Npart;
    ppm = partportcnts + port * ridcnt;
    for (mi = 0; mi < cnt; mi++) {
      rid = mpp[mi];
      error_ge(rid,ridcnt);
      ridcnts[rid]++;
      ppm[rid] = 1;
    }
    for (mi = 0; mi < cnt; mi++) {
      rid1 = mpp[mi];
      for (rid2 = 0; rid2 < ridcnt; rid2++) {
        if (rid2 > rid1 && ppm[rid2]) {
          error_ge_cc(rid1 * ridcnt + rid2,ridrid,"rid %u rid2 %u",rid1,rid2);
          cnt2 = rid2cnts[rid1 * ridcnt + rid2];
          if (cnt2 == portcnt - 1) info(0,"port %u cnt %u for rid,rid %u,%u at %u",port,cnt,rid1,rid2,mi);
          error_ge(cnt2,portcnt);
          rid2cnts[rid1 * ridcnt + rid2] = cnt2 + 1;
        }
      }
    }

    lcnt = lpartcnts[port];
    lmpp = lportparts + port * Npart;
    lppm = lpartportcnts + port * ridcnt;
    for (mi = 0; mi < lcnt; mi++) {
      rid = lmpp[mi];
      error_nz(lppm[rid],rid);
      error_ge(rid,ridcnt);
      lppm[rid] = 1;
      portsperpart[rid]++;
    }
    for (mi = 0; mi < lcnt; mi++) {
      rid1 = lmpp[mi];
      for (rid2 = 0; rid2 < ridcnt; rid2++) {
        if (rid2 > rid1 && lppm[rid2]) lrid2cnts[rid1 * ridcnt + rid2]++;
      }
    }

    pdep = ports + port;
    vrb0(0,"cnt %u iter %u %s",cnt,itercnts[port],pdep->name);
    error_ge(cnt,Npart);
    partstats[min(cnt,partivs)]++;
  }

  for (rid = 0; rid < ridcnt; rid++) {
    pportcnt = portsperpart[rid];
    error_gt(pportcnt,portcnt,rid);
    if (pportcnt > 3000) info(0,"rid %u ports %u",rid,pportcnt);
  }

  cumcnt = 0;
  for (iv = 0; iv <= partivs; iv++) {
    cnt = partstats[iv];
    cumcnt += cnt;
//    if (cnt) info(0,"parts %u cnt %u sum %u",iv,cnt,cumcnt);
  }

/* search N sets of 2 parts with highest number of shared ports
   with each set on different parts
   [rid1,rid] = number of ports in both rid1 and 2
   sets are symmetrical : rid1 < rid2 is used
 */

  partcnt = ridcnt;

  for (rid = 0; rid < ridcnt; rid++) {
    ridhis[rid] = 0;
    partmerges[rid] = rid;
  }

//  repeat merge highest combi until number of discrete rid2 = number of aimed parts
  iter = 0;
  part2 = partcnt * partcnt;

  while (partcnt > aimcnt && iter++ < 100) {

    newpartcnt = partcnt;

    msgprefix(0,"iter %u",iter);

    for (rid = 0; rid < ridcnt; rid++) {
      if (partmerges[rid] != rid) ridhis[rid] = 1;
      else if (portsperpart[rid] > 2900) { info(0,"rid %u ports %u",rid,portsperpart[rid]); ridhis[rid] = 1; }
      else ridhis[rid] = 0;
    }

    nhino = part2;

    info(0,"prepare %u sets",nhino);
    hino = 0;
    for (rid1 = 0; rid1 < ridcnt; rid1++) {
      if (ridhis[rid1]) continue;
      for (rid2 = 0; rid2 < ridcnt; rid2++) {
        if (rid2 <= rid1 || ridhis[rid2] /* || rid2his[rid1 * ridcnt + rid2] */) continue;
        hisorts[hino].cnt = rid2cnts[rid1 * ridcnt + rid2];
        hisorts[hino].rid1 = rid1;
        hisorts[hino].rid2 = rid2;
        hino++;
      }
    }
    error_gt(hino,nhino,ridcnt);

    info(0,"sort %u from %u sets",hino,nhino);
    nhino = hino;

    qsort(hisorts,hino,sizeof(struct hisort),hicmp);
    info(0,"filter %u sets",hino);

    // select sets to merge, excluding any part chosen previously
    fullcnt = 0;
    for (hino = 0; hino < nhino; hino++) {
      rid1 = hisorts[hino].rid1;
      rid2 = hisorts[hino].rid2;
      cnt = hisorts[hino].cnt;

      if (rid1 == rid2 || ridhis[rid1] || ridhis[rid2]) continue;
      if (iter == 8) info(0,"rid1 %u rid2 %u cnt  %u his %u %u",rid1,rid2,cnt,ridhis[rid1],ridhis[rid2]);
//      if (rid2his[rid1 * ridcnt + rid2]) continue;

//      if (hino < 12) info(0,"rid1 %u rid2 %u cnt %u",rid1,rid2,cnt);

      cnt2 = lrid2cnts[rid1 * ridcnt + rid2];
      if (iter == 9) info(0,"rid1 %u rid2 %u cnt2 %u",rid1,rid2,cnt2);
      error_gt(cnt2,portsperpart[rid1],rid1);
      error_gt(cnt2,portsperpart[rid2],rid2);
      error_gt(portsperpart[rid1],portcnt,rid1);
      error_gt(portsperpart[rid2],portcnt,rid2);
      pportcnt = portsperpart[rid1] + portsperpart[rid2] - cnt2;
      if (pportcnt < 3000) {
        partmerges[rid2] = rid1;
        newpartcnt--;
        ridhis[rid1] = ridhis[rid2] = 1;
        vrb0(0,"hi %u 10-%u 1-%u ports %u merge rid %u to %u",hino,cnt,cnt2,pportcnt,rid2,rid1);
      } else {
        vrb0(0,"hi %u 10-%u 1-%u ports %u no merge rid %u to %u",hino,cnt,cnt2,pportcnt,rid2,rid1);
//        rid2his[rid1 * ridcnt + rid2] = 1;
        fullcnt++;
      }
    }
    info(0,"parts %u from %u, %u full",newpartcnt,partcnt,fullcnt);
    if (newpartcnt == partcnt) {
      info(0,"end iter %u on zero out of %u groups parts %u",iter,nhino,partcnt);
      break;
    }

    // merge
    aclear(partstats);
    for (port = 0; port < portcnt; port++) {

      if (progress(&eta,"pass 1 port %u of %u parts %u",port,portcnt,partcnt)) return 1;

      cnt = partcnts[port];
      if (cnt == 0) continue;

      mpp = portparts + port * Npart;
      ppm = partportcnts + port * ridcnt;  // ppm is updated in place

      mi = 0;
      while(mi < cnt) {
        rid = mpp[mi];
        error_ge(rid,ridcnt);
        rid2 = partmerges[rid];
        error_z(ppm[rid],rid);
        if (rid == rid2 || rid2 == hi32) { mi++; continue; }
        if (ppm[rid2]) {
          error_eq(cnt,1);
          if (mi + 1 < cnt) mpp[mi] = mpp[--cnt]; // replace with last if have both
          else cnt--;
        } else { mpp[mi] = rid2; ppm[rid2] = 1; }
        ppm[rid] = 0;
      }
      partcnts[port] = cnt;

      lcnt = lpartcnts[port];
      lmpp = lportparts + port * Npart;
      lppm = lpartportcnts + port * ridcnt;
      error_z(lcnt,port);

      mi = 0;
      while(mi < lcnt) {
        rid = lmpp[mi];
        error_ge(rid,ridcnt);
        error_z(lppm[rid],rid);
        rid2 = partmerges[rid];
        if (rid == rid2) { mi++; continue; }
        else if (rid2 == hi32) { warning(0,"port %u stale rid %u at %u of %u",port,rid,mi,lcnt); mi++; continue; }
        error_ge(rid,ridcnt);
        if (lppm[rid2]) {
          error_eq(lcnt,1);
          if (rid == 2072 || port == 209) info(0,"port %u merge rid %u to %u at %u of %u",port,rid,rid2,mi,lcnt);
          if (mi + 1 < lcnt) lmpp[mi] = lmpp[--lcnt];
          else lcnt--;
        } else {
          if (rid == 2072 || port == 209) info(0,"port %u merge rid %u to %u at %u of %u",port,rid,rid2,mi,lcnt);
          lmpp[mi] = rid2; lppm[rid2] = 1;
        }
        lppm[rid] = 0;
      }
      lpartcnts[port] = lcnt;
      partstats[min(partivs,lcnt)]++;
      if (port == 209) info(0,"port %u in %u parts",port,lcnt);
    }

    lcnt = partstats[0];
    if (lcnt) warning(0,"%u port\as in no partition",lcnt);
    lcnt = partstats[1];
    genmsg(lcnt ? Info : Warn,0,"%u port\as in 1 partition",lcnt);
    for (iv = 2; iv < partivs; iv++) {
      lcnt = partstats[iv];
      if (lcnt > portcnt / 100) info(0,"%u port\as in %u partition\as each", lcnt,iv);
    }
    lcnt = partstats[iv];
    if (lcnt) info(0,"%u port\as in %u+ partitions each", lcnt,iv);

    // mark as done for next iters
    for (rid = 0; rid < ridcnt; rid++) {
      if (partmerges[rid] != rid) {
        partmerges[rid] = hi32;
      }
    }

    partcnt = newpartcnt;
    part2 = partcnt * partcnt;

    // mark for next iter
    nclear(rid2cnts,ridrid);
    nclear(lrid2cnts,ridrid);
    nclear(portsperpart,ridcnt);

    for (port = 0; port < portcnt; port++) {
      if (progress(&eta,"pass 2 port %u of %u parts %u",port,portcnt,partcnt)) return 1;

      cnt = partcnts[port];
      if (cnt == 0) continue;

      mpp = portparts + port * Npart;
      ppm = partportcnts + port * ridcnt;

      for (mi = 0; mi < cnt; mi++) {
        rid = mpp[mi];
        error_ge(rid,ridcnt);
        error_z(ppm[rid],rid);
        ppm[rid] = 0;
      }
      for (mi = 0; mi < cnt; mi++) {
        rid = mpp[mi];
        error_nz(ppm[rid],rid);
        ppm[rid] = 1;
      }
      for (mi = 0; mi < cnt; mi++) {
        rid1 = mpp[mi];
        for (rid2 = 0; rid2 < ridcnt; rid2++) {
          if (rid2 > rid1 && ppm[rid2]) rid2cnts[rid1 * ridcnt + rid2]++;
        }
      }

      lcnt = lpartcnts[port];
      lmpp = lportparts + port * Npart;
      lppm = lpartportcnts + port * ridcnt;

      for (mi = 0; mi < lcnt; mi++) {
        rid = lmpp[mi];
        error_ge(rid,ridcnt);
        error_z(lppm[rid],rid);
        lppm[rid] = 0;
      }
      for (mi = 0; mi < lcnt; mi++) {
        rid = lmpp[mi];
        error_nz(lppm[rid],rid);
        lppm[rid] = 1;
      }
      for (mi = 0; mi < lcnt; mi++) {
        rid1 = lmpp[mi];
        error_ne_cc(partmerges[rid1],rid1,"port %u rid %u at %u of %u",port,rid1,mi,lcnt);
        error_ne_cc(lppm[rid1],1,"port %u lcnt %u pos %u",port,lcnt,mi);
//        if (rid1 == 1101) info(0,"port %u rid %u ppp %u",port,rid1,portsperpart[rid1]);
        pportcnt = portsperpart[rid1];
        error_ge(pportcnt,portcnt);
        portsperpart[rid1] = pportcnt + 1;
        for (rid2 = 0; rid2 < ridcnt; rid2++) {
          if (rid2 > rid1 && lppm[rid2]) lrid2cnts[rid1 * ridcnt + rid2]++;
        }
      }
    }
    aclear(partstats2);
    for (rid = 0; rid < ridcnt; rid++) {
      if (partmerges[rid] != rid) continue;
      cnt = portsperpart[rid];
      partstats2[min(partiv2s,cnt)]++;
    }
    cnt = partstats2[0];
    if (cnt) warning(0,"%u part\as with no ports",cnt);
    for (iv = 1; iv < partiv2s; iv++) {
      cnt = partstats2[iv];
      if (cnt) info(0,"%u part\as with %u port\as each", cnt,iv);
    }
    cnt = partstats2[partiv2s];
    if (cnt) info(0,"%u part\as with %u+ ports each", cnt,iv);

  } // while partcnt < aimed

  msgprefix(0,NULL);

  error_ge(partcnt,Npart);

  // assemble results
  part = sumcnt = 0;
  for (rid = 0; rid < ridcnt; rid++) {
    rid2part[rid] = hi32;
    rid2 = partmerges[rid];
    if (rid2 != hi32) {
      error_ne(rid,rid2);
      cnt = portsperpart[rid2];
      sumcnt += cnt;
      info(0,"part %u ports %u sum %u",part,cnt,sumcnt);
      rid2part[rid] = part++;
    }
  }
  error_ne(part,partcnt);

  tpart = partcnt;

  aclear(partstats);

  iv = cnt2 = 0;
  for (port = 0; port < portcnt; port++) {
    cnt = lpartcnts[port];
    partstats[min(partivs,cnt)]++;
  }

  for (iv = 0; iv <= partivs; iv++) {
    cnt2 = 0;
    for (port = 0; port < portcnt; port++) {
      cnt = lpartcnts[port];
      if (cnt != iv) continue;
      pdep = ports + port;
      info(0,"port %u in %u part\as %u deps %u arrs %s",port,cnt,pdep->ndep,pdep->narr,pdep->name);
      if (cnt2++ == 2) break;
    }
  }

  portcnts = gnet->portcnts;

  cnt = partstats[0];
  if (cnt) warning(0,"%u port\as in no partition",cnt);
  cnt = partstats[1];
  genmsg(cnt ? Info : Warn,0,"%u port\as in 1 partition",cnt);

  for (iv = 2; iv <= partivs; iv++) {
    cnt = partstats[iv];
    if (cnt) info(0,"%u port\as in %u partition\as%s each", cnt,iv,iv == partivs ? "+" : "");
  }

  ub4 hpcnt2,hxcnt2;
  ub4 dist,*hopdist;
  ub4 midur,*hopdur;

  // resequence
  for (port = 0; port < portcnt; port++) {
    lcnt = lpartcnts[port];
    lmpp = lportparts + port * Npart;
    lppm = lpartportcnts + port * ridcnt;
//    pdep = ports + port;

    nclear(lppm,partcnt);
    for (mi = 0; mi < lcnt; mi++) {
      rid1 = lmpp[mi];
      error_ge(rid1,ridcnt);
      part = rid2part[rid1];
      if (part == hi32) {
        info(0,"port %u rid %u at %u of %u in no part",port,rid1,mi,lcnt);
        lmpp[mi] = hi32;
        continue;
      }
      error_ge_cc(part,partcnt,"port %u rid %u",port,rid1);
      lmpp[mi] = part;
      portcnts[part]++;
    }
    for (mi = 0; mi < lcnt; mi++) { // check dups
      part = lmpp[mi];
      if (part == hi32) continue;
      error_ge(part,partcnt);
      error_ne(lppm[part],0);
      lppm[part] = 1;
    }
  }

  ub4 prostats[8];
  aclear(prostats);

  // determine hop membership, promote ports to global
  for (hop = 0; hop < hopcnt; hop++) {
    hp = hops + hop;
    dep = hp->dep;
    arr = hp->arr;
    if (dep == arr) continue;

    dcnt = lpartcnts[dep];
    acnt = lpartcnts[arr];
    dmpp = lportparts + dep * Npart;
    ampp = lportparts + arr * Npart;
    dppm = lpartportcnts + dep * ridcnt;
    appm = lpartportcnts + arr * ridcnt;

    if (dppm[tpart] && appm[tpart]) continue;

    // for now, promote ports directly. possibly refine by promoting a shared connected port instead
    mi = 0;
    while (mi < dcnt && appm[dmpp[mi]]) mi++;
    if (dcnt && mi < dcnt && dmpp[mi] != tpart) { // promote on dpart not in apart
      if (dcnt >= Npart - 1) warning(0,"hop %u dep %u in %u parts",hop,dep,dcnt);
      else if (dppm[tpart] == 0) {
        prostats[1]++;
        dmpp[dcnt] = tpart; lpartcnts[dep] = dcnt + 1; dppm[tpart] = 1;
        portcnts[tpart]++;
      }
      if (acnt >= Npart - 1) warning(0,"hop %u arr %u in %u parts",hop,arr,acnt);
      else if (appm[tpart] == 0) {
        prostats[2]++;
        ampp[acnt] = tpart; lpartcnts[arr] = acnt + 1; appm[tpart] = 1;
        portcnts[tpart]++;
      }
    }
    mi = 0;
    while (mi < acnt && dppm[ampp[mi]]) mi++;
    if (acnt && mi < acnt && ampp[mi] != tpart) { // promote on apart not in dpart
      if (acnt >= Npart - 1) warning(0,"hop %u arr %u in %u parts",hop,arr,acnt);
      else if (appm[tpart] == 0) {
        prostats[3]++;
        ampp[acnt] = tpart; lpartcnts[arr] = acnt + 1; appm[tpart] = 1;
        portcnts[tpart]++;
      }
      if (dcnt >= Npart - 1) warning(0,"hop %u dep %u in %u parts",hop,dep,dcnt);
      else if (dppm[tpart] == 0) {
        prostats[4]++;
        dmpp[dcnt] = tpart; lpartcnts[dep] = dcnt + 1; dppm[tpart] = 1;
        portcnts[tpart]++;
      }
    }
  }

  for (iv = 0; iv < Elemcnt(prostats); iv++) if (prostats[iv]) info(0,"promote %u: %u",iv,prostats[iv]);
  info(0,"%u ports promoted to top part",portcnts[tpart]);

  ub1 partsingpart[Npart];
  ub4 hiconports[Npart];
  ub4 conhiports[Npart];

  aclear(partsingpart);
  aclear(hiconports);
  aclear(conhiports);

  // make all parts represented in topnet
  for (port = 0; port < portcnt; port++) {
    pp = ports + port;
    lcnt = lpartcnts[port];
    lmpp = lportparts + port * Npart;
    lppm = lpartportcnts + port * ridcnt;

    if (lppm[tpart] == 0) continue;

    ppartcnt = 0;
    for (part = 0; part < partcnt; part++) {
      if (lmpp[part] == 0) continue;
      partsingpart[part] = 1;
      ppartcnt++;
    }
    cnt = pp->ndep + pp->narr + ppartcnt;
    for (part = 0; part < partcnt; part++) {
      if (lmpp[part] == 0) continue;
      if (cnt > conhiports[part]) { conhiports[part] = cnt; hiconports[part] = port; }
    }
  }

  for (part = 0; part < partcnt; part++) {
    if (partsingpart[part]) continue;
    port = hiconports[part];
    pp = ports + port;
    info(0,"add part %u to global by port %u with %u dep %u arr %u parts",part,port,pp->ndep,pp->narr,pp->partcnt);
    lcnt = lpartcnts[port];
    lmpp = lportparts + port * Npart;
    lppm = lpartportcnts + port * ridcnt;

    error_nz(lppm[tpart],port);
    if (lcnt >= Npart - 1) warning(0,"port %u in %u parts",port,lcnt);
    else {
        lmpp[lcnt] = tpart; lpartcnts[port] = lcnt + 1; lmpp[tpart] = 1;
        portcnts[tpart]++;
    }
  }

  info(0,"%u ports in top part after part rep",portcnts[tpart]);

  partcnt++;  // add gpart

  ub1 *gportparts = alloc(partcnt * portcnt,ub1,0,"part portparts",portcnt);

  // set portparts
  for (port = 0; port < portcnt; port++) {
    pp = ports + port;
    cnt = lpartcnts[port];
    if (cnt == 0) {
      info(0,"port %u not in any part %s",port,pp->name);
      continue;
    }

    mpp = lportparts + port * Npart;
    for (mi = 0; mi < cnt; mi++) {
      part = mpp[mi];
      gportparts[port * partcnt + part] = 1;
    }
    for (part = 0; part < partcnt; part++) if (gportparts[port * partcnt + part]) pp->partcnt++;
  }

#if 0
  ub4 hicnt,hipart,*ridparts = alloc(partcnt,ub4,0,"part ridparts",partcnt);

  // make ports on each route share parts

  for (rid = 0; rid < ridcnt; rid++) {
    chainlen = 0;
    for (hop = 0; hop < hopcnt; hop++) {
      hp = hops + hop;
      if (hp->rid != rid) continue;
      chainlen++;
      dep = hp->dep; arr = hp->arr;
      for (part = 0; part < tpart; part++) {
        if (gportparts[dep * partcnt + part]) ridparts[part]++;
        if (gportparts[arr * partcnt + part]) ridparts[part]++;
      }
    }
    if (chainlen < 3) continue;

    hicnt = hipart = 0;
    for (part = 0; part < tpart; part++) {
      cnt = ridparts[part];
      if (cnt > hicnt) { hicnt = cnt; hipart = part; }
    }
    for (hop = 0; hop < hopcnt; hop++) {
      hp = hops + hop;
      if (hp->rid != rid) continue;
      dep = hp->dep; arr = hp->arr;
      if (gportparts[dep * partcnt + hipart] == 0) { gportparts[dep * partcnt + hipart] = 1; portcnts[hipart]++; }
      if (gportparts[arr * partcnt + hipart] == 0) { gportparts[arr * partcnt + hipart] = 1; portcnts[hipart]++; }
    }
  }
#endif

  // add hi-conn ports to top
  ub4 hidcon = 0;
  for (port = 0; port < portcnt; port++) {
    if (gportparts[port * partcnt + tpart]) continue;
    pp = ports + port;
    cnt = pp->ndep + pp->narr;
    hidcon = max(hidcon,cnt);
  }
  error_z(hidcon,0);
  hidcon = max(hidcon,2);

  ub4 portcon,*portconns = alloc(hidcon+1,ub4,0,"part portconns",hidcon);
  ub4 hiaddcnt,hiportcnt,coniv;

  for (port = 0; port < portcnt; port++) {
    if (gportparts[port * partcnt + tpart]) continue;
    pp = ports + port;
    cnt = pp->ndep + pp->narr;
    portconns[cnt]++;
  }

  if (portcnts[tpart] < aimpartsize * 2) hiaddcnt = 500;
  else hiaddcnt = 100;
  hiportcnt = 0; coniv = hidcon;

  while (hiportcnt < hiaddcnt && coniv > 2) { hiportcnt += portconns[coniv--]; }

  for (port = 0; port < portcnt; port++) {
    if (gportparts[port * partcnt + tpart]) continue;
    pp = ports + port;
    cnt = pp->ndep + pp->narr;
    if (cnt <= coniv) continue;
    gportparts[port * partcnt + tpart] = 1;
    portcnts[tpart]++;
  }

  ub4 hiacon,hicondep,hiconarr;

  // share connecting ports if needed for part connectivity
  for (part = 0; part < partcnt; part++) {
    for (port = 0; port < portcnt; port++) {
      if (gportparts[port * partcnt + part] == 0) continue;
      pp = ports + port;

      dcnt = acnt = hidcon = hiacon = 0; hicondep = hiconarr = hi32;
      for (hop = 0; hop < hopcnt; hop++) {
        dep = gportsbyhop[hop * 2];
        arr = gportsbyhop[hop * 2 + 1];
        if (port == dep || port == arr) {
          if (gportparts[dep * partcnt + part] && gportparts[arr * partcnt + part]) {
            if (port == dep) dcnt++; else acnt++;
          } else {
            if (gportparts[dep * partcnt + part] == 0) {
              pdep = ports + dep;
              cnt = pdep->ndep + pdep->narr;
              if (cnt > hidcon) { hidcon = cnt; hicondep = dep; }
            }
            if (gportparts[arr * partcnt + part] == 0) {
              parr = ports + arr;
              cnt = parr->ndep + parr->narr;
              if (cnt > hiacon) { hiacon = cnt; hiconarr = arr; }
            }
          }
        }
      }
      if ( (pp->ndep && dcnt == 0) || (pp->narr && acnt == 0) ) {
        if (hicondep != hi32) {
          if (gportparts[hicondep * partcnt + part] == 0) {
            info(0,"add port %u to part %u with conn %u",hicondep,part,hidcon);
            gportparts[hicondep * partcnt + part] = 1;
            portcnts[part]++;
          }
        }
        if (hiconarr != hi32) {
          if (gportparts[hiconarr * partcnt + part] == 0) {
            info(0,"add port %u to part %u with conn %u",hiconarr,part,hiacon);
            gportparts[hiconarr * partcnt + part] = 1;
            portcnts[part]++;
          }
        }
      }
    }
  }

  // count part hops
  for (hop = 0; hop < hopcnt; hop++) {
    dep = gportsbyhop[hop * 2];
    arr = gportsbyhop[hop * 2 + 1];
    if (dep == arr) continue;

    // make sure no hop gets lost
    cnt = 0;
    for (part = 0; part < partcnt; part++) {
      if (gportparts[dep * partcnt + part] && gportparts[arr * partcnt + part]) {
        hopcnts[part]++;
        cnt++;
      }
    }
    error_z(cnt,hop);
  }

  for (part = 0; part < partcnt; part++) {
    pportcnt = portcnts[part];
    phopcnt = hopcnts[part];
    info(0,"part %u ports %u hops %u",part,pportcnt,phopcnt);
  }

  tportcnt = portcnts[tpart];

  gnet->partcnt = partcnt;
  gnet->portparts = gportparts;

  afree(portparts,"part portparts");
  afree(lportparts,"part portparts");

  // separate into partitions
  for (part = 0; part < partcnt; part++) {
    net = getnet(part);
    partp = parts + part;

    pportcnt = portcnts[part];
    phopcnt = hopcnts[part];
    pridcnt = ridcnts[part];

    if (part == tpart) {
      error_ne(part,partcnt-1);
      error_ne(tportcnt,pportcnt);
    }

    zpportcnt = 0;

    if (pportcnt == 0) {
      warning(0,"partition %u has no ports",part);
      continue;
    }
    if (phopcnt == 0) {
      warning(0,"partition %u has no hops",part);
      continue;
    }

    msgprefix(0,"s%u ",part);

    info(0,"partition %u: %u ports %u hops",part,pportcnt,phopcnt);

    pports = alloc(pportcnt,struct port,0,"ports",pportcnt);
    phops = alloc(phopcnt,struct hop,0,"phops",phopcnt);

    g2p = alloc(portcnt,ub4,0xff,"g2p-ports",portcnt);
    p2g = alloc(pportcnt,ub4,0xff,"p2g-ports",pportcnt);
    g2phop = alloc(hopcnt,ub4,0xff,"g2p-hops",hopcnt);

    p2zp = alloc(portcnt,ub4,0xff,"p2zp-ports",portcnt);
    zp2p = alloc(portcnt,ub4,0xff,"p2zp-ports",portcnt);

    portsbyhop = alloc(phopcnt * 2, ub4,0xff,"net portsbyhop",portcnt);

    // assign ports : members of this part
    pp = pports;
    pport = tcnt = 0;
    for (port = 0; port < portcnt; port++) {
      gp = ports + port;
      if (gp->partcnt == 0) { warninfo(gp->ndep || gp->narr,0,"port %u is not in any part %s",port,gp->name); continue; }

      if (gportparts[port * partcnt + part] == 0) {
        if (part == tpart) gp->tpart = 0;
        continue;
      }

      error_z(gp->partcnt,port);

      memcpy(pp,gp,sizeof(*pp));
      pp->id = pport;
      pp->gid = port;
      pp->ngdep = pp->ndep;
      pp->ngarr = pp->narr;
      pp->ndep = pp->narr = 0;
      if (part == tpart) gp->tpart = 1;
      if (gportparts[port * partcnt + tpart]) { // note: includes top
        pp->tpart = 1;
        tcnt = net->tportcnt;
        if (tcnt < Elemcnt(net->tports)) net->tports[tcnt++] = pport;
        net->tportcnt = tcnt;
      }

      g2p[port] = pport;
      p2g[pport] = port;

      zport = port2zport[port];
      error_ge(zport,portcnt);
      zpport = zp2p[zport];
      if (zpport == hi32) {
        zpport = zp2p[zport] = zpportcnt++;
      }
      p2zp[pport] = zpport;
      zp2p[zpport] = pport;
      pp++;
      pport++;
    }
    error_ne(pport,pportcnt);
    info(0,"part %u has %u top ports",part,tcnt);

    // separate and resequence hops
    hp = phops;
    phop = hpcnt2 = hxcnt2 = 0;
    hopdist = alloc(phopcnt,ub4,0,"net hopdist",phopcnt);
    hopdur = alloc(phopcnt,ub4,0,"net hopdur",phopcnt);

    for (hop = 0; hop < hopcnt; hop++) {
      ghp = hops + hop;
      dep = ghp->dep;
      arr = ghp->arr;
      if (dep == arr) {
        if (nilhopcnt == 0) error(Exit,"hop %u dep %u equals arr",hop,dep);
        continue;
      }

      error_ge(dep,portcnt);
      error_ge(arr,portcnt);

      if (gportparts[dep * partcnt + part] == 0) continue;
      if (gportparts[arr * partcnt + part] == 0) continue;

      pdep = ports + dep;
      parr = ports + arr;
      dname = pdep->name;
      aname = parr->name;

      dist = ghp->dist;

      if (phop >= phopcnt) warning(0,"hop %u phop %u phopcnt %u %u %u",hop,phop,phopcnt,hpcnt2,hxcnt2);
      error_ge(phop,phopcnt);
      g2phop[hop] = phop;
      memcpy(hp,ghp,sizeof(*hp));
      hp->gid = hop;
      t0 = hp->tp.t0;
      t1 = hp->tp.t1;
      midur = hp->tp.midur;
      hopdur[phop] = midur;

//      info(0,"hop %u tt range %u-%u",hop,t0,t1);
      depp = g2p[dep];
      error_ge(depp,pportcnt);
      arrp = g2p[arr];
      error_ge(arrp,pportcnt);
      error_eq_cc(depp,arrp,"hop %u dep %u arr %u",hop,dep,arr);

      hp->dep = depp;
      hp->arr = arrp;
      portsbyhop[phop * 2] = depp;
      portsbyhop[phop * 2 + 1] = arrp;
      hopdist[phop] = dist;
      hp->dist = dist;
      hp++;
      phop++;
    }
    warncc(phop < phopcnt,0,"%u out of %u hops",phop,phopcnt);
    phopcnt = phop;

    net->part = part;
    net->istpart = (part == tpart && partcnt > 1);

    net->zportcnt = zpportcnt;
    net->portcnt = pportcnt;
    net->hopcnt = phopcnt;
    net->ports = pports;
    net->hops = phops;
    net->g2pport = g2p;
    net->p2gport = p2g;
    net->g2phop = g2phop;

    net->port2zport = p2zp;
    net->zport2port = zp2p;

    net->portsbyhop = portsbyhop;
    net->hopdist = hopdist;
    net->hopdur = hopdur;

    net->routes = routes;  // not partitioned
    net->ridcnt = ridcnt;
    net->pridcnt = ridcnts[part];

    // global
    net->chains = chains;
    net->chaincnt = chaincnt;
    net->routechains = gnet->routechains;
    net->chainhops = bchainhops;
    net->chainmets = gnet->chainmets;
    net->chainhopcnt = chainhopcnt;

    net->eventmem = gnet->eventmem;
    net->evmapmem = gnet->evmapmem;
    net->events = gnet->events;
    net->evmaps = gnet->evmaps;

    net->maxvariants = variants;
    net->routevarmask = varmask;
    net->hichainlen = gnet->hichainlen;

    marklocal(net);

    info(0,"partition %u connectivity",part);
    showconn(pports,pportcnt,1);
  }
  msgprefix(0,NULL);

  return 0;
}
