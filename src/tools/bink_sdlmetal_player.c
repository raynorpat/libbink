/*
 * libbink - SDL + Metal playback example (macOS)
 *
 * Keys: Esc/q quit, Space pauses, Left/Right seek one second,
 *       M toggles interpolation.
 *
 * Hooks (environment): BINK_PLAYER_TEST_FRAMES, BINK_PLAYER_SHADER,
 * BINK_PLAYER_T, BINK_PLAYER_DUMP, BINK_PLAYER_TEST_SEEK.
 */

#ifdef BINK_USE_SDL3
#include <SDL3/SDL.h>
#else
#include <SDL.h>
#endif

#include "tools/bink_metal_renderer.h"
#include "tools/bink_renderer.h"
#include "tools/bink_sdl_player_core.h"

static const BinkRendererBackend metal_backend = {
    "metal",
    SDL_WINDOW_METAL,
    bink_metal_prepare,
    bink_metal_renderer_create,
    bink_metal_renderer_upload,
    bink_metal_renderer_upload_motion,
    bink_metal_renderer_draw,
    bink_metal_renderer_readback,
    bink_metal_renderer_destroy
};

int main(int argc, char **argv)
{
    return bink_player_run(argc, argv, &metal_backend);
}
