# 1 Apple Filing Protocol (AFP 2.0)

## 1.1 Overview

AFP is the high-level file service protocol used by AppleShare servers and Macintosh-family clients.  
It runs on **ASP** (AppleTalk Session Protocol), which itself runs on **ATP/DDP/LLAP**.

Goals:

- HFS-like file-system semantics over the network (volumes, directories, files, data/resource forks, Finder info).
- Platform-independent, case-insensitive names (max 31 characters in classic AFP).
- Per-session authentication and access control.
- Byte-range locking and shared access control for multi-user file sharing.

---

## 1.2 AFP Session Structure

Each AFP session operates within an ASP session. ASP provides sequencing and reliability; AFP defines the semantic file-service requests that ride on top of it.

Typical sequence:

1. **Server discovery / status**  
   ASP `GetStatus` (on the AppleTalk Server Listening Socket, no session) → returns a **Status Block** that embeds AFP server information. Logically this exposes FPGetSrvrInfo-style data before a session exists.

2. **Open ASP session**  
   ASP `OpenSess` → returns an ASP SessionRef (session ID) if the server accepts the connection.

3. **AFP login**  
   AFP `FPLogin` (opcode 0x12) is sent as an ASP `SPCommand` on the new ASP session. It negotiates AFP version and UAM and may be followed by `FPLoginCont` exchanges if the UAM needs multiple round-trips. If login fails, the client sends ASP `CloseSess` to tear the session down immediately.

4. **Browse server**  
   - `FPGetSrvrParms` (0x10) lists volumes and flags.
   - `FPOpenVol` (0x18) opens a volume and selects return parameters.
   - `FPGetVolParms` (0x11) reads additional volume parameters by bitmap.

5. **Use volumes**  
   Directory and file operations (create/delete/rename/move, parameter gets/sets), fork I/O (`FPRead`/`FPWrite` etc.), Desktop database (`FPOpenDT`/`FPCloseDT`, icon and APPL calls), and catalog enumeration (`FPEnumerate`, `FPGetFileDirParms`).

6. **Logout / close**  
   `FPLogout` terminates the AFP session; the server flushes and closes all resources associated with that session. The client (or server) then issues ASP `CloseSess` to drop the underlying transport session.

---

## 1.3 AFP on ASP

For all AFP calls except FPGetSrvrInfo, FPWrite, and FPAddIcon:

- The AFP command is carried in an ASP **SPCommand**.
- The ASP Command data payload starts with **1 byte AFP opcode** followed by that call’s AFP parameter block.
- The ASP / ATP layer returns the **32‑bit AFP result code in the 4 ATP UserBytes** of the TResp packet:
   - `user[0..3]` contain the big‑endian AFP result (e.g. `0xFFFFEC65` for ParamErr).
   - The AFP handler (`afp_handle_command`) now returns the 32-bit AFP result code and writes the **AFP reply payload**
      (if any) plus its length via an output pointer.

Consequences for this document:

- All `### Reply` tables below describe **only the AFP reply payload bytes that follow the result code**, not the result code itself.
- The AFP result code is **never part of the reply payload structures** in this stack; it always travels in the ATP header’s UserBytes.

FPWrite and FPAddIcon use ASP **SPWrite** so that large data or bitmaps can be streamed in separate ATP packets.

FPGetSrvrInfo is implemented using ASP `GetStatus` (no session on the SLS); its Status Block carries AFP‑style server info but no separate AFP result word.

---

## 1.4 AFP Command Frame Format (inside ASP Command data)

```
+---------+-----------+-----------+-----------+-----------+--------------------+
| Command | RequestID | Flags     | Reserved  | DataLength| Parameters...      |
| (1 B)   | (2 B)     | (1 B)     | (1 B)     | (2 B)     | variable (≤ 578 B) |
+---------+-----------+-----------+-----------+-----------+--------------------+
```

| Field      | Size | Meaning                                          |
| :--------- | :--- | :----------------------------------------------- |
| Command    | 1    | AFP opcode (e.g. 0x01 FPGetSrvrInfo)             |
| RequestID  | 2    | Client-assigned identifier                       |
| Flags      | 1    | Bit 0 = More Cmds (unused here), others reserved |
| DataLength | 2    | Length of parameter block                        |
| Parameters | var  | Operation-specific data                          |

---

## 1.5 AFP function opcodes

The first byte of the ASP Command payload is the AFP function opcode:

| Opcode (hex) | AFP function    |
| :----------- | :-------------- |
| 0x01         | ByteRangeLock   |
| 0x02         | CloseVol        |
| 0x03         | CloseDir        |
| 0x04         | CloseFork       |
| 0x05         | CopyFile        |
| 0x06         | CreateDir       |
| 0x07         | CreateFile      |
| 0x08         | Delete          |
| 0x09         | Enumerate       |
| 0x0A         | Flush           |
| 0x0B         | FlushFork       |
| 0x0E         | GetForkParms    |
| 0x0F         | GetSrvrInfo     |
| 0x10         | GetSrvrParms    |
| 0x11         | GetVolParms     |
| 0x12         | Login           |
| 0x13         | LoginCont       |
| 0x14         | Logout          |
| 0x15         | MapID           |
| 0x16         | MapName         |
| 0x17         | MoveAndRename   |
| 0x18         | OpenVol         |
| 0x19         | OpenDir         |
| 0x1A         | OpenFork        |
| 0x1B         | Read            |
| 0x1C         | Rename          |
| 0x1D         | SetDirParms     |
| 0x1E         | SetFileParms    |
| 0x1F         | SetForkParms    |
| 0x20         | SetVolParms     |
| 0x21         | Write           |
| 0x22         | GetFileDirParms |
| 0x23         | SetFileDirParms |
| 0x24         | ChangePassword  |
| 0x25         | GetUserInfo     |
| 0x30         | OpenDT          |
| 0x31         | CloseDT         |
| 0x33         | GetIcon         |
| 0x34         | GetIconInfo     |
| 0x35         | AddAPPL         |
| 0x36         | RemoveAPPL      |
| 0x37         | GetAPPL         |
| 0x38         | AddComment      |
| 0x39         | FPRemoveComment |
| 0x3A         | GetComment      |
| 0xC0         | AddIcon         |

---

## 1.6 AFP Result Codes (AFP result in ATP UserBytes)

In this stack, AFP result codes are reported in the **ATP UserBytes** field of the TResp, not as a separate 4‑byte word in the ASP payload. The 4 user bytes of every AFP‑carrying ATP response contain the big‑endian signed 32‑bit AFP result; a value of `0x00000000` indicates success (`NoErr`). Selected codes:

| Decimal | Hex        | Name             |
| :------ | :--------- | :--------------- |
| 0       | 0x00000000 | NoErr            |
| -5000   | 0xFFFFEC78 | AccessDenied     |
| -5001   | 0xFFFFEC77 | AuthContinue     |
| -5002   | 0xFFFFEC76 | BadUAM           |
| -5003   | 0xFFFFEC75 | BadVersNum       |
| -5004   | 0xFFFFEC74 | BitmapErr        |
| -5005   | 0xFFFFEC73 | CantMove         |
| -5006   | 0xFFFFEC72 | DenyConflict     |
| -5007   | 0xFFFFEC71 | DirNotEmpty      |
| -5008   | 0xFFFFEC70 | DiskFull         |
| -5009   | 0xFFFFEC6F | EOFErr           |
| -5010   | 0xFFFFEC6E | FileBusy         |
| -5011   | 0xFFFFEC6D | FlatVol          |
| -5012   | 0xFFFFEC6C | ItemNotFound     |
| -5013   | 0xFFFFEC6B | LockErr          |
| -5014   | 0xFFFFEC6A | MiscErr          |
| -5015   | 0xFFFFEC69 | NoMoreLocks      |
| -5016   | 0xFFFFEC68 | NoServer         |
| -5017   | 0xFFFFEC67 | ObjectExists     |
| -5018   | 0xFFFFEC66 | ObjectNotFound   |
| -5019   | 0xFFFFEC65 | ParamErr         |
| -5020   | 0xFFFFEC64 | RangeNotLocked   |
| -5021   | 0xFFFFEC63 | RangeOverlap     |
| -5022   | 0xFFFFEC62 | SessClosed       |
| -5023   | 0xFFFFEC61 | UserNotAuth      |
| -5024   | 0xFFFFEC60 | CallNotSupported |
| -5025   | 0xFFFFEC5F | ObjectTypeErr    |
| -5026   | 0xFFFFEC5E | TooManyFilesOpen |
| -5027   | 0xFFFFEC5D | ServerGoingDown  |
| -5028   | 0xFFFFEC5C | CantRename       |
| -5029   | 0xFFFFEC5B | DirNotFound      |
| -5030   | 0xFFFFEC5A | IconTypeError    |
| -5031   | 0xFFFFEC59 | VolLocked        |
| -5032   | 0xFFFFEC58 | ObjectLocked     |

---

## 1.7 General conventions for call descriptions

In the per-call reference below:

- We do not repeat the 1-byte opcode in the Request/Reply tables. It is always present as the first byte of every AFP parameter block.
- Where the spec shows an explicit 1-byte pad labeled 0, we list it as “Pad (must be 0)” in the tables.
- All dates are AFP timestamps (seconds since 00:00:00 1 Jan 1904 GMT; signed 32-bit).
- “PathType” uses:
  - 1 = short names
  - 2 = long names.
- For variable-length parameters (names, comments, etc.), the spec sometimes uses offsets rather than in-line strings. Where offsets are used, they are 16-bit integers measured from the start of the parameter area for that structure (as in FPEnumerate and FPGetFileDirParms).

---

# 2 AFP Call Reference

The following sections describe each AFP call in a consistent schema:

- Summary
- Request (parameter block after the 1-byte opcode)
- Reply (AFP reply **payload only**, i.e. bytes after the AFP result)
- Result codes
- Semantics & rights (short, implementation-oriented paraphrase of the spec)

---

## FPByteRangeLock (0x01)

### Summary

Lock or unlock a range of bytes in an open fork to prevent concurrent modification.

### Request

|    # | Field       | Type | Size | Description                            |
| ---: | :---------- | :--- | :--- | :------------------------------------- |
|    1 | Flags       | byte | 1    | Contains UnlockFlag and Start/EndFlag. |
|    2 | OForkRefNum | int  | 2    | Fork reference.                        |
|    3 | Offset      | long | 4    | Start of range.                        |
|    4 | Length      | long | 4    | Length of range.                       |

**Flags Bits:**

- **Bit 0 (UnlockFlag):** 0 \= Lock, 1 \= Unlock.
- **Bit 7 (Start/EndFlag):** 0 \= Start-relative, 1 \= End-relative.

### Reply

|   \# | Field      | Type | Size | Description                          |
| ---: | :--------- | :--- | :--- | :----------------------------------- |
|    1 | RangeStart | long | 4    | The actual start offset of the lock. |

### Result Codes

- **ParamErr** - RefNum is unknown or incorrect range
- **LockErr** - requested range locked by another user
- **NoMoreLocks** - maximum number of locks has been reached
- **RangeOverlap** - overlaps with other region locked by same user
- **RangeNotLocked** - range not locked, or locked by other user

---

## FPCloseVol (0x02)

### Summary

Close a previously opened volume. Releases the Volume ID and all associated server-side resources for that volume.

### Request

|    # | Field     | Type | Size | Description                 |
| ---: | :-------- | :--- | :--- | :-------------------------- |
|    1 | Pad (0)   | byte | 1    | Pad byte                    |
|    2 | Volume ID | int  | 2    | Volume ID from `FPOpenVol`. |

### Reply

_The reply doesn't carry any payload_

### Result Codes

- **NoErr** – Volume closed.
- **ParamErr** – Invalid Volume ID.

### Details

A client must close each opened volume before `FPLogout`. Closing a volume implicitly closes all directories, forks, and file handles associated with it.

---

## FPCloseDir (0x03)

### Summary

Close a directory opened by `FPOpenDir`.

### Request

|    # | Field        | Type | Size | Description                    |
| ---: | :----------- | :--- | :--- | :----------------------------- |
|    1 | Pad          | byte | 1    | Pad byte 0                     |
|    2 | Volume ID    | int  | 2    | Volume ID from `FPOpenVol`.    |
|    3 | Directory ID | long | 4    | Directory ID from `FPOpenDir`. |

### Reply

_The reply doesn't carry any payload_

### Result Codes

- **NoErr** – Directory closed.
- **ParamErr** – Volume ID or directory ID incorrect.

### Details

Releases server-side state associated with a directory handle.

---

## FPCloseFork (0x04)

### Summary

Close a fork previously opened implicitly by read/write operations or via parameter calls.

### Request

|    # | Field       | Type | Size | Description            |
| ---: | :---------- | :--- | :--- | :--------------------- |
|    1 | Pad         | byte | 1    | Pad byte 0             |
|    2 | OForkRefNum | int  | 2    | Fork reference number. |

### Reply

_The reply doesn't carry any payload_

### Result Codes

- **NoErr** – Fork closed.
- **ParamErr** – Bad OForkRefNum.

### Details

Closes either the data fork or resource fork of a file. 

---

## FPCopyFile (0x05)

### Summary

Copy a file to a new location (same or different volume). The server opens the source
file for read with DenyWrite; if that open fails the call returns `DenyConflict`.
On success the server creates a new file in the destination directory with the
name given by `NewName` (or the same name as the source if `NewName` is null),
assigns a unique file number, and preserves the source file’s parameters unless
explicitly changed by the requested copy bitmap.

### Request

|    # | Field               | Type   | Size | Description                                 |
| ---: | :------------------ | :----- | :--- | :------------------------------------------ |
|    1 | 0                   | byte   | 1    | Must be 0.                                  |
|    2 | Source Volume ID    | int    | 2    | Volume ID of the source file.               |
|    3 | Source Directory ID | long   | 4    | Directory containing source file.           |
|    4 | Dest Volume ID      | int    | 2    | Destination volume.                         |
|    5 | Dest Directory ID   | long   | 4    | Destination directory.                      |
|    6 | Source PathType     | byte   | 1    | 1 = short, 2 = long names.                  |
|    7 | Source Pathname     | string | var  | Pascal name of source file.                 |
|    8 | Dest PathType       | byte   | 1    | 1 = short, 2 = long names.                  |
|    9 | Dest Pathname       | string | var  | Destination parent directory (may be null). |
|   10 | NewType             | byte   | 1    | 1 = short NewName, 2 = long NewName.        |
|   11 | NewName             | string | var  | Name of copy (or null)                      |

### Reply

_The reply doesn't carry any payload_

### Result Codes

- **NoErr** – Copy successful.
- **ParamErr** – Volume ID, directory ID, or pathname is unknown or bad.
- **ObjectNotFound** – Source file missing.
- **ObjectExists** – Destination exists (unless overwrite semantics allowed).
- **AccessDenied** – Insufficient rights.
- **VolLocked** - Volume is read-only.
- **DenyConflict** - File cannot be opened for Read/DenyWrite.
- **DiskFull** – Insufficient space.
- **ObjectTypeErr** – Source is a directory (not file).

### Details

- The server attempts to open the source file for read with DenyWrite; if this open fails the server returns `DenyConflict`.
- The copy is created in the destination parent directory and given the `NewName` provided; if `NewName` is null the server uses the source file's name. The alternate name (short/long) is generated per the catalog-node naming rules.
- A unique file number is assigned to the new file and its Parent ID is set to the destination parent directory ID.
- All other file parameters (dates, Finder info, attributes, fork contents) are copied from the source unless the copy bitmap omits a particular component; the destination parent directory's modification date is updated to the server clock.
- Source and destination may be on the same or different volumes.
- The caller must have Search access to all ancestors of the source file except the source parent, and Read access to the source parent directory.
- The caller must have Search or Write access to all ancestors of the destination except the destination parent, and Write access to the destination parent directory.

---

## FPCreateDir (0x06)

### Summary

Create an empty directory on the specified volume; the server assigns a unique
directory ID per volume and initializes owner, group, rights, attributes, and timestamps as described below.

### Request

|    # | Field        | Type   | Size | Description                                  |
| ---: | :----------- | :----- | :--- | :------------------------------------------- |
|    1 | 0            | byte   | 1    | Must be 0.                                   |
|    2 | Volume ID    | int    | 2    | Volume of the directory.                     |
|    3 | Directory ID | long   | 4    | Parent directory.                            |
|    4 | PathType     | byte   | 1    | 1 = short, 2 = long.                         |
|    5 | Pathname     | string | var  | Pascal Path including name of new directory. |

### Reply

|   #   | Field            | Type | Size |                                |
| :---: | ---------------- | ---- | ---: | ------------------------------ |
|   1   | FPError          | long |    4 |                                |
|   2   | New Directory ID | long |    4 | Directory ID of new directory. |

### Result Codes

- **NoErr** – Created.
- **ParamErr** – Volume ID, directory ID, or pathname is unknown or bad.
- **ObjectNotFound** - Parent directory unknown.
- **ObjectExists** – Name in use.
- **AccessDenied** – Rights insufficient.
- **DiskFull** – Not enough volume space.

### Details

- The server creates an empty directory named by `Pathname` and assigns it a unique (per-volume) New Directory ID.
- Owner ID is set to the calling user's user ID; Group ID is set to the user's primary group ID if one exists.
- Initial access rights: owner = read, write, search; group = none; world = none.
- Finder info is initialized to 0 and all directory attributes are cleared.
- Creation date and modification date for the new directory, and the modification date of the parent directory, are set to the server clock. The directory's backup date is set to `$80000000` to indicate it has never been backed up.
- The directory's alternate name (long or short) is generated according to the catalog-node naming rules.
- The operation requires the volume to be open (a prior `FPOpenVol`), and the caller must have Search or Write access to all ancestor directories except the parent, and Write access to the parent directory.

---

## FPCreateFile (0x07)

### Summary

Create a new file on the specified volume. Behaviour differs for a soft create
(fails if the name exists) versus a hard create (replaces an existing file that
is not open). The server initializes file metadata and fork lengths as described
below.

### Request

|    # | Field        | Type   | Size | Description                                    |
| ---: | :----------- | :----- | :--- | :--------------------------------------------- |
|    1 | Flags        | byte   | 1    | Bit 7 = CreateFlag (0 = soft, 1 = hard create) |
|    2 | Volume ID    | int    | 2    | Volume of the file.                            |
|    3 | Directory ID | long   | 4    |                                                |
|    4 | PathType     | byte   | 1    | 1 = short, 2 = long.                           |
|    5 | Pathname     | string | var  | Path with name of new file.                    |

### Reply

_The reply doesn't carry any payload_

### Result Codes

- **ParamErr** – Volume ID, directory ID, or pathname is unknown or bad.
- **ObjectNotFound** – Ancestor directory not found.
- **ObjectExists** – (soft create) a file with that name already exists.
- **ObjectTypeErr** – A directory with that name already exists.
- **AccessDenied** – Caller lacks the required rights; in AFP 1.1 the volume may be ReadOnly.
- **VolLocked** – Destination volume is ReadOnly.
- **FileBusy** – (hard create) file exists and is open.
- **DiskFull** – Insufficient space on the volume.

### Details

- **Soft create:** If a file with the same name exists, returns `ObjectExists`; otherwise, creates a new file, assigns a unique file number, sets Finder info and attributes to 0, both fork lengths to 0, and sets creation/modification dates to the server clock. Backup date is `$80000000` (never backed up). Alternate name is generated per catalog rules.
- **Hard create:** If the file exists and is not open, deletes and recreates it, resetting all parameters as above.
- **Soft create rights:** Requires Search or Write access to all ancestors except parent, and Write access to parent.
- **Hard create rights:** Requires Search access to all ancestors except parent, and Read/Write access to parent.
- **Precondition:** Volume must be opened via `FPOpenVol`.

---

## FPDelete (0x08)

### Summary

Delete a file or directory.

### Request

|    # | Field        | Type   | Size | Description                                 |
| ---: | :----------- | :----- | :--- | :------------------------------------------ |
|    1 | 0            | byte   | 1    | Pad byte (must be 0)                        |
|    2 | Volume ID    | int    | 2    | Volume of the file/directory.               |
|    3 | Directory ID | long   | 4    |                                             |
|    4 | PathType     | byte   | 1    | 1 = short, 2 = long                         |
|    5 | Pathname     | string | var  | Pathname of file or directory to be deleted |

### Reply

_The reply doesn't carry any payload_

### Result Codes

- **ParamErr** – Volume ID, directory ID, or pathname is unknown or bad.
- **ObjectNotFound** – Input parameters do not point to an existing file or directory.
- **DirNotEmpty** – The directory is not empty.
- **FileBusy** – The file is open.
- **AccessDenied** – Caller lacks required rights.
- **ObjectLocked** – (AFP 2.0) the file or directory is marked DeleteInhibit.
- **VolLocked** – (AFP 2.0) the volume is ReadOnly.

### Details

- If the target is a directory, the server verifies the directory is empty; if not, it returns `DirNotEmpty`.
- If the target is a file, it must not be open by any user; otherwise the server returns `FileBusy`.
- On successful deletion, the parent directory's modification date is set to the server clock.
- Caller must have Search access to all ancestor directories except the parent directory, and Write access to the parent directory.
- Additionally, when deleting a directory the caller must have Search access to the parent directory; when deleting a file the caller must have Read access to the parent directory
- Precondition: the volume must have been opened previously via `FPOpenVol`.

---

## FPEnumerate (0x09)

### Summary

List the contents of a directory.

### Request

|    # | Field            | Type   | Size | Description                                                    |
| ---: | :--------------- | :----- | :--- | :------------------------------------------------------------- |
|    1 | Pad (0)          | byte   | 1    | Must be 0                                                      |
|    2 | Volume ID        | int    | 2    | Volume identifier.                                             |
|    3 | Directory ID     | long   | 4    | Ancestor Directory ID.                                         |
|    4 | File Bitmap      | int    | 2    | Parameters for files (format follows FPGetFileDirParms).       |
|    5 | Directory Bitmap | int    | 2    | Parameters for directories (format follows FPGetFileDirParms). |
|    6 | ReqCount         | int    | 2    | Max number of items to return.                                 |
|    7 | Start Index      | int    | 2    | 1-based index to start enumeration.                            |
|    8 | MaxReplySize     | int    | 2    | Max size of reply block.                                       |
|    9 | PathType         | byte   | 1    | 1 = short, 2 = long.                                           |
|   10 | Pathname         | string | var  | Path to directory.                                             |

### Reply

|    # | Field            | Type | Size | Description                               |
| ---: | :--------------- | :--- | :--- | :---------------------------------------- |
|    1 | File Bitmap      | int  | 2    | Echo of request.                          |
|    2 | Directory Bitmap | int  | 2    | Echo of request.                          |
|    3 | ActCount         | int  | 2    | Actual number of items returned.          |
|    4 | Structures       | var  | var  | Packed list of File/Directory structures. |

- **Struct Length** (1 byte, unsigned, includes itself, the File/Dir flag, parameters, and any trailing pad)
- **File/Dir Flag** (1 byte: bit 7 is a File/Dir Flag: 0=File, 1=Dir)
- **Parameters** (Packed according to appropriate bitmap)
- **Pad** (optional 1 byte 0x00 added only if needed to make the overall structure length even)

### Result Codes

- **ParamErr** – Bad session, volume, pathname type, or MaxReplySize too small.
- **DirNotFound** – Directory not found.
- **BitmapErr** – Requested parameter not available; tried to get Directory ID for a variable Directory ID volume; both bitmaps empty.
- **AccessDenied** – Insufficient access rights.
- **ObjectNotFound** – No more offspring to enumerate.
- **ObjectTypeErr** – Path points to a file, not a directory.
- 
### Details

- Enumerates directory contents based on input bitmaps and parameters.
- If File Bitmap is empty, only directories are listed.
- If Directory Bitmap is empty, only files are listed.
- If both bitmaps have bits set, both files and directories are listed.
- Offspring structures are returned in no particular order.
- Enumeration stops when:
   - ReqCount items are returned,
   - the reply block is full,
   - or no more offspring exist.
- No partial structures are returned in the reply.
- Each reply includes the input bitmaps, followed by packed offspring structures.
- Variable-length fields (e.g., Long Name, Short Name) are referenced by offsets.
- These fields are packed at the end of each structure.
- Structures are padded to even length.
- If NoErr is returned, all reply structures are valid.
- Any error means no valid offspring structures are present.
- If Offspring Count is requested, the count reflects the user's access rights.
- User must have search access to all ancestors except the target directory.
- For enumerating directories: search access to the directory is required.
- For enumerating files: read access to the directory is required.
- FPOpenVol must be called for the volume before enumeration.
- Directory enumeration may miss or duplicate entries if the directory changes during enumeration.
- Accurate results require enumerating until ObjectNotFound and filtering duplicates.
- Offspring indices are not stable between enumerations.

---

## FPFlush (0x0A)

### Summary

Flush volume-level state: directory, file and fork information.

### Request

|    # | Field     | Type | Size | Description          |
| ---: | :-------- | :--- | :--- | :------------------- |
|    1 | 0         | byte | 1    | Pad byte (must be 0) |
|    2 | Volume ID | int  | 2    | Volume to flush.     |

### Reply

_The reply doesn't carry any payload_

### Result Codes

- **NoErr** – Successful.
- **ParamErr** – Bad session refnum or volume ID..

### Details

- Flushes pending updates for the volume to disk.
- May flush open forks, catalog changes, and volume data structures.
- AFP does not mandate which operations are flushed; implementation-dependent.
- Clients should not rely on specific flush behavior.

---

## FPFlushFork (0x0B)

### Summary

Flush pending updates to a fork.

### Request

|    # | Field       | Type | Size | Description          |
| ---: | :---------- | :--- | :--- | :------------------- |
|    1 | 0           | byte | 1    | Pad byte (must be 0) |
|    2 | OForkRefNum | int  | 2    |                      |

### Reply

_The reply doesn't carry any payload_

### Result Codes

- **NoErr** – Flushed.
- **ParamErr** – Bad OForkRefNum.

### Details

- Ensures that any data buffered for a specific fork is written to disk.
- If the fork was modified, the file’s modification date is updated to the server clock.
- Servers may buffer FPWrite calls for performance; this call forces a flush.
- Clients can use FPFlushFork to guarantee all changes are committed.

---

## FPGetForkParms (0x0E)

### Summary

Retrieve metadata about an open fork (data or resource fork).

### Request

|    # | Field       | Type | Size | Description                                   |
| ---: | :---------- | :--- | :--- | :-------------------------------------------- |
|    1 | 0           | byte | 1    | Pad byte (must be 0)                          |
|    2 | OForkRefNum | int  | 2    | Reference to the fork.                        |
|    2 | Bitmap      | int  | 2    | Sames as File Bitmap in the FPGetFileDirParms |

### Reply

|   #   | Field           | Type | Size | Description                        |
| :---: | --------------- | ---- | ---: | ---------------------------------- |
|   1   | Bitmap          | int  |    2 | Bitmap from the request.           |
|   2   | File parameters | var  |  var | Parameters as indicated by bitmap. |

### Result Codes

- **NoErr** - Success.
- **ParamErr** - OForkRefNum is unknown.
- **BitmapErr** - Incorrect bitmap.

### Details

- Bitmap and parameter returned are the same as File Bitmap in the FPGetFileDirParms.
- Retrieves specified file parameters for the fork referenced by `OForkRefNum`.
- Server packs parameters in bitmap order; variable-length fields are placed at the end.
- Variable-length parameters are referenced by fixed-length offsets (integers) from the start of the parameter block.
- Only the length of the indicated fork can be retrieved; requesting the other fork's length returns `BitmapErr`.
- Fork does not need to be open for read to query parameters.

---

## FPGetSrvrInfo (0x0F)

### Summary

Obtain descriptive information about a server (AFP versions, UAMs, server name, optional volume icon+mask) without first opening an AFP session. Implemented over ASP _GetStatus_.

### Request

At the AFP level, FPGetSrvrInfo has **no parameter bytes** beyond the opcode; the ASP GetStatus request carries the addressing.

### Reply

|    # | Field                           | Type  | Size | Description                                                         |
| ---: | :------------------------------ | :---- | :--- | :------------------------------------------------------------------ |
|    1 | Offset to Machine Type          | int   | 2    | Offset (from start of info block) of Machine Type string.           |
|    2 | Offset to count of AFP Versions | int   | 2    | Offset of AFP Version list count byte.                              |
|    3 | Offset to count of UAMs         | int   | 2    | Offset of UAM list count byte.                                      |
|    4 | Offset to Volume Icon/Mask      | int   | 2    | 0 if no icon/mask supplied; otherwise offset to 256-byte icon+mask. |
|    5 | Flags                           | int   | 2    | See **Flags bits** below.                                           |
|    6 | Server Name                     | str   | var  | Pascal string; begins immediately after `Flags`.                    |
|    7 | Machine Type                    | str   | var  | Pascal string at offset #1.                                         |
|    8 | Count of AFP Versions           |
|    8 | AFP Versions                    | list  | var  | 1-byte count followed by that many Pascal strings.                  |
|    9 | Count of UAMs                   |
|   10 | UAMs                            | list  | var  | 1-byte count followed by that many Pascal strings.                  |
|   11 | Volume Icon and Mask (opt.)     | bytes | 256  | If present; located at offset #4.                                   |

**Flags bits (FPGetSrvrInfo Flags)**

Bit numbering: bit 0 = least significant.

|  Bit | Name             | Meaning                                  |
| ---: | ---------------- | ---------------------------------------- |
|    0 | SupportsCopyFile | 1 if server supports `FPCopyFile`.       |
|    1 | SupportsChgPwd   | 1 if server supports `FPChangePassword`. |
| 2-15 | —                | Reserved, must be 0.                     |

### Result Codes

Returned via ASP _CmdResult_ status.

### Details

- FPGetSrvrInfo returns a server information block with a header of offsets to each field (Machine Type, AFP Versions, UAMs, Volume Icon/Mask).
- Offsets are relative to the start of the block; Volume Icon/Mask offset is 0 if not present.
- AFP Versions and UAMs are lists: 1-byte count followed by packed Pascal strings (no padding).
- Server Name always starts immediately after the Flags field.
- Fields may be packed in any order; clients must use offsets, not assume layout.
- This is the only AFP call allowed before session setup.
- Implemented via ASP GetStatus.

---

## FPGetSrvrParms (0x10)

### Summary

Retrieve server-wide parameters: current server time and a list of all volumes, with per-volume flags.

### Request

At the AFP level the request block has **no parameters** other than the opcode:

- (AFP opcode: `GetSrvrParms`)

### Reply

|    # | Field       | Type | Size | Description                                            |
| ---: | :---------- | :--- | :--- | :----------------------------------------------------- |
|    1 | Server Time | long | 4    | Current server date/time.                              |
|    2 | NumVols     | byte | 1    | Number of volumes managed by the server.               |
|    3 | Volume List | —    | var  | `NumVols` structures, packed back-to-back, no padding. |

Each **volume structure**:

|   #   | Field    | Type | Size | Description                  |
| :---: | -------- | ---- | ---: | ---------------------------- |
|   1   | VolFlags | byte |    1 | Per-volume flags; see below. |
|   2   | VolName  | str  |  var | Pascal string volume name.   |

**VolFlags bits (FPGetSrvrParms per-volume header)**

|  Bit | Name          | Meaning                                            |
| ---: | ------------- | -------------------------------------------------- |
|    0 | HasPassword   | 1 if this volume is password-protected.            |
|    1 | HasConfigInfo | 1 for the Apple II configuration volume (AFP 2.0). |
|  2-7 | —             | Reserved, must be 0.                               |

### Result Codes

- `ParamErr` – Session refnum unknown (from ASP).

### Details

Lists all volumes on the server. In AFP 2.0 one of them may have `HasConfigInfo=1`, indicating it holds Apple II configuration information.

---

## FPGetVolParms (0x11)

### Summary

Retrieve parameters describing a particular volume.

### Request

|    # | Field     | Type | Size | Description                                |
| ---: | :-------- | :--- | :--- | :----------------------------------------- |
|    1 | Pad (0)   | byte | 1    | Must be 0 (reserved).                      |
|    2 | Volume ID | int  | 2    | Identifier from `FPOpenVol`.               |
|    3 | Bitmap    | int  | 2    | Selects which volume parameters to return. |

**Bitmap bits (FPGetVolParms, also used by FPOpenVol)**

Bit 0 is the least-significant bit.

|  Bit | Parameter         | Type | Notes                                                 |
| ---: | ----------------- | ---- | ----------------------------------------------------- |
|    0 | Attributes        | int  | See **Volume Attributes bits** below.                 |
|    1 | Signature         | int  | Volume signature.                                     |
|    2 | Creation Date     | long | Volume creation time.                                 |
|    3 | Modification Date | long | Volume modification time.                             |
|    4 | Backup Date       | long | Last backup time.                                     |
|    5 | Volume ID         | int  | Same as in request; must be requested by `FPOpenVol`. |
|    6 | Bytes Free        | long | Unsigned.                                             |
|    7 | Bytes Total       | long | Unsigned.                                             |
|    8 | Volume Name       | int  | Offset to Pascal string within parameter block.       |

**Volume Attributes bits (bit 0 of bitmap selects this 16-bit field)**

|  Bit | Name     | Meaning                     |
| ---: | -------- | --------------------------- |
|    0 | ReadOnly | Volume is locked read-only. |
| 1-15 | —        | Reserved (must be 0).       |

### Reply

|   #   | Field  | Type | Size | Description                                                                                     |
| :---: | ------ | ---- | ---: | ----------------------------------------------------------------------------------------------- |
|   1   | Bitmap | int  |    2 | Echo of request bitmap.                                                                         |
|   2   | Params | —    |  var | Parameters in bitmap order; variable-length fields are returned via offsets, packed at the end. |

### Result Codes

- `ParamErr` – Unknown session refnum or Volume ID.
- `BitmapErr` – Bitmap null or asks for a parameter not available via this call.

### Details

Returns selected volume information. Variable-length fields (e.g. Volume Name) are referenced by 2-byte offsets measured from the start of the parameter area (not from the bitmap).

---

## FPLogin (0x12)

### Summary

Establish an AFP session with a server and initiate user authentication using a chosen UAM.

### Request

(Transported over ASP `OpenSession`.)

|    # | Field          | Type   | Size | Description                                                                                  |
| ---: | :------------- | :----- | :--- | :------------------------------------------------------------------------------------------- |
|    1 | AFP Version    | string | var  | Pascal string AFP version to use (for example `'AFPVersion 2.0'`).                           |
|    2 | UAM            | string | var  | Pascal string user authentication method (`'Cleartxt Passwrd'`, `'Randnum Exchange'`, etc.). |
|    3 | User Auth Info | bytes  | var  | UAM-specific data; may be null.                                                              |

Example contents of **User Auth Info**:

- For `'Cleartxt Passwrd'`: user name (Pascal string) followed by password (8-byte cleartext, padded with null bytes). If necessary, a null byte is added after the user name so the password begins on an even boundary.

### Reply

|    # | Field          | Type  | Size | Description                                                                       |
| ---: | :------------- | :---- | :--- | :-------------------------------------------------------------------------------- |
|    1 | ID Number      | int   | 2    | For multi-stage UAMs; used by `FPLoginCont` when `AuthContinue` is returned.      |
|    2 | User Auth Info | bytes | var  | UAM-specific data (for example a random challenge); valid only if `AuthContinue`. |

### Result Codes

- `NoServer` – server not responding.
- `BadVersNum` – AFP version not supported.
- `BadUAM` – UAM unknown.
- `ParamErr` – user unknown.
- `UserNotAuth` – UAM failed (bad credentials).
- `AuthContinue` – additional exchanges required; call `FPLoginCont`.
- `ServerGoingDown` – server shutting down.
- `MiscErr` – user already authenticated.

### Details

If `AuthContinue` is returned, the session exists but authentication is incomplete; the client must use `FPLoginCont` (with the returned ID Number and User Auth Info) to finish the UAM-specific handshake. On success, the SRefNum may be used in all subsequent AFP calls for this session.

---

## FPLoginCont (0x13)

### Summary

Continue or complete a multi-exchange UAM started by `FPLogin` (for example `'Randnum Exchange'`).

### Request

|   #   | Field          | Type  | Size | Description                                                               |
| :---: | -------------- | ----- | ---: | ------------------------------------------------------------------------- |
|   1   | Pad (0)        | byte  |    1 | Must be 0 (reserved).                                                     |
|   2   | ID Number      | int   |    2 | ID from previous `FPLogin` or `FPLoginCont` reply.                        |
|   3   | User Auth Info | bytes |  var | UAM-specific continuation data (for example DES-encrypted random number). |

### Reply

|    # | Field          | Type  | Size | Description                              |
| ---: | :------------- | :---- | :--- | :--------------------------------------- |
|    1 | ID Number      | int   | 2    | For further exchanges if `AuthContinue`. |
|    2 | User Auth Info | bytes | var  | UAM-specific data if `AuthContinue`.     |

### Result Codes

- `NoServer` – server not responding.
- `UserNotAuth` – authentication failed; server closes the session.
- `AuthContinue` – more exchanges required.

### Details

On final success (`NoErr`), the SRefNum from the original `FPLogin` is validated; on `UserNotAuth`, the server closes the session and invalidates the SRefNum.

---

## FPLogout (0x14)

### Summary

Terminate an AFP session.

### Request

No parameters:

- (AFP opcode `Logout` only.)

### Reply

_The reply doesn't carry any payload_

### Result Codes

- `ParamErr` – Session refnum unknown.

### Details

Server flushes and closes all forks opened by the session, frees all session resources, and invalidates the SRefNum.

---

## FPMapID (0x15)

### Summary

Map a user or group ID to its textual name.

### Request

|    # | Field       | Type | Size | Description                                                         |
| ---: | :---------- | :--- | :--- | :------------------------------------------------------------------ |
|    1 | Subfunction | byte | 1    | 1 = map **user ID → user name**; 2 = map **group ID → group name**. |
|    2 | ID          | long | 4    | User or group ID to map.                                            |

### Reply

|    # | Field | Type   | Size | Description                       |
| ---: | :---- | :----- | :--- | :-------------------------------- |
|    1 | Name  | string | var  | Pascal string user or group name. |

### Result Codes

- `ParamErr` – Session refnum or subfunction invalid; no ID supplied.
- `ItemNotFound` – ID not recognized.

### Details

A user or group ID of 0 maps to a null string.

---

## FPMapName (0x16)

### Summary

Map a user or group name to its numeric ID.

### Request

|   #   | Field       | Type   | Size | Description                                                         |
| :---: | ----------- | ------ | ---: | ------------------------------------------------------------------- |
|   1   | Subfunction | byte   |    1 | 3 = map **user name → user ID**; 4 = map **group name → group ID**. |
|   2   | Name        | string |  var | Pascal string user or group name to map.                            |

### Reply

|    # | Field | Type | Size | Description              |
| ---: | :---- | :--- | :--- | :----------------------- |
|    1 | ID    | long | 4    | Resulting user/group ID. |

### Result Codes

- `ParamErr` – Session refnum or subfunction invalid.
- `ItemNotFound` – Name not recognized.

### Details

A null user/group name maps to ID 0.

---

## FPMoveAndRename (0x17)

### Summary

Move a file or directory to a new location, optionally renaming it.

### Request

|    # | Field               | Type   | Size | Description                      |
| ---: | :------------------ | :----- | :--- | :------------------------------- |
|    1 | Pad (0)             | byte   | 1    | Must be 0.                       |
|    2 | Volume ID           | int    | 2    | Volume identifier.               |
|    3 | Source Directory ID | long   | 4    | Ancestor of source.              |
|    4 | Dest Directory      | long   | 4    | Ancestor of destination parent.  |
|    5 | Source PathType     | byte   | 1    | 1 = short, 2 = long.             |
|    6 | Source Pathname     | string | var  | Path to object.                  |
|    7 | Dest PathType       | byte   | 1    | 1 = short, 2 = long.             |
|    8 | Dest Pathname       | string | var  | Path to NEW PARENT directory.    |
|    9 | NewType             | byte   | 1    | 1 = short, 2 = long.             |
|   10 | NewName             | string | var  | New name (or null to keep same). |

### Reply

_The reply doesn't carry any payload_

### Result Codes

- **ParamErr** – Session refnum, volume identifier, or pathname type unknown; pathname or NewName invalid.
- **ObjectNotFound** – Input parameters do not point to an existing file or directory.
- **ObjectExists** – A file or directory named `NewName` already exists.
- **CantMove** – Attempt to move a directory into one of its descendant directories.
- **AccessDenied** – Caller lacks required rights.
- **ObjectLocked** – (AFP 2.0) source or destination is marked RenameInhibit.
- **VolLocked** – (AFP 2.0) the volume is ReadOnly.

### Details

- Moves a file or directory (CNode) to a new parent directory, optionally renaming it.
- If `NewName` is null, the original name is retained; otherwise, the name is updated per catalog-node naming rules (short/long).
- The CNode is deleted from its original parent and inserted into the destination parent; Parent ID is updated.
- Modification dates for the CNode, source parent, and destination parent are set to the server clock.
- All other parameters remain unchanged; if the CNode is a directory, all descendants are moved unchanged.
- The destination must be on the same volume; cross-volume moves are not supported.
- Requires prior `FPOpenVol` for the volume.
- Rights:
   - Directory: search access to all ancestors (including source/destination parents), plus write access to both parents.
   - File: search access to all ancestors except source/destination parents, plus read/write to source parent and write to destination parent.

---

## FPOpenVol (0x18)

### Summary

Make a volume available to the workstation and return its Volume ID and selected volume parameters.

### Request

|   #   | Field       | Type   | Size | Description                                                                 |
| :---: | ----------- | ------ | ---: | --------------------------------------------------------------------------- |
|   1   | Pad (0)     | byte   |    1 | Must be 0 (reserved).                                                       |
|   2   | Bitmap      | int    |    2 | Same bitmap as `FPGetVolParms`; **must** include the Volume ID bit (bit 5). |
|   3   | Volume Name | string |  var | Pascal string name of volume as returned by `FPGetSrvrParms`.               |
|   4   | Pad         | byte   |    1 | Optional pad byte to make passwrd begin on even bundary.                    |
|   5   | Password    | bytes  |    8 | Optional password in cleartext; padded with trailing nulls to 8 bytes.      |

> A null byte is inserted between `Volume Name` and `Password` if necessary to make the first byte of `Password` start on an even boundary, as noted in the AFP block diagram.

### Reply

|   #   | Field  | Type | Size | Description                                              |
| :---: | ------ | ---- | ---: | -------------------------------------------------------- |
|   1   | Bitmap | int  |    2 | Echo of request bitmap.                                  |
|   2   | Params | —    |  var | Volume parameters in the same format as `FPGetVolParms`. |

The returned parameter set **must** include Volume ID (since its bit was required in the request bitmap).

### Result Codes

- `ParamErr` – Volume name unknown.
- `BitmapErr` – Bitmap null or contains bits unsupported by this server.
- `AccessDenied` – Password missing or does not match stored password.

### Details

- Must be called before accessing any CNodes on a volume.
- Password (if required) is sent in cleartext, padded to 8 bytes; comparison is case-sensitive.
- Access is denied if the password is missing or incorrect.
- On success, requested volume parameters are returned and the user can access files/directories.
- Bitmap must request Volume ID; null Bitmap is invalid.
- Multiple FPOpenVol calls are allowed; FPCloseVol invalidates the Volume ID.

---

## FPOpenDir (0x19)

### Summary

Open a directory and obtain its Directory ID (essential for variable DirID volumes).

### Request

|    # | Field        | Type   | Size | Description            |
| ---: | :----------- | :----- | :--- | :--------------------- |
|    1 | Pad (0)      | byte   | 1    | Must be 0              |
|    2 | Volume ID    | int    | 2    | Volume identifier.     |
|    3 | Directory ID | long   | 4    | Ancestor Directory ID. |
|    4 | PathType     | byte   | 1    | 1 = short, 2 = long.   |
|    5 | Pathname     | string | var  | Path to directory.     |

### Reply

| #    | Field        | Type | Size | Description                   |
| :--- | :----------- | :--- | :--- | :---------------------------- |
| 1    | Directory ID | long | 4    | The ID of the open directory. |

### Result Codes

- **ParamErr** – Session refnum, volume identifier, or pathname type is unknown; pathname is bad.
- **ObjectNotFound** – Input parameters do not point to an existing directory.
- **AccessDenied** – User does not have the required access rights.
- **ObjectTypeErr** – Input parameters point to a file, not a directory.


### Details

 - If the Volume ID parameter refers to a variable Directory ID volume, the server generates a Directory ID for the specified directory.
 - If the Volume ID parameter refers to a fixed Directory ID type, the server returns the fixed Directory ID for the directory.
 - For fixed Directory ID volumes, it is recommended to use FPGetFileDirParms or FPEnumerate instead of this call.
 - The user must have search access to all ancestor directories, including the parent of the target directory.
 - The user must have previously called FPOpenVol for the specified volume.

---

## FPOpenFork (0x1A)

### Summary

Open the Data or Resource fork of a file for I/O. 

### Request

| #    | Field        | Type   | Size | Description                                  |
| :--- | :----------- | :----- | :--- | :------------------------------------------- |
| 1    | Flag         | byte   | 1    | Most significant bit indicates Rsrc/DataFlag |
| 2    | Volume ID    | int    | 2    | Volume identifier.                           |
| 3    | Directory ID | long   | 4    | Ancestor Directory ID.                       |
| 4    | Bitmap       | int    | 2    | File parameters to return (File Bitmap).     |
| 5    | AccessMode   | int    | 2    | Read/Write and deny-mode bits (see below).   |
| 6    | PathType     | byte   | 1    | 1 = short, 2 = long.                         |
| 7    | Pathname     | string | var  | Path to file.                                |

**AccessMode bits (low byte)**

- Bit 0 (`Read`) – request read access to the fork.
- Bit 1 (`Write`) – request write access to the fork.
- Bit 4 (`DenyRead`) – prevent other sessions from opening the fork for read.
- Bit 5 (`DenyWrite`) – prevent other sessions from opening the fork for write.

Bits 4–15 are reserved and must be zero.

### Reply

| #    | Field           | Type | Size | Description                     |
| :--- | :-------------- | :--- | :--- | :------------------------------ |
| 1    | Bitmap          | int  | 2    | Echo of request bitmap.         |
| 2    | OForkRefNum     | int  | 2    | Open Fork Reference Number.     |
| 3    | File Parameters | var  | var  | Parameters requested in Bitmap. |

### Result Codes

- **ParamErr** – Invalid session, volume, or pathname.
- **ObjectNotFound** – File not found.
- **BitmapErr** – Requested parameter not available.
- **DenyConflict** – Deny mode prevents opening fork.
- **AccessDenied** – Insufficient rights or file/volume is read-only.
- **ObjectLocked** – File is write-inhibited.
- **VolLocked** – Volume is read-only.
- **ObjectTypeErr** – Target is a directory.
- **TooManyFilesOpen** – Fork limit reached.

### Details

- Server opens the fork if access rights and requested mode allow.
- On success, reply includes requested file parameters (bitmap order), Bitmap, and new OForkRefNum.
- On DenyConflict, OForkRefNum is 0 but parameters are still returned.
- BitmapErr if request asks for the other fork's length.
- Variable-length fields (Long/Short Name) are packed at the end, referenced by offsets.
- If fork is opened and attributes requested, DAlreadyOpen or RAlreadyOpen bit is set.
- Rights:
   - Read/none: search ancestors except parent, read access to parent.
   - Write: volume not read-only; if both forks empty, search or write ancestors except parent, write to parent; if either fork not empty and write requested, search ancestors except parent, read/write to parent.
- FPOpenVol must be called first; each fork gets a unique OForkRefNum.

---

## FPRead (0x1B)

### Summary

Read data from an open fork.

### Request

|    # | Field        | Type | Size | Description                       |
| ---: | :----------- | :--- | :--- | :-------------------------------- |
|    1 | Pad (0)      | byte | 1    | Must be 0                         |
|    2 | OForkRefNum  | int  | 2    | Fork reference.                   |
|    3 | Offset       | long | 4    | Byte offset to begin reading.     |
|    4 | ReqCount     | long | 4    | Number of bytes requested.        |
|    5 | Newline Mask | byte | 1    | Mask for newline character check. |
|    6 | Newline Char | byte | 1    | Character to terminate read on.   |

### Reply

|    # | Field | Type  | Size | Description                             |
| ---: | :---- | :---- | :--- | :-------------------------------------- |
|    1 | Data  | bytes | var  | The data read (carried in ASP payload). |

### Result Codes

- **ParamErr** – Invalid session, fork reference, or parameters.
- **AccessDenied** – No read access to fork.
- **EOFErr** – End of fork reached.
- **LockErr** – Requested range is locked.

### Details

- Reads bytes from an open fork, starting at the specified Offset.
- Reading stops when one of the following occurs:
   - The requested number of bytes (ReqCount) is read.
   - End of fork (EOF) is reached.
   - A locked range (by another user) is encountered.
   - A byte matches Newline Char after applying Newline Mask (each byte read is ANDed with Newline Mask; if the result equals Newline Char, reading stops).
- If EOF or a locked range is encountered, the reply includes all data read up to that point and returns EOFErr or LockErr.
- Any Newline Mask value is allowed; setting it to $00 disables newline checking.
- Reading unwritten bytes yields undefined results.
- The fork must be open for read by the requesting user.
- Lock the range before reading to avoid partial reads due to concurrent locks.
- The transport may split the request into multiple reads; ActCount is managed by the transport and not returned in the reply.

---

## FPRename (0x1C)

### Summary

Rename a file or directory (without moving it).

### Request

| #    | Field        | Type   | Size | Description            |
| :--- | :----------- | :----- | :--- | :--------------------- |
| 1    | Pad (0)      | byte   | 1    | Must be 0.             |
| 2    | Volume ID    | int    | 2    | Volume identifier.     |
| 3    | Directory ID | long   | 4    | Ancestor Directory ID. |
| 4    | PathType     | byte   | 1    | 1 = short, 2 = long.   |
| 5    | Pathname     | string | var  | Path to object.        |
| 6    | NewType      | byte   | 1    | 1 = short, 2 = long.   |
| 7    | NewName      | string | var  | New name.              |

### Reply

_The reply doesn't carry any payload_

### Result Codes

- **ParamErr** – Session refnum, volume identifier, or pathname type is unknown; pathname or NewName is bad.
- **ObjectNotFound** – Input parameters do not point to an existing file or directory.
- **ObjectExists** – A file or directory with the name NewName already exists.
- **AccessDenied** – User does not have the required rights.
- **VolLocked** – The volume is ReadOnly.
- **ObjectLocked** – The file or directory is marked RenameInhibit.
- **CantRename** – Attempt to rename a volume or root directory.

### Details

- The server assigns a new name to the file or directory.
- The alternate name (long or short) is generated according to catalog node naming rules.
- The modification date of the parent directory is updated to the server’s clock.
- Rights required:
   - To rename a directory: search access to all ancestors (including parent), and write access to the parent directory.
   - To rename a file: search access to all ancestors except the parent, and read & write access to the parent directory.
- The user must have previously called `FPOpenVol` for the volume.

---

## FPSetDirParms (0x1D)

### Summary

Set parameters for a directory (attributes, access rights, owner/group ID, dates, Finder info, ProDOS info, etc.). Uses the **Directory Bitmap** format from FPGetFileDirParms.

### Request

|    # | Field                | Type   | Size | Description                                                            |
| ---: | :------------------- | :----- | :--- | :--------------------------------------------------------------------- |
|    1 | Pad (0)              | byte   | 1    | Must be 0 (reserved).                                                  |
|    2 | Volume ID            | int    | 2    | Volume identifier.                                                     |
|    3 | Directory ID         | long   | 4    | Directory identifier.                                                  |
|    4 | Bitmap               | int    | 2    | Selects which parameters to set.                                       |
|    5 | PathType             | byte   | 1    | 1 = short names, 2 = long names.                                       |
|    6 | Pathname             | string | var  | Path to directory.                                                     |
|    7 | Pad (0)              | byte   | 0-1  | A null byte is added if necessary to align Params on an even boundary. |
|    8 | Directory Parameters | var    | var  | Parameters packed in bitmap order; variable length fields use offsets. |

### Reply

_The reply doesn't carry any payload_

### Result Codes

- **ParamErr** – Bad refs or pathname; owner/group ID invalid.
- **ObjectNotFound** – Directory not found.
- **BitmapErr** – Bitmap null or invalid.
- **AccessDenied** – Insufficient rights (requires Owner for permissions; Write for others).
- **VolLocked** – Volume is ReadOnly.
- **ObjectTypeErr** – Path points to a file.

### Details

- Sets directory parameters in bitmap order; variable-length fields use offsets and are packed at the end.
- Insert a null byte after Pathname if needed for even alignment.
- Changing access controls, dates (except modification), Finder Info, ProDOS Info, or attributes updates the directory's modification date.
- Changing access controls, owner/group ID, or Invisible also updates the parent directory's modification date.
- Access rights changes apply immediately to all sessions.
- If any required rights are missing, the call fails with AccessDenied and no changes occur.
- Rights required:
   - For access rights, owner/group ID, or DeleteInhibit/RenameInhibit/WriteInhibit/Invisible: search or write to all ancestors (including parent), and must be owner.
   - For other parameters (empty directory): search or write to ancestors except parent, plus write to parent.
   - For other parameters (non-empty directory): search to all ancestors including parent, plus write to parent.
- Must call FPOpenVol for the volume first.
- Cannot set directory name, parent Directory ID, Directory ID, or Offspring Count (use FPRename/FPMoveAndRename).
- Variable-length fields use offsets; bit 15 of Attributes (Set/Clear) sets (1) or clears (0) attributes.

---

## FPSetFileParms (0x1E)

### Summary

Set parameters for a file (attributes, dates, etc.).

### Request

|    # | Field           | Type   | Size | Description                                                            |
| ---: | :-------------- | :----- | :--- | :--------------------------------------------------------------------- |
|    1 | Pad (0)         | byte   | 1    | Must be 0                                                              |
|    2 | Volume ID       | int    | 2    | Volume identifier.                                                     |
|    3 | Directory ID    | long   | 4    | Ancestor directory ID.                                                 |
|    4 | Bitmap          | int    | 2    | Selects parameters to set (File Bitmap format).                        |
|    5 | PathType        | byte   | 1    | 1 = short, 2 = long.                                                   |
|    6 | Pathname        | string | var  | Path to file.                                                          |
|    7 | Pad (0)         | byte   | 0-1  | A null byte is added if necessary to align Params on an even boundary. |
|    8 | File Parameters | var    | var  | Parameters packed in bitmap order.                                     |

### Reply

_The reply doesn't carry any payload_

### Result Codes

- **ParamErr** – Bad refs.
- **ObjectNotFound** – File not found.
- **AccessDenied** – Insufficient rights (Search/Write to parent usually required).
- **VolLocked** – Volume ReadOnly.
- **BitmapErr** – Bitmap null or invalid.
- **ObjectTypeErr** – Path points to a directory.

### Details

- Sets file parameters in bitmap order; variable-length fields use offsets and are packed at the end.
- Variable-length fields are referenced by offsets from the start of the parameter area.
- Insert a null byte after Pathname if needed for even alignment.
- Can set/clear: Attributes (except DAlreadyOpen, RAlreadyOpen, CopyProtect), Creation/Modification/Backup Date, Finder Info, ProDOS Info.
- Set/Clear bit applies to all specified attributes; cannot mix set/clear in one call.
- Changing Invisible updates parent directory's modification date.
- Changing attributes, dates (except modification), Finder Info, or ProDOS Info updates file's modification date.
- Rights: if both forks empty, search or write to ancestors except parent, plus write to parent; if either fork not empty, search to ancestors except parent, plus read & write to parent.
- Must call FPOpenVol for the volume first.
- Cannot set file name, parent Directory ID, file number, or fork lengths.

---

## FPSetForkParms (0x1F)

### Summary

Set the fork length for a specified open fork.

### Request

|    # | Field       | Type | Size | Description                                        |
| ---: | :---------- | :--- | :--- | :------------------------------------------------- |
|    1 | Pad (0)     | byte | 1    | Must be 0.                                         |
|    2 | OForkRefNum | int  | 2    | RefNum of fork.                                    |
|    3 | Bitmap      | int  | 2    | Same as File Bitmap in the FPGetFileDirParms call. |
|    4 | Fork Length | long | 4    | New end-of-fork.                                   |

### Reply

_The reply doesn't carry any payload_

### Result Codes

- **ParamErr** – Session refnum or open fork refnum is unknown.
- **BitmapErr** – Attempted to set a parameter not allowed by this call, or bitmap is null.
- **DiskFull** – No more space available on the volume.
- **LockErr** – Locked range conflict exists.
- **AccessDenied** – User lacks required rights; in AFP 1.1, volume is ReadOnly.
- **VolLocked** – In AFP 2.0, volume is ReadOnly.

### Details

- The server receives the Bitmap and Fork Length, and updates the length of the fork identified by `OForkRefNum`.
- If the Bitmap requests to set the length of the file’s other fork, or any other file parameter, the server returns `BitmapErr`.
- If truncating the fork would remove a region locked by another user, the server returns `LockErr`.
- The fork must be open for write access by the user.
- This call cannot set a file’s name (use `FPRename`), parent directory (use `FPMoveAndRename`), or file number.

---

## FPSetVolParms (0x20)

### Summary

Set volume parameters (only Backup Date is modifiable).

### Request

|    # | Field       | Type | Size | Description                      |
| ---: | :---------- | :--- | :--- | :------------------------------- |
|    1 | Pad (0)     | byte | 1    | Must be 0                        |
|    2 | Volume ID   | int  | 2    | Volume identifier.               |
|    3 | Bitmap      | int  | 2    | Must select Backup Date (bit 4). |
|    4 | Backup Date | long | 4    | New backup date.                 |

### Reply

_The reply doesn't carry any payload_

### Result Codes

- **ParamErr** – Session refnum or volume identifier is unknown.
- **BitmapErr** – Attempted to set a parameter not allowed by this call, or bitmap is null.
- **VolLocked** – The volume is ReadOnly.

### Details

- Only the Backup Date bit may be set in the bitmap field (same format as FPGetVolParms).
- The server updates the backup date for the specified volume.
- Requires a prior FPOpenVol call for the volume.

---

## FPWrite (0x21)

### Summary

Write data to an open fork.

### Request

|    # | Field         | Type | Size | Description                                |
| ---: | :------------ | :--- | :--- | :----------------------------------------- |
|    1 | Start/EndFlag | bit  | 1    | Bit 7 of the opcode/flag byte (see notes). |
|    2 | OForkRefNum   | int  | 2    | Fork reference.                            |
|    3 | Offset        | long | 4    | Offset relative to Start (0) or End (1).   |
|    4 | ReqCount      | long | 4    | Number of bytes to write.                  |

_Note: The Start/EndFlag indicates if Offset is relative to the beginning (0) or end (1) of the file._

### Reply

|    # | Field       | Type | Size | Description                                      |
| ---: | :---------- | :--- | :--- | :----------------------------------------------- |
|    1 | LastWritten | long | 4    | The byte number just past the last written byte. |

### Result Codes

- **ParamErr** – Session refnum or open fork refnum is unknown.
- **AccessDenied** – User does not have the required access rights.
- **LockErr** – Some or all of the requested range is locked by another user.
- **DiskFull** – No more space exists on the volume.

### Details

- Writes data provided in the ASP write payload to the open fork.
- Supports writing at an offset relative to either the start or end of the fork (using Start/EndFlag).
- Extends the fork if writing past the current end-of-file.
- Returns the byte number just past the last byte written.
- If any part of the target range is locked by another user, no data is written and LockErr is returned.
- The fork's modification date is not updated until the fork is closed.
- The fork must be open for write access by the user.
- It is recommended to lock the range before writing to avoid partial writes due to concurrent locks.
- The transport may split large writes into multiple ASP requests; data is streamed in ASP packets.

---

## FPGetFileDirParms (0x22)

### Summary

Retrieve parameters for a CNode (file or directory).

### Request
|   \# | Field            | Type   | Size | Description                                    |
| ---: | :--------------- | :----- | :--- | :--------------------------------------------- |
|    1 | Pad (0)          | byte   | 1    | Must be 0\.                                    |
|    2 | Volume ID        | int    | 2    | Volume identifier.                             |
|    3 | Directory ID     | long   | 4    | Ancestor directory ID.                         |
|    4 | File Bitmap      | int    | 2    | Parameters to return if object is a file.      |
|    5 | Directory Bitmap | int    | 2    | Parameters to return if object is a directory. |
|    6 | PathType         | byte   | 1    | 1 \= short, 2 \= long.                         |
|    7 | Pathname         | string | var  | Path to object.                                |

#### File Bitmap bits

Bit 0 is least significant. Types are the underlying parameter types.

|    Bit | Parameter            | Type     | Notes                               |
| -----: | -------------------- | -------- | ----------------------------------- |
|      0 | Attributes           | int      | See **File Attributes bits** below. |
|      1 | Parent Directory ID  | long     | Parent directory ID.                |
|      2 | Creation Date        | long     | File creation time.                 |
|      3 | Modification Date    | long     | File modification time.             |
|      4 | Backup Date          | long     | Last backup time.                   |
|      5 | Finder Info          | 32 bytes | Finder info record.                 |
|      6 | Long Name            | int      | Offset to Pascal string.            |
|      7 | Short Name           | int      | Offset to Pascal string.            |
|      8 | File Number          | long     | Unique file ID.                     |
|      9 | Data Fork Length     | long     | Length of data fork.                |
|     10 | Resource Fork Length | long     | Length of resource fork.            |
|     13 | ProDOS Info          | 6 bytes  | Apple II ProDOS info.               |
| others | —                    | —        | Reserved.                           |

**File Attributes bits**

|  Bit | Name          | Meaning                     |
| ---: | ------------- | --------------------------- |
|    0 | Invisible     | Finder “invisible” flag.    |
|    1 | MultiUser     | Multiuser bit.              |
|    2 | System        | System file.                |
|    3 | DAlreadyOpen  | Data fork already open.     |
|    4 | RAlreadyOpen  | Resource fork already open. |
|    5 | WriteInhibit  | Read-only file.             |
|    6 | BackupNeeded  | Needs backup.               |
|    7 | RenameInhibit | Rename prohibited.          |
|    8 | DeleteInhibit | Delete prohibited.          |
|   10 | CopyProtect   | Copy-protected.             |
|   15 | Set/Clear     | Used only in Set calls.     |

#### Directory Bitmap bits

Bit 0 is least significant. Types are the underlying parameter types.

|    Bit | Parameter           | Type     | Notes                                        |
| -----: | ------------------- | -------- | -------------------------------------------- |
|      0 | Attributes          | int      | See **Directory Attributes bits** below.     |
|      1 | Parent Directory ID | long     | Parent directory ID.                         |
|      2 | Creation Date       | long     | Directory creation time.                     |
|      3 | Modification Date   | long     | Directory modification time.                 |
|      4 | Backup Date         | long     | Last backup time.                            |
|      5 | Finder Info         | 32 bytes | Finder info record.                          |
|      6 | Long Name           | int      | Offset to Pascal string.                     |
|      7 | Short Name          | int      | Offset to Pascal string.                     |
|      8 | Directory ID        | long     | Directory ID.                                |
|      9 | Offspring Count     | int      | Number of offspring (files and directories). |
|     10 | Owner ID            | long     | Owner user ID.                               |
|     11 | Group ID            | long     | Group ID.                                    |
|     12 | Access Rights       | long     | Packed owner/group/world rights and summary. |
|     13 | ProDOS Info         | 6 bytes  | Apple II ProDOS info.                        |
| others | —                   | —        | Reserved.                                    |

**Directory Attributes bits**

|  Bit | Name          | Meaning                  |
| ---: | ------------- | ------------------------ |
|    0 | Invisible     | Finder “invisible” flag. |
|    2 | System        | System file.             |
|    6 | BackupNeeded  | Needs backup.            |
|    7 | RenameInhibit | Rename prohibited.       |
|    8 | DeleteInhibit | Delete prohibited.       |


### Reply

|   \# | Field            | Type | Size | Description                                |
| ---: | :--------------- | :--- | :--- | :----------------------------------------- |
|    1 | File Bitmap      | int  | 2    | Echo of request.                           |
|    2 | Directory Bitmap | int  | 2    | Echo of request.                           |
|    3 | File/DirFlag     | byte | 1    | 0 = File, 1 = Directory.                   |
|    4 | Pad (0)          | byte | 1    | Padding.                                   |
|    5 | Parameters       | var  | var  | Packed parameters for the specific object. |

### Result Codes

- **ParamErr** – Invalid session, volume, pathname type, or pathname.
- **ObjectNotFound** – File or directory not found.
- **BitmapErr** – Requested parameter not available via this call.
- **AccessDenied** – Insufficient access rights.

### Details

- Returns parameters for a file or directory, as found.
- Variable-length fields (names) are returned as offsets from the start of the Parameters block.
- Packs parameters in bitmap order in the reply.
- Includes File/DirFlag to indicate object type.
- Echoes input bitmaps before parameters.
- Variable-length fields are packed at the end; offsets are used in bitmap order.
- If both bitmaps are null and the CNode exists, only bitmaps and File/DirFlag are returned.
- Directory access rights are returned as a 4-byte long, showing owner/group/world privileges.
- The upper byte of Access Rights is a summary; its high bit shows ownership.
- Offspring Count reflects accessible children, adjusted for user rights.
- Requires search access to all ancestors except the parent directory.
- For directories, search access to parent; for files, read access to parent.
- FPOpenVol must be called first for the volume.
- Most attributes are stored as flags in Finder Info.

---

## FPSetFileDirParms (0x23)

### Summary

Set parameters common to both files and directories.

### Request

|    # | Field        | Type   | Size | Description                                                            |
| ---: | :----------- | :----- | :--- | :--------------------------------------------------------------------- |
|    1 | Pad (0)      | byte   | 1    | Must be 0                                                              |
|    2 | Volume ID    | int    | 2    | Volume identifier.                                                     |
|    3 | Directory ID | long   | 4    | Ancestor directory ID.                                                 |
|    4 | Bitmap       | int    | 2    | Selects parameters to set (only common parameters allowed).            |
|    5 | PathType     | byte   | 1    | 1 = short, 2 = long.                                                   |
|    6 | Pathname     | string | var  | Path to file or directory.                                             |
|    7 | Pad (0)      | byte   | 0-1  | A null byte is added if necessary to align Params on an even boundary. |
|    8 | Parameters   | var    | var  | Parameters packed in bitmap order.                                     |

The Bitmap is the same as File or Directory Bitmap of the FPGetFileDirParms call (only parameters common to both bitmaps may be set by this call)

### Reply

_The reply doesn't carry any payload_

### Result Codes

- **ParamErr** – Invalid session, volume, pathname type, or pathname.
- **ObjectNotFound** – Input parameters do not refer to an existing file or directory.
- **AccessDenied** – User lacks required access rights.
- **VolLocked** – Vvolume is read-only.
- **BitmapErr** – Bitmap is null or requests a parameter not settable via this call.

### Details

- Sets fields common to files and directories without needing to know the object type.
- Can set/clear: Invisible, System attributes, Creation/Modification/Backup Date, Finder Info, ProDOS Info.
- Parameters are packed in bitmap order; variable-length fields use offsets from the start of the parameter area.
- Add a null byte after Pathname if needed for even alignment.
- If Attributes are set, the Set/Clear bit applies to all specified attributes; cannot mix set/clear in one call.
- Changing Invisible updates the parent directory's modification date.
- Changing attributes, dates (except modification), Finder Info, or ProDOS Info updates the object's modification date.
- Rights required:
   - Non-empty directory: search to all ancestors incl. parent, plus write to parent.
   - Empty directory: search or write to ancestors except parent, plus write to parent.
   - Non-empty file: search to ancestors except parent, plus read & write to parent.
   - Empty file: search or write to ancestors except parent, plus write to parent.
- Must call FPOpenVol for the volume first.
- If object type is known, use FPSetFileParms or FPSetDirParms for more specific changes.
- Use FPSetDirParms for directory Access Rights, Owner ID, or Group ID.
- Use FPSetFileParms for file attributes other than Invisible and System.

---

## FPChangePassword (0x24)

### Summary

Allow a logged-in user to change their password. New in AFP 2.0 and optional (may not be supported).

Inputs and block layout depend on the UAM:

### Common Input

- UAM (string) – Pascal string naming the UAM to use (`'Cleartxt Passwrd'` or `'Randnum Exchange'`).

### Request – `'Cleartxt Passwrd'` UAM

|    # | Field        | Type   | Size | Description                                                      |
| ---: | :----------- | :----- | :--- | :--------------------------------------------------------------- |
|    1 | Pad (0)      | byte   | 1    | Reserved.                                                        |
|    2 | UAM          | string | var  | `'Cleartxt Passwrd'`.                                            |
|    3 | Pad (0)      | byte   | 1    | Added if necessary so `User Name` begins on an even boundary.    |
|    4 | User Name    | string | var  | Pascal string.                                                   |
|    5 | Pad (0)      | byte   | 1    | Added if necessary so `Old Password` begins on an even boundary. |
|    6 | Old Password | bytes  | 8    | Old password in cleartext, padded with nulls to 8 bytes.         |
|    7 | New Password | bytes  | 8    | New password in cleartext, padded with nulls to 8 bytes.         |

### Request – `'Randnum Exchange'` UAM

Same general layout, but the passwords are encrypted:

|    # | Field                             | Type  | Size | Description                                                      |
| ---: | :-------------------------------- | :---- | :--- | :--------------------------------------------------------------- |
|    1 | Pad (0)                           | byte  | 1    | Reserved.                                                        |
|    2 | UAM                               | str   | var  | `'Randnum Exchange'`.                                            |
|    3 | Pad (0)                           | byte  | 1    | Added if necessary so `User Name` begins on an even boundary.    |
|    4 | User Name                         | str   | var  | Pascal string.                                                   |
|    5 | Pad (0)                           | byte  | 1    | Added if necessary so `Old Password` begins on an even boundary. |
|    6 | Old Password (encrypted with new) | bytes | 8    | 8-byte block.                                                    |
|    7 | New Password (encrypted with old) | bytes | 8    | 8-byte block.                                                    |

> In both variants, a null byte is inserted after the user name, if needed, so the first byte of the old password begins on an even boundary.

### Reply

_The reply doesn't carry any payload_

### Result Codes

- `UserNotAuth` – UAM failed (old password wrong) or no user logged in.
- `BadUAM` – UAM not supported for FPChangePassword.
- `CallNotSupported` – Server doesn’t support this call.
- `AccessDenied` – Call disabled administratively for this user.
- `ParamErr` – User name null or >31 chars or not found.

### Details

- Describes password change mechanisms for two User Authentication Modules (UAMs): 'Cleartxt Passwrd' and 'Randnum Exchange'
- For 'Cleartxt Passwrd', old and new passwords are sent in cleartext; the server verifies the old password and updates to the new password if matched
- For 'Randnum Exchange', old password is encrypted with the new password and new password is encrypted with the old password, both using DES; the server decrypts and verifies before updating
- Passwords shorter than 8 bytes are padded with null bytes
- The server may not support password change or the user may not have permission; granting password change rights is an administrative decision outside the protocol

---

## FPGetUserInfo (0x25)

### Summary

Retrieve information about a user (User ID and Primary Group ID). New in AFP 2.0.

### Request

|    # | Field    | Type | Size | Description                                                              |
| ---: | :------- | :--- | :--- | :----------------------------------------------------------------------- |
|    1 | ThisUser | byte | 1    | Bit-field; see below, **must** indicate “this user”.                     |
|    2 | User ID  | long | 4    | User ID to query; ignored if `ThisUser` indicates “this session’s user”. |
|    3 | Bitmap   | int  | 2    | Selects which user info parameters to return.                            |

**ThisUser bits (request byte)**

|  Bit | Meaning                                                                                         |
| ---: | ----------------------------------------------------------------------------------------------- |
|    0 | ThisUser – when 1, ignore `User ID` and return info for the user who is client of this session. |
|  1-7 | Reserved (must be 0).                                                                           |

**Bitmap bits (FPGetUserInfo)**

|  Bit | Parameter        | Type | Description           |
| ---: | ---------------- | ---- | --------------------- |
|    0 | User ID          | long | User’s ID.            |
|    1 | Primary Group ID | long | User’s primary group. |
| 2-15 | —                | —    | Reserved (must be 0). |

### Reply

|   #   | Field  | Type | Size | Description                                   |
| :---: | ------ | ---- | ---: | --------------------------------------------- |
|   1   | Bitmap | int  |    2 | Echo of request bitmap.                       |
|   2   | Params | —    |  var | Requested parameters, packed in bitmap order. |

### Result Codes

- `ParamErr` – `ThisUser` bit not set as required.
- `ItemNotFound` – User ID unknown.
- `BitmapErr` – Bitmap asks for unsupported parameter.
- `AccessDenied` – Caller not permitted to retrieve this user’s info.
- `CallNotSupported` – AFP 1.1 server (no FPGetUserInfo).

### Details

- This call is effectively limited to querying the user who is client of the current session (`ThisUser` set).
- User ID in the request is reserved for future expansion.

---

## FPOpenDT (0x30)

### Summary

Open the Desktop Database for a volume.

### Request

|    # | Field     | Type | Size | Description        |
| ---: | :-------- | :--- | :--- | :----------------- |
|    1 | Pad (0)   | byte | 1    | Must be 0          |
|    2 | Volume ID | int  | 2    | Volume identifier. |

### Reply

|    # | Field    | Type | Size | Description                 |
| ---: | :------- | :--- | :--- | :-------------------------- |
|    1 | DTRefNum | int  | 2    | Desktop Database Reference. |

### Result Codes

- **ParamErr** – Invalid session of volume.

### Details

- The server opens the Desktop database for the specified volume.
- Returns a unique Desktop Database Reference Number (DTRefNum) for this session.
- The DTRefNum must be used in all subsequent Desktop database calls for this volume.

---

## FPCloseDT (0x31)

### Summary

Close the Desktop Database.

### Request

| #    | Field    | Type | Size | Description                 |
| :--- | :------- | :--- | :--- | :-------------------------- |
| 1    | Pad (0)  | byte | 1    | Must be 0                   |
| 2    | DTRefNum | int  | 2    | Desktop Database Reference. |

### Reply

_The reply doesn't carry any payload_

### Result Codes

- **ParamErr** – Invalid database reference.

### Details

- Database must first be opened with a successful FPOpenDT call.

---

## FPGetIcon (0x33)

### Summary

Retrieve an icon bitmap from Desktop DB for a given (FileCreator, FileType, IconType) triple.

### Request

|    # | Field       | Type    | Size |                                           |
| ---: | :---------- | :------ | :--- | :---------------------------------------- |
|    1 | Pad 0       | byte    | 1    | 0 pad byte                                |
|    2 | DTRefNum    | int     | 2    |                                           |
|    3 | FileCreator | ResType | 4    |                                           |
|    4 | FileType    | ResType | 4    |                                           |
|    5 | IconType    | byte    | 1    |                                           |
|    6 | Pad 0       | byte    | 1    | 0 pad byte                                |
|    7 | Length      | int     | 2    | Max bytes client can accept (0 to probe). |

### Reply

|    # | Field       | Type  | Size    | Description               |
| ---: | :---------- | :---- | :------ | :------------------------ |
|    1 | Icon Bitmap | bytes | ≤Length | Icon bitmap data.         |

### Result Codes

- **ParamErr** – bad refs.
- **ItemNotFound** – no matching icon.

### Details

- If Length is 0, reply just indicates presence/absence via error code.
- If Length < actual size, server returns only Length bytes (no error).
- Requires FPOpenDT.

---

## FPGetIconInfo (0x34)

### Summary

Enumerate icons for a FileCreator, returning type, size and tag for each icon by index.

### Request

|   #   | Field       | Type    | Size |                                |
| :---: | ----------- | ------- | ---: | ------------------------------ |
|   1   | Pad 0       | byte    |    1 | 0 pad byte                     |
|   2   | DTRefNum    | int     |    2 |                                |
|   3   | FileCreator | ResType |    4 |                                |
|   4   | IconIndex   | int     |    2 | 1-based index per FileCreator. |

### Reply

|    # | Field    | Type    | Size |                       |
| ---: | :------- | :------ | :--- | :-------------------- |
|    1 | IconTag  | long    | 4    |                       |
|    2 | FileType | ResType | 4    |                       |
|    3 | IconType | byte    | 1    |                       |
|    4 | Pad 0    | byte    | 1    |                       |
|    5 | Size     | int     | 2    | Bitmap size in bytes. |

### Result Codes

- **ParamErr** – bad refs.
- **ItemNotFound** – index beyond icon list.

### Details

- Client iterates IconIndex from 1 upwards until ItemNotFound.
- Requires FPOpenDT.

---

## FPAddAPPL (0x35)

### Summary

Add (or replace) an APPL mapping in a volume’s Desktop database for a given application and file creator.

### Request

|    # | Field        | Type    | Size | Description                              |
| ---: | :----------- | :------ | :--- | :--------------------------------------- |
|    1 | Pad 0        | byte    | 1    | 0 pad byte                               |
|    2 | DTRefNum     | int     | 2    | Desktop database refnum (from FPOpenDT). |
|    3 | Directory ID | long    | 4    | Ancestor directory of the application.   |
|    4 | FileCreator  | ResType | 4    | 4-byte creator code for the application. |
|    5 | APPL Tag     | long    | 4    | Opaque tag stored with the mapping.      |
|    6 | PathType     | byte    | 1    | 1 = short names, 2 = long names.         |
|    7 | Pathname     | string  | var  | Pathname to the application.             |

### Reply

_The reply doesn't carry any payload_

### Result Codes

- **ParamErr** – bad session/DT refnum or pathname.
- **ObjectNotFound** – application path does not name a file.
- **AccessDenied** – rights insufficient.
- **ObjectTypeErr** – path names a directory.

### Details

- Adds/updates the APPL mapping for (Directory ID, filename, FileCreator). If a mapping already exists for that triple, it is replaced.
- Multiple applications can share the same FileCreator; APPL Tag distinguishes them.
- User must have search or write rights to all ancestors except the parent directory, and write rights to the application’s parent directory.
- Requires a successful FPOpenDT for the volume and the application must already exist.

---

## FPRemoveAPPL (0x36)

### Summary

Remove an APPL mapping from a volume's Desktop database.

### Request

|    # | Field        | Type    | Size | Description                              |
| ---: | :----------- | :------ | :--- | :--------------------------------------- |
|    1 | Pad 0        | byte    | 1    | 0 pad byte                               |
|    2 | DTRefNum     | int     | 2    | Desktop database refnum (from FPOpenDT). |
|    3 | Directory ID | long    | 4    | Ancestor directory of the application.   |
|    4 | FileCreator  | ResType | 4    | 4-byte creator code for the application. |
|    5 | PathType     | byte    | 1    | 1 = short names, 2 = long names.         |
|    6 | Pathname     | string  | var  | Pathname to the application.             |

### Reply

_The reply doesn't carry any payload_

### Result Codes

- **ParamErr** – bad refs.
- **ObjectNotFound** – file path invalid.
- **AccessDenied** – insufficient rights.
- **ItemNotFound** – no APPL mapping for that application/creator.

### Details

- Removes only the mapping, not the application file.
- Rights: search to all ancestors except parent, plus read & write to parent.
- Requires FPOpenDT.

---

## FPGetAPPL (0x37)

### Summary

Look up APPL mappings by FileCreator and index, returning desktop tag plus the application’s file parameters.

### Request

|   #   | Field       | Type    | Size |                                                        |
| :---: | ----------- | ------- | ---: | ------------------------------------------------------ |
|   1   | Pad 0       | byte    |    1 | 0 pad byte                                             |
|   2   | DTRefNum    | int     |    2 |                                                        |
|   3   | FileCreator | ResType |    4 |                                                        |
|   4   | APPL Index  | int     |    2 | 1-based index; 0 returns first mapping.                |
|   5   | Bitmap      | int     |    2 | File bitmap selecting which file parameters to return. |

### Reply

|    # | Field       | Type | Size | Description                         |
| ---: | :---------- | :--- | :--- | :---------------------------------- |
|    1 | Bitmap      | int  | 2    | Echo of input bitmap.               |
|    2 | APPL Tag    | long | 4    | Tag stored with mapping.            |
|    3 | File params | var  | var  | File parameters selected by bitmap. |

### Result Codes

- **ParamErr** – bad refs.
- **ItemNotFound** – no mapping for index or creator.
- **BitmapErr** – invalid bitmap.

### Details

- APPL list is per-FileCreator; client iterates indices from 1 until ItemNotFound.
- Rights: search to all ancestors except parent, plus read to parent of application file.
- Requires FPOpenDT and that application still exists.

---

## FPAddComment (0x38)

### Summary

Attach a comment string (Finder comment) to a file or directory entry in the volume’s Desktop database.

### Request

|    # | Field        | Type   | Size | Description                                                   |
| ---: | :----------- | :----- | :--- | :------------------------------------------------------------ |
|    1 | Pad 0        | byte   | 1    | 0 pad byte                                                    |
|    2 | DTRefNum     | int    | 2    | Desktop database refnum.                                      |
|    3 | Directory ID | long   | 4    | Directory containing the target object.                       |
|    4 | PathType     | byte   | 1    | 1 = short, 2 = long names.                                    |
|    5 | Pathname     | string | var  | Path to file or directory.                                    |
|    6 | Pad 0        | byte   | 0-1  | Added if necessary to make comment begin on an even boundary. |
|    7 | Comment      | string | var  | Comment text (truncated to 199 bytes if longer).              |

### Reply

_The reply doesn't carry any payload_

### Result Codes

- **ParamErr** – bad refs or pathname.
- **ObjectNotFound** – target object does not exist.
- **AccessDenied** – rights insufficient.

### Details

- Stores comment text in the Desktop DB for the specified CNode; truncates silently beyond 199 bytes.
- Rights differ for empty vs non-empty files/dirs:
  - Non-empty directory: search to all ancestors inc. parent, plus write to parent.
  - Empty directory: search or write to ancestors except parent, plus write to parent.
  - Non-empty file: search to all ancestors except parent, plus read & write to parent.
  - Empty file: search or write to ancestors except parent, plus write to parent.
- Requires FPOpenDT and that the object exists.

---

## FPRemoveComment (0x39)

### Summary

Remove the Desktop database comment associated with a file or directory.

### Request

|    # | Field        | Type   | Size | Description                             |
| ---: | :----------- | :----- | :--- | :-------------------------------------- |
|    1 | Pad 0        | byte   | 1    | 0 pad byte                              |
|    2 | DTRefNum     | int    | 2    | Desktop database refnum.                |
|    3 | Directory ID | long   | 4    | Directory containing the target object. |
|    4 | PathType     | byte   | 1    | 1 = short, 2 = long names.              |
|    5 | Pathname     | string | var  | Path to file or directory.              |

### Reply

_The reply doesn't carry any payload_

### Result Codes

- **ParamErr** – bad refs/type/path.
- **ObjectNotFound** – no such file/directory.
- **ItemNotFound** – no comment present.
- **AccessDenied** – insufficient rights.

### Details

- Rights mirror FPAddComment (cases for empty / non-empty dir/file).
- Requires FPOpenDT.

---

## FPGetComment (0x3A)

### Summary

Fetch the comment text associated with a file or directory in the Desktop database.

### Request

|    # | Field        | Type   | Size | Description                             |
| ---: | :----------- | :----- | :--- | :-------------------------------------- |
|    1 | Pad 0        | byte   | 1    | 0 pad byte                              |
|    2 | DTRefNum     | int    | 2    | Desktop database refnum.                |
|    3 | Directory ID | long   | 4    | Directory containing the target object. |
|    4 | PathType     | byte   | 1    | 1 = short, 2 = long names.              |
|    5 | Pathname     | string | var  | Path to file or directory.              |

### Reply

|    # | Field   | Type   | Size | Description     |
| ---: | :------ | :----- | :--- | :-------------- |
|    1 | Comment | string | var  | Comment string. |

### Result Codes

- **ParamErr** – bad refs.
- **ObjectNotFound** – CNode missing.
- **AccessDenied** – rights insufficient.
- **ItemNotFound** – no comment stored.

### Details

- For directory comments: search to all ancestors including parent.
- For file comments: search to all ancestors except parent, plus read access to parent.
- Requires FPOpenDT.

---

## FPAddIcon (0xC0)

### Summary

Add or replace an icon bitmap in the Desktop database for a given (FileCreator, FileType, IconType) triple. Bitmap is streamed via ASP SPWrite.

### Request

|    # | Field       | Type    | Size | Description                                         |
| ---: | :---------- | :------ | :--- | :-------------------------------------------------- |
|    1 | Pad 0       | byte    | 1    | 0 pad byte                                          |
|    2 | DTRefNum    | int     | 2    | Desktop database refnum.                            |
|    3 | FileCreator | ResType | 4    | Creator code.                                       |
|    4 | FileType    | ResType | 4    | File type code.                                     |
|    5 | IconType    | byte    | 1    | Icon variant (small/large/mask etc.).               |
|    6 | Pad 0       | byte    | 1    | 0 pad byte                                          |
|    7 | IconTag     | long    | 4    | Opaque tag stored with the icon.                    |
|    8 | BitmapSize  | int     | 2    | Size in bytes of icon bitmap to follow via SPWrite. |

### Reply

_The reply doesn't carry any payload_

### Result Codes

- **ParamErr** – bad refs.
- **IconTypeError** – replacing an icon with one of a different size.

### Details

- If an icon already exists for the triple (FileCreator, FileType, IconType) and the new bitmap has the same size, it is replaced; otherwise error.
- Requires FPOpenDT.
