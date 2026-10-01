# slots2c.awk — turn config/slots.conf into a C table for the kernel.
#
#   awk -f tools/slots2c.awk config/slots.conf > build/gen/slots_conf.c
#
# Output: one array indexed by slot number (1-20), holding the owner's name
# or NULL. Mistakes in the config stop the build with a message.

BEGIN {
    NSLOTS = 20
    errors = 0
}

{
    sub(/#.*/, "")                  # strip comments
    if (NF == 0)
        next                        # blank line
    name = $1
    if (length(name) > 15) {
        err("name '" name "' is longer than 15 characters")
        next
    }
    if (NF == 1)
        err("'" name "' has no slots")
    for (i = 2; i <= NF; i++) {
        s = $i
        if (s !~ /^[0-9]+$/ || s < 1 || s > NSLOTS)
            err("'" s "' is not a slot number (1-" NSLOTS ")")
        else if (s == 1 || s == 10)
            err("slot " s " belongs to the system")
        else if (s in owner)
            err("slot " s " given to both " owner[s] " and " name)
        else
            owner[s + 0] = name
    }
}

END {
    if (errors)
        exit 1
    print "/* Generated from config/slots.conf by tools/slots2c.awk — do not edit. */"
    print "#include <sched.h>"
    print ""
    print "const char *const slot_config[SCHED_SLOTS + 1] = {"
    for (s = 1; s <= NSLOTS; s++)
        if (s in owner)
            printf "    [%d] = \"%s\",\n", s, owner[s]
    print "};"
}

function err(msg) {
    printf "config/slots.conf:%d: %s\n", NR, msg > "/dev/stderr"
    errors++
}
