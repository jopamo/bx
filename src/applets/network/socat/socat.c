#include "dispatch/applets.h"

/* The imported socat entrypoint is renamed at compile time by Meson. */
int bx_socat_main_impl(int argc, char* argv[]);

int bx_socat_main(int argc, char** argv) {
    return bx_socat_main_impl(argc, argv);
}
