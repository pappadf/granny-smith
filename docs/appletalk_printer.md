# AppleTalk Printer Access Protocol (PAP)

## 1. Overview

The **AppleTalk Printer Access Protocol (PAP)** is a **session-level, connection-oriented protocol** used to communicate between AppleTalk workstations and servers.
Historically named for printing, PAP is actually a general request/response session protocol usable by many kinds of servers—not just printers.

PAP provides:

* Connection setup and teardown
* Full-duplex data exchange
* Duplicate-free, reliable delivery via ATP (Exactly-Once transactions)
* Server discovery and status reporting via NBP
* A connection arbitration mechanism for fair job scheduling
* Detection and teardown of half-open/dead connections via tickling

PAP runs on top of **ATP** and uses **NBP** for name registration and lookup.
Workstation-side PAP and server-side PAP are intentionally asymmetric.

---

## 2. Protocol Architecture

PAP relies on lower-level AppleTalk transports as follows:

* **NBP** — used for server name registration and workstation lookup of servers
* **ATP (Exactly-Once mode)** — transports PAP packets, including OpenConn, Data, Status, Tickle
* **DDP** — datagram layer for ATP & NBP
* **Data Link** — physical network (LocalTalk, EtherTalk, etc.)

PAP servers open a **Session Listening Socket (SLS)** where they register at least one NBP entity name. Clients use NBP Lookup to find this SLS before initiating a PAP connection.

---

## 3. PAP Services

PAP defines several services for establishing and managing connections. The main categories:

### 3.1 Client-side services

* **PAPOpen** — Opens a connection to a server’s SLS. Performs NBP lookup (unless given direct SLS address). Returns a connection reference and status string.
* **PAPRead** — Requests incoming data; triggers a SendData request to the peer.
* **PAPWrite** — Sends data (possibly with EOF marker). Obeys flow-control rules determined by negotiated flow quantum; callers may also send a zero-length write to signal EOF explicitly.
* **PAPClose** — Closes the connection and cancels tickle transactions.
* **PAPStatus** — Retrieves the server’s current status string without requiring a connection.

### 3.2 Server-side services

* **SLInit** — Opens an SLS, registers an NBP name, sets server flow quantum.
* **GetNextJob** — Indicates server availability to accept a new job/connection. Multiple outstanding calls allow multiple simultaneous jobs.
* **SLClose** — Shuts down the SLS and terminates all active connections.
* **PAPRegName / PAPRemName** — Add or remove server names associated with an SLS.
* **HeresStatus** — Updates the server’s status string (returned in Status and OpenConnReply).

---

## 4. Server Connection States

A PAP server cycles through several states depending on outstanding **GetNextJob** calls:
(See diagram in source.)

* **Blocked** — No GetNextJob pending; server reports “busy.”
* **Waiting** — One or more GetNextJob calls queued; server can accept new jobs.
* **Arbitration (ARB)** — 2-second window to fairly select among multiple simultaneous OpenConn requests using WaitTime.
* **Unblocked** — Server has completed ARB and still has remaining GetNextJob readiness.

Servers may host multiple independent PAP “servers” by opening multiple SLS sockets via multiple SLInit calls.

---

## 5. Connection Establishment

### 5.1 Workstation Initiation

1. Client calls **PAPOpen**, giving a server name (or direct SLS address).
2. PAP performs **NBP Lookup** to obtain server’s SLS address.
3. Client opens ATP responding socket **Rw** and generates an 8-bit **ConnID**.
4. Client sends **OpenConn (TReq)** to the server. Includes:

   * ConnID
   * Rw responding socket
   * Client flow quantum
   * WaitTime (age of connection attempt)


**ConnIDs must be chosen to avoid collisions**, especially when opening many connections rapidly.

### 5.2 Server Arbitration

When an OpenConn request arrives:

* If **Blocked**, server sends OpenConnReply → *PrinterBusy*.
* If **Waiting**, the **first** OpenConn request triggers a **2-second ARB interval**, collecting additional OpenConn requests and matching them to pending GetNextJob entries. Later requests arriving during that window do not restart the timer—they are evaluated within the same interval.
* Selection is based on the client-supplied **WaitTime** to ensure fairness.
* At end of ARB, server accepts as many connections as matching GetNextJob entries, sending OpenConnReply (success) with:

  * ConnID
  * Server’s responding socket **Rs**
  * Server flow quantum
  * Status string


If no GetNextJobs remain after ARB, the server returns to **Blocked**.

During the ARB window, if all pending GetNextJob slots already track a WaitTime and a new OpenConn arrives with a **greater** WaitTime, the server replaces the slot holding the **smallest** WaitTime. Otherwise the request is rejected as “busy.”

### 5.3 Workstation Retry Behavior

If server returns *PrinterBusy*, the workstation waits ~2 seconds, increases WaitTime, and retries OpenConn (ATP retry count = 5; retry interval = 2 seconds). **Every retry must refresh the WaitTime field** with the total elapsed attempt duration so the server can compare fairness accurately. Retries continue until success or the application aborts.

---

## 6. Data Transfer Phase

After connection opens, PAP:

* Negotiates **flow quantum** for each side (max buffer units of 512 bytes).
* Enters full-duplex read-driven data transfer.

### 6.1 Read-driven model

A PAPRead call sends an ATP **SendData** request to the peer, equal in size to the read buffer (must equal the local flow quantum). The read buffer provided to PAPRead **must not be smaller** than the negotiated flow quantum; most stacks simply size the buffer exactly to that quantum.

Receipt of SendData grants **send credit** for PAPWrite at the opposite end. The direction is always reader→writer: the side that wants to read issues SendData, and the peer must remember that credit until it has real data (or an intentional EOF) ready to satisfy the read. Sending an empty **Data** response without EOF simply causes the reader to immediately retry, so robust implementations queue the PAPWrite until credit exists and only return zero-length Data when they simultaneously assert EOF.

SendData (the ATP TReq generated by PAPRead) uses **infinite retries** with a **15-second retry interval**. The request carries an ATP bitmap that advertises how many 512-byte buffers are available. PAP sets this bitmap to match the local flow quantum so the peer knows exactly how much data it may transmit.


### 6.2 Writing

A PAPWrite will transmit data only if send credit is available—otherwise it queues until a SendData arrives. The writer never fabricates its own SendData sequence; instead it waits for the peer’s PAPRead-driven credit and consumes that credit when emitting Data responses.
Data is transmitted using ATP **Data** responses:

* ≤512 bytes per packet
* Last packet has **EOM** bit set
* EOF indicator may be included in the last write **or carried in a zero-length write** dedicated to EOF


Flow-control rule: **PAPWrite cannot exceed the peer’s flow quantum**. If a caller attempts to send more data than the available credit allows, PAPWrite must immediately return an error without transmitting anything.

### 6.3 Tickle mechanism & connection timer

To detect half-open connections, each side maintains a **2-minute connection timer**:

* Any received PAP/ATP packet resets the timer.
* Each side sends ATP **Tickle** requests periodically. The retry interval is defined as **half the connection timeout** (120s timeout → 60s tickle cadence) and retry count is infinite.
* Receiver resets timer but sends no response.
* If timer expires → connection considered dead; PAP tears down the connection.

### 6.4 Printer-to-workstation chatter

LaserWriter-class devices occasionally deliver PostScript status text back to the workstation (for example, to display `%%[ status: printing; jobname=Foo ]%%` dialogs). That path still follows the read-driven model: the workstation issues PAPRead (which becomes a SendData TReq aimed at the printer’s responding socket) and the printer answers with a Data response only when it has status bytes or wants to signal EOF. Real hardware often batches several status strings per SendData credit; conversely, a server that has nothing to report should simply set the EOF flag when returning a zero-length Data so the workstation does not loop forever trying to read nonexistent output.

The emulator now mirrors that behaviour: every workstation-issued SendData credit is queued and left pending inside ATP until we have fresh status text or an intentional EOF to deliver. When the spooler updates the printer status (`%%[ status: printing job … ]%%`) the queued credit is serviced immediately; otherwise the request remains outstanding so PAPRead blocks instead of spinning. EOF is asserted only when the PostScript stream is complete or the job is torn down, which matches the LaserWriter flow-control model and prevents the Chooser loop observed in earlier builds.


### 6.5 Lost-credit recovery and query reads

Practical interoperability hinges on a few additional rules that came directly from log-based feedback:

* **Retry send-data aggressively when polled.** The very first SendData request is easy to lose because the printer often issues it before the workstation finishes processing OpenConnReply. We timestamp every outstanding SendData and, whenever a Tickle **or** SendStatus arrives, immediately retransmit if the credit has been outstanding for ~500 ms. That keeps PostScript input moving even when the client never sends Tickles.
* **Never answer SendData with status strings.** SendData is workstation-issued read credit, not a status poll. Holding the request open (until we have PostScript stderr/status bytes or an intentional EOF) prevents the `SendData → Data (EOF)` spin that previously broke Chooser and LaserWriter 8.
* **Use empty EOF replies for query reads.** The workstation may open a short-lived connection to ask PostScript capability questions. When we have no reply payload, we immediately return a zero-length Data response with EOF set, both for inactive sessions and for in-progress jobs that have no PS-level answer. This unblocks PAPRead without leaking status-channel text into the data stream.
* **Finish the job as soon as EOF is observed.** Once PAPWrite delivers a packet with the EOF flag and the ATP bitmap has been satisfied, the emulator closes the spool file, updates the status string to `idle`, and tears down the session instead of sending more status responses. Leaving the session open causes the Mac to issue endless SendStatus probes and keeps the query connection alive forever.


---

## 7. Duplicate Filtration

ATP XO mode guarantees duplicate-free **responses**, but not duplicate-free **requests** in internets. PAP handles this using a sequence number:

* **SendData** requests include a 16-bit sequence number (1–65535, wraps to 1).
* Sequence 0 means “unsequenced” (legacy compatibility) and is rarely emitted by LaserWriter-era stacks.
* Receiver accepts a request only if `sequence == last_sequence + 1`; duplicates are ignored even though ATP will faithfully retry.
* Otherwise the request is ignored as a duplicate.


Each side independently maintains:

* Its own outbound SendData sequence
* The last accepted sequence from peer

ATP responses still require additional guardrails:

* **Treat `atp->bitmap` in Data responses as a *sequence number*, not a mask.** Convert it to a mask via `(1u << seq)` before touching the outstanding bitmap. This keeps retransmitted packets (or packets resent after a retry) from being misinterpreted as new data.
* **Check the bitmap before writing to the spool.** Always confirm that the corresponding bit in `pending_bitmap` is still set before appending payload bytes to disk; duplicates must be ignored to keep the spool file byte-accurate.
* **Honor the ATP EOM bit.** When the responder sets `ATP_CONTROL_EOM`, it is explicitly saying “no more fragments for this transaction,” even if your bitmap still advertises free slots. Force `pending_bitmap = 0` in that case so the transaction can complete and release credit quickly.
* **EOF closes the session.** When the PAP EOF flag arrives in the final packet of the transaction and the bitmap has been drained (possibly via EOM), call the session-finishing path immediately so the workstation sees the job as complete.

---

## 8. Connection Termination

A PAPClose call:

1. Sends ATP **CloseConn** request.
2. Receiver replies with **CloseConnReply** (courtesy only).
3. Both sides cancel all pending ATP transactions, including Tickle.

Server may continue processing the job internally after receiving CloseConn, issuing new GetNextJob when ready for next job.
Server may force-close all jobs via **SLClose**.


---

## 9. Status Gathering

* PAPStatus is sent to a server’s SLS at any time—no connection required.
* Server’s PAP replies with **Status** response containing Pascal-style status string (1 length byte + ≤255 bytes).
* Status strings are supplied by SLInit or HeresStatus.


---

## 10. PAP Packet Formats

PAP uses ATP user bytes for the PAP header:

| User Byte | Meaning                                                              |
| --------: | -------------------------------------------------------------------- |
|         1 | ConnID (0 for SendStatus/Status)                                      |
|         2 | PAP Function                                                          |
|         3 | Data packets: EOF flag \| SendData: sequence high byte \| Tickle: 0   |
|         4 | SendData: sequence low byte \| All other packets: 0                  |

*(Code typically indexes these bytes as user[0..3]; the table preserves the spec’s 1-based numbering.)*

Additional header notes:

* **Tickle** packets require User Bytes 3 and 4 to be zero.
* **SendStatus/Status** packets must use ConnID = 0.
* **SendData** requests propagate their ATP bitmap to represent the available buffers (i.e., the local flow quantum).

Packet formats include:

* **OpenConn / OpenConnReply** (includes socket numbers, flow quantum, WaitTime/result, status string)
* **SendData** (includes seq number; no payload — represents reader-issued credit)
* **Data** (includes EOF flag + payload ≤512 bytes — emitted when servicing a SendData credit)
* **Tickle**
* **CloseConn / CloseConnReply**
* **SendStatus / Status**

The list above refers to data fields located in the **ATP payload**. ConnID and PAP Function always reside in the user-byte header, separate from these payload structures.
---

## 11. PAP Function Codes

| Function       | Value |
| -------------- | ----- |
| OpenConn       | 1     |
| OpenConnReply  | 2     |
| SendData       | 3     |
| Data           | 4     |
| Tickle         | 5     |
| CloseConn      | 6     |
| CloseConnReply | 7     |
| SendStatus     | 8     |
| Status         | 9     |

**OpenConnReply Result Codes:**

* **0 — NoError**
* **$FFFF — PrinterBusy**


---

## 12. PAP Client Interface

Below is a concise summary of each call:

### 12.1 PAPOpen (workstation)

* Params: server name or SLS address, flow quantum, status buffer
* Returns: result code, connection refnum
* Status string may update multiple times as OpenConnReplies arrive.


### 12.2 PAPClose (workstation/server)

* Closes connection and cancels transactions.

### 12.3 PAPRead / PAPWrite (both ends)

* Read: returns data, length, EOF flag
* Write: sends data (≤ peer flow quantum), can send EOF

### 12.4 PAPStatus (workstation)

* Returns server status string; no connection required.

### 12.5 SLInit (server)

* Opens SLS, registers name, sets flow quantum, initial status
* Returns server refnum

### 12.6 GetNextJob (server)

* Announces readiness for next connection
* Returns connection refnum for accepted job

### 12.7 SLClose (server)

* Shuts down SLS & closes all open jobs

### 12.8 PAPRegName / PAPRemName (server)

* Add/remove server NBP names for an SLS

### 12.9 HeresStatus (server)

* Updates server status string


---

## 13. PAP Parameters for the LaserWriter

The Apple LaserWriter implements PAP with these constraints:

* **Flow quantum: 8**
* **Only one job at a time** (never unblocked state; only waiting, ARB, blocked)


Timers and limits:

* Max data unit: **512 bytes**
* Max status string: **255 bytes**
* Tickle timer: **60 sec**
* Connection timeout: **2 min**
* OpenConn retry: **5 attempts**, **2 sec interval**


