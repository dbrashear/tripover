// net.c - main network setup

/*
   This file is part of Tripover, a broad-search journey planner.

   Copyright (C) 2014 Joris van der Geer.

   This work is licensed under the Creative Commons Attribution-NonCommercial-NoDerivatives 4.0 International License.
   To view a copy of this license, visit http://creativecommons.org/licenses/by-nc-nd/4.0/
 */

/* Initialize the network once at startup :

   create a pre-computed connectivity network used in search

   - Separate ports in full and minor.
     Full ports have enough connectivity to include in a (full x full) matrix.
     Minor ports connect only to one or two single transfer stops.
     Actual plannig is done on these transfer ports.

   - Build connectivity matrix between any 2 full ports
     base matrix for direct (non-stop) hops
     derived matrix for each of n intermediate hops

     each matrix contains a list of possible trips from port A to port B
     the list is trimmed on heuristics, such as distance, cost, timing

   - Prepare various metrics used for heuristics
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
#include "net.h"
#include "compound.h"

#undef hdrstop

static struct network gs_nets[Npart];

static struct gnetwork gs_gnet;

void ininet(void)
{
  msgfile = setmsgfile(__FILE__);
  iniassert();
}

struct network *getnet(ub4 part)
{
  error_ge(part,Npart);
  return gs_nets + part;
}

static struct network *cgetnet(ub4 callee,ub4 part)
{
  enter(callee);
  error_ge(part,Npart);
  error_ge(part,gs_gnet.partcnt);
  leave(callee);
  return gs_nets + part;
}

struct gnetwork *getgnet(void)
{
  return &gs_gnet;
}

#define Geohist (20+2)

#define Watches 2
static ub4 watches = 1;
static ub4 watch_dep[Watches] = { 20, 0 };
static ub4 watch_arr[Watches] = { 3, 0 };

static ub2 cnt0lim_xpart2 = 8;
static ub2 cnt0lim_xpart1 = 16;
static ub2 cnt0lim_part = 32;

// create 0-stop connectivity matrix and derived info.
// n-stop builds on this
static int mknet0(struct network *net)
{
  ub4 portcnt = net->portcnt;
  ub4 pportcnt = net->pportcnt;
  ub4 zportcnt = net->zportcnt;
  ub4 hopcnt = net->hopcnt;
  ub4 part = net->part;

  struct port *ports,*pdep,*parr;
  struct hop *hops,*hp;
  struct range distrange;

  char *dname,*aname;
  ub4 *p2z,*z2p;
  ub4 *portsbyhop;
  ub2 concnt,cntlim,gen,*con0cnt;
  ub4 ofs,*con0ofs;
  ub4 hop,*con0lst;
  ub4 dist,*dist0,*hopdist;
  ub4 geohist[Geohist];
  double fdist;
  ub4 dep,arr,zdep,zarr,port2,zport2,da,zda,depcnt,arrcnt;
  ub4 needconn,haveconn,watch;
  ub2 iv;
  struct eta eta;

  if (portcnt == 0) return error(0,"no ports for %u hops net",hopcnt);
  if (hopcnt == 0) return error(0,"no hops for %u port net",portcnt);
  error_z(zportcnt,portcnt);

  info(0,"init 0-stop connections for %u+%u port %u hop network in %u zports",pportcnt,portcnt - pportcnt,hopcnt,zportcnt);

  port2 = portcnt * portcnt;
  zport2 = zportcnt * zportcnt;

  ports = net->ports;
  hops = net->hops;

  p2z = net->port2zport;
  z2p = net->zport2port;

  portsbyhop = net->portsbyhop;

  con0cnt = alloc(port2, ub2,0,"net0 concnt",portcnt);
  con0ofs = alloc(port2, ub4,0xff,"net0 conofs",portcnt);

  con0lst = mkblock(net->conlst,hopcnt,ub4,Init1,"net0 0-stop conlst");

  // geographical direct-line distance
  dist0 = alloc(port2, ub4,0,"net0 geodist",portcnt);
  hopdist = alloc(hopcnt,ub4,0,"net0 hopdist",hopcnt);

  info(0,"calculating \ah%u distance pairs", port2);
  for (dep = 0; dep < portcnt; dep++) {
    pdep = ports + dep;
    error_zz(pdep->lat,pdep->lon);
  }
  for (dep = 0; dep < portcnt; dep++) {
    error_ge(dep,portcnt);
    pdep = ports + dep;
    dname = pdep->name;
    for (arr = 0; arr < portcnt; arr++) {
      error_ge(arr,portcnt);
      if (dep == arr) continue;
      parr = ports + arr;
      aname = parr->name;
      error_eq_cc(pdep->gid,parr->gid,"%s %s",dname,aname);
      da = dep * portcnt + arr;
      if (pdep->lat == parr->lat &&pdep->lon == parr->lon) {
        info(0,"ports %u-%u coloc %u,%u-%u,%u %s to %s",dep,arr,pdep->lat,pdep->lon,parr->lat,parr->lon,dname,aname);
        dist = 0;
      } else {
        fdist = geodist(pdep->rlat,pdep->rlon,parr->rlat,parr->rlon);
        if (fdist < 1e-10) warning(0,"port %u-%u distance ~0 for latlon %u,%u-%u,%u %s to %s",dep,arr,pdep->lat,pdep->lon,parr->lat,parr->lon,dname,parr->name);
        else if (fdist < 0.001) warning(0,"port %u-%u distance %e for latlon %u,%u-%u,%u %s to %s",dep,arr,fdist,pdep->lat,pdep->lon,parr->lat,parr->lon,dname,aname);
        else if (fdist > 1.0e+8) warning(0,"port %u-%u distance %e for latlon %u,%u-%u,%u %s to %s",dep,arr,fdist,pdep->lat,pdep->lon,parr->lat,parr->lon,dname,aname);
        dist = (ub4)fdist;
         error_ovf(dist,ub4);
      }
//      error_z(dist,arr);
      dist0[da] = dist;
    }
  }
  info(0,"done calculating \ah%u distance pairs", port2);
  mkhist(dist0,port2,&distrange,Geohist,geohist,"geodist",Vrb);

  // create 0-stop connectivity
  // support multiple hops per port pair
  ub4 ovfcnt = 0;
  for (hop = 0; hop < hopcnt; hop++) {
    hp = hops + hop;
    dep = hp->dep;
    arr = hp->arr;
    error_ge(dep,portcnt);
    error_ge(arr,portcnt);
    pdep = ports + dep;
    parr = ports + arr;
    dname = pdep->name;
    aname = parr->name;
    if (dep == arr) {
      if (dep < net->pportcnt) warning(0,"%chop %u %s dep == arr %u %s",hp->compound ? 'c' : ' ',hop,hp->name,dep,dname);
      continue;
    }
    for (watch = 0; watch < watches; watch++) {
      if (dep == watch_dep[watch]) {
        info(0,"hop %u %u-%u compound %u %s to %s",hop,dep,arr,hp->compound,dname,aname);
      }
    }

    da = dep * portcnt + arr;
    concnt = con0cnt[da];
    if (dep >= pportcnt && arr >= pportcnt) cntlim = cnt0lim_xpart2;
    else if (dep >= pportcnt || arr >= pportcnt) cntlim = cnt0lim_xpart1;
    else cntlim = cnt0lim_part;

    if (concnt >= cntlim) ovfcnt++;
    else if (concnt == cntlim-1) {
      info(0,"connect overflow hop %u port %u-%u %s to %s",hop,dep,arr,dname,aname);
      con0cnt[da] = (ub2)(concnt+1);
      ovfcnt++;
    } else con0cnt[da] = (ub2)(concnt+1);

    dist = dist0[da];
//    error_z(dist,hop);
    hp->dist = dist;
    hopdist[hop] = dist;
  }
  if (ovfcnt) warning(0,"limiting 0-stop net by \ah%u",ovfcnt);

  ofs = 0;
  needconn = haveconn = 0;
  for (dep = 0; dep < portcnt; dep++) {

    progress(&eta,"port %u of %u in pass 2 0-stop %u hop net",dep,portcnt,hopcnt);

    pdep = ports + dep;
//    if (dport->ndep == 0) continue;
    dname = pdep->name;

    for (arr = 0; arr < portcnt; arr++) {
      if (dep == arr) continue;
      parr = ports + arr;
//      if (aport->narr == 0) continue;

      aname = parr->name;

      needconn++;

      da = dep * portcnt + arr;
      concnt = con0cnt[da];
      if (concnt == 0) continue;

      haveconn++;

      con0ofs[da] = ofs;
      gen = 0;
      for (hop = 0; hop < hopcnt; hop++) {
        if (portsbyhop[2 * hop] != dep || portsbyhop[2 * hop + 1] != arr) continue;

        hp = hops + hop;
        if (hp->dep != dep || hp->arr != arr) {
          warning(0,"hop %u %s %u-%u not %u-%u",hop,hp->name,hp->dep,hp->arr,dep,arr);
          continue;
        }
        error_ge(ofs,hopcnt);
        con0lst[ofs+gen] = hop;
        for (watch = 0; watch < watches; watch++) {
          if (dep == watch_dep[watch] /* && arr == watch_arr[watch] */) {
            info(0,"dep %u arr %u hop %u %u of %u at %p %s to %s",dep,arr,hop,gen,concnt,con0lst + ofs + gen,dname,aname);
          }
        }
        gen++;
        if (gen >= concnt) break;
      }
      ofs += gen;
    }
  }
  net->lstlen[0] = ofs;
  info(0,"0-stop connectivity \ah%u of \ah%u  left \ah%u",haveconn,needconn,needconn - haveconn);

  // get connectivity stats
  ub4 hicnt = 0,hiport = 0;
  ub4 depstats[32];
  ub4 arrstats[32];
  ub4 depivs = Elemcnt(depstats) - 1;
  ub4 arrivs = Elemcnt(arrstats) - 1;

  aclear(arrstats);
  aclear(depstats);

  for (dep = 0; dep < portcnt; dep++) {
    depcnt = 0;
    for (arr = 0; arr < portcnt; arr++) {
      if (dep == arr) continue;
      depcnt += con0cnt[dep * portcnt + arr];
      if (depcnt > hicnt) { hicnt = depcnt; hiport = dep; }
    }
//    error_ne(depcnt,ports[dep].ndep);
    depstats[min(depivs,depcnt)]++;
  }
  for (iv = 0; iv <= depivs; iv++) {
    if (depstats[iv]) info(0,"%u port\as with %u departure\as", depstats[iv], iv);
  }
  pdep = ports + hiport;
  info(0,"port %u has %u deps %s",hiport,hicnt,pdep->name);

  hicnt = hiport = 0;
  for (arr = 0; arr < portcnt; arr++) {
    arrcnt = 0;
    for (dep = 0; dep < portcnt; dep++) {
      if (dep == arr) continue;
      arrcnt += con0cnt[dep * portcnt + arr];
      if (arrcnt > hicnt) { hicnt = arrcnt; hiport = arr; }
    }
//    error_ne(arrcnt,ports[arr].narr);
    arrstats[min(arrivs,arrcnt)]++;
  }
  for (iv = 0; iv <= arrivs; iv++) {
    if (arrstats[iv]) info(0,"%u ports with %u arrivals", arrstats[iv], iv);
  }
  parr = ports + hiport;
  info(0,"port %u has %u arrs %s",hiport,hicnt,parr->name);

  net->dist0 = dist0;
  net->hopdist = hopdist;

  net->con0cnt = con0cnt;
  net->con0ofs = con0ofs;

  net->concnt[0] = con0cnt;
  net->conofs[0] = con0ofs;

  net->lodist[0] = dist0;
  net->hidist[0] = dist0;

  net->needconn = needconn;

  return 0;
}

#define Distbins 256
static ub4 distbins[Distbins];

// create n-stop connectivity matrix and derived info
static int mknetn(struct network *net,ub4 nstop)
{
  ub4 pportcnt,portcnt = net->portcnt;
  ub4 hopcnt = net->hopcnt;

  struct port *ports,*pmid,*pdep,*parr;
  char *dname,*aname;
  block *lstblk,*lstblk1,*lstblk2;
  ub4 *portsbyhop;
  ub2 *concnt,*cnts1,*cnts2;
  ub4 *portdst;
  ub4 ofs,ofs1,ofs2,endofs,*conofs,*conofs1,*conofs2;
  ub4 *lst,*conlst1,*conlst2,*lst1,*lst11,*lst2,*lst22,*lstv1,*lstv2;
  ub4 *dist0,*hopdist,*distlims;
  ub4 dep,mid,arr,port2,depcnt,depmid,midarr,deparr,iport1,iport2;
  ub4 iv,watch;
  ub4 depstats[4];
  ub4 cnt,nstop1,n1,n2,n12,nleg1,nleg2,v1,v2,leg,leg1,leg2,nleg;
  size_t lstlen;
  ub4 midstop1,midstop2;
  ub4 lodist,lodist1,lodist2,lodist12,*lodists,*lodist1s,*lodist2s;
  ub4 hidist,hidist1,hidist2,hidist12,*hidists,*hidist1s,*hidist2s;
  ub4 dist1,dist2,dist12,distlim,distrange;
  ub4 cntlim,sumcnt,gen,outcnt;
  ub4 constats[16];

  ub4 varlimit = 128; // todo configurable and dependent on other aspects
  ub4 var12limit = 64;

  ub4 portlimit = 4000;

  ub4 dupcode,legport1,legport2;
  ub4 trip1ports[Nleg * 2];
  ub4 trip2ports[Nleg * 2];

  struct eta eta;

  error_z(nstop,0);
  error_zz(portcnt,hopcnt);

  pportcnt = net->pportcnt;

  info(0,"init %u-stop connections for %u port %u hop network",nstop,portcnt,hopcnt);

  port2 = portcnt * portcnt;

  ports = net->ports;

  portsbyhop = net->portsbyhop;

  concnt = alloc(port2, ub2,0,"net concnt",portcnt);
  lodists = alloc(port2, ub4,0xff,"net lodist",portcnt);
  hidists = alloc(port2, ub4,0,"net hidist",portcnt);

  distlims = alloc(port2, ub4,0,"net distlims",portcnt);

  portdst = alloc(portcnt, ub4,0,"net portdst",portcnt);

  dist0 = net->dist0;
  hopdist = net->hopdist;

  error_zp(dist0,0);
  error_zp(hopdist,0);

  nleg = nstop + 1;
  nstop1 = nstop - 1;

/* Essentially we do for each (departure,arrival) pair:
   Search for a 'via' port such that trip (departure,via) and (via,arrival) exist
   This is done for a given number of total stops.
   Hence, search for such via with various stops for X and Y in dep-X-via-Y-arr
   Trim list of alternatives based on e.g. distance
   Store result by value. This is memory-intensive but keeps code simple

   In short: foreach dep  foreach arr foreach stopline  foreach a with n1[dep,a] with n2 [a,arr] 
*/

/*
  pass 1 : foreach (dep,arr) pair at this #stops:
  bound overall best and worst
  currently, cost is distance only
  create histogram and derive threshold to use as filter in next pass
  estimate size of trip list matrix
  obtain basic stats
*/

  ub4 dupstats[8];
  ub4 cntstats[9];

  lstlen = 0;

  aclear(distbins);

  aclear(dupstats);
  aclear(cntstats);

  ub4 dmid,dmidcnt,*dmids = alloc(portcnt * nstop,ub4,0,"net vias",portcnt);
  ub4 dudep,duarr,mudep,muarr;
  ub4 *drdeps,*drarrs,*mrdeps,*mrarrs;
  char *mname;
  ub4 dmidcnts[Nstop];

  // for each departure port
  for (dep = 0; dep < pportcnt; dep++) {

    progress(&eta,"port %u of %u in pass 1 %u-stop net",dep,pportcnt,nstop);

    if (dep > portlimit) continue;

    pdep = ports + dep;
    if (pdep->ndep == 0) { cntstats[0]++; continue; }
    dname = pdep->name;
    dudep = pdep->nudep;
    duarr = pdep->nuarr;
    drdeps = pdep->drids;
    drarrs = pdep->arids;

    // prepare eligible via's

    for (midstop1 = 0; midstop1 < nstop; midstop1++) {
      cnts1 = net->concnt[midstop1];

      dmid = 0;
      for (mid = 0; mid < portcnt; mid++) {
        if (mid == dep) continue;
        pmid = ports + mid;
        if (pmid->ndep == 0) continue;
        depmid = dep * portcnt + mid;

        n1 = cnts1[depmid];
        if (n1 == 0) continue;

        // todo: skip vias only on same route
        mname = pmid->name;
        mudep = pmid->nudep;
        muarr = pmid->nuarr;
        mrdeps = pmid->drids;
        mrarrs = pmid->arids;

        if (dudep == 1 && duarr == 1 && mudep == 1 && muarr == 1 && drdeps[0] == mrarrs[0] && drdeps[0] != hi32) {
          vrb(0,"skip %u-%u-x on same oneway route %x %s to %s",dep,mid,drdeps[0],dname,mname);
          continue;
        }
        if (dudep == 2 && duarr == 2 && mudep == 2 && muarr == 2) {
          if(drdeps[0] == mrarrs[0] && drdeps[0] != hi32 && drarrs[1] == mrdeps[1] && mrdeps[1] != hi32) {
            vrb(0,"skip %u-%u-x on same twoway route %x.%x %s to %s",dep,mid,drdeps[0],mrdeps[1],dname,mname);
            continue;
          }
        }

        dmids[midstop1 * portcnt + dmid++] = mid;
      }
      dmidcnts[midstop1] = dmid;
    }

    outcnt = 0;

    // for each arrival port
    for (arr = 0; arr < portcnt; arr++) {
      if (arr == dep) continue;

      parr = ports + arr;
      if (parr->narr == 0) { cntstats[1]++; continue; }
      aname = parr->name;

      deparr = dep * portcnt + arr;

      lodist = hi32; hidist = 0;
      cnt = cntlim = 0;

      // for each #stops between dep-via-arr.
      // e.g. trip dep-a-b-via-c-arr has 2 stops before and 1 after via
      for (midstop1 = 0; midstop1 < nstop; midstop1++) {
        midstop2 = nstop1 - midstop1;

        cnts1 = net->concnt[midstop1];
        cnts2 = net->concnt[midstop2];

        lodist1s = net->lodist[midstop1];
        lodist2s = net->lodist[midstop2];
        hidist1s = net->hidist[midstop1];
        hidist2s = net->hidist[midstop2];

        // for each via
        // first obtain distance range
        dmidcnt = dmidcnts[midstop1];
        for (dmid = 0; dmid < dmidcnt; dmid++) {
          mid = dmids[midstop1 * portcnt + dmid];
          if (mid == arr) continue;
          if (arr < pportcnt && mid >= pportcnt) { cntstats[3]++; continue; }
          pmid = ports + mid;
          if (pmid->narr == 0) { cntstats[4]++; continue; }

          depmid = dep * portcnt + mid;

          n1 = cnts1[depmid];
          error_z(n1,mid);

          midarr = mid * portcnt + arr;
          n2 = cnts2[midarr];
          if (n2 == 0) { cntstats[6]++; continue; }

          error_ovf(n1,ub2);
          error_ovf(n2,ub2);

          n12 = n1 * n2;
          if (n12 > var12limit) { cntstats[7]++; n12 = var12limit; }
          cnt += n12;
          cntlim = min(cnt,varlimit);

          lodist1 = lodist1s[depmid];
          lodist2 = lodist2s[midarr];
          hidist1 = hidist1s[depmid];
          hidist2 = hidist2s[midarr];
          lodist12 = lodist1 + lodist2;
          hidist12 = hidist1 + hidist2;

          if (lodist12 < lodist) {
            lodist = lodist12;
          }
          if (hidist12 > hidist) {
            hidist = hidist12;
          }

        } // each mid stopover port
      } // each midpoint in stop list dep-a-b-arr

      if (cnt) {  // store range info
        lstlen += cntlim;
        concnt[deparr] = (ub2)cntlim;
        outcnt++;
        lodists[deparr] = lodist;
        hidists[deparr] = hidist;
//        error_zz(lodist,hidist);
        error_gt(lodist,hidist);
        distrange = max(hidist - lodist,1);
      } else {
        distrange = 1;
      }

      // if too many options, sort on distance.
      if (cnt > varlimit) {
        cntstats[8]++;

        // subpass 2: create distance histogram and derive threshold
        for (midstop1 = 0; midstop1 < nstop; midstop1++) {
          midstop2 = nstop1 - midstop1;
          nleg1 = midstop1 + 1;
          nleg2 = midstop2 + 1;

          cnts1 = net->concnt[midstop1];
          cnts2 = net->concnt[midstop2];

          lstblk1 = net->conlst + midstop1;
          lstblk2 = net->conlst + midstop2;

          conlst1 = blkdata(lstblk1,0,ub4);
          conlst2 = blkdata(lstblk2,0,ub4);

          conofs1 = net->conofs[midstop1];
          conofs2 = net->conofs[midstop2];

          for (mid = 0; mid < portcnt; mid++) {
            if (mid == dep || mid == arr) continue;
            if (arr < pportcnt && mid >= pportcnt) continue;
            depmid = dep * portcnt + mid;

            n1 = cnts1[depmid];
            if (n1 == 0) continue;

            midarr = mid * portcnt + arr;
            n2 = cnts2[midarr];
            if (n2 == 0) continue;

            for (watch = 0; watch < watches; watch++) {
              if (dep == watch_dep[watch]) vrb(0,"dep %u arr %u mid %u n1 %u n2 %u",dep,arr,mid,n1,n2);
            }

            ofs1 = conofs1[depmid];
            ofs2 = conofs2[midarr];
            error_eq(ofs1,hi32);
            error_eq(ofs2,hi32);

            lst1 = conlst1 + ofs1 * nleg1;
            lst2 = conlst2 + ofs2 * nleg2;

            bound(lstblk1,ofs1 * nleg1,ub4);
            bound(lstblk2,ofs2 * nleg2,ub4);

            // each dep-via alternative, except dep-*-arr-*-via
            for (v1 = 0; v1 < n1; v1++) {
              dist1 = 0;
              lst11 = lst1 + v1 * nleg1;

              for (leg1 = 0; leg1 < nleg1; leg1++) {
                leg = lst11[leg1];
                error_ge(leg,hopcnt);
                dist1 += hopdist[leg];
                trip1ports[leg1 * 2] = portsbyhop[leg * 2];
                trip1ports[leg1 * 2 + 1] = portsbyhop[leg * 2 + 1];
              }
              dupcode = 0;
              for (legport1 = 1; legport1 < nleg1 * 2 - 1; legport1++) {
                iport1 = trip1ports[legport1];
                if (iport1 == dep) { dupcode = 1; break; }
                else if (iport1 == arr) { dupcode = 2; break; }
                else if (iport1 == mid) { dupcode = 3; break; }
              }

              dupstats[min(dupcode,Elemcnt(dupstats))]++;
              if (dupcode) continue;

              for (watch = 0; watch < watches; watch++) {
                if (dep == watch_dep[watch]) vrb(0,"dep %u arr %u mid %u \av%u%p n1 %u n2 %u",dep,arr,mid,nleg1,lst11,n1,n2);
              }
              checktrip(net,lst11,nleg1,dep,mid,dist1);
              for (v2 = 0; v2 < n2; v2++) {
                dist2 = 0;
                lst22 = lst2 + v2 * nleg2;

                for (leg2 = 0; leg2 < nleg2; leg2++) {
                  leg = lst22[leg2];
                  error_ge(leg,hopcnt);
                  dist2 += hopdist[leg];
                  trip2ports[leg2 * 2] = portsbyhop[leg * 2];
                  trip2ports[leg2 * 2 + 1] = portsbyhop[leg * 2 + 1];
                }
                dupcode = 0;
                for (legport2 = 1; legport2 < nleg2 * 2 - 1; legport2++) {
                  iport2 = trip2ports[legport2];
                  if (iport2 == dep) { dupcode = 4; break; }
                  else if (iport2 == arr) { dupcode = 5; break; }
                  else if (iport2 == mid) { dupcode = 6; break; }
                }

                dupstats[min(dupcode,Elemcnt(dupstats))]++;
                if (dupcode) continue;

//                checktrip(lst22,nleg2,mid,arr,dist2);

                // filter out repeated 'B' visits in dep-*-B-*-via-*-B-*-arr
                for (legport1 = 0; legport1 < nleg1 * 2; legport1++) {
                  iport1 = trip1ports[legport1];
                  for (legport2 = 1; legport2 < nleg2 * 2; legport2++) {
                    iport2 = trip2ports[legport2];
                    if (iport1 == iport2) { dupcode = 7; break; }
                  }
                  if (dupcode) break;
                }
                if (dupcode) {
                  dupstats[min(dupcode,Elemcnt(dupstats))]++;
                  continue;
                }

                // mark in histogram, discard actual trip here
                dist12 = dist1 + dist2;
//                error_lt(dist12,lodist); // todo fails
//                error_gt(dist12,hidist);  // todo: fails
                iv = (dist12 - lodist) * Distbins / distrange;
                if (iv < Distbins) distbins[iv]++;
              } // each v2
            } // each v1

          } // each mid
        } // each midpoint

        // derive threshold to aim at desired number of alternatives
        sumcnt = 0; iv = 0;
        while (sumcnt < cntlim && iv < Distbins) {
          sumcnt += distbins[iv++];
        }
        if (iv == Distbins) distlim = hidist;
        else {
          iv--;
          distlim = lodist + iv * distrange / Distbins;
        }
        aclear(distbins);
      } else distlim = hidist;  // not cnt limited
      distlims[deparr] = distlim;

    } // each arrival port
    portdst[dep] = outcnt;

  } // each departure port

  for (iv = 0; iv < Elemcnt(dupstats); iv++) info(0,"dup %u: %u",iv,dupstats[iv]);
  for (iv = 0; iv < Elemcnt(cntstats); iv++) info(0,"cnt %u: %u",iv,cntstats[iv]);

  info(0,"%u-stop pass 1 done, tentative \ah%lu triplets",nstop,lstlen);

  if (lstlen == 0) {
    warning(0,"no connections at %u-stop",nstop);
    return 0;
  }

  ub4 cnt1,newcnt;
  struct range portdr;
  ub4 ivportdst[32];
  mkhist(portdst,portcnt,&portdr,Elemcnt(ivportdst),ivportdst,"outbounds by port",Vrb);

  // prepare list matrix and its offsets
  conofs = alloc(port2, ub4,0xff,"net conofs",portcnt);  // = org

  lstblk = net->conlst + nstop;

  lst = mkblock(lstblk,lstlen * nleg,ub4,Init1,"netv %u-stop conlst",nstop);
  net->lstlen[nstop] = lstlen;

  cnts1 = net->concnt[nstop1];
  ofs = newcnt = 0;
  for (deparr = 0; deparr < port2; deparr++) {
    cnt = concnt[deparr];
    cnt1 = cnts1[deparr];
    if (cnt) {
      conofs[deparr] = ofs;
      ofs += cnt;
      if (cnt1 == 0) newcnt++;
    }
  }
  error_ne(ofs,lstlen);
  info(0,"\ah%u new connections",newcnt);

  aclear(dupstats);

  // pass 2: fill based on range and thresholds determined above
  // most code comments from pass 1 apply
  for (dep = 0; dep < portcnt; dep++) {

    progress(&eta,"port %u of %u in pass 2 %u-stop net",dep,portcnt,nstop);

    if (dep > portlimit) continue;

    for (arr = 0; arr < portcnt; arr++) {
      if (arr == dep) continue;
      deparr = dep * portcnt + arr;

      cnt = concnt[deparr];
      if (cnt == 0) continue;
      gen = 0;

      distlim = distlims[deparr];

      ofs = conofs[deparr];
      lstv1 = lst + ofs * nleg;
      error_ge(ofs,lstlen);

      endofs = ofs + cnt - 1;

      bound(lstblk,endofs * nleg,ub4);

      midstop1 = 0;
      while (midstop1 < nstop && gen < cnt) {
        midstop2 = nstop1 - midstop1;
        nleg1 = midstop1 + 1;
        nleg2 = midstop2 + 1;

        cnts1 = net->concnt[midstop1];
        cnts2 = net->concnt[midstop2];
 
        lstblk1 = net->conlst + midstop1;
        lstblk2 = net->conlst + midstop2;

        conlst1 = blkdata(lstblk1,0,ub4);
        conlst2 = blkdata(lstblk2,0,ub4);

        mid = 0;
        while (mid < portcnt && gen < cnt) {
          if (mid == dep || mid == arr) { mid++; continue; }
          if (arr < pportcnt && mid >= pportcnt) { mid++; continue; }
          depmid = dep * portcnt + mid;
          n1 = cnts1[depmid];
          if (n1 == 0) { mid++; continue; }

          midarr = mid * portcnt + arr;
          n2 = cnts2[midarr];
          if (n2 == 0) { mid++; continue; }

          n12 = n1 * n2;

          conofs1 = net->conofs[midstop1];
          conofs2 = net->conofs[midstop2];

          ofs1 = conofs1[depmid];
          ofs2 = conofs2[midarr];
          lst1 = conlst1 + ofs1 * nleg1;
          lst2 = conlst2 + ofs2 * nleg2;

          bound(lstblk1,ofs1 * nleg1,ub4);
          bound(lstblk2,ofs2 * nleg2,ub4);

          v1 = 0;
          while (v1 < n1 && gen < cnt) {
            dist1 = 0;
            lst11 = lst1 + v1 * nleg1;

            for (leg1 = 0; leg1 < nleg1; leg1++) {
              leg = lst11[leg1];
              error_ge(leg,hopcnt);
              dist1 += hopdist[leg];
              trip1ports[leg1 * 2] = portsbyhop[leg * 2];
              trip1ports[leg1 * 2 + 1] = portsbyhop[leg * 2 + 1];
            }
            error_ne(trip1ports[0],dep);
            error_eq(trip1ports[0],mid);
            error_eq(trip1ports[0],arr);
            error_eq(trip1ports[nleg1 * 2 - 1],dep);
            error_ne(trip1ports[nleg1 * 2 - 1],mid);
            error_eq(trip1ports[nleg1 * 2 - 1],arr);
            dupcode = 0;
            for (legport1 = 1; legport1 < nleg1 * 2 - 1; legport1++) {
              iport1 = trip1ports[legport1];
              if (iport1 == dep) { dupcode = 1; break; }
              else if (iport1 == arr) { dupcode = 2; break; }
              else if (iport1 == mid) { dupcode = 3; break; }
            }

            dupstats[min(dupcode,Elemcnt(dupstats))]++;
            if (dupcode) { v1++; continue; }

            checktrip(net,lst11,nleg1,dep,mid,dist1);
            v2 = 0;
            while (v2 < n2 && gen < cnt) {
              dist2 = 0;
              lst22 = lst2 + v2 * nleg2;

              for (leg2 = 0; leg2 < nleg2; leg2++) {
                leg = lst22[leg2];
                error_ge(leg,hopcnt);
                dist2 += hopdist[leg];
                trip2ports[leg2 * 2] = portsbyhop[leg * 2];
                trip2ports[leg2 * 2 + 1] = portsbyhop[leg * 2 + 1];
              }
              error_ne(trip2ports[0],mid);
              error_eq(trip2ports[0],dep);
              error_eq(trip2ports[0],arr);
              error_ne(trip2ports[nleg2 * 2 - 1],arr);
              error_eq(trip2ports[nleg2 * 2 - 1],dep);
              error_eq(trip2ports[nleg2 * 2 - 1],mid);
              dupcode = 0;
              for (legport2 = 1; legport2 < nleg2 * 2 - 1; legport2++) {
                iport2 = trip2ports[legport2];
                if (iport2 == dep) { dupcode = 4; break; }
                else if (iport2 == arr) { dupcode = 5; break; }
                else if (iport2 == mid) { dupcode = 6; break; }
              }

              dupstats[min(dupcode,Elemcnt(dupstats))]++;
              if (dupcode) { v2++; continue; }

              // filter out repeated visits a-B-c-d-B-f
              for (legport1 = 0; legport1 < nleg1 * 2; legport1++) {
                iport1 = trip1ports[legport1];
                for (legport2 = 1; legport2 < nleg2 * 2; legport2++) {
                  iport2 = trip2ports[legport2];
                  if (iport1 == iport2) { dupcode = 7; break; }
                }
                if (dupcode) break;
              }
              if (dupcode) {
                dupstats[min(dupcode,Elemcnt(dupstats))]++;
                v2++;
                continue;
              }

//              checktrip(lst22,nleg2,mid,arr,dist2);
              dist12 = dist1 + dist2;
              if (dist12 <= distlim) {
                gen++;
                memcpy(lstv1,lst11,nleg1 * sizeof(ub4));
//                vrb(CC,"dep %u arr %u mid %u \av%u%p base %p n1 %u n2 %u",dep,arr,mid,nleg,lstv1,lst,n1,n2);
//                checktrip(lstv1,nleg1,dep,mid,dist1);
                lstv2 = lstv1 + nleg1;
                memcpy(lstv2,lst22,nleg2 * sizeof(ub4));
//                vrb(CC,"dep %u arr %u mid %u \av%u%p base %p n1 %u n2 %u",dep,arr,mid,nleg,lstv1,lst,n1,n2);
//                checktrip3(lstv1,nleg,dep,arr,mid,dist12);
                lstv1 = lstv2 + nleg2;
              }
              v2++;
            }
            v1++;
          }
          mid++;
        } // each mid
        midstop1++;
      } // each mid stopover port
      error_gt(gen,cnt);
      concnt[deparr] = (ub2)gen;
    } // each arrival port
  } // each departure port

  info(0,"pass 2 done, \ah%lu triplets", lstlen);

  for (iv = 0; iv < Elemcnt(dupstats); iv++) info(0,"dup %u: %u",iv,dupstats[iv]);

  // get connectivity stats
  aclear(depstats);
  ub4 depivs = Elemcnt(depstats) - 1;
  for (dep = 0; dep < portcnt; dep++) {
    depcnt = 0;
    for (arr = 0; arr < portcnt; arr++) {
      depcnt++;
    }
    ports[dep].depcnts[nstop] = (ub2)depcnt;
    depstats[min(depivs,depcnt)]++;
  }
  for (iv = 0; iv <= depivs; iv++) info(0,"%u ports with %u departures", depstats[iv], iv);

  struct range conrange;

  aclear(constats);
  mkhist2(concnt,port2,&conrange,Elemcnt(constats),constats,"connection",Info);

  net->concnt[nstop] = concnt;
  net->conofs[nstop] = conofs;

  net->lodist[nstop] = lodists;
  net->hidist[nstop] = hidists;

  net->lstlen[nstop] = lstlen;

  unsigned long doneconn,leftperc,leftcnt,needconn = net->needconn;
  ub4 n,da,nda = 0,port,hascon,hicon,arrcon,loarrcon,lodep = 0;
  ub4 tports[Nstop];
  ub4 gtports[Nstop];
  ub4 deparrs[16];
  ub4 nstops[16];
  ub4 lodeparrs[16];
  ub4 lonstops[16];
  ub4 ndacnt = Elemcnt(deparrs);
  struct port *pp;

  for (nda = 0; nda < ndacnt; nda++) {
    deparrs[nda] = lodeparrs[nda] = hi32;
    nstops[nda] = lonstops[nda] = 0;
  }

  doneconn = 0;
  loarrcon = hi32;

  for (dep = 0; dep < portcnt; dep++) {
    arrcon = 0;
    nda = 1;
    pdep = ports + dep;
    for (arr = 0; arr < portcnt; arr++) {
      if (dep == arr) continue;
      deparr = dep * portcnt + arr;
      hascon = 0;
      nstop1 = 0;
      while (nstop1 <= nstop) {
        concnt = net->concnt[nstop1];
        error_zp(concnt,nstop1);
        if (concnt[deparr]) { hascon = 1; break; }
        nstop1++;
      }
      if (hascon) {
        arrcon++;
        if (nstop1 >= nstops[0]) { nstops[0] = nstop1; deparrs[0] = deparr; }
      } else if (nda < ndacnt) deparrs[nda++] = deparr;
    }
    doneconn += arrcon;
    if (arrcon < loarrcon) {
      loarrcon = arrcon;
      lodep = dep;
      memcpy(lodeparrs,deparrs,ndacnt * sizeof(ub4));
      memcpy(lonstops,nstops,ndacnt * sizeof(ub4));
    }
    leftcnt = portcnt - arrcon - 1;
    if (leftcnt) {
      vrb(0,"port %u lacks %u connection\as %s",dep,(ub4)leftcnt,pdep->name);
      for (da = 1; da < nda; da++) {
        deparr = deparrs[da];
        if (deparr == hi32) break;
        arr = deparr % portcnt;
        parr = ports + arr;
        vrb(0,"port %u no connection to %u %s",dep,arr,parr->name);
      }
    }
  }
  error_gt(doneconn,needconn);
  leftcnt = needconn - doneconn;
  leftperc = leftcnt * 100 / needconn;
  if (leftperc > 1) info(0,"0-%u-stop connectivity \ah%lu of \ah%lu  left \ah%lu = %u%%", nstop,doneconn,needconn,leftcnt,(ub4)leftperc);
  else info(0,"0-%u-stop connectivity \ah%lu of \ah%lu  left \ah%lu", nstop,doneconn,needconn,leftcnt);

  pdep = ports + lodep;
  leftcnt = portcnt - loarrcon - 1;
  if (leftcnt) info(0,"port %u lacks %u connection\as %s",lodep,(ub4)leftcnt,pdep->name);

  for (nda = 0; nda < ndacnt; nda++) {
    deparr = lodeparrs[nda];
    if (deparr == hi32) break;
    hicon = lonstops[nda];
    dep = deparr / portcnt;
    arr = deparr % portcnt;
    pdep = ports + dep;
    parr = ports + arr;
    concnt = net->concnt[hicon];
    if (concnt == NULL) return error(0,"%u stops cnt nil",hicon);
    cnt = concnt[deparr];
    info(0,"%u-%u %u vars at %u stops %s to %s",dep,arr,cnt,hicon,pdep->name,parr->name);
    if (cnt == 0) continue;
    nleg = hicon + 1;
    conofs = net->conofs[hicon];
    ofs = conofs[deparr];
    lstblk = net->conlst + hicon;
    lst = blkdata(lstblk,ofs * nleg,ub4);
    if (triptoports(net,lst,nleg,tports,gtports)) break;
    for (n = 0; n <= nleg; n++) {
      port = tports[n];
      if (port >= portcnt) warning(0,"port #%u %x",n,port);
      else {
        pp = ports + port;
        info(0,"port #%u %u %s",n,port,pp->name);
      }
    }
  }
  return 0;
}

// count connections within part
static ub2 hasconn(ub4 callee,struct network *net,ub4 deparr)
{
  ub4 nstop = 0;
  ub2 **concnts = net->concnt,*cnts,cnt = 0;

  enter(callee);
  error_ge(deparr,net->portcnt * net->portcnt);
  while (nstop <= net->maxstop && cnt == 0) {
    cnts = concnts[nstop++];
    if (cnts) cnt = cnts[deparr];
  }
  leave(callee);
  return cnt;
}

// count connections between parts
// todo: work in progress
static ub2 hasxcon(ub4 callee,ub4 gdep,ub4 garr,ub4 dpart,ub4 apart)
{
  struct gnetwork *gnet = getgnet();
  struct network *dnet,*anet,*xnet,*xanet;
  struct port *gpp,*xpp;
  ub4 nstop,nleg,portno,tripno,xpi,api;
  ub4 xapart,xpart,partcnt;
  ub4 portcnt,pportcnt;
  ub4 dep,arr,xarr,xxarr,deparr,gxarr;
  ub2 **concnts,*cnts,dcnt = 0;
  ub4 **conofs,*dofss,dofs;
  ub4 *dlst,*trip,*trip0;
  block *conlst,*lstblk;
  ub4 ports[Nleg * 2];
  ub4 gports[Nleg * 2];
  int rv;
  ub2 *xmappos,*xmapbase,mask;
  block *xpartmap = &gnet->xpartmap;
  ub4 xmapofs;

  ub4 stats[6];
  ub4 iv;

  enter(callee);

  aclear(stats);

  error_eq(dpart,apart);

  dnet = cgetnet(caller,dpart);
  anet = cgetnet(caller,apart);

  partcnt = gnet->partcnt;

  pportcnt = dnet->pportcnt;
  portcnt = dnet->portcnt;

  dep = dnet->g2pport[gdep];
  arr = pportcnt + apart;
  if (apart > dpart) arr--;

  error_ge(dep,pportcnt);
  error_ge(arr,portcnt);
  deparr = dep * portcnt + arr;

  gpp = gnet->ports + garr;

  xmapbase = blkdata(xpartmap,0,ub2);

  nstop = 1;
  concnts = dnet->concnt;
  conofs = dnet->conofs;
  conlst = dnet->conlst;
  while (nstop <= dnet->maxstop) {
    nleg = nstop + 1;
    cnts = concnts[nstop];
    if (!cnts) { nstop++; stats[0]++; continue; }
    dcnt = cnts[deparr];
    if (dcnt == 0) { nstop++; stats[1]++; continue; }
    dofss = conofs[nstop];
    lstblk = conlst + nstop;
    dlst = blkdata(lstblk,0,ub4);
    dofs = dofss[deparr];
    bound(lstblk,dofs * nleg,ub4);
    bound(lstblk,dofs * nleg + nstop,ub4);

    trip0 = dlst + dofs * nleg;
    tripno = 0;
    while (tripno < dcnt) {
      trip = trip0 + tripno * nleg;
      rv = triptoports(dnet,trip,nstop,ports,gports);
      if (rv) return 0;
      gxarr = gports[nstop];
      if (gxarr == garr) return 1;
      tripno++;
    }
    nstop++;
  }

#if 0
    portno = nstop;
    while (portno) {
      xarr = ports[portno];
      gxarr = gports[portno];
      portno--;
      if (xarr == hi32 || gxarr == hi32) { stats[2]++; continue; }
      if (xarr >= pportcnt) xpart = xarr - pportcnt;
      else { stats[3]++; xpart = dpart; } // stat
      if (xpart != apart) { // use xmap to check reach

        if (xpart >= partcnt) {
          info(0,"xarr %u pport %u",xarr,pportcnt); // todo
          return 0;
        }
        xnet = cgetnet(caller,xpart);
        xpp = xnet->ports + xarr;
        xpi = 0;
        while (xpi < xpp->partcnt && xpi < Nxpart && xpp->partnos[xpi] != apart) xpi++;
        if (xpi == Nxpart) break; // todo
        else if (xpp->partnos[xpi] != apart) {
          xapart = hi32;
          stats[4]++;  // stat
          for (xpi = 0; xpi < min(xpp->partcnt,Nxpart); xpi++) {
            for (api = 0; api < min(gpp->partcnt,Nxpart); api++) {
              if (xpp->partnos[xpi] == gpp->partnos[api]) { xapart = xpp->partnos[xpi]; break; }
            }
            if (xapart != hi32) break;
          }
          if (xapart == hi32) { stats[5]++; break; }
          xanet = cgetnet(caller,xapart);
          xmapofs = xpp->pmapofs[xpi];
          xxarr = xanet->g2pport[gxarr];
          pportcnt = xanet->pportcnt;
        } else {
          xmapofs = xpp->pmapofs[xpi];
          xxarr = xnet->g2pport[gxarr];
          pportcnt = xnet->pportcnt;
        }
        bound(xpartmap,xmapofs,ub2);
        bound(xpartmap,xmapofs + pportcnt,ub2);
        xmappos = xmapbase + xmapofs;

        mask = xmappos[xxarr];
        if (mask) dcnt++;
      }
    }
    nstop++;
  }
#endif

  for (iv = 0; iv < Elemcnt(stats); iv++) vrb(0,"%u:%u", iv,stats[iv]);

  if (dcnt) vrb(0,"hasxcon %u.%u-%u.%u %u",dpart,gdep,apart,garr,dcnt);
  leave(callee);
  return dcnt;
}

static void chkgconn(struct gnetwork *gnet)
{
  ub4 gdep,dpart;
  ub4 partcnt = gnet->partcnt;
  ub4 gportcnt = gnet->portcnt;
  struct port *gpdep,*gports = gnet->ports;

  for (gdep = 0; gdep < gportcnt; gdep++) {
    gpdep = gports + gdep;
    dpart = gpdep->part;
    error_ge_cc(dpart,partcnt,"port %u %s",gdep,gpdep->name);
  }
}

static int dogconn(ub4 callee,struct gnetwork *gnet)
{
  // fill xmap and show cumulative connectivity
  struct network *dnet,*anet,*danet,*xnet;
  ub4 partcnt = gnet->partcnt;
  ub4 *portcnts = gnet->portcnts;
  ub4 xportcnt,gportcnt = gnet->portcnt;
  ub4 dpart,apart,xpart,xpartcnt,xpi;
  ub4 gdep,garr,dep,arr,xdep,xarr,adep,da,dapart;
  struct port *gpdep,*gparr,*gports = gnet->ports;
  ub1 *portparts = gnet->portparts;
  ub2 **concnts,*cnts,cnt;
  ub4 sample,gconn = 0, noxcon = 0;
  ub2 *xmappos,*xmapbase,mask,stopmask;
  block *xpartmap = &gnet->xpartmap;
  ub4 xmapofs,xmapcnt = 0;
  ub4 nstop,fillcnt = 0;
  struct eta eta;

  if (partcnt < 2) return info(0,"no inter-partition maps for %u partition\as",partcnt);

  enter(callee);

  xmappos = xmapbase = blkdata(xpartmap,0,ub2);

  info0(0,"fill inter-partition reach maps");
  for (gdep = 0; gdep < gportcnt; gdep++) {

    progress(&eta,"port %u of %u in %u partition\as",gdep,gportcnt,partcnt);

    gpdep = gports + gdep;
    dpart = gpdep->part;
    error_ge_cc(dpart,partcnt,"port %u %s",gdep,gpdep->name);
    xpartcnt = gpdep->partcnt;
    if (xpartcnt < 2) continue;
    for (xpi = 0; xpi < min(xpartcnt,Nxpart); xpi++) {
      xpart = gpdep->partnos[xpi];
      xmapofs = gpdep->pmapofs[xpi];
      bound(xpartmap,xmapofs,ub2);
      xmappos = xmapbase + xmapofs;
      xportcnt = portcnts[xpart];
      if (xportcnt == 0) continue;
      bound(xpartmap,xmapofs + xportcnt - 1,ub2);
      xnet = getnet(xpart);
      concnts = xnet->concnt;
      if (!concnts[0]) continue;
      fillcnt++;
      xdep = xnet->g2pport[gdep];
      for (arr = 0; arr < xportcnt; arr++) {
        da = xdep * xnet->portcnt + arr;
        cnt = 0;
        mask = 0; stopmask = 1;
        for (nstop = 0; nstop <= xnet->maxstop; nstop++) {
          cnts = concnts[nstop];
          cnt = cnts[da];
          if (cnt) mask |= stopmask;
          stopmask <<= 1;
        }
        xmappos[arr] = stopmask;
      }
    }
    dpart = gpdep->part;
    error_ge_cc(dpart,partcnt,"port %u %s",gdep,gpdep->name);
  }
  info(0,"done filling %u inter-partition reach maps",fillcnt);

  if (gportcnt > 20000) {
    sample = gportcnt / 10000;
    info(0,"sampling connectivity in %u steps",sample);
  } else sample = 0;

  info0(0,"global connectivity");

  for (gdep = 0; gdep < gportcnt; gdep += rnd(sample) ) {

    progress(&eta,"port %u of %u in global connect, sample %u",gdep,gportcnt,sample);

    gpdep = gports + gdep;
    dpart = gpdep->part;
    dnet = cgetnet(caller,dpart);
    error_ne(dnet->part,dpart);
    dep = dnet->g2pport[gdep];
    error_ge(dep,dnet->pportcnt);
    error_ge(dep,dnet->portcnt);
    concnts = dnet->concnt;

    error_ne(dnet->part,dpart);

    for (garr = 0; garr < gportcnt; garr += rnd(sample) ) {
      error_ne(dnet->part,dpart);
      if (garr == gdep) continue;
      gparr = gports + garr;
      apart = gparr->part;
      if (apart >= partcnt) warning(0,"port %u part %u %s",garr,apart,gparr->name);
      anet = cgetnet(caller,apart);

      if (portparts[garr * partcnt + dpart]) {  // arr is member of dep.part
        arr = dnet->g2pport[garr];
        error_ge(arr,dnet->pportcnt);
        da = dep * dnet->portcnt + arr;
        cnt = hasconn(caller,dnet,da);
        if (cnt) gconn++;

      } else { // dep and arr not in same part: search for part with both as member
        dapart = 0;
        while (dapart < partcnt && (portparts[gdep * partcnt + dapart] & portparts[garr * partcnt + dapart]) == 0) dapart++;
        if (dapart < partcnt) {
          danet = cgetnet(caller,dapart);
          adep = danet->g2pport[gdep];
          arr = danet->g2pport[garr];
          error_ge(adep,danet->pportcnt);
          error_ge(arr,danet->pportcnt);
          da = adep * danet->portcnt + arr;
          cnt = hasconn(caller,danet,da);
          if (cnt) gconn++;

        } else {  // no shared part for dep and arr
          xarr = dnet->pportcnt + apart;  // arr placeholder in dep.part
          if (apart > dpart) xarr--;
          error_ge(xarr,dnet->portcnt);
          error_ge(dep,dnet->portcnt);
          da = dep * dnet->portcnt + xarr;
          cnt = hasconn(caller,dnet,da);
          if (cnt == 0) { noxcon++; continue; }
          // follow
          xdep = anet->pportcnt + dpart;
          if (dpart > apart) xdep--;
          error_ge(xdep,anet->portcnt);
          arr = anet->g2pport[garr];
          error_ge(arr,anet->pportcnt);
          da = xdep * anet->portcnt + arr;
          cnt = hasconn(caller,anet,da);
          if (cnt == 0) { noxcon++; continue; }

          cnt = hasxcon(caller,gdep,garr,dpart,apart);
          if (cnt == 0) { noxcon++; continue; }

          xmapcnt++;
        }
      }
    }
  }
  info(0,"global connectivity \ah%u of \ah%u, \ah%u interpart, \ah%u missing interpart",gconn,gportcnt * (gportcnt - 1),xmapcnt,noxcon);

  leave(callee);

  return 0;
}

// initialize basic network, and connectivity for each number of stops
// the number of stops need to be determined such that all port pairs are reachable
int mknet(ub4 maxstop)
{
  ub4 portcnt,hopcnt;
  ub4 nstop,part,partcnt;
  int rv;
  struct gnetwork *gnet = getgnet();
  struct network *net;

  if (dorun(FLN,Runmknet) == 0) return 0;

  partcnt = gnet->partcnt;
  if (partcnt == 0) return info0(0,"skip init zero-partition net");

  for (part = 0; part < partcnt; part++) {

    info(0,"mknet partition %u of %u",part,partcnt);
    net = getnet(part);

    portcnt = net->portcnt;
    hopcnt = net->hopcnt;

    if (portcnt == 0) { info0(0,"skip mknet on 0 ports"); continue; }
    if (hopcnt == 0) { info0(0,"skip mknet on 0 hops"); continue; }

    msgprefix(0,"s%u ",part);

    rv = compound(net);
    if (rv) return msgprefix(1,NULL);

    if (dorun(FLN,Runnet0)) {
      if (mknet0(net)) return msgprefix(1,NULL);
    } else continue;

    limit_gt(maxstop,Nstop,0);

    if (dorun(FLN,Runnetn)) {
      for (nstop = 1; nstop <= maxstop; nstop++) {
        if (mknetn(net,nstop)) return msgprefix(1,NULL);
        net->maxstop = nstop;
        chkgconn(gnet);
        if (net->lstlen[nstop] == 0) {
          break;
        }
      }
      info(0,"partition %u static network init done",part);
    } else info(0,"partition %u skipped static network init",part);

  } // each part

  msgprefix(0,NULL);

  dogconn(caller,gnet);

  return 0;
}

int showconn(struct port *ports,ub4 portcnt,int local)
{
  ub4 nodep,noarr,nodeparr,oneroute,oneroutes;
  ub4 port,ndep,narr,ngdep,ngarr,n;
  ub4 *drids,*arids;
  struct port *pp;

  ub4 constats[256];
  ub4 depstats[256];
  ub4 arrstats[256];
  ub4 statmax = Elemcnt(depstats) - 1;

  aclear(constats);
  aclear(depstats);
  aclear(arrstats);
  nodep = noarr = nodeparr = oneroutes = 0;

  for (port = 0; port < portcnt; port++) {
    pp = ports + port;
    ndep = pp->ndep; narr = pp->narr;
    ngdep = pp->ngdep; ngarr = pp->ngarr;
    drids = pp->drids; arids = pp->arids;
    oneroute = 1;
    if (ndep == 0 && narr == 0) {
      if (local && (ngdep | ngarr)) warning(0,"port %u is unconnected, global has %u dep %u arr %s",port,ngdep,ngarr,pp->name);
      else if (!local) info(0,"port %u is unconnected %s",port,pp->name);
      nodeparr++;
      oneroute = 0;
    } else if (ndep == 0) {
      if (local && ngdep) warning(0,"port %u has 0 deps %u arr\as, global has %u %s",port,narr,ngdep,pp->name);
      else if (!local) info(0,"port %u has no deps - %s",port,pp->name);
      nodep++;
      if (narr > 1 && arids[0] != arids[1]) oneroute = 0;
    } else if (narr == 0) {
      if (local && ngarr) warning(0,"port %u has %u dep\as 0 arrs, global has %u %s",port,ndep,ngarr,pp->name);
      else if (!local) info(0,"port %u has no arrs - %s",port,pp->name);
      noarr++;
      if (ndep > 1 && drids[0] != drids[1]) oneroute = 0;
    } else {
      if (ndep > 1 && drids[0] != drids[1]) oneroute = 0;
      if (narr > 1 && arids[0] != arids[1]) oneroute = 0;
    }
    if (ndep < 16 && narr < 16) constats[(ndep << 4) | narr]++;
    if (narr) depstats[min(ndep,statmax)]++;
    if (ndep) arrstats[min(narr,statmax)]++;
    oneroutes += oneroute;
  }
  if (nodeparr) genmsg(local ? Info : Warn,0,"%u of %u ports without connection",nodeparr,portcnt);
  if (nodep) info(0,"%u of %u ports without departures",nodep,portcnt);
  if (noarr) info(0,"%u of %u ports without arrivals",noarr,portcnt);
  for (ndep = 0; ndep < (local ? 3 : 4); ndep++) {
    for (narr = 0; narr < (local ? 3 : 4); narr++) {
      n = constats[(ndep << 4) | narr];
      if (n) info(0,"%u port\as with %u dep\as and %u arr\as", n,ndep,narr);
    }
  }
  info0(0,"");
  for (ndep = 0; ndep < (local ? 16 : 32); ndep++) {
    n = depstats[ndep];
    if (n) info(0,"%u port\as with %u dep\as and 1+ arrs", n,ndep);
  }
  n = depstats[statmax];
  if (n) info(0,"%u port\as with %u+ deps and 1+ arrs", n,statmax);
  for (narr = 0; narr < (local ? 16 : 32); narr++) {
    n = arrstats[narr];
    if (n) info(0,"%u ports with %u arr\as and 1+ deps", n,narr);
  }
  n = arrstats[statmax];
  if (n) info(0,"%u port\as with %u+ arrs and 1+ deps", n,statmax);
  info(0,"%u of %u ports on a single route",oneroutes,portcnt);
  return 0;
}

// check whether a triplet passes thru the given ports
void checktrip_fln(struct network *net,ub4 *legs, ub4 nleg,ub4 dep,ub4 arr,ub4 dist,ub4 fln)
{
  ub4 legno,legno2,leg,leg2,arr0,cdist,hopcnt = net->hopcnt;
  struct hop *hp,*hp2,*hops = net->hops;

  error_eq_fln(arr,dep,"arr","dep",fln);

  for (legno = 0; legno < nleg; legno++) {
    leg = legs[legno];
    error_ge_fln(leg,hopcnt,"hop","hopcnt",fln);
  }
  if (nleg > 2) {
    for (legno = 0; legno < nleg; legno++) {
      leg = legs[legno];
      hp = hops + leg;
      for (legno2 = legno+1; legno2 < nleg; legno2++) {
        leg2 = legs[legno2];
        hp2 = hops + leg2;
        error_eq_fln(leg,leg2,"leg1","leg2",fln);
        error_eq_fln(hp->dep,hp2->dep,"dep1","dep2",fln);
        error_eq_fln(hp->dep,hp2->arr,"dep1","arr2",fln);
        if (legno2 != legno + 1) error_eq_fln(hp->arr,hp2->dep,"arr1","dep2",fln);
        error_eq_fln(hp->arr,hp2->arr,"arr1","arr2",fln);
      }
    }
  }

  leg = legs[0];
  hp = hops + leg;
  error_ne_fln(hp->dep,dep,"hop.dep","dep",fln);
  arr0 = hp->arr;
  cdist = hp->dist;

  leg = legs[nleg-1];
  hp = hops + leg;
  error_ne_fln(hp->arr,arr,"hop.arr","arr",fln);

  for (legno = 1; legno < nleg; legno++) {
    leg = legs[legno];
    hp = hops + leg;
    error_ne_fln(arr0,hp->dep,"prv.arr","dep",fln);
    arr0 = hp->arr;
    cdist += hp->dist;
  }
  error_ne_fln(dist,cdist,"dist","cdist",fln);
}

// check whether a triplet passes thru the given ports
void checktrip3_fln(struct network *net,ub4 *legs, ub4 nleg,ub4 dep,ub4 arr,ub4 via,ub4 dist,ub4 fln)
{
  ub4 legno,leg;
  struct hop *hp,*hops = net->hops;
  int hasvia = 0;

  error_lt_fln(nleg,2,"nleg","2",fln);
  checktrip_fln(net,legs,nleg,dep,arr,dist,fln);
  error_eq_fln(via,dep,"via","dep",fln);
  error_eq_fln(via,arr,"via","arr",fln);

  for (legno = 1; legno < nleg; legno++) {
    leg = legs[legno];
    hp = hops + leg;
    if (hp->dep == via) hasvia = 1;
  }
  error_z_fln(hasvia,0,"via","0",fln);
}

int triptoports(struct network *net,ub4 *trip,ub4 triplen,ub4 *ports,ub4 *gports)
{
  ub4 leg,l,dep,arr = hi32;
  ub4 hopcnt = net->hopcnt;
  ub4 pportcnt = net->pportcnt;
  ub4 partcnt = net->partcnt;

  error_ge(triplen,Nleg);

  for (leg = 0; leg < triplen; leg++) {
    l = trip[leg];
    if (l >= hopcnt) return error(0,"leg %u hop %x",leg,l);
    dep = net->portsbyhop[2 * l];
    if (leg) {
      if (dep != arr) return error(0,"leg %u hop %u dep %u not connects to preceding arr %u",leg,l,dep,arr);
    }
    error_ge(dep,net->portcnt);
    arr = net->portsbyhop[2 * l + 1];
    error_ge(arr,net->portcnt);
    if (dep > pportcnt && dep - pportcnt >= partcnt) return error(0,"dep %u > %u+%u",dep,pportcnt,partcnt);
    ports[leg] = dep;
    gports[leg] = net->p2gport[dep];
  }
  if (arr > pportcnt && arr - pportcnt >= partcnt) return error(0,"arr %u > %u+%u",arr,pportcnt,partcnt);
  ports[triplen] = arr;
  gports[triplen] = net->p2gport[arr];
  if (triplen < Nstop) ports[triplen+1] = gports[triplen+1] = hi32;
  return 0;
}
