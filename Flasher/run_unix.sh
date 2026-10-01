#!/usr/bin/env sh
set -eu
cd "$(dirname "$0")"

if command -v python3 >/dev/null 2>&1; then
    exec python3 run.py
fi

echo "Python 3 was not found."
echo "Install Python 3.11 or newer and run this script again."
exit 1
