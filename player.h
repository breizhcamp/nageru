#ifndef _PLAYER_H
#define _PLAYER_H 1

#include "clip_list.h"

void start_player_thread();
void play_clip(const Clip &clip, unsigned stream_idx);

#endif  // !defined(_PLAYER_H)
