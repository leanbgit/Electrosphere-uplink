/*
 * cd_internal.h -- shared by the CD files (not used elsewhere).
 *
 * The CD drive is split in three:
 *   cd_image.c   one disc image file (.bin, raw 2352-byte sectors): reading
 *                sectors, the ISO 9660 directory, the English version laid
 *                over it (Project NEMO's patch), MSF <-> LBA
 *   cd_discs.c   the discs mounted: one, or both as one drive (disc 2 after
 *                disc 1), with the game's index files merged so either disc's
 *                streams are found; the ac3_disc_* functions for the host
 *   psyq_cd.c    the drive as the game's library sees it: CdControl
 *                commands, CdRead, streaming sectors one by one at the
 *                drive's speed, callbacks and events
 */
#ifndef AC3_CD_INTERNAL_H
#define AC3_CD_INTERNAL_H
#include "hle.h"
#include "ac3_english.h"
#include <stdio.h>

/* a raw sector: 12 sync bytes, then the "whole" sector the drive delivers:
 * 4 header bytes (MSF and mode), 8 subheader bytes, 2048 bytes of user data
 * (and error correction, unused) */
#define RAW_SECTOR   2352
#define SYNC_SIZE    12
#define USER_OFFSET  24
#define USER_SIZE    2048
#define WHOLE_SIZE   2340

/* the subheader, as offsets into a whole sector */
#define SUB_FILE   4
#define SUB_CHAN   5
#define SUB_MODE   6
#define SUBMODE_AUDIO 0x04
#define SUBMODE_DATA  0x08

typedef struct {
    FILE *fp;
    long  sectors;           /* readable sectors (with the English version: its size) */
    u32   base;              /* first LBA on the drive (disc 2 follows disc 1) */
    int   number;            /* 1 or 2 */
    ac3_en_overlay_t *en;    /* the English version, or NULL */
    long  src_sectors;       /* sectors in the file itself */
} disc_image_t;

/* ---- cd_image.c ----------------------------------------------------------- */
#define image_open          cd_image_open
#define image_close         cd_image_close
#define image_read          cd_image_read
#define image_read_user     cd_image_read_user
#define iso_find            cd_iso_find
#define lba_to_msf          cd_lba_to_msf
#define msf_to_lba          cd_msf_to_lba
#define msf_to_lba_signed   cd_msf_to_lba_signed
int  image_open(disc_image_t *d, const char *path);
void image_close(disc_image_t *d);
int  image_read(const disc_image_t *d, u32 rel, u8 *out);        /* whole sector */
int  image_read_user(const disc_image_t *d, u32 rel, u8 *out);   /* user data */
int  iso_find(const disc_image_t *d, const char *want, u32 *out_lba, u32 *out_size);
void lba_to_msf(u32 lba, u8 *m, u8 *s, u8 *f);                   /* BCD */
s32  msf_to_lba_signed(u8 m, u8 s, u8 f);
u32  msf_to_lba(u8 m, u8 s, u8 f);                               /* 0 before the start */

/* ---- cd_discs.c ----------------------------------------------------------- */
#define read_sector_whole   cd_read_sector_whole
#define read_sector         cd_read_sector
#define drive_sectors       cd_drive_sectors
#define disc_mounted        cd_disc_mounted
#define find_file           cd_find_file
#define pack_note_read      cd_pack_note_read
#define discs_machine_save  cd_discs_machine_save
int  read_sector_whole(u32 lba, u8 *out);   /* any disc on the drive, with the merged indexes */
int  read_sector(u32 lba, u8 *out);         /* its user data */
u32  drive_sectors(void);
int  disc_mounted(void);
int  find_file(const char *name, u32 *lba, u32 *size);   /* on the booted disc */
void pack_note_read(u32 lba);               /* a read starts here (English packs follow the disc) */
void discs_machine_save(FILE *f);

#endif
