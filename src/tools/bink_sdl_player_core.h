#ifndef BINK_SDL_PLAYER_CORE_H
#define BINK_SDL_PLAYER_CORE_H

#include "tools/bink_renderer.h"

/* Run one SDL player with the given renderer backend. Each player's main is
 * a backend table and this call. */
int bink_player_run(int argc, char **argv,
                    const BinkRendererBackend *backend);

#endif
