//
// Copyright(C) 1993-1996 Id Software, Inc.
// Copyright(C) 2005-2014 Simon Howard
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// DESCRIPTION:
//	Main program, simply calls D_DoomMain high level loop.
//

#include "config.h"

#include <stdio.h>

#include "SDL.h"

#include "doomtype.h"
#include "i_system.h"
#include "m_argv.h"

//
// D_DoomMain()
// Not a globally visible function, just included for source reference,
// calls all startup code, parses command line options.
//

void D_DoomMain (void);

int main(int argc, char **argv)
{
    // Line-buffered from the very first write, before anything has been
    // printed - setvbuf is only defined that way, and an agent runs this
    // engine as a subprocess with its output on a pipe, which libc otherwise
    // makes block-buffered. Everything the API's route builder says about
    // what it could and could not reach then sits in a buffer until the
    // process exits, and a subprocess that gets killed never does.
    setvbuf(stdout, NULL, _IOLBF, 0);

    // save arguments

    myargc = argc;
    myargv = argv;

    M_FindResponseFile();

    // start doom

    D_DoomMain ();

    return 0;
}

