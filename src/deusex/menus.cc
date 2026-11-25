#include "core/log.h"
#include "core/objects.h"
#include "core/settings.h"
#include "core/vm.h"
#include "deusex/deusex.h"
#include "deusex/references.h"
#include "deusex/script.h"
#include "deusex/ui.h"
#include "core/gc.h"
#include "engine/resources.h"
#include "extension/extension.h"
#include "extension/windows.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace dx::deusex {
    MenuPatch::MenuPatch(std::string name)
        : what(std::move(name))
    {
    }

    UClass* MenuPatch::findClass(std::string_view path)
    {
        UClass* found = objects::findClass(path);
        if (!found) {
            miss(std::string(path));
        }
        return found;
    }

    UObject* MenuPatch::defaults(UClass* cls)
    {
        UObject* object = cls ? cls->defaultObject : nullptr;
        if (cls && !object) {
            miss(cls->name.str() + " defaults");
        }
        return object;
    }

    UProperty* MenuPatch::property(UStruct* type, const char* name)
    {
        UProperty* found = type ? type->findProperty(Name(name)) : nullptr;
        if (!found) {
            miss(name);
        }
        return found;
    }

    void MenuPatch::miss(const std::string& name)
    {
        if (missing.empty()) {
            missing = name;
        }
    }

    bool MenuPatch::ready()
    {
        if (missing.empty()) {
            return true;
        }
        if (!logged) {
            logged = true;
            log::warn("DevUI", "skipping {}: {} not found", what, missing);
        }
        return false;
    }

    // The original picture: the menu's tiles side by side, textureCols across, in RGBA with
    // what is see-through at alpha 0, down to its last drawn row and across to the end of its
    // first row of tiles.
    struct MenuArt::Picture {
        std::vector<UTexture*> tiles;
        int cols = 0;
        int stride = 0;
        int width = 0;
        int height = 0;
        std::vector<uint8_t> pixels;

        const uint8_t* at(int x, int y) const
        {
            return &pixels[(static_cast<size_t>(y) * static_cast<size_t>(stride) + static_cast<size_t>(x)) * 4];
        }
    };

    MenuArt::MenuArt(UObject* screen, std::string name)
        : menu(screen)
        , what(std::move(name))
    {
        MenuPatch patch(what);
        int32_t* colsField = patch.field<int32_t>(menu, "textureCols");
        int32_t* rowsField = patch.field<int32_t>(menu, "textureRows");
        UProperty* slots = patch.property(menu ? menu->cls : nullptr, "clientTextures");
        if (!patch.ready()) {
            return;
        }
        auto art = std::make_unique<Picture>();
        art->cols = *colsField;
        int tileRows = *rowsField;
        if (art->cols <= 0 || tileRows <= 0 || art->cols * tileRows > slots->arrayDim) {
            log::warn("DevUI", "skipping {}: {}x{} tiles", what, art->cols, tileRows);
            return;
        }
        int height = 0;
        for (int i = 0; i < art->cols * tileRows; i++) {
            UObject* tile = *patch.field<UObject*>(menu, "clientTextures", i);
            objects::ensureLoaded(tile);
            art->tiles.push_back(cast<UTexture>(tile));
            UTexture* texture = art->tiles.back();
            if (!texture || texture->mips.empty() || texture->width() > 256 || texture->height() > 256) {
                log::warn("DevUI", "skipping {}: tile {} has no texture", what, i);
                return;
            }
            height = std::max(height, (i / art->cols) * 256 + static_cast<int>(texture->height()));
            if (i < art->cols) {
                art->width += static_cast<int>(texture->width());
            }
        }
        art->stride = art->cols * 256;
        art->pixels.assign(static_cast<size_t>(art->stride * height) * 4, 0);
        int used = 0;
        for (int i = 0; i < art->cols * tileRows; i++) {
            UTexture* tile = art->tiles[static_cast<size_t>(i)];
            std::vector<uint8_t> rgba = tile->rgba(0);
            // A palette texture is see-through at index 0; one made here is RGBA8 and keeps that
            // in its alpha.
            bool palette = tile->format() == 0;
            int tileWidth = static_cast<int>(tile->width());
            for (int y = 0; y < static_cast<int>(tile->height()); y++) {
                for (int x = 0; x < tileWidth; x++) {
                    size_t from = static_cast<size_t>(y * tileWidth + x);
                    int toX = (i % art->cols) * 256 + x;
                    int toY = (i / art->cols) * 256 + y;
                    uint8_t* to = &art->pixels[(static_cast<size_t>(toY) * static_cast<size_t>(art->stride) + static_cast<size_t>(toX)) * 4];
                    if (palette ? tile->mips[0].data[from] != 0 : rgba[from * 4 + 3] != 0) {
                        to[0] = rgba[from * 4 + 2];
                        to[1] = rgba[from * 4 + 1];
                        to[2] = rgba[from * 4 + 0];
                        to[3] = 255;
                        used = std::max(used, toY + 1);
                    }
                }
            }
        }
        // Rows below the last drawn one are left off, so the result needs no more tiles than
        // the menu has room for.
        art->height = used;
        for (int y = 0; y < art->height; y++) {
            rows.push_back(y);
        }
        picture = std::move(art);
    }

    MenuArt::MenuArt(UObject* screen, std::string name, int width, int height, std::span<const uint8_t> bgra)
        : menu(screen)
        , what(std::move(name))
    {
        MenuPatch patch(what);
        int32_t* colsField = patch.field<int32_t>(menu, "textureCols");
        UObject** firstTile = patch.field<UObject*>(menu, "clientTextures", 0);
        if (!patch.ready()) {
            return;
        }
        // The first tile stays as the one apply takes the package and smoothing from.
        objects::ensureLoaded(*firstTile);
        UTexture* first = cast<UTexture>(*firstTile);
        if (!first || width <= 0 || height <= 0 || bgra.size() != static_cast<size_t>(width) * static_cast<size_t>(height) * 4) {
            log::warn("DevUI", "skipping {}: {}", what, first ? "the picture is not its size" : "the menu has no first tile");
            return;
        }
        auto art = std::make_unique<Picture>();
        art->tiles.push_back(first);
        art->cols = *colsField;
        art->stride = art->cols * 256;
        art->width = width;
        art->height = height;
        if (art->cols <= 0 || art->width > art->stride) {
            log::warn("DevUI", "skipping {}: {} wide, past the menu's {} tiles across", what, art->width, art->cols);
            return;
        }
        art->pixels.assign(static_cast<size_t>(art->stride) * static_cast<size_t>(art->height) * 4, 0);
        for (int y = 0; y < art->height; y++) {
            auto row = bgra.subspan(static_cast<size_t>(y) * static_cast<size_t>(width) * 4, static_cast<size_t>(width) * 4);
            std::copy(row.begin(), row.end(), &art->pixels[static_cast<size_t>(y) * static_cast<size_t>(art->stride) * 4]);
        }
        for (int y = 0; y < art->height; y++) {
            rows.push_back(y);
        }
        picture = std::move(art);
    }

    MenuArt::~MenuArt() = default;

    bool MenuArt::valid() const
    {
        return picture != nullptr;
    }

    int MenuArt::width() const
    {
        return picture ? picture->width : 0;
    }

    int MenuArt::height() const
    {
        return static_cast<int>(rows.size());
    }

    void MenuArt::remove(int from, int to)
    {
        from = std::clamp(from, 0, height());
        to = std::clamp(to, from, height());
        rows.erase(rows.begin() + from, rows.begin() + to);
    }

    void MenuArt::repeat(int at, int from, int to)
    {
        // Neither end may fall outside the art.
        at = std::clamp(at, 0, height());
        from = std::clamp(from, 0, height());
        to = std::clamp(to, from, height());
        std::vector<int> band(rows.begin() + from, rows.begin() + to);
        rows.insert(rows.begin() + at, band.begin(), band.end());
    }

    void MenuArt::cutTo(int newHeight)
    {
        rows.resize(static_cast<size_t>(std::clamp(newHeight, 0, height())));
    }

    bool MenuArt::apply()
    {
        if (!picture) {
            return false;
        }
        // The new tiles go in the package of the art they replace.
        UTexture* first = picture->tiles.front();
        MenuPatch patch(what);
        UProperty* slots = patch.property(menu ? menu->cls : nullptr, "clientTextures");
        UClass* textureClass = patch.findClass("Engine.Texture");
        if (!patch.ready() || width() <= 0 || height() <= 0) {
            return false;
        }
        int cols = picture->cols;
        int tileRows = std::min((height() + 255) / 256, slots->arrayDim / cols);
        if (tileRows * 256 < height()) {
            log::warn("DevUI", "{}: art {} high is cut off at {}, the menu's {} tiles", what, height(), tileRows * 256, slots->arrayDim);
        }
        int drawn = std::min(height(), tileRows * 256);
        for (int i = 0; i < slots->arrayDim; i++) {
            *reinterpret_cast<UObject**>(menu->propertyAddress(Name("clientTextures"), i)) = nullptr;
        }
        for (int row = 0; row < tileRows; row++) {
            for (int col = 0; col < cols; col++) {
                UTexture* texture = cast<UTexture>(objects::newObject(textureClass, first->outer, Name(), RF_Public | RF_Transient));
                *texture->propertyAddress(Name("Format")) = 5;
                *reinterpret_cast<uint32_t*>(texture->propertyAddress(Name("bNoSmooth"))) = *reinterpret_cast<uint32_t*>(first->propertyAddress(Name("bNoSmooth")));
                TextureMip mip;
                mip.width = static_cast<uint32_t>(std::min(256, width() - col * 256));
                mip.height = static_cast<uint32_t>(std::min(256, drawn - row * 256));
                for (uint32_t y = 0; y < mip.height; y++) {
                    int fromY = rows[static_cast<size_t>(row * 256) + y];
                    for (uint32_t x = 0; x < mip.width; x++) {
                        const uint8_t* pixel = picture->at(col * 256 + static_cast<int>(x), fromY);
                        mip.data.insert(mip.data.end(), pixel, pixel + 4);
                    }
                }
                texture->mips.push_back(std::move(mip));
                *reinterpret_cast<UObject**>(menu->propertyAddress(Name("clientTextures"), row * cols + col)) = texture;
            }
        }
        *reinterpret_cast<int32_t*>(menu->propertyAddress(Name("textureRows"))) = tileRows;
        return true;
    }

    void growMenuBackground(UObject* menu, int at, int height)
    {
        MenuPatch patch("growing a menu background");
        int32_t* clientHeight = patch.field<int32_t>(menu, "ClientHeight");
        if (!patch.ready()) {
            return;
        }
        MenuArt art(menu, "growing a menu background");
        art.repeat(at, at - height, at);
        art.apply();
        *clientHeight += height;
    }

    void insertMenuButton(UObject* menu, int index, const std::string& label, UClass* invoke)
    {
        MenuPatch patch("the " + label + " menu button");
        UClass* cls = menu ? menu->cls : nullptr;
        UProperty* buttons = patch.property(cls, "buttonDefaults");
        UStruct* button = buttons ? buttons->structType : nullptr;
        UProperty* y = patch.property(button, "Y");
        UProperty* invokeMember = patch.property(button, "Invoke");
        UProperty* names = cls ? cls->findProperty(Name("ButtonNames")) : nullptr;
        UProperty* text = names ? nullptr : patch.property(button, "Text");
        int32_t* clientHeight = patch.field<int32_t>(menu, "ClientHeight");
        if (!patch.ready()) {
            return;
        }
        constexpr int count = 10;
        if (index < 1 || index >= count || buttons->arrayDim < count || (names && names->arrayDim < count)) {
            log::warn("DevUI", "skipping the {} menu button: no room for it at {}", label, index);
            return;
        }
        auto entry = [&](int i) { return menu->propertyAddress(Name("buttonDefaults"), i); };
        auto name = [&](int i) { return reinterpret_cast<std::string*>(menu->propertyAddress(Name("ButtonNames"), i)); };
        auto top = [&](int i) { return reinterpret_cast<int32_t*>(entry(i) + y->offset); };
        // Inserting at index 1 copies row 0 over row 1, so measure their spacing first.
        const int gap = *top(1) - *top(0);
        for (int i = count - 1; i > index; i--) {
            button->copyValue(entry(i), entry(i - 1));
            if (names) {
                *name(i) = *name(i - 1);
            }
        }
        button->copyValue(entry(index), entry(index - 1));
        if (names) {
            *name(index) = label;
        } else {
            *reinterpret_cast<std::string*>(entry(index) + text->offset) = label;
        }
        *reinterpret_cast<UObject**>(entry(index) + invokeMember->offset) = invoke;
        for (int i = index; i < count; i++) {
            *top(i) += gap;
        }
        int slot = *top(index);
        MenuArt art(menu, "the " + label + " menu button's art");
        art.repeat(slot, slot - gap, slot);
        art.apply();
        *clientHeight += gap;
    }

    void swapMenuButtons(UObject* menu, int a, int b)
    {
        MenuPatch patch("swapping menu buttons");
        UClass* cls = menu ? menu->cls : nullptr;
        UProperty* buttons = patch.property(cls, "buttonDefaults");
        UStruct* button = buttons ? buttons->structType : nullptr;
        UProperty* action = patch.property(button, "action");
        UProperty* invoke = patch.property(button, "invoke");
        UProperty* key = patch.property(button, "key");
        UProperty* names = patch.property(cls, "ButtonNames");
        if (!patch.ready()) {
            return;
        }
        if (a < 0 || b < 0 || a >= buttons->arrayDim || b >= buttons->arrayDim || a >= names->arrayDim || b >= names->arrayDim) {
            log::warn("DevUI", "skipping swapping menu buttons {} and {}: not in the menu", a, b);
            return;
        }
        uint8_t* first = menu->propertyAddress(Name("buttonDefaults"), a);
        uint8_t* second = menu->propertyAddress(Name("buttonDefaults"), b);
        std::swap(first[action->offset], second[action->offset]);
        std::swap(*reinterpret_cast<UObject**>(first + invoke->offset), *reinterpret_cast<UObject**>(second + invoke->offset));
        std::swap(*reinterpret_cast<std::string*>(first + key->offset), *reinterpret_cast<std::string*>(second + key->offset));
        std::swap(*reinterpret_cast<std::string*>(menu->propertyAddress(Name("ButtonNames"), a)), *reinterpret_cast<std::string*>(menu->propertyAddress(Name("ButtonNames"), b)));
    }

    int findMenuButton(UObject* menu, UClass* invoke)
    {
        UProperty* buttons = menu && invoke ? menu->cls->findProperty(Name("buttonDefaults")) : nullptr;
        UProperty* member = buttons && buttons->structType ? buttons->structType->findProperty(Name("invoke")) : nullptr;
        for (int i = 0; member && i < buttons->arrayDim; i++) {
            if (*reinterpret_cast<UObject**>(menu->propertyAddress(Name("buttonDefaults"), i) + member->offset) == invoke) {
                return i;
            }
        }
        return -1;
    }

    void removeMenuButton(UObject* menu, int index)
    {
        MenuPatch patch("removing a menu button");
        UClass* cls = menu ? menu->cls : nullptr;
        UProperty* buttons = patch.property(cls, "buttonDefaults");
        UStruct* button = buttons ? buttons->structType : nullptr;
        UProperty* y = patch.property(button, "Y");
        UProperty* names = patch.property(cls, "ButtonNames");
        int32_t* clientHeight = patch.field<int32_t>(menu, "ClientHeight");
        if (!patch.ready()) {
            return;
        }
        constexpr int count = 10;
        if (index < 0 || index >= count - 1 || buttons->arrayDim < count || names->arrayDim < count) {
            log::warn("DevUI", "skipping removing menu button {}: not in the menu", index);
            return;
        }
        auto entry = [&](int i) { return menu->propertyAddress(Name("buttonDefaults"), i); };
        auto name = [&](int i) { return reinterpret_cast<std::string*>(menu->propertyAddress(Name("ButtonNames"), i)); };
        auto top = [&](int i) { return reinterpret_cast<int32_t*>(entry(i) + y->offset); };
        // The gap to the next button, which the buttons below move up by.
        int slot = *top(index);
        int gap = *top(index + 1) - slot;
        for (int i = index; i < count - 1; i++) {
            button->copyValue(entry(i), entry(i + 1));
            *name(i) = *name(i + 1);
        }
        // A button with no name is not made.
        *name(count - 1) = "";
        for (int i = index; i < count; i++) {
            *top(i) -= gap;
        }
        MenuArt art(menu, "removing a menu button's art");
        art.remove(slot, slot + gap);
        art.apply();
        *clientHeight -= gap;
    }

    void removeOptionRows(UObject* screen, std::vector<int> rows)
    {
        MenuPatch patch("removing options rows");
        UClass* cls = screen ? screen->cls : nullptr;
        UProperty* choices = patch.property(cls, "choices");
        int32_t* startY = patch.field<int32_t>(screen, "choiceStartY");
        int32_t* gap = patch.field<int32_t>(screen, "choiceVerticalGap");
        int32_t* helpY = patch.field<int32_t>(screen, "helpPosY");
        int32_t* clientHeight = patch.field<int32_t>(screen, "ClientHeight");
        if (!patch.ready()) {
            return;
        }
        int count = choices->arrayDim;
        auto choice = [&](int i) { return reinterpret_cast<UObject**>(screen->propertyAddress(Name("choices"), i)); };
        // From the bottom up, so each row's place in the art is still where it was.
        std::sort(rows.begin(), rows.end(), std::greater<>());
        rows.erase(std::unique(rows.begin(), rows.end()), rows.end());
        MenuArt art(screen, "removing options rows' art");
        for (int row : rows) {
            if (row < 0 || row >= count || !*choice(row)) {
                log::warn("DevUI", "skipping removing options row {}: not in the screen", row);
                continue;
            }
            for (int i = row; i < count - 1; i++) {
                *choice(i) = *choice(i + 1);
            }
            *choice(count - 1) = nullptr;
            int y = *startY + row * *gap;
            art.remove(y, y + *gap);
            *clientHeight -= *gap;
            *helpY -= *gap;
        }
        art.apply();
    }

    void removeIgnoredChoices(UObject* screen)
    {
        UProperty* choices = screen ? screen->cls->findProperty(Name("choices")) : nullptr;
        if (!choices) {
            return;
        }
        std::vector<int> rows;
        for (int i = 0; i < choices->arrayDim; i++) {
            UClass* choice = cast<UClass>(*reinterpret_cast<UObject**>(screen->propertyAddress(Name("choices"), i)));
            const uint8_t* address = choice && choice->defaultObject ? choice->defaultObject->propertyAddress(Name("configSetting")) : nullptr;
            UProperty* property = address ? choice->findProperty(Name("configSetting")) : nullptr;
            if (!property || (property->type != PropType::Str && property->type != PropType::String)) {
                continue;
            }
            const settings::Setting* setting = settings::forConfigSetting(*reinterpret_cast<const std::string*>(address));
            if (setting && setting->support == settings::Support::Ignored) {
                rows.push_back(i);
            }
        }
        if (!rows.empty()) {
            removeOptionRows(screen, rows);
        }
    }

    void removeIgnoredChoices()
    {
        MenuPatch patch("removing the choices of settings Ambrosia ignores");
        UClass* window = patch.findClass("DeusEx.MenuUIScreenWindow");
        if (!patch.ready()) {
            return;
        }
        for (UObject* object : objects::all()) {
            UClass* screen = cast<UClass>(object);
            if (screen && screen->defaultObject && screen->isChildOf(window)) {
                removeIgnoredChoices(screen->defaultObject);
            }
        }
    }

    namespace {
        enum class FillAcross { RowsToRight,
            RowsToLeft,
            Columns };

        // The persona buttons draw additively, so every pixel of their textures is set: the
        // border line is bright, the inside of the button darker and the corners cut off by the
        // bevels darker still. The outline is traced from the bright border pixels alone.
        bool borderPixel(const std::vector<uint8_t>& rgba, size_t index)
        {
            return std::max({ rgba[index * 4], rgba[index * 4 + 1], rgba[index * 4 + 2] }) >= 128;
        }

        // A solid version of a button border texture: each row (or column) is filled out to
        // the border pixels, so it matches the border's shape, bevels included.
        UTexture* filledShape(UTexture* border, FillAcross mode)
        {
            // Keeps the shapes, which nothing else holds, but not the borders they came from.
            static CachedObjects<std::map<std::pair<UTexture*, FillAcross>, UTexture*>> shapes;
            auto& cache = shapes.entries;
            auto found = cache.find({ border, mode });
            if (found != cache.end()) {
                return found->second;
            }
            objects::ensureLoaded(border);
            UTexture* shape = nullptr;
            if (border && !border->mips.empty()) {
                int width = static_cast<int>(border->width());
                int height = static_cast<int>(border->height());
                std::vector<uint8_t> rgba = border->rgba(0);
                std::vector<uint8_t> pixels(static_cast<size_t>(width * height) * 4, 0);
                auto set = [&](int x, int y) {
                    std::fill_n(pixels.begin() + static_cast<std::ptrdiff_t>(y * width + x) * 4, 4, uint8_t { 255 });
                };
                bool rows = mode != FillAcross::Columns;
                int lines = rows ? height : width;
                int length = rows ? width : height;
                for (int line = 0; line < lines; line++) {
                    int first = -1;
                    int last = -1;
                    for (int i = 0; i < length; i++) {
                        int x = rows ? i : line;
                        int y = rows ? line : i;
                        if (borderPixel(rgba, static_cast<size_t>(y * width + x))) {
                            first = first < 0 ? i : first;
                            last = i;
                        }
                    }
                    if (first < 0) {
                        continue;
                    }
                    int from = mode == FillAcross::RowsToLeft ? 0 : first;
                    int to = mode == FillAcross::RowsToRight ? length - 1 : last;
                    for (int i = from; i <= to; i++) {
                        set(rows ? i : line, rows ? line : i);
                    }
                }
                shape = ui::newTexture(static_cast<uint32_t>(width), static_cast<uint32_t>(height), std::move(pixels));
            }
            cache[{ border, mode }] = shape;
            return shape;
        }

        struct ButtonPiece {
            UTexture* texture;
            int width;
        };

        // One of the button's texture structs, or nothing when a changed DeusEx.u lays it out
        // differently.
        std::optional<ButtonPiece> piece(UObject* button, const char* textures)
        {
            UProperty* property = button->cls->findProperty(Name(textures));
            UStruct* type = property ? property->structType : nullptr;
            UProperty* tex = type ? type->findProperty(Name("tex")) : nullptr;
            UProperty* width = type ? type->findProperty(Name("width")) : nullptr;
            uint8_t* entry = button->propertyAddress(Name(textures), 0);
            if (!tex || !width || !entry) {
                return std::nullopt;
            }
            UObject* texture = *reinterpret_cast<UObject**>(entry + tex->offset);
            return ButtonPiece { cast<UTexture>(texture), *reinterpret_cast<int32_t*>(entry + width->offset) };
        }
    }

    bool currentTabBackdrop(UObject* window, Color& fill, std::vector<extension::BackdropTile>& tiles)
    {
        // Called for every window drawn, so the class is looked up once it has loaded, and
        // again should it ever be freed.
        static UClass* navButton = nullptr;
        static gc::ReleaseHook navButtonReleased([](const gc::ReleasedSet& released) {
            if (released.count(navButton)) {
                navButton = nullptr;
            }
        });
        if (!navButton) {
            navButton = objects::findClass("DeusEx.PersonaNavButtonWindow");
            if (!navButton) {
                return false;
            }
        }
        auto* button = cast<extension::UWindow>(window);
        // Each persona screen disables the tab that leads to itself.
        if (!button || !button->isA(navButton) || button->sensitive()) {
            return false;
        }
        // Fill the tab's own outline, laid out as PersonaBorderButtonWindow.DrawWindow lays out
        // its left, center and right textures.
        std::optional<ButtonPiece> leftPiece = piece(button, "Left_Textures");
        std::optional<ButtonPiece> centerPiece = piece(button, "Center_Textures");
        std::optional<ButtonPiece> rightPiece = piece(button, "Right_Textures");
        auto* faceColor = script::field<Color>(button, "colButtonFace");
        if (!leftPiece || !centerPiece || !rightPiece || !faceColor) {
            return false;
        }
        const ButtonPiece& left = *leftPiece;
        const ButtonPiece& center = *centerPiece;
        const ButtonPiece& right = *rightPiece;
        float leftOffset = static_cast<float>(script::integer(button, "leftOffset"));
        float top = static_cast<float>(script::integer(button, "buttonVerticalOffset"));
        float height = static_cast<float>(script::integer(button, "buttonHeight"));
        float width = button->width();
        float leftWidth = static_cast<float>(left.width);
        float rightWidth = static_cast<float>(right.width);
        tiles.push_back({ filledShape(left.texture, FillAcross::RowsToRight), leftOffset, top, leftWidth, height });
        tiles.push_back({ filledShape(center.texture, FillAcross::Columns), leftOffset + leftWidth, top, width - leftWidth - rightWidth - leftOffset, height });
        tiles.push_back({ filledShape(right.texture, FillAcross::RowsToLeft), width - rightWidth, top, rightWidth, height });
        std::erase_if(tiles, [](const extension::BackdropTile& tile) { return !tile.texture; });
        Color face = *faceColor;
        auto shade = [](uint8_t channel) { return static_cast<uint8_t>(channel * 4 / 5); };
        fill = { shade(face.r), shade(face.g), shade(face.b), 255 };
        return !tiles.empty();
    }

    namespace {
        UObject* lastChoice = nullptr;
        std::string lastState;

        // Windows are deleted once their level is gone; nothing here may still point at one.
        gc::ReleaseHook choiceListener([](const gc::ReleasedSet& released) {
            if (released.count(lastChoice)) {
                lastChoice = nullptr;
            }
        });
    }

    void syncResolutionChoice(const std::string& windowState, bool locked, int refreshRate)
    {
        UObject* top = extension::topModal();
        if (!script::isA(top, "MenuScreenDisplay")) {
            lastChoice = nullptr;
            return;
        }
        auto* client = cast<extension::UWindow>(script::object(top, "winClient"));
        UObject* choice = nullptr;
        for (auto* child = client ? client->firstChild() : nullptr; child; child = child->nextSibling()) {
            if (script::isA(child, "MenuChoice_Resolution")) {
                choice = child;
            } else if (script::isA(child, "MenuChoice_FrameRateLimit")) {
                int last = 1;
                while (last < static_cast<int>(std::size(settings::frameRates)) && settings::frameRates[last] < refreshRate) {
                    last++;
                }
                // Update on entry and after a move to another monitor.
                auto* label = script::field<std::string>(child, "enumText", last);
                auto* next = script::field<std::string>(child, "enumText", last + 1);
                if (label && (*label != std::to_string(refreshRate) || (next && !next->empty()))) {
                    for (int i = 1; auto* text = script::field<std::string>(child, "enumText", i); i++) {
                        *text = i < last ? std::to_string(settings::frameRates[i]) : i == last ? std::to_string(refreshRate) : "";
                    }
                    vm::Call(child, Name("SetValue")).i(std::clamp(script::integer(child, "currentValue"), 0, last)).run();
                }
            }
        }
        if (!choice) {
            return;
        }
        if (choice != lastChoice || windowState != lastState) {
            // GetScreenResolutions only fills as many entries as it finds, so clear the old
            // ones first or a shorter list keeps the end of the longer one.
            for (int32_t i = 0; std::string* text = script::field<std::string>(choice, "enumText", i); i++) {
                *text = "";
            }
            vm::Call(choice, Name("GetScreenResolutions")).run();
            vm::Call(choice, Name("LoadSetting")).run();
            lastChoice = choice;
            lastState = windowState;
        }
        for (const char* part : { "btnAction", "btnInfo" }) {
            if (auto* button = cast<extension::UWindow>(script::object(choice, part))) {
                button->setSensitivity(!locked);
            }
        }
        cast<extension::UWindow>(choice)->setSensitivity(!locked);
    }

    void syncAimAssistChoice()
    {
        UObject* top = extension::topModal();
        if (!script::isA(top, "MenuScreenGamepad")) {
            return;
        }
        auto* client = cast<extension::UWindow>(script::object(top, "winClient"));
        extension::UWindow* assist = nullptr;
        bool gyro = settings::get(settings::gamepadGyro);
        for (auto* child = client ? client->firstChild() : nullptr; child; child = child->nextSibling()) {
            if (script::isA(child, "MenuChoice_GyroAiming")) {
                gyro = script::integer(child, "currentValue") != 0;
            } else if (script::isA(child, "MenuChoice_AimAssist")) {
                assist = child;
            }
        }
        if (assist) {
            // Change only the caption: the menu still saves the player's chosen value.
            if (auto* info = script::object(assist, "btnInfo")) {
                int index = gyro ? 0 : script::integer(assist, "currentValue");
                auto* label = script::field<std::string>(assist, "enumText", index);
                if (label && script::string(info, "buttonText") != *label) {
                    vm::Call(info, Name("SetButtonText")).s(*label).run();
                }
            }
            assist->setSensitivity(!gyro);
            for (const char* part : { "btnAction", "btnInfo" }) {
                if (auto* button = cast<extension::UWindow>(script::object(assist, part))) {
                    button->setSensitivity(!gyro);
                }
            }
        }
    }

    void syncMixerChoice()
    {
        UObject* top = extension::topModal();
        if (!script::isA(top, "MenuScreenSound")) {
            return;
        }
        auto* client = cast<extension::UWindow>(script::object(top, "winClient"));
        for (auto* child = client ? client->firstChild() : nullptr; child; child = child->nextSibling()) {
            if (!script::isA(child, "MenuChoice_AudioMixer")) {
                continue;
            }
            auto* value = script::field<int32_t>(child, "currentValue");
            auto* raytraced = script::field<std::string>(child, "enumText", 2);
            if (value && raytraced && *value != 2 && !raytraced->empty()) {
                raytraced->clear();
            }
            return;
        }
    }
}
