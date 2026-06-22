# AppleTalk Phase 2 over LocalTalk

# Part I — Physical and Link Layers

---

## 1. LocalTalk Physical Layer (RS-422)

### 1.1 Electrical characteristics

LocalTalk uses the Macintosh RS-422 serial interface at **230.4 kb/s**, half-duplex, differential signaling. (Hardware tolerance varies; not a normative spec value.)
The line operates with transformer-coupled differential pairs and a shared bus topology of ≤ 300 m total cable length and ≤ 32 nodes per segment.

| Signal | Description                | Notes                                    |
| ------ | -------------------------- | ---------------------------------------- |
| TXD ±  | Differential transmit pair | FM0 encoded data from SCC                |
| RXD ±  | Differential receive pair  | Monitors bus activity                    |
| GND    | Shield reference           | Connected through transformer center tap |

Idle line = mark (logical 1).  Logic 0 is represented by a transition within the bit cell (FM0).

### 1.2 Encoding — FM0

Each bit time (1 / 230 400 ≈ 4.34 µs) contains at least one transition.

* **Logical 1** → transition at bit boundary only.
* **Logical 0** → transition in middle of bit cell.
  LLAP is HDLC‑like: frames are delimited by flag bytes; the frame trailer carries a 2‑byte FCS (CRC‑CCITT, poly 0x1021); abort is 12–18 ones.

### 1.3 Medium access — CSMA/CA

LocalTalk uses **Carrier Sense Multiple Access with Collision Avoidance**:

1. Node monitors RXD for **idle ≥ 200 µs**.
2. Apply randomized backoff per Appendix B (mask‑based local/global backoff).
3. Transmit **RTS (0x84)**.
4. If no **CTS (0x85)** is received, back off and retry (typically with exponential adjustment).
5. On successful **CTS (0x85)** handshake, send frame.

This scheme minimizes collision bandwidth loss because collisions happen during the short RTS/CTS phase.

---

## 2. LLAP — LocalTalk Link Access Protocol

### 2.1 Overview

LLAP is the AppleTalk data-link layer.  It delivers datagrams between nodes on a single LocalTalk segment using the CSMA/CA medium.
Services provided:

* Dynamic node ID assignment (1–254)
* Best-effort, error-free frame delivery
* Directed and broadcast transmission
* Encapsulation of DDP (short header) and link-management packets

### 2.2 Node ID range

| Value   | Meaning                                    |
| :------ | :----------------------------------------- |
| 0       | Link Control (used in Enquiry/Acknowledge) |
| 1 – 254 | Usable node addresses                      |
| 255     | Broadcast                                  |

### 2.3 Dynamic Node ID Acquisition

1. Pick random ID (1–254).
2. Send **ENQ (0x81)** control frame to that address.
3. Wait **Enquiry Timeout = 200 ms**.
4. If no **ACK (0x82)** is received, ID is free.
5. Otherwise, pick another ID and retry up to 20 times.

```text
[Example]
Src 00  → Dst 12 : 01
If node 12 exists, it replies:
Src 12 → Dst 00 : 02
```

### 2.4 LLAP Frame Format

```
+------------+------------+------------+------------+-----------------+------------+
| DestNodeID | SrcNodeID  | Type       | Data[0..N]                |  FCS (2B)  |
|   (1 B)    |   (1 B)    | (1 B)      | variable (≤ 600 B)        |  CRC-CCITT |
+------------+------------+------------+----------------------------+------------+
```

| Field      | Size  | Description                               |
| :--------- | :---- | :---------------------------------------- |
| DestNodeID | 1     | Target node ID (1–254; **255 = broadcast**) |
| SrcNodeID  | 1     | Sender’s node ID                          |
| Type       | 1     | Frame type (see below)                    |
| Data       | ≤ 600 | Encapsulated DDP packet or control info   |
| FCS        | 2     | CRC-CCITT, 16-bit remainder (poly 0x1021) |

### 2.5 Type Field Values

Use the canonical LLAP Type code list from Inside AppleTalk Appendix C ("LLAP type field values"). ENQ/CTS are link‑management bytes and are not LLAP header Type values. This document intentionally does not restate numeric codes; consult Appendix C and align any examples accordingly.

### 2.6 Frame Example — Directed DDP Delivery

```text
Dest: 07  Src: 0E  Type: [Short DDP]
DDP (short header)
  LenLo = 0x2C  LenHi = 0x00
  DestSocket = 0x02 (NBP)
  SrcSocket  = dynamic (example: 0xAB)
  Type       = 0x02 (NBP)
  Data = <NBP Lookup packet>
```

### 2.7 Broadcast Delivery

DestNodeID = **0xFF**.
Used for NBP Lookups and ZIP queries. Nodes that receive the broadcast only deliver the encapsulated DDP up‑stack if the destination socket is open (a listener is present).

### 2.8 Error Handling

Frames failing FCS check are silently discarded.  LLAP does not retransmit; upper layers (ATP or ASP) provide reliability.

---

## 3. Timing Parameters (Summary)

| Parameter              | Symbol          | Typical Value     | Description                                                   |
| :--------------------- | :-------------- | :---------------- | :------------------------------------------------------------ |
| Inter-Frame Gap        | IFG             | 200 µs            | Idle before transmission; also max CTS start window (directed) |
| Randomized Backoff     | —               | —                  | Mask‑based (local/global) per Appendix B                      |
| Idle Detect Gap        | IDG             | 400 µs            | Broadcast idle detect before RTS‑to‑255                       |
| Enquiry Timeout        | T<sub>ENQ</sub> | 200 ms            | Node‑ID probe reply wait                                      |
| Max LLAP packet size   | —               | 603 B             | 3‑byte LLAP header + ≤600 B data; frame trailer adds 2‑byte FCS |

---

Directed vs. broadcast timing: Directed transfers require the destination to begin CTS within 200 µs (the IFG window) after receiving an RTS. For broadcast, sense idle for ≥ 400 µs (IDG), apply randomized backoff (Appendix B), send RTS‑to‑255, then transmit if the line remains idle within ≤ 200 µs (IFG).

Note: Values above are typical/implementation parameters. For normative names and magnitudes, see Inside AppleTalk Appendix B (algorithms) and Appendix C (parameter tables).

## 4. Example Transmission Dialog

**Directed Data Transfer**

```
A → B   : RTS (0x84)
B → A   : CTS (0x85)
A → B   : LLAP frame (Type = [per Appendix C])
```

If A receives no CTS, it backs off randomly before retrying.

---

## 5. Implementation Notes (for emulation)

* Simulate per-node transmit queues and random back-off.
* Enforce single-talker rule: only one active transmission at a time.
* Maintain NodeID map (1–254) → MAC instance.
* Treat broadcast (**255**) as multi-cast to all nodes.
* CRC-CCITT calculated over Dest..Data fields; initial value = 0xFFFF.

---

# Part II — Network and Naming Layers

*(Source references: Inside AppleTalk 2nd Ed., Ch. 4, 7–8 ; Inside Macintosh: Networking 1994)*

---

## 1. DDP — Datagram Delivery Protocol

### 1.1 Overview

DDP forms the AppleTalk **network layer**, providing connectionless datagram delivery between sockets on nodes.
On LocalTalk (non-extended network), DDP uses the **short header format** (5 bytes).
Routers, network numbers, and extended headers are not used.

| Service                     | Description                                       |
| :-------------------------- | :------------------------------------------------ |
| Best-effort packet delivery | Delivers one datagram per LLAP frame              |
| Socket dispatch             | Each socket ID (1–254) maps to a listener process |
| Checksum (optional)         | Omitted on LocalTalk short header                 |
| Broadcast support           | LLAP DestNodeID = 255 → broadcast                 |
| Encapsulation               | Encapsulated in LLAP (Type per Appendix C)        |

---

### 1.2 Addressing

An AppleTalk node address = **(Network Number, Node ID)**.
In LocalTalk, network number = fixed constant (e.g., 1) and omitted in short header.

| Component | Bits | Description                                           |
| :-------- | :--- | :---------------------------------------------------- |
| Node ID   | 8    | Unique 1–254 assigned by LLAP                         |
| Socket    | 8    | Endpoint within node (1–254; 255 = wildcard reserved) |

Socket 0 is unused. There is no dedicated broadcast socket; broadcast occurs at the link layer (LLAP DestNodeID = 0xFF) and is delivered only to processes with the destination socket open.

---

### 1.3 Short DDP Header Format (5 bytes on LocalTalk)

On a single LocalTalk segment (non-extended network), the DDP short header is 5 bytes with no checksum field. The first two bytes encode a 10‑bit length value.

```
+-----------------+------------+------------+------------+
| LenHi(2b)|LenLo | DestSocket | SrcSocket  | Type       |
+-----------------+------------+------------+------------+
  byte0 bits 1..0 → Length[9:8]
  byte1            → Length[7:0]
```

Length decoding: length = ((byte0 & 0x03) << 8) | byte1. This is the total DDP size (header + payload).

| Field      | Size | Description                                              |
| :--------- | :--- | :------------------------------------------------------- |
| Length     | 2    | Total DDP length (5‑byte header + data)                  |
| DestSocket | 1    | Destination socket ID                                    |
| SrcSocket  | 1    | Source socket ID                                         |
| Type       | 1    | DDP protocol type (ATP=0x03, NBP=0x02, AEP=0x04, …)      |

Note: Some references show checksum fields for other media or the 13‑byte extended header. On LocalTalk short DDP, checksum is omitted (treated as zero).

---

### 1.4 Socket numbers and DDP Types

Consult Inside AppleTalk Appendix C for authoritative DDP Type values (protocol numbers) and well‑known sockets. Do not conflate DDP Type with socket numbers; listeners must bind to the correct well‑known sockets for each protocol per Appendix C.

---

### 1.5 DDP Example (LocalTalk short header)

```text
00 0D 08 AB 03 ...
Length=0x000D
DestSocket=0x08 (ASP listener)
SrcSocket=0xAB (client ephemeral)
Type=0x03 (ATP)
Payload=ATP header (8 bytes), followed by ASP payload (if any)
```

---

### 1.6 Behavior

* Best-effort delivery; no retries or acks.
* On LocalTalk short DDP, the checksum field is not present.
* Socket listeners must be registered before receiving.
* Broadcast (LLAP DestNodeID = **255**) is delivered only to nodes that have the destination socket open.

---

### 1.7 AEP — AppleTalk Echo Protocol (DDP Type 0x04)

Clients sometimes probe reachability using AEP. The request is carried directly in a DDP datagram with `Type=0x04` (AEP). Servers simply echo the payload back to the sender (same socket tuple), without interpretation.

Minimal interop:

- Listen for DDP `Type=0x04` on any socket you care about (or broadly),
- Reply with the exact payload you received, swapping DDP src/dst sockets and nodes.

This is useful for diagnostics and for some system components that verify a peer before initiating higher‑level protocols.

---

## 2. NBP — Name Binding Protocol

### 2.1 Purpose

NBP maps human-readable names to AppleTalk addresses (node + socket + network).
It is used by the Chooser to find services (e.g., “LaserWriter”, “AFPServer”) and by servers to register themselves.

### 2.2 Name Format

A full name is a 3-tuple:

| Element | Example              | Description                  |
| :------ | :------------------- | :--------------------------- |
| Object  | “LaserWriter 12/640” | Instance name                |
| Type    | “LaserWriter”        | Service type                 |
| Zone    | “*”                  | Zone name (single zone here) |

Tuple is encoded as Pascal-style length-byte followed by string (≤ 32 bytes each).

---

### 2.3 Packet Structure

```
+------+--------+-----------+-------------+------------------+
| Func | TupleCnt | NBPID | Tuples[...] | (variable) |
+------+--------+-----------+-------------+------------------+
```

| Field    | Size     | Description                          |
| :------- | :------- | :----------------------------------- |
| Func     | 1        | Function code (see table)            |
| TupleCnt | 1        | Number of tuples (1–15)              |
| NBPID    | 1        | Transaction ID (random)              |
| Tuples   | variable | One or more object/type/zone records |

NBP tuples are themselves structured:

```
+-----------------+-----------------+-----------------+--------------+
| NetHi | NetLo | Node | Socket | Enumerator | Obj | Type | Zone |
+-----------------------------------------------------------------+
```

---

### 2.4 Functions

| Code | Name          | Direction          | Description                     |
| :--- | :------------ | :----------------- | :------------------------------ |
| 1    | NBP_Lookup    | Client → broadcast | Find names matching type/object |
| 2    | NBP_LkUpReply | Server → unicast   | Reply with matching tuple       |
| 3    | NBP_Brk       | Client → server    | Cancel in-progress lookup       |
| 4    | NBP_Register  | Server → broadcast | Announce name in use            |
| 5    | NBP_Delete    | Server → broadcast | Withdraw name                   |
| 6    | NBP_Confirm   | Client ↔ server    | Confirm name still valid        |

---

### 2.5 Typical Service Names

| Service           | Object               | Type          | Zone |
| :---------------- | :------------------- | :------------ | :--- |
| AppleShare Server | “Shared Folders”     | “AFPServer”   | “*”  |
| LaserWriter       | “LaserWriter 12/640” | “LaserWriter” | “*”  |

---

### 2.6 Lookup Sequence (Example)

**Client finding a LaserWriter:**

1. Client broadcasts DDP to socket 2 (NBP), Func=Lookup, Type=“LaserWriter”.
2. All nodes holding matching registrations respond with NBP_LkUpReply.
3. Client displays reply tuples in Chooser list.
4. User selects target; client uses returned address (node + socket).

```text
[Broadcast]
SrcSocket=dynamic (example: 0xAB)  DestSocket=0x02 (NBP)
NBP Lookup:  object="=",  type="LaserWriter",  zone="*"
```

Server reply contains:

```text
Net=0x0001 Node=0x0E Socket=0x06 Obj="LaserWriter 12/640"
```

---

### 2.7 Name Registration Workflow

**Server registration:**

1. Generate NBPID.
2. Broadcast NBP Register for <Obj,Type,Zone>.
3. If duplicate found (same tuple reply), append enumerator digit (1–255).
4. Maintain registration table; re-register every T≈1 min.

---

### 2.8 NBP Examples

#### Register Frame Example

```text
DDP Type=NBP (0x02), DestSocket=2, SrcSocket=server
Func=4 (Register), TupleCnt=1, NBPID=0x17
Tuple: Net=0x0001, Node=0x0E, Socket=0x08,
  Obj="Shared Folders", Type="AFPServer", Zone="*"
```

#### Lookup Reply Frame

```text
Func=2 (LookupReply), NBPID=0xA1
Tuple: Net=0x0001, Node=0x0E, Socket=0x06,
       Obj="LaserWriter 12/640", Type="LaserWriter", Zone="*"
```

---

### 2.9 Timers and Retry Parameters

| Parameter               | Value | Description            |
| :---------------------- | :---- | :--------------------- |
| NBP Retransmit Count    | 8     | Maximum lookup retries |
| NBP Retransmit Interval | 1 s   | Between retries        |
| NBP Name Aging          | 2 min | Re-register period     |

---

## 3. ZIP — Zone Information Protocol

### 3.1 Purpose

ZIP associates network numbers with zone names for user browsing.
In a single, non-routed LocalTalk segment, ZIP is trivial: it reports only the default zone “*”.

### 3.2 Minimal Implementation (One Zone)

Implement the following ATP-style requests:

| Request     | Function                             | Response |
| :---------- | :----------------------------------- | :------- |
| GetMyZone   | Returns zone name of requesting node | “*”      |
| GetZoneList | Returns list of zones for network    | [“*”]    |

All fields can be static; no router interaction needed.

### 3.3 GetMyZone Reply Example

```text
ZoneNameCount=1
ZoneName="*"
```

---

### 3.4 ZIP Timers (Optional)

| Parameter          | Default | Purpose                      |
| :----------------- | :------ | :--------------------------- |
| ZIP Query Interval | 30 s    | (routers only — unused here) |
| ZIP Reply Timeout  | 2 s     | Default client wait time     |

---

### 3.5 Behavior in Single-Segment Emulation

* Zone list fixed to ["*"].
* All NBP tuples use Zone="*".
* No ZIP Notify or FwdReq processing required.
* Ignore multicast addresses and RTMP interaction.

---

# Part III — Transaction and Session Layers

*(Source references: Inside AppleTalk 2nd Ed., Ch. 9–12 ; Inside Macintosh: Networking 1994)*

---

## 1. ATP — AppleTalk Transaction Protocol

### 1.1 Overview

ATP provides **reliable request/response** service over DDP.
Each *transaction* consists of one **request** and one or more **responses**, identified by a **Transaction ID (TID)**.
ATP guarantees *at-least-once* (ALO) or *exactly-once* (XO) semantics with retransmission and acknowledgment bitmaps.

Used by:

* ASP (AppleTalk Session Protocol)
* PAP (Printer Access Protocol)
* ZIP, NBP confirmations, etc.

Encapsulated in DDP packets with `Type = 0x03`.

---

### 1.2 ATP Header Format

```
+--------+--------+--------+--------+--------+--------+--------+--------+
| CtlByte| Bitmap | TransIDHi | TransIDLo | UserBytes[0..3]             |
|                                            | (Responder may echo)       |
+--------+--------+--------+--------+--------+--------+--------+--------+
| Data... (0–578 bytes total payload)                                   |
+-----------------------------------------------------------------------+
```

| Field     | Size     | Description                                 |
| :-------- | :------- | :------------------------------------------ |
| CtlByte   | 1        | Control bits (see below)                    |
| Bitmap    | 1        | Response bitmap or sequence bits            |
| TransID   | 2        | Transaction ID (TID), assigned by requester |
| UserBytes | 4        | User-defined data (e.g., ASP session ref)   |
| Data      | variable | Request or response data                    |

---

### 1.3 Control Byte (bit definitions)

ATP uses the top two bits for the function, with additional flags below:

| Bits | Mask | Meaning                                |
| :--- | :--- | :------------------------------------- |
| 7..6 | 0xC0 | Function: 01=TReq, 10=TResp, 11=TRel   |
| 5    | 0x20 | XO (Exactly‑Once requested by client)  |
| 4    | 0x10 | EOM (End Of Message)                   |
| 3    | 0x08 | STS (Send Transaction Status)          |
| 2..0 | 0x07 | Timeout/retry interval (request field) |

Common values:

* `0x40` = TReq (ALO)
* `0x60` = TReq + XO (Exactly‑Once)
* `0x90` = TResp + EOM (single‑packet response)
* `0xB0` = TResp + XO + EOM
* `0xC0` = TRel (Release)

---

### 1.4 Requester Behavior

1. Create TID (16-bit, increment per transaction).
2. Send ATP Request DDP packet to responder’s socket (e.g., 8 for ASP).
3. Start timer `Treq = 1 s`; retry up to 8 times if no Response bitmap received.
4. When all response bits acknowledged, send **Release** (CtlByte=0xC0).

### 1.5 Responder Behavior

Exactly‑Once (XO) request handling is implemented with a transactions list and duplicate filtering:

1. On receiving a TReq:
   - If XO bit set (0x20), search the transactions list for a matching tuple `(src node, src socket, dst socket, TID)`.
     - Not found (new request): insert a new entry, mark it as executing, then deliver the request to the application.
     - Found and response already sent: this is a duplicate → retransmit the cached response(s) immediately without invoking the application again.
     - Found and still executing: ignore the duplicate request (no response).
   - If XO is not set, process normally (no caching required).
2. Generate TResp packet(s); set EOM on the final response. If XO, attach a copy of the response bytes to the entry and mark it response‑ready so duplicates can be answered directly from the cache.
3. On receiving a TRel (Ctl=0xC0) from the requester, remove the corresponding entry from the transactions list.
4. Release timer: time‑stamp entries upon insertion and periodically purge entries older than the release interval (prevents permanent retention if a TRel is lost). In this emulator, a coarse tick‑based timeout is used with a small, bounded cache.

Constants (as used in code):

```
#define ATP_CONTROL_TREQ    0x40
#define ATP_CONTROL_TRESP   0x80
#define ATP_CONTROL_TREL    0xC0
```

---

### 1.6 Example — Simple ALO Transaction

**Request:**

```text
Ctl=0x40 Bitmap=0x00 TID=0x1234 User=00000000
Payload="Hello"
```

**Response:**

```text
Ctl=0x42 Bitmap=0x01 TID=0x1234 User=00000000
Payload="OK"
```

Requester releases after response receipt.

---

### 1.7 Request Bitmap vs. Response Sequence

ATP supports up to 8 response packets per transaction.

• In a TReq (request), the second byte is a **bitmap** of desired response numbers (bit 0 → response 0, … bit 7 → response 7). For a single‑packet exchange this is typically `0x01`.

• In a TResp (response), the second byte is a **sequence number** (0–7) identifying which response this packet is (e.g., `0x00` for the first/only response). The responder sets **EOM (0x10)** on the final response.

This mechanism supports fragmentation of large replies (up to 8×578 bytes). After receiving all responses, the requester sends **TRel (0xC0)**; responders do not reply to TRel.

---

### 1.8 Timers (typical)

| Parameter | Default | Description              |
| :-------- | :------ | :----------------------- |
| Treq      | 1 s     | Request timeout          |
| Trel      | 30 s    | Response cache retention |
| Tresp     | 2 s     | Inter-response delay     |
| Retries   | 8       | Max retransmissions      |

---

## 2. ASP — AppleTalk Session Protocol

### 2.1 Overview

ASP sits atop ATP and provides **session-based** interaction between a client and a server process (e.g., AFP).
It supports opening, maintaining, and closing sessions, sending commands, and exchanging status or attention messages.

| ASP Packet | Transport framing                       | Description                     |
| :--------- | :--------------------------------------- | :------------------------------ |
| OpenSess   | ATP transaction                          | Establishes session with server |
| Command    | ATP transaction                          | Sends AFP command block         |
| Write      | ATP transaction                          | Transfers AFP write data        |
| GetStatus  | ATP transaction (SLS, out‑of‑band)       | Queries server status           |
| Tickle     | ATP‑ALO transaction (infinite retry)     | Keepalive ping                  |
| CloseSess  | ATP transaction                          | Terminates session              |

Encapsulated as ASP over ATP within DDP packets (DDP Type = 0x03).

---

### 2.2 Session Identification

Each ASP session is identified by a **Session Reference Number** (SRN), a 16-bit integer assigned by the server in the OpenSessReply.
Subsequent ASP commands include the SRN in the header.

---

### 2.3 ASP Packet Structure

#### Common ASP Header (in ATP Data — excludes SPFunction)

| Field         | Size     | Description                                  |
| :------------ | :------- | :------------------------------------------- |
| SessionRefNum | 2        | Session reference (0 for OpenSess)           |
| ReqRefNum     | 2        | Request reference number (client-assigned)   |
| CmdResult     | 2        | Result or error code (see note below)        |
| Data          | variable | Command-specific content (e.g., AFP payload) |

Important: In this stack, the ASP operation selector (SPFunction) is carried in ATP UserBytes[0], not inside the ASP data. The table above describes only the ASP bytes present in ATP Data after the SPFunction has been indicated via the ATP header.

Special case — ASP Command replies: For SPCommand, the ASP command reply data begins with a **4‑byte CmdResult** (big‑endian), followed by the command‑specific reply bytes (see Inside AppleTalk Figure 11‑12). CmdResult is not carried in ATP UserBytes.

Exceptions (authoritative, no ASP data bytes):
- SLS GetStatus: Encoded entirely in ATP UserBytes; response data carries the status block (see 2.10).
- OpenSess: Encoded entirely in ATP UserBytes; both request and reply have zero ATP data. See 2.5 and 2.6.

---

Note — Exception (authoritative): The out-of-band ASP GetStatus at the Server Listening Socket (SLS) does not use this header. Instead, the request is encoded entirely in ATP UserBytes with no ATP Data, and the response data contains the status block bytes. See section 2.10.

### 2.4 Functions (SPFunction values)

| Code | Name          | Direction     | Description                     |
| :--- | :------------ | :------------ | :------------------------------ |
| 1    | CloseSess     | Either        | Terminate session               |
| 2    | Command       | Client→Server | Send AFP command                |
| 3    | GetStatus     | Client→Server | Retrieve server status block    |
| 4    | OpenSess      | Client→Server | Establish a new session         |
| 5    | Tickle        | Either        | Session keepalive               |
| 6    | Write         | Client→Server | Transfer file data              |
| 7    | WriteContinue | Client→Server | Continue long write sequence    |
| 8    | Attention     | Server→Client | Async notification              |

---

Location of SPFunction: For all ASP operations (OpenSess, Command, Write, etc.), this stack encodes the SPFunction in ATP UserBytes[0]. The ASP bytes placed in ATP Data begin with SessionRefNum (or 0 for OpenSess), followed by ReqRefNum, CmdResult, and any command-specific data. Do not prepend an ASP Function byte inside the ATP data.

### 2.5 Session Establishment Workflow

1. **NBP Lookup**: Client finds `AFPServer` via NBP.
2. **OpenSess (special, no ASP data)**: Client sends ATP Request to the Server Listening Socket (SLS, socket 8 — per Appendix C) with zero ATP data. UserBytes encode:
  - UserBytes[0] = SPFunction = OpenSess (0x04)
  - UserBytes[1] = WSS (workstation session socket)
  - UserBytes[2..3] = ASP version (16‑bit)
3. **OpenSessReply (no ASP data)**: Server replies with zero ATP data. UserBytes encode:
  - UserBytes[0] = SSS (server session socket)
  - UserBytes[1] = Session ID
  - UserBytes[2..3] = Error code (16‑bit; 0x0000 = OK)
4. **Tickle**: Each side sends periodic Tickle packets every 30 s to maintain the session.
5. **Command Exchanges**: ASP Commands encapsulate AFP commands using ATP (Function=Command). Subsequent packets from the workstation must include the Session ID and target the SSS returned by the server.
6. **CloseSess**: Client or server terminates; both sides release ATP resources.

Practical interop notes:

* Some classic Mac stacks send an initial ATP TReq to ASP’s well‑known socket (8) with **no ASP payload** as a reachability probe. Replying with an **empty TResp** (`Ctl=0x90`, `Seq=0`) is sufficient to advance to OpenSess. Alternatively, a minimal GetStatus reply also works.
* Mirror the requester’s **XO bit** (0x20) in responses for XO transactions.
* The client will send **TRel (0xC0)** after receiving the response(s); do not reply to TRel.
* For compatibility, some systems also try a legacy ASP socket (54). Accepting both 8 and 54 improves interoperability.

---

### 2.6 Example — OpenSess

```text
DDP: Type=ATP (0x03)
ATP Request (to SLS socket 8; zero ATP data):
  UserBytes=04 <WSS> <ASPVerHi> <ASPVerLo>
ATP Response (zero ATP data):
  UserBytes=<SSS> <SessionID> <ErrHi> <ErrLo>
```

---

### 2.7 Tickle packet and session management

A session remains open until it is explicitly terminated by either end or until one end goes down or becomes unreachable. ASP provides a mechanism known as session tickling that is initiated as soon as a session is opened. In session tickling, each end periodically sends a packet to the peer’s session socket to indicate liveness. If either end fails to receive any packets (tickles, requests, or replies) on a session for the configured maintenance timeout, it assumes the peer is unreachable and closes the session.

Framing (authoritative per Inside AppleTalk): Tickle is carried as an ATP-ALO transaction request with the ASP selector encoded in the ATP UserBytes. The request has infinite retries and a ~30 s retry/timeout. Fields:

- UserBytes[0] = SPFunction = Tickle (0x05)
- UserBytes[1] = SessionID (1 byte)
- UserBytes[2..3] = 0x00 0x00 (unused)
- ATP Data: none

Responder behavior:

- Do not send any response to a Tickle. Receipt of any packet (Tickle, request, or reply) simply refreshes the local session liveness timer. If no packets are received for the configured idle period (≈120 s), the session is closed.

---

### 2.8 ASP State Machine (simplified)

```
CLOSED → (OpenSess) → OPEN
OPEN ↔ (Command/Write/Tickle)
OPEN → (CloseSess) → CLOSED
```

---

### 2.9 ASP Timers

| Parameter             | Default | Description            |
| :-------------------- | :------ | :--------------------- |
| ASP Tickling Interval | 30 s    | Keepalive period       |
| ASP Session Timeout   | 120 s   | Close after idle       |
| ASP Request Timeout   | 60 s    | ATP-level retry window |

---

### 2.10 GetStatus at the Server Listening Socket (SLS) — out‑of‑band status

Authoritative behavior: SPGetStatus is an out‑of‑band ASP operation that lets a workstation obtain a server’s status block without opening a session. No Session Reference Number (SRN) or ASP header is present, and no ASP sequence state is created. (Per Inside AppleTalk Ch. 11; also see Ch. 13, which notes that FPGetSrvrInfo should be implemented using the ASP GetStatus mechanism.)

Why it matters: Chooser and similar browsers issue GetStatus before establishing a session to display server info (AFP FPGetSrvrInfo) and decide which authentication methods to offer.

Transport framing (no session):

- DDP: Type = ATP (0x03); DestSocket = ASP well‑known socket (8 in this document); SrcSocket = client ephemeral.
- ATP Request (ALO):
  - UserByte0 = SPFunction = GetStatus (0x03)
  - UserByte1–3 = 00 00 00
  - Bitmap typically 0x01 (expect one response)
  - ATP Data = none (no ASP header)
- ATP Response(s):
  - UserBytes = 00 00 00 00
  - Data = bytes of the Service Status Block (possibly fragmented across ≤ 8 responses, up to ~4624 bytes total)
  - Final response sets EOM; requester then sends TRel.

Status block — binary layout (returned bytes):

| Offset | Size      | Field                               | Description |
| :----- | :-------- | :---------------------------------- | :---------- |
| 0      | 2 bytes   | Offset to Machine Type               | Byte offset from start of block to the Pascal string holding the machine type |
| 2      | 2 bytes   | Offset to count of AFP Versions      | Byte offset from start of block to the count of AFP versions |
| 4      | 2 bytes   | Offset to count of UAMs              | Byte offset from start of block to the count of User Authentication Modules |
| 6      | 2 bytes   | Offset to Volume Icon and Mask       | Byte offset from start of block to the optional icon/mask pair (0 = none) |
| 8      | 2 bytes   | Flags                                | Bit 0 = SupportsCopyFile; Bit 1 = SupportsChgPwd; remaining bits = 0 |
| 10     | variable  | Server Name (P‑string)               | 1‑byte length followed by characters |
| …      | variable  | Machine Type (P‑string)              | 1‑byte length followed by characters |
| …      | 1 byte    | Count of AFP Versions                | Number of AFP version strings to follow |
| …      | n×P‑str   | AFP Versions                         | Each as a Pascal string (e.g., “AFPVersion 2.0”) |
| …      | 1 byte    | Count of UAMs                        | Number of supported UAMs |
| …      | m×P‑str   | UAM Names                            | e.g., "No User Authent", "Cleartxt Passwd", "Randnum Exchange" |
| …      | 256 bytes | Volume Icon and Mask (optional)      | Two 32×32 bitmaps (icon and mask), 128 bytes each |

Notes:

- Strings are Pascal style (length byte + data).
- The precise content is defined by AFP; ASP transports this opaque block.
- Servers commonly keep the block ≤ 578 bytes to avoid fragmentation, but clients must handle multi‑response replies up to the ATP maximum (≤ 8 responses, ~4624 bytes).

Server/client implementation guidance:

- Server maintains a status blob associated with the SLS; update it when capabilities (AFP versions, UAMs, flags) change.
- On SPGetStatus, construct the ATP Request/Response as above—no SRN, no ASP header; place the status block only in the ATP Response data.
- Support XO semantics if requested and cache per ATP rules; set EOM on the final fragment.

Example — SPGetStatus (single‑packet reply):

```
DDP:  Type=ATP (0x03)  DestSocket=0x08 (ASP)  SrcSocket=0xAB

ATP Request:
  CtlByte=0x40 (TReq ALO)  Bitmap=0x01  TID=0x4A12
  UserBytes=03 00 00 00   ; SPFunction=GetStatus
  Data: — (none)

ATP Response:
  CtlByte=0x52 (TResp EOM)  Seq=0x00  TID=0x4A12
  UserBytes=00 00 00 00
  Data: <Service Status Block bytes per layout above>

ATP Release (from requester):
  CtlByte=0xC0 (TRel)
```

### 2.11 ASP Commands on an open session

Once a session has been opened, the workstation client of ASP can send a sequence of commands to the server end. These commands are delivered in the same order as they were issued at the workstation end, and replies to the commands are returned to the workstation end by ASP. The two types of commands, SPCommands and SPWrites, differ in the direction of the primary flow of data. In addition, the server end can send an SPAttention call to the workstation end to inform the workstation of some server need. The following sections describe how ASP uses ATP to perform these commands.

SPCommands are very similar to ATP requests. The ASP workstation client sends a command (encoded in a variable-length command block) to the server-end client requesting the server to perform a particular function and to send back a variable-length command reply. Examples of such commands are requests to open a particular file on a file server or to read a certain range of bytes from an already opened file. In the first case, a small amount of reply data is returned; in the second case, a multipacket reply might be generated. Each SPCommand translates into an ATP request sent to the SSS, and the command reply is received as one or more ATP response packets.

In this stack, when the ASP client makes an SPCommand call, ASP sends an ATP‑XO request to the SSS of the indicated session with the following header fields (in ATP UserBytes), and the command block as ATP payload:

```
UserBytes[0] = SPFunction = Command (0x02)
UserBytes[1] = ASP SessionID (1 byte)
UserBytes[2] = SequenceHi (16‑bit sequence number)
UserBytes[3] = SequenceLo

ATP Data = Command block bytes (AFP opcode followed by parameters)
```

Responder behavior:

- Execute the requested command and return the reply in one or more ATP responses; set EOM on the final response. XO (0x20) is mirrored from the request.
- For large replies, multiple responses may be used (sequence numbers 0..7); this emulator may clip to one packet and set EOM until fragmentation is implemented.
- Duplicate XO requests are answered from the cached response (see ATP XO semantics in section 1.5).

Reply result encoding:

- For SPCommand replies, the ATP data begins with a **4‑byte CmdResult** (big‑endian). `0x00000000` indicates success (NoErr). Failures use negative AFP codes (e.g., BadVersNum = `0xFFFFEC75`).
- The AFP reply payload bytes (if any) follow the 4‑byte CmdResult. For errors, the reply payload is typically empty.

Notes:

- SPWrites (workstation → server data) and SPAttention (server → workstation indication) follow similar ASP-over-ATP framing conventions but differ in data direction. They are not detailed here.

## 3. ADSP — AppleTalk Data Stream Protocol

### 3.1 Overview

ADSP provides a **reliable byte-stream connection** similar to TCP, built directly on DDP (`Type = 10`).
It is often used for high-performance AFP or printer channels, though AFP over ASP is sufficient for most clients.

Key services:

* Connection establishment (Open, OpenAck)
* Sequenced, acknowledged data delivery
* Flow control (windows)
* Forward Reset and Attention messages

---

### 3.2 Packet Header

| Field      | Size     | Description                      |
| :--------- | :------- | :------------------------------- |
| Descriptor | 2        | Message type (control/data)      |
| CID        | 2        | Connection ID                    |
| SeqRecv    | 4        | Highest sequence number received |
| SeqSend    | 4        | Next sequence number to send     |
| Ack        | 4        | Acknowledgment number            |
| Data       | variable | Stream payload or control info   |

---

### 3.3 Connection Lifecycle

1. **Open Connection Request** → OpenConnection control packet.
2. **Open Ack** ↔ Exchange CIDs and initial sequence numbers.
3. **Data Transfer** → Full-duplex byte stream.
4. **Close** → Either end sends Close control packet.

ADSP ensures in-order, reliable delivery via positive acknowledgments and sequence tracking.

---

### 3.4 ADSP in Simple Emulation

For a single LocalTalk segment, ADSP is optional.
Most AFP and PAP clients function fully with ASP and ATP reliability.
However, implementing ADSP increases compatibility with System 7 and later.
In emulation:

* Treat ADSP like TCP-lite.
* Maintain sequence windows (~2048 bytes).
* Reconnect automatically after resets.

---

# Part IV — Application Protocols

*(Source references: Inside AppleTalk 2nd Ed., Ch. 13–14; Inside Macintosh: Networking 1994)*

---

This section provides abbreviated overviews of the application-layer protocols. For detailed specifications, see the dedicated documents:
- **AFP (Apple Filing Protocol)**: See `appletalk_server.md` for complete AFP call reference.
- **PAP (Printer Access Protocol)**: See `appletalk_printer.md` for comprehensive PAP documentation.

---

## 1  Printer Access Protocol (PAP)

### 1.1 Overview

PAP provides a bidirectional, flow-controlled session between a workstation and a printer.
It runs over ATP on DDP socket 6. Each connection is full-duplex and packet-windowed.

Used by: LaserWriter and AppleTalk spoolers.

---

### 1.2 PAP Frame Types

| Code | Name           | Purpose                                 |
| :--- | :------------- | :-------------------------------------- |
| 0x01 | OpenConn       | Open printer connection                 |
| 0x02 | OpenConnReply  | Server response                         |
| 0x03 | SendData       | Host sends print data                   |
| 0x04 | Data           | Printer acknowledgment and flow control |
| 0x05 | CloseConn      | Request to close                        |
| 0x06 | CloseConnReply | Confirm closure                         |
| 0x07 | Tickle         | Keepalive                               |
| 0x08 | SendStatus     | Status query                            |
| 0x09 | Status         | Return status string                    |

---

### 1.3 Connection Establishment Sequence

```
Client → Server: OpenConn (socket 6)
Server → Client: OpenConnReply (accept or busy)
→ Full-duplex session established
```

Each side maintains window counters for data packets (1–8 outstanding).

---

### 1.4 Data Transfer

Printing is streamed through ATP transactions of type SendData/RecvData.
Flow control is managed by window bitmap and acknowledgment messages.

**Example Sequence:**

```text
Client → Server: PAP SendData (512 bytes PostScript)
Server → Client: PAP Data (bitmap=0x01) ack
(repeat until job done)
Client → Server: CloseConn
Server → Client: CloseConnReply
```

#### 2.4.1 Credit semantics and empty replies

* **SendData == read credit.** The workstation issues SendData only when PAPRead wants bytes; the printer must hold those credits until it has status text, PostScript stderr, or EOF to deliver. Immediately answering each SendData with a canned status string (or always setting EOF) creates the infinite-read loop we saw in Chooser logs.
* **Query reads need empty EOF responses.** When a workstation performs a PostScript capability query, it expects either structured PostScript or an immediate EOF. If the printer has nothing to send, reply with a zero-length Data response with EOF=1 instead of a status record. Do the same when the session is already inactive so the outstanding PAPRead completes cleanly.
* **Finish the job as soon as EOF is consumed.** Once the PAP data packet carries EOF and the ATP transaction completes, close the spool file, update the status string to `idle`, and tear down the session. Leaving the job in “printing” state after EOF causes the workstation to poll SendStatus forever.

#### 2.4.2 Lost-credit recovery

The first SendData of a session often races with OpenConnReply and is silently discarded by classic Mac OS. Retry logic must therefore:

* Timestamp every outstanding SendData request.
* Retransmit whenever a Tickle **or** SendStatus arrives and the request has been pending for ~500 ms.
* Continue retransmitting until the bitmap reflects progress. Tickle packets are not guaranteed—SendStatus polling is far more common—so the retry hook must run in both paths.

#### 2.4.3 Data response hygiene

* Treat `atp->bitmap` in Data responses as a sequence number (0–7). Convert it to a mask before touching the pending bitmap so that retries/duplicates do not append the same bytes again.
* Inspect the pending bitmap before writing to disk. If the bit is already cleared, ignore the payload entirely.
* Honor the ATP `EOM` flag: if the Mac sets it, force the outstanding bitmap to zero even if you originally asked for more packets. That tells PAP the transaction has finished and prevents stuck credits.

---

### 1.5 Status Query

At any time the client may query printer state using `SendStatus`.
The printer returns a PostScript-style dictionary encoded as ASCII:

```
status: (idle)
product: (LaserWriter)
printerType: (PostScript)
version: (23.0)
note: (Ready)
```

---

### 1.6 Example Connection (Complete)

```text
1. NBP Lookup (type="LaserWriter") → Node=0x0E, Socket=6
2. ATP OpenConn → OK
3. PAP SendData: "%!PS-Adobe-3.0\n"
4. PAP SendData: "...<job>..."
5. PAP SendData: "showpage\n%%EOF\n"
6. PAP CloseConn → CloseConnReply
7. Session ended
```

---

## 2  LaserWriter PostScript Channel

### 2.1 Overview

PAP payload is opaque to AppleTalk; it carries **PostScript** print streams.
A PostScript job follows Adobe Document Structuring Conventions (DSC).

### 2.2 Job Structure

```
%!PS-Adobe-3.0
%%Title: MyDocument
%%Pages: 2
%%EndComments
...prolog...
%%Page: 1 1
(draw operations)
showpage
%%Page: 2 2
...
showpage
%%EOF
```

Each job is delivered as a contiguous stream of PAP SendData packets.

---

### 2.3 End-of-Job Handling

* Printer consumes PostScript until `%%EOF`.
* Upon completion, PAP connection is closed by printer or client.
* Status changes to `idle` when ready for next job.

---

### 2.4 Spooling and Error Reporting

Spoolers (e.g., AppleShare Print Server) queue jobs and return status via PAP Status dictionary.
If PostScript errors occur, printer sends a status update containing `status:(stopped)` and `note:(error)`.

---

### 2.5 Example status strings

| Status String | Meaning        |
| :------------ | :------------- |
| `idle`        | Printer ready  |
| `printing`    | Processing job |
| `busy`        | Buffer full    |
| `stopped`     | Error state    |

---

### 2.6 Implementation Guidelines (for emulator)

* Accept PostScript bytes over PAP and write to spool file or pipe to renderer.
* Respond with `idle`/`printing` states as appropriate.
* Maintain job queue IDs for multiple clients.
* Periodically send Tickle to keep connection alive.
* Support GetStatus requests even when no job active.

---

## 3  Service Discovery Summary

| Service             | NBP Object           | NBP Type      | Socket | Underlying Protocol |
| :------------------ | :------------------- | :------------ | :----- | :------------------ |
| AppleShare Server   | “Shared Folders”     | “AFPServer”   | 8      | ASP/ATP/DDP/LLAP    |
| LaserWriter Printer | “LaserWriter 12/640” | “LaserWriter” | 6      | PAP/ATP/DDP/LLAP    |

---

## 4  End-to-End Example Flows

### 4.1 AppleShare Mount (Guest Session)

```
NBP Lookup (type="AFPServer")
→ Node=0x0E Socket=8
ASP OpenSess → OK (Session 0x21)
AFP FPGetSrvrInfo → info reply
AFP FPLogin (Guest) → OK
AFP FPOpenVol("SharedDisk") → VolID=1
AFP FPLogout → ASP CloseSess
```

### 4.2 LaserWriter Job

```
NBP Lookup (type="LaserWriter")
→ Node=0x0F Socket=6
ATP OpenConn → OK
PAP SendData (PostScript header)
PAP SendData (job body)
PAP CloseConn → CloseConnReply
```

---

# Appendix — Field breakdown examples (no raw hex)

*(Source references: Inside AppleTalk 2nd Ed., App. B–C; Inside Macintosh: Networking 1994)*

To avoid propagating incorrect numeric constants, this appendix shows field‑level breakdowns rather than literal hex dumps. When you need concrete byte values, always consult the canonical type/socket/value tables in Inside AppleTalk Appendix C and compute final FCS values after assembling frames.

Examples include:

- LLAP + DDP (Short Header) — broadcast NBP Lookup: LLAP DestNodeID = 0xFF, Type = [Short DDP per Appendix C]; DDP DestSocket = 0x02 (NBP); SrcSocket = dynamic.
- NBP Lookup Reply — tuple includes service’s listening socket (e.g., 0x06 for LaserWriter/PAP).
- ATP transaction — ASP OpenSess (SLS socket 8 per Appendix C) with UserBytes conveying OpenSess fields; zero ATP data.
- AFP SPCommand — XO request carrying AFP payload with the opcode as the first byte of ATP data; reply data begins with a 4‑byte CmdResult, followed by the AFP reply payload.
- PAP Open/SendData/Status — layered on ATP over DDP socket 6.

