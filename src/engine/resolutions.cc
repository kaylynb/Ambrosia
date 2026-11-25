#include "engine/resolutions.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

namespace dx::resolutions {
    namespace {
        // Common monitor and window sizes, grouped by aspect ratio.
        constexpr std::array standard {
            // 16:9
            Size { 1280, 720 },
            Size { 1366, 768 },
            Size { 1600, 900 },
            Size { 1920, 1080 },
            Size { 2560, 1440 },
            Size { 3200, 1800 },
            Size { 3840, 2160 },
            Size { 5120, 2880 },
            Size { 7680, 4320 },
            // 16:10
            Size { 1280, 800 },
            Size { 1440, 900 },
            Size { 1680, 1050 },
            Size { 1920, 1200 },
            Size { 2560, 1600 },
            Size { 2880, 1800 },
            Size { 3840, 2400 },
            // 21:9 ultrawide
            Size { 2560, 1080 },
            Size { 3440, 1440 },
            Size { 3840, 1600 },
            Size { 5120, 2160 },
            // 32:9 super ultrawide
            Size { 3840, 1080 },
            Size { 5120, 1440 },
            Size { 7680, 2160 },
            // 3:2
            Size { 1920, 1280 },
            Size { 2160, 1440 },
            Size { 2256, 1504 },
            Size { 2880, 1920 },
            Size { 3000, 2000 },
            // 4:3 and 5:4
            Size { 1280, 960 },
            Size { 1400, 1050 },
            Size { 1600, 1200 },
            Size { 2048, 1536 },
            Size { 1280, 1024 },
        };

        // Heights to fill in from when the display's aspect ratio has no standard sizes.
        constexpr std::array fallbackHeights { 720, 900, 1080, 1440, 1800, 2160 };

        float aspect(Size size)
        {
            return static_cast<float>(size.width) / static_cast<float>(size.height);
        }

        // Within 3%: the ultrawide sizes differ by about 1% from each other, and the nearest
        // two groups (3:2 and 16:10, 4:3 and 5:4) by about 7%.
        bool sameShape(Size a, Size b)
        {
            return a.height > 0 && b.height > 0 && std::abs(aspect(a) / aspect(b) - 1.0f) < 0.03f;
        }

        // Narrowest first, as a menu lists them.
        bool smallerFirst(Size a, Size b)
        {
            return a.width < b.width || (a.width == b.width && a.height < b.height);
        }

        bool fits(Size size, Size room)
        {
            return size.width <= room.width && size.height <= room.height;
        }

        // Standard sizes in native's shape, smallest first.
        std::vector<Size> shapedLike(Size native)
        {
            std::vector<Size> sizes;
            for (Size size : standard) {
                if (size.height >= minimumHeight && sameShape(size, native)) {
                    sizes.push_back(size);
                }
            }
            if (sizes.empty() && native.height > 0) {
                for (int height : fallbackHeights) {
                    int width = static_cast<int>(std::lround(static_cast<float>(height) * aspect(native) / 2.0f)) * 2;
                    sizes.push_back({ width, height });
                }
            }
            std::sort(sizes.begin(), sizes.end(), smallerFirst);
            return sizes;
        }

        // Narrowest first, without repeats, keeping current when there are more
        // than the menu holds by dropping the smallest others.
        std::vector<Size> finish(std::vector<Size> sizes, Size current)
        {
            if (current.width > 0 && current.height > 0) {
                sizes.push_back(current);
            }
            std::sort(sizes.begin(), sizes.end(), smallerFirst);
            sizes.erase(std::unique(sizes.begin(), sizes.end()), sizes.end());
            while (sizes.size() > menuLimit) {
                sizes.erase(sizes.front() == current ? sizes.begin() + 1 : sizes.begin());
            }
            return sizes;
        }
    }

    std::vector<Size> windowed(Size native, Size room, Size current)
    {
        // A window needn't match the display, so the common 16:9 and 16:10 sizes are offered
        // on any display, such as an ultrawide one, as well as sizes of the display's shape.
        std::vector<Size> sizes;
        for (Size shape : { native, Size { 16, 9 }, Size { 16, 10 } }) {
            for (Size size : shapedLike(shape)) {
                if (fits(size, room)) {
                    sizes.push_back(size);
                }
            }
        }
        return finish(std::move(sizes), current);
    }

    std::vector<Size> exclusive(std::span<const Size> modes, Size native, Size current)
    {
        std::vector<Size> sizes { native };
        for (Size mode : modes) {
            if (mode.height >= minimumHeight && sameShape(mode, native)) {
                sizes.push_back(mode);
            }
        }
        return finish(std::move(sizes), current);
    }

    Size defaultWindow(Size native, Size room)
    {
        // The largest of the display's own shape, failing that the largest of any.
        Size best;
        for (Size size : shapedLike(native)) {
            if (fits(size, room) && size.height > best.height) {
                best = size;
            }
        }
        if (best.height == 0) {
            for (Size size : windowed(native, room, {})) {
                best = size.height > best.height ? size : best;
            }
        }
        return best.height == 0 ? room : best;
    }

    std::string format(const std::vector<Size>& sizes)
    {
        std::string text;
        for (Size size : sizes) {
            if (!text.empty()) {
                text += " ";
            }
            text += std::to_string(size.width) + "x" + std::to_string(size.height);
        }
        // MenuChoice_Resolution only reads a size followed by a space or by more sizes, so a
        // lone one would leave the choice blank.
        if (sizes.size() == 1) {
            text += " ";
        }
        return text;
    }
}
