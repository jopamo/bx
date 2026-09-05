/* vi:set ts=8 sts=4 sw=4 noet:
 *
 * VIM - Vi IMproved    by Bram Moolenaar
 *
 * Do ":help uganda" in Vim to read copying and usage conditions.
 */

/*
 * bx keeps callback-backed input-method options without GUI or XIM backends.
 */

#include "vim.h"

#define USE_IMACTIVATEFUNC (*p_imaf != NUL)
#define USE_IMSTATUSFUNC (*p_imsf != NUL)

#if defined(FEAT_EVAL) && defined(IME_WITHOUT_XIM)
static callback_T imaf_cb;
static callback_T imsf_cb;

    char *
did_set_imactivatefunc(optset_T *args UNUSED)
{
    if (option_set_callback_func(p_imaf, &imaf_cb) == FAIL)
	return e_invalid_argument;

    return NULL;
}

    char *
did_set_imstatusfunc(optset_T *args UNUSED)
{
    if (option_set_callback_func(p_imsf, &imsf_cb) == FAIL)
	return e_invalid_argument;

    return NULL;
}

    static void
call_imactivatefunc(int active)
{
    typval_T argv[2];
    int save_KeyTyped = KeyTyped;

    argv[0].v_type = VAR_NUMBER;
    argv[0].vval.v_number = active ? 1 : 0;
    argv[1].v_type = VAR_UNKNOWN;
    (void)call_callback_retnr(&imaf_cb, 1, argv);

    KeyTyped = save_KeyTyped;
}

    static int
call_imstatusfunc(void)
{
    int is_active;
    int save_KeyTyped = KeyTyped;

    if (exiting || is_autocmd_blocked())
	return FALSE;
    ++msg_silent;
    is_active = call_callback_retnr(&imsf_cb, 0, NULL);
    --msg_silent;

    KeyTyped = save_KeyTyped;
    return is_active > 0;
}

    int
set_ref_in_im_funcs(int copyID UNUSED)
{
    int abort = FALSE;

    abort = set_ref_in_callback(&imaf_cb, copyID);
    abort = abort || set_ref_in_callback(&imsf_cb, copyID);
    return abort;
}

static int im_was_set_active = FALSE;

    int
im_get_status(void)
{
    if (USE_IMSTATUSFUNC)
	return call_imstatusfunc();
    return im_was_set_active;
}

    void
im_set_active(int active_arg)
{
    int active = !p_imdisable && active_arg;

    if (USE_IMACTIVATEFUNC && active != im_get_status())
    {
	call_imactivatefunc(active);
	im_was_set_active = active;
    }
}

#endif // FEAT_EVAL && IME_WITHOUT_XIM
