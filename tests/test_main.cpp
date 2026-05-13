#include <cstdio>

int gw_protocol_run_lifecycle_tests();

int main()
{
    const int failures = gw_protocol_run_lifecycle_tests();
    if (failures != 0) {
        std::fprintf(stderr, "%d test(s) failed\n", failures);
        return 1;
    }

    std::printf("all gw protocol lifecycle tests passed\n");
    return 0;
}
