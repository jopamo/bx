#ifndef BX_VIM_FEATURE_OVERRIDES_H
#define BX_VIM_FEATURE_OVERRIDES_H

#define BX_VIM_EMBEDDED 1
#define BX_VIM_CURATED_RUNTIME 1
#define BX_VIM_NO_PACKAGES 1
#define BX_VIM_NO_COLOR_LISTS 1

#ifdef HAVE_CLIPMETHOD
# undef HAVE_CLIPMETHOD
#endif

#ifdef FEAT_GUI
# undef FEAT_GUI
#endif
#ifdef FEAT_GUI_GTK
# undef FEAT_GUI_GTK
#endif
#ifdef FEAT_GUI_MOTIF
# undef FEAT_GUI_MOTIF
#endif
#ifdef FEAT_GUI_PHOTON
# undef FEAT_GUI_PHOTON
#endif
#ifdef FEAT_GUI_MAC
# undef FEAT_GUI_MAC
#endif
#ifdef FEAT_GUI_MSWIN
# undef FEAT_GUI_MSWIN
#endif
#ifdef FEAT_CLIPBOARD
# undef FEAT_CLIPBOARD
#endif
#ifdef FEAT_XCLIPBOARD
# undef FEAT_XCLIPBOARD
#endif
#ifdef FEAT_CLIPBOARD_PROVIDER
# undef FEAT_CLIPBOARD_PROVIDER
#endif
#ifdef FEAT_SOCKETSERVER
# undef FEAT_SOCKETSERVER
#endif
#ifdef FEAT_CLIENTSERVER
# undef FEAT_CLIENTSERVER
#endif
#ifdef FEAT_JOB_CHANNEL
# undef FEAT_JOB_CHANNEL
#endif
#ifdef FEAT_TERMINAL
# undef FEAT_TERMINAL
#endif
#ifdef FEAT_NETBEANS_INTG
# undef FEAT_NETBEANS_INTG
#endif
#ifdef FEAT_LIBCALL
# undef FEAT_LIBCALL
#endif
#ifdef FEAT_SPELL
# undef FEAT_SPELL
#endif
#ifdef FEAT_FOLDING
# undef FEAT_FOLDING
#endif
#ifdef FEAT_SESSION
# undef FEAT_SESSION
#endif
#ifdef FEAT_VIMINFO
# undef FEAT_VIMINFO
#endif
#ifdef FEAT_CRYPT
# undef FEAT_CRYPT
#endif
#ifdef FEAT_SODIUM
# undef FEAT_SODIUM
#endif
#ifdef FEAT_PROFILE
# undef FEAT_PROFILE
#endif
#ifdef FEAT_TIMERS
# undef FEAT_TIMERS
#endif
#ifdef FEAT_PRINTER
# undef FEAT_PRINTER
#endif
#ifdef FEAT_POSTSCRIPT
# undef FEAT_POSTSCRIPT
#endif
#ifdef FEAT_MULTI_LANG
# undef FEAT_MULTI_LANG
#endif
#ifdef FEAT_LANGMAP
# undef FEAT_LANGMAP
#endif
#ifdef FEAT_KEYMAP
# undef FEAT_KEYMAP
#endif
#ifdef FEAT_CONCEAL
# undef FEAT_CONCEAL
#endif
#ifdef FEAT_MENU
# undef FEAT_MENU
#endif
#ifdef FEAT_TERM_POPUP_MENU
# undef FEAT_TERM_POPUP_MENU
#endif
#ifdef FEAT_TOOLBAR
# undef FEAT_TOOLBAR
#endif
#ifdef FEAT_PROP_POPUP
# undef FEAT_PROP_POPUP
#endif
#ifdef HAS_MESSAGE_WINDOW
# undef HAS_MESSAGE_WINDOW
#endif
#ifdef FEAT_SOUND
# undef FEAT_SOUND
#endif
#ifdef FEAT_SOUND_CANBERRA
# undef FEAT_SOUND_CANBERRA
#endif

#endif /* BX_VIM_FEATURE_OVERRIDES_H */
