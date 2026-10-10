#include <stdio.h>

#include "m_argv.h"

#include "doomgeneric.h"

pixel_t* DG_ScreenBuffer = NULL;

void M_FindResponseFile(void);
void D_DoomMain (void);


void doomgeneric_Create(int argc, char **argv)
{
	// save arguments
    myargc = argc;
    myargv = argv;

	M_FindResponseFile();

    DG_Init();

    /*
     * I_VideoBuffer is allocated by I_InitGraphics(), which happens during
     * Doom initialization.  Do not point DG_ScreenBuffer at it here: at this
     * point I_VideoBuffer is still NULL.  TanjaOS presents I_VideoBuffer
     * directly from I_FinishUpdate()/DG_DrawFrame(), so the alias is installed
     * after the real screen buffer has been allocated.
     */
    D_DoomMain ();
}

