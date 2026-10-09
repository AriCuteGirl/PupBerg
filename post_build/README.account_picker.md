## What is this ?
A small tool that makes the emu use your own Steam name and SteamID instead of a random one.  
It finds the Steam accounts that logged in on this PC, lets you pick yours, and saves it in the emu's global `configs.user.ini`.  
Other settings in that file are kept as they are.

## How to use
### Windows
Double click `account_picker_x64.exe` and type the number of your account.  
It writes to `%APPDATA%\GSE Saves\settings\configs.user.ini`.

### Linux
```shell
chmod +x account_picker.sh
./account_picker.sh
```
It writes to `~/.local/share/GSE Saves/settings/configs.user.ini` (native games) and also into every Proton/Wine prefix where the emu already ran (Steam `compatdata`, `~/.wine`, `~/.local/share/proton-pfx`).  
To also cover a prefix the emu hasn't run in yet, pass it as an argument:
```shell
./account_picker.sh ~/Games/my-prefix
```

## Notes
* The game doesn't connect to real Steam, so you still won't show up as "playing" in the Steam client.
* Every player should pick their own account, two players with the same SteamID will conflict in lobbies.
* A game folder with `steam_settings/configs.user.ini` or local saves overrides the global file.
