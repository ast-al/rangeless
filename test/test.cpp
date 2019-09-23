#define RANGELESS_FN_ENABLE_RUN_TESTS 0
#define RANGELESS_MT_ENABLE_RUN_TESTS 1

#include <fn.hpp>
#include <mt.hpp>

int main()
{
#if RANGELESS_FN_ENABLE_RUN_TESTS
    rangeless::fn::impl::run_tests();
#endif

#if RANGELESS_MT_ENABLE_RUN_TESTS
    rangeless::mt::impl::run_tests();
#endif

    return 0;
}
