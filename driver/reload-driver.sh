#!/usr/bin/env bash
set -euo pipefail
rme_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
module="$rme_dir/babyface-pro-linux/tools/kernel/snd-usb-babyface-pro.ko"
[[ -f "$module" ]] || { echo 'Build the driver first with just driver-build.' >&2; exit 1; }
read -r built_kernel _ <<< "$(modinfo -F vermagic "$module")"
[[ "$built_kernel" == "$(uname -r)" ]] || { echo 'Module does not match running kernel.' >&2; exit 1; }
expected=$(modinfo -F srcversion "$module")
[[ -n "$expected" ]]
printf '%s\n' 'Close TuxMix and audio apps. Power speakers OFF and disconnect headphones.'
read -r -p 'Type yes when ready to reload (outputs will remain muted): ' reply
[[ "$reply" == yes ]] || { echo 'Cancelled.'; exit 0; }
# Authenticate before interrupting audio.
sudo -v
state_dir=$(mktemp -d "$rme_dir/rme-eval-tools/state/reload-state-XXXXXXXX")
units=(wireplumber.service pipewire-pulse.socket pipewire.socket pipewire-pulse.service pipewire.service)
active=()
for unit in "${units[@]}"; do
    if systemctl --user is-active --quiet "$unit"; then active+=("$unit"); fi
done
printf '%s\n' "${active[@]}" > "$state_dir/active-services.txt"
if [[ -d /sys/module/snd_usb_babyface_pro ]]; then
    alsactl -f "$state_dir/mixer-before.state" store BabyfacePro
fi
mute() {
    amixer -q -c BabyfacePro cset name='AN1/2 Playback Switch' off
    amixer -q -c BabyfacePro cset name='PH3/4 Playback Switch',index=1 off
}
failed() {
    local rc=$?
    trap - EXIT
    if ((rc != 0)); then
        if [[ -d /sys/module/snd_usb_babyface_pro ]]; then mute || true; fi
        echo "Reload did not complete. Keep speakers off. Saved state: $state_dir" >&2
        echo 'Audio services may remain stopped; no forced unload or automatic mixer restore was attempted.' >&2
    fi
    exit "$rc"
}
trap failed EXIT
systemctl --user stop "${units[@]}"
if [[ -d /sys/module/snd_usb_babyface_pro ]]; then
    mute
    sudo rmmod snd_usb_babyface_pro
fi
sudo insmod "$module" frames_per_urb=32 nurbs=8
sudo udevadm settle
mute
[[ $(cat /sys/module/snd_usb_babyface_pro/srcversion) == "$expected" ]]
if ((${#active[@]})); then systemctl --user start "${active[@]}"; fi
mute
amixer -c BabyfacePro cget name='AN1/2 Playback Switch'
amixer -c BabyfacePro cget name='PH3/4 Playback Switch',index=1
printf '%s\n' "Loaded $expected. Mixer snapshot: $state_dir" 'Check routing and levels in TuxMix before unmuting. This load is temporary, not installed for boot.'
