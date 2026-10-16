#include "MiniTest.h"
void register_ps2_vu1_tests();
void register_ps2_vu_tests();
void register_ps2_gs_tests();
void register_ps2_memory_tests();
void reset_ps2_test_function_table();
int main() {
    MiniTest::BeforeEach(reset_ps2_test_function_table);
    register_ps2_vu1_tests();
    register_ps2_vu_tests();
    register_ps2_gs_tests();
    register_ps2_memory_tests();
    return MiniTest::Run();
}
