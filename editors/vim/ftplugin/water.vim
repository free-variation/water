" water filetype plugin: word characters, comments, paren matching
if exists("b:did_ftplugin")
  finish
endif
let b:did_ftplugin = 1

" Tokens are whitespace-delimited and may be built from punctuation
" (+, ~, >r, @i,j, has?, column-sums, …). Treat that punctuation as part of
" a keyword so word-motions and syntax keyword matching see whole tokens.
" The structural delimiters ( ) [ ] { } | " \ are deliberately left out.
setlocal iskeyword=@,48-57,_,192-255,33,35-39,42-47,58-64,94,126

" Comments: \ to end of line, and ( … ) stack comments.
setlocal comments=:\\
setlocal commentstring=\\\ %s

" Paren matching for arrays [ ], frames { }, and ( ) stack comments.
setlocal matchpairs=(:),[:],{:}

" matchit: jump across the multi-word control structures with %.
if exists("loaded_matchit") || exists("g:loaded_matchit")
  let b:match_words =
        \ '\<if\>:\<else\>:\<then\>,' .
        \ '\<begin\>:\<while\>:\<until\>:\<again\>:\<repeat\>,' .
        \ '\%(\[:\|\[|\|\[>\):\:\]'
  let b:match_ignorecase = 0
endif

" Self-delimiting punctuation, mirroring the tokenizer: ; ] } end a token,
" [ { start one, and the two-char forms ([: [( [| [> and :] )]) stay whole.
" These insert-mode helpers add the canonical spaces as you type, and stay
" quiet inside strings and comments.
function! s:InStringOrComment() abort
  let name = synIDattr(synID(line('.'), max([col('.') - 1, 1]), 1), 'name')
  return name =~# 'String\|Comment'
endfunction

function! s:TokenBeforeCursor() abort
  return matchstr(getline('.')[: col('.') - 2], '\S*$')
endfunction

function! s:SpaceBeforeSemicolon() abort
  if col('.') <= 1 || s:InStringOrComment()
    return ';'
  endif
  let previous = getline('.')[col('.') - 2]
  if previous ==# ' ' || previous ==# "\t"
    return ';'
  endif
  return ' ;'
endfunction

" ] and }: insert a leading space unless the closer completes a two-char
" form (:] or )], where the space is rewritten to sit before the pair) or
" closes a bracket opened inside the same token (a path predicate).
function! s:SpaceBeforeCloser(closer) abort
  if col('.') <= 1 || s:InStringOrComment()
    return a:closer
  endif
  let token = s:TokenBeforeCursor()
  if token ==# ''
    return a:closer
  endif
  let opener = a:closer ==# ']' ? '[' : '{'
  if count(split(token, '\zs'), opener) > count(split(token, '\zs'), a:closer)
    return a:closer
  endif
  let previous = token[-1:]
  if a:closer ==# ']' && (previous ==# ':' || previous ==# ')')
    if strlen(token) == 1
      return a:closer
    endif
    return "\<BS> " . previous . a:closer
  endif
  return ' ' . a:closer
endfunction

inoremap <buffer><expr> ; <SID>SpaceBeforeSemicolon()
inoremap <buffer><expr> ] <SID>SpaceBeforeCloser(']')
inoremap <buffer><expr> } <SID>SpaceBeforeCloser('}')

" [ and {: the space after them is decided at the NEXT keystroke — [x
" becomes [ x unless x forms a two-char opener; {x always spaces.
function! s:SpaceAfterOpener() abort
  if v:char =~# '\s' || s:InStringOrComment()
    return
  endif
  let token = s:TokenBeforeCursor()
  if token ==# '['
    if v:char !~# '[:(|>]'
      let v:char = ' ' . v:char
    endif
  elseif token ==# '{' || token ==# '[:' || token ==# '[(' ||
        \ token ==# '[|' || token ==# '[>'
    let v:char = ' ' . v:char
  endif
endfunction

augroup WaterOpenerSpace
  autocmd! * <buffer>
  autocmd InsertCharPre <buffer> call s:SpaceAfterOpener()
augroup END

let b:undo_ftplugin = "setlocal iskeyword< comments< commentstring< matchpairs<"
      \ . " | unlet! b:match_words b:match_ignorecase"
      \ . " | silent! iunmap <buffer> ;"
      \ . " | silent! iunmap <buffer> ]"
      \ . " | silent! iunmap <buffer> }"
      \ . " | silent! autocmd! WaterOpenerSpace * <buffer>"
