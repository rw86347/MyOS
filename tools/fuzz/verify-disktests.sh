#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Rodger Wilson
# Check that each disk attack case actually fails against the bug it is meant
# to catch. A case that passes either way is measuring nothing, and writing
# the network checks caught exactly that twice (see docs/security.md).
#
# Each case below puts one of the ext2 fixes back the way it was, rebuilds,
# and reports which checks noticed. The tree is restored afterwards.
set -e
# Ignore SIGPIPE. Without this, piping this script's output into `head` kills
# it before the restore trap below runs, and the tree is left holding a
# deliberately reintroduced bug. That happened once and cost half an hour.
trap '' PIPE
cd "$(dirname "$0")/../.."

SRC="fs/ext2.c"
TMP=$(mktemp -d)
# Restoring the sources is not enough: the binary left behind is the one built
# from the LAST reintroduced bug, and anything run afterwards (a fuzz session,
# a hand-run of the suite) then exercises deliberately broken code and reports
# bugs that are not real. That happened once. Delete it so the next use has to
# rebuild.
trap 'for f in $SRC; do cp "$TMP/$(echo $f | tr / _)" "$f"; done; rm -rf "$TMP"; rm -f build/fuzz/diskattacks build/fuzz/disk/fs/ext2.o' EXIT
for f in $SRC; do cp "$f" "$TMP/$(echo $f | tr / _)"; done

run_case() {
    name="$1"; shift
    for f in $SRC; do cp "$TMP/$(echo $f | tr / _)" "$f"; done
    if ! python3 - "$@" <<'PY'
import sys
path, old, new = sys.argv[1], sys.argv[2], sys.argv[3]
s = open(path).read()
if s.count(old) != 1:
    sys.exit("patch did not apply to %s (%d matches)" % (path, s.count(old)))
open(path, 'w').write(s.replace(old, new))
PY
    then
        # A patch that no longer matches the source would otherwise rebuild
        # the *unmodified* file, see every case pass, and report "NOT CAUGHT"
        # -- a script written to catch tests that measure nothing, itself
        # measuring nothing. Say so instead.
        printf '\n=== %s ===\nPATCH DID NOT APPLY: the source moved under it\n' "$name"
        return 1
    fi
    # Delete rather than touch: a patch and a build inside the same second
    # leave make thinking the binary is current, and the previous case's
    # binary then answers for this one.
    rm -f build/fuzz/diskattacks build/fuzz/disk/fs/ext2.o
    printf '\n=== %s ===\n' "$name"
    if make diskattacks >"$TMP/out" 2>&1; then
        echo "NOT CAUGHT — every case passed with the bug reintroduced"
        grep -E '^[0-9]+ passed' "$TMP/out" || true
        return 1
    fi
    grep -E '^  FAIL|^[0-9]+ passed' "$TMP/out"
}

fail=0

run_case "the directory-entry name_len check removed" \
    fs/ext2.c \
    ' &&
           (uint32_t)e->name_len + 8 <= e->rec_len;' \
    ';' || fail=1

run_case "rec_len no longer required to be a multiple of four" \
    fs/ext2.c \
    ' && !(e->rec_len & 3)' \
    '' || fail=1

# Put the division back above the check that guards it. The later, correct
# assignment still runs and computes the same value, so this reintroduces the
# ordering bug and nothing else.
run_case 'the ".." fixup not requiring an aligned offset' \
    fs/ext2.c \
    ' && !(at & 3)' \
    '' || fail=1

run_case "the group count divided before blocks_per_group is checked" \
    fs/ext2.c \
    '    fs->readonly = (ro_compat & ~(RO_COMPAT_SPARSE | RO_COMPAT_LARGEFILE)) != 0;' \
    '    fs->readonly = (ro_compat & ~(RO_COMPAT_SPARSE | RO_COMPAT_LARGEFILE)) != 0;
    fs->groups = (fs->blocks - fs->first_data + fs->bpg - 1) / fs->bpg;' || fail=1

run_case 'the ".." fixup trusting "."'"'"'s rec_len again' \
    fs/ext2.c \
    'at >= DIRENT_LEN(1) && !(at & 3) && at + 8 <= fs->bsize' \
    '1' || fail=1

run_case "inode_size no longer required to be a power of two" \
    fs/ext2.c \
    ' || (fs->isize & (fs->isize - 1))' \
    '' || fail=1

run_case "group counts no longer bounded by the bits in a bitmap block" \
    fs/ext2.c \
    ' || fs->ipg > fs->bsize * 8 || fs->bpg > fs->bsize * 8' \
    '' || fail=1

run_case "blocks_count and inodes_count unchecked again" \
    fs/ext2.c \
    ' ||
        fs->first_data > 1 || fs->blocks <= fs->first_data || !fs->inodes' \
    '' || fail=1

run_case "a fast symlink reporting whatever size says" \
    fs/ext2.c \
    '    if (!has_blocks(di))
        n = min(n, sizeof di->block);
    char *t = kmalloc(n ? n : 1);
    if (!t)
        return -ENOMEM;
    if (!has_blocks(di)) {
        memcpy(t, di->block, n);                            /* a "fast" symlink */' \
    '    char *t = kmalloc(n ? n : 1);
    if (!t)
        return -ENOMEM;
    if (!has_blocks(di)) {
        memcpy(t, di->block, min(n, sizeof di->block));     /* a "fast" symlink */' || fail=1

run_case "load_symlink failure ignored again" \
    fs/ext2.c \
    '                if (load_symlink(fs, c, cdi) < 0)
                    c->size = 0;' \
    '                load_symlink(fs, c, cdi);' || fail=1

run_case "the rejection message shifting by the value it rejected" \
    fs/ext2.c \
    'dev->name, incompat, (uint32_t)s->log_block_size);' \
    'dev->name, incompat, (uint32_t)1024 << s->log_block_size);' || fail=1

printf '\n'
if [ $fail -ne 0 ]; then
    echo "VERIFY FAILED: a bug above was reintroduced and no case noticed"
    exit 1
fi
echo "VERIFY OK: every reintroduced bug was caught by a case"
