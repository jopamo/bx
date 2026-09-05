/* vi:set ts=8 sts=4 sw=4 noet:
 *
 * VIM - Vi IMproved    by Bram Moolenaar
 *
 * Do ":help uganda" in Vim to read copying and usage conditions.
 */

/* bx retains vimrc/exrc writing without disabled view/session persistence. */

#include "vim.h"

    void
ex_mkrc(exarg_T *eap)
{
    FILE *fd;
    int failed = FALSE;
    char_u *fname;

    if (eap->cmdidx == CMD_mksession || eap->cmdidx == CMD_mkview)
    {
	ex_ni(eap);
	return;
    }

    if (*eap->arg != NUL)
	fname = eap->arg;
    else if (eap->cmdidx == CMD_mkvimrc)
	fname = (char_u *)VIMRC_FILE;
    else
	fname = (char_u *)EXRC_FILE;

    fd = open_exfile(fname, eap->forceit, WRITEBIN);
    if (fd != NULL)
    {
	if (eap->cmdidx == CMD_mkvimrc)
	    (void)put_line(fd, "version 6.0");

	if (p_cp)
	    (void)put_line(fd, "if !&cp | set cp | endif");
	else
	    (void)put_line(fd, "if &cp | set nocp | endif");

	{
	    int flags = OPT_GLOBAL;

	    failed |= (makemap(fd, NULL) == FAIL
		    || makeset(fd, flags, FALSE) == FAIL);
	}

	if (put_line(fd, "\" vim: set ft=vim :") == FAIL)
	    failed = TRUE;

	failed |= fclose(fd);
	if (failed)
	    emsg(_(e_error_while_writing));
    }

    apply_autocmds(EVENT_SESSIONWRITEPOST, NULL, NULL, FALSE, curbuf);
}

    int
put_eol(FILE *fd)
{
    if (putc('\n', fd) < 0)
	return FAIL;
    return OK;
}

    int
put_line(FILE *fd, char *s)
{
    if (fputs(s, fd) < 0 || put_eol(fd) == FAIL)
	return FAIL;
    return OK;
}
