/*
 * SaveB -- save cross-pollution test (step=2)
 * Run alternately with SaveA to detect cross-app save corruption.
 */

#define APP_ID          0xBB
#define APP_NAME        "SaveB"
#define STEP            2
#define VSAVE_SIZE_EVEN 32768

#include "../save_test.h"

int main(void) { return save_test_main(); }
