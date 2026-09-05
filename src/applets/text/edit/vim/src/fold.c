/* vi:set ts=8 sts=4 sw=4 noet:
 *
 * VIM - Vi IMproved    by Bram Moolenaar
 *
 * Do ":help uganda" in Vim to read copying and usage conditions.
 */

#include "vim.h"

#if defined(FEAT_EVAL)

static void
foldclosed_both(typval_T *argvars UNUSED, typval_T *rettv, int end UNUSED)
{
    rettv->vval.v_number = -1;
}

    void
f_foldclosed(typval_T *argvars, typval_T *rettv)
{
    foldclosed_both(argvars, rettv, FALSE);
}

    void
f_foldclosedend(typval_T *argvars, typval_T *rettv)
{
    foldclosed_both(argvars, rettv, TRUE);
}

    void
f_foldlevel(typval_T *argvars UNUSED, typval_T *rettv UNUSED)
{
}

    void
f_foldtext(typval_T *argvars UNUSED, typval_T *rettv)
{
    rettv->v_type = VAR_STRING;
    rettv->vval.v_string = NULL;
}

    void
f_foldtextresult(typval_T *argvars UNUSED, typval_T *rettv)
{
    rettv->v_type = VAR_STRING;
    rettv->vval.v_string = NULL;

    if (in_vim9script() && check_for_lnum_arg(argvars, 0) == FAIL)
	return;
}

#endif // FEAT_EVAL
