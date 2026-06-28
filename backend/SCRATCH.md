How do draw text
----------------

sk_sp<SkFontMgr> fontManager = GetSharedFontManager();
if (!fontManager)
{
    return;
}

sk_sp<SkTypeface> typeface = fontManager->makeFromFile("/Users/john/Projects/synthosilicon/layout_engine/backend/fonts/RobotoMono-Medium.ttf", 0);
if (!typeface)
    return;

// 2. Pair the typeface with a font size (e.g., 24 points)
SkFont font(typeface, 24.0f);
font.setSubpixel(false); // Optional: Enable subpixel anti-aliasing

// 3. Configure the paint (styling details)
SkPaint paint;
paint.setColor({0, 1, 0, alpha});
paint.setAntiAlias(false);

// 4. Draw the text to the canvas at coordinates (X, Y)
// The Y coordinate targets the baseline of the text
std::stringstream ss;
ss << "LAYOUT ENGINE " << "w: " << width << ", h: " << height;
std::string text = ss.str();

canvas->save();

// 1. Move to the text position in your world
canvas->translate(x, y);

// 2. Un-flip the Y-axis so text reads top-to-bottom
canvas->scale(1.0f, -1.0f);

canvas->drawString(text.c_str(), 10.0f, height - 22.f, font, paint);

canvas->restore();

IMPORTANT: do not allocate font and paint during render_frame - very slow.
