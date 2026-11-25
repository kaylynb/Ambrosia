#include "game/pad_prompts.h"

#include <catch_amalgamated.hpp>

using dx::game::promptStyleFor;
using dx::glyphs::Style;

TEST_CASE("each kind of pad gets the prompts printed on it", "[pad_prompts]")
{
    struct Case {
        SDL_GamepadType type;
        Style style;
    };

    const Case cases[] = {
        { SDL_GAMEPAD_TYPE_XBOX360, Style::Xbox },
        { SDL_GAMEPAD_TYPE_XBOXONE, Style::Xbox },
        { SDL_GAMEPAD_TYPE_PS3, Style::PlayStation },
        { SDL_GAMEPAD_TYPE_PS4, Style::PlayStation },
        { SDL_GAMEPAD_TYPE_PS5, Style::PlayStation },
        { SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO, Style::Switch },
        { SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT, Style::Switch },
        { SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT, Style::Switch },
        { SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR, Style::Switch },
        { SDL_GAMEPAD_TYPE_GAMECUBE, Style::Xbox },
        { SDL_GAMEPAD_TYPE_STEAM, Style::Xbox },
        { SDL_GAMEPAD_TYPE_STANDARD, Style::Xbox },
        { SDL_GAMEPAD_TYPE_UNKNOWN, Style::Xbox },
    };
    for (const Case& each : cases) {
        CAPTURE(each.type);
        CHECK(promptStyleFor(each.type, each.type, SDL_GetGamepadButtonLabelForType(each.type, SDL_GAMEPAD_BUTTON_SOUTH)) == each.style);
    }
}

TEST_CASE("a pad's type as overridden wins over its own", "[pad_prompts]")
{
    // Steam Input shows a PlayStation pad as an Xbox 360 one and says through the override
    // what it really is.
    CHECK(promptStyleFor(SDL_GAMEPAD_TYPE_PS5, SDL_GAMEPAD_TYPE_XBOX360, SDL_GAMEPAD_BUTTON_LABEL_CROSS) == Style::PlayStation);
    CHECK(promptStyleFor(SDL_GAMEPAD_TYPE_XBOXONE, SDL_GAMEPAD_TYPE_PS4, SDL_GAMEPAD_BUTTON_LABEL_A) == Style::Xbox);
    // An override SDL cannot name leaves the pad's own type.
    CHECK(promptStyleFor(SDL_GAMEPAD_TYPE_STANDARD, SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO, SDL_GAMEPAD_BUTTON_LABEL_A) == Style::Switch);
}

TEST_CASE("a pad SDL cannot name is told by its bottom button's label", "[pad_prompts]")
{
    CHECK(promptStyleFor(SDL_GAMEPAD_TYPE_STANDARD, SDL_GAMEPAD_TYPE_STANDARD, SDL_GAMEPAD_BUTTON_LABEL_CROSS) == Style::PlayStation);
    CHECK(promptStyleFor(SDL_GAMEPAD_TYPE_UNKNOWN, SDL_GAMEPAD_TYPE_UNKNOWN, SDL_GAMEPAD_BUTTON_LABEL_B) == Style::Switch);
    CHECK(promptStyleFor(SDL_GAMEPAD_TYPE_STANDARD, SDL_GAMEPAD_TYPE_STANDARD, SDL_GAMEPAD_BUTTON_LABEL_A) == Style::Xbox);
    CHECK(promptStyleFor(SDL_GAMEPAD_TYPE_STANDARD, SDL_GAMEPAD_TYPE_STANDARD, SDL_GAMEPAD_BUTTON_LABEL_UNKNOWN) == Style::Xbox);
}

TEST_CASE("the prompts follow the pad used last", "[pad_prompts]")
{
    dx::game::PromptPads pads;
    CHECK_FALSE(pads.style());
    pads.connected(1, Style::Xbox);
    CHECK(pads.style() == Style::Xbox);

    SECTION("connecting a pad moves the prompts to it")
    {
        pads.connected(2, Style::PlayStation);
        CHECK(pads.style() == Style::PlayStation);
        // Using the first one again moves them back, and the second one back again.
        pads.used(1);
        CHECK(pads.style() == Style::Xbox);
        pads.used(2);
        CHECK(pads.style() == Style::PlayStation);
        pads.used(2);
        CHECK(pads.style() == Style::PlayStation);
    }
    SECTION("removing the pad in use hands the prompts to the one used before it")
    {
        pads.connected(2, Style::PlayStation);
        pads.connected(3, Style::Switch);
        pads.used(1);
        pads.used(3);
        CHECK(pads.style() == Style::Switch);
        pads.removed(3);
        CHECK(pads.style() == Style::Xbox);
        pads.removed(1);
        CHECK(pads.style() == Style::PlayStation);
        pads.removed(2);
        CHECK_FALSE(pads.style());
    }
    SECTION("pads not connected change nothing")
    {
        pads.used(9);
        pads.removed(9);
        CHECK(pads.style() == Style::Xbox);
    }
    SECTION("a pad connected again takes its new style")
    {
        pads.connected(2, Style::PlayStation);
        pads.connected(1, Style::Switch);
        CHECK(pads.style() == Style::Switch);
        pads.removed(1);
        CHECK(pads.style() == Style::PlayStation);
    }
}
