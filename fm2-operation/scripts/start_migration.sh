#!/bin/bash
source config.txt

# Path to the QEMU monitor Unix socket
MONITOR_SOCKET="./qemu-monitor-migration"

# Use socat to send the command to the QEMU monitor and read the response
echo "migrate_set_capability postcopy-ram on" | socat stdio unix-connect:"$MONITOR_SOCKET"
echo "migrate_set_capability postcopy-preempt on" | socat stdio unix-connect:"$MONITOR_SOCKET"
echo "migrate_set_parameter max-bandwidth 1342177280B" | socat stdio unix-connect:"$MONITOR_SOCKET"
echo "migrate_set_parameter max-postcopy-bandwidth 2684354560" | socat stdio unix-connect:"$MONITOR_SOCKET"
echo "migrate -d tcp:$DST_IP:$MIGRATION_PORT" | socat stdio unix-connect:"$MONITOR_SOCKET"
echo "migrate_start_postcopy" | socat stdio unix-connect:"$MONITOR_SOCKET"

# migrate_set_parameter max-postcopy-bandwidth 1342177280
# migrate_set_parameter max-bandwidth 1342177280
# migrate_incoming tcp:0:4444


sleep 5
echo "info migrate" | socat stdio unix-connect:"$MONITOR_SOCKET"
echo""