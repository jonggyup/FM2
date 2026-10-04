#!/usr/bin/env bash
set -euo pipefail

# check with 'cat /proc/cpuinfo | grep "MHz"'

# Disable SMT/Hyper-Threading (generic)
if [ -e /sys/devices/system/cpu/smt/control ] && [ "$(cat /sys/devices/system/cpu/smt/control)" = "on" ]; then
  sudo bash -c 'echo off > /sys/devices/system/cpu/smt/control'
fi

# Verify AMD frequency driver
DRV_FILE="/sys/devices/system/cpu/cpu0/cpufreq/scaling_driver"
if [ ! -e "$DRV_FILE" ]; then
  printf "No cpufreq driver found. Please ensure CPU frequency scaling is available/enabled.\n"
  exit 1
fi

DRV="$(cat "$DRV_FILE")"
case "$DRV" in
  amd-pstate|amd-pstate-epp|acpi-cpufreq)
    : # supported
    ;;
  *)
    printf "Unsupported scaling driver '%s'. Detected driver is not AMD's or ACPI. Please handle CPU freq manually.\n" "$DRV"
    exit 1
    ;;
esac

# Silent pushd/popd
pushd () { command pushd "$@" >/dev/null; }
popd  () { command popd  "$@" >/dev/null; }

# Set governor + clamp min/max to max on all online CPUs
for CPU in /sys/devices/system/cpu/cpu[0-9]*; do
  [ -e "$CPU/online" ] && [ "$(cat "$CPU/online")" = "0" ] && continue
  if [ -d "$CPU/cpufreq" ]; then
    pushd "$CPU/cpufreq"
      if [ -w scaling_governor ]; then
        sudo bash -c 'echo performance > scaling_governor'
      fi
      if [ -r cpuinfo_max_freq ]; then
        MAX="$(cat cpuinfo_max_freq)"
        [ -w scaling_max_freq ] && sudo bash -c "echo $MAX > scaling_max_freq"
        [ -w scaling_min_freq ] && sudo bash -c "echo $MAX > scaling_min_freq"
      fi
    popd
  fi
done

# If amd_pstate sysfs exists, set perf pcts to 100
if [ -d /sys/devices/system/cpu/amd_pstate ]; then
  pushd /sys/devices/system/cpu/amd_pstate
    [ -w max_perf_pct ] && sudo bash -c 'echo 100 > max_perf_pct'
    [ -w min_perf_pct ] && sudo bash -c 'echo 100 > min_perf_pct'
    # No 'no_turbo' here; boost handled below.
  popd
fi

# Disable Core Performance Boost (Turbo) on AMD (global cpufreq boost knob)
if [ -e /sys/devices/system/cpu/cpufreq/boost ]; then
  # 0 = boost off, 1 = boost on
  sudo bash -c 'echo 0 > /sys/devices/system/cpu/cpufreq/boost'
fi

# Disable all idle states (C-states) for every online CPU
for CPU in /sys/devices/system/cpu/cpu[0-9]*; do
  [ -e "$CPU/online" ] && [ "$(cat "$CPU/online")" = "0" ] && continue
  for STATE in "$CPU"/cpuidle/state[0-9]*; do
    [ -e "$STATE/disable" ] && echo 1 | sudo tee "$STATE/disable" >/dev/null
  done
done

