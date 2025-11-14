#include "dispatch/applets.h"

int bx_screen_main_impl(int argc, char** argv);

int bx_screen_main(int argc, char** argv) {
    return bx_screen_main_impl(argc, argv);
}
