#ifndef LAYOUT_ENGINE_FFI_H
#define LAYOUT_ENGINE_FFI_H

#include <stdint.h>

#if defined(_WIN32)
#define EXPORT __declspec(dllexport)
#else
#define EXPORT __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C"
{
#endif
    // Initializes a new layout engine
    EXPORT void *init_layout_engine();

    // Generate test data
    EXPORT void generate_test_data(void *layout_engine);

    EXPORT void zoom(void *layout_engine_ptr, float zoom);

    EXPORT void zoom_fit(void *layout_engine_ptr);

    EXPORT void pan(void *layout_engine_ptr, float x_um, float y_um);

    EXPORT void serialize_library_browser_data(void *layout_engine);

    // Expose data memory address to Dart FFI
    EXPORT const uint8_t *get_browser_data_ptr();

    // Expose data size boundaries to Dart FFI
    EXPORT uint32_t get_browser_data_size();

    // Destroy layout engine and release memory
    EXPORT void destroy(void *layout_engine_ptr);

#ifdef __cplusplus
}
#endif
#endif // LAYOUT_ENGINE_FFI_H