#!/bin/bash
# Script to set performance settings for an AMD CPU

set -e

# --- Disable SMT (AMD's equivalent of Hyper-Threading) ---
if [ -e "/sys/devices/system/cpu/smt/control" ]; then
    if [ "$(cat /sys/devices/system/cpu/smt/control)" == "on" ]; then
        echo "Disabling SMT..."
        sudo bash -c "echo off > /sys/devices/system/cpu/smt/control"
    else
        echo "SMT is already off."
    fi
else
    echo "SMT control not found."
fi

# --- Check for AMD CPU frequency driver ---
if [ -e "/sys/devices/system/cpu/amd_pstate" ]; then
    echo "AMD pstate driver detected."
elif [ -e "/sys/devices/system/cpu/cpufreq/policy0/scaling_driver" ] && \
     [ "$(cat /sys/devices/system/cpu/cpufreq/policy0/scaling_driver)" == "acpi-cpufreq" ]; then
    echo "acpi-cpufreq driver detected."
else
    echo "Warning: Could not confirm AMD pstate or acpi-cpufreq driver."
    echo "Will attempt to proceed with generic cpufreq controls."
fi

# --- Disable CPU Boost (Turbo) ---
# This is the AMD/generic equivalent of Intel's 'no_turbo'
if [ -e "/sys/devices/system/cpu/cpufreq/boost" ]; then
    echo "Disabling CPU Boost..."
    sudo bash -c "echo 0 > /sys/devices/system/cpu/cpufreq/boost"
else
    echo "Warning: CPU boost control not found."
fi

# --- Silent pushd and popd (unchanged) ---
pushd () {
    command pushd "$@" > /dev/null
}
popd () {
    command popd "$@" > /dev/null
}

# --- Set Governor and Lock Frequencies for all Cores ---
echo "Setting performance governor and locking frequencies..."
pushd /sys/devices/system/cpu

# Use a more robust glob to match all cpus
for CORE in cpu[0-9]*; do
    # Skip if $CORE is not a directory
    [ ! -d "$CORE" ] && continue

    pushd $CORE

    if [ -e "online" ] && [ "$(cat online)" == "0" ]; then
        popd
        continue
    fi

    # Check if cpufreq directory exists
    if [ ! -d "cpufreq" ]; then
        popd
        continue
    fi

    pushd cpufreq

    if [ -e "cpuinfo_max_freq" ]; then
        MAX_FREQ=$(cat cpuinfo_max_freq)
        sudo bash -c "echo 'performance' > scaling_governor"
        sudo bash -c "echo $MAX_FREQ > scaling_max_freq"
        sudo bash -c "echo $MAX_FREQ > scaling_min_freq"
    else
        echo "Warning: Could not set frequencies for $CORE"
    fi

    popd # cpufreq
    popd # $CORE
done
popd # /sys/devices/system/cpu

# --- Disable CPU Idle C-States ---
echo "Disabling CPU idle C-states..."
for cpu in /sys/devices/system/cpu/cpu[0-9]*; do
    # Skip if not a directory
    [ ! -d "$cpu" ] && continue

    # Check if cpuidle directory exists
    if [ ! -d "$cpu/cpuidle" ]; then
        continue
    fi

    # Loop over each idle state for the current CPU
    for state in $cpu/cpuidle/state[0-9]*; do
        if [ -e "$state/disable" ]; then
            sudo bash -c "echo 1 > $state/disable"
        fi
    done
done

echo "Performance tuning complete."
