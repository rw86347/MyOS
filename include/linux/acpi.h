#ifndef LINUX_ACPI_H
#define LINUX_ACPI_H
/* No ACPI yet (CONFIG_ACPI off): the driver's ACPI tables (SAR, PPAG...)
   stay at their defaults through its own stubs (fw/acpi.h). */
#include <linux/types.h>
#include <linux/uuid.h>
#define TRUE  1
#define FALSE 0
#define ACPI_TYPE_INTEGER 1
#define ACPI_TYPE_STRING  2
#define ACPI_TYPE_BUFFER  3
#define ACPI_TYPE_PACKAGE 4
typedef u64 acpi_integer;
#define ACPI_FREE(p) kfree(p)
void kfree(const void *p);
union acpi_object {
    u32 type;
    struct { u32 type; u64 value; } integer;
    struct { u32 type; u32 length; char *pointer; } string;
    struct { u32 type; u32 length; u8 *pointer; } buffer;
    struct { u32 type; u32 count; union acpi_object *elements; } package;
};
typedef void *acpi_handle;
#define ACPI_HANDLE(dev) NULL
#endif
