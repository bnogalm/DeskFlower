# DeskFlower: design and internals

How DeskFlower works inside: processes, security, networking, input, clipboard, files, packaging
and updates. For what it is and how to install it, see the [README](../README.md).

## Architecture

| Process | Role |
|---|---|
| `deskflower-service` | Identity, discovery, pairing, encrypted sessions, workspace replication and input routing |
| `deskflower-session` | Started by the service inside the interactive session: reports monitors, captures thumbnails, captures and injects keyboard/mouse |
| `deskflower-agent` | QML user interface only; controls the service over local IPC (`QLocalSocket` + CBOR) |

The session helper authenticates to the service with a secret passed in its environment
(`DESKFLOWER_SESSION_TOKEN`). It exits when it loses the service, which starts a fresh one. A
helper that keeps exiting right away (no display yet, a portal that keeps failing) is restarted less and less
often: 2 s, doubling up to once a minute, back to 2 s once one ran 15 s (`SessionLauncher`).

On Linux the service is a `systemd --user` unit, which outside GNOME and KDE may start without the
graphical session's variables. The agent (started by XDG autostart in every desktop) therefore
imports `DISPLAY`, `WAYLAND_DISPLAY`, `XAUTHORITY`, `XDG_SESSION_TYPE`, `XDG_CURRENT_DESKTOP` and
the session bus address into systemd and D-Bus activation before it starts the service
(`systemctl --user import-environment`, `dbus-update-activation-environment --systemd`), and also
sends them in its IPC hello (`agent {env}`): the service lays the allowed variables over its own
environment for the helper and restarts the helper only when that gives other values than the
running one really got (a helper started before any agent, with the same display, stays). The
variables are taken from one agent at a time (the first that sends them, until it disconnects), so
a second agent (another session of the user, one under `ssh -X`) does not move the helper back and
forth. Linux only: never on Windows, where the service is LocalSystem and agents of any user
connect, nor on macOS, where launchd gives the helper its session.

Why keyboard and mouse sharing does not work in the session (no display, the desktop refused the
remote desktop permission, no input capture portal, a missing permission) is not only logged: the
helper reports `InputBackend::problem()` (`inputStatus`), the service puts it in its state
(`inputProblem`), and the agent shows it in a banner.

**Threads in the service (0.1.20).** The service's main thread routes input, keeps the encrypted
sessions alive (a keepalive every second; a session with nothing received for 6 s is dropped and
the cursor sent home) and answers the local IPC, so it never waits on a disk. The disk work of a
file transfer runs on a thread of its own per transfer, as the console user (impersonated per
batch of at most 100 ms of work): on the receiving side creating folders, writing files, checking
the free space, making room and cleaning up; on the serving side building the manifest and reading
the offered files. The main thread only moves frames between the network and the transfer's
bounded queue (decryption stays there, at most 1 MB of the socket per turn): when the disk falls
behind it stops reading the stream (`SecureStream::setReadPaused`; TCP's flow control slows the
sender) and resumes once the worker caught up; over a relayed tunnel, which has no flow control,
a paused stream fails ("The receiving disk is too slow for this connection") once more than 64 MB
wait. A transfer checks at every batch that the user it acts for is still the signed-in one, and
stops ("The signed-in user changed") without deleting anything as the wrong user. Before 0.1.20
a large paste (thousands of files) did that work on the main thread: the sessions timed out and
the cursor was sent home mid-transfer. (`src/core/filetransfer.*`, `src/core/transferworker.*`,
`tests/tst_transfer.cpp`.)

**Many small files (0.1.20).** A copied source tree (thousands of small files in thousands of
folders) used to be written one file after another, with a link check of the whole path (2-3 ms)
before every file. Now:
- *Held destination folders* (`fs::SafeTree`): the destination's existing folders are checked once
  and held open, and every folder the transfer makes is created, opened without following a
  reparse point, checked (a directory, not a symbolic link or junction) and held; a file's folder
  is then a lookup. On Windows the handles are opened without `FILE_SHARE_DELETE`: no held folder
  can be renamed, deleted or swapped for a junction while the transfer writes (so received
  folders cannot be renamed or deleted until it ends), which also closes the window the per-file
  check left between checking and opening. They are released before the end is reported and
  before any cleanup.
- *Packed frames* (transfer protocol 3): every data frame is full (64 KB), the items one after
  another across file boundaries, instead of one item per frame; a tree of small files sends
  about half the frames, and a Node's thread costs about the same per frame whatever its size.
- *Parallel writers* (robocopy /MT style): the transfer's worker reads the frames, makes the
  folders and splits the data between the files; a few writer threads of the transfer create,
  write and close the files (one writer for fewer than 64 files under 1 MB, otherwise half the
  processor's threads, 2 to 8; the small files are counted, not averaged). All of one file goes to one writer, in order
  (its size on disk only grows); each file goes to the least busy writer in the order of the
  list (a path listed twice goes to the writer of the first, after it). The writers' queues share
  an 8 MB budget (from 8 MB down to 4 MB once full), behind which the stream is paused as before.
  Every writer works as the console user, checked against the user the transfer started with
  (a change seen by any writer stops them all and deletes nothing), and never holds the user's
  rights while it waits. The first failure names the file and stops the others; a failed or
  cancelled transfer is cleaned up only once every writer has stopped. On Windows a file is
  created with `CreateFileW` (long paths, shared for reading so a paste can read it while it
  grows, never resized ahead) without following a reparse point at its own name.

Measured over loopback (both devices on one Windows 11 laptop with Microsoft Defender, a tree of
15,645 files in 3,656 folders, 892 MB; `tests/bench_transfer.cpp`, without the service, so
without impersonation): 80-89 s with the earlier unpacked design, 24 s with held folders, 23 s with packed frames
(one run each), 21-26 s with parallel writers (five runs, about 24 s on average: no gain shown
on that machine; the gain over the earlier design comes from the held folders); one 1 GB file 157-173 MB/s
(132-137 MB/s on the same day before the writers took the received frames without a copy).
A paste into Explorer then copies the files once more out of the staging folder, which these
figures leave out (an optional, off by default, *Move paste* moves them instead: see under
[Clipboard](#clipboard)). On that machine creating files is the limit: the same files written
by a plain loop take 12-13 s however many threads write them (CopyFileW, which copies from a
local file, is not limited the same way), and over loopback the sender's reads compete with the
receiver's writes for the same disk and scanner.

**Faster pastes of many files (0.1.20).** Pasting a large folder from another device into
Explorer worked but was slow: Explorer asks for the files one after another, and every file
cost several round trips to the agent, on the thread that also drew the window. Now:
- *Agent:* a small file (1 MiB or less) already complete on disk is handed to Explorer directly
  in memory, without a stream and its extra round trips; anything else still gets the growing
  stream. The file list is built once per paste instead of per request, a waiting stream polls
  sooner (1-20 ms on a high-resolution timer), copies in 1 MiB blocks, and the window's
  animation stops while a paste reads files. A request that takes no stream is refused before
  anything starts, so nothing is transferred before a real paste, as before.
- *Service:* the older received copies are no longer deleted before the transfer starts (2-30 s
  for a large one); they are renamed aside and deleted in the background alongside it (a copy
  that does not fit waits for that cleanup before the free space check). A folder with at least
  64 small files gets parallel writers even with one huge file among them. The sending device
  opens and reads small files ahead on a few threads, in the same order, so the protocol is
  unchanged.

Measured with an Explorer-like reader in another process (`tests/bench_paste.cpp`, the same
15,645-file tree, Release): files already received 30.0 s before, 22.8 s after; while they are
being received 47 s before, 37.6 s after; reading the same files from a local folder takes 20.5 s.

**Transfer protocol and versions (0.1.20).** Every device advertises the protocol of the
features that change incompatibly between versions (`wire::kProtocolVersion`, today 3: the file
transfers) in its authenticated hello (`pv`): devices before 0.1.20 have protocol 1, and 0.1.20
advertises 3. A device that sends no `pv` is taken, only as a fallback, to have the one its
version implies (2 from 0.1.20, 1 before). Two devices of different protocols do not
transfer files with each other: a fetch is refused at once with a clear error ("<device> uses an
older transfer protocol (version 2). The protocol has changed: update it."), a publisher answers a
get of another protocol with that error (an older receiver shows it as its transfer error), a
receiver refuses a manifest without `pv` 3 and the old data frames, and an older receiver ignores
the new ones (it fails with "Transfer ended early", never misreads them). Copied files and large
clipboard formats from such a device are not offered (the text, HTML and small images it sends
inline still are) and the user is told which device to update. The agent shows a banner for every
connected paired device of another protocol, and a tray notification once per device and protocol:
for an older device, a hint to click Update on it when this device can send it its signed package
(it sees the offer in our hello), or a link to the downloads; for a newer device that offers its
package, an Update button. Everything else keeps working with older devices: discovery, pairing,
the hello, keyboard and mouse, the clipboard's text, the remote view, the layout and signed
updates (which never depended on the transfer protocol), so an old device is still found, warned
and updated. (`src/core/wire.h`, `src/core/node.*`, `src/core/filetransfer.*`,
`src/agent/qml/Main.qml`, `tests/tst_node.cpp`, `tests/tst_transfer.cpp`, `tests/tst_update.cpp`.)

### Security

- **Identity:** every device has an X25519 key pair; `deviceId` = BLAKE2b(public key).
- **Pairing:** 6-digit PIN with **CPace** (PAKE over ristretto255).
  - The PIN is single use, and every request gets a new one.
  - Guessing: after 3 attempts in which a guess was made (a `PairMsg1` arrived) and that do not
    end in pairing (wrong PIN, abandoned, timed out; not the user cancelling) the device refuses
    pairing requests for 5 minutes, and after the first failure it answers each guess only after
    300 ms (`src/core/pairing.*`). A challenge nobody answers is no attempt, so a stranger cannot
    lock the device out of pairing by requesting a PIN and walking away, and a device that
    already has a challenge pending gets that same one again, never a second PIN.
  - The session id in the challenge is the pairing's secret: a pending challenge is repeated only
    to the address and port the request came from (a stranger repeating the requester's public
    ids from elsewhere gets a refusal, not the challenge), and the PIN answer and the confirmation
    are taken only from there, so a session id obtained some other way spends no PIN and ends no
    pairing.
  - Aborts: a `PairAbort` ends a pairing only when it carries the secret that identifies it (the
    session id from the challenge, or before that the random nonce the `PairRequest` carried,
    which the responder's refusals echo) and comes from the address the pairing talks to, so a
    stranger who knows the public ids cannot cancel somebody else's pairing. The reason a peer
    sends is a code shown in local words (`e`, with the lockout minutes in `m`); free text is
    only shown from older versions, capped at 200 characters.
  - The channel identifier binds both public keys, so a man in the middle cannot swap them.
  - The `PairRequest` may carry `tl`, the level the requester chose for the responder (0.1.20):
    only a suggestion that the responder's PIN dialog shows preselected, never a decision; a
    `group` suggestion is only pointed at, and applies only if the responder's user picks it (see
    [Trust levels](#trust-levels)). Older responders ignore it.
- **Sessions:** `Noise_IK_25519_ChaChaPoly_SHA256` handshake.
  - AEAD packets with an explicit counter and an anti-replay window.
  - Rekey every 2 minutes.
  - When a peer restarts, the reliable channel is resynchronised through a boot id. A peer that
    restarted before its old session timed out (a quick service restart or update) is a new
    connection on both sides: the other side sends its hello again (without it the restarted one
    would know no version, and the clipboard and drags would refuse it) and its higher layers
    start over with it (`tests/tst_node.cpp`, `tests/e2e/tst_e2e.cpp`).
  - Replayed handshakes: a handshake initiation carries the sender's boot id and a counter; the
    responder takes one only when its counter is above the last one seen in that boot, and
    remembers the last 8 boots of each peer, so a recording from before the peer's last restarts
    is refused too (a restart starts a new epoch, whatever the clock does, so a clock set back
    never blocks a device). Until the first packet of the new session decrypts, an initiation
    changes nothing else (not the peer's address, nor the reliable channel, nor the responder's
    own pending initiation): a recording from an older boot costs the responder a key exchange
    and an unconfirmed session, nothing more.
  - A forged or corrupted handshake response is discarded without touching the pending
    handshake (the initiator's index travels in clear, so anyone can send one), and responses
    that reach a pending handshake are rate limited like initiations.
  - Addresses: packets go where the paired device was last authenticated (a handshake or a
    decrypted packet). Every other endpoint known for it is kept beside that one (see
    [Networks, routers and NAT](#networks-routers-and-nat)). Beacons are unauthenticated, so
    what they claim for a device is only one more endpoint to try: handshakes go to all of them
    in turn (at most 4 beacon claims per device, forgotten after 30 s without beacons), and the
    endpoint where a handshake completes becomes the address. A forged beacon can therefore
    neither steer handshakes to its sender nor keep the real device's claim out, and a device
    answers a paired peer's authenticated initiation even while its own is pending, unless its
    own went to that same endpoint a moment ago (the usual simultaneous start, where the
    smaller id's initiation wins).
- **Transport:** reliable, ordered channel over UDP (SACK, retransmission, fragmentation) plus
  unreliable datagrams for mouse movement.
- **Files and large data:** TCP (`src/core/securestream.*`), encrypted the same way: a separate
  Noise IK handshake with the paired keys (prologue `deskflower/tcp/1`), then ChaCha20-Poly1305
  frames with per-direction counters. Only trusted devices are accepted, in both directions.
  TCP uses the UDP port number when it is free, otherwise any port, announced in beacons and in
  the authenticated `hello`.
  - A recorded first handshake message can be replayed by anyone, so the accepting side trusts a
    stream only once the first encrypted frame from the other side decrypts; until then it counts
    as unauthenticated (10 s handshake timeout).
  - When no TCP connection gets through (a router or firewall in the way), the same stream runs
    over a connection the other device opens back (*dial-back*), or tunnelled through the UDP
    session; either way it is the same Noise handshake with the paired keys, so nothing about
    who is trusted changes (see [Networks, routers and NAT](#networks-routers-and-nat)).
- **Trust levels** (see [Trust levels](#trust-levels)): what a paired device may do here is
  decided here (guest, this device only, all my devices) and declared to it in the authenticated
  `hello` (`tl`); the pair shares the stricter choice. Between devices that both chose *All my
  devices*, `group.members` and `group.introduce` carry other members' public keys over the
  encrypted session, so one PIN trusts the whole group: a compromised member can introduce
  devices to every other member. Introductions never change an existing record, are not
  forwarded, and skip devices listed in `forgotten.json` (written by *Forget*, cleared by a PIN
  pairing or by *Allow again* under *Forgotten devices*).
- **Pairings belong to a user:** the Windows service serves every user of the computer, so a
  pairing is recorded with the SID of the user signed in at the console when it was made, and the
  peer is reachable (sessions, messages, streams, input) only while that user is the one signed in
  there, their lock screen included. On the sign-in screen, or in another user's session, the peer
  is not answered and does not appear in the agent. Pairings made before 0.1.16 go to the first
  user seen. Linux and macOS run one service per user, so this does not apply there.
- **The service never uses its own rights on the user's files.** On Windows the service runs as
  LocalSystem, and every path it serves or writes comes from the console session (the agent's
  `clip` files, a `dragCatch` answer, the drop folder under the cursor) or from the user's own
  Received folder, which that user may rearrange at will. So the service does all of it **as the
  signed-in user** (`platform::actAsInteractiveUser`, injected into `FileTransfer` from
  `src/service/main.cpp`): enumerating offered files and folders, opening each one to send it
  (whether the serving pass starts on a file or on the blobs a clipboard copy puts first),
  creating and writing received files, moving dropped files into place, and deleting staging and
  old download folders. With nobody signed in nothing is offered, served or written ("Nobody is
  signed in"). **Reading** is bounded by that user's rights alone: a path the user chose may go
  through a junction or link (their "My Documents" junction, a redirected folder) and is offered
  and served as usual, since it is opened as the user; links met *inside* a chosen folder are left
  out, whatever they point to, and the user is told how many items stayed behind. **Writing** is
  where a planted link could redirect rights the user does not have: the destination (the fetch
  target, every received folder and file, the drop folder's staging folder) must not pass through
  a junction or symbolic link, checked before it is used and again right before each folder is
  created and each file is opened (`fs::passesThroughLink`), so a link planted between the checks
  leads nowhere. A Windows shortcut (`.lnk`) is an ordinary file in both directions (Qt 6 reports
  it as `isSymLink()`; only `isSymbolicLink()` and `isJunction()` count as links). The drop
  folder is not confined to the home folder (a drop onto another drive is legitimate): the user's
  rights are the boundary. A transfer that fails or is cancelled removes what it created under
  its target folder, as the user, so no half-written file stays behind
  (`tests/tst_transfer.cpp`, `tests/tst_fsutil.cpp`).
- **IPC clients belong to the console session, and stay checked.** The pipe is open to local
  users (the agent runs as the user), so a client is admitted only from the active console
  session: from its process id when it connects, and from its own token at its first message
  (`ImpersonateNamedPipeClient`, which a pipe only allows once something was read from it, the
  token's session id, then `RevertToSelf`), and that session is kept per client, the session
  helper included. On every session notification (console connect and disconnect, logon, logoff)
  and on a 2 s watch (notifications can be missed) the service re-checks every client and drops
  those no longer in the active console session, so after a fast user switch the previous user's
  agent stops receiving the new user's clipboard, PINs, prompts and screen previews and can send
  no command for them. When the console user changes, the service also forgets the shared
  clipboard (its content, published offer and fetch in progress), the cached screen thumbnails
  and any pending pairing or remote-view prompt, and revokes every phone session
  (`src/service/servicecontroller.cpp`). Any process of the signed-in user may still talk to the
  service: it can, for instance, start the install of a signed update offered by a paired device,
  or stop the service, exactly as that user can from the agent. Its console session is the
  boundary, not the agent process.
- **The session helper is given a moment to tidy up** when the console session changes (fast
  user switching): the service closes its pipe, on which it puts the system cursors back and
  releases injected keys in the session it leaves and exits by itself; only a helper still there
  after `kExitGraceMs` is terminated, and the service's event loop never waits for it.
- **`%ProgramData%\Deskflower` is created secured.** In service mode the folder (and any missing
  parent) is created in one step with its protected DACL (SYSTEM and Administrators only,
  `CreateDirectoryW` with a security descriptor), so there is no moment between creation and
  securing in which another user could slip a subfolder in and keep ownership of it. At start the
  service still verifies the folder and the `logs` and `updates` subfolders it writes to: a link,
  or an owner other than SYSTEM or Administrators, and it refuses to start, naming the folder to
  delete (`platform::createConfigDir`, `platform::secureDirectory`).
- **A guest is never this device's master.** Guest status is per pairing, so a device that is a
  guest here may be fully trusted by a third device, which would adopt its `master` and relay it
  (the workspace registers gossip). A `master` naming a guest, or a `layout` authored by one, is
  dropped whoever delivers it, and every consumer of the register (keyboard and mouse, remote
  view, clipboard hand-offs, file drags) refuses a guest master on its own, so flagging the
  current master as a guest takes effect at once: its cursor is sent home if it is here, its
  crossings are refused, its clipboard orders and file drags are ignored. The version of a
  dropped register is still remembered, so the next local edit is stamped above it and the
  devices that did take the guest's version adopt the edit instead of keeping two layouts for
  good. The guest's own edit (not one it relays) is answered with `ws.refused {k, r: "guest"}`,
  at most once per kind every 10 s, so that its user is told rather than left with a layout
  nobody else has (`tests/tst_workspace.cpp`, `tests/tst_clipboard.cpp`, `tests/tst_input.cpp`).
- **Only the device a drag was caught on may answer it.** `drag.caught` is taken only while the
  master has a catch pending and only from the device it asked (`InputController::catchSource`),
  never from a guest (which is not even asked: a button held across its edge crosses at once), so
  a third peer cannot substitute its own files for the ones being dragged
  (`tests/tst_filedrag.cpp`). `drag.caught {pending}` only extends the wait for that same answer.
- **Dragged originals are recycled only on confirmation.** Only the device the files were dropped
  on may report (`drag.dropped` names it), and only a confirmed move (the service wrote them, or
  the drop target reported `DROPEFFECT_MOVE` once the files were transferred) sends them to the
  Recycle Bin; anything less keeps them. A device older than 0.1.20 answers `ok` as soon as the
  drop target read the files, without confirmation, so its `ok` counts as a copy. A folder the
  agent names for a drop is used only if it is an absolute path to a folder the console user sees
  (checked while impersonating that user), else the files go into Received.
- **Transfer bounds cannot be inflated by the sender.** A fetch manifest is refused when any item
  exceeds what is left of the limit (the sum is checked overflow-safe, so two 2^62 sizes do not
  wrap past it), when it lists more than 250,000 items (folders included, counted across the
  frames of a split manifest), or when a name is unsafe (folder names are sanitised like file
  names); the bytes actually received are counted against the limit too, independently of what
  was announced. A clipboard announcement only counts the formats this device would keep (plain
  text, HTML, PNG), each clamped to 64 MB, so invented `big` keys or absurd sizes change neither
  the fetch limit nor what is kept pending; a file list over 1 TB, 100,000 files or 100,000
  folders, or with a size below -1 (-1 marks a folder), is not kept pending at all
  (`tests/tst_transfer.cpp`, `tests/tst_clipboard.cpp`).
- **The clipboard is sealed in transit; a guest reads it only on a paste.** Each copy is sealed
  with a fresh key (`crypto::seal`, XChaCha20-Poly1305); the ciphertext travels, the key goes
  with it only to devices the sender trusts (`clip {k}`, inside the encrypted session), and it is
  escrowed at the master (`clip.key`, accepted only from the device that sealed it, and sent again
  when the master changes or reconnects). **User decision:** a trusted device
  (*This device only* / *All my devices*) is not asked for evidence of a paste: a paste
  there just decrypts, and anything reading its clipboard is served, so a trusted device's
  clipboard manager or history may keep shared content. Trusted devices share every copy, and
  take a clipboard from each other whenever it is newer (not only when the cursor brought
  it). For **guests**: a guest gets the clipboard only through a hand-off,
  without the key, and reads a format only after asking the master (`clip.unseal` →
  `clip.unsealed`), which grants only for a paste chord it forwarded there, and only the
  clipboard the master knows the guest holds (never one it relayed before); the grant latches
  briefly per id and device, at the master and in the guest's plaintext cache (after the latch
  the plaintext is wiped and a poll is refused again). A guest replaces nobody's clipboard except
  through a hand-off the master announced. A request names the clipboard id it is for: a stale
  clipboard object renders nothing. Copied files: a trusted device is let into the gated offer
  when the clipboard is sent to it; a guest only on the grant (`clip.allow` → `clip.allowed`,
  answered only by the publisher that was asked, `FileTransfer::allowPeer` on a `gated` offer);
  the master opens only its own current clipboard's files, whatever offer the request names, a
  publisher lets a device in only for a paste of its own current clipboard (the `kid` travels with
  `clip.allow`), and a guest is not even told the copied files' names, only their count and size.
  Copied files are transferred only for a paste on every device, trusted
  ones included (a key or a button pressed there just before; see *Clipboard*), and every
  clipboard object with copied files asks clipboard history and monitors to leave it alone.
  A guest's clipboard object refuses reads with no user input just before and asks clipboard
  history and monitors to leave it alone, and a guest refuses to ask the master on nothing, so a
  clipboard manager reading a guest's clipboard on arrival triggers nothing. Answers without a
  request id are ignored, held requests for keys not yet escrowed are bounded (16), and a guest's
  escrowed key is evicted before a trusted device's. An older peer (before 0.1.20) is sent
  plaintext at a hand-off, as 0.1.19 understands it, unless it is a guest
  (`tests/tst_clipboard.cpp`, `tests/tst_crypto.cpp`, `tests/tst_input.cpp`).
- **Received names are safe on every platform:** besides `..`, absolute paths and drive letters,
  the receiver refuses Windows reserved device names (`CON`, `PRN`, `AUX`, `NUL`, `COM1`-`COM9`,
  `LPT1`-`LPT9`, with or without extension), components ending in a dot or a space, control
  characters and components over 255 UTF-16 units (`FileTransfer::sanitizeRelativePath`).
- **An update download cannot be pinned.** Besides the 30 s idle timeout, a download has a
  deadline (15 minutes) and must advance by at least 64 KB every 30 s, counted from the first byte
  of the package (a source slow to start is covered by the idle timeout and the deadline), so a
  device chosen as the source cannot hold the updater in *Downloading* with a trickle; the user
  can cancel it (*Cancel* next to the progress, IPC `cancelUpdate`, `Updater::cancel`), and a
  guest is never offered as a source (`tests/tst_update.cpp`).
- **Phone sessions belong to the user who granted them** (see [Shared folder and phone
  access](#shared-folder-and-phone-access)).
- **Unauthenticated traffic is bounded**, so that flooding the LAN costs little:
  - Rate limits on beacons, handshake initiations (two key exchanges each before the sender is
    known) and pairing packets; a beacon query is answered at most twice a second per sender, so
    forged source addresses cannot turn a device into a reflector.
  - At most 64 unknown devices are kept from beacons. Beacons never rename a paired device (its
    name comes from the authenticated `hello`).
  - At most 16 TCP connections waiting to authenticate (at the cap the oldest pending one is
    dropped; from one address, at most 8 that have sent nothing, the ninth evicting the oldest
    of them, so paired devices opening several streams at once still get through), a
    connection that sends nothing is closed after 2 s (10 s for the whole handshake once it
    speaks), 32 streams per paired device (tunnelled ones included), and streams quiet in both
    directions for 5 minutes are closed. Reliable messages larger than 8 MB are dropped (bulk
    data travels over streams). A paired device gets at most 8 dial-backs under way at once, and
    a dial-back is taken only with the one-time nonce this device sent over the session, from
    that device, within 10 s.
  - Phone access: request headers must arrive within 15 s, at most 16 connections per address
    (64 in total; when the table is full the connection idle for longest makes room) and 16 TLS
    handshakes at once, and a request body is read only when the request carries a valid
    session. A connection whose last request carried no session is closed after 5 s idle (a
    phone's after 60 s), so strangers cannot hold the slots with cheap requests. Memory follows
    the client's own pace: requests are read from the socket only while the answers to the
    previous ones are going out (at most 256 KB queued) and no file is being streamed, so a
    client that pipelines requests without reading, or keeps sending during a download, is
    slowed down by the kernel instead of filling the service. A streamed file is cut at the
    length announced (a file still being copied into the folder does not spill into the next
    response) and the connection is closed if the file ends early.

- **Updates from paired devices:** Ed25519-signed manifests checked against a public key compiled
  into the build (see [Signed updates](#signed-updates-from-paired-devices)).

Relevant code in `src/core/`: `noise.*`, `cpace.*`, `pairing.*`, `session.*`, `reliable.*`, `node.*`.

### Networks, routers and NAT

Devices on one network find each other with beacons (multicast and broadcast, which routers do
not forward). Across routers, a device is reached at an address the user types (*Add by
IP* on the *Devices* tab), at the last endpoints it was connected at, or at what a group member says. Nothing is
relayed through a third device: two paired devices always talk directly (`src/core/node.*`).

- **Endpoints.** Node keeps every way of reaching a paired device, each with where it came from,
  in this order of preference: *configured* (typed by the user and answered from there by that
  device), *self-reported* (an address of one of its interfaces, from its authenticated
  `hello`), *observed* (where its authenticated packets came from; behind a NAT, the router's
  mapping), *group hint* (told by a group member) and *beacon* (claimed by a beacon, never
  stored). Each source is bounded (3, 8, 2, 4 and 4) and the best 8 non-beacon endpoints are
  stored in `peers.json` (`addresses`: `{a, p, tp, src, ok}`); `address`/`port` still hold the
  last endpoint connected at, for earlier versions. An observed endpoint never replaces a
  configured or self-reported one, and endpoints are kept after the device connects. While it is
  not connected, handshakes go to all of them in turn, quickly at first, then every 5 s; once a
  minute for a device with no configured endpoint, no self-reported address on one of this
  device's subnets and no endpoint that worked in the last 24 hours (across routers no beacon
  comes to speed things up again; addresses it reported on another network are strangers' when
  seen from elsewhere). A change of this device's addresses
  (checked every 5 s) makes the retries quick again and sends a new `hello`.
- **Typed addresses** (`addManualPeer`) are asked every 30 s until a device answers from there,
  and become that device's configured endpoint once it is authenticated there (not on the
  word of a beacon).
- **NAT detection.** The `hello` carries the device's own IPv4 addresses (`la`, at most 8, no
  loopback), its UDP port (`up`) and whether it knows dial-backs and tunnels (`st`). A device
  seen at an address that is none of those is behind a router that translates addresses: the
  agent shows *(via router, NAT)* next to the endpoint in use, and the log says so (info level).
  Peers before 0.1.20 send none of this and are treated as before (direct TCP only): with such a
  peer behind a router, streams towards it (files, remote view) still fail, since it neither
  dials back nor tunnels.
- **Hairpin.** Two devices of one network can meet through the router's public address (a group
  hint, a typed address): streams to it go nowhere. When the device it talks to through a
  router has one of its own addresses on a local subnet, a handshake goes there every 5 s (4
  tries, each one unanswered for 5 s counting as one failure; renewed by a beacon from that
  address or a network change) and the session moves there
  when it answers. Authenticated packets move the address (roaming, a new NAT mapping), except
  from one of the device's own addresses to a router's while the direct one still brings
  packets.
- **Streams** (`Node::openStream`, used by file transfers, remote view and updates) race every
  path and the caller gets one `SecureStream` whatever won:
  1. TCP to every endpoint (most likely first: the address it is seen at when no router
     translates it, its own addresses on a local subnet, configured ones, its other addresses,
     the router's), one more every 250 ms;
  2. over the session, `stream.dial` asks the device to connect back (after 300 ms, at once when
     it is behind a NAT): it tries this device's endpoints the same way, but only those it has
     reason to trust (where its packets come from, endpoints that worked or were typed, its own
     addresses on a local subnet; never group hints or addresses merely claimed on other
     networks, so that a peer cannot make it connect to third parties), and opens the stream
     with a first frame carrying the 16-byte nonce, matched with the request and the device's
     key, after which the stream serves the request like any other;
  3. after 5 s without either, a tunnel: the stream's frames travel over a second reliable
     channel of the UDP session (`wire::Inner::TunnelData`/`TunnelAck`; messages
     `[op][opened by sender][id u32][data]` with open, data and close), with its own window so
     that bulk data never delays input or clipboard messages, and back-pressure from its
     unacknowledged bytes. The stream inside still runs its own Noise handshake. Like the
     control channel, that channel starts over only when the peer restarts or is paired again,
     not when one side alone loses the session (its tunnels are lost; the numbering goes on).

  The stream gives up after 20 s. Every failed TCP attempt is logged at info level with the
  address and port. When the device's own address (no router between) refused TCP while
  another path worked, it is told (`stream.unreachable`); if it is on another subnet, its
  agent shows a notice: a firewall there refuses other networks (see below).
- **Windows Firewall.** The rules `--install` adds let the service in on private and domain
  networks, and on public ones from the local subnet only. *Allow devices on other networks*
  (*Advanced settings*; `otherSubnets` in `settings.json`, applied by the service with `netsh`
  and again at every start) widens the public rule to the private ranges `10.0.0.0/8`,
  `172.16.0.0/12`, `192.168.0.0/16`, the carrier-grade NAT range `100.64.0.0/10` and link-local
  `169.254.0.0/16`. Like the other machine-wide settings, it is taken from the agent of the
  active console session (the only IPC clients the service admits); changes run one `netsh` at a
  time and the last one asked for is what the rule ends with. Linux and macOS firewalls are left
  to the user: allow UDP 24800-24801 and TCP 24801, plus TCP 24802 while phone access is on (or
  the ports chosen), or the service binary.
- **Tests** (`tests/tst_node.cpp`, `tests/natproxy.h`): nodes bind to their own `127.0.0.x`
  (`Node::Options::bindAddress`; the others to `DESKFLOWER_BIND_ADDRESS`, which ctest sets to
  `127.0.0.1`, so no test listens beyond loopback) and a simulated router (endpoint-independent
  mapping, port-restricted or full-cone filtering, mapping timeout, blackout, hairpin, no inbound
  TCP) sits between them through `Options::datagramHook`/`connectHook`: pairing from either side,
  dial-back and tunnelled file transfers, refused dial-backs, endpoints in `peers.json`, group
  hints, the hairpin move (also when the first probe is lost), tunnels after only one side lost
  the session, and reconnection after restarts and new mappings.

### Workspace (screens, layout, master)

`src/core/workspace.*` replicates this state between paired devices over the encrypted reliable channel:

- **Screens:** every device's monitors in its native desktop coordinates (Windows: physical pixels,
  macOS: points, X11: pixels), reported by the session helper.
- **Thumbnails:** small JPEG captures of each monitor (at most 480×300), taken by the session
  helper when it connects, when the Screens tab opens, or when a peer asks. Screen geometry is shared
  with every paired device, but pixels are not: a device sends its thumbnails only to a peer it gave
  *Always allow this device* (the remote view grant below), only in answer to that peer's request,
  one peer at a time, over the encrypted reliable channel. Peers never relay them, nothing is stored on
  disk, and revoking the grant or forgetting the device makes the other side drop its copies; a
  viewer also drops them when the device disconnects, and asks again when it reconnects.
  Without the grant the *Screens* tab shows "No permission to preview this screen" and layout
  editing still works. Received thumbnails are bounded (256 KiB, 1024×1024).
- **Layout:** one offset per device ("pack" of monitors) in a shared plane, edited by dragging packs
  in the agent's *Screens* tab. Edges snap to other packs.
- **Master:** the device whose keyboard and mouse are shared ("Share this keyboard and mouse").

Layout and master are last-writer-wins registers ordered by `(version, author id)` and gossiped to
all connected peers, so concurrent edits converge. A device that treats the author as a guest
drops its edits and answers `ws.refused` (see [Security](#security)); the guest then shows a
notice ("treats this device as a guest: it keeps its own arrangement of the screens"), and its
*Screens* tab marks that device *You are its guest*. They are persisted in
`<config dir>/workspace.json`, together with a cache of remote screens so offline devices can still
be arranged, and with the view plans (where this device shows each screen of a viewed device, see
[Remote view](#remote-view)), which are this device's own and never gossiped.

### Keyboard and mouse

- The master's helper reports when the cursor pushes against an outer edge of its desktop. The
  service maps the point through the layout (`src/core/router.*`) and, if a connected device is
  there, starts capturing: the local cursor is parked and hidden, and input is swallowed by
  low-level hooks.
- Nothing is lost at the edge. While the service decides, the OS would hold the cursor against the
  edge and whatever is moved meanwhile would be gone. So the service tells the master's helper in
  advance where a push past an edge will cross (`edges`: the stretches of its screen edges behind
  which a connected device lies that did not just refuse the input and is entered rather than
  viewed; `Router::neighbourSpans`, kept current as devices connect, disconnect and refuse). On
  such a stretch the helper captures the moment the cursor touches the edge, inside the hook
  callout (Windows: the cursor is parked and the system waits on that callout, so no move is in
  flight), reports the edge with `c: true`, and the motion that follows is applied after the
  crossing. The service confirms (its `capture {on}` is then a no-op) or, when it does not cross
  after all (bounce guard, device gone or refused meanwhile, drag being asked about), answers
  `capture {on: false}` at the edge point so the cursor reappears where it was, naming that edge
  (`ge`) so the helper does not capture there again on the very next move while the reason lasts.
  An early capture reported while the input is already away (the crossing went through a plain
  report meanwhile) is kept as it is. With a button held the helper never captures early (the drag
  is asked about with the cursor in sight), and after a return through an edge (`capture {ge}`)
  that edge does not capture early for 250 ms, on either side. The map only lists a stretch for
  the device the crossing would actually go to (the nearer of two overlapping neighbours), and is
  pushed again when devices connect, disconnect or refuse, the layout changes, or remote view is
  switched on or off. Modifiers held while crossing are handed over after the edge report, so the
  remote gets their presses. Older helpers and other platforms, without the map, work as before.
- A cursor that stops at an edge is explained: when the device beyond declared this one a guest,
  the agent shows a notice saying so (at most once every 10 s per device,
  `InputController::crossingBlocked`), and the *Screens* tab marks the device. A device that
  refuses the input (nothing runs in its session) is explained by its refusal
  (`InputController::inputRefused`); pushing towards it again during the 3 s pause that follows
  adds no notice.
- On Windows, a move seen by the hook right after a cursor warp (capture starting or ending) is
  kept when it is nearer to the warp point than to where the cursor was before it (and within a
  quarter of the monitor of the warp point), and dropped only when it belongs to the old position
  (computed before the warp), so the warp no longer costs the moves that follow it.
- The master keeps a virtual cursor in the target's native coordinates and sends absolute
  positions as unreliable datagrams (latest wins), so pointer acceleration is applied only once:
  the moves read together from the helper make one datagram per pass of the service's event
  loop, not one each. Buttons, wheel and keys use the reliable channel, after the motion that
  preceded them; clicks carry the cursor position. A device that falls behind applies the latest
  position it has (service and helper alike: queued moves not injected yet are replaced, not
  replayed), and its helper corrects an injected move only where the hook saw it land elsewhere.
- Keys travel as PS/2 set-1 scancodes (`0xE0xx` for extended keys), so each device applies its own
  keyboard layout. A key or button pressed before the cursor left is released locally.
- Windows keys (`src/core/keyinjection.h`, pure and unit tested in `tests/tst_keyinjection.cpp`): the helper injects every key by
  scan code (`KEYEVENTF_SCANCODE`), except the Windows keys and Menu (`0xE05B`, `0xE05C`,
  `0xE05D`), which go by virtual key (`VK_LWIN`, `VK_RWIN`, `VK_APPS`) with their scan code and
  `KEYEVENTF_EXTENDEDKEY`: Win shortcuts (Win+D, Win+E...) did nothing on a Windows device driven
  from another one while Alt+Tab worked. The modifiers handed back to the local desktop when
  crossing are released the same way. On the capturing side the hook takes those keys (and Right
  Shift) from their virtual key rather than from the scan code and extended flag the keyboard
  reported, and a key without a scan code gets the one its virtual key maps to, instead of not
  being forwarded at all.
- Keyboard diagnostics (`src/core/keydiagnostics.*`; *Advanced settings → Keyboard
  diagnostics*), to find out why a key combination does nothing on the device being controlled.
  **Only in builds configured with `-DDESKFLOWER_KEY_DIAGNOSTICS=ON`** (off by default, see
  *Building*): otherwise its code is not built, the agent shows nothing of it
  (`Backend.keyDiagnosticsAvailable` is false), and the service, the helper and the peers ignore
  its messages, as an older version does.
  For 2 minutes (or until stopped) each device records, as lines in its log (`keydiag:` prefix)
  and in the agent (shown live, with a *Copy* button):
  - the service: each key forwarded to another device and each key received from the master
    (scan code, down/up, modifiers held in that direction, which device), a key the helper
    sent that went nowhere, and a key received from a device that is not obeyed (not the master,
    a guest, a view request awaiting an answer);
  - the helper on Windows (`InputBackend::setKeyDiagnostics`, lines sent as `diag {l}`): each key
    its hook catches while capturing (virtual key, scan code, flags such as `EXTENDED`), whether it
    was swallowed and forwarded (of the keys held since before the capture, which stay local,
    only that a modifier was released here); each injection with the exact `INPUT` fields,
    `SendInput`'s result and `GetLastError`, and what the hook saw of it (`INJECTED`...), plus
    the foreground program for the Windows keys; once at the start, the helper's user and
    integrity level, session, input desktop, foreground program and keyboard layout. The keys it
    injects carry a mark in `dwExtraInfo` (`kInjectMark`), so its hook records the events of its
    own injection and never other programs' injected keys. The foreground program and the
    start's details are looked up on the helper's main thread, not on the thread that answers
    the low-level hooks. Other platforms' helpers say that they record nothing more.
  The volume stays modest: a key held down (autorepeat) is one line when it goes down and one
  when it comes up, with the repeats counted (`after N repeats`), on every side. The lines that
  say where each side runs (the start, the helper's `where:`, the devices asked) are kept apart
  and never dropped; of the rest the newest are kept (2000 in all, with a line saying how many
  older ones were dropped, which stay in the log). The agent redraws the text at most every
  200 ms.
  Starting it also asks the connected devices that trust this one (`diag.key {on, ms}`, at most
  2 minutes there too) to record for it: their lines come back as `diag.line {l}` and are shown
  with their name, so one copy holds both ends; they are still taken for 2 s after the end, for
  the last lines on the way (a device that stops on its own says so in its last line). A device
  runs one diagnostic at a time, only the
  device that asked stops it remotely, guests are neither asked nor obeyed, and versions without
  diagnostics ignore both messages. While it records, every device shows a banner saying so
  (with who asked). Agent commands: `keyDiag {on}`; events: `keyDiagLine {l, h}` (`h`: a header
  line, never dropped), `keyDiagLines {lines, h}` (the run so far, its first `h` lines the header,
  for an agent that connects, and when a run starts); state:
  `keyDiag {on, ms, by}`; helper event `keyDiag {on}`.
  **Privacy:** only keys that go to or come from another device are recorded (and the events
  of the helper's own injection); what is typed for the device itself is never recorded, and
  nothing at all outside the diagnostic window. But what is typed on the other device while it
  records is recorded key by key, by scan code, so it can be read back (passwords too), and it
  stays in the log of both devices (the lines of the devices asked are written in the log of the
  device that asked too) after the window: the agent says not to type passwords or anything
  secret while it records. Lines stay in the service's memory until the next run, and in the
  log. Tested in `tests/tst_keydiag.cpp`
  (two nodes over loopback; built only with the option on) and `tests/tst_input.cpp` (`theKeyTraceSeesBothEnds`).
- Leaving a device releases everything injected there. A device only obeys the current master.
  If the master disconnects, the device releases its keys. If the helper stops receiving the
  service's heartbeat for 3 s, it gives the input back.
- There is always a way home. If the screens change while the cursor is on another device (a
  monitor goes off, a resolution or the layout changes), the cursor moves onto what is left; when
  no edge leads back to the master any more, or the device has no screens left, the input comes
  back to the master (`Router::canReach`). A device whose session helper is not running (nothing
  would move its cursor) answers `in.refused`: the master keeps its input, says so, and does not
  enter that device again for 3 s.
- Windows silently removes low-level hooks that once took too long to answer
  (`LowLevelHooksTimeout`). While capturing, the mouse hook swallows every move, so the hidden
  cursor stays parked: every 250 ms the helper checks that it still is. If it moved, the hooks
  were taken away (or a program moved it): they are put in again and the cursor parked. If that
  fails, capture ends and the helper tells the service (`captureLost`), which brings the input home.
- Keys go only to the device the cursor is on. When the master locks, signs out or switches
  sessions while the cursor is elsewhere, the cursor comes back to it first
  (`InputController::returnHome`), so what is typed on its lock screen (the password) stays there.
- As LocalSystem the helper's input thread follows the input desktop (Default, Winlogon), so it can
  also work on the lock screen and UAC prompts.
- Platform backends (`src/session/inputbackend_*.cpp`, key tables in `src/core/keymap.*`):

  | Platform | Capture | Injection | Status |
  |---|---|---|---|
  | Windows | Low-level hooks | `SetCursorPos` + `SendInput` | Built and tested |
  | Linux X11 | XInput2 raw events + pointer/keyboard grab | XTest | Built and tested under Xvfb (`tests/tst_x11.cpp`); not tried on a real desktop |
  | Linux Wayland | InputCapture portal (pointer barriers) + libei receiver | RemoteDesktop portal (`PortalSession`, restore token) + libei sender | Built; the portal sequence tested against a fake portal (`tests/tst_portal.cpp`); not tried on a real compositor |
  | macOS | `CGEventTap` + `CGAssociateMouseAndMouseCursorPosition` | `CGEventPost` (flags, click counts) | **Written without a Mac, never run** |

  The key translation tables (evdev and macOS virtual key codes to and from set 1) are unit tested.
  The Linux backends are built and tested in Docker (see [Linux tests in Docker](#linux-tests-in-docker));
  both Linux and macOS still need to be tried on real machines.

- **Linux:**
  - X11 uses root window pixels from XRandR. Capturing needs both grabs (pointer and keyboard):
    one held by another client (an open menu, a drag in progress, a screen locker) is retried
    every 50 ms for half a second, then the capture is given up and reported (`captureLost`, the
    service brings the input home) rather than half-capturing, which would type into both
    computers. The local cursor is hidden with XFixes while another device has the pointer (this
    computer's own mouse shows it again). Early capture at a known edge (`edges`) works as on
    Windows, except that held modifiers are not handed over. Throttles use monotonic clocks.
  - Wayland uses logical coordinates and needs a compositor with InputCapture (GNOME 45+,
    KDE Plasma 6.1+) to be the master, and the RemoteDesktop portal with libei (`ConnectToEIS`)
    to be controlled. The helper opens one remote desktop portal session for everything
    (`src/session/portalsession.*`: CreateSession → SelectDevices with `persist_mode` 2 and the
    stored restore token → ScreenCast.SelectSources (every monitor) → Clipboard.RequestClipboard →
    Start), so the desktop asks once and later sessions start without a dialog
    (`portal-restore-token` in the helper's configuration folder, readable by the user only)
    until the permission is revoked in the desktop's settings. liboeffis is no longer used. The
    InputCapture portal (being the master) has no such persistence: the desktop asks for it each
    time the helper starts. `RequestClipboard` is asynchronous like the rest. If the user grants
    the screens and the clipboard but not the keyboard and mouse, the session still runs for
    those (`inputProblem` says what is missing). When the desktop ends the session (the user
    stops sharing, the portal restarts) or it fails for another reason than a refusal, the helper
    exits and the service starts a new one (with the backoff above), whose session comes back
    from the restore token; after a refusal it stays, without asking again.
  - Without libei at build time, Wayland sessions get no keyboard/mouse sharing; without Qt
    D-Bus, no portal at all; without PipeWire, no screens. The rest still works, and the agent
    says what is missing.
- **macOS** (written without a Mac: built by CI only, never run; see the manual checks in
  *Installers*):
  - Allow *DeskFlower session* in *System Settings → Privacy & Security → Accessibility* (and
    *Input Monitoring* if macOS asks), and in *Screen Recording* for thumbnails and remote view.
    The service starts the helper responsible for itself (`posix_spawn` disclaiming the
    responsibility), so macOS asks for and lists the helper's own bundle
    (`com.deskflower.session`), not the service. Dragging files also needs *DeskFlower* (the
    agent) in *Accessibility*: it posts the mouse events that start and end a drag.
  - What is missing is shown in the agent's window with a button to the right settings page
    (`InputBackend::problem`, sent by the helper as `inputStatus {problem, link}` and kept by the
    service in the state event as `inputProblem`/`inputProblemLink`): Accessibility, Input
    Monitoring (the event tap could not be created), Secure Event Input (a password field or
    Terminal's *Secure Keyboard Entry*: the tap gets no keys, so keys typed for another computer
    would stay on the Mac) and Screen Recording. They are polled every second, and a permission
    granted later takes effect without a restart (the tap is created then). The stub backend of an
    unsupported platform reports itself the same way.
  - The helper is an `LSUIElement` app bundle, so it has no Dock icon.
  - The cursor is hidden while capturing and while another device has the pointer
    (`setCursorHidden`): `CGDisplayHideCursor` works for a background process once its window
    server connection has the private `SetsCursorInBackground` property (looked up at run time).
    The Mac's own mouse shows it again.
  - Modifiers are followed through `kCGEventFlagsChanged` with the device-dependent flag bits (left
    and right keys told apart), also while not capturing: a modifier held when the capture starts
    is released on the Mac, not pressed on the other device.
  - File drags: the left button's state and its release are reported like on Windows
    (`leftButtonHeld`, `localRelease`), and a release of a button pressed before the capture is
    reported too (it ends a drag that crossed).
  - Throttles and double clicks use a monotonic clock.
- **Control and Command** (any master, set per device on the Devices page for paired Macs): a PC
  keyboard driving a Mac can have Ctrl act as ⌘ Command. The master swaps the set-1 codes of Ctrl
  (`0x1D`/`0xE01D`) and of the Windows keys, which the Mac maps to Command (`0xE05B`/`0xE05C`),
  on their way to that device (`InputController::setSwapCtrlCmd`; settings: `swapCtrlCmd`, hex
  device ids; state: each peer's `swapCtrlCmd`; command `setSwapCtrlCmd {id, on}`). The paste
  chord is judged on what the device receives (Ctrl+V typed is Command+V there).

### Remote view

A paired device can be flagged *Remote view* in the *Screens* tab. Crossing into it no longer
moves the cursor to its own screens. Its **screens are shown full screen on your monitors**, and you
control it as in a remote desktop session. By default every screen goes to the local monitor where
the edge was crossed, one at a time (first the one the layout puts beyond that edge): pushing the
cursor past an edge of the shown screen towards another screen of that device switches to it.

- **Monitors…:** the button next to the flag opens *Where to show the screens of (name)*, with a
  choice per screen of that device: *Monitor where I cross* (the default), one of your monitors, or
  *Don't show*. Screens on different monitors are shown at the same time, each on its own, as if
  that computer's monitors stood on this desk (for a computer on another desk, whose screens cannot
  be seen); screens sharing a monitor are shown one at a time, the monitor showing the last one the
  cursor was on. A hidden screen is never shown and the cursor never enters it: pushing past an edge
  towards it crosses over it onto the visible screen beyond, if any, and is blocked otherwise.
  The last visible screen cannot be hidden, and a plan that hides every screen is ignored. The
  canvas marks each screen with the monitor it goes to (*→ name*) or *hidden*. The plan is this
  device's own (it names its monitors): kept in `workspace.json` (`"viewPlans"`), never shared with
  other devices, and dropped when the device is forgotten; a monitor it names that is gone falls
  back to the crossed one. At most 8 screens are displayed at once. A plan changed while the view
  is open applies at once: the screens move to the monitors it now names, and if the screen with
  the cursor is now hidden the cursor goes to the centre of the first visible one.
- **Cursor:** it enters at the same relative position along the edge. Pushing it past an edge of the
  shown screen towards another visible screen of that device moves it there (same stream, no new
  approval: the source checks that it is one of its own screens); if that screen shares the monitor,
  the monitor now shows it. Leaving through the side it came in, where no other visible remote
  screen lies, returns home onto the monitor showing the current screen, at the same relative
  position; other edges are blocked. The viewer draws the remote cursor on the window showing its
  screen. A monitor or remote screen of the view that disappears or changes ends the view.
- **Video** (`src/view/framecodec.*`, `src/core/remoteview.*`):
  - Each screen is cut into 128-pixel tiles and only the tiles that changed are sent, as JPEG.
  - Frames are scaled to the monitor that shows the screen and paced per screen by the viewer's
    acknowledgements (at most ~30 per second, never more than one frame of a screen in flight; the
    source gives up when any screen's frame goes unacknowledged for 30 s). A full frame is sent now
    and then, and first for every newly listed screen: each capture of a screen has a generation,
    tagged on the helper's frames (`g`), so a frame of the capture before, still on its way when the
    screen starts afresh or another viewer takes over, is dropped instead of shown. The viewer drops
    a frame larger than the limit it gave for that screen and ends the view when one frame exceeds
    12 MB of tiles (the helper's IPC refuses larger messages).
  - They travel over the encrypted TCP stream, all screens on one stream, each frame tagged with its
    screen. The stream carries frames only: the viewer asks for a set of the source's screens
    (`view {v: 2, screens: [{i, x, y, w, h, mw, mh}]}`), can replace it on the same stream
    (`screens`), acknowledge a frame (`ack {s, i}`) or stop, nothing else; the source answers with
    `frame {i, s, w, h, full, n}` + tiles. A request naming more than 8 screens, a repeated index or
    screen, or a rect that is not one of the source's screens is refused ("Bad request").
  - **Older devices** (before 0.1.20) understand one screen at a time (`view {w, h, screen}`,
    `screen {}`, `ack {s}`): a viewer sends those forms to such a source, so its plan still picks
    the monitor and hides screens, but only the screen with the cursor is shown; a source still
    serves such a viewer as before.
- **Approval on the shown device:** a device only shows its screen to the current master, only
  while it is paired, and only with its own user's consent. The *Remote view* flag in the layout
  grants nothing by itself.
  - The first time, the agent on that device asks *"(name) wants to see this screen"* with
    *Allow once*, *Always allow this device* and *Deny*. No answer within 30 seconds counts as Deny,
    and after a Deny the same device is refused for 10 seconds. The viewer forwards no keyboard or
    mouse input until the request is accepted, and the shown device ignores input from the
    requester while it asks, so nobody can click the prompt remotely.
  - *Always allow* is stored in `peers.json` (`"allowView": true`). It can also be given in advance
    with the *May view this screen without asking* switch of that device in the *Devices* tab (a
    paired master already has this keyboard and mouse; while the prompt is open its input is
    ignored, so turning the switch on then answers *Always allow* locally). Turning the switch off
    revokes it and ends a running view; forgetting the device does too. Pairing again starts
    without the grant, and an unpaired device is refused.
  - **Guests** never view the screen: the request is refused without asking ("does not show its
    screen to guests"), and making a device a guest (its *Trust* selector) withdraws its grant
    and ends a running view.
  - **Off entirely:** *Allow remote view* in *Advanced settings* (`"remoteView": false` in
    `settings.json`, applied at once). Off, every request is refused (the viewer reads "remote view
    is off"), a running view and the thumbnails end, even for devices with the grant, and this
    device shows nobody else's screen: crossing into a device flagged *Remote view* moves the cursor
    there as usual. The grants are kept for when it is turned back on.
  - Somebody must be signed in on the shown device with the agent running to answer; otherwise the
    request is refused. Signing out (or switching user) ends a running view: consent does not
    carry over to another session.
  - While its screen is shown, the device's agent displays *"Your screen is being shown on …"* with
    a *Stop* button, and the tray icon shows a notification.
- **Failures:** the viewer never waits forever. Its window shows the current step (connecting,
  waiting for approval) and, when the view fails, the reason stays on screen for 3 seconds and also
  appears as a message in the agent: the device is not reachable or did not answer, the request was
  denied or timed out, nobody can approve it, the device is no longer the master or was unpaired,
  or its screen cannot be captured (for example a locked or protected desktop). The cursor then
  returns home and the device is not entered again for 3 seconds.
- **Capture:** Windows uses DXGI Desktop Duplication on a worker thread, with GDI as a fallback
  (and for the first frame). Linux X11 uses XShm (MIT-SHM; plain `XGetImage` on a remote X
  server, noticed by the first `XShmGetImage` that fails where `XGetImage` works) on the
  streamer's worker thread with an X connection of its own, and reports an unchanged screen as
  such. Linux Wayland uses the ScreenCast part of the helper's portal session: the monitors'
  PipeWire streams are received on PipeWire's own thread (raw BGRx/BGRA/RGBx/RGBA in shared
  memory, at least three buffers, `src/session/pipewirecapture.*`), and the latest frame of each
  serves both the remote view and the thumbnails (`LocalScreens::setCaptureSource`). Frames are
  copied only while the remote view streams (for a second after each of its requests); otherwise
  the newest buffer is only kept (not given back to the compositor) and copied when a thumbnail
  asks for it. The desktop shows its screen sharing indicator while the helper runs. macOS 12.3+
  uses ScreenCaptureKit (`src/session/screengrabber_mac.mm`: an `SCStream` of the display at its
  pixel size, without the cursor, frames handed to the worker thread; only complete frames count,
  an idle one means nothing changed), and thumbnails use `SCScreenshotManager` on macOS 14+; both
  need the *Screen Recording* permission, and older systems fall back to `QScreen::grabWindow`. On
  Wayland the viewer windows are shown full screen on their output, since a Wayland client cannot
  place a window.
- **Status:** capture → encryption → transport → reassembly is tested end to end on Windows, and so
  are the approval, refusal and timeout paths (`tests/tst_view.cpp`). The full-screen viewer window
  and the approval dialog have not been tried on two machines yet.

### Clipboard

`src/core/clipboardsync.*`. Trusted devices share one clipboard; a guest gets it only when the
shared mouse brings it, and reads it only when the user pastes on it with the keyboard.

- **Trusted devices share every copy.** Between devices that trust each other (*This device only*
  or *All my devices*, as each side sees the other), a copy is announced at once to every
  connected trusted device running 0.1.20 or later (`clip {bc}`), and a device takes a clipboard
  from any device it trusts whenever it is newer. Two computers used each with its own keyboard,
  or with nobody sharing a keyboard and mouse, still copy on one and paste on the other; a device
  that connects (or reconnects) is sent the current clipboard once its `hello` arrived, and so is
  every connected trusted device when "Share clipboard" is turned back on. *(Changed
  in 0.1.20: up to 0.1.19 the clipboard reached only the device the cursor moved to.)*
- **Hand-off** (guests, and peers older than 0.1.20): the master sees every crossing from X to Y.
  If X is the master, it sends its clipboard to Y; otherwise it tells Y to expect X's clipboard
  and tells X to send it to Y. X obeys only the master, and only right after the cursor really
  left it (an `in.leave` within the hand-off grace). A device takes a clipboard from a device it
  treats as a **guest** only through such a hand-off (from the exact device the master told it to
  expect); a guest is never the master. So no guest can replace a clipboard, or pull one, without
  the user moving the cursor.
- Copied passwords are shared like anything else (password managers' "do not share" marks are not
  honoured on purpose).
- **Sealed in transit; the key goes only to trusted devices.** Each copy is sealed with a fresh
  32-byte key under a random 16-byte id (`kid`): `crypto::seal` (XChaCha20-Poly1305, the id and
  format bound in). What travels is the ciphertext. A trusted device is sent the key with it
  (`clip {k}`, over the encrypted session; a key that does not open every format of the content,
  checked once the large formats are fetched too, is dropped):
  **when the user pastes there, the content is simply decrypted and handed over**, with no time
  window, no evidence of a paste and no round trip; whatever reads the clipboard there (a
  clipboard manager, the system's history) is served too, and the decrypted formats stay cached
  until the clipboard changes. A guest never gets the key with the content: the device that
  sealed it **escrows the key at the master** (`clip.key`, or its own escrow when it is the
  master; sent again when the workspace master changes or reconnects), and the guest asks the
  master for it when the user pastes. A device that holds the ciphertext without the key (it came
  through a guest) asks the master too, and the master grants a trusted
  device at once (`clip.unsealed {keep}`: it keeps the key from then on). The escrow holds the
  newest key of a few authors; a guest's key is the first to go when it is full, so copying on a
  guest cannot push out the keys of trusted devices.
- **A guest's paste needs the chord.** The master grants a guest's `clip.unseal` only with
  evidence that the user ordered the paste on that guest with the keyboard: a paste chord it
  forwarded there (Ctrl+V, Shift+Insert, Ctrl+Shift+V, Cmd+V on a Mac; Win+V is not a paste)
  within two seconds; a context-menu or Edit → Paste cannot grant it. The master also keeps track
  of which clipboard each device holds (the author's key escrow and every hand-off it
  orchestrated): a guest is granted only the clipboard the cursor brought it, never one it relayed
  earlier. The grant latches for a few seconds (applications ask several times per paste), at the
  master and in the guest's own plaintext cache: after that the plaintext is wiped and a later
  read needs a fresh grant. A guest refuses to even ask the master unless a key or button press
  from the master reached it just now (for copied files, the paste rule of every device below:
  a key or button pressed there just before, refused quietly otherwise), and its
  clipboard object refuses reads with no user input
  just before without asking anyone (Windows: `GetLastInputInfo`, a refusal remembered 200 ms)
  and carries `ExcludeClipboardContentFromMonitorProcessing` / `CanIncludeInClipboardHistory = 0`,
  so a **clipboard manager reading a guest's clipboard on arrival, or polling later, gets nothing**
  and costs nothing. Copied files are gated by the same grant for a guest, with the publisher let
  in only after it (`clip.allow` → `clip.allowed`). A refused paste is a notice on the guest.
  Whether reads need the grant is told to the clipboard's owner with the content (`clipSet
  {gated}`) and told again whenever it changes (another master, the master's declared trust
  arriving after the content, a key granted to keep), so a guest's object is never left ungated
  nor a trusted device's gated.
- **Older peers**: a peer older than 0.1.20 (no sealing) is sent the clipboard **as 0.1.19
  understands it** at a hand-off: plaintext inline (`clip {f}`), the larger formats and the copied
  files in one offer opened to that peer (`o`, made once per clipboard), so it keeps working,
  degraded but not dropped. Not to an older guest (it knows no grant), nor when this device holds
  no key for the content: then the user is told it could not be delivered. Plaintext from an older peer (`clip {f}`) is still understood: it is re-sealed here
  (the key escrowed by the device that re-sealed it) and its offer, open to us, is fetched as
  before. An older **master** cannot hold keys: a paste that needs one (a guest's, or content that
  came without the key) fails at once with "Update DeskFlower on <master>" rather than waiting.

- Formats: plain text, HTML and images (as PNG), up to 64 MB each. Small sealed formats travel with
  the announcement over UDP; larger ones are fetched right away over encrypted TCP as ciphertext
  (`src/core/filetransfer.*`), useless to the receiver without the key.
- **Copied files and folders** (up to 100,000 files and 100,000 folders; the size is bounded by
  the receiving device's free disk space, checked before anything is written, and a 1 TB sanity
  cap) are not transferred when
  copied: the announcement only lists their names and sizes (file system metadata, up to 2,000
  entries). Folders are entries of their own (the copied folders and every subfolder, `dc` and
  `[path, -1]` in the list), so **empty folders and folder-only copies** travel too; hidden and
  system files and shortcuts (`.lnk`) are copied like any file, and links inside a copied folder
  are left out with a notice. A peer older than 0.1.20 is listed the files alone (it would drop a
  list with folder entries); a device of another transfer protocol cannot fetch them
  at all (see [Transfer protocol and versions](#architecture) and `src/core/filetransfer.h`).
  They travel when the user **pastes** on the other device, straight
  from the device they were copied on: a trusted device was let into the files' offer with the
  announcement (`clip {fa}`), so the paste is the grant; a guest only after the master grants its
  paste (the publisher is told to let it fetch them, and nobody else). The transfer is one the
  user sees, with its progress and a Cancel button, and a failure is a notice:
  - Windows: the agent offers them as virtual files (`FileGroupDescriptorW` + `FileContents`, as
    Remote Desktop does; a folder is a descriptor with `FILE_ATTRIBUTE_DIRECTORY`, which Explorer
    creates). Other programs may look at the list without causing a transfer.
    - **Streaming paste (0.1.20, service mode).** When the service says the files can be pasted
      while they arrive (`clipSet {stream}`: the device was let into the offer, reads need no
      guest grant, and the copy is listed, or announced with every relative path shorter than 260
      UTF-16 units (`sp`) and at most 50,000 files and folders), Explorer pastes with **its own
      progress dialog and Cancel button**, and copies each file while it arrives. The first
      contents read opens a paste session (`src/clipboard/pastesession_win.*`, a streaming
      `clipFetch` on a connection of its own, subject to the paste rule below): the service starts
      the transfer into a fresh staging folder and names it at once (`clipStarted`), and each
      `FileContents` is a stream on the file being written (`src/clipboard/streamhost_win.*`)
      whose `Read` returns once the bytes asked for are on disk (the file's size there; never a
      short read before its end). The streams live in the agent's multithreaded apartment, so
      Explorer's reads wait on COM's threads and never on the agent's own thread. A copy too large
      to list in the announcement is offered with the descriptor anyway, listed when a reader
      first asks for it (`clipList` → `clipListed`, in chunks of 5,000: a list-only fetch from the
      publisher that moves names and sizes, never contents, needs no paste, and is kept once
      listed: a list that failed without being refused is asked for again by the next reader).
      **Cancel in Explorer** stops the transfer, within a minute: Explorer stops reading and
      releases its streams, and a session nobody reads for 60 s while it runs (after a Cancel, an
      Explorer that crashed, a dialog left open) is given up: its connection closes and the
      service cancels the transfer nobody else reads (the clipboard object keeps its session
      between two files, so releasing a stream alone does not end it). Cancel in DeskFlower's
      window (its list of transfers) stops it at once. The next contents read, after a click, opens a new
      session. A newer clipboard does not stop a streaming
      paste under way (it goes on, detached, for its readers); a change of signed-in user does.
      The staging folder is kept (never pruned) while a session reads it. Disk space: the staged
      copies (up to 3 are kept, besides any copy still in use by a transfer or a streaming paste)
      plus the destination Explorer writes.
    - **Move paste (0.1.20, service mode, large pastes).** For a paste of **at least 2,000 files
      and folders or at least 1 GB** (`ClipboardBridge::kMovePasteMinEntries` /
      `kMovePasteMinBytes`), Explorer's second, single-threaded copy out of the staging folder
      would be most of the wait (every byte and every file created twice). Such a paste is
      offered instead as a deferred `CF_HDROP` with *Preferred DropEffect* = move (no descriptor,
      no contents): the service says the files can be moved (`clipSet {move}`: another device's
      files that this device was let into, reads needing no guest grant, and the setting on) and
      the agent picks it by size (`clipSet {pendingBytes}`). The paste's read of `CF_HDROP` opens
      a paste session for a **fresh** transfer (`clipFetch {stream, fresh}`, under the same paste
      rule as any transfer: a read that is no paste gets nothing, and nothing of it is remembered)
      into a new staging folder, waits for it on a worker thread while the agent's thread keeps
      pumping, with **DeskFlower's progress window and its Cancel** (Explorer shows nothing
      meanwhile), and then names the transferred top-level items. Explorer moves them: on the
      same volume as the received folder (`%LOCALAPPDATA%`, usually C:) that is a rename of the
      top-level items, instant; to another volume Explorer copies and deletes, about the cost of
      the virtual files without the concurrent second write. That copy serves one paste: a read
      again within 2 s with nothing taken from it gets the same items; any other read is another
      paste and gets a transfer of its own (the earlier copy may be gone, still being moved, or
      half moved by a paste cancelled midway); a second paste of the same object while a transfer
      runs is refused. Once the items are gone the copy is **consumed** (looked at on every read,
      and right after Explorer reports a move: *Performed* or *Logical Performed DropEffect* or
      *Paste Succeeded* through `SetData`, or `EndOperation`; a report alone, which cannot say
      which paste it is from, consumes nothing): the agent tells the service (`clipConsumed` on
      that session's connection), which removes what is left of the folder (an empty folder,
      files skipped in a conflict; only a folder directly in `Clipboard`, never one another paste
      reads or a transfer writes) and makes the next paste transfer again. A copy not consumed (a
      paste cancelled before or during its move) stays pinned while the clipboard object lives,
      at most the current one and two earlier ones (older ones go with the older received
      copies); when the clipboard changes, Explorer may still be moving out of it, so it stays
      pinned up to 30 minutes more (consumed as soon as its items are gone). The results Explorer
      sets on the object are not offered back to the next paste, which moves again. The service
      gives a move paste's request only another device's files transferred for it (never files
      copied on this device), puts the earlier copy back as the current one when the request is
      refused for want of a paste, and does not re-apply the clipboard after *Receive* when a
      move paste takes the same transfer. While a transfer holds the paste, newer clipboard
      changes wait for it; only the latest of them is applied once it returns. The clipboard
      keeps its content (not *cut*: nothing is deleted at the source), so the user can paste
      again. *Advanced settings → Pasting files → Large pastes: move the received files
      instead of copying them* (`"pasteMove"` in `settings.json`, for clipboards copied from then on) turns it on;
      it is **off by default**: on by default, Explorer's own reads of
      CF_HDROP (a right-click in a folder, its check for whether Paste is possible) started the
      whole transfer inside `GetData` and failed or hung meanwhile, so Explorer offered no Paste
      for large copies; off, large pastes are the streaming virtual files above.
      **Limits** (Explorer's behaviour cannot be tested automatically here; the tests drive the
      data object with fakes): whether Explorer empties the clipboard after moving out of a
      foreign data object, how it presents the long wait inside `GetData(CF_HDROP)`, its conflict
      dialogs and undo (Ctrl+Z moves the items back where they came from, the staging folder that
      was removed after the move: undo may fail, or put them back there, where they go later with
      the older received copies), and paths: the staging folder's own path counts in Explorer's
      limit on the moved items' paths (a warning is logged when one reaches `MAX_PATH`; Explorer
      may leave such items behind, the source keeps them) are checked by hand. Smaller pastes keep the streaming paste above. Linux and macOS are
      unchanged.
    - **Otherwise** (an older service, a guest's clipboard, longer paths, more than 50,000
      entries, an unlisted copy from a publisher older than 0.1.20, the agent outside service
      mode, drags) the objects work as before: virtual files whose first contents read has the
      service transfer everything, with DeskFlower's progress window (below), or, without a list
      or with a relative path longer than a descriptor holds (`MAX_PATH - 1`; it is never
      truncated, which would build a different tree), a deferred `CF_HDROP` of the transferred
      copies, in the same object as the sealed formats, transferred the same way (it recreates
      empty folders too). A list that turns out longer or deeper than announced makes the
      clipboard be applied again that way. The agent waits for such a transfer on a worker thread
      while its own thread keeps processing messages (`fetchWhilePumping`): its window, the
      transfer's progress and further calls into the clipboard object stay alive during a
      transfer that takes minutes, and a clipboard change meanwhile waits until the render
      returns; if the agent quits meanwhile (an update), it stops waiting for the transfer.
      Explorer's own thread waits inside `GetData` until the whole transfer has finished, with
      DeskFlower's progress but none in Explorer.
  - Linux (session helper) the lazy `QMimeData` offers a `text/uri-list`, plus
    `x-special/gnome-copied-files` (Nautilus) and `application/x-kde-cutselection` (Dolphin), that
    are rendered when an application reads them (the paste). The paste does not wait for a long
    transfer inside the selection request (Qt applications give up after 5 s): the first read
    starts the transfer in the background, with its progress in the agent, and waits for it at
    most 2.5 s. A small transfer completes within that paste; otherwise the application gets
    nothing this time, the user is told "Receiving N pasted items… paste again once they arrived",
    and then "N pasted items arrived: paste again"; that paste gets the local copies at once.
    A failure is a notice too, and the next paste asks again. Files received outright (already
    local) are offered with the same three formats.
  - macOS (session helper, `src/clipboard/clipboardbridge_mac.mm`): one pasteboard item per
    top-level file or folder (from the list, `pendingList`), each with a lazily provided file URL
    (`NSPasteboardItemDataProvider`), which is what Finder pastes. The first item read has the
    service transfer everything (`clipFetch`, once), as on Linux but waiting up to 8 s (Finder
    waits longer than Qt applications): a longer transfer goes on with its progress in the agent,
    with the same notices to paste again. After a refusal, a failure or that wait, the other items
    of the same paste give up at once (for 1.5 s), so one paste asks once. Without the full list (a
    guest gets none; more than 2,000 entries) a single item offers every path in the legacy
    `NSFilenamesPboardType` (and the first as its file URL); once transferred, the files replace it
    as one file URL each. macOS 15 builds even that legacy list from the items' file URLs, so the
    helper lists such a copy on demand (`clipList`: names and sizes only, no paste needed) and
    writes the items again, one per top-level file or folder, unless a paste already started. An
    item that gave nothing for a type (a refused or failed paste, a read that was no paste) is not
    asked for it again by the pasteboard: the same items are written again (0.3 s later, or 1.6 s
    after a refused or failed transfer, once the other items of that paste gave up), so the next
    paste asks. A replacement of the pasteboard waits while an item is being provided
    (the pasteboard server is waiting for that data). Qt's own pasteboard code is not used for
    this, because it reads every URL as soon as the data is set.
  - Transferred files are stored in `<received folder>/Clipboard/<stamp>-<n>/` (`n` counts the
    transfers from the time the service started, so a paste again after a cancel, a failure or a
    restart of the service never meets what an earlier one left; the last 3 are kept, never one a transfer or a streaming paste is
    still using).
    The received folder is hidden and apart from the shared folder, so phones never see these files
    (`platform::receivedFolder`): `%LOCALAPPDATA%\Deskflower\Received` on Windows,
    `~/.cache/deskflower/received` on Linux, `~/Library/Caches/Deskflower/Received` on macOS.
  The sender only serves exactly what it offered. The receiver rejects unsafe paths (`..`,
  absolute paths, drive letters) and enforces the size limit.
- **Copied files move only for a paste, on every device (0.1.20).** Formats (text, HTML, images)
  stay free on trusted devices, as above. File data does not: whatever reads the clipboard on its
  own (a clipboard history, a monitor, a file manager looking at what arrived, a remote desktop
  or virtual machine clipboard redirector) must not start a transfer of a 14 GB folder just
  because the cursor brought it. The service transfers copied files (`clipFetch`) only when a key
  or a button was pressed at this device after the clipboard was put on it and at most 2 s before
  the request: a paste chord (Ctrl+V, Shift+Insert, Ctrl+Shift+V, Cmd+V on a Mac), Enter, the
  Apps key, Shift+F10 or a mouse button (a click on Paste, a menu, Retry), forwarded by the master
  or pressed on this device's own keyboard and mouse (the session helper reports such presses,
  `localPress`, without saying which key; never motion, never injected input). A request with no
  press yet waits 300 ms for one still on its way (the helper and the pasting application talk to
  the service on different connections), then is refused quietly: an info line in the log, no
  notice, and the owner gives the reader nothing (`clipFetched {code: "noPaste"}`; the helper
  and the macOS bridge do not remember such a refusal, so the paste that follows asks again).
  When a list of the files was just given (a file manager reads the names first, then the
  contents), the 2 s count from when the list was asked for. Explorer reads the descriptor at the
  paste but creates every folder of the tree before it reads the first file, which can take far
  longer than 2 s for thousands of folders: the agent's streaming request says how long ago the
  descriptor was read (`clipFetch {listedAgo}`), and a press within 2 s either side of that read
  (at most 2 minutes ago) counts too. Without it (an older agent, the non-streaming objects) a
  first contents read that comes too late is refused, and Explorer's Try Again (a click) or
  Receive gets the files. Requests that come while one waits
  or transfers join it. This applies to trusted devices, the master and guests alike (a guest
  still needs the master's grant on top). The owner names the clipboard it asks for
  (`clipFetch {kid}`): a stale object is refused. Listing the files (names and sizes, see the
  streaming paste above) needs no paste: it moves no contents.
- **Receive.** The first refusal for want of a paste on a clipboard (`clipWaiting {kid, count,
  from}`) makes the agent show "N items copied on X" with a **Receive** button in its progress
  window, for an application that read the clipboard on its own and needs the files anyway (some
  file managers enable Paste only after reading it). Receive (`clipReceive {kid}`, on the agent's
  main connection) transfers the files without a press, only for the current clipboard and only
  when its publisher let this device in already (never a guest's clipboard, which needs the
  master's grant); once they are here the clipboard is applied again with the local copies, so
  the application that refused to paste sees them.
- **Clipboard monitors are asked to leave file clipboards alone.** Every clipboard object with
  copied files from another device (listed or not, gated or not), and every guest's object,
  carries the markers that clipboard history and monitors honour: on Windows
  `ExcludeClipboardContentFromMonitorProcessing`, `CanIncludeInClipboardHistory` = 0 and
  `CanUploadToCloudClipboard` = 0; on X11/Wayland `x-kde-passwordManagerHint` = `secret`
  (offered by the portal too); on macOS `org.nspasteboard.TransientType`, `ConcealedType` and
  `AutoGeneratedType`, with the pasteboard prepared for this Mac only (no Universal Clipboard).
  A trusted device's text-only clipboard stays unmarked (history keeps it). The markers may also
  hide file clipboards from remote desktop and virtual machine clipboard redirectors (to be checked
  in a virtual machine).
- **Compatibility (0.1.20).** The only change on the wire is an optional key of the announcement,
  `sp: true` (every relative path of the copied files and folders is shorter than 260 UTF-16
  units; sent only then, never to a guest, ignored by older devices). The list-only fetch is the
  ordinary transfer request ended after its manifest (between devices of the same
  transfer protocol only). The rest is local IPC (`clipSet {stream}`, `clipList`/`clipListed`,
  `clipFetch {kid, stream, listedAgo}`, `clipStarted`, `clipFetched {code}`, `clipWaiting`, `clipReceive`,
  `localPress`, `localPressSupport`), and an older agent with a 0.1.20 service, or a 0.1.20 agent
  with an older service, keeps the non-streaming paste. The paste rule and the markers apply on
  a 0.1.20 receiver whatever the publisher; the streaming paste of a copy too large to list needs
  a 0.1.20 publisher (for `sp`), and keeping the sessions alive during a large transfer needs both
  devices on 0.1.20 (each side's disk work moved off its main thread).
- **Last-writer-wins on a logical clock**: a copy is stamped max(wall-clock ms, newest stamp seen +
  1), so a copy made after seeing another one wins over it even when the devices' clocks disagree
  (with wall-clock stamps alone, a device whose clock was behind lost its copies in silence). An
  older clipboard never replaces a newer one (logged); a stamp more than a day ahead of this
  device's clock is refused, with a notice.
- The OS clipboard is read and written by `src/clipboard/clipboardbridge.*`, in a process that runs
  as the user: on Windows in service mode the **agent** (the session helper runs as LocalSystem,
  and Windows does not give such a process the user's clipboard reliably: changes went unseen and
  pasted files need calls into the owner), elsewhere the session helper. So on Windows clipboard
  sharing needs the agent running (it starts at sign-in by default). The bridge reads and writes
  through a `ClipboardBackend` (`src/clipboard/clipboardbackend.h`): `QClipboard` on Windows,
  macOS and X11, and on Wayland, where a background program without a focused window gets no
  clipboard through the Wayland protocol, the portal's Clipboard interface of the helper's remote
  desktop session (`src/session/portalclipboard.*`): `SelectionOwnerChanged` says the clipboard
  changed, the shared formats are read with `SelectionRead`; our content is announced by format
  with `SetSelection`, and each format is rendered (a sealed one decrypted) only when an
  application pastes it (`SelectionTransfer` → `SelectionWrite` → `SelectionWriteDone`). The bridge ignores the change
  notification caused by content it applied itself, so content never bounces between devices.
  Sealed content is put on the clipboard as one lazy object that renders each format only when an
  application asks for it: on Windows an `IDataObject` offering `CF_UNICODETEXT`, `HTML Format`
  (the `CF_HTML` header built at render time), `CF_DIB` and `PNG` next to the virtual files, which
  is not `OleFlushClipboard`ed (that would decrypt every format and transfer every file at once);
  on macOS lazily provided `NSPasteboardItem`s (public.utf8-plain-text, public.html, public.png on
  the first item, next to the file URLs); elsewhere a lazy `QMimeData` subclass whose
  `retrieveData` decrypts on demand. A rendered format is kept for the object's lifetime; a guest's (`clipSet {gated}`) only a few seconds, and its
  refusals are not remembered beyond a moment. A render asks the service over a connection to a
  second local server (`ipc::renderServerName`, `<name>-render`) that serves only `clipUnseal` /
  `clipFetch` and pushes nothing else, so a clipboard reader asking many times does not cost a
  full state and thumbnails burst each time; the owner waits longer (5 s) than the service waits
  for the master, so a late grant is not lost ("the first paste fails, the second works").
  macOS also keeps whatever a provider supplied on the pasteboard until the next copy (the
  pasteboard server holds it), so a guest's rendered format stays there too.
- macOS notices a copy only when an application is activated, which the background helper never
  is: the bridge polls the general pasteboard's change count every 300 ms (a counter: no content
  is read, no permission is needed) and skips the changes it made itself.
  `CF_UNICODETEXT` is rendered with CRLF line ends, and an HTML document that already carries the
  `CF_HTML` fragment markers is used as it is.
- **The owner is kept alive and in step.** On Windows the service watches for an agent in the
  console session (`AgentWatch`): when a user is signed in, agents are expected (they start at
  sign-in, or one ran in this session) and none has been connected for 5 s, it starts one
  (`platform::relaunchAgent`), retrying with exponential back-off up to 5 minutes (reset once an
  agent stayed up for a minute; paused while an update installs). The sign-in screen has a session
  helper but no user, so nothing is started there; right after a sign-in it waits 30 s, because
  Windows starts the Run key's agent late. The agent runs once per session anyway (a `Local\`
  mutex; elsewhere a lock file): a second one asks the first to show its window, unless started
  minimized, and exits, so two agents never both own the clipboard. When an agent (or, elsewhere,
  the session helper) connects, the clipboard that came from another device is put on it again
  (`ClipboardSync::reapply`), so nothing adopted while it was away is lost, unless the user
  copied something here since that was not shared (too large, an unsupported format, sharing
  off): that stays; if there is none
  (after a service restart the service holds nothing), it is told to drop a lazy clipboard object
  it may still own (`clipClear`), which would otherwise fail every paste in silence.
- The agent tells the user what happens to copied files: shared, not shared (and why), being
  received, ready to paste, or why they could not be fetched. Every other refusal or failure is a
  notice too, at most once per kind every few seconds (`ClipboardSync::notice`): a clipboard that
  could not be delivered to an older peer, a master too old to hold the keys, a paste refused or
  unanswered, a format that could not be decrypted or is not there, a stale clipboard object, a
  format too large to share, a guest's clipboard the
  cursor did not bring, a clipboard stamped far in the future, part of a clipboard that could not
  be received. A guest's reads with no sign of a paste (a clipboard manager polling) are refused
  without a notice. With the window hidden in the tray, problems show as a tray notification.
- **Transfer progress.** Files being received for a paste or a drag (`FileTransfer::fetch` with a
  kind) report their progress to the agents (IPC event `transfer`, at most 4 per second), which
  show them with a Cancel button (`cancelTransfer`) in the window and in the tray tooltip; a
  cancelled transfer removes what it had written. The event also names the folder the files go
  into (`target`) and whether Cancel would stop it now (`cancellable`). The agent also
  shows pasted files' transfers in a small progress window of its own, on every OS: at the bottom
  right of the screen, above the other windows, without taking the focus, with the progress, the
  files, the rate, the time left and Cancel, and the Receive offer above. A streaming paste Explorer
  shows itself is left out (no second dialog). Once done, a row offers *Open folder* (Windows) or
  says to paste again (Linux and macOS, whose pastes do not wait for long transfers), then goes
  after a few seconds (`src/agent/qml/TransferWindow.qml`).
- "Share clipboard" (Screens tab) turns it off per device, persisted in
  `<config dir>/settings.json`. Clipboard content is never written to disk.
- A device hands on what it adopted with its own offer: the sealed ciphertext (with the key only to
  devices it trusts), not files it has not transferred itself.

### Dragging files between devices

`src/core/filedrag.*` (service), `src/agent/filedragagent_win.cpp` and
`src/clipboard/virtualfiles_win.cpp` (Windows agent), `src/agent/filedragagent_x11.cpp` (Linux
X11), `src/agent/filedragagent_mac.mm` (macOS; written without a Mac, never run). Drag files and
folders in Explorer, a Linux file manager on X11 or Finder across to another device and drop them
as on that computer: into a folder window, a spot on the desktop, or an application that takes
files. Into a folder they are **moved**; an application that only reads them (a browser
uploading, a mail attaching) gets a **copy**. Dragging *from* a device needs its agent to catch the
drag (Windows, Linux X11, macOS); dragging *onto* any device works, shown as a real drag where the
agent runs native drags (the same three), and otherwise put into a folder of that device (below).

1. Crossing an edge with the left button held, the master holds the crossing back for a moment
   (`kDragCatchTimeoutMs`, 1.5 s) and asks the device the drag is on what is dragged. Its agent
   puts a small invisible drop target under the cursor: Explorer's drag hands it the paths (nothing
   is read), and the agent ends that drag without a drop. Without files (a window, a text
   selection) the crossing goes on as usual. With files, the source answers at once
   `drag.caught {pending}` and lists them (folders recursively, with the user's rights) on a thread
   of its own, showing "Preparing N items…" next to its cursor; the master then waits up to
   `kDragCatchExtendedMs` (10 s) in all, so a large tree is not lost to the deadline. (A master
   older than 0.1.20 is not sent `pending`, which it would take for "no files".) If the listing
   takes longer still, the crossing goes ahead without the files and the master says that nothing
   was moved (`InputController::dragCatchTimedOut`); a late answer is withdrawn (the master tells
   the source `drag.dropped` onto itself, which unpublishes the offer). A catch replaced by a newer
   one is never answered (`drag.caught` names no catch, so its answer would pass for the newer one's).
2. The source offers the files (FileTransfer) with their names and sizes. On whichever device the
   cursor is, if that device's agent runs native drags (capability `dnd` in the hello, see below),
   it runs a real drag (`SHDoDragDrop`, on a thread of its own, with the files' icon and count as
   the drag image), so Explorer and applications show and take it as a local one. Files from
   another device are virtual files (`FileGroupDescriptorW` + `FileContents`): nothing travels
   until the drop target reads them. Back on the source device, the originals are dragged and
   Windows moves them itself (nothing is downloaded from its own offer).
3. Releasing the button ends that drag:
   - **over a folder window or the desktop** (Explorer's `CabinetWClass`, `Progman`/`WorkerW`
     under the cursor, looking through the drag image), the agent ends the drag *without* a drop
     and hands the folder to the service, which transfers into it: first into a hidden
     `.deskflower-drop-<id>` folder there, then into place under free names ("name (2).ext"),
     nothing overwritten, with the transfer's progress and Cancel button shown. (Dropped as
     virtual files, Explorer would wait inside the drop for the whole transfer with nothing to
     show.) The drop goes into the folder the window shows, also when released over a sub-folder's
     icon;
   - **over anything else**, with a drop: the drop target reads the virtual files; the device
     transfers them then (into `<received folder>/Dragged/`, the last 3 kept) and serves them. The
     object offers `IDataObjectAsyncCapability`, so a target that supports it (the shell's folder
     views, file dialogs) extracts on a thread of its own with its own progress, and the drag's
     thread keeps its OLE apartment alive until the target ends the operation (up to 30 min).
4. The originals go to the source's **Recycle Bin** only on explicit confirmation that the files
   were kept: the service finished writing them into the folder (step 3, first case), or the drop
   target reported a move — `EndOperation` with success and `DROPEFFECT_MOVE` in async mode,
   otherwise "Performed DropEffect" (or "Paste Succeeded") with `DROPEFFECT_MOVE`, once the files
   were transferred. The virtual files prefer a move, which folder views honour. "Logical
   Performed DropEffect" is never taken as evidence: the shell sets it as it starts, before reading
   anything, even when the transfer then fails (`tests/tst_dragwin.cpp`). A drop target that read
   the files without reporting a move got a copy: the device answers `drag.result {ok, k}` and the
   source keeps the originals and says so (a source older than 0.1.20, which would recycle on
   `ok`, is told `ok: false` instead). A device older than 0.1.20 cannot say any of this: its
   `ok` is taken as a copy too (it sends it whenever the drop target read the files). If nothing
   took them, or anything failed, the originals stay and the user is told. Starting another drag
   does not forget one already released: its offer stays published, waiting for the result, for up
   to 2 hours (`kDroppedResultMs`); offers never released anywhere are withdrawn.

**A drag never stays on screen.** The device's drag does not follow its own mouse button (the
button is held on another computer, or was released there by the catch): it ends when the master
says (drop or cancel), and its capture window is fed the cursor's moves. It also ends there by
itself (cancelled; the release then takes nothing and the source is told) on Escape (typed at that
device or forwarded by the master), when the master is that device and its user's own button is
released (a low-level mouse hook) with no word from the service for 1.5 s, when the service has not
said `dragAlive` for 10 s (it does every 2 s while the agent shows a drag, and says so in
`dragHover {alive}`; with an older service, which says neither, this watchdog stays off), or when
the agent loses the service. On the service side, every drag shown or released on the device is
tracked by offer: a drop target reading the files is always answered — with an error at once, the
transfer cancelled — when the drag leaves the device, another one replaces it, or its master or
source disconnects. `dragHover` goes only to agents that declared `dnd`; if they all disconnect
before answering, it is answered at once as lost: the drag there is over, nothing is taken, and
both devices give a notice (also at the 60-minute timeout, never treated as "no agent").

**Devices without native drags** (a Wayland session, or no agent running). The agent declares
`dnd` in its IPC hello when it runs native drags (`FileDragAgent::supportsNativeDrag`: Windows,
Linux X11, macOS); the service then announces the capability to peers (`Node::setCapability`,
`cap` in the authenticated hello; a peer older than 0.1.20, which sends no list, is taken to have it
on Windows only). The master shows no drag on a device without it and, on release there, sends
`drag.drop`: the device puts the files into the folder its agent names for that point
(`folderAt`: the folder window or desktop under the cursor on Windows and macOS, the Downloads
folder on Linux), else into its Received folder (same staging and free names), with a notice saying
where. A device that was sent a drag but could not show it (an older master, an agent that answers
`unavailable` or none at all, a Mac whose agent lacks the Accessibility permission) saves into
Received on release.

**Linux X11** speaks XDND (version 5, as GTK and Qt do) on an X connection of the agent's own:
- catching: a 3×3 XDND-aware window is put under the cursor and the pointer nudged by one pixel
  (XTest), so the drag in progress enters it like any drop target; while it hovers there the files
  are read from `XdndSelection` as `text/uri-list` (INCR for long lists), and the local drag is
  cancelled with an Escape (XTest) instead of being dropped;
- showing a drag: the agent acts as an XDND source that follows the cursor (moved by the master):
  `XdndEnter`/`XdndPosition` to the XDND-aware window under it, one position at a time; on the
  master's drop, files from another device are **transferred first** (the target waits; progress
  in the agent), then `XdndDrop`, and the target reads their local paths (`text/uri-list` or
  `x-special/gnome-copied-files`); the originals of this device are offered as they are, with the
  move action (for files from another device too: the target moves the transferred copies). Only
  a move the target confirms (`XdndFinished` of version 5 with `XdndActionMove`) of files that were
  transferred lets the originals on the other device go; a target that copied them, an older one,
  or one that read the list without finishing, keeps a copy and the originals stay. Escape, the
  service's silence and the master's own released button end the drag as on Windows (polled:
  `XQueryKeymap`, `XQueryPointer`).
- the Recycle Bin is the freedesktop trash (`QFile::moveToTrash`); without a file list the folder
  is the Downloads folder (no file manager says which folder is under a point).

On macOS the agent does the same with AppKit (it needs the *Accessibility* permission for
DeskFlower itself, to post the mouse events; without it each attempt says so, nothing is caught,
and files dragged onto the Mac are saved into its Received folder):
- Catching: the drag pasteboard (`NSPasteboardNameDrag`) holds the file URLs of the drag in
  progress, if it was written since the left button last went down (a global mouse-down monitor
  notes its change count). The agent then shows a transparent window of its own, which takes no
  drop, under the cursor and releases the button there, so Finder's drag ends without moving
  anything.
- Showing a drag: a transparent window under the cursor gets a posted mouse-down and starts an
  `NSDraggingSession` with the originals (file URLs) or, for files from another device, one
  `NSFilePromiseProvider` per top-level file or folder. The window then moves out of the way, and
  the cursor moves the master makes are fed to the drag as mouse-dragged events. A drop is a posted
  release; a cancel is a release over our own window, which takes nothing. When Finder writes the
  promises (on a queue of their own) the service transfers everything once and each item is moved
  into place; promises all written into the target's folder are a confirmed move (the originals
  on the other device may go), anything less a copy or a failure. Escape (`CGEventSourceKeyState`),
  the service's silence and the loss of the service end the drag as on Windows; the master's own
  released button is not watched there.
- The folder under a point (the fallback without a list) is the desktop when no window is there,
  or the folder of Finder's front window when that window is under the cursor (asked through
  AppleScript, which macOS asks the user to allow once); the originals go to the Trash
  (`NSFileManager trashItemAtURL`).

**Wayland** gives a background program neither: starting a drag needs an input serial of a
focused surface of its own, and only the focused client sees a drag's data offer. The agent
therefore declares no `dnd`: nothing is caught there (the crossing goes on without files), and
the master sends files for it with `drag.drop` (above), which puts them into the Downloads folder.
Dragging files out of a Wayland device is not possible (nothing can be caught there); catching
through the agent's own window when it has the focus is not implemented.

Works with remote-view devices too. Without a file list (more than 2,000 entries) no drag is shown:
the files go straight into the folder window under the cursor, else into Received. Folders are
entries of their own (as for copied files: `dc`, `[path, -1]`), so dragged empty folders and
nested trees arrive whole, both ways; links inside a dragged folder stay behind, with a notice, and
a device older than 0.1.20 is shown the files alone. Refusals are not silent: dragging out of a
guest, a guest master's drags, and a crossing refused after the catch (the device declared this one
a guest meanwhile: the drag then ends at its source) each give a notice, at most once a minute.

**Drag diagnostics are always on**, whatever the logging setting: the service writes
`<config dir>/logs/drag.log` (categories `deskflower.drag` and `deskflower.transfer`) and the agent
`%LOCALAPPDATA%\Deskflower\deskflower-agent\logs\drag.log` (`deskflower.agent.drag`,
`deskflower.clipboard.virtual`): catch, listing time and counts, hover, release, folder hand-off,
transfer duration and result, the effect the drop target reported, the end of an async
extraction, the result sent to the source, recycling. Each file is rotated at 512 KB with one
older file kept, and like every log it carries no names or paths (`installDiagnosticLog`).

### Shared folder and phone access

- **Shared folder:** where phone uploads go, and what phones see. Files copied or dragged from
  paired devices are not there (see the received folder above). It is chosen
  in the agent (*Files → Change…*) and stored in `<config dir>/settings.json`; the default is
  `<user home>/Deskflower`. *Open* shows it in the file manager, and the tab shows its contents live.
  - Phones and paired computers put files there, so it must be an ordinary folder of the user
    (`platform::sharedFolderProblem`): never the whole home folder, a hidden folder, `AppData`
    (Windows; it holds the Startup folder) or `Library` (macOS), a whole drive or a system folder
    (Windows, Program Files, ProgramData, `/etc`, `/usr`...). The agent says why it refuses a
    folder and offers to choose another.
  - The Windows service runs as LocalSystem, so there the folder must also be inside the signed-in
    user's folder and reached without links or junctions (a stored folder that no longer qualifies
    falls back to the default). It is created with the user's rights. When nobody is signed in,
    phones get no files, and files copied or dragged from paired devices go to a folder only the
    service can reach (`ProgramData\Deskflower\Received`).
  - Received files are never written through a link or junction, and old clipboard and drag folders
    are deleted without following links (`src/core/fsutil.*`).
- **Phone access** (*Files* tab, off by default): an HTTPS web app on the local network
  (`src/web/`, port 24802; a small HTTP/1.1 server on Qt Network's `QSslServer`, because Qt HTTP
  Server is GPL-only). It lets a phone upload photos and files (camera included), browse and
  download the shared folder, and send text to the computer's clipboard.
  - *Upload files* opens the phone's file picker; *Take a photo* and *Record a video* open the
    camera directly (one kind per button: with several in `accept`, phones show the gallery).
  - The certificate is self-signed: EC P-256, generated with OpenSSL, with the LAN addresses in
    its subjectAltName, and kept for 825 days. Phones warn about it once.
  - Access: the QR code carries a single-use token valid for 10 minutes; a used or expired code
    is replaced by a new one on screen. The token is exchanged for an `HttpOnly; Secure;
    SameSite=Lax` session cookie (12 h; Lax because the scan opens the browser from the camera app,
    a cross-site navigation). *Disconnect phones* revokes every session.
  - A session belongs to the Windows user who scanned the code: it is recorded with that user's
    SID and refused (401) while another user is at the console, and every session is revoked when
    the console user changes, so a phone granted by one user never reads or writes the next
    user's folder on a shared PC (`tests/tst_phoneuser.cpp`). Linux and macOS run one service per
    user, so this does not apply there.
  - The QR uses the address of the interface that has a default gateway, so it is not the address
    of a host-only or virtual adapter.
  - Uploads go in chunks of up to 8 MB to a hidden temporary file, can resume, and never
    overwrite an existing file. Upload names and folders with spaces are sent percent-encoded
    (`encodeURIComponent`), so a photo named `IMG 1.jpg` keeps its name and uploads into a folder
    called `My docs` work. Downloads are streamed from disk.
  - Downloads honour a single byte range (`206 Partial Content`, `Content-Range`,
    `Accept-Ranges: bytes`), so videos and audio play and seek in the phone's browser (iOS
    Safari probes with `Range: bytes=0-1` and refuses to play otherwise). A multi-range request
    gets the whole file (no amplification), a range past the end `416`.
  - The page shows the folder live: while it is visible it asks for the listing every 2 s, and the
    computer answers 304 (ETag) when nothing changed.
  - Every path resolves inside the shared folder, symbolic links included, and the Windows service
    reads and writes the folder **as the signed-in user** (impersonation), never as LocalSystem:
    a link inside the folder cannot reach anything that user could not.
    Looking up who is signed in needs the service's own rights, so those lookups step out of the
    impersonation for a moment (`platform_win.cpp`, `AsService`): they only read the user's token
    (SID and profile folder), no file is touched with those rights. Nested impersonations restore
    whatever the thread had before (`tests/tst_platform.cpp`).
  - Only photos, video, audio, plain text and PDF open in the phone's browser (sandboxed). Anything
    else (HTML, SVG, scripts, programs...) is always sent as a download, never rendered, so a file in
    the folder cannot run as the site with the phone's session; the page asks before downloading it.
  - Strict CSP (no inline code), `Referrer-Policy: no-referrer`, `nosniff`, no framing.

## Building

Windows (Visual Studio 2022 + Qt 6.11 in `C:\Qt`). libsodium is downloaded automatically:

```bat
scripts\build-windows.bat win-debug
ctest --test-dir build\win-debug
```

Besides the library tests, `tst_e2e` starts real `deskflower-service` processes (the ones built
next to the tests) and drives them over their IPC as the agent and the session helper do: pairing,
the cursor crossing, the sealed clipboard (text, files and folders), drags, restarts, a group of
three and the same behind a simulated NAT (a port-forwarding router on loopback, for UDP and the
TCP file streams). On Windows `tst_clipboardbridge` pastes the agent's clipboard objects into a
folder through the shell, and `tst_dragwin` drops the drag's virtual files on the shell's own drop
target and copy engine; on macOS `tst_macclipboard` reads the pasteboard; `tst_servicecontroller`
drives a service controller over its local IPC. Every test listens on 127.0.0.1 only
(`DESKFLOWER_BIND_ADDRESS`, set by ctest and honoured by `Node` and the phone server), and the
cases that touch the real desktop or profile (the clipboard, the shell, a possible dialog, folders
in the home folder; ctest label `desktop`) skip unless `DESKFLOWER_DESKTOP_TESTS=1`, so `ctest` is
safe on the machine you are working on. What they cover, how to run them by hand,
the desktop tests and the manual checklist (Windows Sandbox, Hyper-V NAT, Docker) are in
[TESTING.md](TESTING.md).

Build options (CMake cache variables, `-D<name>=<value>` at configure time):

| Option | Default | What it does |
| --- | --- | --- |
| `DESKFLOWER_UPDATE_PUBKEY` | empty | Ed25519 public key for signed updates from paired devices (see *Signed updates from paired devices*); empty: disabled. |
| `DESKFLOWER_KEY_DIAGNOSTICS` | `OFF` | Builds the keyboard diagnostics (see *Keyboard and mouse*), a troubleshooting tool that records the keys going between devices. Off: no UI, its agent, helper and peer messages ignored, `tst_keydiag` not built. |

Dependencies: Qt ≥ 6.8, libsodium and OpenSSL 3. Linux also needs
X11 (Xi, Xtst, Xrandr, Xfixes, Xext) and, for Wayland, Qt D-Bus, libei and PipeWire. On Windows,
libsodium is downloaded automatically and OpenSSL comes from the Qt installer
(`C:\Qt\Tools\OpenSSLv3`; its DLLs are copied next to the executables).

Linux: Qt 6.8 or newer (distributions such as Ubuntu 24.04 ship older versions: use the Qt
online installer or `aqtinstall`), then for example on Debian/Ubuntu:
`libsodium-dev libssl-dev libx11-dev libxi-dev libxtst-dev libxrandr-dev libxfixes-dev libxext-dev libei-dev libpipewire-0.3-dev`.
macOS: Qt 6.8+, `brew install libsodium openssl@3`. Then:

```sh
cmake --preset unix-debug -DCMAKE_PREFIX_PATH=/path/to/Qt/6.x/gcc_64
cmake --build --preset unix-debug
```

### Linux tests in Docker

`tests/docker/` builds and tests the tree on Ubuntu 24.04 with the CI's packages and Qt 6.11.2
(installed unattended with `aqtinstall`), from any machine with Docker (Docker Desktop on Windows):

```sh
tests/docker/run.sh                      # build + the whole suite
tests/docker/run.sh -R 'tst_x11|tst_portal'   # arguments go to ctest
BUILD_ONLY=1 tests/docker/run.sh         # compile only
```

The source is mounted read-only; the build lives in the Docker volume `deskflower-linux-build`
(incremental; `docker volume rm deskflower-linux-build` starts clean). The suite runs inside
`dbus-run-session` and `xvfb-run` with `QT_QPA_PLATFORM=offscreen`, as in the CI: `tst_x11` opens
the Xvfb display itself (input backend grabs, early capture, local buttons, XShm capture, the
clipboard as `xclip` sees it, the helper's paste of copied files against a fake service, XDND both
ways), and `tst_portal` runs a fake xdg-desktop-portal on its own thread and bus connection
(session, restore token, refusal, closing, a session without input, clipboard in both
directions, a copy elsewhere during a sealed paste). Both grab, inject, write the clipboard or
claim the portal's bus name, so they run only where `DESKFLOWER_PRIVATE_SESSION=1` says the display
and the bus are the run's own (`build-and-test.sh` and the CI set it; `DESKFLOWER_DESKTOP_TESTS=1`
also enables them), carry the ctest label `desktop`, and skip themselves without a display or a
bus. `tst_sessionlauncher` (Linux, macOS) runs the service's `SessionLauncher` with the test
program as the helper (no restart for the same session, restart for another, backoff). Not covered: a real compositor (GNOME/KDE portals, libei, PipeWire
frames), real file managers, and drags between real applications.

## Packaging (self-contained folder)

```bat
:: Release build + Qt libraries, QML modules, plugins and the MSVC runtime in dist\DeskFlower\bin
scripts\package-windows.bat [destination]
```

On other platforms: `cmake --install build/<preset> --prefix <dir>` (Linux: Qt's deploy scripts;
macOS: `DeskFlower.app`, deployed and signed as described under *Installers*).

## Application icon

A desktop monitor with a flower on its screen. The sources are hand-written SVGs in `resources/icons`:
`deskflower.svg` (64 px and up) and `deskflower-small.svg` (a simplified drawing on a 16 px grid, used up to
48 px, which also covers the tray). `python scripts/make-icons.py` (Pillow only) rasterizes them into
`png/deskflower-<size>.png`, `deskflower.ico` and `deskflower.icns`; the generated files are checked in.
They are used for the Windows executables (`resources/deskflower.rc`), the agent window and tray icon
(Qt resource), the MSI (shortcut and *Apps & features*), the Linux hicolor theme (`Icon=deskflower`) and
the macOS bundles (`CFBundleIconFile`).

## Installers

| Platform | Package | Built by | Status |
|---|---|---|---|
| Windows | MSI (WiX 3, downloaded into `build/tools`) | `scripts\package-windows.bat` → `dist\Deskflower-<version>-Windows-x64.msi` | Built and installed |
| Linux | DEB, self-contained in `/opt/deskflower` | `cpack -G DEB` in the build directory; CI → `deskflower_<version>-Linux-amd64.deb` | Built by CI, not tried on a real machine |
| macOS | DMG with `DeskFlower.app` (every program inside, signed ad hoc) | `cpack -G DragNDrop`; CI → `deskflower-<version>-macOS-arm64.dmg` | Written without a Mac: built and checked by CI only, never run |

**Web page** (`docs/index.html`, GitHub Pages: *Settings → Pages → Deploy from a branch*, `main`,
`/docs`): the feature guide, with screenshots taken from two demo instances ("Office PC" and
"Laptop") and made-up screen previews. Set `REPO` in its script to the published repository.

**CI** (`.github/workflows/build.yml`, GitHub Actions): on every push to `main`, pull request, `v*`
tag or manual run, it builds on Ubuntu 24.04 and macOS 14 (Apple silicon) with Qt 6.11.2, runs the
tests (offscreen) and packages the DEB and the DMG, kept as artifacts of the run. Windows is not in
CI: its MSI is signed with the update key, which never leaves this machine.

The Windows MSI (per-machine):

- installs to `Program Files\DeskFlower`;
- registers and starts the service through `deskflower-service --install` (silent custom action,
  idempotent for upgrades), and removes it on uninstall;
- adds the firewall rules and a Start menu shortcut. The service writes the agent's autostart
  entry (`--minimized`: system tray only) according to *⋮ → Start DeskFlower when I sign in*, so an
  upgrade keeps that choice, and starts the agent again after every installation or upgrade;
- keeps `%ProgramData%\DeskFlower` (identity and paired devices) on uninstall;
- when its signed manifest sits next to it, keeps a copy of itself so that this device can update
  its paired devices ([Signed updates](#signed-updates-from-paired-devices)).

The agent lives in the system tray: closing its window hides it. The tray menu has *Open*,
*Share this keyboard and mouse* and *Quit DeskFlower*. Quitting stops the service too (every
connection, phone access, the session helper) until DeskFlower is opened again (the agent starts
the service when it finds it stopped) or the computer restarts. While the service is not running
the window shows a *Start* button. Signed-in users may start the service (not stop it): `--install`
adds that right to the service's permissions.

The DEB enables the `systemd --user` service for every user and starts the agent at logon (XDG
autostart); the agent gives systemd the session's display variables before starting the service
(see [Architecture](#architecture)).

The macOS DMG holds `DeskFlower.app` (`cmake/MacBundle.cmake`): the agent is the bundle's program,
the service sits next to it (`Contents/MacOS/deskflower-service`) and the session helper is a bundle
of its own inside (`Contents/Helpers/deskflower-session.app`, so that macOS grants it its own
privacy permissions). The installation runs `macdeployqt` once for the three (Qt, libsodium,
libcrypto), points the helper at the shared `Contents/Frameworks` (and writes its `qt.conf`), adds
libssl (Qt's OpenSSL backend loads it by name at run time: the service's LaunchAgent sets
`DYLD_FALLBACK_LIBRARY_PATH` to `Contents/Frameworks`), fails if anything still points to Homebrew or
the Qt used to build, and signs the whole bundle ad hoc. CI also checks with `codesign --verify`,
`otool -L` and `DYLD_PRINT_LIBRARIES` that the service and the helper load nothing from outside the
bundle. Copy `DeskFlower.app` to *Applications* and open it: there is no installer, so the agent
writes the service's LaunchAgent (`deskflower-service --install`) the first time, and again when the
bundle has moved, then starts it. The agent's own start at sign-in is a second LaunchAgent
(`com.deskflower.agent`, `--minimized`) that the service writes or removes according to *⋮ → Start
DeskFlower when I sign in* (only a service inside an installed `DeskFlower.app`, default instance:
one run from a build tree, a test or `--instance`/`--config-dir` never touches it). Both are shown under DeskFlower in *System Settings → General → Login
Items* (`AssociatedBundleIdentifiers`).
Not notarized: the first time, macOS refuses to open an app from the internet that is not
notarized; *System Settings → Privacy & Security → Open Anyway* (or right-click → *Open*) allows it.
An ad hoc signature changes with every build, so macOS may ask for the privacy permissions again
after an update.

Manual checks still needed on a Mac (nothing of the macOS code has run yet): a clean account
without Homebrew; the DMG opens and the service and helper start (`~/Library/Logs/Deskflower`);
the Accessibility / Input Monitoring / Screen Recording banner and its button; capture and
injection, with the cursor hidden on both sides and modifiers held while crossing; Secure Keyboard
Entry in Terminal shows the warning; Ctrl acting as Command; text, HTML, image, files and folders
(empty ones too) copied both ways, with a transfer longer than 8 s (paste again); dragging files and
folders both ways (Finder → PC, PC → Finder, desktop, and an application taking files), dropping on
nothing, and the Trash after a move; thumbnails and remote view (ScreenCaptureKit, macOS 12.3+ and
14+); the agent starting at sign-in; and the phone server's TLS (bundled libssl).

## Trust levels

When pairing, each side chooses what the other device becomes on it: *Pair* in the *Devices* tab
asks before sending the request, and the dialog that shows the PIN asks the answering side. The
request carries the requester's choice as a suggestion (`tl`), which that dialog shows preselected
when it is *Guest* or *This device only*. *All my devices* is never applied on the requester's
word: the dialog offers *This device only* and points at the request ("A asks to join *All my
devices*... choose it only if A is one of yours"), and the group forms only when the answering
user picks it (a group member can introduce devices to every member, so a stranger's device
must not get there because nobody looked at the dialog; `Node::pairingSuggestion`). A device
that is a member already stays one. The suggestion never lowers what a device already paired
there had, and never promotes a device the answering side made a guest (it stays *Guest*);
what the dialog shows when the PIN is typed
is what applies, and a choice sent after the pairing ended is ignored rather than kept for the
next one (`Node::setPairTrust`). After pairing, if the other side's first `hello` declares less
than this side chose, a notice says so (for example "B chose *This device only* for this
device, not *All my devices*: choose *All my devices* on B too"). The *Trust* selector of a
paired device changes it later and explains each level when hovered; switching to *Guest* or to
*All my devices* asks for confirmation first, saying what changes. Guests either way (a guest
here, or a device that treats this one as a guest) are marked on the *Screens* tab and listed in
the tray icon's tooltip while connected. The choice is stored in `peers.json`
(`"guest": true`, `"group": true`, or neither for *This device only*) and applies on this device
only; the other device chooses on its own side what this one may do there.

- **Guest**, for a device that is not yours.
  - Its keyboard and mouse never drive this device: crossing into it is refused (as for a device
    without a session), so its cursor stays home. Since the level is declared in the `hello`,
    the other device knows and does not even try: this device leaves its edge map, so its cursor
    is not captured at the edge and thrown back.
  - It cannot change the screen arrangement or who shares the keyboard and mouse here (it still
    receives them).
  - It never views this screen nor gets its thumbnails, whatever the remote view grant says.
  - It gets this device's clipboard (sealed) when the cursor crosses to it, but can read it only
    when the user **pastes on it with the keyboard** (Ctrl+V, Shift+Insert, Ctrl+Shift+V, Cmd+V):
    a context-menu or Edit → Paste cannot, by design, so a guest never reads the clipboard just by
    receiving it; of copied files it learns only the count and size until it pastes them, not the
    names. What the guest copies still comes here when the cursor does.
  - Files reach it only when they are dropped on its screen, or when the user pastes copied files
    on it with the keyboard: it may fetch nothing else this device offers.
- **This device only**: one of your devices, trusted by this one; every pair of devices needs its
  own PIN. Devices paired before trust levels existed are this. Like *All my devices*, it shares
  the clipboard with this device on every copy, and pasting there just pastes (see
  [Clipboard](#clipboard)).
- **All my devices**: one of your devices, trusted by every device of your group, and it trusts
  them. Each device declares the level it gave the other in the authenticated `hello`, and the
  pair shares the **stricter** of the two: the group only forms between devices that both chose
  it. A device that chose *Guest* or *This device only* for you is not in your group, whatever
  you chose for it, and still gets on this device exactly what you chose for it here; the
  *Devices* tab says so next to the selector, once the device has connected since the service
  started (what it declared is remembered until the pairing ends, not across restarts).

  When two devices share the group level, each sends the other its list of group members (their
  public keys, names, last address and every endpoint known for them with its source, kept as
  group hints; `group.members`) and tells those members about it
  (`group.introduce`), on every `hello` (connection, rename, level change), so one PIN pairs a new
  device with the whole group. Introductions are not forwarded: a device learns the members of
  the device it paired with, and since those members then introduce it in turn to their own
  members, every member ends up storing every other, so the group still converges. An
  introduction never changes a record that exists already (a device paired with a PIN keeps its
  level: a guest is never promoted) and never brings back a device that was forgotten: *Forget*
  (which asks first and says so) on either side writes the device's id and name to
  `forgotten.json` (shared by every user of the device, like `peers.json`), and only pairing it
  again with a PIN, or *Allow again* in the *Forgotten devices* list of the *Devices* tab, clears
  that (*Allow again* pairs nothing: it lets the group members introduce it again, and the
  connected ones are asked to list their members at once). When a member introduces a forgotten
  device, a notice says so once ("B wanted to add X, but you forgot X on this device"). Pairing
  again with a device that is still stored offers the requester's suggestion in the PIN dialog
  but never less than its current level, a guest stays a guest, and *All my devices* is only
  pointed at unless the device is a member already. A member's list is taken up to 64 entries, and an address hint only when it is a
  unicast address. Demoting a member stops this device introducing it; the
  other members keep what they stored, and keep introducing it among themselves, until they
  forget it too. On the Windows service, introduced devices belong to the
  user signed in when the introduction arrives, like pairings, and only that user's members are
  introduced.

  **Mind what this extends:** a PIN pairing as *All my devices* gives every member of the group
  full control of the new device (keyboard, mouse, clipboard, files, screen requests) and the
  new device the same over all of them, and a compromised member can introduce any device it
  likes to every other member. Use it for devices you own; a borrowed or shared device is a
  guest.

## Signed updates from paired devices

When a paired device runs a newer DeskFlower for the same platform, its row on the **Devices** tab
says "DeskFlower 0.2.0 is available from this device" with an **Update** button. Nothing is
installed without that click. Updates do not depend on the transfer protocol (they have their own
frames, `src/core/updater.h`): a device too old to transfer files with this one is still offered
and sent the update, and the protocol banner (see [Architecture](#architecture)) points to that
Update button or offers it. The package is checked before it is installed; the device that sends
it asks nothing (it hands out a signed public package), but only to paired devices, one transfer at
a time and at most one request per device every 10 seconds.

**Release format.** The installer package (the MSI) plus `<package>.manifest`, a small JSON file
(product, version, platform such as `windows-x86_64`, file name, size, SHA-256), and
`<package>.manifest.sig`, an Ed25519 signature of the manifest's exact bytes (hexadecimal text). The
receiving device installs the package only if:

- the signature verifies with the public key compiled into it (`DESKFLOWER_UPDATE_PUBKEY`);
- the product is DeskFlower, the platform is its own and the version is strictly newer (dotted
  numbers only: downgrades, the same version and pre-release tags are refused);
- the size is at most 512 MB and the SHA-256 of the received bytes matches the manifest.

Builds without `DESKFLOWER_UPDATE_PUBKEY` never accept a package (the agent says updates are not
configured); there is no override.

**Transfer and installation** (`src/core/updater.*`). Devices announce their version and the package
they can serve in the authenticated `hello`. The package travels over the encrypted TCP stream
(`SecureStream`) into `%ProgramData%\DeskFlower\updates\incoming` (SYSTEM and Administrators only).
Signature, platform, version and size are checked from the manifest before any data is accepted.
After the download the service reopens the file read-only with a sharing mode that refuses writers,
renames and deletes, hashes those exact bytes again and starts
`%SystemRoot%\System32\msiexec.exe /i <msi> /qn /norestart` (with logging on, its log goes to
`%ProgramData%\DeskFlower\logs\update-install.log`) while it still holds the file. msiexec is a
separate process, so it carries on when the MSI stops the service; `MajorUpgrade` removes the old
version and the new service starts at the end. Until then no other update starts and the file stays
locked; if msiexec fails, the agent shows its error code and the file is deleted. Agents are asked to
quit first (msiexec in session 0 cannot close them), and the new service starts the agent again in the
console session; a failed upgrade rolls back and registers the old service again. The service refuses
to run if `%ProgramData%\DeskFlower` is a link or is owned by another account than SYSTEM or
Administrators, and never follows links inside the update store. Installing needs the service running as LocalSystem:
in console mode, and on Linux and macOS for now, the agent reports that updating is not supported.

**Which package a device serves.** An MSI cannot contain itself, so the service keeps a copy. The
MSI runs `deskflower-service --import-update "<the MSI being installed>"`. When a signed manifest for
exactly the version being installed sits next to the MSI, the service checks it and copies the
three files into `%ProgramData%\DeskFlower\updates\package`. A device installed from a signed
`dist\` MSI can therefore update its peers, and a device updated by a peer keeps that package and
passes it on, since the downloaded manifest is saved next to the downloaded MSI. Without the
manifest (unsigned builds), or for a repair from the Windows Installer cache, nothing is copied and
the installation goes on as usual.

**The signing key.** `scripts\package-windows.bat` handles it. On the first run it creates the key
pair in `release-signing-key\` at the repository root, writes a `README-BACKUP.txt` next to it and
ends with a banner asking you to back the folder up:

```
release-signing-key\update-signing.key   secret: only your account can read it
release-signing-key\update-signing.pub   public key (64 hex characters)
```

The folder is git-ignored and never packaged. **Copy it somewhere safe**: every later package run
signs with it, and installed devices only accept updates signed with the key they were built with.
To package on another machine, copy the folder to the same place (or point `DESKFLOWER_SIGNING_DIR`
at it). If it is lost, the next run creates a new key and every device has to be reinstalled by
hand once.

Each run builds the release with `DESKFLOWER_UPDATE_PUBKEY` set to the public key, creates the MSI
and writes `dist\Deskflower-<version>-Windows-x64.msi.manifest` and `.manifest.sig` next to it, checking the
signature against the key compiled into that build. Elsewhere, configure with
`-DDESKFLOWER_UPDATE_PUBKEY=<hex>` (or the environment variable of the same name). The tool also
works by hand:

```bat
deskflower-sign manifest <package> <version> [platform]   :: writes <package>.manifest
deskflower-sign sign <secret-key-file> <file>              :: writes <file>.sig
deskflower-sign verify <public-key-hex|--compiled> <file> <file>.sig
```

Keep the secret key backed up and private: whoever holds it can push packages to every device.
**If it is lost or leaked**, create a new key pair, build with the new public key and install that
version on every device by hand. Devices built with the old key reject packages signed with the
new one, and the other way round.

## Running as a system service

```bat
:: Windows, from an elevated prompt: registers "Deskflower" (LocalSystem, automatic start,
:: restart on failure), adds inbound firewall rules (private/domain networks, and public ones
:: for the local subnet only, or other private networks too with "Allow devices on other
:: networks") and starts it.
deskflower-service --install
deskflower-service --uninstall
```

Install from a packaged folder (above): the service runs as LocalSystem, which does not have the
Qt development directories on its `PATH`.

| Platform | Mechanism | Configuration and logs |
|---|---|---|
| Windows | Service Control Manager, `--service` entry point | `%ProgramData%\DeskFlower` (SYSTEM and Administrators only) |
| Linux | `systemd --user` unit written by `--install` | `~/.config/Deskflower/deskflower` |
| macOS | LaunchAgent written by `--install` (the agent does it the first time) | `~/Library/Preferences/Deskflower/deskflower`, `~/Library/Logs/Deskflower` |

On Windows the agent runs as the logged-in user while the service runs as LocalSystem. The IPC pipe
is therefore open to local users, and the service only accepts clients from the active console
session, re-checked on every session change (a client left behind by a fast user switch is
dropped). Any process in that session, not only the agent, can give the service the commands the
agent gives it (including starting a signed update offered by a paired device, or stopping the
service); files are only ever read and written with that user's rights. Without any flag the
service runs in the foreground (console mode) with per-user configuration, which is the mode used
for development.

## Running two instances on one machine

```bat
set PATH=C:\Qt\6.11.2\msvc2022_64\bin;%PATH%
build\win-debug\deskflower-service --instance a --port 24851
build\win-debug\deskflower-service --instance b --port 24852
build\win-debug\deskflower-agent --instance a
build\win-debug\deskflower-agent --instance b
```

Two instances on one machine share a single desktop, so only one of them should run the session
helper: start the other with `--no-session`. To run the helper by hand (for example under a
debugger), start the service with `--no-session` and `DESKFLOWER_SESSION_TOKEN=<secret>`, then
start `deskflower-session --instance <name>` with the same variable. They also share the user's
folders: `--home <dir>` (console mode only) makes an instance keep its shared and received files
in `<dir>/Deskflower` and `<dir>/Received` instead (the end-to-end tests use it so that they never
touch, or prune, the real user's received files). `--version` and `--help` print and exit, also
when started without a console (never a message box that would wait for a click).

In agent A, click **Pair** next to B and type the PIN that B displays. Then open the **Screens**
tab (or start the agent with `--page screens`) to arrange both devices.

**Problems** are always recorded, whatever the logging setting: `problems.log` (in
`<config dir>/logs` for the service, readable by SYSTEM and Administrators only on Windows; in the
agent's local application data, `%LOCALAPPDATA%\Deskflower\deskflower-agent\logs` on Windows,
for the agent; `<config dir>/logs/session` for the session helper, which the service starts
with `--crash-dir`) gets a line when the process starts, one when it exits normally, and one for each
`qFatal`, `std::terminate` (with the exception's message), abort, unhandled exception (Windows:
code, module and offset) or fatal signal (Unix); a start with no exit before the next start is an
end that nothing could report. On Windows the crash also leaves a minidump next to it
(`service-<date>-<time>.dmp`, `agent-…`, `session-…`: thread stacks and modules, no heap; the
newest 3 are kept). Crashes inside the event loop are covered too, in slots run by Qt timers and
in Win32 timer callbacks, whose exceptions Windows would otherwise swallow (that suppression is
turned off, `UOI_TIMERPROC_EXCEPTION_SUPPRESSION`, so such a crash ends the process with its
record instead of leaving it running with corrupt state). The record of an
unhandled exception is formatted on a thread of its own, so a stack overflow is recorded too. `problems.log` moves to `problems.log.1` past 256 KB (`src/core/crashreport.*`). It holds
no keys, clipboard contents or file names.

**Logs** are off by default. Turn them on by hand with `"logging": true` in
`<config dir>/settings.json` (there is no option in the agent) and restart the service: it then
writes `<config dir>/logs/service.log`, the session helper `session.log` and updates
`update-install.log`. With logging off those files are deleted at start. Log files never get debug
messages, and no message carries typed keys, clipboard contents, file names or paths: they only say
what happened (connections, crossings, errors). In console mode every message also goes to stderr.
The one exception to "off by default" is the small drag diagnostics log (`drag.log`, see
[Dragging files between devices](#dragging-files-between-devices)), kept by the service and the
agent at all times under the same rules.

Ports: UDP 24800 (discovery via multicast `239.255.77.100` and broadcast), UDP 24801 (sessions),
TCP 24801 (file transfers) and TCP 24802 (phone access over HTTPS, only while enabled).
All three can be changed in the agent (*⋮ → Advanced settings…*, a window of its own; or
`deskflower-agent --page settings`). They are stored as `dataPort`, `discoveryPort` and `phonePort`
in `<config dir>/settings.json` and used from the service's next start (the window offers *Restart
service*); `--port` on the service command line overrides `dataPort`. The discovery port must be
the same on every device; the data port is learned from beacons (and from the `hello`). The
firewall rules allow the program, not fixed ports (see [Networks, routers and
NAT](#networks-routers-and-nat) for other networks).
The service adds its firewall rules when installed; run from a console, Windows asks for
permission on first run.
