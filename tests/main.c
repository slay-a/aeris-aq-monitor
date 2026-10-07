#include <stdio.h>
#include "test.h"

int g_tests_run = 0, g_tests_failed = 0;
const char *g_suite = "";

int main(void)
{
    printf("Aeris core test suite\n=====================\n");
    suite_crc8();
    suite_scd40();
    suite_sgp41();
    suite_sht31();
    suite_pmsa003i();
    suite_gas_index();
    suite_features();
    suite_json();
    suite_api();
    suite_history();
    suite_classifier();
    suite_aqi();
    suite_model();
    suite_integration();

    printf("\n%d checks, %d failed\n", g_tests_run, g_tests_failed);
    if (g_tests_failed == 0) printf("ALL PASS\n");
    return g_tests_failed ? 1 : 0;
}
