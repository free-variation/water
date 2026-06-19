" Vim indent file for logicforth (.l4)

if exists("b:did_indent")
  finish
endif
let b:did_indent = 1

setlocal indentexpr=GetLogicforthIndent()
setlocal indentkeys=!^F,o,O,0=then,0=else,0=until,0=again,0=repeat,0=;,0=:],0=},0=],0=>
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

" Whole-token openers/closers: defs, control flow, quotations, and the
" structure literals (frames {}, arrays [], sets <>). Counted as standalone
" whitespace-delimited tokens, so the > inside words (>r, side>, cons>array)
" is never mistaken for a set close.
let s:openers = ['[:', '{', '[', '<', 'if', '?if', 'begin', 'else', ':']
let s:closers = [':]', '}', ']', '>', ';']
let s:dedent  = [';', ':]', '}', ']', '>', 'then', 'else', 'until', 'again', 'repeat']

function! GetLogicforthIndent() abort
  let pnum = prevnonblank(v:lnum - 1)
  if pnum == 0
    return 0
  endif

  let sw = shiftwidth()
  let opened = 0
  let closed = 0
  for tok in split(s:Clean(getline(pnum)))
    if index(s:openers, tok) >= 0
      let opened += 1
    elseif index(s:closers, tok) >= 0
      let closed += 1
    endif
  endfor

  " A net-opening previous line indents the body one level; a pure closer line
  " does not reduce the next line further (its own dedent already returned to
  " the opener's column).
  let ind = indent(pnum)
  if opened > closed
    let ind += sw
  endif

  " A line that *starts* with a closer aligns one level out, with its opener.
  if index(s:dedent, get(split(s:Clean(getline(v:lnum))), 0, '')) >= 0
    let ind -= sw
  endif

  return ind > 0 ? ind : 0
endfunction
