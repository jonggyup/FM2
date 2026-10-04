#!/usr/bin/env bash
# Combine YCSB Redis throughput samples into a workload-by-column CSV.

set -euo pipefail
shopt -s nullglob

default_pattern='fm2_redis_perf_wkld_*.dat'
output=''
files=()

usage() {
    cat <<EOF
Usage: ${0##*/} [-o OUTPUT.csv] [--] [FILE ...]

Extract "current ops/sec" samples from YCSB Redis logs. The output has an
elapsed_sec column followed by one column per workload. With no FILE arguments,
the script reads ${default_pattern}.
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
    awk '
        function fail(message) {
            print "error: " message > "/dev/stderr"
            failed = 1
            exit 1
        }

        FNR == 1 {
            file_count++
            current_file = file_count
            path[current_file] = FILENAME

            name = FILENAME
            sub(/^.*\//, "", name)
            sub(/^fm2_redis_(downtime_|perf_wkld_)/, "", name)
            sub(/\.dat$/, "", name)
            workload[current_file] = name
        }

        /Running[[:space:]]+workload[^[:space:]]+[[:space:]]+with/ {
            name = $0
            sub(/^.*Running[[:space:]]+/, "", name)
            sub(/[[:space:]]+with.*$/, "", name)
            workload[current_file] = name
        }

        /[0-9]+[[:space:]]+sec:.*;[[:space:]]*[0-9]+([.][0-9]+)?[[:space:]]+current ops\/sec/ {
            elapsed_text = $0
            match(elapsed_text, /[0-9]+[[:space:]]+sec:/)
            elapsed_text = substr(elapsed_text, RSTART, RLENGTH)
            sub(/[[:space:]]+sec:/, "", elapsed_text)
            elapsed = elapsed_text + 0

            throughput_text = $0
            match(throughput_text, /[0-9]+([.][0-9]+)?[[:space:]]+current ops\/sec/)
            throughput_text = substr(throughput_text, RSTART, RLENGTH)
            sub(/[[:space:]]+current ops\/sec/, "", throughput_text)

            key = current_file SUBSEP elapsed
            if (key in throughput) {
                fail(FILENAME ": duplicate sample for " elapsed " sec")
            }
            throughput[key] = throughput_text
            sample_count[current_file]++
            times[elapsed] = 1

            if (!have_time || elapsed < min_time) min_time = elapsed
            if (!have_time || elapsed > max_time) max_time = elapsed
            have_time = 1
        }

        END {
            if (failed) exit 1

            for (i = 1; i <= file_count; i++) {
                if (!sample_count[i]) {
                    fail(path[i] ": no throughput samples found; expected lines " \
                         "containing current ops/sec")
                }
                for (j = 1; j < i; j++) {
                    if (workload[i] == workload[j]) {
                        fail("duplicate workload name " workload[i] \
                             " (from " path[i] ")")
                    }
                }
            }

            printf "elapsed_sec"
            for (i = 1; i <= file_count; i++) printf ",%s", workload[i]
            print ""

            for (elapsed = min_time; elapsed <= max_time; elapsed++) {
                if (!(elapsed in times)) continue
                printf "%d", elapsed
                for (i = 1; i <= file_count; i++) {
                    key = i SUBSEP elapsed
                    if (key in throughput) printf ",%s", throughput[key]
                    else printf ","
                }
                print ""
            }
        }
    ' "${files[@]}"
}

if [[ -n $output ]]; then
    extract >"$output"
else
    extract
fi
