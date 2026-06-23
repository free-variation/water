% Unification micro-benchmarks — SWI-Prolog reference for the unification
% sections of logic.l4 (deep spine, wide, nested frame). Each term is built
% once; the loop unifies it against ground then backtracks, so it measures
% unify + bind + trail-undo, as logic.l4 does. Note: Prolog has no open
% records, so `frame` is a fixed compound (no :extra field), and `wide` is a
% list where logicforth uses a flat array.
:- initialization(main, main).

var_spine(0, _).
var_spine(D, t(_, R))    :- D > 0, D1 is D - 1, var_spine(D1, R).
ground_spine(0, 0).
ground_spine(D, t(D, R)) :- D > 0, D1 is D - 1, ground_spine(D1, R).

loop(_, _, 0) :- !.
loop(V, G, N) :- ( V = G, fail ; true ), N1 is N - 1, loop(V, G, N1).

main :-
    var_spine(250, DV), ground_spine(250, DG),
    get_time(A0), loop(DV, DG, 300000), get_time(A1), T1 is A1 - A0,
    format("deep-unify   (depth 250 x 300k): ~6f s~n", [T1]),

    length(WV, 6000), numlist(1, 6000, WG),
    get_time(B0), loop(WV, WG, 250000), get_time(B1), T2 is B1 - B0,
    format("wide-unify   (6000 vars x 250k):  ~6f s~n", [T2]),

    FV = f(_,_,_,_, addr(geo(_,_,_),_,_), tags(_,_,_)),
    FG = f(1,2,3,4, addr(geo(51,0,12),10001,ny), tags(7,8,9)),
    get_time(C0), loop(FV, FG, 5000000), get_time(C1), T3 is C1 - C0,
    format("frame-unify  (12 vars x 5M):      ~6f s~n", [T3]),

    Total is T1 + T2 + T3,
    format("elapsed: ~6f s~n", [Total]).
