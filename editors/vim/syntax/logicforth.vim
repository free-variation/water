" Vim syntax file for logicforth (.l4)
" Relies on the broad 'iskeyword' set in ftplugin/logicforth.vim so that
" punctuation words (+, ~, >r, @i,j, has?, kebab-case) match as keywords.

if exists("b:current_syntax")
  finish
endif

syn case match
syn iskeyword @,48-57,_,192-255,33,35-39,42-47,58-64,94,126

" --- Comments -------------------------------------------------------------
" `\ …` to end of line (the \ must be its own token), and ( … ) stack comments.
syn match   logicforthComment "\%(^\|\s\)\zs\\\%(\s.*\)\=$" contains=@Spell
syn region  logicforthComment start="(\s" end=")" contains=@Spell

" --- Strings --------------------------------------------------------------
" Raw strings; "" is the one escape (a literal ").
syn region  logicforthString start=+"+ skip=+""+ end=+"+ contains=@Spell

" --- Numbers --------------------------------------------------------------
syn match   logicforthNumber "\<-\=\d\+\%(\.\d\+\)\=\%([eE][-+]\=\d\+\)\=\>"

" --- Symbol and path literals ---------------------------------------------
syn match   logicforthSymbol  ":\k\+"
syn match   logicforthPath    "/\a\k*"

" --- Capitalized identifier = logic variable ------------------------------
syn match   logicforthLogicVar "\<\u\w*\>"

" --- Defining / structure -------------------------------------------------
syn keyword logicforthDefine : ; variable symbol to forget inline ' lookup immediate
syn match   logicforthDefine "\[:"
syn match   logicforthDefine ":\]"
" Name introduced by a colon definition.
syn match   logicforthDefName "\%(^\|\s\):\s\+\zs\k\+"

" --- Control flow ---------------------------------------------------------
syn keyword logicforthConditional if ?if else then
syn keyword logicforthRepeat begin until again while repeat times i-times
syn keyword logicforthKeyword exit execute

" --- Logic ----------------------------------------------------------------
syn keyword logicforthLogic lvar unify ~ deref $ amb fail

syn keyword logicforthBoolean null

" --- Structural delimiters ------------------------------------------------
syn match   logicforthDelimiter "[][{}<>]"
syn match   logicforthDelimiter "\s\zs|\ze\s"

" --- Built-in words -------------------------------------------------------
syn keyword logicforthBuiltin ! !i % * *! + +! ++ - -! -- . .a .s / /! 0= 1+ 1-
syn keyword logicforthBuiltin = >r >side side> @ @i @i,j @j ^ 2dup nip
syn keyword logicforthBuiltin dup drop swap over rot depth roll clear
syn keyword logicforthBuiltin and or not 0= lt gt
syn keyword logicforthBuiltin negate abs sqrt exp log ln sq mod quotient pi now sleep
syn keyword logicforthBuiltin sin cos tan tanh asin acos atan round truncate round-up round-down
syn keyword logicforthBuiltin f* f*+ f*- f+ f++ f- f-- f/ f1+ f1- f^ fabs facos fasin fatan
syn keyword logicforthBuiltin fcos fexp fln flog fmod fnegate fround fround-down fround-up
syn keyword logicforthBuiltin fsin fsq fsqrt ftan ftanh ftruncate
syn keyword logicforthBuiltin set array array-of range concat reverse reverse-slice! take skip last
syn keyword logicforthBuiltin destruct destruct-to slice! to-slice! member? size union intersection difference
syn keyword logicforthBuiltin map mapn filter reduce
syn keyword logicforthBuiltin frame array>frame frame>array keys values has? merge delete-at update-at copy
syn keyword logicforthBuiltin json>frame frame>json
syn keyword logicforthBuiltin matrix 0-matrix diagonal-matrix identity-matrix matrix-range reshape
syn keyword logicforthBuiltin dim flatten transpose diagonal num-elements
syn keyword logicforthBuiltin dgemm-nn dgemm-nt dgemm-tn dgemm-tt
syn keyword logicforthBuiltin sum max min mean row-sums row-maxes row-mins row-means
syn keyword logicforthBuiltin column-sums column-maxes column-mins column-means
syn keyword logicforthBuiltin match match-all replace split join substring format string>symbol
syn keyword logicforthBuiltin emit cr . .s .a
syn keyword logicforthBuiltin r> r@ >side side> side-drop side-depth
syn keyword logicforthBuiltin reset shift shift-with resume throw catch try-catch
syn keyword logicforthBuiltin load reload save save-image load-image
syn keyword logicforthBuiltin read-file write-file append-file env env!
syn keyword logicforthBuiltin start-process read write close wait stop running? run parallel-run
syn keyword logicforthBuiltin read-out read-err write-in
syn keyword logicforthBuiltin words see see-compiled man help gc bye
syn keyword logicforthBuiltin vf* vf+ vf- vf/ vfabs vfcos vfexp vflog vfneg vfsin vfsq vfsqrt vftan vftanh
syn keyword logicforthBuiltin vvf* vvf*+ vvf*- vvf+ vvf- vvf/

" --- Highlight links ------------------------------------------------------
hi def link logicforthComment      Comment
hi def link logicforthString       String
hi def link logicforthNumber       Number
hi def link logicforthSymbol       Constant
hi def link logicforthPath         Constant
hi def link logicforthLogicVar     Identifier
hi def link logicforthDefine       Define
hi def link logicforthDefName      Function
hi def link logicforthConditional  Conditional
hi def link logicforthRepeat       Repeat
hi def link logicforthKeyword      Keyword
hi def link logicforthLogic        Special
hi def link logicforthBoolean      Boolean
hi def link logicforthDelimiter    Delimiter
hi def link logicforthBuiltin      Statement

let b:current_syntax = "logicforth"
