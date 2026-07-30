/*
 * SD card bring-up probe — step 1 of adding SD storage.
 *
 * Deliberately does nothing but answer "is the card wired correctly and does
 * it mount?". The ring buffer, the write path and the operator commands are
 * separate later steps, so that a failure here points at the wiring or the
 * card rather than at any of that.
 *
 * Superseded by sd_ring.c once this is confirmed on hardware.
 */

#include <stdio.h>

#include "ff.h"
#include "f_util.h"
#include "rtc.h"
#include "hw_config.h"
#include "sd_card.h"

void sd_probe(void) {
    printf("\n--- sd probe ---\n");

    if (!sd_init_driver()) {
        printf("sd: driver init failed\n");
        return;
    }
    sd_card_t *pSD = sd_get_by_num(0);
    if (!pSD) {
        printf("sd: no card configured in hw_config.c\n");
        return;
    }

    // FatFs asks the RP2040's internal RTC for file timestamps
    // (FF_FS_NORTC is 0). Not seeded from the DS3231 yet — that belongs with
    // the step that actually creates files.
    time_init();

    // Mount failure is reported, not panicked on (../gmcount/fatfs.c panics).
    // A headless logger that halts on a bad card also stops answering UDP.
    FRESULT fr = f_mount(&pSD->fatfs, pSD->pcName, 1);
    if (FR_OK != fr) {
        printf("sd: mount failed: %s (%d)\n", FRESULT_str(fr), fr);
        printf("--- sd probe: FAILED ---\n\n");
        return;
    }

    uint64_t sectors = sd_sectors(pSD);
    printf("sd: mounted, %llu sectors (%llu MB)\n",
           (unsigned long long)sectors,
           (unsigned long long)((sectors * 512ull) >> 20));

    DWORD free_clusters = 0;
    FATFS *fs = NULL;
    fr = f_getfree("", &free_clusters, &fs);
    if (FR_OK == fr) {
        uint64_t free_bytes =
            (uint64_t)free_clusters * fs->csize * FF_MAX_SS;
        // Cluster size decides how much slack the 64MB ring costs later, so
        // it is worth seeing now.
        printf("sd: %llu MB free, cluster size %lu bytes\n",
               (unsigned long long)(free_bytes >> 20),
               (unsigned long)(fs->csize * FF_MAX_SS));
    } else {
        printf("sd: f_getfree failed: %s (%d)\n", FRESULT_str(fr), fr);
    }

    // List what is already on the card, so a card with existing content is
    // obvious before anything starts writing to it.
    DIR dj;
    FILINFO fno;
    fr = f_findfirst(&dj, &fno, "", "*");
    if (FR_OK != fr) {
        printf("sd: f_findfirst failed: %s (%d)\n", FRESULT_str(fr), fr);
    } else if (!fno.fname[0]) {
        printf("sd: (card is empty)\n");
    } else {
        while (FR_OK == fr && fno.fname[0]) {
            printf("sd:   %s  %llu bytes%s\n", fno.fname,
                   (unsigned long long)fno.fsize,
                   (fno.fattrib & AM_DIR) ? "  [dir]" : "");
            fr = f_findnext(&dj, &fno);
        }
    }
    f_closedir(&dj);

    printf("--- sd probe: OK ---\n\n");
}
