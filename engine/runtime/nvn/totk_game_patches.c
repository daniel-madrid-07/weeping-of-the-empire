/* TotK game patches in the engine's runtime: the data page writer and its glue, built from the repo's games/totk sources
   (one TU here, so the shared game-object key's mkninja_win.py stays as it is). Inert with an unpatched main: the data
   page is unused slack after main's.bss, and the extra memory is 0.*/
#include "../../../games/totk/runtime/totk_patches.c"
#include "../../../games/totk/engine/totk_engine.c"
