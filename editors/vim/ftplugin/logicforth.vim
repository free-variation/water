" logicforth filetype plugin: word characters, comments, paren matching
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
        \ '\[::\:\]'
  let b:match_ignorecase = 0
endif

" `;` is a space-delimited word (it ends a definition). Gluing it to the
" previous token — `quantile;` instead of `quantile ;` — reads as one unknown
" word and is a common, silent mistake. On typing ;, insert a leading space
" unless the previous character is already whitespace or the line is empty.
function! s:SpaceBeforeSemicolon() abort
  let column = col('.')
  if column <= 1
    return ';'
  endif
  let previous = getline('.')[column - 2]
  if previous ==# ' ' || previous ==# "\t"
    return ';'
  endif
  return ' ;'
endfunction

inoremap <buffer><expr> ; <SID>SpaceBeforeSemicolon()

let b:undo_ftplugin = "setlocal iskeyword< comments< commentstring< matchpairs<"
      \ . " | unlet! b:match_words b:match_ignorecase"
      \ . " | silent! iunmap <buffer> ;"
