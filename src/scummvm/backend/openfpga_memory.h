#ifndef OPENFPGA_MEMORY_H
#define OPENFPGA_MEMORY_H

#include "common/scummsys.h"

#ifdef NONSTANDARD_PORT

enum OpenFPGAMemoryPool {
	kOpenFPGAPoolMidiDriver = 0,
	kOpenFPGAPoolCharsetRenderer,
	kOpenFPGAPoolCostumeRenderer,
	kOpenFPGAPoolCostumeLoader,
	kOpenFPGAPoolScummActor,
	kOpenFPGAPoolScummActorTable,
	kOpenFPGAPoolScummSortedActorTable,
	kOpenFPGAPoolCount
};

enum {
	kOpenFPGAMaxScummActors = 80
};

void *openfpga_pool_alloc(OpenFPGAMemoryPool pool, size_t size);
void openfpga_pool_free(OpenFPGAMemoryPool pool, void *ptr);
bool openfpga_pool_contains(OpenFPGAMemoryPool pool, const void *ptr);
size_t openfpga_pool_slot_size(OpenFPGAMemoryPool pool);
uint openfpga_pool_high_water(OpenFPGAMemoryPool pool);

#endif /* NONSTANDARD_PORT */

#endif /* OPENFPGA_MEMORY_H */
