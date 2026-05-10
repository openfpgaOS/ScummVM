/*
 * of_file.h -- File I/O for openfpgaOS
 *
 * Use standard C fopen/fread/fwrite/fclose/fseek/ftell.
 *   fopen("data.bin", "rb")       -- opens a read-only APF data file
 *   fopen("MyGame_0.sav", "wb")  -- opens a read/write save file
 *
 * Use opendir/readdir to discover available files.
 */

#ifndef OF_FILE_H
#define OF_FILE_H

#include <stdint.h>

/* Standard file I/O is via POSIX (fopen/fread/fwrite/fclose).
 * The of_file_* helpers below are advanced async DMA reads only. */

#ifndef OF_PC

#include "of_syscall.h"
#include "of_syscall_numbers.h"
#include "of_services.h"

/* Register a filename→data-slot binding for fopen() by name.
 *
 * The kernel auto-discovers APF filenames at boot. Apps can still call
 * this when they want to provide or override a binding explicitly.
 * Overwrites any prior mapping with the same name.
 * Max 32 registrations total.
 *
 *   of_file_slot_register(3, "data.bin");
 *   FILE *f = fopen("data.bin", "rb");
 */
static inline void of_file_slot_register(uint32_t slot_id, const char *filename) {
    OF_SVC->file_slot_register(slot_id, filename);
}

/* Start a non-blocking file read from a data slot.
 * dest must point into a DMA-target region (the kernel rejects
 * destinations outside the platform's bridge-addressable window).
 * callback(token, result) fires when DMA completes: result=0 success, <0 error.
 * Only one async read in flight at a time (bridge limitation).
 * Returns token >= 0 on success, < 0 if busy or error. */
static inline int of_file_read_async(int slot_id, uint32_t offset,
                                     void *dest, uint32_t length,
                                     void (*callback)(int token, int result)) {
    return (int)of_ecall5(OF_EID_FILE, OF_FILE_FID_READ_ASYNC,
                          slot_id, (long)offset, (long)dest,
                          (long)length, (long)callback).value;
}

/* Poll async read progress. Call from your main loop.
 * Returns 1 if a read completed (callback invoked), 0 otherwise. */
static inline int of_file_async_poll(void) {
    return (int)of_ecall0(OF_EID_FILE, OF_FILE_FID_ASYNC_POLL).value;
}

/* Check if an async read is in flight. */
static inline int of_file_async_busy(void) {
    return (int)of_ecall0(OF_EID_FILE, OF_FILE_FID_ASYNC_BUSY).value;
}

#endif /* OF_PC */

#endif /* OF_FILE_H */
