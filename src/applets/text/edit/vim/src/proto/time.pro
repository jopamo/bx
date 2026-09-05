/* time.c */
time_T vim_time(void);
char *get_ctime(time_t thetime, int add_newline);
void f_localtime(typval_T *argvars, typval_T *rettv);
void f_reltime(typval_T *argvars, typval_T *rettv);
void f_reltimefloat(typval_T *argvars, typval_T *rettv);
void f_reltimestr(typval_T *argvars, typval_T *rettv);
void f_strftime(typval_T *argvars, typval_T *rettv);
void f_strptime(typval_T *argvars, typval_T *rettv);
void time_push(void *tv_rel, void *tv_start);
void time_pop(void *tp);
void time_msg(char *mesg, void *tv_start);
time_T get8ctime(FILE *fd);
int put_time(FILE *fd, time_T the_time);
void time_to_bytes(time_T the_time, char_u *buf);
void add_time(char_u *buf, size_t buflen, time_t tt);
/* vim: set ft=c : */
