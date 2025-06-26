/* Global declarations for the ed editor.  */
/* GNU ed - The GNU line editor.
   Copyright (C) 1993, 1994 Andrew L. Moore, Talke Studio
   Copyright (C) 2006-2026 Antonio Diaz Diaz.

   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <stdbool.h>

/* Prefix GNU ed symbols so the applet can live inside the multicall bx binary
   without colliding with unrelated applets or helpers. */
#define jmp_state bx_ed_jmp_state
#define append_lines bx_ed_append_lines
#define close_sbuf bx_ed_close_sbuf
#define copy_lines bx_ed_copy_lines
#define current_addr bx_ed_current_addr
#define dec_addr bx_ed_dec_addr
#define delete_lines bx_ed_delete_lines
#define get_line_node_addr bx_ed_get_line_node_addr
#define get_sbuf_line bx_ed_get_sbuf_line
#define inc_addr bx_ed_inc_addr
#define inc_current_addr bx_ed_inc_current_addr
#define init_buffers bx_ed_init_buffers
#define isbinary bx_ed_isbinary
#define join_lines bx_ed_join_lines
#define last_addr bx_ed_last_addr
#define modified bx_ed_modified
#define warned bx_ed_warned
#define move_lines bx_ed_move_lines
#define open_sbuf bx_ed_open_sbuf
#define path_max bx_ed_path_max
#define put_lines bx_ed_put_lines
#define put_sbuf_line bx_ed_put_sbuf_line
#define search_line_node bx_ed_search_line_node
#define set_binary bx_ed_set_binary
#define set_current_addr bx_ed_set_current_addr
#define set_modified bx_ed_set_modified
#define set_warned bx_ed_set_warned
#define yank_lines bx_ed_yank_lines
#define clear_undo_stack bx_ed_clear_undo_stack
#define push_undo_atom bx_ed_push_undo_atom
#define reset_undo_state bx_ed_reset_undo_state
#define undo bx_ed_undo
#define clear_active_list bx_ed_clear_active_list
#define next_active_node bx_ed_next_active_node
#define set_active_node bx_ed_set_active_node
#define unset_active_nodes bx_ed_unset_active_nodes
#define escchar bx_ed_escchar
#define get_extended_line bx_ed_get_extended_line
#define get_stdin_line bx_ed_get_stdin_line
#define linenum bx_ed_linenum
#define print_lines bx_ed_print_lines
#define read_file bx_ed_read_file
#define write_file bx_ed_write_file
#define reset_unterminated_line bx_ed_reset_unterminated_line
#define unmark_unterminated_line bx_ed_unmark_unterminated_line
#define extended_regexp bx_ed_extended_regexp
#define interactive bx_ed_interactive
#define may_access_filename bx_ed_may_access_filename
#define print_escaped bx_ed_print_escaped
#define restricted bx_ed_restricted
#define scripted bx_ed_scripted
#define show_strerror bx_ed_show_strerror
#define show_warning bx_ed_show_warning
#define strip_cr bx_ed_strip_cr
#define traditional bx_ed_traditional
#define error_msg bx_ed_error_msg
#define first_e_command bx_ed_first_e_command
#define invalid_address bx_ed_invalid_address
#define main_loop bx_ed_main_loop
#define set_def_filename bx_ed_set_def_filename
#define set_error_msg bx_ed_set_error_msg
#define set_prompt bx_ed_set_prompt
#define set_verbose bx_ed_set_verbose
#define unmark_line_node bx_ed_unmark_line_node
#define build_active_list bx_ed_build_active_list
#define get_pattern_for_s bx_ed_get_pattern_for_s
#define extract_replacement bx_ed_extract_replacement
#define next_matching_node_addr bx_ed_next_matching_node_addr
#define search_and_replace bx_ed_search_and_replace
#define set_subst_regex bx_ed_set_subst_regex
#define replace_subst_re_by_search_re bx_ed_replace_subst_re_by_search_re
#define subst_regex bx_ed_subst_regex
#define disable_interrupts bx_ed_disable_interrupts
#define enable_interrupts bx_ed_enable_interrupts
#define home_directory bx_ed_home_directory
#define resize_buffer bx_ed_resize_buffer
#define set_signals bx_ed_set_signals
#define set_window_lines bx_ed_set_window_lines
#define window_columns bx_ed_window_columns
#define window_lines bx_ed_window_lines

enum Pflags			/* print suffixes */
  {
  pf_l = 0x01,			/* list after command */
  pf_n = 0x02,			/* enumerate after command */
  pf_p = 0x04			/* print after command */
  };


typedef struct line_node		/* Line node */
  {
  struct line_node * q_forw;
  struct line_node * q_back;
  long pos;			/* position of text in scratch buffer */
  int len;			/* length of line ('\n' is not stored) */
  }
line_node;


enum { UADD = 0, UDEL = 1, UMOV = 2, VMOV = 3 };
typedef struct undo_atom		/* Undo atom */
  {
  int type;
  line_node * head;			/* head of list */
  line_node * tail;			/* tail of list */
  }
undo_atom;

#ifndef max
#define max( a, b ) ( (( a ) > ( b )) ? ( a ) : ( b ) )
#endif
#ifndef min
#define min( a, b ) ( (( a ) < ( b )) ? ( a ) : ( b ) )
#endif

static const char * const mem_msg = "Memory exhausted";
static const char * const no_prev_subst = "No previous substitution";

/* defined in buffer.c */
bool append_lines( const char ** const ibufpp, const int addr,
                   bool insert, const bool isglobal );
bool close_sbuf( void );
bool copy_lines( const int first_addr, const int second_addr, const int addr );
int current_addr( void );
int dec_addr( int addr );
bool delete_lines( const int from, const int to, const bool isglobal );
int get_line_node_addr( const line_node * const lp );
char * get_sbuf_line( const line_node * const lp );
int inc_addr( int addr );
int inc_current_addr( void );
bool init_buffers( void );
bool isbinary( void );
bool join_lines( const int from, const int to, const bool isglobal );
int last_addr( void );
bool modified( void );
bool warned( void );
bool move_lines( const int first_addr, const int second_addr, const int addr,
                 const bool isglobal );
bool open_sbuf( void );
int path_max( const char * filename );
bool put_lines( const int addr );
const char * put_sbuf_line( const char * const buf, const int size );
line_node * search_line_node( const int addr );
void set_binary( void );
void set_current_addr( const int addr );
void set_modified( const bool b );
void set_warned( const bool b );
bool yank_lines( const int from, const int to );
void clear_undo_stack( void );
undo_atom * push_undo_atom( const int type, const int from, const int to );
void reset_undo_state( void );
bool undo( const bool isglobal );

/* defined in global.c */
void clear_active_list( void );
const line_node * next_active_node( void );
bool set_active_node( const line_node * const lp );
void unset_active_nodes( const line_node * bp, const line_node * const ep );

/* defined in io.c */
unsigned char escchar( const unsigned char ch );
bool get_extended_line( const char ** const ibufpp, int * const lenp,
                        const bool strip_escaped_newlines );
const char * get_stdin_line( int * const sizep );
unsigned linenum( void );
bool print_lines( int from, const int to, const int pflags );
int read_file( const char * const filename, const int addr,
               bool * const read_onlyp );
int write_file( const char * const filename, const char * const mode,
                const int from, const int to );
void reset_unterminated_line( void );
void unmark_unterminated_line( const line_node * const lp );

/* defined in main.c */
bool extended_regexp( void );
bool interactive( void );
bool may_access_filename( const char * const name );
void print_escaped( const char * p, const bool to_stdout );
bool restricted( void );
bool scripted( void );
void show_strerror( const char * const filename, const int errcode );
void show_warning( const char * const filename, const char * const msg );
bool strip_cr( void );
bool traditional( void );

/* defined in main_loop.c */
const char * error_msg( void );
int first_e_command( const char * const filename );
void invalid_address( void );
int main_loop( const bool initial_error, const bool loose );
bool set_def_filename( const char * const s );
void set_error_msg( const char * const msg );
bool set_prompt( const char * const s );
void set_verbose( void );
void unmark_line_node( const line_node * const lp );

/* defined in regex.c */
bool build_active_list( const char ** const ibufpp, const int first_addr,
                        const int second_addr, const bool match );
const char * get_pattern_for_s( const char ** const ibufpp );
bool extract_replacement( const char ** const ibufpp, const bool isglobal );
int next_matching_node_addr( const char ** const ibufpp );
bool search_and_replace( const int first_addr, const int second_addr,
                         const int snum, const bool isglobal );
bool set_subst_regex( const char * const pat, const bool ignore_case );
bool replace_subst_re_by_search_re( void );
bool subst_regex( void );

/* defined in signal.c */
void disable_interrupts( void );
void enable_interrupts( void );
const char * home_directory( void );
bool resize_buffer( char ** const buf, int * const size, const unsigned min_size );
void set_signals( void );
void set_window_lines( const int lines );
int window_columns( void );
int window_lines( void );
