#ifndef ATA_H
#define ATA_H

/* Detect IDE/ATA hard disks and register each one as a block device
   named hda, hdb, hdc, hdd. */
void ata_init(void);

#endif
