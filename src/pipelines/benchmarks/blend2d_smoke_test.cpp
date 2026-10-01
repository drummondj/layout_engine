#include <blend2d/blend2d.h>

#include <cstdio>
#include <cstdlib>

// Throwaway build/link verification for the Blend2D FetchContent chain
// (CMakeLists.txt's own "Blend2D" block) - confirms blend2d::blend2d
// actually builds and links before any real RasterizeBlend2DStage code
// depends on it. Not a real benchmark or test; not run by ctest.
int main()
{
    BLImage img(64, 64, BL_FORMAT_PRGB32);
    BLContext ctx(img);
    ctx.clear_all();
    ctx.fill_rect(BLRect(4, 4, 32, 32), BLRgba32(0xFFFF0000u));
    ctx.end();

    BLImageData data;
    img.get_data(&data);
    if (data.size.w != 64 || data.size.h != 64)
    {
        std::fprintf(stderr, "blend2d_smoke_test: unexpected image size %dx%d\n", data.size.w, data.size.h);
        return EXIT_FAILURE;
    }

    std::printf("blend2d_smoke_test OK: %dx%d format=%d stride=%td\n", data.size.w, data.size.h, data.format, data.stride);
    return EXIT_SUCCESS;
}
