#include "../harness.h"
static void run_tests(void) {
    /* Through a variable: a literal condition trips MSVC /W4 C4127. */
    int two = 1 + 1;
    CHECK(two == 2);
}
TEST_MAIN()
