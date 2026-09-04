/*
 * This file is automagically created from comm.c -- DO NOT EDIT
 */

#ifndef SCREEN_COMM_H
#define SCREEN_COMM_H

#include "acls.h"

struct comm
{
  char *name;
  int flags;
  AclBits userbits[ACL_BITS_PER_CMD];
};

extern struct comm comms[];

#define ARGS_MASK	(3)

#define ARGS_0	(0)
#define ARGS_1	(1)
#define ARGS_2	(2)
#define ARGS_3	(3)

#define ARGS_PLUS1	(1<<2)
#define ARGS_PLUS2	(1<<3)
#define ARGS_PLUS3	(1<<4)
#define ARGS_ORMORE	(1<<5)

#define NEED_FORE	(1<<6)	/* this command needs a fore window */
#define NEED_DISPLAY	(1<<7)	/* this command needs a display */
#define NEED_LAYER	(1<<8)	/* this command needs a layer */
#define CAN_QUERY	(1<<9)  /* this command can be queried, i.e. used with -Q to
				   get back a result to stdout */

#define ARGS_01		(ARGS_0 | ARGS_PLUS1)
#define ARGS_02		(ARGS_0 | ARGS_PLUS2)
#define ARGS_12		(ARGS_1 | ARGS_PLUS1)
#define ARGS_23		(ARGS_2 | ARGS_PLUS1)
#define ARGS_24		(ARGS_2 | ARGS_PLUS2)
#define ARGS_34		(ARGS_3 | ARGS_PLUS1)
#define ARGS_012	(ARGS_0 | ARGS_PLUS1 | ARGS_PLUS2)
#define ARGS_0123	(ARGS_0 | ARGS_PLUS1 | ARGS_PLUS2 | ARGS_PLUS3)
#define ARGS_123	(ARGS_1 | ARGS_PLUS1 | ARGS_PLUS2)
#define ARGS_124	(ARGS_1 | ARGS_PLUS1 | ARGS_PLUS3)
#define ARGS_1234	(ARGS_1 | ARGS_PLUS1 | ARGS_PLUS2 | ARGS_PLUS3)

struct action
{
  int nr;
  char **args;
  int *argl;
  int quiet;	/* Suppress (currently unused)
		   0x01 - Error message
		   0x02 - Normal message
		*/
};

#define RC_ILLEGAL -1

#endif /* SCREEN_COMM_H */

#define RC_ACLADD 0
#define RC_ACLCHG 1
#define RC_ACLDEL 2
#define RC_ACLGRP 3
#define RC_ACLUMASK 4
#define RC_ACTIVITY 5
#define RC_ADDACL 6
#define RC_ALLPARTIAL 7
#define RC_ALTSCREEN 8
#define RC_AT 9
#define RC_ATTRCOLOR 10
#define RC_AUTH 11
#define RC_AUTODETACH 12
#define RC_AUTONUKE 13
#define RC_BACKTICK 14
#define RC_BCE 15
#define RC_BELL 16
#define RC_BELL_MSG 17
#define RC_BIND 18
#define RC_BINDKEY 19
#define RC_BLANKER 20
#define RC_BLANKERPRG 21
#define RC_BREAK 22
#define RC_BREAKTYPE 23
#define RC_BUFFERFILE 24
#define RC_BUMPLEFT 25
#define RC_BUMPRIGHT 26
#define RC_C1 27
#define RC_CAPTION 28
#define RC_CHACL 29
#define RC_CHARSET 30
#define RC_CHDIR 31
#define RC_CJKWIDTH 32
#define RC_CLEAR 33
#define RC_COLLAPSE 34
#define RC_COLON 35
#define RC_COMMAND 36
#define RC_COMPACTHIST 37
#define RC_CONSOLE 38
#define RC_COPY 39
#define RC_CRLF 40
#define RC_DEFAUTONUKE 41
#define RC_DEFBCE 42
#define RC_DEFBREAKTYPE 43
#define RC_DEFC1 44
#define RC_DEFCHARSET 45
#define RC_DEFDYNAMICTITLE 46
#define RC_DEFENCODING 47
#define RC_DEFESCAPE 48
#define RC_DEFFLOW 49
#define RC_DEFGR 50
#define RC_DEFHSTATUS 51
#define RC_DEFKANJI 52
#define RC_DEFLOG 53
#define RC_DEFMODE 54
#define RC_DEFMONITOR 55
#define RC_DEFMOUSETRACK 56
#define RC_DEFNONBLOCK 57
#define RC_DEFOBUFLIMIT 58
#define RC_DEFSCROLLBACK 59
#define RC_DEFSHELL 60
#define RC_DEFSILENCE 61
#define RC_DEFSLOWPASTE 62
#define RC_DEFUTF8 63
#define RC_DEFWRAP 64
#define RC_DEFWRITELOCK 65
#define RC_DETACH 66
#define RC_DIGRAPH 67
#define RC_DINFO 68
#define RC_DISPLAYS 69
#define RC_DUMPTERMCAP 70
#define RC_DYNAMICTITLE 71
#define RC_ECHO 72
#define RC_ENCODING 73
#define RC_ESCAPE 74
#define RC_EVAL 75
#define RC_EXEC 76
#define RC_FIT 77
#define RC_FLOW 78
#define RC_FOCUS 79
#define RC_FOCUSMINSIZE 80
#define RC_GR 81
#define RC_GROUP 82
#define RC_HARDCOPY 83
#define RC_HARDCOPY_APPEND 84
#define RC_HARDCOPYDIR 85
#define RC_HARDSTATUS 86
#define RC_HEIGHT 87
#define RC_HELP 88
#define RC_HISTORY 89
#define RC_HSTATUS 90
#define RC_IDLE 91
#define RC_IGNORECASE 92
#define RC_INFO 93
#define RC_KANJI 94
#define RC_KILL 95
#define RC_LASTMSG 96
#define RC_LAYOUT 97
#define RC_LICENSE 98
#define RC_LOCKSCREEN 99
#define RC_LOG 100
#define RC_LOGFILE 101
#define RC_LOGTSTAMP 102
#define RC_MAPDEFAULT 103
#define RC_MAPNOTNEXT 104
#define RC_MAPTIMEOUT 105
#define RC_MARKKEYS 106
#define RC_META 107
#define RC_MONITOR 108
#define RC_MOUSETRACK 109
#define RC_MSGMINWAIT 110
#define RC_MSGWAIT 111
#define RC_MULTIINPUT 112
#define RC_MULTIUSER 113
#define RC_NEXT 114
#define RC_NONBLOCK 115
#define RC_NUMBER 116
#define RC_OBUFLIMIT 117
#define RC_ONLY 118
#define RC_OSC52 119
#define RC_OTHER 120
#define RC_PARENT 121
#define RC_PARTIAL 122
#define RC_PASTE 123
#define RC_PASTEFONT 124
#define RC_POW_BREAK 125
#define RC_POW_DETACH 126
#define RC_POW_DETACH_MSG 127
#define RC_PREV 128
#define RC_PRINTCMD 129
#define RC_PROCESS 130
#define RC_QUIT 131
#define RC_READBUF 132
#define RC_READREG 133
#define RC_REDISPLAY 134
#define RC_REGISTER 135
#define RC_REMOVE 136
#define RC_REMOVEBUF 137
#define RC_RENDITION 138
#define RC_RESET 139
#define RC_RESIZE 140
#define RC_SCREEN 141
#define RC_SCROLLBACK 142
#define RC_SELECT 143
#define RC_SESSIONNAME 144
#define RC_SETENV 145
#define RC_SETSID 146
#define RC_SHELL 147
#define RC_SHELLTITLE 148
#define RC_SILENCE 149
#define RC_SILENCEWAIT 150
#define RC_SLEEP 151
#define RC_SLOWPASTE 152
#define RC_SORENDITION 153
#define RC_SORT 154
#define RC_SOURCE 155
#define RC_SPLIT 156
#define RC_STARTUP_MESSAGE 157
#define RC_STATUS 158
#define RC_STUFF 159
#define RC_SU 160
#define RC_SUSPEND 161
#define RC_TERM 162
#define RC_TERMCAP 163
#define RC_TERMCAPINFO 164
#define RC_TERMINFO 165
#define RC_TITLE 166
#define RC_TRUECOLOR 167
#define RC_UMASK 168
#define RC_UNBINDALL 169
#define RC_UNSETENV 170
#define RC_UTF8 171
#define RC_VBELL 172
#define RC_VBELL_MSG 173
#define RC_VBELLWAIT 174
#define RC_VERBOSE 175
#define RC_VERSION 176
#define RC_WALL 177
#define RC_WIDTH 178
#define RC_WINDOWLIST 179
#define RC_WINDOWS 180
#define RC_WRAP 181
#define RC_WRITEBUF 182
#define RC_WRITELOCK 183
#define RC_XOFF 184
#define RC_XON 185
#define RC_ZMODEM 186
#define RC_ZOMBIE 187
#define RC_ZOMBIE_TIMEOUT 188

#define RC_LAST 188
