/* popupmenu.c */
void pum_set_border(int enable);
void pum_set_shadow(int enable);
void pum_set_margin(int enable);
void pum_display(pumitem_T *array, int size, int selected);
void pum_call_update_screen(void);
int pum_under_menu(int row, int col, int only_redrawing);
void pum_opacity_changed(void);
void pum_redraw(void);
void pum_undisplay(void);
void pum_clear(void);
int pum_visible(void);
int pum_redraw_in_same_position(void);
void pum_may_redraw(void);
int pum_get_height(void);
void pum_set_event_info(dict_T *dict);
void pum_set_border_chars(int top, int right, int bottom, int left, int top_left, int top_right, int bottom_right, int bottom_left);
void put_shadow_char(int row, int col);
/* vim: set ft=c : */
