#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Rodger Wilson
# split-repos.sh — the two public repos, made from this one:
#
#   build/split/MyOS        the kernel          github.com/rw86347/MyOS
#   build/split/myos-live   the live disks      github.com/rw86347/myos-live
#
#   tools/split-repos.sh [BRANCH]       (default: the current branch)
#
# Both are this repo's history, filtered (git filter-branch keeps every
# author, date and message), so running it again after more commits gives
# the same commits plus the new ones, and a plain push carries them over.
#
#   MyOS        everything but live/, the live-only tools and theme files at
#               their old places (tools/fetch-live.py..., assets/themes), and
#               docs/hardwareRequirements.md (private: never published). Its
#               .gitignore gains /live/, where myos-live is checked out.
#   myos-live   live/ as the repo's top, plus the history of the tools and
#               theme files that moved into it. Never live/writeperfect
#               (WritePerfect's own: all rights reserved) or
#               live/overlay.local (this machine's Wi-Fi network).
#
# Nothing is pushed: see the end for the commands.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BRANCH=${1:-$(git -C "$ROOT" rev-parse --abbrev-ref HEAD)}
OUT=$ROOT/build/split
MOVED='fetch-live.py mklivehd.sh live-prepare.py test-install.py qemu-desktop.py wifi-desktop.qs
       make-worldmap.py make-wastebasket-icons.py tar2cpio.py msi-chrome-report.sh check-xscreen.py'

clone() {   # a fresh copy of BRANCH, alone, as "main"
    rm -rf "$OUT/$1"
    git clone -q --no-local --no-checkout --single-branch --branch "$BRANCH" "$ROOT" "$OUT/$1"
    git -C "$OUT/$1" remote remove origin
    git -C "$OUT/$1" config core.quotePath false
    [ "$BRANCH" = main ] || git -C "$OUT/$1" branch -q -m "$BRANCH" main
}
mkdir -p "$OUT"

# --- MyOS: drop the live disk's files; ignore live/ ---------------------------
OLD_TOOLS=$(for t in $MOVED; do printf ' tools/%s' "$t"; done)
clone MyOS
FILTER_BRANCH_SQUELCH_WARNING=1 git -C "$OUT/MyOS" filter-branch -q --prune-empty --index-filter "
    git rm -r -q --cached --ignore-unmatch -- live assets/themes assets/golden-hour-desktop.png \
        'assets/golden-hour-desktop@2x.png' docs/hardwareRequirements.md $OLD_TOOLS
    g=\$(git ls-files -s .gitignore | cut -d' ' -f2)
    if [ -n \"\$g\" ]; then
        n=\$( { git cat-file blob \$g; printf '\n# The live disk is its own repo (myos-live), checked out here.\n/live/\n'; } \
              | git hash-object -w --stdin)
        git update-index --cacheinfo 100644,\$n,.gitignore
    fi" -- main

# --- myos-live: live/ at the top, with the files that moved into it -------------
TOOLS_RE=$(echo $MOVED | sed 's/\./\\./g; s/ /|/g')
clone myos-live
FILTER_BRANCH_SQUELCH_WARNING=1 git -C "$OUT/myos-live" filter-branch -q --prune-empty --index-filter "
    git ls-files -s | perl -ne '
        my (\$meta, \$path) = /^(\\S+ \\S+ \\S+)\\t(.*)\$/ or next;
        next if \$path =~ m{^live/(writeperfect|overlay\\.local)(/|\$)};
        if (\$path =~ s{^live/}{}) { }
        elsif (\$path =~ m{^tools/($TOOLS_RE)\$}) { }
        elsif (\$path =~ m{^assets/(themes/|golden-hour-desktop)}) { }
        else { next }
        print \"\$meta\\t\$path\\n\";
    ' > \"\$GIT_INDEX_FILE.list\"
    rm -f \"\$GIT_INDEX_FILE.new\"
    GIT_INDEX_FILE=\"\$GIT_INDEX_FILE.new\" git read-tree --empty
    GIT_INDEX_FILE=\"\$GIT_INDEX_FILE.new\" git update-index --index-info < \"\$GIT_INDEX_FILE.list\"
    mv \"\$GIT_INDEX_FILE.new\" \"\$GIT_INDEX_FILE\"
    rm -f \"\$GIT_INDEX_FILE.list\"" -- main

for r in MyOS myos-live; do
    git -C "$OUT/$r" for-each-ref --format='delete %(refname)' refs/original | git -C "$OUT/$r" update-ref --stdin
    git -C "$OUT/$r" reflog expire --expire=now --all
    git -C "$OUT/$r" gc -q --prune=now
    echo "$r: $(git -C "$OUT/$r" rev-list --count main) commits, $(git -C "$OUT/$r" ls-tree -r main | wc -l | tr -d ' ') files, main = $(git -C "$OUT/$r" rev-parse --short main)"
done
cat <<EOF
To publish (each repo is a plain clone you can look through first):
    git -C $OUT/MyOS push https://github.com/rw86347/MyOS.git main
    git -C $OUT/myos-live push https://github.com/rw86347/myos-live.git main
EOF
