/*
 * libbink - SDL + OpenGL playback example
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

#include "tools/bink_gl_renderer.h"
#include "tools/bink_renderer.h"
#include "tools/bink_sdl_player_core.h"

static const BinkRendererBackend opengl_backend = {
    "opengl",
    SDL_WINDOW_OPENGL,
    bink_gl_prepare,
    bink_gl_renderer_create,
    bink_gl_renderer_upload,
    bink_gl_renderer_upload_motion,
    bink_gl_renderer_draw,
    bink_gl_renderer_readback,
    bink_gl_renderer_destroy
};

int main(int argc, char **argv)
{
    return bink_player_run(argc, argv, &opengl_backend);
}
