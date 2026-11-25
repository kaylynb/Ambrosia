#include "deusex/script.h"

#include "core/objects.h"
#include "engine/input.h"
#include "engine/props_engine.h"
#include "extension/windows.h"

namespace dx::script {
    namespace {
        UProperty* property(UObject* object, const char* name)
        {
            return object && !object->destroyed && object->cls ? object->cls->findProperty(Name(name)) : nullptr;
        }

        std::string unquoted(std::string text)
        {
            if (text.size() >= 2 && (text.front() == '"' || text.front() == '\'') && text.back() == text.front()) {
                return text.substr(1, text.size() - 2);
            }
            return text;
        }

        // Linked lists in game objects are followed no further than this, in case one loops.
        constexpr size_t listLimit = 200;
    }

    UObject* object(UObject* object, const char* name, int32_t index)
    {
        UObject** value = field<UObject*>(object, name, index);
        return value ? *value : nullptr;
    }

    int32_t integer(UObject* object, const char* name)
    {
        int32_t* value = field<int32_t>(object, name);
        return value ? *value : 0;
    }

    float number(UObject* object, const char* name)
    {
        float* value = field<float>(object, name);
        return value ? *value : 0.0f;
    }

    std::string string(UObject* object, const char* name)
    {
        std::string* value = field<std::string>(object, name);
        return value ? *value : std::string();
    }

    bool flag(UObject* object, const char* name)
    {
        UProperty* found = property(object, name);
        return found && found->toText(object->data + found->offset) == "True";
    }

    void setFlag(UObject* object, const char* name, bool value)
    {
        if (UProperty* found = property(object, name)) {
            found->fromText(object->data + found->offset, value ? "True" : "False");
        }
    }

    std::string text(UObject* object, const char* name, int32_t index)
    {
        UProperty* found = property(object, name);
        if (!found || index < 0 || index >= found->arrayDim) {
            return {};
        }
        return unquoted(found->toText(object->data + found->offset + static_cast<size_t>(index) * found->elementSize()));
    }

    std::string member(UObject* object, const char* name, int32_t index, const std::string& memberName)
    {
        std::string value = text(object, name, index);
        size_t at = value.find(memberName + "=");
        if (at == std::string::npos) {
            return {};
        }
        at += memberName.size() + 1;
        size_t end = value.find_first_of(",)", at);
        return unquoted(value.substr(at, end == std::string::npos ? std::string::npos : end - at));
    }

    bool isA(UObject* object, const char* className)
    {
        return object && !object->destroyed && object->isA(Name(className));
    }

    extension::UButtonWindow* usableButton(UObject* screen, const char* name)
    {
        auto* button = cast<extension::UButtonWindow>(object(screen, name));
        for (extension::UWindow* current = button; current; current = current->parent()) {
            if (!current->visible() || !current->sensitive()) {
                return nullptr;
            }
            if (current == screen) {
                break;
            }
        }
        return button;
    }

    bool pressButton(UObject* screen, const char* name)
    {
        extension::UButtonWindow* button = usableButton(screen, name);
        if (button) {
            button->activate(IK_Enter);
        }
        return button != nullptr;
    }

    std::vector<UObject*> inventory(UObject* player)
    {
        std::vector<UObject*> items;
        for (UObject* item = player ? actor::Inventory(player) : nullptr; item && items.size() < listLimit; item = actor::Inventory(item)) {
            items.push_back(item);
        }
        return items;
    }

    std::vector<UObject*> augmentations(UObject* player)
    {
        std::vector<UObject*> augs;
        size_t count = 0;
        for (UObject* aug = object(object(player, "AugmentationSystem"), "FirstAug"); aug && count < listLimit; aug = object(aug, "next"), count++) {
            if (flag(aug, "bHasIt")) {
                augs.push_back(aug);
            }
        }
        return augs;
    }

    bool isGrenade(UObject* item)
    {
        return isA(item, "WeaponLAM") || isA(item, "WeaponGasGrenade") || isA(item, "WeaponEMPGrenade") || isA(item, "WeaponNanoVirusGrenade");
    }
}
