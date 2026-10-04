#!/usr/bin/env bash
set -euo pipefail

KERNEL_DIR="${1:-fm2-kernel}"

echo "[1/4] Copying config from currently running kernel..."
RUNNING_KERNEL="$(uname -r)"
BOOT_CONFIG="/boot/config-${RUNNING_KERNEL}"

if [[ ! -f "$BOOT_CONFIG" ]]; then
    echo "ERROR: $BOOT_CONFIG does not exist"
    exit 1
fi

cp "$BOOT_CONFIG" .config

echo "[2/4] Removing Ubuntu/Canonical certificate references..."

scripts/config --file .config \
    --set-str SYSTEM_TRUSTED_KEYS ""

scripts/config --file .config \
    --set-str SYSTEM_REVOCATION_KEYS ""


./scripts/config --enable EFI
./scripts/config --enable EFI_STUB
./scripts/config --enable EFI_FAKE_MEMMAP
./scripts/config --enable EFI_SOFT_RESERVE

./scripts/config --enable DAX
./scripts/config --enable DEV_DAX
./scripts/config --enable DEV_DAX_HMEM
./scripts/config --enable DEV_DAX_HMEM_DEVICES
scripts/config --enable CONFIG_BLK_DEV_PMEM
scripts/config --enable CONFIG_DEV_DAX_PMEM

echo "[3/4] Updating config for this kernel source tree..."
make olddefconfig

echo "[4/4] Verifying certificate settings..."
grep -E \
    'CONFIG_SYSTEM_TRUSTED_KEYS|CONFIG_SYSTEM_REVOCATION_KEYS' \
    .config || true




echo
echo "Done."
echo "Config copied from: $BOOT_CONFIG"
echo "New config: $(pwd)/.config"
echo
echo "You can now build with:"
echo "  make -j\$(nproc)"
