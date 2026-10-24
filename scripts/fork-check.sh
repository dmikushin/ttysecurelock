#!/bin/bash
# Fork discipline check: ttysecurelock must change only what X forces.
# Lists every file changed since upstream xsecurelock v1.9.0 (the restart
# commit 2cf9729, an unchanged copy of v1.9.0) and fails if a file outside the
# whitelist (the console platform layer, build glue, docs, tests, scripts)
# differs: upstream code such as main.c and helpers/*.c must stay as it is.
# Usage: scripts/fork-check.sh [BASE]   (default 2cf9729); checks the work tree.
set -u
cd "$(dirname "$0")/.."
base=${1:-2cf9729}
whitelist='^(xconsole/|test/console/|docs/|scripts/|configure\.ac$|Makefile\.am$|\.gitignore$)'
status=0
echo "Changes since $base (work tree, untracked files included):"
changed=$( (git diff --name-only "$base"; git ls-files --others --exclude-standard) | sort -u)
for f in $changed; do
  if [[ $f =~ $whitelist ]]; then
    echo "  ok      $f"
  else
    echo "  FORBIDDEN $f"
    git --no-pager diff "$base" -- "$f" | sed 's/^/      /'
    status=1
  fi
done
echo
echo "Build glue hunks (configure.ac, Makefile.am):"
git --no-pager diff --stat "$base" -- configure.ac Makefile.am | sed 's/^/  /'
if [ $status -eq 0 ]; then
  echo "PASS: no upstream file outside the platform layer and build glue changed"
else
  echo "FAIL: upstream files changed (see above)"
fi
exit $status
