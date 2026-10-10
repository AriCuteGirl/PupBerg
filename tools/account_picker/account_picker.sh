#!/usr/bin/env bash
# PupBerg account picker
# finds the Steam accounts that logged in on this PC (config/loginusers.vdf)
# and writes the chosen one into the emu's configs.user.ini,
# both the native Linux one and the one inside every Proton/Wine prefix
#
# usage: ./account_picker.sh [extra wine prefix ...]

set -u

echo "PupBerg account picker"
echo "======================"
echo

vdf=""
for dir in "$HOME/.steam/steam" "$HOME/.local/share/Steam" "$HOME/.var/app/com.valvesoftware.Steam/.local/share/Steam"; do
  if [[ -f "$dir/config/loginusers.vdf" ]]; then
    vdf="$dir/config/loginusers.vdf"
    steam_dir="$dir"
    echo "Found Steam at: $dir"
    echo
    break
  fi
done

[[ -n "$vdf" ]] || {
  echo "[X] couldn't find any Steam accounts on this PC (is Steam installed and have you logged in?)"
  exit 1
}

# one line per account: steamid<TAB>persona<TAB>account<TAB>mostrecent
mapfile -t accounts < <(awk '
  {
    n = 0; delete tok
    line = $0
    while (match(line, /"[^"]*"/)) {
      tok[++n] = substr(line, RSTART + 1, RLENGTH - 2)
      line = substr(line, RSTART + RLENGTH)
    }
  }
  n == 1 && length(tok[1]) == 17 && tok[1] ~ /^[0-9]+$/ { if (id != "") print id "\t" persona "\t" acc "\t" recent; id = tok[1]; persona = ""; acc = ""; recent = 0; next }
  n == 2 && tolower(tok[1]) == "personaname" { persona = tok[2] }
  n == 2 && tolower(tok[1]) == "accountname" { acc = tok[2] }
  n == 2 && tolower(tok[1]) == "mostrecent"  { recent = tok[2] }
  END { if (id != "") print id "\t" persona "\t" acc "\t" recent }
' "$vdf")

[[ ${#accounts[@]} -gt 0 ]] || {
  echo "[X] no accounts found in $vdf"
  exit 1
}

for i in "${!accounts[@]}"; do
  IFS=$'\t' read -r id persona acc recent <<< "${accounts[$i]}"
  marker=""
  [[ "$recent" = "1" ]] && marker="  <- last used"
  echo "  [$((i + 1))] $persona  ($acc, $id)$marker"
done

while true; do
  echo
  read -r -p "Pick your account [1-${#accounts[@]}]: " choice || exit 1
  [[ "$choice" =~ ^[0-9]+$ ]] && (( choice >= 1 && choice <= ${#accounts[@]} )) && break
  echo "[X] invalid choice"
done

IFS=$'\t' read -r sel_id sel_persona _ _ <<< "${accounts[$((choice - 1))]}"

# update account_name/account_steamid in place, keep every other line as is
write_user_ini() {
  local ini="$1"
  mkdir -p "$(dirname "$ini")" || return 1
  local tmp
  tmp="$(mktemp)" || return 1
  [[ -f "$ini" ]] || : > "$ini"
  awk -v name="account_name=$sel_persona" -v sid="account_steamid=$sel_id" '
    function flush() {
      if (!has_name) print name
      if (!has_id) print sid
      has_name = has_id = 1
    }
    { sub(/\r$/, "") }
    /^\[/ {
      if (in_user) flush()
      in_user = ($0 ~ /^\[user::general\]/)
      if (in_user) { found = 1; has_name = has_id = 0 }
      print; next
    }
    in_user && /^account_name=/    { print name; has_name = 1; next }
    in_user && /^account_steamid=/ { print sid;  has_id = 1;   next }
    { print }
    END {
      if (in_user) flush()
      if (!found) { if (NR) print ""; print "[user::general]"; print name; print sid }
    }
  ' "$ini" > "$tmp" && mv "$tmp" "$ini"
}

targets=("${XDG_DATA_HOME:-$HOME/.local/share}/GSE Saves/settings/configs.user.ini")

# Proton/Wine prefixes where the emu already ran, plus any passed on the command line
search_roots=(
  "$HOME/.wine"
  "$HOME/.local/share/proton-pfx"
  "$steam_dir/steamapps/compatdata"
  "$HOME/PortProton/data/prefixes"
  "$HOME/.var/app/com.usebottles.bottles/data/bottles/bottles"
  "$HOME/.local/share/bottles/bottles"
  "$HOME/Games"
)
if [[ -f "$steam_dir/steamapps/libraryfolders.vdf" ]]; then
  while IFS= read -r lib; do
    search_roots+=("$lib/steamapps/compatdata")
  done < <(awk -F'"' 'tolower($2) == "path" { print $4 }' "$steam_dir/steamapps/libraryfolders.vdf")
fi
for prefix in "$@"; do
  search_roots+=("$prefix")
done

while IFS= read -r -d '' saves; do
  targets+=("$saves/settings/configs.user.ini")
done < <(find "${search_roots[@]}" -maxdepth 9 -type d -path '*/drive_c/users/*/AppData/Roaming/GSE Saves' -print0 2>/dev/null | sort -zu)

# an explicitly passed prefix gets the file even if the emu never ran there yet
for prefix in "$@"; do
  found_user=0
  for user_dir in "$prefix"/drive_c/users/* "$prefix"/pfx/drive_c/users/*; do
    [[ -d "$user_dir" && "$(basename "$user_dir")" != "Public" ]] || continue
    targets+=("$user_dir/AppData/Roaming/GSE Saves/settings/configs.user.ini")
    found_user=1
  done
  [[ $found_user = 1 ]] || echo "[X] '$prefix' doesn't look like a Wine/Proton prefix (no drive_c/users), skipping"
done

echo
mapfile -t targets < <(printf '%s\n' "${targets[@]}" | sort -u)
for ini in "${targets[@]}"; do
  if write_user_ini "$ini"; then
    echo "[OK] $ini"
  else
    echo "[X]  failed to write $ini"
  fi
done

echo
echo "Done! Games will now use '$sel_persona' ($sel_id)"
