#!/usr/bin/env bash
# Usage:
#   ./extract_time_redis.sh            # scans fm2_redis_downtime_*.dat
#   ./extract_time_redis.sh files...   # non-Redis files are ignored
#
# Output (CSV to stdout):
#   workload,downtime_s,migration_s,file

set -euo pipefail
shopt -s nullglob

# Collect inputs. Restrict the set by basename so VoltDB/TPCC downtime logs can
# never be included, even when they are supplied explicitly.
if [[ $# -gt 0 ]]; then
  candidates=("$@")
else
  candidates=(fm2_redis_downtime_*.dat)
fi

files=()
for candidate in "${candidates[@]}"; do
  bn=$(basename -- "$candidate")
  if [[ $bn == fm2_redis_downtime_*.dat ]]; then
    files+=("$candidate")
  else
    echo "Skipping non-Redis timing file: $candidate" >&2
  fi
done

if [[ ${#files[@]} -eq 0 ]]; then
  echo "No Redis timing files found (expected fm2_redis_downtime_*.dat)." >&2
  exit 1
fi

echo "workload,downtime_s,migration_s,file"

for f in "${files[@]}"; do
  [[ -f "$f" ]] || { echo "Skipping non-file: $f" >&2; continue; }

  bn=$(basename -- "$f")
  wkld=${bn#fm2_redis_downtime_}
  wkld=${wkld%.dat}

  awk -v wk="$wkld" -v file="$f" '
    /^vm downtime:/     { dow = $3 }
    /^Migration start:/ { st  = $3 }
    /^Migration end:/   { en  = $3 }
    END {
      if (dow == "" || st == "" || en == "") {
        printf("Error: missing fields in %s (wkld=%s)\n", file, wk) > "/dev/stderr";
        exit 2
      }
      dow_s  = dow / 1e9
      diff_s = (en - st) / 1e9
      printf("%s,%.2f,%.2f,%s\n", wk, dow_s, diff_s, file)
    }
  ' "$f"
done
