#include "applets.h"
#include "bx/diag.h"
#include "lib/cli_common.h"

#if BX_HAVE_MIRA_EMBED
#include <mira/embed.h>
#endif

int bx_wget_main(int argc, char** argv) {
#if BX_HAVE_MIRA_EMBED
    return mira_embed_run_argv(argc, argv);
#else
    struct bx_diag_ctx diag = {
        .progname = bx_cli_progname((argc > 0) ? argv[0] : NULL, "wget"),
        .exit_status = 0,
    };

    bx_diag(&diag, "applet unavailable: bx built without mira-embed support");
    return 127;
#endif
}
