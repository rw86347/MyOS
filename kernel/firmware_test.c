/*
 * firmware_test.c — request_firmware, checked at boot (make test builds).
 * The test initramfs carries /lib/firmware/myos-test.fw (user/rootfs).
 */
#include <errno.h>
#include <initrd.h>
#include <kprintf.h>
#include <linux/firmware.h>
#include <selftest.h>
#include <string.h>

void firmware_selftest(void)
{
    static const char want[] = "MyOS firmware test\n";
    const struct firmware *fw;
    int err = request_firmware(&fw, "myos-test.fw", NULL);
    check(err == 0 && fw && fw->size == sizeof want - 1 &&
          memcmp(fw->data, want, sizeof want - 1) == 0, "request_firmware reads the file");
    /* Unchanged since boot, so it is the archive's own bytes: no copy. */
    int lent = !err && fw->data >= initrd_data() && fw->data < initrd_data() + initrd_size();
    check(lent, "request_firmware lends the archive's bytes");
    if (!err) {
        kprintf("firmware: myos-test.fw, %lu bytes, %s\n", (unsigned long)fw->size,
                lent ? "from the archive (no copy)" : "copied");
        release_firmware(fw);
    }
    const struct firmware *none = (const void *)1;
    err = firmware_request_nowarn(&none, "no-such-file.bin", NULL);
    check(err == -ENOENT && none == NULL, "firmware_request_nowarn: missing file");
}
