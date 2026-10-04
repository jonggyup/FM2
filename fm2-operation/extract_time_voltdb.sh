#!/usr/bin/env bash
# Extract VoltDB downtime and total migration time into CSV.

set -euo pipefail
shopt -s nullglob

default_pattern='fm2_voltdb_downtime_wkld_*.dat'
output=''
files=()

usage() {
    cat <<EOF
Usage: ${0##*/} [-o OUTPUT.csv] [--] [FILE ...]

Extract vm downtime and migration duration from VoltDB controller logs.
Output columns: workload,downtime_s,migration_s,file
With no FILE arguments, the script reads ${default_pattern}.
EOF
}

while (($#)); do
    case $1 in
        -h|--help)
            usage
            exit 0
            ;;
        -o|--output)
            if (($# < 2)); then
                echo "error: $1 requires a path" >&2
                exit 2
            fi
            output=$2
            shift 2
            ;;
        --output=*)
            output=${1#*=}
            shift
            ;;
        --)
            shift
            files+=("$@")
            break
            ;;
        -*)
            echo "error: unknown option: $1" >&2
            usage >&2
            exit 2
            ;;
        *)
            files+=("$1")
            shift
            ;;
    esac
done

if ((${#files[@]} == 0)); then
    files=($default_pattern)
fi

if ((${#files[@]} == 0)); then
    echo "error: no input files matched '$default_pattern'" >&2
    exit 1
fi

for file in "${files[@]}"; do
    if [[ ! -f $file ]]; then
        echo "error: not a regular file: $file" >&2
        exit 1
    fi
    if [[ ! -s $file ]]; then
        echo "error: input file is empty: $file" >&2
        exit 1
    fi
    if [[ -n $output && $file == "$output" ]]; then
        echo "error: output path is also an input file: $file" >&2
        exit 1
    fi
done

extract() {
    echo "workload,downtime_s,migration_s,file"

    local file base workload
    for file in "${files[@]}"; do
        base=${file##*/}
        case $base in
            fm2_voltdb_downtime_wkld_*.dat)
                workload=${base#fm2_voltdb_downtime_wkld_}
                workload=${workload%.dat}
                ;;
            fm2_voltdb_downtime_*.dat)
                workload=${base#fm2_voltdb_downtime_}
                workload=${workload%.dat}
                ;;
            fm2_voltdb_downtime.dat)
                workload=tpcc
                ;;
            *)
                workload=${base%.dat}
                ;;
        esac

        awk -v workload="$workload" -v file="$file" '
            /^[[:space:]]*vm downtime:/     { downtime = $3 }
            /^[[:space:]]*Migration start:/ { start = $3 }
            /^[[:space:]]*Migration end:/   { end = $3 }
            END {
                if (downtime == "" || start == "" || end == "") {
                    printf "error: missing timing fields in %s (workload=%s)\n", \
                           file, workload > "/dev/stderr"
                    exit 2
                }
                printf "%s,%.9f,%.9f,%s\n", workload, downtime / 1e9, \
                       (end - start) / 1e9, file
            }
        ' "$file"
    done
}

if [[ -n $output ]]; then
    extract >"$output"
else
    extract
fi
