" Vim indent file for logicforth (.l4)

if exists("b:did_indent")
  finish
endif
let b:did_indent = 1

setlocal indentexpr=GetLogicforthIndent()
setlocal indentkeys=!^F,o,O,0=then,0=else,0=until,0=again,0=repeat,0=;,0=:],0=},0=],0=>,0=)]
setlocal nolisp nosmartindent

if exists("*GetLogicforthIndent")
  finish
endif

" Blank out string and comment spans, preserving column positions, so their
" contents are neither tokenised as structure nor shift the columns we align to.
function! s:Blank(line) abort
  let l = a:line
  let l = substitute(l, '"\%([^"]\|""\)*"', '\=repeat(" ", len(submatch(0)))', 'g')
  let l = substitute(l, '\%(^\|\s\)\zs\\\%(\s.*\)\=$', '\=repeat(" ", len(submatch(0)))', '')
  let l = substitute(l, '\%(\[\)\@<!(\s.\{-})', '\=repeat(" ", len(submatch(0)))', 'g')
  return l
endfunction

" Whitespace-delimited tokens with their 0-based *display* columns (tabs
" expanded per 'tabstop'), so alignment matches the screen even with tab
" indentation, and so the > inside words (>r, frame>json) is never mistaken
" for a set close.
function! s:Tokens(line) abort
  let toks = []
  let i = 0
  let n = len(a:line)
  let vcol = 0
  let ts = &tabstop
  while i < n
    let ch = a:line[i]
    if ch == "\t"
      let vcol += ts - (vcol % ts)
      let i += 1
      continue
    elseif ch == ' '
      let vcol += 1
      let i += 1
      continue
    endif
    let start = i
    let startv = vcol
    while i < n && a:line[i] != ' ' && a:line[i] != "\t"
      let vcol += 1
      let i += 1
    endwhile
    call add(toks, [a:line[start : i - 1], startv])
  endwhile
  return toks
endfunction

" Structure literals align continuation lines to the column of the first token
" after the opener; control-flow and definitions indent their body one level.
let s:bopen  = {'{': 1, '[': 1, '<': 1, '[:': 1, '[(': 1}
let s:bclose = {'}': 1, ']': 1, '>': 1, ':]': 1, ')]': 1}
let s:cfopen = {'if': 1, '?if': 1, 'begin': 1}
let s:cfclose = {'then': 1, 'until': 1, 'again': 1, 'repeat': 1, ';': 1}

function! GetLogicforthIndent() abort
  let sw = shiftwidth()
  let stack = []

  for lnum in range(1, v:lnum - 1)
    let toks = s:Tokens(s:Blank(getline(lnum)))
    let base = indent(lnum)
    let ti = 0
    let ntoks = len(toks)
    while ti < ntoks
      let tok = toks[ti][0]
      let col = toks[ti][1]
      if has_key(s:bopen, tok)
        let bodycol = (ti + 1 < ntoks) ? toks[ti + 1][1] : col + sw
        call add(stack, [col, bodycol])
      elseif tok ==# ':'
        " Forth definitions do not nest: a new : starts fresh at top level,
        " so an unterminated definition above cannot indent everything below.
        let stack = [[base, base + sw]]
      elseif has_key(s:cfopen, tok)
        call add(stack, [base, base + sw])
      elseif tok ==# 'else'
        if !empty(stack)
          let top = remove(stack, -1)
          call add(stack, [top[0], top[0] + sw])
        endif
      elseif has_key(s:bclose, tok) || has_key(s:cfclose, tok)
        if !empty(stack)
          call remove(stack, -1)
        endif
      endif
      let ti += 1
    endwhile
  endfor

  if empty(stack)
    return 0
  endif

  let cur = s:Tokens(s:Blank(getline(v:lnum)))
  let first = empty(cur) ? '' : cur[0][0]
  if first ==# ':'
    return 0
  endif
  let top = stack[-1]
  if has_key(s:bclose, first) || has_key(s:cfclose, first) || first ==# 'else'
    return top[0]
  endif
  return top[1]
endfunction
