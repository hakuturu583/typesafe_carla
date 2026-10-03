#!/usr/bin/env bash
# Resolves a carla-simulator/carla ref (branch, tag or full commit SHA) to the
# 40-hex commit SHA it points at now, and prints it.
#
#   tools/resolve_carla_ref.sh <ref> [repository]
#
# CI builds LibCarla from the printed SHA, not from the (moving) ref, and keys
# the LibCarla build cache by it: the cache entry and the build always agree,
# a new ue5-dev commit misses the cache, and an unchanged one hits it.
# A full SHA is printed as given (no lookup); a short SHA is rejected, as git
# cannot fetch it.
set -euo pipefail

ref=${1:?usage: resolve_carla_ref.sh <ref> [repository]}
repo=${2:-https://github.com/carla-simulator/carla}

if [[ "$ref" =~ ^[0-9a-f]{40}$ ]]; then
  echo "$ref"
  exit 0
fi

# A branch, or a tag (an annotated tag's "^{}" line is the commit it tags).
out=$(git ls-remote "$repo" "refs/heads/${ref}" "refs/tags/${ref}" "refs/tags/${ref}^{}")
# Precedence: the peeled tag, then the branch, then the tag itself.
sha=$(awk -v r="$ref" '
  $2 == "refs/tags/" r "^{}" { peeled = $1 }
  $2 == "refs/heads/" r      { head = $1 }
  $2 == "refs/tags/" r       { tag = $1 }
  END { print (peeled != "" ? peeled : head != "" ? head : tag) }' <<<"$out")
if [ -z "$sha" ]; then
  echo "error: '${ref}' is not a branch or tag of ${repo} (or a full 40-hex commit SHA)" >&2
  exit 1
fi
echo "$sha"
