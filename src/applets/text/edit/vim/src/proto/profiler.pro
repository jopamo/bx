/* profiler.c */
void profile_start(proftime_T *tm);
void profile_end(proftime_T *tm);
void profile_sub(proftime_T *tm, proftime_T *tm2);
char *profile_msg(proftime_T *tm);
float_T profile_float(proftime_T *tm);
void profile_setlimit(long msec, proftime_T *tm);
int profile_passed_limit(proftime_T *tm);
void profile_zero(proftime_T *tm);
/* vim: set ft=c : */
