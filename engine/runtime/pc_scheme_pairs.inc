// pc_scheme_pairs.inc: linked action pairs. PC_PAIR(a, b): a macro's trigger and a key it reads while it runs. The two
// never share a key in any gameplay context where either has a row, across contexts: pc_bind_set refuses such a bind
// (PCB_CHAIN, no swap, the other action named), build() logs it if the settings make one, and the test keeps the defaults
// apart.
