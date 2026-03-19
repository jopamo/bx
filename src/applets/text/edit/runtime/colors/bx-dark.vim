set background=dark
hi clear
if exists("syntax_on")
  syntax reset
endif
let colors_name = "bx-dark"
hi Normal ctermfg=252 ctermbg=NONE guifg=#d0d0d0 guibg=#101010
hi LineNr ctermfg=244 ctermbg=NONE guifg=#808080 guibg=NONE
hi CursorLineNr ctermfg=223 ctermbg=NONE gui=bold guifg=#ffd787 guibg=NONE
hi StatusLine cterm=bold ctermfg=255 ctermbg=238 gui=bold guifg=#ffffff guibg=#444444
hi StatusLineNC ctermfg=250 ctermbg=236 guifg=#bcbcbc guibg=#303030
hi Visual ctermfg=NONE ctermbg=239 guifg=NONE guibg=#3a3a5a
hi Search ctermfg=16 ctermbg=221 guifg=#000000 guibg=#ffd75f
hi IncSearch ctermfg=16 ctermbg=214 gui=bold guifg=#000000 guibg=#ffaf00
hi MatchParen cterm=bold ctermfg=231 ctermbg=31 gui=bold guifg=#ffffff guibg=#0087af
