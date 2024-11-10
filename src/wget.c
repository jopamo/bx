#include "applets.h"
#include "diag.h"

#if BX_HAVE_MIRA_EMBED
#include <mira/embed.h>
#endif

int bx_wget_main(int argc, char **argv) {
#if BX_HAVE_MIRA_EMBED
    return mira_embed_run_wget(argc, argv);
#else
    (void)argc;
    (void)argv;
    bx_err("wget applet unavailable: bx built without mira-embed support");
    return 127;
#endif
}
