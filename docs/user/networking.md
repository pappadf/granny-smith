# Networking Macs together

Emulated Macs can see each other over AppleTalk, as Macs on a real network
did: file sharing, program linking and network games. Macs in other tabs of
your browser are connected for you; a friend's Mac in another browser, on
another computer anywhere, is connected with one invitation and one reply.

Each Mac keeps its own small AppleTalk network, and Granny Smith connects the
networks for you. The Chooser on each Mac shows the others' file servers.

## 1. Macs in other tabs

1. Open Granny Smith in two or more tabs of the same browser.
2. In each tab, start a Mac with **AppleTalk** set to **Active** (the default).
3. That is all: within a few seconds the tabs are connected. The panel's
   **Network** tab lists them under **This computer**.

A tab keeps its Mac running while it is hidden behind another, so the other
tabs can still reach it. Close a tab, or turn AppleTalk off in it, and the
others notice within a few seconds. A browser may suspend a tab that has been
hidden for a long time (to save power); the other tabs then show it as
**Suspended by the browser**, and it rejoins when you look at it again.

## 2. A friend's Mac

You need a way to send your friend a link, and to receive a reply: any chat
or email will do. You both need a Mac running.

### 2.1 Inviting a friend

1. Open the panel's **Network** tab.
2. Type your name in **Your name** (your friend sees it).
3. Click **Invite a friend**. After a few seconds an invitation appears.
4. Click **Copy invitation** and send it to your friend.
5. When your friend sends back their reply, paste it under
   **2. Paste your friend's reply** and click **Connect**.
6. Keep this tab open: the connection lives in it.

### 2.2 Accepting an invitation

1. Open the invitation link your friend sent. (Or paste the invitation under
   **Or paste an invitation** in the **Network** tab and click
   **Accept the invitation**.)
2. If no Mac is running yet, start one: the invitation opens in its
   **Network** tab.
3. Click **Copy reply** and send the reply back to your friend.
4. When your friend pastes it, the two Macs are connected. Your friend's
   name shows under **Friends**, with the time a message takes to go there
   and back.

Your friend can then reach the Macs in all your tabs, and you theirs.

The time beside your friend's name is how long a message takes to go there
and back. File sharing works at any delay. Past about 100 ms network games
feel it, and past 200 ms action games are hard to play; in Marathon, choose
**Net Speed: AppleTalk Remote Access**.

### 2.3 Disconnecting

Click **Disconnect** in the **Network** tab, or close the tab that holds the
connection.

## 3. Using the network on the Mac

- **Files:** turn on **File Sharing** in the Mac's **Sharing Setup** control
  panel (System 7) to offer its disks; the other Mac finds it in the Chooser
  under **AppleShare**.
- **Games:** network games find each other through their own "join" or
  "gather" dialogs.
- **Zones:** with every Mac in the same zone (the default, **Granny Smith**)
  the Chooser lists one zone and shows everything in it, as on a single
  network.

Your browser's own **Shared Folders** server and the LaserWriter are offered
only to the Macs in your own tab, not to friends.

## 4. Speed

While a Mac is connected to another, the toolbar's **Max** speed is off: it
would run the Mac's clock ahead of the other's, and file sharing sessions
would drop. **Real** and **Faster** work normally.

## 5. When it does not connect

| What you see | What to try |
|---|---|
| "That is not an invitation" or "not a reply" | Copy the whole text again: a part of it is missing |
| "The two networks could not reach each other" | Some networks (some workplaces, schools and mobile connections) block direct connections. Try another network on one side, or use a relay server (below) |
| The tabs do not connect | Check that AppleTalk is **Active** on each Mac, and that the tabs are in the same browser profile (a private window is separate) |
| The other Mac is not in the Chooser | Its file sharing must be on, and AppleTalk must be **Active** in the Chooser on both Macs |

### 5.1 A relay server

When the two networks cannot connect directly, a relay server (TURN) can pass
the traffic between them. Granny Smith does not run one: use the free tier of
a TURN service, or your own. In the **Network** tab, click
**Relay server (TURN)…** and fill in its address (it starts with `turn:` or
`turns:`), user name and password. They are kept in this browser only, and
used for your next invitation or reply. One side having a relay is usually
enough. The traffic is small: an hour of use is around 100 MB at most.
