#define RANGELESS_FN_ENABLE_RUN_TESTS 1

#include <fn.hpp>

int main()
{
#if RANGELESS_FN_ENABLE_RUN_TESTS
    rangeless::fn::impl::run_tests();
#endif

    return 0;
}
