#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Rodger Wilson
# =============================================================================
# myos-ssh.sh — log in to a MyOS live stick over ssh, with this Mac's key
#
#   tools/myos-ssh.sh setup          put the keys in live/overlay.local (once)
#   tools/myos-ssh.sh [command...]   ssh to the MSI, found on the LAN
#   tools/myos-ssh.sh find           just print its address
#   tools/myos-ssh.sh scp SRC... DST scp, "myos:" naming the stick
#
#   MYOS_HOST=192.168.1.50 ...       skip the search
#   MYOS_HOST=127.0.0.1 MYOS_PORT=2222 ...   QEMU's forward (make test-ssh)
#   SSH_KEY=~/.ssh/other ...         another key (setup and connecting)
#
# setup writes two things into live/overlay.local/etc/ssh (git-ignored; the
# live build copies the directory onto the stick):
#   ssh_host_ed25519_key     the stick's host key, made here, so it is the
#                            same after every reflash. A stick that made its
#                            own on first boot would have a new one each time,
#                            and ssh would rightly refuse it as another machine.
#   authorized_keys/myos     this Mac's public key, for the live user "myos"
#                            (sshd_config.d/10-myos.conf looks there)
# Then rebuild (make ARCH=x86_64 live-uefi) and write the stick.
#
# Connecting checks the stick's host key under the name "myos"
# (HostKeyAlias), not under its address: DHCP may move it, and nothing else on
# the LAN can answer as the MSI. The search asks every address of this Mac's
# /24 for its host key at once and keeps the one that answers with ours.
# The ssh settings go to build/myos-ssh.config, so after one run
#   ssh -F build/myos-ssh.config myos ...
# works by hand too.
# =============================================================================
set -euo pipefail
cd "$(dirname "$0")/.."

ETC=live/overlay.local/etc/ssh
HOSTKEY=$ETC/ssh_host_ed25519_key
SSH_KEY=${SSH_KEY:-$HOME/.ssh/id_ed25519_nopass}
PORT=${MYOS_PORT:-22}
CACHE=build/myos-ssh.host
CONFIG=build/myos-ssh.config
KNOWN=build/myos-ssh.known_hosts

setup() {
    [ -f "$SSH_KEY.pub" ] || { echo "no $SSH_KEY.pub (set SSH_KEY)" >&2; exit 1; }
    mkdir -p "$ETC/authorized_keys"
    if [ ! -f "$HOSTKEY" ]; then
        ssh-keygen -q -t ed25519 -N '' -C myos-live -f "$HOSTKEY"
        echo "made the stick's host key: $HOSTKEY"
    fi
    cp "$SSH_KEY.pub" "$ETC/authorized_keys/myos"
    # sshd refuses a private key others can read, and (StrictModes) a key
    # file or directory anyone but root can write. The modes go onto the stick.
    chmod 600 "$HOSTKEY"
    chmod 644 "$HOSTKEY.pub" "$ETC/authorized_keys/myos"
    chmod 755 "$ETC/authorized_keys"
    echo "myos may log in with: $(ssh-keygen -lf "$SSH_KEY.pub")"
    echo "the stick's host key: $(ssh-keygen -lf "$HOSTKEY.pub")"
    echo "next: make ARCH=x86_64 live-uefi, then write the stick (docs/UEFI-USB.md)"
}

need_setup() {
    [ -f "$HOSTKEY.pub" ] || { echo "run tools/myos-ssh.sh setup first" >&2; exit 1; }
}

ours() { cut -d' ' -f2 "$HOSTKEY.pub"; }    # the host key, base64

# Does $1 answer on $PORT with the stick's host key?
is_myos() {
    ssh-keyscan -T 3 -t ed25519 -p "$PORT" "$1" 2>/dev/null | grep -qF "$(ours)"
}

find_myos() {
    if [ -n "${MYOS_HOST:-}" ]; then
        echo "$MYOS_HOST"
        return
    fi
    if [ -f "$CACHE" ] && is_myos "$(cat "$CACHE")"; then
        cat "$CACHE"
        return
    fi
    local ifc me list hit
    ifc=$(route -n get default 2>/dev/null | awk '/interface:/ { print $2 }')
    me=$(ipconfig getifaddr "$ifc" 2>/dev/null || true)
    [ -n "$me" ] || { echo "no address on $ifc to search from; set MYOS_HOST" >&2; exit 1; }
    list=$(mktemp "${TMPDIR:-/tmp}/myos-ssh.XXXXXX")
    for i in $(seq 1 254); do echo "${me%.*}.$i"; done > "$list"
    echo "looking for the MSI on ${me%.*}.0/24 ..." >&2
    hit=$(ssh-keyscan -T 3 -t ed25519 -p "$PORT" -f "$list" 2>/dev/null |
          grep -F "$(ours)" | awk '{ print $1 }' | head -1 | sed 's/^\[\(.*\)\]:.*/\1/')
    rm -f "$list"
    [ -n "$hit" ] || { echo "no MyOS sshd with our host key on ${me%.*}.0/24" >&2; exit 1; }
    mkdir -p build
    echo "$hit" > "$CACHE"
    echo "$hit"
}

write_config() {
    mkdir -p build
    echo "myos $(cut -d' ' -f1,2 "$HOSTKEY.pub")" > "$KNOWN"
    cat > "$CONFIG" <<EOF
# Made by tools/myos-ssh.sh; rewritten on every run.
Host myos
    HostName $1
    Port $PORT
    User myos
    IdentityFile $SSH_KEY
    IdentitiesOnly yes
    PasswordAuthentication no
    KbdInteractiveAuthentication no
    HostKeyAlias myos
    UserKnownHostsFile $PWD/$KNOWN
    StrictHostKeyChecking yes
    ConnectTimeout 10
    ServerAliveInterval 15
EOF
}

case "${1:-}" in
setup)
    setup
    ;;
find)
    need_setup
    find_myos
    ;;
scp)
    need_setup
    shift
    write_config "$(find_myos)"
    scp -F "$CONFIG" "$@"
    ;;
*)
    need_setup
    write_config "$(find_myos)"
    ssh -F "$CONFIG" myos "$@"
    ;;
esac
