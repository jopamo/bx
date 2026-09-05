/* vi:set ts=8 sts=4 sw=4 noet:
 *
 * VIM - Vi IMproved    by Bram Moolenaar
 *
 * Do ":help uganda" in Vim to read copying and usage conditions.
 */

#include "vim.h"

#if defined(FEAT_EVAL)

/* Preserve the builtin results exposed when client/server support is absent. */
    void
f_remote_expr(typval_T *argvars UNUSED, typval_T *rettv)
{
    rettv->v_type = VAR_STRING;
    rettv->vval.v_string = NULL;
}

    void
f_remote_foreground(typval_T *argvars UNUSED, typval_T *rettv UNUSED)
{
}

    void
f_remote_peek(typval_T *argvars UNUSED, typval_T *rettv)
{
    rettv->vval.v_number = -1;
}

    void
f_remote_read(typval_T *argvars UNUSED, typval_T *rettv)
{
    rettv->v_type = VAR_STRING;
    rettv->vval.v_string = NULL;
}

    void
f_remote_send(typval_T *argvars UNUSED, typval_T *rettv)
{
    rettv->v_type = VAR_STRING;
    rettv->vval.v_string = NULL;
}

    void
f_remote_startserver(typval_T *argvars UNUSED, typval_T *rettv UNUSED)
{
    emsg(_(e_clientserver_feature_not_available));
}

    void
f_server2client(typval_T *argvars UNUSED, typval_T *rettv)
{
    rettv->vval.v_number = -1;
}

    void
f_serverlist(typval_T *argvars UNUSED, typval_T *rettv)
{
    rettv->v_type = VAR_STRING;
    rettv->vval.v_string = NULL;
}

#endif // FEAT_EVAL
