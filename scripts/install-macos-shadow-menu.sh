#!/bin/zsh
set -eu

script_dir="${0:A:h}"
repo_root="${script_dir:h}"
built_app="${repo_root}/dist/FreeRDP Shadow.app"
app_dir="${HOME}/Applications/FreeRDP Shadow.app"
contents_dir="${app_dir}/Contents"
macos_dir="${contents_dir}/MacOS"

if [[ "${1:-}" == "--uninstall" ]]; then
	if [[ ! -d "${app_dir}" ]]; then
		print "FreeRDP Shadow is not installed."
		exit 0
	fi
	if [[ -x "${macos_dir}/FreeRDPShadowMenu" ]]; then
		"${macos_dir}/FreeRDPShadowMenu" --unregister-login-item || true
	fi
	/usr/bin/osascript -e 'tell application id "io.freerdp.shadow.sonoma.menu" to quit' \
		>/dev/null 2>&1 || true
	sleep 1
	trash_path="${HOME}/.Trash/FreeRDP Shadow-$(date +%Y%m%d-%H%M%S).app"
	mv "${app_dir}" "${trash_path}"
	print "Moved the app to ${trash_path}"
	print "The server log was preserved at ${HOME}/Library/Logs/FreeRDPShadow/server.log"
	exit 0
fi

if pgrep -x FreeRDPShadowMenu >/dev/null 2>&1; then
	print -u2 "FreeRDP Shadow is already running. Choose 'Quit FreeRDP Shadow' from its menu,"
	print -u2 "then run this installer again."
	exit 1
fi

python3 "${script_dir}/build-macos-shadow-app.py"
mkdir -p "${app_dir:h}"
ditto "${built_app}" "${app_dir}"
codesign --verify --deep --strict "${app_dir}"
"${macos_dir}/FreeRDPShadowMenu" --register-login-item

print "Installed ${app_dir}"
print "Opening the menu-bar app with automatic client resolution and cursor profiles."
print "Approve its Login Item if macOS requests it."
open "${app_dir}"
