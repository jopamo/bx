#ifndef BX_APPLETS_SYSTEM_SCREEN_CLI_H
#define BX_APPLETS_SYSTEM_SCREEN_CLI_H

_Noreturn void ScreenCliExitWithUsage(
	const char *program_name,
	const char *message,
	const char *argument
);
int ScreenCliParseEscape(char *text);

#endif
