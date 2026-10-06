#ifndef LINUX_FIRMWARE_H
#define LINUX_FIRMWARE_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

/*
 * Firmware files for drivers (kernel/firmware.c), with Linux's names:
 * a driver asks for /lib/firmware/<name> and gets its bytes, read-only,
 * until it gives them back with release_firmware().
 */
struct device;
struct inode;

struct firmware {
    size_t size;
    const uint8_t *data;
    void *priv;                     /* ours: the heap copy to free (NULL: none) */
    struct inode *inode;            /* ours: held while the driver has it */
};

/* 0 and *fw set, or -errno (-ENOENT: no such file) and *fw NULL. The first
   logs a missing file, as Linux does; the second stays quiet (a driver
   trying several names in turn). May sleep (a file on a disk). */
int  request_firmware(const struct firmware **fw, const char *name, struct device *dev);
int  firmware_request_nowarn(const struct firmware **fw, const char *name, struct device *dev);
void release_firmware(const struct firmware *fw);

/* The same, finished later: cont(fw, context) runs on a work thread with
   the file (or NULL). Returns at once; -errno only if it could not even
   queue the request. */
struct module;
int request_firmware_nowait(struct module *module, bool uevent, const char *name,
                            struct device *dev, unsigned int gfp, void *context,
                            void (*cont)(const struct firmware *fw, void *context));

#endif
