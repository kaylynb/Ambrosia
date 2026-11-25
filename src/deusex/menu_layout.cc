#include "deusex/menu_layout.h"

#include "core/log.h"
#include "core/objects.h"
#include "core/sdl_handles.h"
#include "core/workers.h"
#include "deusex/deusex.h"
#include "deusex/menu_backdrop.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <format>
#include <fstream>
#include <string_view>
#include <system_error>
#include <utility>

namespace dx::deusex {
    namespace {
        // An int property of an object's defaults, or nothing.
        std::optional<int32_t> intOf(UObject* object, const char* name)
        {
            UProperty* property = object ? object->cls->findProperty(Name(name)) : nullptr;
            if (!property || property->type != PropType::Int) {
                return std::nullopt;
            }
            return *reinterpret_cast<const int32_t*>(object->propertyAddress(Name(name)));
        }

        const char* kindName(MenuSlot::Kind kind)
        {
            switch (kind) {
            case MenuSlot::Kind::Button:
                return "button";
            case MenuSlot::Kind::Field:
                return "field";
            case MenuSlot::Kind::TextBox:
                return "textbox";
            case MenuSlot::Kind::Track:
                return "track";
            }
            return "button";
        }

        // The slot round a widget: the art's outline is a pixel outside it all round.
        MenuSlot around(MenuSlot::Kind kind, int x, int y, int width, int height)
        {
            return { kind, x - 1, y - 1, width + 2, height + 2 };
        }
    }

    const std::vector<std::string>& generatedArtScreens()
    {
        static const std::vector<std::string> screens = {
            "MenuScreenSound",
            "MenuScreenDisplay",
            "MenuScreenControls",
            "MenuScreenGamepad",
            "MenuScreenOptions",
            "MenuScreenGraphics",
        };
        return screens;
    }

    std::optional<MenuLayout> optionsLayout(UClass* screen)
    {
        UClass* base = objects::findClass("DeusEx.MenuUIScreenWindow");
        UClass* sliderChoice = objects::findClass("DeusEx.MenuUIChoiceSlider");
        UClass* enumChoice = objects::findClass("DeusEx.MenuUIChoiceEnum");
        UClass* sliderWindow = objects::findClass("DeusEx.MenuUISliderButtonWindow");
        if (!screen || !screen->defaultObject || !base || !screen->isChildOf(base) || !sliderChoice || !enumChoice || !sliderWindow || !sliderWindow->defaultObject) {
            return std::nullopt;
        }
        UObject* defaults = screen->defaultObject;
        UProperty* choices = screen->findProperty(Name("choices"));
        auto width = intOf(defaults, "ClientWidth");
        auto height = intOf(defaults, "ClientHeight");
        auto startX = intOf(defaults, "choiceStartX");
        auto startY = intOf(defaults, "choiceStartY");
        auto gap = intOf(defaults, "choiceVerticalGap");
        auto helpY = intOf(defaults, "helpPosY");
        auto helpLeft = intOf(defaults, "defaultHelpLeftOffset");
        auto helpHeight = intOf(defaults, "defaultHelpHeight");
        auto helpDiff = intOf(defaults, "defaultHelpClientDiffY");
        auto scaleWidth = intOf(sliderWindow->defaultObject, "defaultScaleWidth");
        if (!choices || !width || !height || !startX || !startY || !gap || !helpY || !helpLeft || !helpHeight || !helpDiff || !scaleWidth) {
            return std::nullopt;
        }
        MenuLayout layout;
        layout.screen = screen->name.str();
        layout.width = *width;
        layout.height = *height;
        int row = 0;
        for (int i = 0; i < choices->arrayDim; i++) {
            UClass* choice = cast<UClass>(*reinterpret_cast<UObject**>(defaults->propertyAddress(Name("choices"), i)));
            if (!choice || !choice->defaultObject) {
                continue;
            }
            UObject* choiceDefaults = choice->defaultObject;
            // CreateChoices puts each choice a gap below the last, up by its buttonVerticalOffset,
            // which its action button is then moved back down by: the button sits on the row.
            int x = *startX;
            int y = *startY + row * *gap;
            row++;
            int offset = intOf(choiceDefaults, "buttonVerticalOffset").value_or(0);
            // MenuUIChoiceButton: 243 wide, MenuUIActionButtonWindow's 19 high.
            layout.slots.push_back(around(MenuSlot::Kind::Button, x, y, 243, 19));
            if (choice->isChildOf(sliderChoice)) {
                // MenuUIChoiceSlider: the slider at choiceControlPosX in the choice; its track
                // defaultScaleWidth by 21, and the value text 60 by 19 at (184, 1) in it.
                int control = x + intOf(choiceDefaults, "choiceControlPosX").value_or(270);
                int top = y - offset;
                layout.slots.push_back(around(MenuSlot::Kind::Track, control, top, *scaleWidth, 21));
                layout.slots.push_back(around(MenuSlot::Kind::Field, control + 184, top + 1, 60, 19));
            } else if (choice->isChildOf(enumChoice)) {
                // MenuUIChoiceEnum: the value, defaultInfoWidth by 19 at defaultInfoPosX.
                int info = x + intOf(choiceDefaults, "defaultInfoPosX").value_or(270);
                int infoWidth = intOf(choiceDefaults, "defaultInfoWidth").value_or(77);
                layout.slots.push_back(around(MenuSlot::Kind::Field, info, y - offset, infoWidth, 19));
            }
        }
        // The help text (MenuUIWindow.CreateHelpWindow): its outline is in its width.
        if (*helpY > 0) {
            layout.slots.push_back({ MenuSlot::Kind::TextBox, *helpLeft - 1, *helpY - 1, *width - *helpDiff, *helpHeight + 2 });
        }
        return layout;
    }

    std::string layoutHash(const MenuLayout& layout)
    {
        // FNV-1a over the size and slots.
        uint32_t hash = 2166136261u;
        auto mix = [&](int value) {
            for (int shift = 0; shift < 32; shift += 8) {
                hash ^= static_cast<uint32_t>(value >> shift) & 0xffu;
                hash *= 16777619u;
            }
        };
        mix(layout.width);
        mix(layout.height);
        for (const MenuSlot& slot : layout.slots) {
            mix(static_cast<int>(slot.kind));
            mix(slot.x);
            mix(slot.y);
            mix(slot.width);
            mix(slot.height);
        }
        return std::format("{:08x}", hash);
    }

    std::string layoutJson(const MenuLayout& layout)
    {
        std::string json = std::format("{{\n \"name\": \"{}\",\n \"hash\": \"{}\",\n \"width\": {},\n \"height\": {},\n \"items\": [\n", layout.screen, layoutHash(layout), layout.width, layout.height);
        for (size_t i = 0; i < layout.slots.size(); i++) {
            const MenuSlot& slot = layout.slots[i];
            json += std::format("  {{\"kind\": \"{}\", \"x\": {}, \"y\": {}, \"w\": {}, \"h\": {}}}{}\n", kindName(slot.kind), slot.x, slot.y, slot.width, slot.height, i + 1 < layout.slots.size() ? "," : "");
        }
        json += " ]\n}\n";
        return json;
    }

    void dumpMenuLayouts(const std::filesystem::path& directory)
    {
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        for (const std::string& name : generatedArtScreens()) {
            std::optional<MenuLayout> layout = optionsLayout(objects::findClass("DeusEx." + name));
            if (!layout) {
                log::warn("DevUI", "no layout to write for {}", name);
                continue;
            }
            std::filesystem::path file = directory / (name + ".json");
            std::ofstream out(file, std::ios::binary);
            out << layoutJson(*layout);
            if (!out) {
                log::warn("DevUI", "cannot write {}", file.string());
                continue;
            }
            Backdrop art = generateBackdrop(*layout, backdropSeed(name));
            std::filesystem::path picture = directory / (name + ".png");
            if (!sdl::savePng(picture.string(), art.bgra, art.width, art.height)) {
                log::warn("DevUI", "cannot write {}", picture.string());
            }
            log::info("DevUI", "wrote {}'s layout {} and art to {}", name, layoutHash(*layout), directory.string());
        }
    }

    void applyGeneratedMenuArt()
    {
        auto began = std::chrono::steady_clock::now();

        struct Job {
            UClass* screen;
            MenuLayout layout;
            Backdrop art;
        };

        std::vector<Job> jobs;
        for (const std::string& name : generatedArtScreens()) {
            UClass* screen = objects::findClass("DeusEx." + name);
            if (std::optional<MenuLayout> layout = optionsLayout(screen)) {
                jobs.push_back({ screen, std::move(*layout), {} });
            }
        }
        // Each backdrop on its own, on as many threads as background work may use (this one
        // among them); they touch nothing but their own layout and pixels. The menus are changed
        // here afterwards, on this thread.
        std::atomic<size_t> next = 0;
        auto work = [&](const std::stop_token&) {
            for (size_t i; (i = next++) < jobs.size();) {
                jobs[i].art = generateBackdrop(jobs[i].layout, backdropSeed(jobs[i].layout.screen));
            }
        };
        size_t helpers = jobs.empty() ? 0 : std::min(jobs.size(), static_cast<size_t>(std::max(1, workers::threadCap()))) - 1;
        std::vector<workers::Worker> threads(helpers);
        for (workers::Worker& thread : threads) {
            thread.start("Menu backdrops", work);
        }
        work(std::stop_token());
        for (workers::Worker& thread : threads) {
            thread.join();
        }
        int made = 0;
        for (Job& job : jobs) {
            MenuArt menuArt(job.screen->defaultObject, "the " + job.layout.screen + " generated art", job.art.width, job.art.height, job.art.bgra);
            made += menuArt.apply() ? 1 : 0;
        }
        auto took = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began).count();
        log::info("DevUI", "generated {} menu backdrops in {:.1f} ms", made, took);
    }
}
