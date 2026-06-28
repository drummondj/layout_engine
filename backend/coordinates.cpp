
#include "coordinates.h"

namespace le
{
    float Coordinates::pixels_2_dbu(int pixels)
    {
        return pixels / pixels_per_dbu;
    }

    float Coordinates::dbu_2_pixels(int dbu)
    {
        return dbu * pixels_per_dbu;
    }

    float Coordinates::um_2_pixels(float um)
    {
        return um * pixels_per_um;
    }

    void Coordinates::set_pixels_per_dbu(float value)
    {
        pixels_per_dbu = value;
        pixels_per_um = dbu_per_um * pixels_per_dbu;
    }

    void Coordinates::set_pixels_per_um(float value)
    {
        pixels_per_um = value;
        pixels_per_dbu = pixels_per_um / dbu_per_um;
    }

    void Coordinates::set_dbu_per_um(float value)
    {
        dbu_per_um = value;
        pixels_per_dbu = pixels_per_um / dbu_per_um;
    }

    void Coordinates::set_pan_in_pixels(float x, float y)
    {
        pan_x_dbu = pixels_2_dbu(x);
        pan_y_dbu = pixels_2_dbu(y);
    }

    void Coordinates::set_pan_in_dbu(float x, float y)
    {
        pan_x_dbu = x;
        pan_y_dbu = y;
    }

    float Coordinates::get_pan_x_in_pixels()
    {
        return dbu_2_pixels(pan_x_dbu);
    }

    float Coordinates::get_pan_y_in_pixels()
    {
        return dbu_2_pixels(pan_y_dbu);
    }

    float Coordinates::get_pan_x_in_dbu()
    {
        return pan_x_dbu;
    }

    float Coordinates::get_pan_y_in_dbu()
    {
        return pan_y_dbu;
    }

    float Coordinates::get_pixels_per_dbu()
    {
        return pixels_per_dbu;
    }

    float Coordinates::get_pixels_per_um()
    {
        return pixels_per_um;
    }

    std::unique_ptr<database::RectT> Coordinates::get_viewport_in_dbu(int width_in_pixels, int height_in_pixels)
    {
        auto viewport = std::make_unique<database::RectT>();
        viewport->ll = std::make_unique<database::PointT>();
        viewport->ur = std::make_unique<database::PointT>();

        auto width_in_dbu = pixels_2_dbu(width_in_pixels);
        auto height_in_dbu = pixels_2_dbu(height_in_pixels);
        viewport->ll->x = pan_x_dbu;
        viewport->ll->y = pan_y_dbu;
        viewport->ur->x = pan_x_dbu + width_in_dbu;
        viewport->ur->y = pan_y_dbu + height_in_dbu;

        return viewport;
    }
}