#include "config.h"

#include "process_prompts.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "input.h"
#include "layout.h"
#include "mark.h"
#include "misc.h"
#include "process.h"
#include "process_lookup.h"

extern int enter_window_name_mode;
extern struct plop plop_tab[MAX_PLOP_DEFS];

static void AKAFin(char *buf, size_t len, void *data)
{
	(void)data; /* unused */

	if (len && fore)
		ChangeAKA(fore, buf, strlen(buf));

	enter_window_name_mode = 0;
}

void InputAKA(void)
{
	char *s, *ss;
	size_t len;

	if (enter_window_name_mode == 1)
		return;

	enter_window_name_mode = 1;

	Input("Set window's title to: ", ARRAY_SIZE(fore->w_akabuf) - 1, INP_COOKED, AKAFin, NULL, 0);
	s = fore->w_title;
	if (!s)
		return;
	for (; *s; s++) {
		if ((*(unsigned char *)s & 0x7f) < 0x20 || *s == 0x7f)
			continue;
		ss = s;
		len = 1;
		LayProcess(&ss, &len);
	}
}

static void SelectFin(char *buf, size_t len, void *data)
{
	int n;

	(void)data; /* unused */

	if (!len || !display)
		return;
	if (len == 1 && *buf == '-') {
		SetForeWindow(NULL);
		Activate(0);
		return;
	}
	if ((n = WindowByNoN(buf)) < 0)
		return;
	SwitchWindow(GetWindowByNumber(n));
}

void SelectLayoutFin(char *buf, size_t len, void *data)
{
	Layout *lay;

	(void)data; /* unused */

	if (!len || !display)
		return;
	if (len == 1 && *buf == '-') {
		LoadLayout(NULL);
		Activate(0);
		return;
	}
	lay = FindLayout(buf);
	if (!lay)
		Msg(0, "No such layout\n");
	else if (lay == D_layout)
		Msg(0, "This IS layout %d (%s).\n", lay->lay_number, lay->lay_title);
	else {
		LoadLayout(lay);
		Activate(0);
	}
}

void InputSelect(void)
{
	Input("Switch to window: ", 20, INP_COOKED, SelectFin, NULL, 0);
}

static char setenv_var[31];

static void SetenvFin1(char *buf, size_t len, void *data)
{
	(void)data; /* unused */

	if (!len || !display)
		return;
	InputSetenv(buf);
}

static void SetenvFin2(char *buf, size_t len, void *data)
{
	(void)data; /* unused */

	if (!len || !display)
		return;
	setenv(setenv_var, buf, 1);
	MakeNewEnv();
}

void InputSetenv(char *arg)
{
	static char setenv_buf[50 + ARRAY_SIZE(setenv_var)];

	if (arg) {
		strncpy(setenv_var, arg, ARRAY_SIZE(setenv_var) - 1);
		setenv_var[ARRAY_SIZE(setenv_var) - 1] = '\0';
		sprintf(setenv_buf, "Enter value for %s: ", setenv_var);
		Input(setenv_buf, 30, INP_COOKED, SetenvFin2, NULL, 0);
	} else
		Input("Setenv: Enter variable name: ", 30, INP_COOKED, SetenvFin1, NULL, 0);
}

void copy_reg_fn(char *buf, size_t len, void *data)
{
	(void)data; /* unused */

	struct plop *pp = plop_tab + (int)(unsigned char)*buf;

	if (len) {
		memset(buf, 0, len);
		return;
	}
	if (pp->buf)
		free(pp->buf);
	pp->buf = NULL;
	pp->len = 0;
	if (D_user->u_plop.len) {
		if ((pp->buf = malloc(D_user->u_plop.len)) == NULL) {
			Msg(0, "%s", strnomem);
			return;
		}
		memmove(pp->buf, D_user->u_plop.buf, D_user->u_plop.len);
	}
	pp->len = D_user->u_plop.len;
	pp->enc = D_user->u_plop.enc;
	Msg(0, "Copied %zu characters into register %c", D_user->u_plop.len, *buf);
}

void ins_reg_fn(char *buf, size_t len, void *data)
{
	(void)data; /* unused */

	struct plop *pp = plop_tab + (int)(unsigned char)*buf;

	if (len) {
		memset(buf, 0, len);
		return;
	}
	if (!fore)
		return;
	if (*buf == '.')
		Msg(0, "ins_reg_fn: Warning: pasting real register '.'!");
	if (pp->buf) {
		MakePaster(&fore->w_paster, pp->buf, pp->len, 0);
		return;
	}
	Msg(0, "Empty register.");
}

void process_fn(char *buf, size_t len, void *data)
{
	struct plop *pp = plop_tab + (int)(unsigned char)*buf;

	(void)data; /* unused */

	if (len) {
		memset(buf, 0, len);
		return;
	}
	if (pp->buf) {
		ProcessInput(pp->buf, pp->len);
		return;
	}
	Msg(0, "Empty register.");
}

void confirm_fn(char *buf, size_t len, void *data)
{
	struct action act;

	if (len || (*buf != 'y' && *buf != 'Y')) {
		memset(buf, 0, len);
		return;
	}
	act.nr = *(int *)data;
	act.args = noargs;
	act.argl = NULL;
	act.quiet = 0;
	DoAction(&act);
}

struct inputsu {
	struct acluser **up;
	char name[24];
	char pw1[130];
	char pw2[130];
};

static void suFin(char *buf, size_t len, void *data)
{
	struct inputsu *i = (struct inputsu *)data;
	char *p;
	size_t l;

	if (!*i->name) {
		p = i->name;
		l = ARRAY_SIZE(i->name) - 1;
	} else if (!*i->pw1) {
		strcpy(p = i->pw1, "\377");
		l = ARRAY_SIZE(i->pw1) - 1;
	} else {
		strcpy(p = i->pw2, "\377");
		l = ARRAY_SIZE(i->pw2) - 1;
	}
	if (buf && len)
		strncpy(p, buf, 1 + ((l < len) ? l : len));
	if (!*i->name)
		Input("Screen User: ", ARRAY_SIZE(i->name) - 1, INP_COOKED, suFin, (char *)i, 0);
	else if (!*i->pw1)
		Input("User's UNIX Password: ", ARRAY_SIZE(i->pw1) - 1, INP_COOKED | INP_NOECHO, suFin, (char *)i, 0);
	else if (!*i->pw2)
		Input("User's Screen Password: ", ARRAY_SIZE(i->pw2) - 1, INP_COOKED | INP_NOECHO, suFin, (char *)i, 0);
	else {
		if ((p = DoSu(i->up, i->name, i->pw2, i->pw1)))
			Msg(0, "%s", p);
		free((char *)i);
	}
}

int InputSu(struct acluser **up, char *name)
{
	struct inputsu *i;

	if (!(i = (struct inputsu *)calloc(1, sizeof(struct inputsu))))
		return -1;

	i->up = up;
	if (name && *name)
		suFin(name, (int)strlen(name), (char *)i);
	else
		suFin(NULL, 0, (char *)i);
	return 0;
}
