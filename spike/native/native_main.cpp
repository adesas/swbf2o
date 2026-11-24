/* Native entry point for the same check the browser runs. It calls the same function the wasm module
 * exports, for the same two receive orders, and prints what the engine produced. */
#include <stdio.h>

extern "C" const char* wire_report(const char* orderString);

int main(void)
{
    puts(wire_report("01"));
    puts(wire_report("10"));
    return 0;
}
