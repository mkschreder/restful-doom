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
//
//    


#ifndef __M_RANDOM__
#define __M_RANDOM__


#include "doomtype.h"



// Returns a number from 0 to 255,
// from a lookup table.
int M_Random (void);

// As M_Random, but used only by the play simulation.
int P_Random (void);

// Fix randoms for demos.
void M_ClearRandom (void);



// The gameplay and presentation RNG cursors.
//
// Exposed so that a simulation checkpoint can put them back: a restore has to
// call G_InitNew to stand the level up, G_InitNew calls M_ClearRandom, and a
// counterfactual that re-seeds the dice is not returning to the state it
// claims to. See api_snapshot.c.
extern int rndindex;
extern int prndindex;

#endif