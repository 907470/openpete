/* openpete_cdrom_mmio.h — the CD-ROM controller's registers for a game that
 * drives the controller itself (core/psx/cdrom_mmio.c). */
#ifndef OPENPETE_CDROM_MMIO_H
#define OPENPETE_CDROM_MMIO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Register reads and writes at 0x1F801800..0x1F801803 (physical); 1 when
 * the address was one of the controller's. */
int  openpete_cdrom_mmio_read8(uint32_t phys, uint8_t* out);
int  openpete_cdrom_mmio_write8(uint32_t phys, uint8_t value);

/* One step per VBL body: interrupts due at this VBL count are raised. */
void openpete_cdrom_mmio_vbl(uint32_t vbls);

/* The golden's `.cdirq` sibling; 1 when found. Commands issued from an
 * instruction at or above `pc` are the game's own (the rows below it are
 * libcd's, thunked on the host and skipped). */
int  openpete_cdrom_mmio_open(const char* trace_path);
void openpete_cdrom_mmio_set_raw_pc_min(uint32_t pc);

/* The reference's response to the library's next command `cmd` in the
 * current record (the drive's answer to a query libcd issued), for a thunk
 * that answers it; 1 when a row was consumed. */
int  openpete_cdrom_mmio_libcd_response(uint8_t cmd, uint8_t* out, uint32_t* n_out);
/* The VBLANK of the last interrupt of the command the previous call
 * answered: its completion on the reference. */
uint32_t openpete_cdrom_mmio_libcd_last_done_vbl(void);
/* The record that interrupt was recorded in (between the anchor hits of
 * that index and the next). */
uint32_t openpete_cdrom_mmio_libcd_last_done_rec(void);
/* The status byte of that last response. */
uint8_t  openpete_cdrom_mmio_libcd_last_done_status(void);
/* The idx-th interrupt of that command: its record, VBLANK, cause and
 * status byte; 0 past the command's interrupts. */
int      openpete_cdrom_mmio_libcd_last_irq(uint32_t idx, uint32_t* rec, uint32_t* vbl, uint8_t* irq, uint8_t* status);

/* The loaded disc image's sector count (the table answer to GetTD 0). */
uint32_t openpete_cdrom_mmio_disc_sectors(void);

void openpete_cdrom_mmio_summary(void);

#ifdef __cplusplus
}
#endif
#endif
