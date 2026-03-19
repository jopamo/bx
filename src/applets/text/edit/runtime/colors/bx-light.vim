set background=light
hi clear
if exists("syntax_on")
  syntax reset
endif
let colors_name = "bx-light"
hi Normal ctermfg=16 ctermbg=NONE guifg=#202020 guibg=#f6f6f6
hi LineNr ctermfg=246 ctermbg=NONE guifg=#8a8a8a guibg=NONE
hi CursorLineNr ctermfg=88 ctermbg=NONE gui=bold guifg=#870000 guibg=NONE
hi StatusLine cterm=bold ctermfg=16 ctermbg=252 gui=bold guifg=#202020 guibg=#d0d0d0
hi StatusLineNC ctermfg=240 ctermbg=254 guifg=#6c6c6c guibg=#e4e4e4
hi Visual ctermfg=NONE ctermbg=153 guifg=NONE guibg=#cfe8ff
hi Search ctermfg=16 ctermbg=186 guifg=#202020 guibg=#d7d787
hi IncSearch ctermfg=16 ctermbg=215 gui=bold guifg=#202020 guibg=#ffaf5f
hi MatchParen cterm=bold ctermfg=16 ctermbg=117 gui=bold guifg=#202020 guibg=#87d7ff
