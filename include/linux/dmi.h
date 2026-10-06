#ifndef LINUX_DMI_H
#define LINUX_DMI_H
/* The firmware's description of the machine (SMBIOS/DMI: vendor, model).
   Drivers keep tables of machines that need a quirk. We don't read the
   SMBIOS tables yet, so no table ever matches (docs/gaps.md). */
#include <linux/types.h>
enum dmi_field {
    DMI_NONE, DMI_BIOS_VENDOR, DMI_BIOS_VERSION, DMI_BIOS_DATE, DMI_SYS_VENDOR,
    DMI_PRODUCT_NAME, DMI_PRODUCT_VERSION, DMI_PRODUCT_SERIAL, DMI_PRODUCT_UUID,
    DMI_PRODUCT_SKU, DMI_PRODUCT_FAMILY, DMI_BOARD_VENDOR, DMI_BOARD_NAME,
};
struct dmi_strmatch {
    unsigned char slot:7;
    unsigned char exact_match:1;
    char substr[79];
};
struct dmi_system_id {
    int (*callback)(const struct dmi_system_id *);
    const char *ident;
    struct dmi_strmatch matches[4];
    void *driver_data;
};
#define DMI_MATCH(a, b)       { .slot = a, .substr = b }
#define DMI_EXACT_MATCH(a, b) { .slot = a, .substr = b, .exact_match = 1 }
static inline int dmi_check_system(const struct dmi_system_id *l) { (void)l; return 0; }
static inline const struct dmi_system_id *dmi_first_match(const struct dmi_system_id *l) { (void)l; return NULL; }
static inline const char *dmi_get_system_info(int f) { (void)f; return NULL; }
#endif
