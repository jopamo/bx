#include "config.h"

#include "process_action_store.h"

#include <stdlib.h>
#include <string.h>

#include "misc.h"
#include "process.h"
#include "screen.h"

struct kclass {
	struct kclass *next;
	char *name;
	struct action ktab[256 + KMAP_KEYS];
};

static struct kclass *kclasses;

struct action *FindKtab(char *class, int create)
{
	struct kclass *kp, **kpp;
	int i;

	if (class == NULL)
		return ktab;
	for (kpp = &kclasses; (kp = *kpp) != NULL; kpp = &kp->next)
		if (!strcmp(kp->name, class))
			break;
	if (kp == NULL) {
		if (!create)
			return NULL;
		if (strlen(class) > 80) {
			Msg(0, "Command class name too long.");
			return NULL;
		}
		kp = malloc(sizeof(struct kclass));
		if (kp == NULL) {
			Msg(0, "%s", strnomem);
			return NULL;
		}
		kp->name = SaveStr(class);
		for (i = 0; i < (int)(ARRAY_SIZE(kp->ktab)); i++) {
			kp->ktab[i].nr = RC_ILLEGAL;
			kp->ktab[i].args = noargs;
			kp->ktab[i].argl = NULL;
			kp->ktab[i].quiet = 0;
		}
		kp->next = NULL;
		*kpp = kp;
	}
	return kp->ktab;
}

void ClearAction(struct action *act)
{
	char **p;

	if (act->nr == RC_ILLEGAL)
		return;
	act->nr = RC_ILLEGAL;
	if (act->args == noargs)
		return;
	for (p = act->args; *p; p++)
		free(*p);
	free((char *)act->args);
	act->args = noargs;
	act->argl = NULL;
}

void SaveAction(struct action *act, int nr, char **args, int *argl)
{
	int argc = 0;
	char **pp;
	int *lp;

	if (args)
		while (args[argc])
			argc++;
	if (argc == 0) {
		act->nr = nr;
		act->args = noargs;
		act->argl = NULL;
		return;
	}
	if ((pp = malloc((unsigned)(argc + 1) * sizeof(char *))) == NULL)
		Panic(0, "%s", strnomem);
	if ((lp = malloc((unsigned)(argc) * sizeof(int))) == NULL)
		Panic(0, "%s", strnomem);
	act->nr = nr;
	act->args = pp;
	act->argl = lp;
	while (argc--) {
		*lp = argl ? *argl++ : (int)strlen(*args);
		*pp++ = SaveStrn(*args++, *lp++);
	}
	*pp = NULL;
}

char **SaveArgs(char **args)
{
	char **ap, **pp;
	int argc = 0;

	while (args[argc])
		argc++;
	if ((pp = ap = malloc((unsigned)(argc + 1) * sizeof(char *))) == NULL)
		Panic(0, "%s", strnomem);
	while (argc--)
		*pp++ = SaveStr(*args++);
	*pp = NULL;
	return ap;
}
