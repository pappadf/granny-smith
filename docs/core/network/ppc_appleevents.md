# PPC Toolbox and Apple events on the wire

*In-house reference for `src/core/network/appletalk_ppc.c` and
`src/core/network/appletalk_aevt.c`. This document is the **sole coding
reference** for the two layers, the role
[appletalk_server.md](appletalk_server.md) plays for AFP: implementation code
cites section numbers here, not outside sources. Where a fact came from is
recorded once, in [Appendix A](#appendix-a--provenance), per the provenance
rule in `local/gs-docs/proposals/proposal-appletalk-ppc-appleevents.md` §2.*

*The transport below this document is ADSP, specified in
[appletalk.md §3](appletalk.md#3-adsp--appletalk-data-stream-protocol).*

---

## 1. What this is

System 7's **program linking** lets an application on one Macintosh send
**Apple events** to an application on another, over AppleTalk. Three layers
sit on top of DDP:

```
  Apple event            AETF byte stream                     §5
  high-level event       36-byte HighLevelEventMsg header      §4.4
  PPC session            'SREQ'/'SAPT' dialog, message blocks   §4
  ADSP                   reliable byte stream, DDP type 7      appletalk.md §3
```

We implement the host side of all three, so the emulator can pose as a peer
Macintosh: send verbs to guest applications and read structured replies.

Two conventions hold everywhere below:

* **Byte order is big-endian**, and every multi-byte field is naturally
  aligned to an even offset. Structures are laid out as the 68K compiler of
  the era laid them out: 2-byte member alignment, and a trailing pad byte
  where the total would otherwise be odd.
* **`Str32` is 33 bytes**: a length byte followed by 32 bytes of storage, all
  of it transmitted whether used or not. Likewise `Str63` is 64 bytes.

## 2. Shared record layouts

These three records appear inside session messages, so their byte sizes are
load-bearing.

### 2.1 `PPCPortRec` — 72 bytes

The name of one program-linking port: what a user sees in the guest's PPC
browser, plus a type discriminator.

| Offset | Size | Field | Notes |
| -----: | ---: | ----- | ----- |
| 0 | 2 | `nameScript` | Mac script code; 0 (Roman) for everything we send |
| 2 | 33 | `name` | `Str32`, the port name |
| 35 | 1 | — | pad |
| 36 | 2 | `portKindSelector` | 1 = by creator and type, 2 = by string |
| 38 | 33 | `portTypeStr` | `Str32`, when the selector is 2 |
| 38 | 4 | `creator` | overlays `portTypeStr` when the selector is 1 |
| 42 | 4 | `type` | " |
| 71 | 1 | — | pad |

Applications registered by the Process Manager use selector 2 with an
**8-character type string**: the application's four-byte signature followed
by the literal `ep01`. When a name collides, the trailing digits count up
(`ep02` … `ep99`). So the Finder's port is typically `Finder` /
`MACSep01`.

### 2.2 `LocationNameRec` — 104 bytes

Where a port lives, from the caller's point of view.

| Offset | Size | Field |
| -----: | ---: | ----- |
| 0 | 2 | `locationKindSelector`: 0 none, 1 NBP entity, 2 NBP type only |
| 2 | 33+1 | NBP object string (`Str32` + pad) |
| 36 | 33+1 | NBP type string |
| 70 | 33+1 | NBP zone string |

The three strings together are the era's `EntityName`, 102 bytes with its
explicit pads; the selector in front rounds the record to 104.

> An assembly-language equate in the same era's interfaces declares the
> embedded entity as 100 bytes, which would make this record 102. That equate
> is wrong: the C and Pascal interfaces agree on 102 + 2, and the shipping
> Toolbox is compiled from C. **104 is the wire truth.**

### 2.3 `PortInfoRec` — 74 bytes

One entry of a list-ports reply (§4.6).

| Offset | Size | Field |
| -----: | ---: | ----- |
| 0 | 1 | filler |
| 1 | 1 | `authRequired` — non-zero if the port refuses guest sessions |
| 2 | 72 | `PPCPortRec` |

## 3. Discovery: NBP and the browse

A Macintosh with program linking switched on registers **one** NBP entity:

```
<machine name> : PPCToolBox @ <zone>
```

The object is the machine's Sharing Setup name, the type is the literal
`PPCToolBox`, and the socket is the one ADSP assigned to that machine's
connection-listening socket. *Every* port on the machine is reached through
that single entity and socket — individual applications are **not** separate
NBP names.

Enumerating a machine's ports is therefore a two-step browse:

1. NBP `LkUp` for `=:PPCToolBox@*` yields one tuple per program-linking
   machine on the network (node, socket, name).
2. For each machine, open an ADSP connection to that socket and run the
   list-ports exchange of §4.6.

Our own host port advertises the same way: object = `appletalk.aevt.port_name`
(default `gs-host`), type `PPCToolBox`, zone `*`, socket = our ADSP
connection-listening socket (§7).

## 4. The PPC session layer

### 4.1 Framing rule

There is **no length prefix anywhere in this layer**. Every session message
is exactly one ADSP client message: the sender writes the block and marks the
last byte end-of-message; the receiver reads until EOM and takes the byte
count from the stream. A receiver posts a read large enough for the biggest
block it could get (290 bytes, §4.3) and lets EOM cut it short.

A message longer than 32767 bytes is split into several ADSP writes with EOM
set only on the last, so one logical block can span chunks.

### 4.2 Message types

Every block begins with a four-byte type code:

| Code | Direction | Meaning |
| ---- | --------- | ------- |
| `SREQ` | initiator → responder | session request (§4.3) |
| `SAPT` | responder → initiator | session accepted |
| `SREJ` | responder → initiator | session rejected by the PPC Toolbox |
| `UREJ` | responder → initiator | session rejected by the user or the application |
| `ACNT` | responder → initiator | continue with authentication (challenge) |
| `ARSP` | initiator → responder | authentication response |
| `LPRT` | initiator → responder | list port names (§4.6) |
| `LRSP` | responder → initiator | list-ports response trailer |

Rejection reasons, carried as the second longword of `SREJ`:

| Value | Meaning |
| ----: | ------- |
| 1 | the port exists but is not network-visible |
| 2 | no such port |
| 3 | no user record for the supplied name |
| 4 | authentication failed |
| 5 | no outstanding `PPCInform` — nobody is listening for a session |
| 6 | guest linking is not enabled on that machine |
| 7 | program linking is switched off |

A `UREJ` carries an application-defined value in the same slot.

### 4.3 The session-request block — 290 bytes

| Offset | Size | Field |
| -----: | ---: | ----- |
| 0 | 4 | `SREQ` |
| 4 | 4 | user data — handed to the responder's `PPCInform` client |
| 8 | 72 | requester's `PPCPortRec` |
| 80 | 72 | destination `PPCPortRec` |
| 152 | 104 | requester's `LocationNameRec` |
| 256 | 33 | `Str32` user name; **length 0 means a guest session** |
| 289 | 1 | pad |

Accept and reject blocks are 8 bytes: the type code, then a longword that
carries the rejection reason (`SREJ`/`UREJ`) or is unused (`SAPT`).

> The unused longword of `SAPT` is **not zeroed** by System 7 — it is whatever
> the sender's shared write buffer held. A conforming receiver ignores it, and
> so do we.

### 4.4 Session state machine

**Initiator** (what `appletalk.aevt.send` drives):

1. NBP-resolve the target machine's `PPCToolBox` entity to an address.
2. Open an ADSP connection to it (`ADSP` open dialog, appletalk.md §3.4).
3. Write the session-request block, EOM.
4. Read one block and dispatch on its first longword:
   * `SAPT` → the session is live; move to data transfer.
   * `ACNT` → authenticated flow (§4.5) — we do not implement it and fail the
     session with a readable reason.
   * `SREJ` → map the reason code of §4.2 to a message and tear down.
   * `UREJ` → the far side's client refused; report its value.
   * anything else → the peer is not speaking PPC; tear down.

**Responder** (what a guest talks to):

1. Accept the ADSP open request on the connection-listening socket.
2. Read one block. `SREQ` → look the destination port up in the local port
   registry; unknown → `SREJ` reason 2; registered but not network-visible →
   `SREJ` reason 1.
3. Check the user name: length 0 is a guest session and is accepted when
   guest linking is enabled; a non-empty name means authenticated linking,
   which we decline with `SREJ` reason 3.
4. `SAPT`, then data transfer.

Teardown has no message of its own: **closing the ADSP connection ends the
session**. A session also ends when the connection times out.

### 4.5 Authentication (not implemented; extension point)

Authenticated linking is a challenge/response over the same connection: the
responder sends `ACNT` plus eight challenge bytes, the initiator DES-encrypts
them with the user's password and echoes them in a 12-byte `ARSP` (type code
plus two longwords), and the responder compares against its own encryption of
the same challenge. Two facts matter if this is ever built: the era's
challenge generator is the clock, so both longwords are usually identical and
carry almost no entropy; and the stored password is obfuscated in the
Users & Groups file with a trivial keyed XOR, not hashed. We implement
guest-only linking, matching the AFP server's guest-only stance.

### 4.6 Listing a machine's ports

| Block | Size | Layout |
| ----- | ---: | ------ |
| request | 114 | `LPRT`(4), start index(2), requested count(2), `PPCPortRec` filter(72), `Str32` user name(33), pad(1) |
| entries | ≤ 518 | up to **7** `PortInfoRec` (§2.3) per ADSP write, EOM only on the last |
| trailer | 6 | `LRSP`(4), actual count(2) |

The start index makes the enumeration resumable; a port whose
`authRequired` byte is set will not take our guest session.

### 4.7 Data transfer: message blocks

Once a session is live, each logical message is:

| Offset | Size | Field |
| -----: | ---: | ----- |
| 0 | 4 | block creator |
| 4 | 4 | block type |
| 8 | 4 | user data |
| 12 | … | payload |

terminated by EOM. For a high-level event the creator is the **event class**
and the type is the **event ID** — the same two codes that appear again
inside the event header, which is what lets a receiver route the message
before parsing anything.

## 5. High-level events and the Apple event stream

### 5.1 `HighLevelEventMsg` — 36 bytes

The payload of a high-level-event message block starts with:

| Offset | Size | Field | What we send |
| -----: | ---: | ----- | ------------ |
| 0 | 2 | header length | 36 |
| 2 | 2 | version | **3** |
| 4 | 4 | reserved | 0 |
| 8 | 16 | `EventRecord` | see below |
| 24 | 4 | user refcon | the Apple event **return ID**, which correlates a reply |
| 28 | 4 | posting options | 0 for a plain event |
| 32 | 4 | message length | length of the AETF stream that follows |

The embedded 16-byte `EventRecord` is reused as a header rather than as an
event:

| Offset | Size | Field | Value |
| -----: | ---: | ----- | ----- |
| 0 | 2 | `what` | 23 (`kHighLevelEvent`) |
| 2 | 4 | `message` | the **event class** (`aevt`, `core`, …) |
| 6 | 4 | `when` | tick stamp; the receiver overwrites it |
| 10 | 4 | `where` | the **event ID**, an OSType overlaid on a `Point` |
| 14 | 2 | `modifiers` | bit 0 set means "this message is a reply" |

`jaym` is the message class of a **return receipt**, not of an Apple event;
an Apple event's class field carries the event's own class.

### 5.2 AETF — the flattened Apple event

The bytes counted by "message length" are an *AppleEvent Transport Format*
stream: a fixed header, then any number of attributes, then a four-byte
marker that ends the attribute section, then any number of parameters.
Attributes and parameters share one entry layout, so a reader that does not
care about the distinction can walk the whole stream with a single loop.

| Offset | Size | Field |
| -----: | ---: | ----- |
| 0 | 4 | signature, always `aevt` |
| 4 | 2 | major version, 1 |
| 6 | 2 | minor version, 1 |
| 8 | … | zero or more **meta parameters** (attributes) |
| … | 4 | `';;;;'` — end of the meta section |
| … | … | zero or more **parameters** |

Every parameter, in either section, is:

| Size | Field |
| ---: | ----- |
| 4 | keyword |
| 4 | descriptor type |
| 4 | data length, **not** counting padding |
| n | data |
| 0–1 | pad to an even offset |

so an entry advances the cursor by `round_up_even(12 + length)`. The
`';;;;'` marker is the one exception: it advances by 4. A stream must end
exactly at the end of its last parameter; anything else is corrupt. An empty
event is the 12-byte header-plus-marker, and a sender may legitimately
shorten that to a message length of 0.

### 5.3 What is *not* in the stream

Several Apple event attributes travel out of band and must be reconstructed
on receipt:

| Attribute | Actually carried in |
| --------- | ------------------- |
| event class `evcl` | `EventRecord.message` (and the block creator) |
| event ID `evid` | `EventRecord.where` (and the block type) |
| return ID `rtid` | `HighLevelEventMsg.userRefcon` |
| address `addr` | the session's identity — who sent it |
| event source `esrc`, missed keyword `miss`, refcon `refc` | local only, never transmitted |

Attributes that *do* appear in the meta section, when set: transaction ID
`tran` (`long`, omitted when zero), timeout `timo` (`long`), reply-requested
`repq` (type `true`, zero length), and interaction level `inte` (`enum`).

### 5.4 Lists and records

A `list` or `reco` descriptor's data is itself structured:

| Size | Field |
| ---: | ----- |
| 4 | item count |
| 4 | prefix size ("factoring") |
| n | prefix, padded to even |
| … | items |

The prefix is a compression device, and only four sizes are legal:

| Prefix size | Meaning | Bytes per item |
| ----------: | ------- | -------------- |
| 0 | unfactored | `[key] type length data` |
| 4 | all items share a type, held in the prefix | `[key] length data` |
| ≥ 8 | shared type **and** shared length, both in the prefix | `[key] data` |
| ≥ 8, item length 1 | packed bytes | one byte per item |

`[key]` is present for records and absent for lists. Anything else is a
malformed descriptor.

An object specifier (`obj `) is a record with the four keywords `form`,
`want`, `seld` and `from`.

### 5.5 Replies

A reply is an ordinary high-level event travelling the other way, with
`modifiers` bit 0 set and the **same return ID** in `userRefcon` — that pair
is what correlates it to the request. Its class/ID are `aevt`/`ansr`, and by
convention it carries `errn` (`long`, sign-extended `OSErr`) when something
went wrong, `errs` (`TEXT`) for the message, and `----` for the result.
`errn == 0`, or the absence of `errn`, means success.

### 5.6 Descriptor types we decode

| Code | Decoded as |
| ---- | ---------- |
| `TEXT`, `cstr` | string |
| `long`, `shor`, `magn`, `comp` | integer |
| `bool` | boolean, from the first data byte |
| `true`, `fals` | boolean, zero-length data |
| `type`, `enum`, `sign`, `prop`, `keyw` | four-character code as a string |
| `null`, `msng` | null / missing, zero-length |
| `list` | list of descriptors |
| `reco`, `obj `, `rang`, `comp` records | keyed map |
| anything else | opaque: kept as raw bytes |

An unknown type is never an error — it round-trips as raw bytes, so a
capture always re-encodes byte for byte.

## 6. In-tree representations

### 6.1 `V_MAP` form

An event decodes to an ordered map:

```json
{
  "class": "aevt",
  "id":    "odoc",
  "attrs": { "timo": { "type": "long", "data": 3600 } },
  "----":  { "type": "list", "data": [ { "type": "alis", "hex": "0200..." } ] }
}
```

* `class` and `id` are four-character strings.
* `attrs` holds the meta section, keyword → leaf.
* Parameters are top-level entries keyed by their four-character keyword, so
  `$e.reply["----"]["data"]` reads the direct object without a helper. Use
  the bracket form throughout: after a bracket index, a dotted key does not
  resolve in a chained path expression.
* A **leaf** always carries `type`. It then carries exactly one of `data`
  (a decoded string, integer, boolean, list or map) or `hex` (uppercase hex
  of the raw bytes, for types §5.6 leaves opaque). Zero-length types such as
  `true` and `null` carry neither.
* On a reply the event object also surfaces `errn` as an integer attribute of
  its own, so a script asserts `$e.errn == 0` without descending.

### 6.2 Text form

The authoring grammar. It is our own definition — inspired by the notation
Apple's developer tools used, but neither ported from nor bug-compatible with
it.

```
event   := class '/' id [ '{' [ param { ',' param } ] '}' ]
param   := key ':' desc
key     := fourcc                       # '----' is the direct object
class,
id      := fourcc
fourcc  := IDENT | "'" 4-chars "'"      # quote anything not identifier-safe

desc    := STRING                       -> TEXT
         | INTEGER                      -> long
         | true | false                 -> true / fals  (zero length)
         | null()                       -> null         (zero length)
         | type(fourcc)                 -> type
         | enum(fourcc)                 -> enum
         | '[' [ desc { ',' desc } ] ']'         -> list
         | rec  '{' [ param { ',' param } ] '}'  -> reco
         | obj  '{' form: …, want: …, seld: …, from: … '}'   -> 'obj '
         | fss(vref, dirid, "name")     -> fss   (an FSSpec)
         | hex(fourcc, "0011AABB")      -> a descriptor of that type, verbatim
```

Strings are double-quoted with `\"` and `\\` escapes. Integers may be decimal
or `0x`-prefixed. Whitespace between tokens is insignificant.

`alis(...)` — building a real alias record from a guest volume path — is
**not implemented**: an alias record embeds volume creation dates and
directory IDs that the host cannot synthesise honestly. Use `fss(...)`, which
the Apple Event Manager coerces to an alias, or `hex('alis', …)` with bytes
captured from the guest.

## 7. What this stack implements

* **Both session roles.** We answer guest-initiated sessions on our host port
  and we open sessions to guest applications.
* **Guest linking only** (§4.5).
* **One host port**, named by `appletalk.aevt.port_name`, registered when
  `appletalk.aevt.enabled` is true.
* **Deterministic identifiers.** Return IDs count up from 1 per run and the
  ADSP layer's ConnIDs likewise, so two runs of one script produce identical
  bytes.
* **Timeouts are instruction budgets**, not wall time: a pending event expires
  when the guest has retired the requested number of instructions without
  replying.
* **Untrusted input.** Everything arriving from the guest is bounds-checked
  before use; a malformed stream produces a `V_ERROR` with a reason, never a
  crash and never a partial write into the tree.
* **Not implemented:** authenticated sessions, store-and-forward (the IPM
  variant of the session layer), alias-record construction, and any host-side
  AppleScript/OSA runtime.

### 7.1 What has been verified against a real guest

`tests/integration/appletalk-ppc` (System 7.1 on a Plus) and
`tests/integration/aevt-finder` (System 7.5 on a IIci) drive the whole stack
against System 7's own `.MPP`/`.DSP` drivers, PPC Toolbox and Apple Event
Manager. Confirmed end to end: NBP discovery, the ADSP open dialog, the
list-ports browse, the session request/accept dialog with a guest user name,
message-block framing, the high-level event header, and **the AETF payload in
both directions** — the Scriptable Finder parses what we send, dispatches it,
and replies with a stream our decoder reads.

The browse independently corroborates §2.1 from the other side: the Finder's
port comes back typed `MACSep01`, the `<signature>ep01` convention.

Worked example, against the 7.5 Finder:

```
appletalk.aevt.send("Finder",
  "core/getd{'----':obj{form:enum(prop), want:type(prop), seld:type(pnam),
                        from:obj{form:enum(prop), want:type(prop),
                                 seld:type(sdsk), from:null()}}}")
→ errn 0, reply["----"]["data"] == "Macintosh HD" 
```

### 7.2 Two things a guest taught us

**A session carries one transaction.** An application services the session
its `PPCInform` accepted, and once it has answered it stops reading — without
closing anything. The ADSP connection underneath stays up, so a second event
written to that session is accepted by the transport and then silently
ignored: the send never gets a reply and eventually times out. Reusing an
open session therefore looks like an obvious optimisation and is a trap. We
open a session per event and close it when the event settles.

**The reply bit in `modifiers` is not reliable.** §5.1 documents bit 0 as
"this message is a reply", and the 7.5 Finder answers with it clear. What
actually identifies a reply is its **return ID** matching an event we are
waiting on — the same correlation the Apple Event Manager uses. We treat the
bit and the `aevt`/`ansr` class as corroboration only; matching on them alone
misfiles genuine replies into the inbox.

### 7.3 Remaining rough edges

* **`aevt/oapp` to a running Finder draws no reply.** The event is delivered
  and the session is accepted; the Finder's Open Application handler simply
  does not answer when it is already running. Events that a handler answers
  (`core/getd`, or anything unhandled, which draws `errAEEventNotHandled`)
  reply normally. Use a query, not `oapp`, when a test needs a round trip.
* **Alias records** are still not constructible host-side (§6.2), so `odoc`
  needs `fss(...)` or captured bytes.
* **Authenticated linking** remains unimplemented (§4.5).

## 8. Object-model surface

```
appletalk
  ppc
    ports        collection  name, type, machine, node, socket, auth_required
    sessions     collection  role, state, peer_node, port, bytes_in/out
    browse()                 refresh `ports` (NBP lookup + list-ports)
    stats                    sessions_opened, sessions_rejected, blocks_in/out
  aevt
    enabled      rw bool     register the host port and accept sessions
    port_name    rw string   the host port's NBP object name
    send(target, event, timeout:, tag:, mode:)  -> the event object
    send_raw(target, bytes)                     -> the event object
    events       collection  state, text, class, id, reply, errn, tag
    inbox        collection  received events: class, id, sender, map, text
    auto_reply   rw string   text-form template answering inbox events
    stats                    sent, replied, errors, received, timeouts
```

`send` is **non-blocking**: replies only arrive while the guest runs, and the
script owns the scheduler. The idiom is

```
let e = appletalk.aevt.send("Finder", "core/getd{'----':obj{…}}", tag="version")
while $e.state == "sent" { scheduler.run 2000000 }
assert $e.errn == 0
```

Named arguments use the shell's `name=value` form — `timeout=`, `tag=`,
`mode=` — not the `name:` spelling the original proposal sketched.

Event objects are append-only for the life of the run, so the `V_OBJECT` a
`send` returns stays valid in a `let` binding. A checkpoint restore drops
sessions, connections and the events collection, and keeps only
`enabled`, `port_name` and `auto_reply`.

---

## Appendix A — Provenance

Per proposal §2: the System 7 sources under `/workspaces/gs-archive` are read
to *understand* protocols and are never copied. Facts learned there are
recorded here, in our own words, and the implementation cites this document.

| Section | Fact | Learned from |
| ------- | ---- | ------------ |
| §1, §4.1 | Layering; one session message per ADSP client message, EOM-terminated; 32767-byte write chunking | behaviour of `OS/PPC/PPCNetwork.c` (sys71src) |
| §2.1–2.3 | `PPCPortRec` 72 B, `LocationNameRec` 104 B, `PortInfoRec` 74 B and their field order | public interface declarations, `Interfaces/CIncludes/PPCToolBox.h`; sizes derived from 68K MPW alignment rules |
| §2.2 note | The 102-vs-104 discrepancy | `Interfaces/AIncludes/PPCToolbox.a` equate contradicting the C and Pascal interfaces |
| §2.1 | Application port type `<signature>ep01`, incrementing on collision | `ProcessMgr/Eppc.c` (mac-rom) |
| §3 | NBP object = machine name, type `PPCToolBox`, one entity and one socket per machine | `OS/PPC/PPCInit.c`, `OS/PPC/PPCEntry.c`; the type string itself is a resource field in `OS/PPC/PPCBrowser.r`. Corroborated by *IM: Interapplication Communication* ch. 11, which gives the same NBP form |
| §4.2 | The eight message type codes and the seven rejection reasons | constant definitions, `OS/PPC/PPCCommon.h` |
| §4.3 | the session-request layout and the guest = zero-length-user-name rule; the uninitialised `SAPT` longword | struct declaration in `OS/PPC/PPCCommon.h`; behaviour in `OS/PPC/PPCNetwork.c` |
| §4.4 | Both state machines, including the rejection branches | behaviour of `OS/PPC/PPCNetwork.c` |
| §4.5 | DES challenge/response, clock-derived challenge, XOR-obfuscated stored password | behaviour of `OS/PPC/PPCAuth.c` |
| §4.6 | Seven entries per write, start-index resumption, trailer block | `OS/PPC/PPCCommon.h` constants; behaviour of `OS/PPC/PPCNetwork.c` |
| §4.7 | Message-block header; creator/type carry the event class and ID | struct declaration in `OS/PPC/PPCCommon.h`; usage in `ProcessMgr/Eppc.c` (mac-rom) |
| §5.1 | `HighLevelEventMsg` and `EventRecord` layouts; version 3; `jaym` is the return-receipt class | public interface declarations, `Interfaces/CIncludes/EPPC.h` and `Events.h`; values written by `ProcessMgr/Eppc.c` (mac-rom) |
| §5.2 | The AETF grammar, header, `';;;;'` marker, 12-byte parameter header | prose description and struct declarations in `Internal/C/AppleEventsInternal.h` (sys71src), corroborated by the parser in `ProcessMgr/AppleEventExtensions.c` |
| §5.2 | Even-byte padding, exact-end validation, the 0-length empty event | behaviour of `Toolbox/AppleEventMgr/AEDFWrapper.inc1.p` and `AEDF.inc1.p` (mac-rom) |
| §5.3 | Which attributes travel out of band | behaviour of `Toolbox/AppleEventMgr/AEDFWrapper.inc1.p` (mac-rom) |
| §5.4 | Count / prefix-size / factoring rules | record declaration in `Toolbox/AppleEventMgr/AEUtil.p`; behaviour of `AEDFWrapper.inc1.p` (mac-rom); corroborated by `ProcessMgr/AppleEventExtensions.c` (sys71src) |
| §5.5 | Reply shape, `errn`/`errs` keywords, reply bit in `modifiers` | `AppleEventReply` declaration and keyword constants in `Internal/C/AppleEventsInternal.h` |
| §5.6 | Descriptor type codes | public constants, `Interfaces/CIncludes/AppleEvents.h` |
| §6, §7, §8 | Everything here is our own design | — |

*IM: Interapplication Communication* (1993), the published specification, is
the semantic reference for chapters 3, 5, 6 and 11 (event anatomy, sending,
object specifiers, the PPC Toolbox API). It contains **no** byte-level
description of either the session protocol or the flattened event; the
tabulated layouts above therefore come from the sources listed.

## Appendix B — Constants

| Name | Value | Meaning |
| ---- | ----- | ------- |
| PPC NBP type | `PPCToolBox` | the entity type every linking machine registers |
| `SREQ`/`SAPT`/`SREJ`/`UREJ`/`ACNT`/`ARSP`/`LPRT`/`LRSP` | — | session message types (§4.2) |
| session-request block | 290 | |
| accept / reject block | 8 | |
| authentication response | 12 | |
| list request | 114 | |
| list trailer | 6 | |
| `PortInfoRec` | 74 | one browse entry |
| entries per list write | 7 | |
| high-level event header | 36 | with version 3 |
| `kHighLevelEvent` | 23 | `EventRecord.what` |
| AETF signature | `aevt` | |
| AETF version | `0x00010001` | major 1, minor 1 |
| meta terminator | `;;;;` | |
| direct object | `----` | |
| reply class / ID | `aevt` / `ansr` | |
| error keywords | `errn`, `errs` | |
