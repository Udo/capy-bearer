#!/usr/bin/env bash
set -euo pipefail

repo_root=$(git rev-parse --show-toplevel)
cd "$repo_root"

out_dir="${1:-changelog}"
mkdir -p "$out_dir"
rm -f "$out_dir"/*.log

while IFS= read -r commit; do
	commit_date=$(git show -s --format=%cs "$commit")
	out_file="$out_dir/$commit_date.log"
	{
		printf 'commit %s\n' "$commit"
		git show -s --format=%B "$commit"
		printf '\n'
	} >> "$out_file"
done < <(git rev-list --reverse HEAD)

printf 'Wrote changelog files to %s\n' "$out_dir"
