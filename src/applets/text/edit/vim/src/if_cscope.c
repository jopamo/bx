/* vi:set ts=8 sts=4 sw=4 noet:
 *
 * VIM - Vi IMproved    by Bram Moolenaar
 *
 * Do ":help uganda" in Vim to read copying and usage conditions.
 */

#include "vim.h"

#if defined(FEAT_EVAL)

/* Keep the cscope_connection() builtin's disabled-feature result. */
    void
f_cscope_connection(typval_T *argvars UNUSED, typval_T *rettv UNUSED)
{
}

#endif // FEAT_EVAL
