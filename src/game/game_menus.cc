#include "game/game.h"

#include "audio/steam_audio.h"
#include "core/log.h"
#include "core/objects.h"
#include "deusex/deusex.h"
#include "deusex/gamepad_layout.h"
#include "game/platform.h"

#include <algorithm>
#include <cstdio>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dx::game {
    // Notes each property a class needs for a patch, and that an array one has `count` entries.
    void Game::needProperties(dx::deusex::MenuPatch& patch, dx::UClass* cls, std::initializer_list<const char*> names, int count)
    {
        for (const char* name : names) {
            dx::UProperty* property = patch.property(cls, name);
            if (property && property->arrayDim < count) {
                patch.miss(std::string(name) + "[" + std::to_string(count - 1) + "]");
            }
        }
    }

    template <typename T>
    T& Game::defaultField(dx::UObject* defaults, const char* name, int32_t index)
    {
        return *reinterpret_cast<T*>(defaults->propertyAddress(dx::Name(name), index));
    }

    // A new options screen with no choices yet, or null if the game's one is not there.
    dx::UClass* Game::createOptionsScreen(const char* className, const char* title)
    {
        dx::deusex::MenuPatch patch(std::string("the ") + title + " screen");
        dx::UClass* parent = patch.findClass("DeusEx.MenuScreenOptions");
        needProperties(patch, parent, { "Title", "choices", "choiceStartY", "choiceVerticalGap", "helpPosY", "ClientHeight", "textureRows" });
        if (!patch.ready()) {
            return nullptr;
        }
        dx::UClass* screen = dx::objects::createSubclass(parent, dx::Name(className));
        dx::UObject* defaults = screen->defaultObject;
        defaultField<std::string>(defaults, "Title") = title;
        for (int i = 0; i < screen->findProperty(dx::Name("choices"))->arrayDim; i++) {
            defaultField<dx::UObject*>(defaults, "choices", i) = nullptr;
        }
        return screen;
    }

    // A choice of the setting, which it reads and writes through the console's ini: (and so
    // shows the setting's default until it is first saved), with the setting's default for its
    // Reset to Defaults. Returns the choice's class, or null when it could not be made.
    dx::UClass* Game::addChoice(dx::UClass* screen, int index, const char* parentPath, const char* name, const char* action, const char* help, const dx::settings::Setting& setting, const std::vector<const char*>& values, int infoWidth)
    {
        // A screen that could not be made has already said so.
        if (!screen) {
            return nullptr;
        }
        dx::deusex::MenuPatch patch(std::string("the ") + name + " menu choice");
        dx::UClass* parent = patch.findClass(parentPath);
        needProperties(patch, parent, { "actionText", "helpText", "configSetting", "defaultValue", "defaultInfoWidth", "defaultInfoPosX" });
        needProperties(patch, parent, { "enumText" }, static_cast<int>(values.size()));
        needProperties(patch, screen, { "choices" }, index + 1);
        if (!patch.ready()) {
            return nullptr;
        }
        dx::UClass* choice = dx::objects::createSubclass(parent, dx::Name(name));
        dx::UObject* defaults = choice->defaultObject;
        defaultField<std::string>(defaults, "actionText") = action;
        defaultField<std::string>(defaults, "helpText") = help;
        defaultField<std::string>(defaults, "configSetting") = dx::settings::configSetting(setting);
        defaultField<int32_t>(defaults, "defaultValue") = static_cast<int32_t>(dx::settings::defaultNumber(setting));
        defaultField<int32_t>(defaults, "defaultInfoWidth") = infoWidth;
        defaultField<int32_t>(defaults, "defaultInfoPosX") = 270;
        int entries = std::min(choice->findProperty(dx::Name("enumText"))->arrayDim, 40);
        for (int i = 0; i < entries; i++) {
            defaultField<std::string>(defaults, "enumText", i) = static_cast<size_t>(i) < values.size() ? values[static_cast<size_t>(i)] : "";
        }
        defaultField<dx::UObject*>(screen->defaultObject, "choices", index) = choice;
        return choice;
    }

    void Game::addEnumChoice(dx::UClass* screen, int index, const char* name, const char* action, const char* help, const dx::settings::Int& setting, const std::vector<const char*>& values, int infoWidth)
    {
        addChoice(screen, index, "DeusEx.MenuUIChoiceEnum", name, action, help, setting.setting, values, infoWidth);
    }

    // A choice of two (Off and On unless named), which reads True and writes True or False.
    void Game::addSwitchChoice(dx::UClass* screen, int index, const char* name, const char* action, const char* help, const dx::settings::Bool& setting, int infoWidth, const std::vector<const char*>& values)
    {
        addChoice(screen, index, "DeusEx.MenuChoice_DetailTextures", name, action, help, setting.setting, values, infoWidth);
    }

    // A slider of whole numbers from `lowest` to `highest`, which reads and writes the setting
    // through the console's ini: as the original's volume sliders do.
    void Game::addSliderChoice(dx::UClass* screen, int index, const char* name, const char* action, const char* help, const dx::settings::Int& setting, int lowest, int highest)
    {
        if (!screen) {
            return;
        }
        dx::deusex::MenuPatch patch(std::string("the ") + name + " menu choice");
        dx::UClass* parent = patch.findClass("DeusEx.MenuUIChoiceSlider");
        needProperties(patch, parent, { "actionText", "helpText", "configSetting", "defaultValue", "numTicks", "startValue", "endValue" });
        needProperties(patch, screen, { "choices" }, index + 1);
        if (!patch.ready()) {
            return;
        }
        dx::UClass* choice = dx::objects::createSubclass(parent, dx::Name(name));
        dx::UObject* defaults = choice->defaultObject;
        defaultField<std::string>(defaults, "actionText") = action;
        defaultField<std::string>(defaults, "helpText") = help;
        defaultField<std::string>(defaults, "configSetting") = dx::settings::configSetting(setting.setting);
        defaultField<float>(defaults, "defaultValue") = static_cast<float>(dx::settings::defaultNumber(setting.setting));
        defaultField<int32_t>(defaults, "numTicks") = highest - lowest + 1;
        defaultField<float>(defaults, "startValue") = static_cast<float>(lowest);
        defaultField<float>(defaults, "endValue") = static_cast<float>(highest);
        defaultField<dx::UObject*>(screen->defaultObject, "choices", index) = choice;
    }

    // Grows an options screen made from one laid out for nine rows (MenuScreenOptions or
    // MenuScreenDisplay) to `rows`: its art is generated for whatever rows it has.
    void Game::sizeOptionsScreen(dx::UObject* defaults, int rows)
    {
        int gap = defaultField<int32_t>(defaults, "choiceVerticalGap");
        defaultField<int32_t>(defaults, "ClientHeight") += gap * (rows - 9);
        defaultField<int32_t>(defaults, "helpPosY") += gap * (rows - 9);
    }

    // A Graphics screen, after Display in the settings menu, for the renderer's own settings;
    // and the display's frame pacing and the UI's size on the Display screen, after its own rows.
    void Game::addExtraOptions()
    {
        dx::UClass* screen = createOptionsScreen("MenuScreenGraphics", "Graphics");
        addEnumChoice(screen, 0, "MenuChoice_Anisotropy", "|&Anisotropic Filtering", "Sharpens textures seen at a steep angle, such as floors in the distance.", dx::settings::anisotropy, { "Off", "2x", "4x", "8x", "16x" });
        addSwitchChoice(screen, 1, "MenuChoice_Trilinear", "|&Trilinear Filtering", "Blends between texture detail levels to remove visible bands on floors and walls.", dx::settings::useTrilinear);
        addEnumChoice(screen, 2, "MenuChoice_Multisampling", "Multi|&sample AA", "Smooths the edges of world geometry. Higher values cost more performance.", dx::settings::multisampling, { "Off", "2x", "4x", "8x" });
        addSwitchChoice(screen, 3, "MenuChoice_FXAA", "|&FXAA", "Fast edge smoothing for the 3D view. It also smooths masked textures such as fences.", dx::settings::fxaa);
        addSwitchChoice(screen, 4, "MenuChoice_SmoothLighting", "Smooth |&Lighting", "Filters lightmaps with a bicubic filter to soften blocky shadow edges.", dx::settings::smoothLightmaps);
        addEnumChoice(screen, 5, "MenuChoice_DetailDistance", "|&Detail Distance", "How far away detail textures stay visible. Normal matches the original game.", dx::settings::detailDistance, { "Normal", "Far", "Very Far" });
        addEnumChoice(screen, 6, "MenuChoice_AmbientOcclusion", "Ambient |&Occlusion", "Adds soft contact shadows under characters and objects. Higher levels are smoother and cost more performance. Off matches the original game.", dx::settings::ambientOcclusion, { "Off", "Low", "Medium", "High" });
        if (screen) {
            sizeOptionsScreen(screen->defaultObject, 7);
        }

        // On Display, after the game's nine rows (some of which go later, as removeIgnoredChoices
        // takes out what Ambrosia ignores and the platform what it has no use for).
        dx::deusex::MenuPatch displayPatch("the Display screen's frame rate and UI scale");
        dx::UClass* display = displayPatch.findClass("DeusEx.MenuScreenDisplay");
        needProperties(displayPatch, display, { "choiceVerticalGap", "ClientHeight", "helpPosY" });
        if (displayPatch.ready()) {
            addEnumChoice(display, 9, "MenuChoice_VSync", "|&Vertical Sync", "Waits for the display refresh. Adaptive skips the wait when a frame is late.", dx::settings::vsync, { "Off", "On", "Adaptive" });
            addEnumChoice(display, 10, "MenuChoice_FrameRateLimit", "Frame Rate L|&imit", "Caps the frame rate to save power and reduce heat.", dx::settings::frameRateLimit, { "Off" });
            addEnumChoice(display, 11, "MenuChoice_UIScale", "|&UI Scale", "Size of menus and the HUD. Auto keeps the whole interface on screen. Larger scales can push long conversations and some menus off the screen.", dx::settings::uiScale, { "Auto", "1x", "2x", "3x", "4x" });
            addEnumChoice(display, 12, "MenuChoice_ConversationFont", "|&Conversation Text", "Size of speech, names and replies within the scaled UI. Applies to the next conversation.", dx::settings::conversationFont, { "Auto", "Small", "Large" });
            sizeOptionsScreen(display->defaultObject, 13);
        }

        dx::deusex::MenuPatch patch("the Graphics button");
        dx::UObject* settings = patch.defaults(patch.findClass("DeusEx.MenuSettings"));
        if (screen && patch.ready()) {
            dx::deusex::insertMenuButton(settings, 4, "Graphics", screen);
        }
    }

    // The Gamepad screen's art is generated for the rows available on this platform.
    void Game::addControllerOptions()
    {
        dx::UClass* screen = createOptionsScreen("MenuScreenGamepad", "Gamepad");
        dx::deusex::MenuPatch patch("the controller options");
        // Where the platform fixes the button prompts (see applyControllerOptions) there is no
        // choice of them.
        const MenuOptions& available = platform::menuOptions();
        int added = controllerRows(available);
        needProperties(patch, screen, { "choices" }, added + 2);
        needProperties(patch, patch.findClass("DeusEx.MenuUIChoiceEnum"), { "enumText" }, 4);
        patch.findClass("DeusEx.MenuChoice_DetailTextures");
        dx::UClass* layoutRow = patch.findClass(std::string("DeusEx.") + dx::padlayout::layoutChoice);
        dx::UClass* calibrateRow = patch.findClass(std::string("DeusEx.") + dx::padlayout::calibrateChoice);
        dx::UObject* settings = patch.defaults(patch.findClass("DeusEx.MenuSettings"));
        dx::UClass* keys = patch.findClass("DeusEx.MenuScreenCustomizeKeys");
        dx::UClass* controls = patch.findClass("DeusEx.MenuScreenControls");
        if (!screen || !patch.ready()) {
            return;
        }
        dx::UObject* defaults = screen->defaultObject;
        // The width of the game's own Enabled/Disabled buttons on the Controls screen.
        constexpr int infoWidth = 77;
        int row = 0;
        addSliderChoice(screen, row++, "MenuChoice_LookSpeed", "L|&ook Speed", "How fast the controller's Look stick turns the view. 5 is the usual speed.", dx::settings::gamepadLookSpeed, 1, 10);
        if (available.buttonPrompts.offered) {
            // The choice's index is the value saved, so Auto comes after the styles it adds to.
            addEnumChoice(screen, row++, "MenuChoice_ButtonGlyphs", "|&Button Prompts", "Which controller's buttons the prompts show. Auto matches the controller in use.", dx::settings::gamepadGlyphs, { "Xbox", "PlayStation", "Switch", "Auto" }, infoWidth);
        }
        addSwitchChoice(screen, row++, "MenuChoice_AimAssist", "|&Aim Assist", "Slows turning near a target and gently follows it. Inactive while Gyro Aiming is on.", dx::settings::gamepadAimAssist, infoWidth);
        addSwitchChoice(screen, row++, "MenuChoice_GyroAiming", "|&Gyro Aiming", "Turn the controller to fine-tune your aim while you use the Look stick. Needs a controller with a gyro.", dx::settings::gamepadGyro, infoWidth);
        addEnumChoice(screen, row++, "MenuChoice_GyroSpace", "Gyro S|&pace", "How turning the controller turns the view. Player turns left and right however the controller is tilted. Local follows the controller's own axes, which suits a handheld or a controller held level.", dx::settings::gamepadGyroSpace, { "Local", "Player" }, infoWidth);
        addSliderChoice(screen, row++, "MenuChoice_GyroSensitivity", "G|&yro Sensitivity", "How far the view turns when you turn the controller.", dx::settings::gamepadGyroSensitivity, 1, 10);
        addSwitchChoice(screen, row++, "MenuChoice_FlickStick", "|&Flick Stick", "With Gyro Aiming on, push the Look stick all the way to turn to face where it points, and turn it around its edge to keep turning. The gyro does the rest of the aiming.", dx::settings::gamepadFlickStick, infoWidth);
        defaultField<dx::UObject*>(defaults, "choices", row++) = calibrateRow;
        defaultField<dx::UObject*>(defaults, "choices", row++) = layoutRow;
        sizeOptionsScreen(defaults, row);

        // Controls, Gamepad, then Keyboard/Mouse.
        int keysButton = dx::deusex::findMenuButton(settings, keys);
        int controlsButton = dx::deusex::findMenuButton(settings, controls);
        if (keysButton >= 0 && controlsButton >= 0 && controlsButton > keysButton) {
            dx::deusex::swapMenuButtons(settings, keysButton, controlsButton);
            controlsButton = keysButton;
        }
        dx::deusex::insertMenuButton(settings, controlsButton + 1, "Gamepad", screen);
    }

    // Stereo, Deus Ex's own mix, or Steam Audio's HRTF, in place of Sample Rate (which Ambrosia
    // ignores, so its row would go), only where Steam Audio loads. It reads and writes the
    // derived AmbrosiaAudioMixer key (settings::audioMixerKey) rather than AmbrosiaHRTF, so it shows
    // Raytraced when the debug overlay or AmbrosiaRaytracedAudio has turned that mode on, but
    // cycles only between Stereo and HRTF (deusex::syncMixerChoice hides Raytraced once it is
    // left).
    void Game::addMixerChoice()
    {
        if (!dx::audio::steam::available()) {
            return;
        }
        dx::UClass* screen = dx::objects::findClass("DeusEx.MenuScreenSound");
        if (!screen) {
            dx::log::warn("DevUI", "skipping the Audio Mixer menu choice: DeusEx.MenuScreenSound not found");
            return;
        }
        dx::UClass* choice = addChoice(screen, 4, "DeusEx.MenuUIChoiceEnum", "MenuChoice_AudioMixer", "Audio |&Mixer", "Stereo is Deus Ex's own mix. HRTF uses Steam Audio to place sounds all around you, including above and below. Use headphones with HRTF.", dx::settings::hrtf.setting, { "Stereo", "HRTF", "Raytraced" }, 83);
        if (choice) {
            defaultField<std::string>(choice->defaultObject, "configSetting") = dx::settings::audioMixerConfigSetting();
        }
    }

    // Takes out the choices the platform has no use for (menu_options.h). Runs after the other
    // menu changes, which find buttons by their places.
    void Game::removeUnavailableOptions()
    {
        const MenuOptions& available = platform::menuOptions();
        // Keyboard/Mouse, the key bindings.
        if (!available.keyBindings) {
            dx::UClass* settings = dx::objects::findClass("DeusEx.MenuSettings");
            int button = settings ? dx::deusex::findMenuButton(settings->defaultObject, dx::objects::findClass("DeusEx.MenuScreenCustomizeKeys")) : -1;
            if (button >= 0) {
                dx::deusex::removeMenuButton(settings->defaultObject, button);
            }
        }
        // The rows holding these choices, found by class since other changes move rows about.
        auto rowsOf = [](dx::UObject* screen, const std::vector<const char*>& classes) {
            std::vector<int> rows;
            dx::UProperty* choices = screen->cls->findProperty(dx::Name("choices"));
            for (int i = 0; choices && i < choices->arrayDim; i++) {
                auto* choice = *reinterpret_cast<dx::UClass**>(screen->propertyAddress(dx::Name("choices"), i));
                for (const char* name : classes) {
                    if (choice && choice->name == dx::Name(name)) {
                        rows.push_back(i);
                    }
                }
            }
            return rows;
        };
        std::vector<RemovedChoice> removed = removedChoices(available);
        for (size_t first = 0; first < removed.size();) {
            std::string_view screenName = removed[first].screen;
            std::vector<const char*> classes;
            size_t end = first;
            for (; end < removed.size() && removed[end].screen == screenName; end++) {
                classes.push_back(removed[end].choice);
            }
            if (dx::UClass* screen = dx::objects::findClass("DeusEx." + std::string(screenName))) {
                dx::deusex::removeOptionRows(screen->defaultObject, rowsOf(screen->defaultObject, classes));
            }
            first = end;
        }
    }

    void Game::addFieldOfViewChoice()
    {
        dx::deusex::MenuPatch patch("the Field of View menu choice");
        dx::UClass* screen = patch.findClass("DeusEx.MenuScreenDisplay");
        dx::UClass* parent = patch.findClass("DeusEx.MenuUIChoiceEnum");
        needProperties(patch, screen, { "choices" }, 3);
        needProperties(patch, parent, { "actionText", "helpText", "configSetting", "defaultValue", "defaultInfoWidth" });
        needProperties(patch, parent, { "enumText" }, 13);
        if (!patch.ready()) {
            return;
        }
        dx::UClass* choice = dx::objects::createSubclass(parent, dx::Name("MenuChoice_FieldOfView"));
        defaultField<dx::UObject*>(screen->defaultObject, "choices", 2) = choice;
        dx::UObject* defaults = choice->defaultObject;
        defaultField<std::string>(defaults, "actionText") = "Field of |&View";
        defaultField<std::string>(defaults, "helpText") = "Horizontal field of view on a 4:3 screen. Wider screens show more at the sides.";
        defaultField<std::string>(defaults, "configSetting") = dx::settings::configSetting(dx::settings::fieldOfViewIndex.setting);
        defaultField<int32_t>(defaults, "defaultValue") = static_cast<int32_t>(dx::settings::defaultNumber(dx::settings::fieldOfViewIndex.setting));
        defaultField<int32_t>(defaults, "defaultInfoWidth") = 98;
        for (int i = 0; i <= 12; i++) {
            defaultField<std::string>(defaults, "enumText", i) = std::to_string(60 + 5 * i);
        }
    }
}
