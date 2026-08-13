#!/bin/zsh
set -eu

script_dir="${0:A:h}"
repo_root="${script_dir:h}"
source_dir="${repo_root}/packaging/macos-shadow-menu"
server="${repo_root}/build-sonoma-shadow-p0-channels/server/shadow/cli/freerdp-shadow-cli"
app_dir="${HOME}/Applications/FreeRDP Shadow.app"
contents_dir="${app_dir}/Contents"
macos_dir="${contents_dir}/MacOS"
resources_dir="${contents_dir}/Resources"
config="${resources_dir}/ShadowConfig.plist"

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

if [[ ! -x "${server}" ]]; then
	print -u2 "Missing shadow server executable: ${server}"
	print -u2 "Build the freerdp-shadow-cli target before installing the menu app."
	exit 1
fi

for helper in /usr/local/bin/w981 /usr/local/bin/mac1080; do
	if [[ ! -x "${helper}" ]]; then
		print -u2 "Missing executable display helper: ${helper}"
		exit 1
	fi
done

mkdir -p "${macos_dir}" "${resources_dir}"

xcrun clang \
	-fobjc-arc \
	-Wall -Wextra -Wpedantic \
	-framework AppKit \
	-framework ApplicationServices \
	-framework CoreGraphics \
	-framework ServiceManagement \
	-o "${macos_dir}/FreeRDPShadowMenu" \
	"${source_dir}/FreeRDPShadowMenu.m"

cp "${source_dir}/Info.plist" "${contents_dir}/Info.plist"
cp "${source_dir}/ShadowConfig.plist" "${config}"
/usr/libexec/PlistBuddy -c "Add :ServerExecutable string ${server}" "${config}"
/usr/libexec/PlistBuddy -c "Add :ConnectDisplayCommand string /usr/local/bin/w981" "${config}"
/usr/libexec/PlistBuddy -c "Add :DisconnectDisplayCommand string /usr/local/bin/mac1080" "${config}"
/usr/libexec/PlistBuddy -c "Add :LogFile string ${HOME}/Library/Logs/FreeRDPShadow/server.log" \
	"${config}"

plutil -lint "${contents_dir}/Info.plist" "${config}"
codesign --force --deep --sign - --identifier io.freerdp.shadow.sonoma.menu "${app_dir}"
codesign --verify --deep --strict "${app_dir}"
"${macos_dir}/FreeRDPShadowMenu" --register-login-item

print "Installed ${app_dir}"
print "Opening the menu-bar app; approve its Login Item if macOS requests it."
open "${app_dir}"
