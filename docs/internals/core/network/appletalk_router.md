# The AppleTalk router

The router in the built-in AppleTalk stack: `src/core/network/appletalk_router.c`
and `appletalk_router.h`, wired into the stack in `src/core/network/appletalk.c`
(DDP dispatch, NBP BrRq, the connection lifecycle). The protocols are specified
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

Until a link to another instance exists, the routing table holds only the local
network: a gateway that leads nowhere. Links, transit forwarding and RTMP/ZIP
between routers are later work on the same module.

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
  .seed    r/w  run as router (default true)
  .net     r/w  this network's number, 1..65279 (default 1)
  .zone    r/w  zone name (default "Granny Smith")
  .stats        rtmp_data_out, rtmp_requests, zip_queries, zip_atp_requests, nbp_brrq
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
- Integration: `appletalk-afp` (System 6: the guest asks for a router and
  routes its lookups through it), `appletalk-afp-sys7` (System 7.5: zone
  requests), `appletalk-afp-longname` (the router off).

## 7. Known debts

- The network number is fixed per process until the web app chooses one per
  machine; two instances need different numbers before they can be linked.
- NBP replies still go to the requester's DDP source rather than to the address
  in the tuple; the two are the same for a guest on this network.

## 8. See also

- [appletalk.md](../../../reference/protocols/appletalk.md) — the protocol reference.
- [appletalk_server.md](appletalk_server.md) — the AFP server on node 33.
