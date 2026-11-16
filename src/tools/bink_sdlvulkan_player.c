/*
 * libbink - SDL + Vulkan playback example
 *
 * On macOS this runs through MoltenVK, which the Vulkan loader picks up
 * from its ICD file.
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

#include "tools/bink_renderer.h"
#include "tools/bink_sdl_player_core.h"
#include "tools/bink_vulkan_renderer.h"

static const BinkRendererBackend vulkan_backend = {
    "vulkan",
    SDL_WINDOW_VULKAN,
    bink_vulkan_prepare,
    bink_vulkan_renderer_create,
    bink_vulkan_renderer_upload,
    bink_vulkan_renderer_upload_motion,
    bink_vulkan_renderer_draw,
    bink_vulkan_renderer_readback,
    bink_vulkan_renderer_destroy
};

int main(int argc, char **argv)
{
    return bink_player_run(argc, argv, &vulkan_backend);
}
