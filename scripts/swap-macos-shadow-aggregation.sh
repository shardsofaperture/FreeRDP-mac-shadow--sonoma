#!/bin/zsh
# Physical-test operator helper. Run at the Mac's local console only.
set -euo pipefail

repo="${0:A:h:h}"
installed="${HOME}/Applications/FreeRDP Shadow.app"
candidate="${repo}/dist/FreeRDP Shadow 0.2.1A.app"
backup="${repo}/dist/FreeRDP Shadow installed-0.2.0.backup.app"
staging="${HOME}/Applications/.FreeRDP Shadow.swap-staging.app"

if [[ $# -ne 1 || ( "$1" != candidate && "$1" != rollback ) ]]; then
	print -u2 "Usage: $0 candidate|rollback (at the Mac local console)"
	exit 2
fi
[[ -d "$installed" && ! -e "$staging" ]] || {
	print -u2 "Installed app missing or swap staging path already exists"; exit 1
}
if [[ "$1" == candidate ]]; then
	[[ -d "$candidate" && ! -e "$backup" ]] || {
		print -u2 "Candidate missing or stable backup already exists"; exit 1
	}
	[[ "$(plutil -extract FreeRDPShadowBuildLabel raw -o - "$installed/Contents/Info.plist")" ==
	   "0.2.0 — Fixed 250 KiB/s" ]] || {
		print -u2 "Installed app is not the expected stable 0.2.0"; exit 1
	}
	ditto "$installed" "$backup"
	codesign --verify --deep --strict "$backup"
	desired="$candidate"
else
	[[ -d "$backup" ]] || { print -u2 "Stable backup missing"; exit 1; }
	[[ "$(plutil -extract FreeRDPShadowBuildLabel raw -o - "$installed/Contents/Info.plist")" ==
	   "0.2.1A — 50 ms latest-state aggregator / Fixed 250 KiB/s" ]] || {
		print -u2 "Installed app is not the expected 0.2.1A candidate"; exit 1
	}
	desired="$backup"
fi
codesign --verify --deep --strict "$desired"

/usr/bin/osascript -e 'tell application id "io.freerdp.shadow.sonoma.menu" to quit'
for attempt in {1..30}; do
	pgrep -x FreeRDPShadowMenu >/dev/null || break
	sleep 1
done
if pgrep -x FreeRDPShadowMenu >/dev/null; then
	print -u2 "Menu app did not quit; installed app has not been changed"
	exit 1
fi
mv "$installed" "$staging"
if ! ditto "$desired" "$installed" ||
   ! codesign --verify --deep --strict "$installed" ||
   ! open "$installed"; then
	print -u2 "Swap failed; restoring the previously installed app"
	rm -rf "$installed"
	mv "$staging" "$installed"
	open "$installed" || true
	exit 1
fi
rm -rf "$staging"
print "Installed $1; signed 0.2.0 backup: $backup"
print "Reconnect the same phone RDP session through SSH before testing."
