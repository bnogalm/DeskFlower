# DeskFlower

Shared keyboard, mouse and clipboard across several computers on the LAN, with PIN pairing and
encrypted UDP traffic. Qt 6 (C++20) for Windows, Linux and macOS.

## License

DeskFlower is licensed under the [PolyForm Noncommercial License 1.0.0](LICENSE.md): free for
personal use and for noncommercial organizations. Any work use (companies of any size, freelancers,
self-employed) needs a [commercial license](COMMERCIAL-LICENSE.md): **5 € per user, once, for
life**, VAT included, paid at <https://ko-fi.com/bnogal> (write the number of users and your billing
country in the message). Full invoices: ask at vegawallet@gmail.com with the Ko-fi receipt.

Third-party software (Qt under the LGPL v3, OpenSSL, libsodium, QR Code generator) is listed in
[THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md), with the license texts in `licenses/`. The
agent's *About* window shows all of them, and the installers put them next to the programs.

Only Qt modules available under the LGPL may be shipped: the packaging excludes the GPL-only ones
that `windeployqt` would otherwise deploy and fails if any is found (`CMakeLists.txt`,
`DESKFLOWER_DEPLOY_OPTIONS`).

## Architecture

| Process | Role |
|---|---|
| `deskflower-service` | Identity, discovery, pairing, encrypted sessions, workspace replication and input routing |
| `deskflower-session` | Started by the service inside the interactive session: reports monitors, captures thumbnails, captures and injects keyboard/mouse |
| `deskflower-agent` | QML user interface only; controls the service over local IPC (`QLocalSocket` + CBOR) |

The session helper authenticates to the service with a secret passed in its environment
(`DESKFLOWER_SESSION_TOKEN`). It exits when it loses the service, which starts a fresh one.

### Security

- **Identity:** every device has an X25519 key pair; `deviceId` = BLAKE2b(public key).
- **Pairing:** 6-digit PIN with **CPace** (PAKE over ristretto255).
  - The PIN is single use, and every request gets a new one.
  - Guessing: after 3 attempts that do not end in pairing (wrong PIN, abandoned, timed out; not the
    user cancelling) the device refuses pairing requests for 5 minutes, and after the first failure
    it answers each guess only after 300 ms (`src/core/pairing.*`).
  - The channel identifier binds both public keys, so a man in the middle cannot swap them.
- **Sessions:** `Noise_IK_25519_ChaChaPoly_SHA256` handshake.
  - AEAD packets with an explicit counter and an anti-replay window.
  - Rekey every 2 minutes.
  - When a peer restarts, the reliable channel is resynchronised through a boot id.
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
- **Pairings belong to a user:** the Windows service serves every user of the computer, so a
  pairing is recorded with the SID of the user signed in at the console when it was made, and the
  peer is reachable (sessions, messages, streams, input) only while that user is the one signed in
  there, their lock screen included. On the sign-in screen, or in another user's session, the peer
  is not answered and does not appear in the agent. Pairings made before 0.1.16 go to the first
  user seen. Linux and macOS run one service per user, so this does not apply there.
- **Unauthenticated traffic is bounded**, so that flooding the LAN costs little:
  - Rate limits on beacons, handshake initiations (two key exchanges each before the sender is
    known) and pairing packets; a beacon query is answered at most twice a second per sender, so
    forged source addresses cannot turn a device into a reflector.
  - At most 64 unknown devices are kept from beacons. Beacons never rename a paired device (its
    name comes from the authenticated `hello`).
  - At most 16 TCP connections waiting to authenticate, 32 streams per paired device, and streams
    quiet in both directions for 5 minutes are closed. Reliable messages larger than 8 MB are
    dropped (bulk data travels over TCP).
  - Phone access: request headers must arrive within 15 s, at most 16 connections per address
    (64 in total) and 16 TLS handshakes at once, and a request body is read only when the request
    carries a valid session.

- **Updates from paired devices:** Ed25519-signed manifests checked against a public key compiled
  into the build (see [Signed updates](#signed-updates-from-paired-devices)).

Relevant code in `src/core/`: `noise.*`, `cpace.*`, `pairing.*`, `session.*`, `reliable.*`, `node.*`.

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
all connected peers, so concurrent edits converge. They are persisted in
`<config dir>/workspace.json`, together with a cache of remote screens so offline devices can still
be arranged.

### Keyboard and mouse

- The master's helper reports when the cursor pushes against an outer edge of its desktop. The
  service maps the point through the layout (`src/core/router.*`) and, if a connected device is
  there, starts capturing: the local cursor is parked and hidden, and input is swallowed by
  low-level hooks.
- The master keeps a virtual cursor in the target's native coordinates and sends absolute
  positions as unreliable datagrams (latest wins), so pointer acceleration is applied only once.
  Buttons, wheel and keys use the reliable channel; clicks carry the cursor position.
- Keys travel as PS/2 set-1 scancodes (`0xE0xx` for extended keys), so each device applies its own
  keyboard layout. A key or button pressed before the cursor left is released locally.
- Leaving a device releases everything injected there. A device only obeys the current master.
  If the master disconnects, the device releases its keys. If the helper stops receiving the
  service's heartbeat for 3 s, it gives the input back.
- Keys go only to the device the cursor is on. When the master locks, signs out or switches
  sessions while the cursor is elsewhere, the cursor comes back to it first
  (`InputController::returnHome`), so what is typed on its lock screen (the password) stays there.
- As LocalSystem the helper's input thread follows the input desktop (Default, Winlogon), so it can
  also work on the lock screen and UAC prompts.
- Platform backends (`src/session/inputbackend_*.cpp`, key tables in `src/core/keymap.*`):

  | Platform | Capture | Injection | Status |
  |---|---|---|---|
  | Windows | Low-level hooks | `SetCursorPos` + `SendInput` | Built and tested |
  | Linux X11 | XInput2 raw events + pointer/keyboard grab | XTest | **Written, never compiled** |
  | Linux Wayland | InputCapture portal (pointer barriers) + libei receiver | RemoteDesktop portal (liboeffis) + libei sender | **Written, never compiled** |
  | macOS | `CGEventTap` + `CGAssociateMouseAndMouseCursorPosition` | `CGEventPost` (flags, click counts) | **Written, never compiled** |

  The key translation tables (evdev and macOS virtual key codes to and from set 1) are unit tested.
  The Linux and macOS backends still need to be built and tried on real machines.

- **Linux:**
  - X11 uses root window pixels from XRandR.
  - Wayland uses logical coordinates and needs a compositor with InputCapture (GNOME 45+,
    KDE Plasma 6.1+). The desktop asks for permission to control input the first time.
  - Without libei/liboeffis at build time, Wayland sessions get no keyboard/mouse sharing; the
    rest still works.
- **macOS:**
  - Allow `deskflower-session` in *System Settings → Privacy & Security → Accessibility*, and in
    *Screen Recording* for the screen previews.
  - The helper is an `LSUIElement` app bundle, so it has no Dock icon.
  - A background process may not be able to hide the cursor while capturing (macOS limitation);
    the cursor stays frozen in place.

### Remote view

A paired device can be flagged *Remote view* in the *Screens* tab. Crossing into it no longer
moves the cursor to its own screens. Its **screens are shown full screen, one at a time, on the
local monitor** where the edge was crossed (the primary one first), and you control it as in a
remote desktop session.

- **Cursor:** it enters at the same relative position along the edge. Pushing it past an edge of the
  shown screen towards another screen of that device switches the view to that screen (same stream,
  no new approval: the source checks that it is one of its own screens). Leaving through the side
  it came in, where no other remote screen lies, returns home; other edges are blocked. The viewer
  draws the remote cursor.
- **Video** (`src/view/framecodec.*`, `src/core/remoteview.*`):
  - The screen is cut into 128-pixel tiles and only the tiles that changed are sent, as JPEG.
  - Frames are scaled to the viewer's monitor and paced by its acknowledgements (at most ~30 per
    second, never more than one frame in flight). A full frame is sent now and then.
  - They travel over the encrypted TCP stream. The stream carries frames only: the viewer can
    acknowledge a frame, pick another of the source's screens, or stop, nothing else.
- **Approval on the shown device:** a device only shows its screen to the current master, only
  while it is paired, and only with its own user's consent. The *Remote view* flag in the layout
  grants nothing by itself.
  - The first time, the agent on that device asks *"(name) wants to see this screen"* with
    *Allow once*, *Always allow this device* and *Deny*. No answer within 30 seconds counts as Deny,
    and after a Deny the same device is refused for 10 seconds. The viewer forwards no keyboard or
    mouse input until the request is accepted, and the shown device ignores input from the
    requester while it asks, so nobody can click the prompt remotely.
  - *Always allow* is stored in `peers.json` (`"allowView": true`) and can only be given from that
    prompt. Revoke it with the *May view this screen without asking* switch in the *Devices* tab
    (shown only while the grant exists; this also ends a running view), or by forgetting the
    device. Pairing again starts without the grant, and an unpaired device is refused.
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
  (and for the first frame). Linux X11 and macOS use `QScreen::grabWindow` (macOS needs the
  *Screen Recording* permission). Wayland would need the ScreenCast portal, which is not
  implemented.
- **Status:** capture → encryption → transport → reassembly is tested end to end on Windows, and so
  are the approval, refusal and timeout paths (`tests/tst_view.cpp`). The full-screen viewer window
  and the approval dialog have not been tried on two machines yet.

### Clipboard

`src/core/clipboardsync.*`: the clipboard follows the shared mouse. What was copied on a device
goes to the device the cursor moves to next, and to no other one.

- Hand-off: the master sees every crossing from X to Y. If X is the master, it sends its clipboard
  to Y; otherwise it tells Y to expect X's clipboard and tells X to send it to Y. X obeys only the
  master, and only right after the cursor left it. Y accepts a clipboard only while the cursor is
  on it, and only from the master or the device it was told to expect. Nothing is broadcast or
  relayed: a password copied on one computer reaches only the one you move to, and no paired device
  can replace another one's clipboard whenever it likes.
- Copied passwords are shared like anything else (password managers' "do not share" marks are not
  honoured on purpose): copying one and moving to another device is taken as wanting it there.

- Formats: plain text, HTML and images (as PNG), up to 64 MB each. Small formats travel with the
  announcement over UDP; larger ones are fetched right away over encrypted TCP
  (`src/core/filetransfer.*`).
- **Copied files and folders** (up to 1 GB / 10,000 files) are not transferred when copied: the
  announcement only lists their names and sizes (file system metadata, up to 2,000 files). They
  travel when the user **pastes** on the other device:
  - Windows: the agent offers them as virtual files (`FileGroupDescriptorW` + `FileContents`, as
    Remote Desktop does). Other programs may look at the list without causing a transfer; when
    Explorer pastes, it asks for the contents, the agent has the service fetch the files, and
    Explorer copies them with its own progress. Without a list (more files), a deferred `CF_HDROP`
    is offered instead.
  - Elsewhere (session helper) the files are fetched first, then put on the clipboard.
  - Transferred files are stored in `<user home>/Deskflower/Clipboard/<stamp>/` (the last 3 are
    kept).
  The sender only serves exactly what it offered. The receiver rejects unsafe paths (`..`,
  absolute paths, drive letters) and enforces the size limit.
- Last-writer-wins, stamped with wall-clock time: an older clipboard brought by the cursor never
  replaces a newer one.
- The OS clipboard is read and written by `src/clipboard/clipboardbridge.*`, in a process that runs
  as the user: on Windows in service mode the **agent** (the session helper runs as LocalSystem,
  and Windows does not give such a process the user's clipboard reliably: changes went unseen and
  pasted files need calls into the owner), elsewhere the session helper. So on Windows clipboard
  sharing needs the agent running (it starts at sign-in by default). The bridge ignores the change
  notification caused by content it applied itself, so content never bounces between devices.
- The agent tells the user what happens to copied files: shared, not shared (and why), being
  received, ready to paste, or why they could not be fetched.
- "Share clipboard" (Screens tab) turns it off per device, persisted in
  `<config dir>/settings.json`. Clipboard content is never written to disk.
- A device hands on what it adopted with its own offer: the formats, not files it has not
  transferred itself.

### Dragging files between devices

`src/core/filedrag.*` (service), `src/agent/filedragagent_win.cpp` and
`src/clipboard/virtualfiles_win.cpp` (agent). Drag files in Explorer across to another device and
drop them as on that computer: into a folder window, a sub-folder, a spot on the desktop, or an
application that takes files. They are **moved** (Windows only for now).

1. Crossing an edge with the left button held, the master holds the crossing back for a moment
   (at most ~1.5 s) and asks the device the drag is on what is dragged. Its agent puts a small
   invisible drop target under the cursor: Explorer's drag hands it the paths (nothing is read),
   and the agent ends that drag without a drop. Without files (a window, a text selection) the
   crossing goes on as usual.
2. The source offers the files (FileTransfer) with their names and sizes. On whichever device the
   cursor is, that device's agent runs a real drag (`SHDoDragDrop`, on a thread of its own, with
   the files' icon and count as the drag image), so Explorer and applications show and take it as
   a local one. Files from another device are virtual files (`FileGroupDescriptorW` +
   `FileContents`): nothing travels until the drop target reads them. Back on the source device,
   the originals are dragged and Windows moves them itself.
3. Releasing the button ends that drag with a drop. The drop target reads the files; the device
   transfers them then (into `<shared folder>/Dragged/`, the last 3 kept) and serves them.
4. Once every file arrived complete (the transfer checks every size; the stream is authenticated),
   the source sends the originals to the **Recycle Bin**. If nothing took them, or anything failed,
   the originals stay.

The device's drag does not follow its own mouse button (the button is held on another computer):
it ends when the master says (drop or cancel), and its capture window is fed the cursor's moves.
Works with remote-view devices too. Not yet on Linux and macOS. Without a file list (more than
2,000 files) the files go straight into the folder under the cursor instead.

### Shared folder and phone access

- **Shared folder:** where phone uploads and files received through the clipboard go. It is chosen
  in the agent (*Files → Change…*) and stored in `<config dir>/settings.json`; the default is
  `<user home>/Deskflower`. *Open* shows it in the file manager, and the tab shows its contents live.
  - The Windows service runs as LocalSystem, so the folder must be inside the signed-in user's
    folder and reached without links or junctions (a stored folder that no longer qualifies falls
    back to the default). It is created with the user's rights.
  - Received files are never written through a link or junction, and old clipboard and drag folders
    are deleted without following links (`src/core/fsutil.*`).
- **Phone access** (*Files* tab, off by default): an HTTPS web app on the local network
  (`src/web/`, port 24802; a small HTTP/1.1 server on Qt Network's `QSslServer`, because Qt HTTP
  Server is GPL-only). It lets a phone upload photos and files (camera included), browse and
  download the shared folder, and send text to the computer's clipboard.
  - The certificate is self-signed: EC P-256, generated with OpenSSL, with the LAN addresses in
    its subjectAltName, and kept for 825 days. Phones warn about it once.
  - Access: the QR code carries a single-use token valid for 10 minutes; a used or expired code
    is replaced by a new one on screen. The token is exchanged for an `HttpOnly; Secure;
    SameSite=Lax` session cookie (12 h; Lax because the scan opens the browser from the camera app,
    a cross-site navigation). *Disconnect phones* revokes every session.
  - The QR uses the address of the interface that has a default gateway, so it is not the address
    of a host-only or virtual adapter.
  - Uploads go in chunks of up to 8 MB to a hidden temporary file, can resume, and never
    overwrite an existing file. Downloads are streamed from disk.
  - Every path resolves inside the shared folder, symbolic links included, and the Windows service
    reads and writes the folder **as the signed-in user** (impersonation), never as LocalSystem:
    a link inside the folder cannot reach anything that user could not.
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

Dependencies: Qt ≥ 6.8, libsodium and OpenSSL 3. Linux also needs
X11 (Xi, Xtst, Xrandr) and, for Wayland, libei/liboeffis plus Qt D-Bus. On Windows,
libsodium is downloaded automatically and OpenSSL comes from the Qt installer
(`C:\Qt\Tools\OpenSSLv3`; its DLLs are copied next to the executables).

Linux: Qt 6.8 or newer (distributions such as Ubuntu 24.04 ship older versions: use the Qt
online installer or `aqtinstall`), then for example on Debian/Ubuntu:
`libsodium-dev libssl-dev libx11-dev libxi-dev libxtst-dev libxrandr-dev libei-dev liboeffis-dev`.
macOS: Qt 6.8+, `brew install libsodium openssl@3`. Then:

```sh
cmake --preset unix-debug -DCMAKE_PREFIX_PATH=/path/to/Qt/6.x/gcc_64
cmake --build --preset unix-debug
```

## Packaging (self-contained folder)

```bat
:: Release build + Qt libraries, QML modules, plugins and the MSVC runtime in dist\DeskFlower\bin
scripts\package-windows.bat [destination]
```

On other platforms: `cmake --install build/<preset> --prefix <dir>` (uses Qt's deploy scripts).

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
| Windows | MSI (WiX 3, downloaded into `build/tools`) | `scripts\package-windows.bat` → `dist\Deskflower-<version>-x64.msi` | Built and installed |
| Linux | DEB, self-contained in `/opt/deskflower` | `cpack -G DEB` in the build directory; CI | Not built yet |
| macOS | DMG with the app bundles | `cpack -G DragNDrop`; CI | Not built yet |

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
autostart). On macOS, copy the bundles and run `deskflower-service --install` once to register
the LaunchAgent.

## Signed updates from paired devices

When a paired device runs a newer DeskFlower for the same platform, its row on the **Devices** tab
says "DeskFlower 0.2.0 is available from this device" with an **Update** button. Nothing is
installed without that click. The package is checked before it is installed; the device that sends
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
`%SystemRoot%\System32\msiexec.exe /i <msi> /qn /norestart` (log:
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
and writes `dist\Deskflower-<version>-x64.msi.manifest` and `.manifest.sig` next to it, checking the
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
:: for the local subnet only) and starts it.
deskflower-service --install
deskflower-service --uninstall
```

Install from a packaged folder (above): the service runs as LocalSystem, which does not have the
Qt development directories on its `PATH`.

| Platform | Mechanism | Configuration and logs |
|---|---|---|
| Windows | Service Control Manager, `--service` entry point | `%ProgramData%\DeskFlower` (SYSTEM and Administrators only) |
| Linux | `systemd --user` unit written by `--install` | `~/.config/Deskflower/deskflower` |
| macOS | LaunchAgent written by `--install` | `~/Library/Preferences/Deskflower/deskflower`, `~/Library/Logs/Deskflower` |

On Windows the agent runs as the logged-in user while the service runs as LocalSystem. The IPC pipe
is therefore open to local users, and the service only accepts clients from the active console
session. Without any flag the service runs in the foreground (console mode) with per-user
configuration, which is the mode used for development.

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
start `deskflower-session --instance <name>` with the same variable.

In agent A, click **Pair** next to B and type the PIN that B displays. Then open the **Screens**
tab (or start the agent with `--page screens`) to arrange both devices.

The service logs to stderr in console mode and always to `<config dir>/logs/service.log`; the
session helper logs to `<config dir>/logs/session.log`.

Ports: UDP 24800 (discovery via multicast `239.255.77.100` and broadcast), UDP 24801 (sessions),
TCP 24801 (file transfers) and TCP 24802 (phone access over HTTPS, only while enabled).
All three can be changed in the agent (*⋮ → Advanced settings…*, a window of its own; or
`deskflower-agent --page settings`). They are stored as `dataPort`, `discoveryPort` and `phonePort`
in `<config dir>/settings.json` and used from the service's next start (the window offers *Restart
service*); `--port` on the service command line overrides `dataPort`. The discovery port must be
the same on every device; the data port is learned from beacons. The firewall rules allow the
program, not fixed ports.
The service adds its firewall rules when installed; run from a console, Windows asks for
permission on first run.
