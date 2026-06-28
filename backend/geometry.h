#pragma once
#include "database/database_generated.h"

namespace le
{
    class Geometry
    {
    public:
        static std::unique_ptr<database::RectT> bbox(const database::ShapeT &shape)
        {
            auto bbox = std::make_unique<database::RectT>();
            bbox->ll = std::make_unique<database::PointT>();
            bbox->ur = std::make_unique<database::PointT>();

            bool first = true;

            for (auto &rect : shape.rects)
            {
                if (first)
                {
                    bbox->ll->x = rect->ll->x;
                    bbox->ll->y = rect->ll->y;
                    bbox->ur->x = rect->ur->x;
                    bbox->ur->y = rect->ur->y;
                    first = false;
                }
                else
                {
                    if (rect->ll->x < bbox->ll->x)
                    {
                        bbox->ll->x = rect->ll->x;
                    }
                    if (rect->ll->y < bbox->ll->y)
                    {
                        bbox->ll->y = rect->ll->y;
                    }
                    if (rect->ur->x > bbox->ur->x)
                    {
                        bbox->ur->x = rect->ur->x;
                    }
                    if (rect->ur->y > bbox->ur->y)
                    {
                        bbox->ur->y = rect->ur->y;
                    }
                }
            }
            // TODO: other shape types

            return bbox;
        }

        static std::unique_ptr<database::RectT> bbox(const std::vector<database::ShapeT *> &shapes)
        {
            auto bbox = std::make_unique<database::RectT>();
            bbox->ll = std::make_unique<database::PointT>();
            bbox->ur = std::make_unique<database::PointT>();

            bool first = true;

            for (auto &shape : shapes)
            {
                for (auto &rect : shape->rects)
                {
                    if (first)
                    {
                        bbox->ll->x = rect->ll->x;
                        bbox->ll->y = rect->ll->y;
                        bbox->ur->x = rect->ur->x;
                        bbox->ur->y = rect->ur->y;
                        first = false;
                    }
                    else
                    {
                        if (rect->ll->x < bbox->ll->x)
                        {
                            bbox->ll->x = rect->ll->x;
                        }
                        if (rect->ll->y < bbox->ll->y)
                        {
                            bbox->ll->y = rect->ll->y;
                        }
                        if (rect->ur->x > bbox->ur->x)
                        {
                            bbox->ur->x = rect->ur->x;
                        }
                        if (rect->ur->y > bbox->ur->y)
                        {
                            bbox->ur->y = rect->ur->y;
                        }
                    }
                }
            }
            // TODO: other shape types

            return bbox;
        }

        static bool rects_overlap(const database::RectT &a, const database::RectT &b)
        {
            return (
                a.ll->x < b.ur->x &&
                a.ur->x > b.ll->x &&
                a.ll->y < b.ur->y &&
                a.ur->y > b.ll->y);
        }
    };
}