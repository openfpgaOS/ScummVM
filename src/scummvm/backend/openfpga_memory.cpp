#define FORBIDDEN_SYMBOL_ALLOW_ALL

#include "openfpga_memory.h"

#ifdef NONSTANDARD_PORT

#include "common/util.h"

namespace {

enum {
	kMidiDriverSlotSize = 512,
	kCharsetRendererSlotSize = 256,
	kCostumeRendererSlotSize = 1024,
	kCostumeLoaderSlotSize = 128,
	kScummActorSlotSize = 4096,
	kActorTableSlotSize = kOpenFPGAMaxScummActors * sizeof(void *)
};

#define OPENFPGA_DECLARE_POOL(name, slotSize, slotCount) \
	union name##Slot { \
		uint64 align; \
		byte storage[slotSize]; \
	}; \
	name##Slot name##Storage[slotCount]; \
	bool name##Used[slotCount]

OPENFPGA_DECLARE_POOL(g_midiDriver, kMidiDriverSlotSize, 2);
OPENFPGA_DECLARE_POOL(g_charsetRenderer, kCharsetRendererSlotSize, 2);
OPENFPGA_DECLARE_POOL(g_costumeRenderer, kCostumeRendererSlotSize, 2);
OPENFPGA_DECLARE_POOL(g_costumeLoader, kCostumeLoaderSlotSize, 2);
OPENFPGA_DECLARE_POOL(g_scummActor, kScummActorSlotSize, kOpenFPGAMaxScummActors);
OPENFPGA_DECLARE_POOL(g_scummActorTable, kActorTableSlotSize, 1);
OPENFPGA_DECLARE_POOL(g_scummSortedActorTable, kActorTableSlotSize, 1);

#undef OPENFPGA_DECLARE_POOL

struct PoolDesc {
	const char *name;
	byte *storage;
	bool *used;
	size_t slotSize;
	size_t slotStride;
	uint slotCount;
	uint usedCount;
	uint highWater;
	uint failures;
};

PoolDesc g_pools[kOpenFPGAPoolCount] = {
	{ "midi-driver",       (byte *)g_midiDriverStorage,       g_midiDriverUsed,       kMidiDriverSlotSize,       sizeof(g_midiDriverStorage[0]),       ARRAYSIZE(g_midiDriverStorage),       0, 0, 0 },
	{ "charset-renderer",  (byte *)g_charsetRendererStorage,  g_charsetRendererUsed,  kCharsetRendererSlotSize,  sizeof(g_charsetRendererStorage[0]),  ARRAYSIZE(g_charsetRendererStorage),  0, 0, 0 },
	{ "costume-renderer",  (byte *)g_costumeRendererStorage,  g_costumeRendererUsed,  kCostumeRendererSlotSize,  sizeof(g_costumeRendererStorage[0]),  ARRAYSIZE(g_costumeRendererStorage),  0, 0, 0 },
	{ "costume-loader",    (byte *)g_costumeLoaderStorage,    g_costumeLoaderUsed,    kCostumeLoaderSlotSize,    sizeof(g_costumeLoaderStorage[0]),    ARRAYSIZE(g_costumeLoaderStorage),    0, 0, 0 },
	{ "scumm-actor",       (byte *)g_scummActorStorage,       g_scummActorUsed,       kScummActorSlotSize,       sizeof(g_scummActorStorage[0]),       ARRAYSIZE(g_scummActorStorage),       0, 0, 0 },
	{ "scumm-actor-table", (byte *)g_scummActorTableStorage,  g_scummActorTableUsed,  kActorTableSlotSize,       sizeof(g_scummActorTableStorage[0]),  ARRAYSIZE(g_scummActorTableStorage),  0, 0, 0 },
	{ "scumm-sorted-table",(byte *)g_scummSortedActorTableStorage, g_scummSortedActorTableUsed, kActorTableSlotSize, sizeof(g_scummSortedActorTableStorage[0]), ARRAYSIZE(g_scummSortedActorTableStorage), 0, 0, 0 }
};

volatile int g_openfpgaMemoryFailurePool = -1;
volatile size_t g_openfpgaMemoryFailureSize = 0;
volatile uint g_openfpgaMemoryFailureCode = 0;

static void memoryPanic(OpenFPGAMemoryPool pool, size_t size, uint code) {
	g_openfpgaMemoryFailurePool = (int)pool;
	g_openfpgaMemoryFailureSize = size;
	g_openfpgaMemoryFailureCode = code;
	__builtin_trap();
	for (;;) {}
}

static PoolDesc *getPool(OpenFPGAMemoryPool pool) {
	if (pool < 0 || pool >= kOpenFPGAPoolCount)
		memoryPanic(pool, 0, 1);
	return &g_pools[pool];
}

static byte *slotPtr(PoolDesc *desc, uint index) {
	return desc->storage + (index * desc->slotStride);
}

} // namespace

void *openfpga_pool_alloc(OpenFPGAMemoryPool pool, size_t size) {
	PoolDesc *desc = getPool(pool);

	if (size == 0 || size > desc->slotSize) {
		++desc->failures;
		memoryPanic(pool, size, 2);
	}

	for (uint i = 0; i < desc->slotCount; ++i) {
		if (!desc->used[i]) {
			desc->used[i] = true;
			++desc->usedCount;
			if (desc->usedCount > desc->highWater)
				desc->highWater = desc->usedCount;
			return slotPtr(desc, i);
		}
	}

	++desc->failures;
	memoryPanic(pool, size, 3);
	__builtin_unreachable();
}

void openfpga_pool_free(OpenFPGAMemoryPool pool, void *ptr) {
	if (!ptr)
		return;

	PoolDesc *desc = getPool(pool);
	for (uint i = 0; i < desc->slotCount; ++i) {
		if (ptr == slotPtr(desc, i)) {
			if (!desc->used[i]) {
				++desc->failures;
				memoryPanic(pool, 0, 4);
			}
			desc->used[i] = false;
			--desc->usedCount;
			return;
		}
	}

	++desc->failures;
	memoryPanic(pool, 0, 5);
}

bool openfpga_pool_contains(OpenFPGAMemoryPool pool, const void *ptr) {
	if (!ptr)
		return false;

	PoolDesc *desc = getPool(pool);
	for (uint i = 0; i < desc->slotCount; ++i) {
		if (ptr == slotPtr(desc, i))
			return true;
	}
	return false;
}

size_t openfpga_pool_slot_size(OpenFPGAMemoryPool pool) {
	return getPool(pool)->slotSize;
}

uint openfpga_pool_high_water(OpenFPGAMemoryPool pool) {
	return getPool(pool)->highWater;
}

#endif /* NONSTANDARD_PORT */
