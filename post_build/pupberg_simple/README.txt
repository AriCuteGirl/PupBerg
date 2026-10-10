PupBerg - quick install
=======================

1. Find the game's steam_api64.dll (or steam_api.dll for 32-bit games) inside the game folder.
   On Linux native games it's libsteam_api.so.
2. Back up the original, then replace it with the one from this package.
3. Copy the steam_settings folder next to it.
   If the game isn't started from Steam, put its App ID (the number in the game's Steam store URL)
   into steam_settings/steam_appid.txt. If steam_settings is ever missing, PupBerg recreates it
   on the next launch.
4. Run the account picker once (account_picker_x64.exe on Windows, account_picker.sh on Linux)
   and pick your Steam account, so friends see your real name.
5. Start the game. Shift+Tab (or Home) opens the PupBerg overlay.

Playing with friends over the internet:
  Overlay -> Network -> "Public Server" -> Create room, send the room code to your friends,
  they press Join with that code. Public rooms also show up in the room list for everyone
  playing the same game, private rooms are code only.
  Prefer a VPN? Switch to "ZeroTier" in the same tab.

Some very old games (Steamworks SDK 1.36 or older) also need steam_settings/steam_interfaces.txt,
see tools/generate_interfaces in the full package.
