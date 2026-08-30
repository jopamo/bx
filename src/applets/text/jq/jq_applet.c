#include "dispatch/applets.h"

int bx_jq_cli_main(int argc, char **argv);

int bx_jq_main(int argc, char **argv) {
    return bx_jq_cli_main(argc, argv);
}
