# Remote AFP volumes

How the AFP server publishes a remote tree -- an archive.org collection or a
host directory -- as a read-only volume that is
filled in as the guest walks it, and how further file servers share the
network with the first.  Sources: `src/core/network/afp_remote.c` (the
volumes), `afp_remote_object.c` (`appletalk.remote`), the server table in
`afp_volume.c`, deferred commands in `appletalk_asp.c`, and the page's
providers in `app/web2/src/bus/remoteAfp.ts` and `app/web2/src/lib/remoteAfp.ts`.
The AFP calls themselves are [appletalk_server.md](appletalk_server.md).

## 1. Several file servers on one network

The network has up to four AFP servers (`AFP_MAX_SERVERS`).  The first is
`appletalk.afp`, "Shared Folders", with the default share; the others are
added by name (`appletalk.remote.add_server`) and each is its own entry in
the Chooser, with its own volumes:

| server | NBP socket | ASP service |
|---|---|---|
| the first (`appletalk.afp`) | 8 (and the session socket 54) | 0 |
| each further one | 10, 11, 12 (`AFP_EXTRA_SOCKET_BASE`) | its own |

ASP keeps a table of services (`asp_add_service`, `ASP_MAX_SERVICES`): a
listening socket, the AFP callbacks and their context.  An OpenSess on a
service's socket opens a session with that service, and the reply names the
service socket as the session's server socket (SSS), so the session's
commands, attentions and write continuations all travel on it.  Before each
callback the server switches `g_afp` to the server its context names, so a
handler serves only that server's volumes; a client that names another
server's volume ID gets ObjectNotFound.  Volume IDs are unique across all
servers (`vol_id_in_use` looks at every one), and FPGetSrvrParms lists only
the server's own volumes.

Removing a server (`remove_server`) closes its sessions, withdraws its NBP
name and frees its volumes.  The first server cannot be removed.

## 2. A remote volume

A remote volume is an ordinary volume over a host directory, the **cache**,
with a URI for its root and the remote state of every folder and file that
has been listed (`struct afp_remote_vol`, a hash of nodes by relative path).
It is read-only: the dispatcher refuses every call that would change it with
VolLocked before the handler runs, and an FPOpenFork asking for write access
with AccessDenied; FPGetVolParms reports the read-only attribute.

What is not in the cache yet is asked for when a command needs it:

| the command | needs | asks for |
|---|---|---|
| FPEnumerate of a folder, or any path through it | the folder's entries | `list` of its URI |
| FPOpenFork of a file, either fork | the file's data | `fetch` of its URI |
| either of those on a container | the container's tree | `fetch` of its URI, then the unpack job |

Until a folder is listed its parameters say it has one child (so the Finder
shows it can be opened); a file not yet fetched reports the listed size.

### 2.1 Waiting: deferred commands

The guest's command cannot wait on the emulator thread, and an answer can
take seconds.  So a handler that finds what it needs is not there yet starts
the request and marks the command deferred (`afp_remote_command_deferred`);
the dispatcher then returns `ASP_RESULT_DEFERRED` and ASP sends no reply.
ASP keeps the command (`asp_defer`, eight per link): the ATP request's
transaction ID, node, socket and bytes.  The client retransmits its request
on ATP's timer; a retransmission of a deferred transaction is recognised
(`asp_is_deferred`) and dropped, so the client keeps waiting.  When a
request finishes (`remote_finish`) `asp_retry_deferred` runs every deferred
command again: one that now finds what it needed is answered (ATP's XO cache
takes the reply, so a late answer still reaches the client), one that needs
more stays deferred.  A request that failed makes the command that asked for
it fail with MiscErr.  A deferred SPWrite is refused with MiscErr rather
than kept (its data would have to be held), which a read-only volume never
needs.

A wait can outlast ASP's two-minute session timer at either end.  The
client's retries mostly end at the XO cache, and System 6's AppleShare stops
retrying after a while, so the server does not count a session with a
deferred command as idle (`asp_session_waiting`).  The client closes a
session it has not heard from in two minutes ("the file server's connection
has unexpectedly closed down"), so the server tickles every open session
every 30 seconds of guest time (`asp_tickle_sessions`).  Guest time can run
much faster than the host's in turbo mode, so a few seconds of downloading
was enough to lose the session before both.

Requests time out after ten minutes of host time; host-side work (a
`dir://` read, an unpacking) does not.

### 2.2 Listings

A listing is a text file, one entry per line, tab-separated:

| field | meaning |
|---|---|
| kind | `d` a folder, `f` a file |
| name | UTF-8; `/`, `\` and control characters become `-` |
| size | bytes |
| mtime | Unix seconds |
| type, creator | four characters each, or empty: the core then guesses from the name's ending |
| uri | the entry's own URI, passed back unread |

Lines starting with `#` are ignored.  Applying one makes the folder's
children: a folder becomes an empty folder, a file an empty placeholder with
the listed type, creator and date (`afp_meta_update`, `utime`).  A file
whose name ends in an archive or disk-image ending (`.sit`, `.sea`, `.cpt`,
`.zip`, `.tar`, `.gz`, `.hqx`, `.bin`, `.img`, `.dsk`, `.image`, `.dc42`,
`.iso`, `.toast`, `.dmg`, ...) and
whose size is at most `unpack_limit` (64 MB by default) is a **container**:
it is shown as a folder named without its endings ("Game.sit.hqx" ->
"Game"), unless that name is another entry of the same listing.

### 2.3 Containers

The first time a container is entered it is fetched and copied out
(`shell_cp_contents`, on the I/O worker): the VFS's namespaces open StuffIt,
Compact Pro, zip (merging macOS's `__MACOSX` resource forks), tar, gzip,
BinHex, MacBinary and HFS, MFS and ISO 9660 images, and each file arrives
with both forks and its Finder info.  A container holding one container is
opened in turn, up to three levels (`.sit.hqx`, a disk image in a zip).
Then the tree is tidied before it is moved into the folder: the bookkeeping
of a copied disk (Desktop DB / DF, DeskTop, Trash, `.DS_Store`, ...) is left
out, and a lone folder is collapsed into its parent (twice at most), so
"Game" holds the game, not "Game/Game Folder/".  A container that does not
open is shown as the original file inside its folder.

### 2.4 The cache

The cache is the volume's directory; remote bookkeeping lives under its
`.gs-afp/remote/` control directory, which the server never shows:

| file | holds |
|---|---|
| `L<hash>.tsv` | the listing of the folder whose relative path hashes to `<hash>` |
| `D<hash>` | a marker: that file was fetched, or that container unpacked |
| `req-<id>`, `unpack-<id>` | a request's result, or an unpacking, in progress |

A folder listed or a file fetched once is served from the cache from then
on, after a reload too, and with no network.  Removing the cache directory
forgets it.

## 3. Who answers requests

The core does not know archive.org.  It passes URIs back unread and decides
nothing by their scheme but one: `dir:///host/path` is a host directory,
listed and copied on the I/O worker (used by tests, and for a host-side
mirror).  Every other request is the event

```
{"event":"afp_remote_request","id":N,"op":"list"|"fetch","uri":"...","dest":"..."}
```

(`notify:afp_remote_request` on the web), answered with
`appletalk.remote.complete(id, status, [message])` once `dest` holds the
listing or the file.  `appletalk.remote.requests` lists the pending ones,
for a page that missed an event.

On the web the page answers them (`app/web2/src/bus/remoteAfp.ts`).  At
startup it adds one server:

| server | volume | URI |
|---|---|---|
| Internet Archive | Software Library | `ia:collection/softwarelibrary_mac` |
| | Shareware Games | `ia:collection/MacintoshSharewareGames` |
| | HyperCard Stacks | `ia:collection/hypercardstacks` |
| | Macintosh Garden | `ia:collection/Macintosh_Garden_Collection` |

Their caches are `/opfs/afp-remote/<server>/<volume>`.  `{"disabled": true}`
in localStorage, key `gs-remote-afp`, leaves the server out.

The page's URIs (`lib/remoteAfp.ts`), every component URI-encoded:

| URI | list | fetch |
|---|---|---|
| `ia:collection/<c>[/<bucket>]` | the collection's items as folders, from `advancedsearch.php`; a collection of more than 300 is shown as folders by the first letter of the items' titles | |
| `ia:item/<item>[/<folder>]` | the item's uploaded files (`/metadata/<item>/files`, without archive.org's metadata, thumbnails and derivatives), a folder level at a time; a zip larger than `unpack_limit` is a folder of its members | |
| `ia:file/<item>/<path>` | | `/cors/<item>/<path>` |
| `ia:zip/<item>/<zip>[/<folder>]` | the zip's members, from archive.org's listing of it (`/download/<item>/<zip>/`), without `__MACOSX` | |
| `ia:member/<item>/<zip>/<member>` | | `/download/<item>/<zip>/<member>` |

A member fetched from a zip comes without the resource fork macOS keeps for
it in `__MACOSX`; StuffIt archives, disk images and the other containers
carry their own, and a whole zip small enough to unpack keeps the merged
forks (§2.3).

## 4. The object model

```
appletalk.remote
  servers          ro  list    the further servers' names
  add_server(name), remove_server(name)
  volumes          ro  list    every remote volume: {server, name, uri, path}
  add_volume(server, name, uri, cache), remove_volume(server, name)
  requests         ro  list    pending requests: {id, op, uri, dest}
  complete(id, status, [message])
  unpack_limit     rw  uint    containers up to this many bytes are folders
  issued, failed   ro  uint    requests since start
appletalk.afp.sessions[i].server_socket   the server a session is with
```

The servers and volumes are network state, like the shares: never in a
checkpoint, not kept across a reload (the page adds them again).

## References

1. *Inside AppleTalk*, Second Edition (Apple Computer, 1990), ch. 11 (ASP)
   and ch. 13 (AFP).
2. *AppleTalk Filing Protocol Version 2.1 and 2.2* (AppleShare IP 6.3
   Developer's Kit, Apple Computer, 1999).
