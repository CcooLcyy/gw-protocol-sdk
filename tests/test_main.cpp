#include <cstdio>

int gw_protocol_run_lifecycle_tests(const char *filter);

int main(int argc, char **argv)
{
    const char *filter = argc > 1 ? argv[1] : nullptr;
    const int failures = gw_protocol_run_lifecycle_tests(filter);
    if (failures != 0) {
        std::fprintf(stderr, "%d test(s) failed\n", failures);
        return 1;
    }

    if (filter == nullptr) {
        std::printf("all gw protocol tests passed\n");
    } else {
        std::printf("%s passed\n", filter);
    }
    return 0;
}
