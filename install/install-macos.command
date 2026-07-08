#!/bin/bash
# PowerGrade — macOS installer. Double-click to run.
# Copies PowerGrade.ofx.bundle into /Library/OFX/Plugins (asks for your password).
set -e
DIR="$(cd "$(dirname "$0")" && pwd)"
SRC="$DIR/PowerGrade.ofx.bundle"

if [ ! -d "$SRC" ]; then
  echo "PowerGrade.ofx.bundle not found next to this installer."
  read -r -p "Press return to close." _; exit 1
fi

osascript <<EOF
do shell script "mkdir -p /Library/OFX/Plugins && rm -rf '/Library/OFX/Plugins/PowerGrade.ofx.bundle' && cp -R '$SRC' /Library/OFX/Plugins/" with administrator privileges
EOF

echo "PowerGrade installed to /Library/OFX/Plugins."
echo "Restart DaVinci Resolve, then find it under Effects > OpenFX > Power Grade."
read -r -p "Press return to close." _
