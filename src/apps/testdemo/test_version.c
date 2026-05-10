#include "test.h"

void test_version(void) {
    section_start("Version");
    uint32_t ver = of_get_version();
    ASSERT("nonzero", ver != 0);
    section_end();
}
