#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Rodger Wilson
# Check that each adversarial test actually fails against the bug it is meant
# to catch. A test that passes either way is measuring nothing, and writing
# these caught exactly that twice (see docs/security.md).
#
# Each case below reintroduces one bug by patching the source, runs the suite,
# and reports which checks noticed. The tree is restored afterwards.
set -e
# Ignore SIGPIPE. Without this, piping this script's output into `head` kills
# it before the restore trap below runs, and the tree is left holding a
# deliberately reintroduced bug. That happened once and cost half an hour.
trap '' PIPE
cd "$(dirname "$0")/../.."

SRC="net/tcp.c net/udp.c net/ipv4.c net/dns.c"
TMP=$(mktemp -d)
# Restoring the sources is not enough: the binary left behind is the one built
# from the LAST reintroduced bug, and anything run afterwards (a fuzz session,
# a hand-run of the suite) then exercises deliberately broken code and reports
# bugs that are not real. That happened once. Delete it so the next use has to
# rebuild.
trap 'for f in $SRC; do cp "$TMP/$(echo $f | tr / _)" "$f"; done; rm -rf "$TMP"; touch $SRC; rm -f build/fuzz/attacks build/fuzz/fuzz_net' EXIT
for f in $SRC; do cp "$f" "$TMP/$(echo $f | tr / _)"; done

run_case() {
    name="$1"; shift
    for f in $SRC; do cp "$TMP/$(echo $f | tr / _)" "$f"; done
    python3 - "$@" <<'PY'
import sys
path, old, new = sys.argv[1], sys.argv[2], sys.argv[3]
s = open(path).read()
if s.count(old) != 1:
    sys.exit("patch did not apply to %s (%d matches)" % (path, s.count(old)))
open(path, 'w').write(s.replace(old, new))
PY
    # Delete rather than touch: a patch and a build inside the same second
    # leave make thinking the binary is current, and the previous case's
    # binary then answers for this one.
    rm -f build/fuzz/attacks build/fuzz/net/*.o build/fuzz/kernel/*.o
    printf '\n=== %s ===\n' "$name"
    if make attacks >"$TMP/out" 2>&1; then
        echo "NOT CAUGHT — every test passed with the bug reintroduced"
        grep -E '^[0-9]+ passed' "$TMP/out" || true
        return 1
    fi
    grep -E '^  FAIL|^[0-9]+ passed' "$TMP/out"
}

fail=0

run_case "TCP ISN back to a clock-derived constant" \
    net/tcp.c \
    'return random_keyed(&t, sizeof t) + timer_ms() * 250u;   /* 250 per ms = 4 us */' \
    '(void)t; return timer_ms() * 64000u + 0x10000000u;' || fail=1

run_case "ephemeral ports back to a counter" \
    net/tcp.c \
    'c->lport = ip_ephemeral_port(port_used);' \
    'static uint16_t np = 49152; for (int i = 0; i < 16384 && !c->lport; i++) { uint16_t p = np++; if (np == 0) np = 49152; if (!port_used(p)) c->lport = p; }' || fail=1

run_case "DNS transaction id back to a counter" \
    net/dns.c \
    'dns_wait_id = (uint16_t)random_u32();' \
    'if (++dns_wait_id == 0) dns_wait_id = 1;' || fail=1

run_case "IP ID back to one counter for the machine" \
    net/ipv4.c \
    'uint32_t h = ((dst ^ ident_key) * 0x9E3779B9u) >> 26;   /* the top 6 bits */
    return (uint16_t)idents[h]++;' \
    '(void)dst; static uint16_t one = 1; return one++;' || fail=1

run_case "no rate limit on echo replies" \
    net/ipv4.c \
    'if (ic.type == ICMP_ECHO && body <= sizeof reply &&
            ip_rate_ok(ntohl(ip.saddr))) {' \
    'if (ic.type == ICMP_ECHO && body <= sizeof reply) {' || fail=1

run_case "no rate limit on resets for unknown connections" \
    net/tcp.c \
    '    if (!ip_rate_ok(to))
        return;
' \
    '' || fail=1

run_case "rate limit made global instead of per peer" \
    net/ipv4.c \
    'struct rl_slot *s = &rl[((peer ^ rl_key) * 0x9E3779B9u) >> 28];' \
    'peer = 0; struct rl_slot *s = &rl[0];' || fail=1

run_case "the source address of an arriving packet trusted again" \
    net/ipv4.c \
    '    if (!ip_source_ok(ntohl(ip.saddr))) {
        kfree_skb(skb);
        return 0;
    }
' \
    '' || fail=1

printf '\n'
if [ $fail -ne 0 ]; then
    echo "VERIFY FAILED: a bug above was reintroduced and no test noticed"
    exit 1
fi
echo "VERIFY OK: every reintroduced bug was caught by a test"
