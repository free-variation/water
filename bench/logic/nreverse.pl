% naive reverse, the classic Prolog LIPS benchmark — SWI-Prolog reference for
% nreverse.l4. Builds a 30-element list once, then nrev's it 30000 times.
:- initialization(main, main).

app([], B, B).
app([H|T], B, [H|R]) :- app(T, B, R).

nrev([], []).
nrev([H|T], R) :- nrev(T, RT), app(RT, [H], R).

mklist(0, []) :- !.
mklist(N, [N|T]) :- N1 is N - 1, mklist(N1, T).

loop(_, 0) :- !.
loop(L, N) :- nrev(L, _), N1 is N - 1, loop(L, N1).

main :-
    mklist(5, L5), nrev(L5, R5), writeln(R5),
    mklist(30, L30),
    get_time(T0), loop(L30, 30000), get_time(T1),
    D is T1 - T0,
    LIPS is 496 * 30000 / D,
    format("nrev(30) x 30000:  ~6f s~n", [D]),
    format("elapsed: ~6f s   LIPS: ~2f~n", [D, LIPS]).
