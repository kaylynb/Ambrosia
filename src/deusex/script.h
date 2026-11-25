#pragma once

#include "core/object.h"

#include <cstdint>
#include <string>
#include <vector>

namespace dx::extension {
    class UButtonWindow;
}

namespace dx::script {
    // Reading and writing the game's UnrealScript objects by property name, for the native
    // overlays and shortcuts that work with Deus Ex's own classes. Every getter takes a null or
    // destroyed object, or a property the class lacks, and returns an empty value.

    // The address of a property, or null.
    template <typename T>
    T* field(UObject* object, const char* name, int32_t index = 0)
    {
        return object && !object->destroyed ? reinterpret_cast<T*>(object->propertyAddress(Name(name), index)) : nullptr;
    }

    UObject* object(UObject* object, const char* name, int32_t index = 0);
    int32_t integer(UObject* object, const char* name);
    float number(UObject* object, const char* name);
    std::string string(UObject* object, const char* name);
    // Bool properties are bits, so they go through the property's text form.
    bool flag(UObject* object, const char* name);
    void setFlag(UObject* object, const char* name, bool value);
    // Any property's value as text, without the quotes around a name or string.
    std::string text(UObject* object, const char* name, int32_t index = 0);
    // One member of a struct in an array, such as emailInfo[2].emailName.
    std::string member(UObject* object, const char* name, int32_t index, const std::string& memberName);

    // Whether an object is of a class or one of its subclasses, by the class's name.
    bool isA(UObject* object, const char* className);

    // A screen's button by property name, when it and the windows around it up to the screen
    // are shown and can be pressed; otherwise null.
    extension::UButtonWindow* usableButton(UObject* screen, const char* name);
    // Presses that button as Enter would, if it can be pressed.
    bool pressButton(UObject* screen, const char* name);

    // The player's inventory, in the order the game keeps it.
    std::vector<UObject*> inventory(UObject* player);
    // The augmentations the player has installed, in the augmentation system's order.
    std::vector<UObject*> augmentations(UObject* player);
    // The four grenades, which are weapons but sit apart from the guns.
    bool isGrenade(UObject* item);
}
