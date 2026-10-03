#!/bin/sh
# Check that each racetest check actually fails against the bug it is meant to
# catch. A test that passes either way is measuring nothing, which has happened
# four times in this project now (see docs/security.md).
#
# Each case below puts one of the sleeping-copy fixes back the way it was,
# rebuilds the test kernel, boots it, and reports which checks noticed. The
# tree is restored afterwards.
#
# Slower than the host-side verify scripts because every case boots QEMU:
# about fifteen seconds each.
set -e
# Ignore SIGPIPE, or piping this into `head` kills it before the restore trap
# runs and leaves a deliberately reintroduced bug in the tree.
trap '' PIPE
cd "$(dirname "$0")/../.."

SRC="fs/pipe.c kernel/proc.c kernel/syscall.c"
TMP=$(mktemp -d)
restore() { for f in $SRC; do cp "$TMP/$(echo $f | tr / _)" "$f"; done; }
# Delete the objects as well: a restored source and a stale object leave the
# next case running the previous case's kernel.
trap 'restore; rm -rf "$TMP"; rm -f build/test/kernel/syscall.o build/test/kernel/proc.o build/test/fs/pipe.o' EXIT
for f in $SRC; do cp "$f" "$TMP/$(echo $f | tr / _)"; done

run_case() {
    name="$1"; shift
    restore
    if ! python3 - "$@" <<'PY'
import sys
path, old, new = sys.argv[1], sys.argv[2], sys.argv[3]
s = open(path).read()
if s.count(old) != 1:
    sys.exit("patch did not apply to %s (%d matches)" % (path, s.count(old)))
open(path, 'w').write(s.replace(old, new))
PY
    then
        # Otherwise the unmodified kernel gets rebuilt, every check passes, and
        # this reports "NOT CAUGHT" -- a script written to catch tests that
        # measure nothing, measuring nothing.
        printf '\n=== %s ===\nPATCH DID NOT APPLY: the source moved under it\n' "$name"
        return 1
    fi
    rm -f build/test/kernel/syscall.o build/test/kernel/proc.o build/test/fs/pipe.o
    printf '\n=== %s ===\n' "$name"
    # QEMU holds a write lock on the disk image, so a concurrent `make test`
    # (another agent, another window) makes this fail for the wrong reason.
    n=0
    while [ $n -lt 5 ]; do
        make test >"$TMP/out" 2>&1 || true
        grep -qi 'failed to get.*lock' "$TMP/out" || break
        n=$((n + 1)); sleep 20
    done
    if grep -q 'racetest: all [0-9]* checks passed' "$TMP/out"; then
        echo "NOT CAUGHT — racetest passed with the bug reintroduced"
        return 1
    fi
    if ! grep -q 'racetest:' "$TMP/out"; then
        echo "INCONCLUSIVE — racetest did not run at all (build or boot broke)"
        grep -E 'error:|Error [0-9]' "$TMP/out" | head -3
        return 1
    fi
    grep -E '^racetest: (FAIL|[0-9]+ of)' "$TMP/out" | head -8
}

fail=0

run_case "pipe_read consuming after the copy instead of before it" \
    fs/pipe.c \
    '    p->tail += n;
    if (copy_to_user(user_buf, p->buf + at, first) < 0 ||
        copy_to_user((uint8_t *)user_buf + first, p->buf, n - first) < 0)
        return -EFAULT;' \
    '    if (copy_to_user(user_buf, p->buf + at, first) < 0 ||
        copy_to_user((uint8_t *)user_buf + first, p->buf, n - first) < 0)
        return -EFAULT;
    p->tail += n;' || fail=1

run_case "pipe_write claiming room after the copy instead of before it" \
    fs/pipe.c \
    '        p->head += k;
        uint32_t claimed = p->head;
        if (copy_from_user(p->buf + at, src, first) < 0 ||
            copy_from_user(p->buf, src + first, k - first) < 0) {
            if (p->head == claimed)
                p->head -= k;       /* nobody else claimed after us: take it back */
            else {
                /* Someone did, so the space has to stay. Leave zeros rather
                   than whatever the page held, so a failed write cannot show
                   the reader the contents of recycled kernel memory. */
                memset(p->buf + at, 0, first);
                memset(p->buf, 0, k - first);
            }
            return done ? (int32_t)done : -EFAULT;
        }
        done += k;' \
    '        if (copy_from_user(p->buf + at, src, first) < 0 ||
            copy_from_user(p->buf, src + first, k - first) < 0)
            return done ? (int32_t)done : -EFAULT;
        p->head += k;
        done += k;' || fail=1

run_case "wait4 copying the status before reaping the child" \
    kernel/proc.c \
    '                int id = c->pid, status = c->status;
                reap(c);
                if (user_status && copy_to_user((void *)user_status, &status, 4) < 0)
                    return -EFAULT;' \
    '                int id = c->pid, status = c->status;
                if (user_status && copy_to_user((void *)user_status, &status, 4) < 0)
                    return -EFAULT;
                reap(c);' || fail=1

run_case "getdents64 not holding the entry it is about to read" \
    kernel/syscall.c \
    '    if (next)
        vfs_get(next);
    for (;;) {' \
    '    for (;;) {' || fail=1

run_case "getdents64 not holding the entry after the current one" \
    kernel/syscall.c \
    '            struct inode *after = vfs_dir_after(i);
            if (after)
                vfs_get(after);' \
    '            struct inode *after = vfs_dir_after(i);' || fail=1

run_case "getdents64 anchoring a cursor on a deleted entry" \
    kernel/syscall.c \
    '            if (i->flags & I_UNLINKED)
                break;' \
    '            if (0)
                break;' || fail=1

printf '\n'
if [ $fail -ne 0 ]; then
    echo "VERIFY FAILED: a bug above was reintroduced and racetest did not notice"
    exit 1
fi
echo "VERIFY OK: every reintroduced bug was caught by racetest"
