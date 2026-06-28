
#pragma once
#include "database/database_generated.h"

namespace le
{
    class Coordinates
    {
    public:
        float pixels_2_dbu(int pixels);

        float dbu_2_pixels(int dbu);

        float um_2_pixels(float um);

        void set_pixels_per_um(float value);

        void set_pixels_per_dbu(float value);

        void set_dbu_per_um(float value);

        void set_pan_in_pixels(float x, float y);

        void set_pan_in_dbu(float x, float y);

        float get_pan_x_in_pixels();

        float get_pan_y_in_pixels();

        float get_pan_x_in_dbu();

        float get_pan_y_in_dbu();

        float get_pixels_per_dbu();

        float get_pixels_per_um();

        std::unique_ptr<database::RectT> get_viewport_in_dbu(int width_in_pixels, int height_in_pixels);

    private:
        float pixels_per_um = 1.0;
        int dbu_per_um = 1000;
        float pixels_per_dbu = 0.001;
        float pan_x_dbu = 0.0;
        float pan_y_dbu = 0.0;
    };
}