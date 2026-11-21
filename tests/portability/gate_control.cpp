// DATABREAKs Engine 029 - positive control for the core purity gate.
//
// Includes a HOST-ONLY header (it uses <cstdio>). Compiled with the poison
// include directory this must FAIL; CTest registers it with WILL_FAIL, so
// the gate is proven to be active rather than silently bypassed.

#include "../../platform/host/file_storage.h"

int main() { return 0; }
