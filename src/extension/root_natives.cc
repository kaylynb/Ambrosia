#include "extension/windows.h"

#include "core/config.h"
#include "core/log.h"
#include "core/objects.h"
#include "core/settings.h"
#include "engine/resources.h"
#include "extension/extension.h"
#include "extension/window_internal.h"
#include "extension/window_natives.h"

#include <algorithm>
#include <utility>

namespace dx::extension {
    namespace {
        URootWindow* rootSelf(UObject* object)
        {
            return cast<URootWindow>(object);
        }

        void nativeRootOptionalBool(UObject* object, Frame& stack, uint32_t& (*field)(UObject*))
        {
            bool value = optionalBoolDefault(stack, true);
            vm::finish(stack);
            if (URootWindow* rootWindow = rootSelf(object)) {
                field(rootWindow) = value ? 1 : 0;
            }
        }
    }

    void nativeEnablePositionalSound(UObject* object, Frame& stack, void*)
    {
        nativeRootOptionalBool(object, stack, rootwindow::bPositionalSound);
    }

    void nativeEnableRendering(UObject* object, Frame& stack, void*)
    {
        nativeRootOptionalBool(object, stack, rootwindow::bRender);
    }

    void nativeShowCursor(UObject* object, Frame& stack, void*)
    {
        nativeRootOptionalBool(object, stack, rootwindow::bCursorVisible);
    }

    void nativeStretchRawBackground(UObject* object, Frame& stack, void*)
    {
        nativeRootOptionalBool(object, stack, rootwindow::bStretchRawBackground);
    }

    void nativeIsPositionalSoundEnabled(UObject* object, Frame& stack, void* result)
    {
        vm::finish(stack);
        URootWindow* rootWindow = rootSelf(object);
        vm::retBool(result, rootWindow && rootwindow::bPositionalSound(rootWindow));
    }

    void nativeIsRenderingEnabled(UObject* object, Frame& stack, void* result)
    {
        vm::finish(stack);
        URootWindow* rootWindow = rootSelf(object);
        vm::retBool(result, rootWindow && rootwindow::bRender(rootWindow));
    }

    void nativeLockMouse(UObject* object, Frame& stack, void*)
    {
        bool lockMove = optionalBoolDefault(stack, true);
        bool lockButton = optionalBoolDefault(stack, true);
        vm::finish(stack);
        if (URootWindow* rootWindow = rootSelf(object)) {
            rootwindow::bMouseMoveLocked(rootWindow) = lockMove ? 1 : 0;
            rootwindow::bMouseButtonLocked(rootWindow) = lockButton ? 1 : 0;
        }
    }

    void nativeSetDefaultEditCursor(UObject* object, Frame& stack, void*)
    {
        UObject* cursor = nullptr;
        bool has = optional(stack, cursor);
        vm::finish(stack);
        if (URootWindow* rootWindow = rootSelf(object)) {
            if (has) {
                rootwindow::defaultEditCursor(rootWindow) = cursor;
            }
        }
    }

    void nativeSetDefaultMovementCursors(UObject* object, Frame& stack, void*)
    {
        UObject* values[5] = {};
        bool present[5] = {};
        for (int i = 0; i < 5; i++) {
            present[i] = optional(stack, values[i]);
        }
        vm::finish(stack);
        URootWindow* rootWindow = rootSelf(object);
        if (!rootWindow) {
            return;
        }
        UObject*& (*fields[5])(UObject*) = { rootwindow::DefaultMoveCursor, rootwindow::defaultHorizontalMoveCursor, rootwindow::defaultVerticalMoveCursor, rootwindow::defaultTopLeftMoveCursor, rootwindow::defaultTopRightMoveCursor };
        for (int i = 0; i < 5; i++) {
            if (present[i]) {
                fields[i](rootWindow) = values[i];
            }
        }
    }

    void nativeSetRawBackground(UObject* object, Frame& stack, void*)
    {
        UObject* texture = nullptr;
        Color color {};
        bool hasTexture = optional(stack, texture);
        bool hasColor = optional(stack, color);
        vm::finish(stack);
        if (URootWindow* rootWindow = rootSelf(object)) {
            if (hasTexture) {
                rootwindow::rawBackground(rootWindow) = texture;
            }
            if (hasColor) {
                rootwindow::rawColor(rootWindow) = color;
            }
        }
    }

    void nativeSetRawBackgroundSize(UObject* object, Frame& stack, void*)
    {
        float w = vm::get<float>(stack);
        float h = vm::get<float>(stack);
        vm::finish(stack);
        if (URootWindow* rootWindow = rootSelf(object)) {
            rootwindow::rawBackgroundWidth(rootWindow) = w;
            rootwindow::rawBackgroundHeight(rootWindow) = h;
        }
    }

    void nativeSetRenderViewport(UObject* object, Frame& stack, void*)
    {
        float newX = vm::get<float>(stack);
        float newY = vm::get<float>(stack);
        float newWidth = vm::get<float>(stack);
        float newHeight = vm::get<float>(stack);
        vm::finish(stack);
        if (URootWindow* rootWindow = rootSelf(object)) {
            rootwindow::renderX(rootWindow) = newX;
            rootwindow::renderY(rootWindow) = newY;
            rootwindow::renderWidth(rootWindow) = newWidth;
            rootwindow::renderHeight(rootWindow) = newHeight;
            rootWindow->renderViewportSet = true;
        }
    }

    void nativeResetRenderViewport(UObject* object, Frame& stack, void*)
    {
        vm::finish(stack);
        if (URootWindow* rootWindow = rootSelf(object)) {
            rootWindow->renderViewportSet = false;
        }
    }

    void nativeSetSnapshotSize(UObject* object, Frame& stack, void*)
    {
        float w = vm::get<float>(stack);
        float h = vm::get<float>(stack);
        vm::finish(stack);
        if (URootWindow* rootWindow = rootSelf(object)) {
            rootwindow::snapshotWidth(rootWindow) = static_cast<int32_t>(w);
            rootwindow::snapshotHeight(rootWindow) = static_cast<int32_t>(h);
        }
    }

    void nativeGenerateSnapshot(UObject*, Frame& stack, void* result)
    {
        bool filter = optionalBoolDefault(stack, false);
        vm::finish(stack);
        vm::ret<UObject*>(result, generateSnapshot(filter));
    }

    void setSnapshotSize(int width, int height)
    {
        if (URootWindow* root = cast<URootWindow>(rootWindow())) {
            rootwindow::snapshotWidth(root) = width;
            rootwindow::snapshotHeight(root) = height;
        }
    }

    void setFrameReader(FrameReader reader)
    {
        state().frameReader = std::move(reader);
    }

    UObject* generateSnapshot(bool filter)
    {
        State& s = state();
        URootWindow* root = cast<URootWindow>(rootWindow());
        std::vector<uint8_t> frame;
        int frameWidth = 0;
        int frameHeight = 0;
        if (!root || !s.frameReader || !s.frameReader(frame, frameWidth, frameHeight) || frameWidth <= 0 || frameHeight <= 0 || frame.size() < static_cast<size_t>(frameWidth) * static_cast<size_t>(frameHeight) * 4) {
            return nullptr;
        }
        int width = rootwindow::snapshotWidth(root);
        int height = rootwindow::snapshotHeight(root);
        if (width <= 0 || height <= 0 || width > 4096 || height > 4096) {
            return nullptr;
        }
        UClass* textureClass = objects::findClass("Engine.Texture");
        UClass* paletteClass = objects::findClass("Engine.Palette");
        if (!textureClass || !paletteClass) {
            return nullptr;
        }
        // As XRootWindow::GenerateSnapshot: a P8 texture the next
        // power of two in each direction (UTexture::Init), with a palette of 256 grays. The
        // whole frame is stretched into its top-left snapshot-sized corner, each pixel the
        // average of the block it covers, in gray (the mean of red, green and blue); the rest
        // stays 0. Windows tile a background from the corner, so only the picture shows.
        auto bits = [](int size) {
            uint8_t result = 0;
            while ((1 << result) < size) {
                result++;
            }
            return result;
        };
        uint8_t ubits = bits(width);
        uint8_t vbits = bits(height);
        int textureWidth = 1 << ubits;
        int textureHeight = 1 << vbits;
        double stepX = static_cast<double>(frameWidth) / width;
        double stepY = static_cast<double>(frameHeight) / height;
        std::vector<uint8_t> pixels(static_cast<size_t>(textureWidth) * static_cast<size_t>(textureHeight));
        for (int y = 0; y < height; y++) {
            int y0 = std::clamp(static_cast<int>(y * stepY), 0, frameHeight - 1);
            int y1 = filter ? std::clamp(static_cast<int>((y + 1) * stepY), y0 + 1, frameHeight) : y0 + 1;
            for (int x = 0; x < width; x++) {
                int x0 = std::clamp(static_cast<int>(x * stepX), 0, frameWidth - 1);
                int x1 = filter ? std::clamp(static_cast<int>((x + 1) * stepX), x0 + 1, frameWidth) : x0 + 1;
                uint32_t sum = 0;
                for (int sy = y0; sy < y1; sy++) {
                    const uint8_t* row = frame.data() + static_cast<size_t>(sy) * static_cast<size_t>(frameWidth) * 4;
                    for (int sx = x0; sx < x1; sx++) {
                        const uint8_t* pixel = row + static_cast<size_t>(sx) * 4;
                        sum += static_cast<uint32_t>(pixel[0]) + pixel[1] + pixel[2];
                    }
                }
                uint32_t count = static_cast<uint32_t>((y1 - y0) * (x1 - x0)) * 3;
                pixels[static_cast<size_t>(y) * static_cast<size_t>(textureWidth) + static_cast<size_t>(x)] = static_cast<uint8_t>(sum / count);
            }
        }
        UObject* transient = objects::packageObject(Name("Transient"));
        auto* palette = cast<UPalette>(objects::newObject(paletteClass, transient, Name(), RF_Public | RF_Transient));
        auto* texture = cast<UTexture>(objects::newObject(textureClass, transient, Name(), RF_Public | RF_Transient));
        if (!palette || !texture) {
            return nullptr;
        }
        palette->colors.resize(256);
        for (uint32_t i = 0; i < 256; i++) {
            palette->colors[i] = i | (i << 8) | (i << 16) | 0xff000000u;
        }
        // TEXF_P8.
        *texture->propertyAddress(Name("Format")) = 0;
        if (uint8_t* address = texture->propertyAddress(Name("Palette"))) {
            *reinterpret_cast<UObject**>(address) = palette;
        }
        for (auto [property, value] : { std::pair { "USize", textureWidth }, std::pair { "VSize", textureHeight }, std::pair { "UClamp", textureWidth }, std::pair { "VClamp", textureHeight } }) {
            if (uint8_t* address = texture->propertyAddress(Name(property))) {
                *reinterpret_cast<int32_t*>(address) = value;
            }
        }
        for (auto [property, value] : { std::pair { "UBits", ubits }, std::pair { "VBits", vbits } }) {
            if (uint8_t* address = texture->propertyAddress(Name(property))) {
                *address = value;
            }
        }
        TextureMip mip;
        mip.width = static_cast<uint32_t>(textureWidth);
        mip.height = static_cast<uint32_t>(textureHeight);
        mip.ubits = ubits;
        mip.vbits = vbits;
        mip.data = std::move(pixels);
        texture->mips.push_back(std::move(mip));
        return texture;
    }

    void nativeIsCurrentModal(UObject* object, Frame& stack, void* result)
    {
        vm::finish(stack);
        UModalWindow* modal = cast<UModalWindow>(object);
        vm::retBool(result, modal && modal->isCurrentModal());
    }

    void nativeSetMouseFocusMode(UObject* object, Frame& stack, void*)
    {
        uint8_t mode = vm::get<uint8_t>(stack);
        vm::finish(stack);
        if (cast<UModalWindow>(object)) {
            modalwindow::focusMode(object) = mode;
        }
    }

    void nativeInitRootWindow(UObject* object, Frame& stack, void*)
    {
        vm::finish(stack);
        if (!object) {
            return;
        }
        std::string className = settings::get(settings::root);
        UClass* cls = extensionClass(className.c_str());
        if (!cls) {
            cls = extensionClass("Extension.RootWindow");
        }
        if (!cls) {
            return;
        }
        UObject* created = objects::newObject(cls, objects::packageObject(Name("Transient")), Name("RootWindow"), RF_Transient);
        objects::flushLoads();
        URootWindow* rootWindow = cast<URootWindow>(created);
        if (!rootWindow) {
            log::error("DevUI", "root window class {} is not a RootWindow", className);
            return;
        }
        State& s = state();
        if (s.root && s.root != rootWindow && !s.root->destroyed) {
            s.root->destroyWindowTree();
        }
        s.root = rootWindow;
        playerpawnext::RootWindow(object) = rootWindow;
        rootwindow::parentPawn(rootWindow) = object;
        window::bIsVisible(rootWindow) = 1;
        window::bIsSensitive(rootWindow) = 1;
        rootwindow::bRender(rootWindow) = 1;
        rootwindow::bCursorVisible(rootWindow) = 1;
        rootwindow::bPositionalSound(rootWindow) = 1;
        // XRootWindow::Init.
        rootwindow::multiClickTimeout(rootWindow) = 0.5f;
        rootwindow::maxMouseDist(rootWindow) = 10.0f;
        rootWindow->width() = virtualWidth();
        rootWindow->height() = virtualHeight();
        window::bConfigured(rootWindow) = 1;
        rootWindow->initWindow();
        window::bIsInitialized(rootWindow) = 1;
    }

    void nativePreRenderWindows(UObject*, Frame& stack, void*)
    {
        vm::get<UObject*>(stack);
        vm::finish(stack);
        State& s = state();
        if (s.root && !s.root->destroyed) {
            s.root->updateLayout();
        }
    }

    void nativePostRenderWindows(UObject*, Frame& stack, void*)
    {
        vm::get<UObject*>(stack);
        vm::finish(stack);
        State& s = state();
        if (s.frameCanvas) {
            drawRoot(*s.frameCanvas);
        }
    }
}
