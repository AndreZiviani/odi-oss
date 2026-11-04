#!/usr/bin/env bash
#
# Refuse a release whose commit is not on the protected branch.
#
#   tools/check-release-ancestry.sh [<commit> [<branch>]]
#
# <commit> defaults to HEAD, <branch> to main. Passes only when <commit> is
# an ancestor of (or equal to) origin/<branch>, after a bounded fetch of that
# branch. Final tags and pre-release tags (v1.2.2-beta.1) are both cut from
# commits that main already contains, so one rule covers both.
#
# The fetch is shallow-safe: a runner checkout is depth 1 by default, which
# cannot answer an ancestry question, so the branch history is fetched in
# full first. ODI_ANCESTRY_FETCH_TIMEOUT (seconds, default 120) bounds it.
# ODI_ANCESTRY_NO_FETCH=1 skips the fetch (the host-side test uses it).
set -euo pipefail

commit=${1:-HEAD}
branch=${2:-main}
limit=${ODI_ANCESTRY_FETCH_TIMEOUT:-120}

sha=$(git rev-parse --verify --quiet "$commit^{commit}") || {
	echo "check-release-ancestry: $commit is not a commit in this checkout" >&2
	exit 1
}

if [ "${ODI_ANCESTRY_NO_FETCH:-0}" != 1 ]; then
	if [ "$(git rev-parse --is-shallow-repository)" = true ]; then
		deepen=(--unshallow)
	else
		deepen=()
	fi
	timeout "$limit" git fetch --no-tags --quiet "${deepen[@]}" origin \
		"+refs/heads/$branch:refs/remotes/origin/$branch" || {
		echo "check-release-ancestry: could not fetch origin/$branch within ${limit}s" >&2
		exit 1
	}
fi

git rev-parse --verify --quiet "refs/remotes/origin/$branch^{commit}" >/dev/null || {
	echo "check-release-ancestry: origin/$branch does not exist" >&2
	exit 1
}

if git merge-base --is-ancestor "$sha" "refs/remotes/origin/$branch"; then
	echo "check-release-ancestry: $sha is on origin/$branch"
else
	echo "check-release-ancestry: $sha is NOT an ancestor of origin/$branch -- refusing to release it" >&2
	echo "check-release-ancestry: merge the change into $branch first, then tag a commit that $branch contains" >&2
	exit 1
fi
