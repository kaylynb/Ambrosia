#include "deusex/game_state.h"
#include "deusex/known_logins.h"

#include <catch_amalgamated.hpp>

TEST_CASE("reading the datacube that gives away a door code makes that keypad's code known", "[gamepad]")
{
    dx::gamestate::restore({});
    CHECK(dx::knownlogins::knownAccounts("01_NYC_UNATCOIsland", "Keypad0").empty());
    dx::knownlogins::markRead("text:01_Datacube03");
    CHECK(dx::knownlogins::knownAccounts("01_NYC_UNATCOIsland", "Keypad0") == std::vector<std::string> { "" });
    // Map and machine names are matched whatever their case.
    CHECK_FALSE(dx::knownlogins::knownAccounts("01_nyc_unatcoisland", "keypad0").empty());
    dx::gamestate::restore({});
    CHECK(dx::knownlogins::knownAccounts("01_NYC_UNATCOIsland", "Keypad0").empty());
}

TEST_CASE("a conversation can give away a computer account", "[gamepad]")
{
    dx::gamestate::restore({});
    // Alex Jacobson's infolink about JC's office: "Login: JCD, password: bionicman."
    dx::knownlogins::markRead("con:DL_Office");
    CHECK(dx::knownlogins::knownAccounts("01_NYC_UNATCOHQ", "ComputerPersonal1") == std::vector<std::string> { "JCD" });
    dx::gamestate::restore({});
}

TEST_CASE("the game state comes back from its saved text", "[gamepad]")
{
    dx::gamestate::restore({});
    dx::gamestate::set("QuickSlots", "Up", "item:WeaponPistol");
    CHECK(dx::gamestate::add("Read", "text:01_datacube03"));
    CHECK_FALSE(dx::gamestate::add("Read", "text:01_datacube03"));
    std::string saved = dx::gamestate::save();
    dx::gamestate::restore({});
    CHECK_FALSE(dx::gamestate::has("Read", "text:01_datacube03"));
    dx::gamestate::restore(saved);
    CHECK(dx::gamestate::get("QuickSlots", "Up") == "item:WeaponPistol");
    CHECK(dx::gamestate::has("Read", "text:01_datacube03"));
    dx::gamestate::set("QuickSlots", "Up", "");
    CHECK_FALSE(dx::gamestate::has("QuickSlots", "Up"));
    dx::gamestate::restore({});
}
