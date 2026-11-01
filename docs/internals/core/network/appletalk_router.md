# The AppleTalk router

The router in the built-in AppleTalk stack: `src/core/network/appletalk_router.c`,
`appletalk_router.h` and the link seam `appletalk_link.h`, wired into the stack
in `src/core/network/appletalk.c` (DDP dispatch and delivery, NBP BrRq and
FwdReq, the connection lifecycle). The protocols are specified
in *Inside AppleTalk*, 2nd ed. (Apple, 1990), ch. 5 (RTMP) and ch. 8 (ZIP and
NBP routing); page numbers below are from that book.

## 1. Responsibilities & design

Every emulator instance keeps **its own LocalTalk network**. With AppleTalk on,
node 33 — the built-in stack's fixed node, which already serves the AFP file
server, the LaserWriter, the ImageWriter card and the program-linking peer — is
also that network's **seed router**:

- it owns the network number (THIS-NET, default 1) and the zone name (default
  "Granny Smith");
- it broadcasts RTMP Data every 10 s and answers RTMP Requests and Route Data
  Requests, so the guest learns THIS-NET and A-ROUTER at boot (5-17);
- it answers ZIP on the Zone Information Socket (6): GetMyZone, GetZoneList and
  GetLocalZones over ATP, and ZIP Query by DDP (8-7, 8-11);
- it turns the guest's NBP BrRq into a LkUp broadcast on the local network
  (8-18), answering for the host's own names as before.

Toward other instances it is an ordinary AppleTalk router whose ports, beside
the local network, are **links**: point-to-point half-router links (5-4) with
no network number of their own. Over every link it exchanges RTMP (routing
table, split horizon, aging, notify neighbor) and ZIP (Query/Reply for zones it
does not know), forwards datagrams in both directions and in transit, and turns
NBP BrRqs for remote zones into FwdReqs. Until a link exists, the routing table
holds only the local network: a gateway that leads nowhere.

`appletalk.router.seed = false` turns the router off and gives back the
routerless cable the stack always had: RTMP Requests and ZIP requests go
unanswered, THIS-NET is 0, and BrRqs are answered as local lookups.

## 2. Key types & files

| Item | Where | What |
|---|---|---|
| `router_t` | `appletalk_router.c` | the router's state, one per process (like the network): seed, net, zone (UTF-8 and MacRoman), the send-RTMP timer, counters |
| `atalk_this_net()` | `appletalk_router.c` | THIS-NET for the stack: the router's net, 0 with the router off |
| `atalk_link_ready()` | `appletalk.c` | whether a datagram would reach the guest now (plugged, attached, SDLC up); the periodic RTMP Data is skipped otherwise, so it does not count as a dropped frame |
| `nbp_route_brrq()` | `appletalk.c` | BrRq → local LkUp |
| `ddp_extended_in()`, `ddp_header_to()` | `appletalk.c` | long-header DDP in and out; a guest that knows a router sends long headers |

The LaserWriter's PAP listener moved from socket 6 to 16 to free the ZIS
(`ATALK_HOST_PAP_SOCKET`).

## 3. Behaviour

### 3.0 Links

A transport (`atalk_link_transport_t`: `send(record, reliable)`, `close`)
carries records; the router frames them (`appletalk_link.h`):

| Record | Content |
|---|---|
| `HELLO` | version 1, the sender's network number, flags (friend link), display name |
| `DDP` | one datagram: extended header + data, as on the wire |
| `PING` / `PONG` | a guest-time stamp, echoed: round trip in `links[i].rtt_ms` |
| `BYE` | the reason |

A link opens when the other end's HELLO arrives; the router answers it with
its own (the first may have gone before the other end listened) and sends its
table at once. While a link waits, its HELLO goes out again every second. A HELLO, or a distance-0 RTMP tuple, naming our own network
number closes the link ("both sides use network N"): two networks with one
number cannot be routed. `atalk_link_lost` (the transport lost the other end)
and BYE close it too; its routes go bad at once.

Datagrams addressed to network 0 on a link are for the router at that end:
its RTMP Data (sender network 0, as for any port that is not an AppleTalk
network, 5-15) and ZIP Queries. Everything else is routed.

### 3.1 Routing table and forwarding

- Entries: network, distance, port (local or a link), state good / suspect /
  bad0 / bad1, and the zone (the zone information table, 8-4).
- RTMP Data in through a link: Update-the-Entry / Create-New-Entry exactly as
  5-22; out every 10 s on every port and 100 ms after any change (links only),
  with split horizon; bad entries go out as distance 31.
- The validity timer (20 s) ages entries good → suspect → bad0 → bad1 → gone;
  an entry is usable while good or suspect.
- A new network, or one coming back from bad, has an unknown zone: a ZIP Query
  goes out on its link at once and every 10 s until the Reply.
- Forwarding: from the local network (the guest's long-header datagrams, and
  the host stack's own datagrams for another network, which `ddp_send` hands to
  the router), from a link to this network (to the host on node 33 or 0, or onto
  LocalTalk from node 33 with the DDP node fields kept), and from link to link
  in transit. Each hop increments the hop count; one arriving with 15 is
  dropped (4-13). The checksum does not cover the hop count, so it stays.
- **Host services filter**: from a link, the host (node 33) is reachable only on
  sockets 1, 2 and 6, and a FwdReq's LkUp is not answered for the host's names,
  unless `share_host_services` is on and the link is a friend link. Tabs of one
  browser serve the same OPFS shared folder, so it is never offered between them.

### 3.2 NBP routing (8-18 to 8-20)

- BrRq from the guest: for every network of the zone (`*` = the local zone):
  the local one gets a LkUp broadcast (and the host answers for its names);
  every other gets a FwdReq to node 0 ("any router") on that network, routed.
- FwdReq for this network (from a link): a LkUp broadcast here; the guest's
  replies go to the requester's address in the tuple, through the router.

- **RTMP Data** (5-13): sender's net, node ID length 8, node 33, version
  indicator `$000082`, then the tuples (the local net at distance 0). The RTMP
  Response to a Request is the same packet without tuples, directed to the
  requesting socket.
- **The send-RTMP timer** is a guest-time scheduler event (`atalk_router.rtmp_send`),
  armed when a connection takes the cable and dropped when it leaves.
- **ZIP ATP requests**: user byte 0 is the function (7, 8, 9), bytes 2–3 the
  1-based start index. The response's user byte 0 is LastFlag (1 for the list
  requests; GetMyZone has none) and bytes 2–3 the count of names in the data,
  which is a list of Pascal strings. An index past the one zone gets an empty
  last answer.
- **ZIP Query**: answered only for networks the router knows (now: its own);
  silence otherwise (8-4). GetNetInfo is for extended networks and is ignored.
- **BrRq**: the zone `*` is the local zone (8-19); a BrRq for any other zone
  finds no network and is dropped. The LkUp carries the zone's name and fills a
  requester net of 0 with THIS-NET.
- **NBP replies** carry THIS-NET in the tuple of a name registered without a
  network.

Observed with real guests (2026-11-01): System 6.0.8 on a Plus sends an RTMP
Request when AppleTalk comes up and its lookups as BrRqs; System 7.5's Chooser
sends GetMyZone and GetZoneList; with a single zone neither Chooser shows a
zone list (that is the Chooser's own rule), but the Finder's Get Info names the
server's zone ("Shared Folders in Granny Smith").

## 4. Object-model / shell surface

```
appletalk.router
  .seed                 r/w  run as router (default true)
  .net                  r/w  this network's number, 1..65279 (default 1); not while linked
  .zone                 r/w  zone name (default "Granny Smith")
  .name                 r/w  display name sent in HELLO
  .link_udp(local, remote, [friend])   headless: link to a router over UDP
  .share_host_services  r/w  friends may use the host's services (default false)
  .routes[i]                 net, distance, state, via, zone
  .links[i]                  state, transport, friend, peer_net, peer_zone, peer_name,
                             rtt_ms, reason, close()
  .stats                     RTMP, ZIP, NBP routing, forwarding and drop counters
```

A running guest picks up a changed `net` when it restarts AppleTalk; `seed`
off takes effect for new requests at once, and the guest ages A-ROUTER after
about 50 s (5-18).

## 5. Checkpointing

The router's configuration is host state, like the rest of the network: not in
a checkpoint. The send-RTMP timer is a scheduler event, registered with every
connection as it is built, so a checkpoint taken with it pending restores; it is
re-armed when the restored machine takes the cable.

## 6. Testing

- `tests/unit/suites/atalk_link`: RTMP Request/Response and Route Data
  Request, the 10 s broadcast, the three ZIP ATP requests and ZIP Query, BrRq
  → LkUp, and long-header DDP in and out.
- `atalk_link` also plays a scripted peer router on a loopback transport:
  learning networks and zones, forwarding both ways, the host-services filter,
  BrRq → FwdReq, transit with split horizon and the hop limit, aging and
  notify-neighbor, a lost link, and a peer with our network number.
- Integration: `appletalk-afp` (System 6: the guest asks for a router and
  routes its lookups through it), `appletalk-afp-sys7` (System 7.5: zone
  requests), `appletalk-afp-longname` (the router off).
- Multi-process integration rows (`TEST_RUNNER`), linked over UDP on the
  loopback interface with `appletalk.router.link_udp`:
  - `appletalk-router-links`: three routers in a triangle learn every network
    and zone at distance 1; C closes its link to A, and A and C reroute to each
    other through B at distance 2.
  - `appletalk-router-afp`: two instances in one zone; B shares its host
    services over a friend link. A's System 6 guest finds B's file server in
    the Chooser (BrRq → FwdReq → LkUp on B's network) and mounts its volume:
    ATP, ASP and AFP across both routers.
  The processes run at independent speeds, so the scripts never wait on fixed
  budgets for each other: they poll routing state with large caps and finish
  on a barrier (marker directories, or the link's BYE).

### Browser: the tab mesh

`src/platform/wasm/em_atalk_tabs.c` links the routers of every tab of one
browser profile, with no user action. It runs in the emulator's own worker
(the router's thread) over one `BroadcastChannel`, `gs-atalk`:

- every 2 s an eligible tab (a machine on the cable, router on) posts
  `{t: "hello", id, net}`; a tab that hears another's hello opens a link to
  it, so each pair has one link, one on each side;
- link records travel as `{t: "rec", from, to, d}`, addressed by tab id;
- a tab not heard for 7 s is lost (its routes go bad); one that turns
  AppleTalk off posts `{t: "bye"}`.

At startup each tab picks a random network number (`atalk_router_set_net`):
tabs that all kept the default 1 would refuse each other. A clash between two
random numbers is refused like any other.

**Hidden tabs keep running.** Browsers pause `requestAnimationFrame` in hidden
documents, workers included, which stopped the machine (and its router) the
moment its tab was hidden. On `visibilitychange` the main loop now switches to
a 16 ms timer while hidden and back to RAF when shown (`em_update_loop_timing`
in `em_main.c`); a worker's timers are not throttled like the page's. Nothing is
drawn meanwhile. A browser may still freeze or discard a long-hidden tab; its
links then go silent and are lost on the other tabs after 7 s.

`tests/e2e/web2-specs/appletalk-tab-mesh.spec.ts`: three pages in one context
link themselves, pick three numbers and reach each other's networks at
distance 1; a closed page is noticed. A second test fires the visibilitychange
a hidden tab gets and checks the machine runs on (headless Chromium never
pauses RAF, so this is a smoke test of the switch, not of a real hidden tab).

### Headless transport

`src/platform/headless/headless_atalk_link.c` implements the platform hook
`platform_atalk_link_udp` (`platform_hooks.h`; the weak default answers "not
on this platform"): one UDP socket per link, bound to `local` and connected
to `remote`, one record per datagram. `hl_pump_once` drains the sockets
between frame-units, so records reach the router on the machine's thread. UDP
may lose a record; nothing above needs it not to (DDP is best effort, HELLO is
retried, PINGs are periodic).

## 7. Known debts

- The web app picks a random network number per tab at startup; it is not yet
  kept per machine configuration, so a guest may see a new number after a
  reload (it learns it again at boot).
- NBP replies to a local lookup still go to the requester's DDP source rather
  than to the address in the tuple (the two are the same for a guest on this
  network); replies to a forwarded lookup go to the tuple.
- ATP's exactly-once cache keys a transaction by the requester's node, not its
  network: with `share_host_services` on, a friend's node with the same number
  as the guest's could collide.
- The zone list keeps one name per network (LocalTalk is nonextended).

## 8. See also

- [appletalk.md](../../../reference/protocols/appletalk.md) — the protocol reference.
- [appletalk_server.md](appletalk_server.md) — the AFP server on node 33.
