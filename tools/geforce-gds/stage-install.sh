#!/usr/bin/env bash
# Stage a built prototype; verification runs after the deployment's own reboot.
set -euo pipefail
here=$(cd -- "$(dirname -- "$0")" && pwd)
[ "$EUID" -eq 0 ] || { echo 'Run stage-install.sh as root' >&2; exit 1; }
prefix=/opt/geforce-gds-prototype
state=/var/lib/geforce-gds-prototype
module="$here/vendor-nvidia-fs/src/nvidia-fs.ko"
test -x "$here/gds_probe"
[ "$(modinfo -F vermagic "$module" | cut -d' ' -f1)" = "$(uname -r)" ]
module_dest="/lib/modules/$(uname -r)/updates/gds-prototype/nvidia-fs.ko"
backup=$(mktemp -d /var/backups/geforce-gds-prototype.XXXXXX)
for file in "$prefix" "$module_dest" /etc/modprobe.d/geforce-gds-prototype.conf /etc/modules-load.d/geforce-gds-prototype.conf /etc/systemd/system/geforce-gds-verify.service; do
    if [ -e "$file" ]; then cp --parents -a "$file" "$backup/"; fi
done
install -d "$prefix" "$state" "$(dirname -- "$module_dest")"
install -m 755 "$here/gds_probe" "$prefix/gds_probe"
for file in run_verified.sh verify_run.py make_fixture.py strict.json README.md verify-boot.sh; do
    install -m 644 "$here/$file" "$prefix/$file"
done
install -m 644 "$module" "$module_dest"
depmod -a "$(uname -r)"
printf '%s\n' 'options nvidia_fs rw_stats_enabled=1 peer_stats_enabled=1' > /etc/modprobe.d/geforce-gds-prototype.conf
printf '%s\n' nvidia_fs > /etc/modules-load.d/geforce-gds-prototype.conf
install -m 644 "$here/geforce-gds-verify.service" /etc/systemd/system/geforce-gds-verify.service
touch "$state/pending"
rm -f "$state/verified"
systemctl daemon-reload
systemctl enable geforce-gds-verify.service
printf 'GDS staged, NOT yet verified. Backups: %s\nVerification runs at the next boot; this script does not reboot.\n' "$backup"
