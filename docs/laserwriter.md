# LaserWriter Driver Workflow

Classic Macintosh systems running the LaserWriter driver follow a repeatable sequence any time a user prints over AppleTalk. The notes below describe that sequence from the driver and printer perspective, using examples captured from original PostScript traffic.

## 1. Discovery and Session Establishment

1. **NBP lookups.** The workstation issues Name Binding Protocol lookups for type `LaserWriter` in the chosen AppleTalk zone until it finds the desired printer.
2. **PAP OpenConn.** Once selected, the driver opens a Printer Access Protocol connection. The OpenConn payload specifies the client socket, desired flow quantum (usually 8), and optional status socket/interval. The printer replies with a status string such as `status: print spooler processing job` and both sides record the negotiated flow control parameters.
3. **Initial SendData credit.** Immediately after OpenReply, the driver sends a PAP `StatusRead` (function code `SendData`) to provide the printer with a credit to deliver status text or PostScript query responses. Credits continue to be granted this way throughout the session.
4. **Keepalives.** PAP tickles and explicit `SendStatus` calls are used to confirm the printer is alive and to fetch the current status text while no job data is pending.

## 2. PatchPrep Capability Query

Before streaming any user document, the driver determines whether the printer already holds Apple’s PatchPrep procset. The query is a small PostScript program sent over the newly opened PAP data channel:

```postscript
%!PS-Adobe-2.0 Query
%%Title: Query for PatchPrep
%%?BeginProcSetQuery: "(AppleDict md)" 71 0
userdict/PV known{userdict begin PV 1 ge{(1)}{(2)}ifelse end}{/md where{pop(2)}{(0)}ifelse}ifelse = flush
%%?EndProcSetQuery: unknown
```

* The driver expects the printer to execute this code and send the numeric result (`0`, `1`, or `2`) back over the status channel, terminated with CR/LF, exactly as a PostScript `=` operator would print it.
* A real LaserWriter answers `0` if PatchPrep is not installed and `1` once it has been successfully loaded. The `2` response indicates a more advanced revision.

### PatchPrep Upload

If the reply indicates PatchPrep is missing, the driver immediately transmits the full procset (roughly 2.4 KB) as ordinary PostScript data. The transfer begins with:

```postscript
%!PS-Adobe-2.0 ExitServer
%%BeginExitServer: 0000000000
serverdict begin exitserver
%%EndExitServer
%%Title: "PatchPrep -- The Apple PostScript Header"
%%Creator: Apple Software Engineering
%%CreationDate: Thursday, August 17, 1989
%%Patches version #1 0
%% ... hex payload follows ...
```

The printer ingests this payload, writes it into `userdict`, and responds with the value `1` when the upload is complete so the driver knows not to resend it in the future.

## 3. Font Directory Query

After resolving PatchPrep, the driver inventories the fonts it can expect on the device. The PostScript query looks like this:

```postscript
%!PS-Adobe-2.0 Query
%%Title: Query for list of known fonts
%%?BeginFontListQuery
save/scratch 100 string def FontDirectory{pop =}forall
systemdict/filenameforall known{(fonts/*){(.)search {pop pop pop}{dup length 6 sub 6 exch getinterval =}ifelse}scratch filenameforall}if
(*) = flush restore
%%?EndFontListQuery: *
```

The script prints each resident font name followed by a newline, then emits `*` to mark the end of the list. The driver continuously issues PAP `StatusRead` requests so the printer can deliver these strings over the status channel. A conventional LaserWriter reports the standard 13 built-in fonts (Courier, Helvetica, Times families, and Symbol) before the terminating asterisk.

## 4. Document Transmission

With the environment prepared, the driver streams the actual PostScript job:

1. **SendData requests.** The printer alternates between issuing PAP SendData transactions (requests for more PostScript) and waiting for responses. Each response corresponds to a segment of the document.
2. **Placeholder EOFs.** While the driver is waiting to start the real job it may send `%%EOF` placeholders on the status channel; printers typically ignore a small number of these until true job data arrives.
3. **Status updates.** Throughout the job the driver polls for status text (`StatusRead`) and may show progress messages to the user.
4. **Completion.** When the final `%%EOF` for the document arrives, the printer closes the job, returns `status: idle` on the next credit, and the driver either issues CloseConn or keeps the PAP session alive for the next print request.

## Summary of Expected Printer Behavior

* **Status strings** should follow the PAP format (`status: …`) and be ready any time the driver issues a `StatusRead`.
* **PatchPrep handshake** always precedes the first real page. Answer the initial query (`0` or `1`) and, if an upload follows, report completion with `1` before accepting subsequent queries.
* **Font list reply** is newline-delimited, terminated by `*`, and delivered via the status channel using the credits supplied by the workstation.
* **Flow control** relies entirely on the PAP SendData bitmap/credit scheme; the printer must not send unsolicited data.

Understanding this sequence makes it easier to build accurate emulations or troubleshoot why a particular workstation is stuck waiting—if PatchPrep never acknowledges, the driver simply keeps repeating the procset upload and never advances to the font query or main document.
