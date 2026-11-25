#include "extension/windows.h"

#include "extension/window_natives.h"

#include <algorithm>
#include <cmath>

namespace dx::extension {
    UWindow* self(UObject* object)
    {
        return cast<UWindow>(object);
    }

    void nativeConvertScriptString(UObject*, Frame& stack, void* result)
    {
        std::string text = vm::get<std::string>(stack);
        vm::finish(stack);
        vm::ret<std::string>(result, text);
    }

    void nativeIsKeyDown(UObject*, Frame& stack, void* result)
    {
        uint8_t key = vm::get<uint8_t>(stack);
        vm::finish(stack);
        vm::retBool(result, state().keys[key]);
    }

    void nativeIsPointInWindow(UObject* object, Frame& stack, void* result)
    {
        float px = vm::get<float>(stack);
        float py = vm::get<float>(stack);
        vm::finish(stack);
        UWindow* window = self(object);
        vm::retBool(result, window && px >= 0 && py >= 0 && px < window->width() && py < window->height());
    }

    void nativeFindWindow(UObject* object, Frame& stack, void* result)
    {
        float px = vm::get<float>(stack);
        float py = vm::get<float>(stack);
        float* outX = vm::getRef<float>(stack);
        float* outY = vm::getRef<float>(stack);
        vm::finish(stack);
        UWindow* window = self(object);
        UWindow* found = nullptr;
        if (window) {
            float ax = 0;
            float ay = 0;
            window->absolutePosition(ax, ay);
            URootWindow* rootWindow = window->root();
            UWindow* current = rootWindow ? static_cast<UWindow*>(rootWindow) : window;
            float cx = 0;
            float cy = 0;
            current->absolutePosition(cx, cy);
            float x = px + ax - cx;
            float y = py + ay - cy;
            while (true) {
                UWindow* hit = nullptr;
                for (UWindow* child = current->lastChild(); child; child = child->prevSibling()) {
                    if (child->visible() && child->usedX <= x && child->usedY <= y && child->usedX + child->width() > x && child->usedY + child->height() > y) {
                        hit = child;
                        break;
                    }
                }
                if (!hit) {
                    break;
                }
                current = hit;
                x -= hit->usedX;
                y -= hit->usedY;
            }
            found = current;
            *outX = x;
            *outY = y;
        }
        vm::ret<UObject*>(result, found);
    }

    void nativeSetClientObject(UObject* object, Frame& stack, void*)
    {
        UObject* client = vm::get<UObject*>(stack);
        vm::finish(stack);
        if (UWindow* window = self(object)) {
            window::clientObject(window) = client;
        }
    }

    void nativeGetClientObject(UObject* object, Frame& stack, void* result)
    {
        vm::finish(stack);
        UWindow* window = self(object);
        vm::ret<UObject*>(result, window ? window::clientObject(window) : nullptr);
    }

    void nativeConvertVectorToCoordinates(UObject* object, Frame& stack, void* result)
    {
        Vector location = vm::get<Vector>(stack);
        float* outX = vm::getRef<float>(stack);
        float* outY = vm::getRef<float>(stack);
        vm::finish(stack);
        UWindow* window = self(object);
        URootWindow* rootWindow = window ? window->root() : nullptr;
        float pixelX = 0;
        float pixelY = 0;
        if (!window || !rootWindow || !state().projector) {
            vm::retBool(result, false);
            return;
        }
        bool inFront = state().projector(location, pixelX, pixelY);
        float rootX = 0;
        float rootY = 0;
        float scale = virtualScale();
        if (inFront) {
            rootX = std::trunc(pixelX / scale - rootWindow->usedX);
            rootY = std::trunc(pixelY / scale - rootWindow->usedY);
        } else {
            rootX = std::trunc(state().viewportWidth * 0.5f / scale - rootWindow->usedX);
            rootY = std::trunc(state().viewportHeight * 0.5f / scale - rootWindow->usedY);
        }
        float ax = 0;
        float ay = 0;
        window->absolutePosition(ax, ay);
        if (outX) {
            *outX = rootX - (ax - rootWindow->usedX);
        }
        if (outY) {
            *outY = rootY - (ay - rootWindow->usedY);
        }
        vm::retBool(result, inFront);
    }

    void nativeAddTimer(UObject* object, Frame& stack, void* result)
    {
        float timeout = vm::get<float>(stack);
        bool loop = optionalBoolDefault(stack, false);
        int32_t clientData = 0;
        Name function;
        optional(stack, clientData);
        optional(stack, function);
        vm::finish(stack);
        UWindow* window = self(object);
        if (!window || function.none()) {
            vm::ret<int32_t>(result, -1);
            return;
        }
        int32_t id = window->nextTimerId++;
        UWindow::Timer timer;
        timer.function = function;
        // XWindow::AddTimer keeps timeouts at
        // least a millionth of a second.
        timeout = std::max(timeout, 1e-6f);
        timer.timeout = timeout;
        timer.loop = loop;
        timer.clientData = clientData;
        timer.left = timeout;
        window->timers[id] = timer;
        vm::ret<int32_t>(result, id);
    }

    void nativeRemoveTimer(UObject* object, Frame& stack, void*)
    {
        int32_t id = vm::get<int32_t>(stack);
        vm::finish(stack);
        if (UWindow* window = self(object)) {
            window->timers.erase(id);
        }
    }

    void nativeGetTickOffset(UObject*, Frame& stack, void* result)
    {
        vm::finish(stack);
        vm::ret<float>(result, 0.0f);
    }

    namespace {
        struct ActorRefLayout {
            UProperty* list = nullptr;
            size_t actorOffset = 0;
            size_t countOffset = 0;
            size_t stride = 0;
            int32_t capacity = 0;
        };

        ActorRefLayout actorRefLayout(UObject* player)
        {
            ActorRefLayout layout;
            if (!player || !player->cls) {
                return layout;
            }
            layout.list = player->cls->findProperty(Name("actorList"));
            if (!layout.list || !layout.list->structType) {
                layout.list = nullptr;
                return layout;
            }
            UProperty* actor = layout.list->structType->findProperty(Name("Actor"));
            UProperty* count = layout.list->structType->findProperty(Name("RefCount"));
            if (!actor || !count) {
                layout.list = nullptr;
                return layout;
            }
            layout.actorOffset = actor->offset;
            layout.countOffset = count->offset;
            layout.stride = layout.list->elementSize();
            layout.capacity = layout.list->arrayDim;
            return layout;
        }
    }

    void nativeAddActorRef(UObject* object, Frame& stack, void*)
    {
        UObject* actor = vm::get<UObject*>(stack);
        vm::finish(stack);
        UWindow* window = self(object);
        UObject* player = window ? window->playerPawn() : nullptr;
        ActorRefLayout layout = actorRefLayout(player);
        if (!layout.list || !actor) {
            return;
        }
        uint8_t* base = player->data + layout.list->offset;
        int32_t& count = playerpawnext::actorCount(player);
        for (int32_t i = 0; i < count; i++) {
            uint8_t* entry = base + static_cast<size_t>(i) * layout.stride;
            if (*reinterpret_cast<UObject**>(entry + layout.actorOffset) == actor) {
                (*reinterpret_cast<int32_t*>(entry + layout.countOffset))++;
                return;
            }
        }
        if (count >= layout.capacity) {
            for (int32_t i = 1; i < count; i++) {
                std::copy_n(base + static_cast<size_t>(i) * layout.stride, layout.stride, base + static_cast<size_t>(i - 1) * layout.stride);
            }
            count--;
        }
        uint8_t* entry = base + static_cast<size_t>(count) * layout.stride;
        *reinterpret_cast<UObject**>(entry + layout.actorOffset) = actor;
        *reinterpret_cast<int32_t*>(entry + layout.countOffset) = 1;
        count++;
    }

    void nativeRemoveActorRef(UObject* object, Frame& stack, void*)
    {
        UObject* actor = vm::get<UObject*>(stack);
        vm::finish(stack);
        UWindow* window = self(object);
        UObject* player = window ? window->playerPawn() : nullptr;
        ActorRefLayout layout = actorRefLayout(player);
        if (!layout.list || !actor) {
            return;
        }
        uint8_t* base = player->data + layout.list->offset;
        int32_t& count = playerpawnext::actorCount(player);
        for (int32_t i = 0; i < count; i++) {
            uint8_t* entry = base + static_cast<size_t>(i) * layout.stride;
            if (*reinterpret_cast<UObject**>(entry + layout.actorOffset) != actor) {
                continue;
            }
            int32_t& refs = *reinterpret_cast<int32_t*>(entry + layout.countOffset);
            refs--;
            if (refs <= 0) {
                for (int32_t j = i + 1; j < count; j++) {
                    std::copy_n(base + static_cast<size_t>(j) * layout.stride, layout.stride, base + static_cast<size_t>(j - 1) * layout.stride);
                }
                uint8_t* last = base + static_cast<size_t>(count - 1) * layout.stride;
                *reinterpret_cast<UObject**>(last + layout.actorOffset) = nullptr;
                *reinterpret_cast<int32_t*>(last + layout.countOffset) = 0;
                count--;
            }
            return;
        }
    }

    void nativeIsActorValid(UObject* object, Frame& stack, void* result)
    {
        UObject* actor = vm::get<UObject*>(stack);
        vm::finish(stack);
        UWindow* window = self(object);
        UObject* player = window ? window->playerPawn() : nullptr;
        ActorRefLayout layout = actorRefLayout(player);
        bool valid = false;
        if (layout.list && actor && !actor->destroyed) {
            uint8_t* base = player->data + layout.list->offset;
            int32_t count = playerpawnext::actorCount(player);
            for (int32_t i = 0; i < count; i++) {
                uint8_t* entry = base + static_cast<size_t>(i) * layout.stride;
                if (*reinterpret_cast<UObject**>(entry + layout.actorOffset) == actor) {
                    valid = true;
                }
            }
        }
        vm::retBool(result, valid);
    }

    void nativeStringToName(UObject*, Frame& stack, void* result)
    {
        std::string text = vm::get<std::string>(stack);
        vm::finish(stack);
        vm::ret<Name>(result, Name(text));
    }

    void registerWindowNatives()
    {
        vm::registerNative(1409, nativeDestroy);
        vm::registerNative(1410, nativeNewChild);
        vm::registerNative(1411, nativeRaise);
        vm::registerNative(1412, nativeLower);
        vm::registerNative(1413, nativeShow);
        vm::registerNative(1414, nativeHide);
        vm::registerNative(1415, nativeIsVisible);
        vm::registerNative(1416, nativeSetSensitivity);
        vm::registerNative(1417, nativeEnableWindow);
        vm::registerNative(1418, nativeDisableWindow);
        vm::registerNative(1419, nativeIsSensitive);
        vm::registerNative(1420, nativeSetSelectability);
        vm::registerNative(1421, nativeSetBackground);
        vm::registerNative(1422, nativeSetBackgroundStyle);
        vm::registerNative(1423, nativeSetBackgroundSmoothing);
        vm::registerNative(1424, nativeSetBackgroundStretching);
        vm::registerNative(1425, nativeGetRootWindow);
        vm::registerNative(1426, nativeGetModalWindow);
        vm::registerNative(1427, nativeGetTabGroupWindow);
        vm::registerNative(1428, nativeGetParent);
        vm::registerNative(1429, nativeGetPlayerPawn);
        vm::registerNative(1430, nativeSetConfiguration);
        vm::registerNative(1431, nativeSetSize);
        vm::registerNative(1432, nativeSetPos);
        vm::registerNative(1433, nativeSetWidth);
        vm::registerNative(1434, nativeSetHeight);
        vm::registerNative(1435, nativeResetSize);
        vm::registerNative(1436, nativeResetWidth);
        vm::registerNative(1437, nativeResetHeight);
        vm::registerNative(1438, nativeSetWindowAlignments);
        vm::registerNative(1439, nativeSetAcceleratorText);
        vm::registerNative(1440, nativeSetFocusWindow);
        vm::registerNative(1441, nativeGetFocusWindow);
        vm::registerNative(1442, nativeMoveFocusLeft);
        vm::registerNative(1443, nativeMoveFocusRight);
        vm::registerNative(1444, nativeMoveFocusUp);
        vm::registerNative(1445, nativeMoveFocusDown);
        vm::registerNative(1446, nativeMoveTabGroupNext);
        vm::registerNative(1447, nativeMoveTabGroupPrev);
        vm::registerNative(1448, nativeIsFocusWindow);
        vm::registerNative(1449, nativeConvertCoordinates);
        vm::registerNative(1450, nativeGrabMouse);
        vm::registerNative(1451, nativeUngrabMouse);
        vm::registerNative(1452, nativeGetCursorPos);
        vm::registerNative(1453, nativeSetCursorPos);
        vm::registerNative(1454, nativeSetDefaultCursor);
        vm::registerNative(1455, nativeGetTopChild);
        vm::registerNative(1456, nativeGetBottomChild);
        vm::registerNative(1457, nativeGetHigherSibling);
        vm::registerNative(1458, nativeGetLowerSibling);
        vm::registerNative(1459, nativeDestroyAllChildren);
        vm::registerNative(1460, nativeAskParentForReconfigure);
        vm::registerNative(1461, nativeConfigureChild);
        vm::registerNative(1462, nativeResizeChild);
        vm::registerNative(1463, nativeQueryPreferredWidth);
        vm::registerNative(1464, nativeQueryPreferredHeight);
        vm::registerNative(1465, nativeQueryPreferredSize);
        vm::registerNative(1466, nativeQueryGranularity);
        vm::registerNative(1467, nativeSetChildVisibility);
        vm::registerNative(1468, nativeAskParentToShowArea);
        vm::registerNative(1469, nativeConvertScriptString);
        vm::registerNative(1470, nativeIsKeyDown);
        vm::registerNative(1471, nativeIsPointInWindow);
        vm::registerNative(1472, nativeFindWindow);
        vm::registerNative(1473, nativePlaySound);
        vm::registerNative(1474, nativeSetSoundVolume);
        vm::registerNative(1475, nativeSetTileColor);
        vm::registerNative(1476, nativeSetTextColor);
        vm::registerNative(1477, nativeSetFont);
        vm::registerNative(1478, nativeSetFonts);
        vm::registerNative(1479, nativeSetNormalFont);
        vm::registerNative(1480, nativeSetBoldFont);
        vm::registerNative(1481, nativeEnableSpecialText);
        vm::registerNative(1482, nativeCarriageReturn);
        vm::registerNative(1483, nativeEnableTranslucentText);
        vm::registerNative(1484, nativeSetBaselineData);
        vm::registerNative(1485, nativeGetGC);
        vm::registerNative(1486, nativeReleaseGC);
        vm::registerNative(1487, nativeSetClientObject);
        vm::registerNative(1488, nativeGetClientObject);
        vm::registerNative(1489, nativeConvertVectorToCoordinates);
        vm::registerNative(1490, nativeAddTimer);
        vm::registerNative(1491, nativeRemoveTimer);
        vm::registerNative(1492, nativeGetTickOffset);
        vm::registerNative(1493, nativeChangeStyle);
        vm::registerNative(1495, nativeSetFocusSounds);
        vm::registerNative(1496, nativeSetVisibilitySounds);
        vm::registerNative(1497, nativeAddActorRef);
        vm::registerNative(1498, nativeRemoveActorRef);
        vm::registerNative(1499, nativeIsActorValid);
        vm::registerNative(1500, nativeSetMouseFocusMode);
        vm::registerNative(1501, nativeIsCurrentModal);
        vm::registerNative(1510, nativeSetDefaultEditCursor);
        vm::registerNative(1511, nativeSetDefaultMovementCursors);
        vm::registerNative(1512, nativeEnableRendering);
        vm::registerNative(1513, nativeIsRenderingEnabled);
        vm::registerNative(1514, nativeSetRenderViewport);
        vm::registerNative(1515, nativeResetRenderViewport);
        vm::registerNative(1516, nativeSetRawBackground);
        vm::registerNative(1517, nativeSetRawBackgroundSize);
        vm::registerNative(1518, nativeStretchRawBackground);
        vm::registerNative(1519, nativeEnablePositionalSound);
        vm::registerNative(1520, nativeIsPositionalSoundEnabled);
        vm::registerNative(1521, nativeLockMouse);
        vm::registerNative(1522, nativeShowCursor);
        vm::registerNative(1523, nativeSetSnapshotSize);
        vm::registerNative(1524, nativeGenerateSnapshot);
        vm::registerNative(1050, nativePreRenderWindows);
        vm::registerNative(1051, nativePostRenderWindows);
        vm::registerNative(1052, nativeInitRootWindow);
        vm::registerNative(1024, nativeStringToName);
    }
}
