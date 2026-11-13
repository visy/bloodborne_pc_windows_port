#include "../src/stuck_instance.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
    assert(bb_instance_is_stuck(1, UINT64_C(8) << 30));
    assert(bb_instance_is_stuck(2, UINT64_C(1) << 30));
    assert(!bb_instance_is_stuck(3, UINT64_C(8) << 30));   /* a working game has many threads */
    assert(!bb_instance_is_stuck(100, UINT64_C(8) << 30));
    assert(!bb_instance_is_stuck(1, (UINT64_C(1) << 30) - 1)); /* an exiting small process */
    puts("stuck instance test passed");
    return 0;
}
