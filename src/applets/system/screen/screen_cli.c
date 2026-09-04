#include <stdio.h>
#include <stdlib.h>

#include "screen.h"
#include "help.h"
#include "process.h"
#include "screen_cli.h"

_Noreturn void ScreenCliExitWithUsage(
	const char *program_name,
	const char *message,
	const char *argument
)
{
	printf("Use: %s [-opts] [cmd [args]]\n", program_name);
	printf(" or: %s -r [host.tty]\n\nOptions:\n", program_name);
#ifdef ENABLE_TELNET
	printf("-4            Resolve hostnames only to IPv4 addresses.\n");
	printf("-6            Resolve hostnames only to IPv6 addresses.\n");
#endif
	printf("-a            Force all capabilities into each window's termcap.\n");
	printf("-A -[r|R]     Adapt all windows to the new display width & height.\n");
	printf("-c file       Read configuration file instead of '.screenrc'.\n");
	printf("-d (-r)       Detach the elsewhere running screen (and reattach here).\n");
	printf("-dmS name     Start as daemon: Screen session in detached mode.\n");
	printf("-D (-r)       Detach and logout remote (and reattach here).\n");
	printf("-D -RR        Do whatever is needed to get a screen session.\n");
	printf("-e xy         Change command characters.\n");
	printf("-f            Flow control on, -fn = off, -fa = auto.\n");
	printf("-h lines      Set the size of the scrollback history buffer.\n");
	printf("-i            Interrupt output sooner when flow control is on.\n");
	printf("-ls [match]   or\n");
	printf("-list         Do nothing, just list our SocketDir [on possible matches].\n");
	printf("-L            Turn on output logging.\n");
	printf("-Logfile file Set logfile name.\n");
	printf("-m            ignore $STY variable, do create a new screen session.\n");
	printf("-O            Choose optimal output rather than exact vt100 emulation.\n");
	printf("-p window     Preselect the named window if it exists.\n");
	printf("-P            Tell screen to enable authentication.\n");
	printf("-q            Quiet startup. Exits with non-zero return code if unsuccessful.\n");
	printf("-Q            Commands will send the response to the stdout of the querying process.\n");
	printf("-r [session]  Reattach to a detached screen process.\n");
	printf("-R            Reattach if possible, otherwise start a new session.\n");
	printf("-s shell      Shell to execute rather than $SHELL.\n");
	printf("-S sockname   Name this session <pid>.sockname instead of <pid>.<tty>.<host>.\n");
	printf("-t title      Set title. (window's name).\n");
	printf("-T term       Use term as $TERM for windows, rather than \"screen\".\n");
	printf("-U            Tell screen to use UTF-8 encoding.\n");
	printf("-v            Print \"screen (bx) %s\".\n", version);
	printf("-wipe [match] Do nothing, just clean up SocketDir [on possible matches].\n");
	printf("-x            Attach to a not detached screen. (Multi display mode).\n");
	printf("-X            Execute <cmd> as a screen command in the specified session.\n");
	if (message != NULL && *message != '\0') {
		printf("\nError: ");
		printf(message, argument);
		printf("\n");
		exit(1);
	}
	exit(0);
}

static char *ScreenCliParseChar(char *text, char *character)
{
	if (*text == '\0')
		return NULL;
	if (*text == '^' && text[1] != '\0') {
		if (*++text == '?')
			*character = '\177';
		else if (*text >= '@')
			*character = Ctrl(*text);
		else
			return NULL;
		++text;
	} else if (*text == '\\' && *++text <= '7' && *text >= '0') {
		*character = 0;
		do
			*character = *character * 8 + *text - '0';
		while (*++text <= '7' && *text >= '0');
	} else {
		*character = *text++;
	}
	return text;
}

int ScreenCliParseEscape(char *text)
{
	unsigned char characters[2];

	if (*text == '\0') {
		SetEscape(NULL, -1, -1);
		return 0;
	}
	text = ScreenCliParseChar(text, (char *)&characters[0]);
	if (text == NULL)
		return -1;
	text = ScreenCliParseChar(text, (char *)&characters[1]);
	if (text == NULL || *text != '\0')
		return -1;
	SetEscape(NULL, characters[0], characters[1]);
	return 0;
}
