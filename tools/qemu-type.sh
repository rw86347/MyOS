#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Rodger Wilson
# qemu-type.sh — turn text into QEMU monitor "sendkey" commands, one per
# character, so a test can "type" at the keyboard of a headless MyOS.
#
#   tools/qemu-type.sh 'echo hello | wc -c'      (Enter is added at the end)
#
# Prints the commands; pipe them into `qemu ... -monitor stdio`.
text=$1
i=0
while [ $i -lt ${#text} ]; do
    c=$(printf '%s' "$text" | cut -c$((i + 1)))
    case "$c" in
        [a-z0-9]) k=$c ;;
        [A-Z])    k="shift-$(printf '%s' "$c" | tr A-Z a-z)" ;;
        ' ')  k=spc ;;          '-')  k=minus ;;        '=')  k=equal ;;
        '/')  k=slash ;;        '.')  k=dot ;;          ',')  k=comma ;;
        ';')  k=semicolon ;;    "'")  k=apostrophe ;;   '`')  k=grave_accent ;;
        '[')  k=bracket_left ;; ']')  k=bracket_right ;; '\') k=backslash ;;
        '|')  k=shift-backslash ;; '>') k=shift-dot ;;  '<')  k=shift-comma ;;
        '"')  k=shift-apostrophe ;; '_') k=shift-minus ;; ':') k=shift-semicolon ;;
        '$')  k=shift-4 ;;      '*')  k=shift-8 ;;      '&')  k=shift-7 ;;
        '(')  k=shift-9 ;;      ')')  k=shift-0 ;;      '!')  k=shift-1 ;;
        '?')  k=shift-slash ;;  '+')  k=shift-equal ;;  '~')  k=shift-grave_accent ;;
        '#')  k=shift-3 ;;      '@')  k=shift-2 ;;      '%')  k=shift-5 ;;
        '{')  k=shift-bracket_left ;; '}') k=shift-bracket_right ;; '^') k=shift-6 ;;
        *)    k=spc ;;
    esac
    echo "sendkey $k"
    i=$((i + 1))
done
echo "sendkey ret"
