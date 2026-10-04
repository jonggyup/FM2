#!/usr/bin/env bash
set -euo pipefail

# Reserve DRAM with memmap=<size>!<physical-address> and expose it as Device DAX.
# Defaults: 30G from NUMA node 0.
#
#   sudo ./setup-devdax.sh                  # 30G, node 0
#   sudo ./setup-devdax.sh 64G              # 64G, node 0
#   sudo ./setup-devdax.sh 64G --node 2     # 64G, node 2
#   sudo ./setup-devdax.sh --check
#   sudo ./setup-devdax.sh --restore

GRUB_FILE=/etc/default/grub
BACKUP_FILE=/etc/default/grub.devdax-backup
ALIGN=$((2 * 1024 * 1024))
DEFAULT_SIZE=30G
DEFAULT_NODE=0

info() { echo "[*] $*"; }
die()  { echo "[ERROR] $*" >&2; exit 1; }
require_root() { [[ $EUID -eq 0 ]] || die "Run this script as root."; }

update_grub() {
    if command -v update-grub >/dev/null 2>&1; then
        update-grub
    elif command -v grub2-mkconfig >/dev/null 2>&1; then
        [[ -f /boot/grub2/grub.cfg ]] && { grub2-mkconfig -o /boot/grub2/grub.cfg; return; }
        [[ -f /boot/efi/EFI/ubuntu/grub.cfg ]] && { grub2-mkconfig -o /boot/efi/EFI/ubuntu/grub.cfg; return; }
        die "Cannot determine grub.cfg path."
    else
        die "Neither update-grub nor grub2-mkconfig exists."
    fi
}

size_to_bytes() {
    python3 - "$1" <<'PY'
import re, sys
m = re.fullmatch(r'(\d+)([KMGT]?)', sys.argv[1].strip().upper())
if not m:
    raise SystemExit("Invalid size; examples: 512M, 30G, 80G")
n, u = int(m.group(1)), m.group(2)
scale = {'':1, 'K':1024, 'M':1024**2, 'G':1024**3, 'T':1024**4}
print(n * scale[u])
PY
}

# Find the highest aligned contiguous System RAM range of SIZE on NODE.
find_ram_range_on_node() {
    local size_bytes=$1 node=$2

    python3 - "$size_bytes" "$ALIGN" "$node" <<'PY'
import glob, os, re, sys

need, align, node = map(int, sys.argv[1:])
node_dir = f"/sys/devices/system/node/node{node}"
if not os.path.isdir(node_dir):
    raise SystemExit(f"NUMA node {node} does not exist")

with open('/sys/devices/system/memory/block_size_bytes') as f:
    block_size = int(f.read().strip(), 16)

blocks = []
for p in glob.glob(node_dir + '/memory[0-9]*'):
    m = re.search(r'memory(\d+)$', p)
    if not m:
        continue
    idx = int(m.group(1))
    state = f'/sys/devices/system/memory/memory{idx}/state'
    try:
        if open(state).read().strip() != 'online':
            continue
    except OSError:
        pass
    blocks.append((idx * block_size, (idx + 1) * block_size))

if not blocks:
    raise SystemExit(f"No online memory blocks found for NUMA node {node}")

blocks.sort()
node_ranges = []
for s, e in blocks:
    if node_ranges and s <= node_ranges[-1][1]:
        node_ranges[-1] = (node_ranges[-1][0], max(node_ranges[-1][1], e))
    else:
        node_ranges.append((s, e))

ram = []
with open('/proc/iomem') as f:
    for line in f:
        if line.startswith(' ') or ': System RAM' not in line:
            continue
        try:
            a, b = line.split(':', 1)[0].strip().split('-')
            ram.append((int(a, 16), int(b, 16) + 1))
        except ValueError:
            pass

candidates = []
for ns, ne in node_ranges:
    for rs, re_ in ram:
        s, e = max(ns, rs), min(ne, re_)
        if s < e:
            candidates.append((s, e))

for start, end in sorted(candidates, key=lambda x: x[1], reverse=True):
    aligned_end = end & ~(align - 1)
    candidate_start = (aligned_end - need) & ~(align - 1)
    candidate_end = candidate_start + need
    if candidate_start >= start and candidate_end <= end:
        print(hex(candidate_start), hex(candidate_end), hex(start), hex(end))
        sys.exit(0)

print('NONE NONE NONE NONE')
PY
}

modify_grub() {
    local param=$1
    [[ -f $GRUB_FILE ]] || die "$GRUB_FILE does not exist."

    if [[ ! -f $BACKUP_FILE ]]; then
        cp -a "$GRUB_FILE" "$BACKUP_FILE"
        info "Saved original GRUB config to $BACKUP_FILE"
    fi

    python3 - "$GRUB_FILE" "$param" <<'PY'
import re, sys
path, new_param = sys.argv[1], sys.argv[2]
text = open(path).read()
m = re.search(r'^GRUB_CMDLINE_LINUX_DEFAULT="([^"]*)"', text, re.M)
if not m:
    raise SystemExit('GRUB_CMDLINE_LINUX_DEFAULT not found')
args = [x for x in m.group(1).split()
        if not x.startswith('memmap=') and not x.startswith('efi_fake_mem=')]
args.append(new_param)
replacement = 'GRUB_CMDLINE_LINUX_DEFAULT="' + ' '.join(args) + '"'
text = text[:m.start()] + replacement + text[m.end():]
open(path, 'w').write(text)
PY
}

check_system() {
    echo "=== Kernel command line ==="
    cat /proc/cmdline
    echo
    echo "=== NUMA topology ==="
    command -v numactl >/dev/null && numactl -H || true
    echo
    echo "=== Device DAX ==="
    if command -v daxctl >/dev/null; then
        daxctl list
    else
        ls -l /dev/dax* 2>/dev/null || echo "No /dev/dax* found"
    fi
    echo
    echo "=== Relevant /proc/iomem ==="
    grep -Ei 'System RAM|Soft Reserved|persistent|dax' /proc/iomem || true
}

restore() {
    require_root
    [[ -f $BACKUP_FILE ]] || die "No backup found at $BACKUP_FILE"
    cp -a "$BACKUP_FILE" "$GRUB_FILE"
    update_grub
    echo "Restored original GRUB configuration. Reboot to apply."
}

setup() {
    require_root

    local size=$DEFAULT_SIZE node=$DEFAULT_NODE
    if [[ $# -gt 0 && $1 != --* ]]; then size=$1; shift; fi
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --node) [[ $# -ge 2 ]] || die "--node requires a number"; node=$2; shift 2 ;;
            *) die "Unknown argument: $1" ;;
        esac
    done

    [[ $node =~ ^[0-9]+$ ]] || die "Invalid NUMA node: $node"
    local size_bytes
    size_bytes=$(size_to_bytes "$size")
    (( size_bytes % ALIGN == 0 )) || die "Size must be 2 MiB aligned."

    info "Selecting $size from NUMA node $node"
    local start end region_start region_end
    read -r start end region_start region_end <<< "$(find_ram_range_on_node "$size_bytes" "$node")"
    [[ $start != NONE ]] || die "No contiguous $size System RAM range found on NUMA node $node."

    printf '    NUMA node    : %s\n' "$node"
    printf '    source range : %s - 0x%x\n' "$region_start" "$((region_end - 1))"
    printf '    reserve      : %s - 0x%x (%s)\n' "$start" "$((end - 1))" "$size"

    local param="memmap=${size}!${start}"
    echo
    info "Installing kernel parameter: $param"
    modify_grub "$param"
    grep '^GRUB_CMDLINE_LINUX_DEFAULT=' "$GRUB_FILE"

    info "Updating GRUB"
    update_grub

    cat <<VERIFY_EOF

Done. Reboot, then verify:

    sudo reboot
    sudo $0 --check

Expected daxctl output should report target_node: $node.
VERIFY_EOF
}

case "${1:-}" in
    --check)   check_system ;;
    --restore) restore ;;
    *)         setup "$@" ;;
esac

