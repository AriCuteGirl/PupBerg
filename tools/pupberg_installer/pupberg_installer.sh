#!/usr/bin/env bash
# PupBerg installer, Linux launcher for the native installer binary next to this script
dir="$(cd -- "$(dirname -- "$0")" && pwd)"
exec "$dir/pupberg_installer" "$@"
