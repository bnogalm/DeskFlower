# Testing DeskFlower

Three layers, from cheapest to most realistic:

1. **Library tests** (`tests/tst_*.cpp`): real `Node`s, sessions, transfers and the clipboard/drag
   logic in one process, over real UDP/TCP on 127.0.0.1, with fake agents written in C++.
2. **End-to-end tests** (`tests/e2e/tst_e2e.cpp`) and, on Windows, the **shell test**
   (`tests/tst_clipboardbridge.cpp`): the real `deskflower-service` program driven over its IPC,
   and the agent's clipboard objects pasted through Explorer's own drop target.
3. **Manual checklist** on real machines or VMs (below): what no automated test can see (the
   desktop, Explorer, LocalSystem, real routers).

Everything automated runs with `ctest` (the Windows helper used during development excludes
`tst_web` and `tst_phoneuser` only where OpenSSL is a stub). CI runs the same on Linux and macOS
(`.github/workflows/build.yml`).

## Safe on a developer's own computer

A plain `ctest` never disturbs the person using the machine:

- **Loopback only.** ctest sets `DESKFLOWER_BIND_ADDRESS=127.0.0.1` for every test, and `Node`
  then binds its UDP, TCP and discovery sockets there instead of on every interface (the phone
  server of `tst_web` and `tst_phoneuser` too); `tst_e2e`
  passes it to the services it starts even when run by hand, and every helper socket of the tests
  (free-port probes, the NAT simulator, fake attackers) binds a loopback address. Nothing listens
  on the network, so Windows never asks for a firewall rule for a new test binary.
- **No desktop.** Cases that would touch the signed-in user's session or profile skip unless
  `DESKFLOWER_DESKTOP_TESTS=1` is set; their tests carry the ctest label `desktop`:

  | Test | What it touches |
  |---|---|
  | `tst_clipboardbridge` (Windows) | replaces the real clipboard's content and pastes through the shell's drop targets |
  | `tst_platform` (four cases) | creates folders in the home folder, and the received folder in `AppData\Local\Deskflower` |
  | `tst_e2e_cli` (`versionDoesNotBlockWithoutConsole`) | should it regress, a message box opens on the desktop |

  CI sets the variable (its runners are disposable). Elsewhere set it only in a VM, Windows
  Sandbox or a container, or on your own machine while you are not using it (see "Desktop tests"
  below). No test injects input, captures the screen or starts a drag on the real desktop.
- **Nothing installed.** Services under test are plain processes with `--instance`, `--config-dir`
  and `--home` in temporary folders; they die with the test program (a Windows job object, the
  Linux parent-death signal).

## End-to-end tests (`tst_e2e`)

`tst_e2e` starts real `deskflower-service` processes in console mode (`--instance <unique>
--config-dir <tmp> --home <tmp> --port <free> --no-discovery --no-session`) and connects to each one
twice over its local socket, with the same CBOR frames as the real programs:

- as the **session helper** (authenticated with `DESKFLOWER_SESSION_TOKEN`): it reports one
  1920×1080 screen and plays the user (`edge`, `mm`, `mb`, `dragRelease`, `activity`, `clip`);
- as the **agent**: it pairs (`addPeer`, `pair`, `pin`, `setPairTrust`), pastes (`clipUnseal`,
  `clipFetch`) and answers the service's questions (`dragCatch`, `dragHover`, `recycle`) the way the
  real agent does.

So the code under test is the shipped service: `ServiceController`, the IPC, the folders
(`--home`), `ClipboardSync`, `FileDrag`, `FileTransfer` and `Node` together. ctest runs it in three
parts, each its own entry:

| ctest entry | What it does |
|---|---|
| `tst_e2e_cli` | `deskflower-service --version` prints and exits; started detached without a console it must not show a message box (Windows; that case is `desktop`, above) |
| `tst_e2e_direct` | two devices A (master) and B on loopback, then a third one, C |
| `tst_e2e_nat` | the same with B behind a simulated home router (`NatRouter`, below) |

Scenarios of `direct` and `nat`, in order (each later one needs the pairing):

1. **Pairing by PIN** — A adds B by address, asks to pair; B shows the PIN (and the level the
   dialog suggests: `device` for a new device); A types it; both connected, with each other's
   version, as `device`. Behind the NAT: B sees A at the router's LAN address, A sees B at the port
   forward.
2. **Cursor crossing** — A shares its keyboard and mouse, lays B to its right; A's helper pushes
   against the right edge: the cursor goes to B and A's helper is told to capture; moving left past
   B's edge brings it home.
3. **Text clipboard** — copied on A (text and HTML), the cursor crosses: B's clipboard owner gets
   the sealed formats (names and sizes only), and the paste (`clipUnseal`, after an `activity`,
   which the service ignores) returns the plaintext; then copied on B and pasted on A after the cursor comes back.
4. **Trusted paste without paste evidence** — a trusted device pastes text and files with no
   input of its own shortly before (only guests need paste evidence).
5. **Files and folders by clipboard** — a tree with nested and empty folders, a folder holding
   only empty folders, Unicode names, an empty file and a 300 KB binary file is copied on A and
   pasted on B, then the other way; the list announced (`pendingFiles`, `pendingList` with folders
   as `[path, -1]`), the pasted tree (names, folders, sizes, SHA-256) and the transfer events shown
   to the user (`transfer`, kind `clipboard`, `done`) are checked; the files land in the received
   folder.
6. **Drag A → B** — the button is held at A's edge: A's agent is asked what is dragged
   (`dragCatch`), B's agent runs the drag (`dragHover`, folders listed), the release ends it there
   (`dragHoverEnd`), the drop target reads the files (`dragFetch`, tree compared), and A recycles the
   originals (move semantics).
7. **Drag B → A** — the button goes down on B and the cursor comes back with it: B is asked,
   A runs the drag, A's own helper reports the release.
8. **Router restart** (`nat` only) — every NAT mapping is dropped: the session survives. B follows
   A to its new mapped port, or, when it already talks to A's own address (below), the cursor still
   crosses.
9. **Restart and reconnection** — B stops and starts again on its port: A finds it; A restarts on
   another port: it goes back to B's last address; both have each other's hello again (this found
   the bug fixed in `Node`: a peer that restarts faster than the session times out) and the cursor
   still crosses.
10. **Group of three** — A and B become a group, C joins A's group by PIN: B and C are introduced,
    trust each other as `group` and connect, with no PIN shown on B.

**Pending features.** Behaviour that is documented as intended but lives on another branch is
checked with `E2E_EXPECT(feature, …)`. While the feature is listed in `kPendingFeatures`
(`tst_e2e.cpp`), a failure is reported as a SKIP with the reason, and a success as a PASS with a
warning to drop it from the list. None is listed now.

**Running by hand.**

```bat
:: Windows (Qt's bin folder on PATH)
build\win-debug\tst_e2e.exe direct
build\win-debug\tst_e2e.exe nat -o result.txt,txt
```

- `DESKFLOWER_SERVICE_EXE` chooses another `deskflower-service` (by default the one next to the
  test, else the path CMake recorded).
- `DF_E2E_LOG_RULES` sets `QT_LOGGING_RULES` for the services (default `deskflower.*.info=true`;
  for example `deskflower.*.debug=true`).
- Each service logs to `<work>/<A|B|C>/service.log`. The failure message prints the work folder
  (`%TEMP%/dfe2e-XXXXXX`) and ends with the last lines of every log and the last events each fake
  client received. The folder is removed afterwards unless `DF_E2E_KEEP=1` is set.
- `DESKFLOWER_BIND_ADDRESS` defaults to `127.0.0.1` for the services (set it to another address
  only on a disposable machine).
- Instance names, ports and folders are unique per run: several runs (other checkouts, ctest `-j`)
  can go at the same time, and nothing touches the real user's configuration or folders.

**The NAT simulator** (`NatRouter` in `tests/e2e/e2eharness.h`) is a port-restricted home router
on loopback. UDP: the WAN address `127.0.0.2:<port>` forwards to B (`127.0.0.1:<B's port>`), and B
sees every outside device as `127.0.0.3:<mapped port>`, one mapping per outside endpoint, as a
router doing masquerade (SNAT) does; only B's own answers go back out. TCP (the file streams, on
the port each device announces in its hello): B's TCP port is forwarded on the WAN address, and B
reaches an outside device's TCP port at `127.0.0.3`, where it sees that device; each side sees the
connection come from the router's other address. The router only carries the traffic until the
devices find each other's own address: a 0.1.20 hello reports it (`la`), and on loopback it is
always reachable, so each side soon talks to the other directly, as two devices behind the same
router do (hairpin; Node probes the address and moves there). Pairing, the first packets and
possibly the first streams go through the router; later streams may not. The router paths alone
(port forwards, dial-back, tunnels, new mappings) are covered by `tst_node` with a simulated
network that blocks direct paths. It does not model a router
without the port forward (both sides behind routers that only map outgoing traffic), hairpinning
or symmetric NATs: those need real routers (Docker or VMs, below). macOS has only `127.0.0.1` on
`lo0` by default: `sudo ifconfig lo0 alias 127.0.0.2 up` and `sudo ifconfig lo0 alias 127.0.0.3
up` enable the `nat` part, which is skipped otherwise.

**What it does not cover.** The real agent (QML, `backend.cpp`, its clipboard and drag code), the
real session helper (input capture and injection, screens), the service mode (LocalSystem,
impersonation, the console-session checks) and the desktop itself. See the shell test and the
checklist.

## Shell test (`tst_clipboardbridge`, Windows)

The agent's clipboard objects as Explorer sees them. Each sealed `IDataObject`
(`createSealedData`) is pasted into a temporary folder through that folder's shell `IDropTarget`
(`SHParseDisplayName` → `GetUIObjectOf(IID_IDropTarget)` → `DragEnter`/`DragOver`/`Drop`), which is
what Ctrl+V in Explorer does, with messages pumped while the shell copies; no window is shown.
Each case runs twice:

- `direct`: the object itself;
- `clipboard`: put on the real clipboard and read back with `OleGetClipboard` (the OLE wrapper, as
  any other application gets it). Skipped when the process may not use the clipboard (a service, a
  sandboxed process, a runner without a desktop). These rows **replace the clipboard's content**
  (and leave it empty); concurrent runs take turns through a named mutex.

The whole test is `desktop`: without `DESKFLOWER_DESKTOP_TESTS=1` it reports a skip and touches
nothing (see "Desktop tests" to run it).

Cases: sealed text and HTML (nothing decrypted until asked; a refused render is not remembered;
`CF_HTML` header), virtual files with nested and empty folders and Unicode names (descriptor
order and attributes, the transfer happens once and only when the shell reads the contents, tree
compared), the deferred `CF_HDROP` used without a list, the fallback to `CF_HDROP` when a relative
path does not fit a descriptor, `ClipboardBridge::applySealed` (and that its own echo is not
reported as a copy), and a folder copied in Explorer (`CF_HDROP`) reported with the folder.

## Desktop tests (opt-in)

Run them in a VM or Windows Sandbox, or on your own machine while you are not using it: they
replace the clipboard, paste into folders through Explorer's code, and create folders in the home
folder and in `AppData\Local\Deskflower`.

```bat
:: Windows, from the build folder (Qt's bin folder on PATH)
set DESKFLOWER_DESKTOP_TESTS=1
ctest -L desktop --output-on-failure
set DESKFLOWER_DESKTOP_TESTS=
```

Checklist:

- [ ] Before: save anything you have copied; close remote-desktop and clipboard-manager tools and
      turn off clipboard history sync (they take the clipboard over and make the `clipboard` rows
      skip).
- [ ] `tst_clipboardbridge`: every case passes in both rows, `direct` and `clipboard` (a skipped
      `clipboard` row says why). No window appears and no Explorer window opens; the clipboard is
      empty afterwards.
- [ ] `tst_platform`: the four profile cases pass; afterwards no `deskflower-test-*` folder is left
      in the home folder (`AppData\Local\Deskflower\Received` stays: it is the app's own folder).
- [ ] `tst_e2e_cli`: `versionDoesNotBlockWithoutConsole` passes within seconds and no dialog
      appears. A dialog showing the version means the regression is back: close it and report it.
- [ ] After: `tasklist /v | findstr deskflower-service` shows no test service left (the installed
      service, if any, runs under LocalSystem with `--service` and is not touched).
- [ ] Then, with the real agent, the clipboard and drag items of the manual checklist below: what
      these tests prove about the shell does not replace a paste with Ctrl+V in Explorer or a drop
      on the desktop.

## Real service mode on Windows (manual, administrator)

The end-to-end tests run the service in console mode. To exercise LocalSystem, impersonation of
the console user and the agent as clipboard owner without an MSI:

```bat
:: elevated prompt; a build with its Qt DLLs deployed next to it (windeployqt), or scripts\package-windows.bat's dist\Deskflower\bin
sc create DeskflowerE2E binPath= "\"C:\path\to\bin\deskflower-service.exe\" --service --instance e2e --port 34950 --no-discovery" obj= LocalSystem start= demand
sc start DeskflowerE2E
:: as the signed-in user: an agent for that instance, a second instance in console mode to pair with
C:\path\to\bin\deskflower-agent.exe --instance e2e
C:\path\to\bin\deskflower-service.exe --instance peer --port 34960 --no-discovery
C:\path\to\bin\deskflower-agent.exe --instance peer
:: afterwards
sc stop DeskflowerE2E & sc delete DeskflowerE2E
```

Configuration and logs of that instance are under `%ProgramData%\DeskFlower\e2e` (turn logs on with
`"logging": true` in its `settings.json`). Check that pasted and dropped files are owned by the user
and land in the user's received folder, that a shared folder outside the user's profile is refused,
and the checklist items marked *service*.

## Manual checklist

Set `"logging": true` in `settings.json` on every machine first (Windows service:
`%ProgramData%\DeskFlower\settings.json`; then restart the service) and keep `service.log`,
`session.log` and the agent's notices of both sides for every failure.

### Setups

**Windows Sandbox** (Windows 11 Pro/Enterprise, *Windows Sandbox* feature on). The sandbox sits
behind a NAT run by the host, so host ↔ sandbox is a real "router in between": discovery does not
cross it (add the other device by IP), and the sandbox sees the host at its LAN address. Save as
`deskflower.wsb` next to `dist\` and open it:

```xml
<Configuration>
  <Networking>Enable</Networking>
  <ClipboardRedirection>Disable</ClipboardRedirection>
  <MappedFolders>
    <MappedFolder>
      <HostFolder>C:\otro\deskflower\dist</HostFolder>
      <SandboxFolder>C:\dist</SandboxFolder>
      <ReadOnly>true</ReadOnly>
    </MappedFolder>
  </MappedFolders>
  <LogonCommand>
    <Command>cmd /c for %m in (C:\dist\Deskflower-*-Windows-x64.msi) do msiexec /i "%m" /qn /l*v C:\Users\WDAGUtilityAccount\Desktop\install.log</Command>
  </LogonCommand>
</Configuration>
```

(`ClipboardRedirection` off, so that the sandbox's own clipboard sharing does not hide or fake a
DeskFlower copy.) The sandbox starts clean every time: a good place for first-run, upgrade (map a
folder with two MSIs) and pairing tests.

**Hyper-V, two VMs behind NAT.** The *Default Switch* already NATs its VMs behind the host (it
hands out addresses by DHCP, and changes its subnet after a host reboot). Two VMs on it reach each
other directly (same subnet), and the host reaches both: host ↔ VM is the "behind a router"
case. For a router you control (port forwarding, hairpin, a restart of the router):

```powershell
# elevated, on the host: an internal switch with its own NAT
New-VMSwitch -Name DFNat -SwitchType Internal
New-NetIPAddress -IPAddress 192.168.77.1 -PrefixLength 24 -InterfaceAlias "vEthernet (DFNat)"
New-NetNat -Name DFNat -InternalIPInterfaceAddressPrefix 192.168.77.0/24
# a VM on DFNat with a static address (192.168.77.10/24, gateway 192.168.77.1); optionally forward
# the DeskFlower port to it from the host's LAN address:
Add-NetNatStaticMapping -NatName DFNat -Protocol UDP -ExternalIPAddress 0.0.0.0 -ExternalPort 24800 -InternalIPAddress 192.168.77.10 -InternalPort 24800
Add-NetNatStaticMapping -NatName DFNat -Protocol TCP -ExternalIPAddress 0.0.0.0 -ExternalPort 24800 -InternalIPAddress 192.168.77.10 -InternalPort 24800
# "restart the router": Remove-NetNat DFNat, then New-NetNat again
```

A second VM on the Default Switch (or a laptop on the LAN) is then "outside". A third VM on DFNat is
"inside the same LAN" (hairpin case: two devices behind one router, introduced by an outside group
member).

**Linux in Docker.** *To be completed by the Linux work:* a compose file with a NAT router
(iptables masquerade + port forward between two bridge networks) and Ubuntu nodes running the DEB
under Xvfb, with `xdotool` and `xclip` driving the real session helper. Until then, the automated
suite (including `tst_e2e`) runs headless in a container with Qt 6.11:

```sh
docker run --rm -v "$PWD:/src:ro" -v deskflower-build:/build -e QT_QPA_PLATFORM=offscreen <image with Qt 6.11.2, cmake, ninja, libsodium-dev, libssl-dev, X11/libei dev packages> \
  bash -c 'cmake -S /src -B /build/b -G Ninja && cmake --build /build/b && ctest --test-dir /build/b --output-on-failure'
```

(The packages are those of the CI's Linux job; Qt comes from `aqtinstall`, since Ubuntu 24.04's Qt
is too old.)

### Checklist (Windows; repeat what applies on Linux X11, Linux Wayland and macOS)

Pairing and connection
- [ ] Pairing by PIN from discovery, and by manual IP; the PIN dialog's suggested level; each
      level (guest, this device, all my devices) shown the same on both sides.
- [ ] Three devices as a group, one of them behind NAT: the third one is introduced and connects
      without a PIN.
- [ ] Restart the service (and the computer) on each side: reconnection, the version is shown
      again on the other side, the cursor and the clipboard work at once.
- [ ] Lock the session and switch user: the cursor returns home, the clipboard is cleared, the
      other user sees none of the first user's pairings (*service*).

Keyboard and mouse
- [ ] The cursor crosses in both directions, on every edge of the layout, with several monitors
      and different scales.

Clipboard (trusted devices paste without any extra step; guests need Ctrl+V on the device)
- [ ] Text, HTML and an image, in both directions; paste with Ctrl+V and with the context menu.
- [ ] One file, several files, a folder with subfolders, an empty folder, a folder holding only
      empty folders; Unicode names; a file over 2 GB (refused with a notice above the limit);
      progress shown while pasting, cancel works, every refusal or failure shows a notice.
- [ ] Source and destination under a folder redirected to OneDrive (Known Folder Move), on a
      OneDrive placeholder, and behind a junction.
- [ ] With a clipboard manager / remote-desktop tool running (Windows clipboard history, AnyDesk):
      nothing is transferred until the user pastes.

Dragging files
- [ ] From Explorer on A to B's desktop and to an Explorer window, and back; a folder (with empty
      subfolders); drop on an application; cancel with Esc; drop back on A; the originals go to A's
      Recycle Bin only when B took them.

Behind a router (Sandbox, Default Switch or DFNat)
- [ ] Pairing by IP, cursor, text clipboard, files and drags in **both** directions.
- [ ] Restart the device behind the router, and the router (DFNat: remove/recreate the NAT):
      reconnection without touching anything.
- [ ] The address shown for a peer behind the router, and what `peers.json` keeps for it.

Updates
- [ ] A device updates another one from the agent (signed MSI); both reconnect, the agent comes
      back.
