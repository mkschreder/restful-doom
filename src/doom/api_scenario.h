//
// Small levels that each pose one problem, built fresh every episode.
//
// The real levels are the wrong thing to learn on and the right thing to be
// tested on. DOOM is deterministic: the same level at a different seed is the
// same level, so a rollout over three maps is three situations however many
// episodes it runs. And each of those situations asks for everything at once
// - navigate, fight, survive a floor that hurts, find a switch - so nothing
// in the return says which part was learned.
//
// These are ViZDoom's scenarios, rebuilt on this engine. The designs are
// theirs; none of the files are, because theirs are UDMF geometry driven by
// compiled ACS and this engine reads neither. Rebuilding them here also buys
// something ViZDoom does not have: the map itself is generated, so a maze is
// a DIFFERENT maze every episode rather than the same one from a new corner.
//
// The agent sees exactly what it sees on a real level. Same observation, same
// options, same route. Only the world is smaller and more pointed.
//

#ifndef __API_SCENARIO__
#define __API_SCENARIO__

#include "doomtype.h"

// The scenario names accepted by the episode endpoint, in order.
int Scenario_Count(void);
const char *Scenario_Name(int index);

// Build `name` with `seed` and arrange for the next episode to play it.
// Writes the episode and map to start. False if the name is not one of ours.
boolean Scenario_Select(const char *name, unsigned int seed,
                        int *episode, int *map);

// The lump a generated level is published under, for p_setup.c to load in
// place of the ExMy the episode and map would otherwise name.
const char *Scenario_LumpName(void);

// Go back to playing the game's own levels.
void Scenario_Clear(void);

boolean Scenario_Active(void);
const char *Scenario_Current(void);

// Called by p_setup.c once the generated level is standing, and by p_tick.c
// once per game tic. Both do nothing unless a scenario is running.
void Scenario_LevelLoaded(void);
void Scenario_PerTic(void);

#endif
