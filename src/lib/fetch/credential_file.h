#ifndef BX_FETCH_CREDENTIAL_FILE_H
#define BX_FETCH_CREDENTIAL_FILE_H

/* BX_FETCH_HEADER_OWNER: fs */
/* BX_FETCH_HEADER_CONSUMERS: fs, cli */

/*
 * Loads one bounded Bearer token, optionally terminated by LF or CRLF.
 * Requires an effective-user-owned regular file, mode 0400 or 0600, with
 * exactly one hard link and no symlinks in any path component.
 * On success replaces *token with owned storage; failure leaves it unchanged.
 * Errors use errno and never include secret contents.
 */
int bx_fetch_bearer_token_load_file(const char* path, char** token);

#define BX_FETCH_HTTP_PASSWORD_MAX_BYTES 4096u

/*
 * Same file protection and replacement contract as above. Loads a password
 * of at most BX_FETCH_HTTP_PASSWORD_MAX_BYTES, stripping one optional LF/CRLF.
 * Empty passwords and spaces are preserved; ASCII controls and DEL are rejected.
 */
int bx_fetch_http_password_load_file(const char* path, char** password);

#endif
