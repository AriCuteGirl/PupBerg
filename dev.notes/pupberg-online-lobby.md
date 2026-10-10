# PupBerg online lobby (work log / handoff)

Goal for release `release-pupberg-0.1.2` (asked 2026-10-10):

1. Remove achievements from the PupBerg overlay UI (page, sidebar entry, Home tile, test button). Popups stay.
2. Online lobby server so friends can play over the internet without a VPN:
   - overlay mode switch: **ZeroTier** vs **Public Server**
   - rooms are **Public** (listed for everyone playing the same game) or **Private** (code only)
3. Recreate `steam_settings` next to the steam_api dll when it is missing at startup
   (steam_appid.txt + overlay enabled), it was never recreated before.
4. Release: besides the big archives, small downloads with just `steam_api64.dll`, `steam_api.dll`,
   `steam_settings` (+ account picker) for Windows and the Linux equivalent.
5. Build + test Windows and Linux (CI), open PR, merge, tag `release-pupberg-0.1.2`.

## Server

- Oracle Cloud Always Free, Frankfurt, Ubuntu 24.04 ARM (A1.Flex 1 OCPU / 6 GB)
- Reserved public IP **130.162.243.144**, port **47620** TCP+UDP (Oracle security list + iptables, saved)
- SSH: `ssh -i ~/.ssh/oracle_pupberg ubuntu@130.162.243.144`
- Service: `pupberg-lobby.service` (systemd, DynamicUser), code in `/opt/pupberg-lobby/pupberg_lobby.py`
- Deploy: copy `tools/lobby_server/pupberg_lobby.py` + `.service`, `sudo systemctl restart pupberg-lobby`
- Protocol documented at the top of `tools/lobby_server/pupberg_lobby.py`, tests: `python3 tools/lobby_server/test_lobby.py`

## Emu side

- `dll/network.cpp` / `dll/dll/network.h`: `Networking::relay_*`. Room members with the same appid become
  `Connection{relayed=true}` with a virtual ip in 198.18.0.0/15, `sendTo()` routes them through the server.
- Settings `[overlay::pupberg]`: `network_mode=zerotier|server`, `lobby_server`, `lobby_room`, `lobby_public`, `lobby_auto_join`.

## Status

- [x] achievements removed from overlay UI (compiles)
- [x] lobby server written, tested locally + live (15 ms relay from CZ), deployed
- [x] relay client in Networking (compiles on Linux)
- [x] auto-join from settings at startup
- [x] overlay UI: mode switch, public/private, room list, create/join/leave (compiles, not seen in game yet)
- [x] steam_settings auto-create (tested on Linux: folder + steam_appid.txt + overlay ini)
- [x] end-to-end test: two Linux emu instances in separate network namespaces (pasta), friends + P2P
      packets both ways through the live server
- [x] simple release packages in release.yml (PupBerg-Windows.zip, PupBerg-Linux.tar.gz), only runs on tags
- [x] PR #4 merged, release-pupberg-0.1.2
- [x] 0.1.3 (PR #5): the Windows build never reached the lobby server because the LAN only connect()
      hook refused it -> connect_unhooked(); also wait for the TCP connect before sending (Wine);
      appid from the Steam library appmanifest; pupberg_installer (+ simple packages from one
      pupberg-simple release job). Windows DLL under Wine vs Linux peer verified through the live server.

Windows/Wine test recipe: CI artifact emu-win-api_experimental-debug-x64-<sha>, a flat-C-API test
program built with MinGW (C++ interfaces returning structs crash across MinGW/MSVC), run with
WINEDLLOVERRIDES="winedbg.exe=d" so a crash doesn't pop up a dialog on the user's desktop.

E2E test recipe: build `debug_x64 api_experimental`, two dirs with a copy of libsteam_api.so and their own
steam_settings (configs.user.ini with different steamids + local_save_path, configs.overlay.ini with
network_mode=server, lobby_room, lobby_auto_join=1), run a small SteamAPI program in each with
`LD_PRELOAD=/usr/lib/libasound.so.2:/usr/lib/libpulse.so.0 pasta --config-net -- ./peer`.
Remember AcceptP2PSessionWithUser, the emu only hands packets to accepted sessions.
