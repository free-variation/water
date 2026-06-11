" Vim indent file for logicforth (.l4)

if exists("b:did_indent")
  finish
endif
let b:did_indent = 1

setlocal indentexpr=GetLogicforthIndent()
setlocal indentkeys=!^F,o,O,0=then,0=else,0=until,0=again,0=repeat,0=;,0=:]
setlocal nolisp nosmartindent

if exists("*GetLogicforthIndent")
  finish
endif

" Strip strings and comments so their contents aren't counted as structure.
function! s:Clean(line) abort
  let l = a:line
  let l = substitute(l, '"\%([^"]\|""\)*"', '', 'g')
  let l = substitute(l, '\%(^\|\s\)\zs\\\%(\s.*\)\=$', '', '')
  let l = substitute(l, '(\s.\{-})', '', 'g')
  return l
endfunction

" Count non-overlapping matches of pat in line.
function! s:Count(line, pat) abort
  let n = 0
  let i = match(a:line, a:pat)
  while i >= 0
    let n += 1
    let i = match(a:line, a:pat, i + 1)
  endwhile
  return n
endfunction

function! GetLogicforthIndent() abort
  let pnum = prevnonblank(v:lnum - 1)
  if pnum == 0
    return 0
  endif

  let prev = s:Clean(getline(pnum))
  let cur  = s:Clean(getline(v:lnum))
  let sw   = shiftwidth()

  " Structure openers on the previous line.
  let openers = s:Count(prev, '\[:')
        \ + s:Count(prev, '\<if\>')
        \ + s:Count(prev, '?if')
        \ + s:Count(prev, '\<begin\>')
        \ + s:Count(prev, '\<else\>')
        \ + s:Count(prev, '\%(^\|\s\):\%(\s\|$\)')

  " Block enders that balance an opener on the same line (defs, quotations).
  let closers = s:Count(prev, ':\]')
        \ + s:Count(prev, '\%(^\|\s\);\%(\s\|$\)')

  let ind = indent(pnum) + (openers - closers) * sw

  " A line that *starts* with a closer aligns one level out, with its opener.
  if cur =~# '^\s*\%(;\|:\]\|then\|else\|until\|again\|repeat\)\>'
    let ind -= sw
  endif

  return ind > 0 ? ind : 0
endfunction
