#pragma once

#include "core/math.h"
#include "core/object.h"
#include "extension/extension.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace dx {
    class World;

    namespace deusex {
        // A startup change to the game's menus, which checks every class and property it uses
        // first: a modified DeusEx.u that renames one loses that one addition, with a line in
        // the log, rather than crashing. Look everything up, then write only if ready().
        class MenuPatch {
        public:
            explicit MenuPatch(std::string what);
            UClass* findClass(std::string_view path);
            // The defaults of a class, or null.
            UObject* defaults(UClass* cls);
            UProperty* property(UStruct* type, const char* name);

            // The address of an object's property (an element of an array one), or null.
            template <typename T>
            T* field(UObject* object, const char* name, int32_t index = 0)
            {
                uint8_t* address = object ? object->propertyAddress(Name(name), index) : nullptr;
                if (!address) {
                    miss(index ? std::string(name) + "[" + std::to_string(index) + "]" : std::string(name));
                }
                return reinterpret_cast<T*>(address);
            }

            // Notes something else the patch needs that is not there.
            void miss(const std::string& name);
            // Logs the first thing missing, once, and returns false if anything was.
            bool ready();

        private:
            std::string what;
            std::string missing;
            bool logged = false;
        };

        // For inspecting levels (--no-mission-script): each level that has not begun play loads
        // with its DeusExLevelInfo's Script cleared, so no mission script is spawned, and without
        // its CameraPoints, the cutscene camera paths. The level is then as placed in the editor,
        // without the cutscenes, flags, story changes to its actors and level changes the script
        // would make.
        inline bool skipMissionScripts = false;
        // For inspecting levels (--no-triggers): each level that has not begun play loads
        // without its Trigger actors (and their subclasses, such as ConversationTrigger and
        // DataLinkTrigger), and no actor is given conversations (ConBindEvents binds none), so
        // walking or flying about sets nothing off: no NPC starts one when the player comes in
        // range, is used or barks.
        inline bool skipTriggers = false;

        void levelLoaded(World& world);
        bool scriptedPawnTick(World& world, UObject* pawn, float deltaSeconds);
        // Whether the actor's script Tick runs on the Steady effect clock (light_effects.h):
        // electricity emitters, and laser and beam triggers while confused by EMP.
        bool onEffectClock(UObject* actor);
        bool renderIterate(World& world, UObject* owner, const std::function<void(UObject* item)>& emit);

        // A menu's background art (its clientTextures, textureCols across) being rearranged:
        // every change to the menus' art goes through one. It is read once, kept as the rows of
        // the original picture that make up each row now, and written back once by apply. Each
        // edit works on the art as the edits before it left it, in pixels from its top.
        class MenuArt {
        public:
            // The art of a menu's defaults. One that cannot be read says why in the log, under
            // `what`, and its edits and apply do nothing.
            MenuArt(UObject* screen, std::string what);
            // The menu's art replaced whole by a picture (BGRA, rows top first, see-through at
            // alpha 0) no wider than its tiles across, to be written back by apply.
            MenuArt(UObject* screen, std::string what, int width, int height, std::span<const uint8_t> bgra);
            ~MenuArt();
            MenuArt(const MenuArt&) = delete;
            MenuArt& operator=(const MenuArt&) = delete;

            bool valid() const;
            int width() const;
            int height() const;
            // Takes out rows [from, to).
            void remove(int from, int to);
            // Puts a copy of rows [from, to) in at row `at`, before what is there.
            void repeat(int at, int from, int to);
            // Keeps the rows above `height`.
            void cutTo(int height);
            // Writes the art back into the menu's tiles, setting its textureRows. Art past the
            // tiles the menu has is cut off, which the log says. False when it was not read.
            bool apply();

        private:
            struct Picture;
            UObject* menu = nullptr;
            std::unique_ptr<Picture> picture;
            std::string what;
            std::vector<int> rows;
        };

        // Makes a menu taller by repeating the band of its background art that ends at row
        // `at`, `height` rows tall, straight after itself, and its ClientHeight with it.
        void growMenuBackground(UObject* menu, int at, int height);
        void insertMenuButton(UObject* menu, int index, const std::string& label, UClass* invoke);
        // Takes a button out of a menu's defaults: those below move up into its place, and its
        // band of the background art goes, so the menu is that much shorter.
        void removeMenuButton(UObject* menu, int index);
        // Swaps two of a menu's buttons, each keeping the other's place (its Y) and art.
        void swapMenuButtons(UObject* menu, int a, int b);
        // The index of the menu button that opens `invoke`, or -1.
        int findMenuButton(UObject* menu, UClass* invoke);
        // Takes rows (by index, in any order) out of an options screen's defaults the same way:
        // the choices below move up, and each row's band of art and its height go.
        void removeOptionRows(UObject* screen, std::vector<int> rows);
        // Takes out of an options screen's defaults, as removeOptionRows does, each choice whose
        // configSetting is a setting Ambrosia ignores (settings::Support::Ignored), such as the
        // Sample Rate and Decals choices.
        void removeIgnoredChoices(UObject* screen);
        // The same for every options screen: each loaded MenuUIScreenWindow class with choices.
        void removeIgnoredChoices();
        // A solid backdrop for the persona tab of the screen being shown, which the game marks
        // only by brighter text. Used as the extension's window backdrop.
        bool currentTabBackdrop(UObject* window, Color& fill, std::vector<extension::BackdropTile>& tiles);
        // Keeps the Display screen's Screen Resolution in step with the window. Deus Ex builds its
        // list once, when the screen opens, so toggling fullscreen there left it stale. Called
        // each frame with what the window is now; locked when the size can't be chosen, as in
        // borderless fullscreen.
        void syncResolutionChoice(const std::string& windowState, bool locked, int refreshRate);
        // Keeps the Sound screen's Audio Mixer from cycling to Raytraced: the choice offers it
        // only while it is the value shown, which is when the raytraced mode was already on as
        // the screen opened. Called each frame.
        void syncMixerChoice();
        void syncAimAssistChoice();
        void registerConversationLayout();
    }
}
