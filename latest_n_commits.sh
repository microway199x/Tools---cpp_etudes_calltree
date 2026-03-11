#!/bin/bash

usage() {
    echo "Usage: $0 [-n N] [-s since] [-c top_commit] [-f filter] <dir>"
    echo "  -n N:          number of latest commits to show (default: 10)"
    echo "  -s since:      only check files modified since this date (e.g. '2025-01-01', '3 months ago')"
    echo "  -c top_commit: branch/commit/tag as the latest commit (default: HEAD)"
    echo "  -f filter:     regex on commit messages; prefix with ! to negate"
    echo "  dir:           directory to scan"
    exit 1
}

N=10
SINCE=""
TOP_COMMIT="HEAD"
FILTER=""

while getopts "n:s:c:f:h" opt; do
    case "$opt" in
        n) N="$OPTARG" ;;
        s) SINCE="$OPTARG" ;;
        c) TOP_COMMIT="$OPTARG" ;;
        f) FILTER="$OPTARG" ;;
        h) usage ;;
        *) usage ;;
    esac
done
shift $((OPTIND - 1))

[ $# -ne 1 ] && usage
DIR="$1"

if [ ! -d "$DIR" ]; then
    echo "Error: '$DIR' is not a directory"
    exit 1
fi

if ! [[ "$N" =~ ^[0-9]+$ ]] || [ "$N" -le 0 ]; then
    echo "Error: N must be a positive integer"
    exit 1
fi

if ! git rev-parse --verify "$TOP_COMMIT^{commit}" >/dev/null 2>&1; then
    echo "Error: invalid revision '$TOP_COMMIT'"
    exit 1
fi

TMPDIR=$(mktemp -d)
trap "rm -rf $TMPDIR" EXIT

TOP_N="$TMPDIR/top_n.tsv"
touch "$TOP_N"

JOBS=$(nproc 2>/dev/null || echo 4)

# Parse since to epoch (threshold floor)
since_ts=0
if [ -n "$SINCE" ]; then
    since_ts=$(date -d "$SINCE" '+%s' 2>/dev/null)
    if [ $? -ne 0 ] || [ -z "$since_ts" ]; then
        echo "Error: invalid date '$SINCE'"
        exit 1
    fi
fi
threshold=$since_ts

# Parse filter: "!regex" means negate
negate_filter=0
filter_re="$FILTER"
if [ "${FILTER:0:1}" = "!" ]; then
    negate_filter=1
    filter_re="${FILTER:1}"
fi

# Commit message cache
declare -A msg_cache

get_msg() {
    local cid="$1"
    if [ -z "${msg_cache[$cid]+x}" ]; then
        msg_cache[$cid]=$(git log -1 --format='%s' "$cid" 2>/dev/null)
    fi
    printf '%s' "${msg_cache[$cid]}"
}

check_filter() {
    [ -z "$filter_re" ] && return 0
    local msg="$1"
    if [ "$negate_filter" -eq 1 ]; then
        ! [[ "$msg" =~ $filter_re ]]
    else
        [[ "$msg" =~ $filter_re ]]
    fi
}

# Phase 1: build file list sorted by recency
echo >&2 "Phase 1: scanning file recency..."

if [ -n "$SINCE" ]; then
    # Efficient: git log --since only walks recent history
    git log --since="$SINCE" "$TOP_COMMIT" --format='%at' --name-only -- "$DIR" 2>/dev/null | \
        awk '
        /^[0-9]+$/ { ts = $0; next }
        /^$/       { next }
        {
            if (!($0 in max_ts) || ts+0 > max_ts[$0]+0) max_ts[$0] = ts
        }
        END { for (f in max_ts) print max_ts[f] "\t" f }
        ' | sort -t$'\t' -k1,1 -rn > "$TMPDIR/file_list.tsv"
else
    # List all tracked files at TOP_COMMIT, get per-file latest timestamp in parallel
    git ls-tree -r --name-only "$TOP_COMMIT" -- "$DIR" 2>/dev/null | \
        xargs -P "$JOBS" -I{} bash -c '
            ts=$(git log -1 --format="%at" "$0" -- "$1" 2>/dev/null)
            [ -n "$ts" ] && printf "%s\t%s\n" "$ts" "$1"
        ' "$TOP_COMMIT" {} | \
        sort -t$'\t' -k1,1 -rn > "$TMPDIR/file_list.tsv"
fi

total=$(wc -l < "$TMPDIR/file_list.tsv")
echo >&2 "Phase 1 done: $total files"

# Phase 2: blame files in recency order, prune by threshold, apply filter
echo >&2 "Phase 2: collecting blame data with pruning..."
processed=0
skipped=0

while IFS=$'\t' read -r file_ts file_path; do
    # Prune: skip file if its latest commit is older than current threshold
    if [ "$threshold" -gt 0 ] && [ "$file_ts" -lt "$threshold" ]; then
        skipped=$((skipped + 1))
        continue
    fi

    processed=$((processed + 1))

    # Blame at TOP_COMMIT, filter blame lines by threshold
    git blame --porcelain "$TOP_COMMIT" -- "$file_path" 2>/dev/null | \
        awk -v thr="$threshold" '
        /^[0-9a-f]{40} / { commit = $1 }
        /^author /        { author = substr($0, 8) }
        /^author-time /   { timestamp = $2 }
        /^author-tz /     {
            if (commit != "" && commit !~ /^0+$/ && timestamp+0 > thr+0) {
                print timestamp "\t" commit "\t" author
            }
        }
        ' | sort -t$'\t' -k1,1 -rn | awk -F'\t' '!seen[$2]++' > "$TMPDIR/new.tsv"

    if [ -s "$TMPDIR/new.tsv" ]; then
        # Merge with global top-N, deduplicate, apply filter, keep top N
        cat "$TOP_N" "$TMPDIR/new.tsv" | \
            sort -t$'\t' -k1,1 -rn | \
            awk -F'\t' '!seen[$2]++' > "$TMPDIR/candidates.tsv"

        > "$TMPDIR/filtered.tsv"
        count=0
        while IFS=$'\t' read -r ts cid auth; do
            msg=$(get_msg "$cid")
            if check_filter "$msg"; then
                printf '%s\t%s\t%s\n' "$ts" "$cid" "$auth" >> "$TMPDIR/filtered.tsv"
                count=$((count + 1))
                [ "$count" -ge "$N" ] && break
            fi
        done < "$TMPDIR/candidates.tsv"

        mv "$TMPDIR/filtered.tsv" "$TOP_N"
        rm -f "$TMPDIR/new.tsv" "$TMPDIR/candidates.tsv"

        # Update threshold if we have N entries
        cur=$(wc -l < "$TOP_N")
        if [ "$cur" -ge "$N" ]; then
            new_thr=$(tail -1 "$TOP_N" | cut -f1)
            [ "$new_thr" -gt "$threshold" ] 2>/dev/null && threshold=$new_thr
        fi
    fi
done < "$TMPDIR/file_list.tsv"

echo >&2 "Phase 2 done: processed=$processed skipped=$skipped (total=$total)"

# Phase 3: format output with commit messages
while IFS=$'\t' read -r ts cid auth; do
    dt=$(date -d "@$ts" '+%Y-%m-%d %H:%M:%S' 2>/dev/null)
    msg=$(get_msg "$cid")
    printf '%s\t%s\t%s\t%s\n' "$cid" "$dt" "$auth" "$msg"
done < "$TOP_N"
