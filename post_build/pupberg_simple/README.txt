PupBerg - quick install
=======================

The easy way: run the installer
  Windows: pupberg_installer.exe
  Linux:   ./pupberg_installer.sh   (also for Windows games running through Proton)

It lists your installed Steam games, or takes any game folder with [f]. For the game you pick it
  - replaces steam_api64.dll / steam_api.dll / libsteam_api.so with PupBerg
    (the originals are kept as *.valve_original, choose the game again and [r] to restore them)
  - creates steam_settings with the game's App ID, the overlay enabled and steam_interfaces.txt
  - asks which Steam account to use, so friends see your real name

Then start the game. Shift+Tab (or Home) opens the PupBerg overlay.

By hand
  1. Back up the game's steam_api64.dll (steam_api.dll for 32-bit games, libsteam_api.so for native
     Linux games) and replace it with the one from this package.
  2. Copy the steam_settings folder next to it and put the game's App ID (the number in its Steam
     store URL) into steam_settings/steam_appid.txt. Games inside a Steam library find it themselves.
  3. Run the account picker once (account_picker_x64.exe on Windows, account_picker.sh on Linux).

Playing with friends over the internet
  Overlay -> Network -> "Public Server" -> Create room, send the room code to your friends,
  they press Join with that code. Public rooms also show up in the room list for everyone
  playing the same game, private rooms are code only.
  Prefer a VPN? Switch to "ZeroTier" in the same tab.
