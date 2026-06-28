#include "layout_engine.h"
#include "layout_engine_ffi.h"
#include <charconv>
#include <iostream>
#include <memory>
#include <sstream>
#include <algorithm>

#include "database/database_generated.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkColorSpace.h"
#include "include/core/SkFont.h"
#include "include/core/SkFontMgr.h"
#include "include/core/SkPaint.h"
#include "include/core/SkRect.h"
#include "include/core/SkSurface.h"
#include "include/core/SkTypeface.h"
#include "include/ports/SkFontMgr_empty.h"
#include "layer_manager.h"
#include "scene.h"
#include "shape_factory.h"
#include "pipeline.h"
#include "coordinates.h"

using namespace le;
using namespace le::database;

#if defined(__APPLE__)
#include "include/ports/SkFontMgr_mac_ct.h"
#elif defined(__linux__)
#include "include/ports/SkFontMgr_fontconfig.h"
#endif
#include <algorithm>

sk_sp<SkFontMgr> GetSharedFontManager()
{
#if defined(__APPLE__)
    // CoreText is guaranteed to be compiled into Skia's Apple binaries
    return ::SkFontMgr_New_CoreText(nullptr);
#elif defined(__linux__)
    // Fontconfig is standard for Linux Skia distributions
    return ::SkFontMgr_New_Fontconfig(nullptr);
#else
    return nullptr;
#endif
}

namespace le
{
    class LayoutEngine
    {
    public:
        LayoutEngine()
        {
            db = std::make_shared<DatabaseT>();
            coordinates = std::make_shared<Coordinates>();
            scene = std::make_shared<Scene>();
        }

        DatabaseT *get_db()
        {
            return db.get();
        }

        void generate_test_data()
        {
            // Create a fake database for testing
            auto id = 0;

            auto library = std::make_unique<LibraryT>();
            library->name = "test_library";
            library->id = id++;

            auto design = std::make_unique<DesignT>();
            design->name = "my_block";
            design->library_id = library->id;
            design->id = id++;

            auto view = std::make_unique<ViewT>();
            view->name = "layout";
            view->type = ViewType_Layout;
            view->design_id = design->id;
            view->id = id++;

            db->libraries.push_back(std::move(library));
            db->designs.push_back(std::move(design));
            db->views.push_back(std::move(view));

            // Setup layer manager
            layer_manager = std::make_shared<LayerManager>();
            layer_manager->add_layer("prBoundary", "#cc3333cc", 5, "#cc333355", 0);
            layer_manager->add_layer("M1", "#3333cccc", 1, "#3333cc55", 1);
            layer_manager->add_layer("M2", "#33cc33cc", 1, "#33cc3355", 2);

            pipeline = std::make_unique<Pipeline>(db, layer_manager, coordinates, scene);
        }

        void zoom(float by)
        {
            float new_zoom = coordinates->get_pixels_per_um() * by;

            // 1. Calculate the exact center point of the canvas in pixels
            float centerX_pixels = static_cast<float>(canvas_width) / 2.0f;
            float centerY_pixels = static_cast<float>(canvas_height) / 2.0f;

            // 2. Figure out what DBU coordinate is currently sitting at that center pixel location.
            // (We use our existing panning values plus the pixel-to-dbu conversion of the center offset)
            float center_dbu_x = coordinates->get_pan_x_in_dbu() + coordinates->pixels_2_dbu(centerX_pixels);
            float center_dbu_y = coordinates->get_pan_y_in_dbu() + coordinates->pixels_2_dbu(centerY_pixels);

            // 3. Apply the new scale factor to your system
            coordinates->set_pixels_per_um(new_zoom);

            // 4. Recalculate what the center pixel offset represents under the NEW scale factor
            float new_center_offset_dbu_x = coordinates->pixels_2_dbu(centerX_pixels);
            float new_center_offset_dbu_y = coordinates->pixels_2_dbu(centerY_pixels);

            // 5. Adjust the panning offsets so the targeted DBU point stays centered on screen
            float new_pan_x = center_dbu_x - new_center_offset_dbu_x;
            float new_pan_y = center_dbu_y - new_center_offset_dbu_y;

            coordinates->set_pan_in_dbu(new_pan_x, new_pan_y);
        }

        void zoom_fit()
        {
            auto bbox = scene->bbox();
            int bbox_width = bbox->ur->x - bbox->ll->x;
            int bbox_height = bbox->ur->y - bbox->ll->y;

            // 1. Calculate the target scale factor
            float scale = std::min(static_cast<float>(canvas_width) / bbox_width,
                                   static_cast<float>(canvas_height) / bbox_height);

            // 2. Calculate dbu window sizes using the NEW scale value immediately
            float canvas_width_in_dbu = static_cast<float>(canvas_width) / scale;
            float canvas_height_in_dbu = static_cast<float>(canvas_height) / scale;

            // 3. Compute the centering offsets accurately
            float xoff = 0.5f * (canvas_width_in_dbu - bbox_width);
            float yoff = 0.5f * (canvas_height_in_dbu - bbox_height);

            // 4. Update your coordinates state all at once
            coordinates->set_pixels_per_dbu(scale);

            // Note: Assuming you have or will change set_pan_in_pixels to set_pan_in_dbu
            // to map directly to your underlying dbu properties
            coordinates->set_pan_in_dbu(bbox->ll->x - xoff, bbox->ll->y - yoff);
        }

        void pan(float by_x_pixels, float by_y_pixels)
        {
            coordinates->set_pan_in_pixels(
                coordinates->get_pan_x_in_pixels() + by_x_pixels,
                coordinates->get_pan_y_in_pixels() + by_y_pixels);
        }

        // Renders the current frame directly into a macOS CoreVideo Pixel Buffer
        void render_frame(CVPixelBufferRef pixelBuffer, int width, int height)
        {
            canvas_width = width;
            canvas_height = height;

            // Calculate new viewport based on window width and height
            viewport = coordinates->get_viewport_in_dbu(width, height);

            // Update pipeline with new viewport to enable shape culling
            pipeline->set_viewport(viewport);

            // Auto-fit if first frame
            if (first_frame && pipeline->did_run())
            {
                zoom_fit();
                first_frame = false;
            }

            // Map Flutter's shared macOS pixel memory directly into a Skia Surface
            SkImageInfo info = SkImageInfo::Make(width,
                                                 height,
                                                 kBGRA_8888_SkColorType,
                                                 kPremul_SkAlphaType,
                                                 SkColorSpace::MakeSRGB());
            // sk_sp<SkSurface> surface = SkSurfaces::WrapPixels(info, baseAddress, bytesPerRow);
            sk_sp<SkSurface> surface = SkSurfaces::Raster(info);
            SkCanvas *canvas = surface->getCanvas();

            // Save current state before transformations
            canvas->save();

            // Flip y-coords
            canvas->translate(0.0f, static_cast<float>(height));
            canvas->scale(1.0f, -1.0f);

            // Scale and translate
            canvas->scale(coordinates->get_pixels_per_dbu(), coordinates->get_pixels_per_dbu());
            canvas->translate(-1 * coordinates->get_pan_x_in_dbu(), -1 * coordinates->get_pan_y_in_dbu());

            // clear previous frame
            canvas->clear(SK_ColorBLACK);

            // draw shapes from pipeline
            std::chrono::steady_clock::time_point begin = std::chrono::steady_clock::now();

            for (auto &[layer_name, shapes] : pipeline->run())
            {
                const auto &layer = layer_manager->get_layer(layer_name);
                auto fill = layer_manager->get_fill_paint_for_layer(layer_name);
                auto stroke = layer_manager->get_stroke_paint_for_layer(layer_name);

                for (auto &shape : shapes)
                {
                    for (auto &rect : shape.get().rects)
                    {
                        auto sk_rect = SkRect::MakeLTRB(rect->ll->x, rect->ll->y, rect->ur->x, rect->ur->y);
                        canvas->drawRect(sk_rect, fill);
                        canvas->drawRect(sk_rect, stroke);
                    }
                }
            }

            // Restore original translations
            canvas->restore();

            std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now();
            // std::cout << "Render speed = " << std::chrono::duration_cast<std::chrono::milliseconds>(end - begin).count() << "[ms]" << std::endl;

            // Send to flutter
            CVPixelBufferLockBaseAddress(pixelBuffer, 0);
            void *baseAddress = CVPixelBufferGetBaseAddress(pixelBuffer);
            size_t bytesPerRow = CVPixelBufferGetBytesPerRow(pixelBuffer);
            surface->readPixels(info, baseAddress, bytesPerRow, 0, 0);

            // Clean up locks
            CVPixelBufferUnlockBaseAddress(pixelBuffer, 0);
        }

    private:
        std::shared_ptr<DatabaseT> db;
        std::shared_ptr<LayerManager> layer_manager;
        std::shared_ptr<Coordinates> coordinates;
        std::shared_ptr<Scene> scene;
        std::shared_ptr<database::RectT> viewport;
        std::unique_ptr<Pipeline> pipeline;
        // Record Canvas width and height for zoom_fit calculation
        int canvas_width;
        int canvas_height;
        bool first_frame = true;
    };
}

static std::vector<uint8_t> browser_data_cache;

extern "C"
{
    // Initializes a new layout engine
    EXPORT void *init_layout_engine()
    {
        return new LayoutEngine();
    }

    // Generate test data
    EXPORT void generate_test_data(void *layout_engine_ptr)
    {
        LayoutEngine *layout_engine = static_cast<LayoutEngine *>(layout_engine_ptr);
        layout_engine->generate_test_data();
    }

    EXPORT void zoom(void *layout_engine_ptr, float zoom)
    {
        LayoutEngine *layout_engine = static_cast<LayoutEngine *>(layout_engine_ptr);
        layout_engine->zoom(zoom);
    }

    EXPORT void zoom_fit(void *layout_engine_ptr)
    {
        LayoutEngine *layout_engine = static_cast<LayoutEngine *>(layout_engine_ptr);
        layout_engine->zoom_fit();
    }

    EXPORT void pan(void *layout_engine_ptr, float x_um, float y_um)
    {
        LayoutEngine *layout_engine = static_cast<LayoutEngine *>(layout_engine_ptr);
        layout_engine->pan(x_um, y_um);
    }

    EXPORT void serialize_library_browser_data(void *layout_engine_ptr)
    {
        LayoutEngine *layout_engine = static_cast<LayoutEngine *>(layout_engine_ptr);

        // 1. Clear previous cache run
        browser_data_cache.clear();

        // 2. Fetch our mutable data source
        // Assumes you added: DatabaseT* get_db() { return current_database.get(); } to LayoutEngine
        auto *db = layout_engine->get_db();
        if (!db)
            return;

        flatbuffers::FlatBufferBuilder builder(4096);
        std::vector<flatbuffers::Offset<le::database::LibraryInfo>> library_offsets;

        // --- LOOP 1: Iterate through all Libraries ---
        for (const auto &lib : db->libraries)
        {
            std::vector<flatbuffers::Offset<le::database::DesignInfo>> design_offsets;

            // --- LOOP 2: Find all Designs belonging to this Library ---
            for (const auto &design : db->designs)
            {
                if (design->library_id != lib->id)
                    continue; // Foreign key check

                std::vector<flatbuffers::Offset<le::database::ViewInfo>> view_offsets;

                // --- LOOP 3: Find all Views belonging to this Design ---
                for (const auto &view : db->views)
                {
                    if (view->design_id != design->id)
                        continue; // Foreign key check

                    // Serialize the leaf: ViewInfo
                    auto v_name = builder.CreateString(view->name);
                    le::database::ViewInfoBuilder view_builder(builder);
                    view_builder.add_id(view->id);
                    view_builder.add_name(v_name);
                    view_offsets.push_back(view_builder.Finish());
                }

                // Serialize the branch: DesignInfo (attaching the views array)
                auto v_vector = builder.CreateVector(view_offsets);
                auto d_name = builder.CreateString(design->name);
                le::database::DesignInfoBuilder design_builder(builder);
                design_builder.add_id(design->id);
                design_builder.add_name(d_name);
                design_builder.add_views(v_vector);
                design_offsets.push_back(design_builder.Finish());
            }

            // Serialize the root: LibraryInfo (attaching the designs array)
            auto d_vector = builder.CreateVector(design_offsets);
            auto l_name = builder.CreateString(lib->name);
            le::database::LibraryInfoBuilder lib_builder(builder);
            lib_builder.add_id(lib->id);
            lib_builder.add_name(l_name);
            lib_builder.add_designs(d_vector);
            library_offsets.push_back(lib_builder.Finish());
        }

        // 3. Wrap everything into a single root BrowserPayload container
        // Make sure to add 'table BrowserPayload { libraries: [LibraryInfo]; } root_type BrowserPayload;' to your .fbs
        auto libs_vector = builder.CreateVector(library_offsets);
        auto payload_offset = le::database::CreateBrowserPayload(builder, libs_vector);
        builder.Finish(payload_offset);

        // 4. Extract and cache raw byte buffer for Dart cross-boundary operations
        browser_data_cache.assign(builder.GetBufferPointer(), builder.GetBufferPointer() + builder.GetSize());
    }

    // Expose data memory address to Dart FFI
    EXPORT const uint8_t *get_browser_data_ptr()
    {
        return browser_data_cache.data();
    }

    // Expose data size boundaries to Dart FFI
    EXPORT uint32_t get_browser_data_size()
    {
        return static_cast<uint32_t>(browser_data_cache.size());
    }

    // Executes a render loop pass using the opaque pointer
    EXPORT void render_pixel_buffer(void *layout_engine_ptr, CVPixelBufferRef pixel_buffer)
    {
        if (!layout_engine_ptr || !pixel_buffer)
            return;
        auto *layout_engine = static_cast<LayoutEngine *>(layout_engine_ptr);
        CVPixelBufferLockBaseAddress(pixel_buffer, 0);
        void *baseAddress = CVPixelBufferGetBaseAddress(pixel_buffer);
        size_t bytesPerRow = CVPixelBufferGetBytesPerRow(pixel_buffer);
        int width = (int)CVPixelBufferGetWidth(pixel_buffer);
        int height = (int)CVPixelBufferGetHeight(pixel_buffer);

        // 2. Forward the properties directly into your shared C++ module
        layout_engine->render_frame(pixel_buffer, width, height);

        // 3. Release the hardware memory lock
        CVPixelBufferUnlockBaseAddress(pixel_buffer, 0); //
    }

    // Cleans up memory allocations to prevent leaks
    EXPORT void destroy(void *layout_engine_ptr)
    {
        if (!layout_engine_ptr)
            return;
        delete static_cast<LayoutEngine *>(layout_engine_ptr);
    }
}
