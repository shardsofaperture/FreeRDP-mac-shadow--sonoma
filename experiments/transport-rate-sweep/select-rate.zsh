#!/bin/zsh
# Local-console manual swap. Never called by build, tests, or capture.
emulate -L zsh
setopt pipefail

if (( $# != 1 )) || [[ ! "$1" =~ '^(150|200|250|300|400)$' ]]; then
  print -u2 'Usage: zsh select-rate.zsh {150|200|250|300|400}'
  exit 2
fi
rate="$1"
repo='/Users/zach/gitr/RDP/FreeRDP-mac-shadow--sonoma'
source="$repo/dist/FreeRDP Shadow 0.2.0-RateSweep.app"
installed='/Users/zach/Applications/FreeRDP Shadow.app'
label='0.2.0-RateSweep — select fixed rate at launch'
identity='Apple Development: shardsofaperture (H7V72A5WH6)'
bundle_id='io.freerdp.shadow.sonoma.menu'

plist_value() {
  /usr/libexec/PlistBuddy -c "Print :$2" "$1"
}
verify_bundle() {
  local app="$1" config="$1/Contents/Resources/ShadowConfig.plist"
  local info="$1/Contents/Info.plist" details
  codesign --verify --deep --strict "$app" || return 1
  details=$(codesign -dv --verbose=4 "$app" 2>&1) || return 1
  [[ "$details" == *"Authority=$identity"* && "$details" == *'TeamIdentifier=66GMSP473V'* ]] || return 1
  [[ "$(plist_value "$info" CFBundleIdentifier)" == "$bundle_id" &&
     "$(plist_value "$info" FreeRDPShadowBuildLabel)" == "$label" &&
     "$(plist_value "$config" BuildLabel)" == "$label" &&
     "$(plist_value "$config" ExperimentalArm)" == RateSweep &&
     "$(plist_value "$config" GraphicsPacingMode)" == FixedRuntime &&
     "$(plist_value "$config" GraphicsRateKiBPerSecond)" == 0 &&
     "$(plist_value "$config" SocketSendBufferKiB)" == 0 &&
     "$(plist_value "$config" LargeRefreshBurst)" == false &&
     "$(plist_value "$config" CoverageScheduler)" == false &&
     "$(plist_value "$config" ListenerAddress)" == '127.0.0.1' &&
     "$(plist_value "$config" ListenerPort)" == 3390 ]]
}

[[ -d "$source" && -d "$installed" ]] || { print -u2 'Source or installed app missing'; exit 1; }
shasum -a 256 -c "$repo/dist/FreeRDP Shadow 0.2.0-RateSweep.sha256.txt" || exit 1
verify_bundle "$source" || { print -u2 'Source bundle verification failed'; exit 1; }

stage_dir=$(mktemp -d "$HOME/Applications/.rate-sweep-stage.XXXXXXXX") || exit 1
staged="$stage_dir/FreeRDP Shadow.app"
ditto "$source" "$staged" || exit 1
verify_bundle "$staged" || { print -u2 'Staged bundle verification failed'; exit 1; }
backup="$HOME/Applications/FreeRDP Shadow.before-rate-$(date +%Y%m%d-%H%M%S).app"
[[ ! -e "$backup" ]] || exit 1
print -r -- "Staged and signed: $label"
print -r -- "Selected rate: $rate KiB/s = $(( rate * 1024 )) B/s"
print -r -- "Bundle config: FixedRuntime, SO_SNDBUF default, burst false, coverage false, 127.0.0.1:3390"
print -r -- "Current app backup: $backup"

osascript -e "tell application id \"$bundle_id\" to quit" || exit 1
for ((i=0; i<30; i++)); do
  if ! lsof -nP -t -iTCP@127.0.0.1:3390 -sTCP:LISTEN >/dev/null 2>&1 &&
     ! pgrep -f "^${installed}/Contents/MacOS/(FreeRDPShadowMenu|freerdp-shadow-cli)( |$)" >/dev/null; then
    break
  fi
  sleep 1
done
if lsof -nP -t -iTCP@127.0.0.1:3390 -sTCP:LISTEN >/dev/null 2>&1 ||
   pgrep -f "^${installed}/Contents/MacOS/(FreeRDPShadowMenu|freerdp-shadow-cli)( |$)" >/dev/null; then
  print -u2 'Old app or listener did not stop; staged app and installed app are untouched.'
  exit 1
fi

mv "$installed" "$backup" || exit 1
if ! mv "$staged" "$installed"; then
  mv "$backup" "$installed"
  exit 1
fi
print -r -- "Installed BuildLabel: $(plist_value "$installed/Contents/Resources/ShadowConfig.plist" BuildLabel)"
print -r -- "Installed rate config: $(plist_value "$installed/Contents/Resources/ShadowConfig.plist" GraphicsPacingMode); selected launch argument: --rate-sweep-kib=$rate"
if ! verify_bundle "$installed"; then
  print -u2 'Installed signature/config failed; restoring backup without launching.'
  mv "$installed" "$stage_dir/rejected.app" && mv "$backup" "$installed"
  exit 1
fi
rmdir "$stage_dir" 2>/dev/null || true
open -n "$installed" --args "--rate-sweep-kib=$rate" || exit 1
for ((i=0; i<30; i++)); do
  pid=$(lsof -nP -t -iTCP@127.0.0.1:3390 -sTCP:LISTEN 2>/dev/null | head -n 1)
  if [[ -n "$pid" ]]; then
    process=$(ps -p "$pid" -o command=)
    menu=$(pgrep -fl 'FreeRDPShadowMenu' | rg -F "$installed/Contents/MacOS/FreeRDPShadowMenu" || true)
    if [[ "$process" == "$installed/Contents/MacOS/freerdp-shadow-cli"* &&
          "$menu" == *"--rate-sweep-kib=$rate"* ]]; then
      print -r -- "Verified installed listener PID $pid: $process"
      print -r -- "Verified menu: $menu"
      print -r -- "Preserved prior app: $backup"
      exit 0
    fi
    print -u2 -- "Unexpected listener or menu: $process | $menu"
    exit 1
  fi
  sleep 1
done
print -u2 -- "Installed app failed to start listener; backup retained: $backup"
exit 1
