scriptencoding utf-8

set nocompatible
set encoding=utf-8

runtime filetype.vim
runtime ftplugin.vim
runtime indent.vim

let mapleader = ","

set nomodeline
set modelines=0

set number
set norelativenumber

set ruler
set laststatus=2

set visualbell
set ttyfast
set showmode
set showcmd
set nowrap
set listchars=tab:▸\ ,eol:¬
nnoremap <leader>l :set list!<CR>

set mouse=

set t_Co=256
set background=dark
runtime syntax/syntax.vim

set textwidth=0
set formatoptions=crq
set tabstop=8
set shiftwidth=0
set softtabstop=-1
set noexpandtab
set shiftround
set autoindent
set nocindent
set nosmartindent

set scrolloff=3
set backspace=indent,eol,start

set matchpairs+=<:>
runtime! macros/matchit.vim

nnoremap j gj
nnoremap k gk

set hidden

nnoremap / /\v
vnoremap / /\v
set hlsearch
set incsearch
set ignorecase
set smartcase
set showmatch
nnoremap <leader><space> :nohlsearch<CR>

inoremap <F1> <Esc>
nnoremap <F1> <Nop>
vnoremap <F1> <Nop>

nnoremap <leader>q gqip
