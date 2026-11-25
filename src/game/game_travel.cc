#include "game/game.h"

#include "core/log.h"
#include "core/settings.h"
#include "core/objects.h"
#include "core/profile.h"
#include "core/url.h"
#include "core/vm.h"
#include "deusex/map_lookup.h"
#include "deusex/quickslots.h"
#include "engine/actor.h"
#include "engine/console.h"
#include "engine/level_release.h"
#include "engine/savegame.h"
#include "engine/world.h"
#include "extension/extension.h"
#include "render/scene_renderer.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace dx::game {
    bool Game::openMap(const std::string& url, bool carryItems)
    {
        DX_ZONE_NAMED("Open map");
        DX_MESSAGE(url);
        // Held where the collector sees it until the new world takes it, since storing the
        // departing level deletes destroyed actors that travel values may still name.
        dx::PendingTravel travel;
        std::vector<std::pair<std::string, std::string>> travelInfo;
        if (carryItems && world) {
            travel = world->collectTravel();
            dx::UObject* player = world->player();
            if (player) {
                dx::UObject* replication = dx::pawn::PlayerReplicationInfo(player);
                std::string key = replication ? dx::playerreplicationinfo::PlayerName(replication) : std::string("Player");
                travelInfo.emplace_back(key, world->travelString(player));
            }
        }
        std::string target = url;
        if (world && !target.empty() && target[0] == '?') {
            target = world->url.substr(0, world->url.find('?')) + target;
        }
        int32_t loadIndex = 0;
        bool loading = dx::savegame::loadRequested(target, loadIndex);
        if (loading) {
            std::string map;
            if (!dx::savegame::prepareLoad(loadIndex, map)) {
                dx::log::error("DevLoad", "cannot load saved game {}", loadIndex);
                return false;
            }
            target = map + "?loadgame=" + std::to_string(loadIndex);
        } else if (world) {
            // Stay on the current level rather than leave it for a map that is not there.
            std::string name = dx::maplookup::mapName(target);
            if (!dx::maplookup::exists(name) && SDL_strcasecmp(name.c_str(), fileMap.c_str()) != 0) {
                dx::log::error("DevLoad", "map not found: {}", target);
                return false;
            }
            DX_ZONE_NAMED("Store departing level");
            dx::savegame::storeDepartingLevel(*world, target);
        }
        bool leaving = world != nullptr;
        processBeforeTravel = processMemoryBytes();
        if (loading || world) {
            dx::savegame::selectLevel(target);
        }
        {
            DX_ZONE_NAMED("Paint loading");
            paintLoading();
        }
        {
            DX_ZONE_NAMED("Flush render level");
            dx::extension::reset();
            renderer->flushLevel();
        }
        std::vector<std::pair<std::string, std::string>> previousTravelInfo;
        if (world && world->level) {
            previousTravelInfo = world->level->travelInfo;
        }
        // UGameEngine::LoadMap tells the viewport's console of the level
        // change (NotifyLevelChange) before it lets go of the old player; the
        // viewport and console themselves stay for the whole session.
        if (world && world->level && viewport) {
            if (dx::UObject* console = dx::player::Console(viewport)) {
                dx::vm::event(console, dx::Name("NotifyLevelChange"));
            }
        }
        if (world && world->level) {
            dx::objects::unloadPackage(world->level->outermost()->name);
        }
        {
            DX_ZONE_NAMED("Delete old world");
            world.reset();
        }
        world = std::make_unique<dx::World>();
        world->viewport = viewport;
        world->incomingTravel = travel.take();
        if (!loading) {
            bool restart = target.find("?restart") != std::string::npos || target.find("?RESTART") != std::string::npos;
            if (restart) {
                travelInfo = previousTravelInfo;
            }
            world->arrivalTravelInfo = std::move(travelInfo);
            world->replaceTravelInfo = true;
        }
        world->inputHook = [](dx::UObject* player, float delta) {
            dx::console::readInput(player, delta);
        };
        bool loaded = world->loadMap(target);
        viewport = world->viewport;
        if (!loaded) {
            if (leaving) {
                returnToEntryMap(target);
            }
            return false;
        }
        // The old level's objects go now that the new one is up, as LoadMap's garbage
        // collection does. GarbageCollection=False under [Engine.GameEngine] keeps them.
        dx::World::releaseTravel(world->incomingTravel);
        pad.levelChanged();
        dx::levelrelease::setEnabled(dx::settings::get(dx::settings::garbageCollection));
        // On in the ini turns it on (the overlay's Memory window saves it there); turning it off
        // is the window's or the console's, so a run that turned it on keeps it.
        if (dx::settings::get(dx::settings::keepFreedObjects)) {
            dx::gc::setKeepFreed(true);
        }
        {
            DX_ZONE_NAMED("Release old level");
            dx::levelrelease::collect(world.get());
        }
        logLevelMemory(dx::maplookup::mapName(target));
        if (loading) {
            world->url = world->level->url;
            // Keep item assignments even when the save has none in stock.
            dx::quickslots::prune(world->player());
        }
        dx::console::resetInput(world->player());
        return true;
    }

    std::string Game::entryMap()
    {
        return dx::settings::get(dx::settings::localMap);
    }

    // A level that fails to load after the old one is gone leaves nothing to play, so go
    // to the title map instead.
    void Game::returnToEntryMap(const std::string& failed)
    {
        std::string entry = entryMap();
        dx::log::error("DevLoad", "cannot open {}, returning to {}", failed, entry);
        dx::World::releaseTravel(world->incomingTravel);
        if (world->level) {
            dx::objects::unloadPackage(world->level->outermost()->name);
        }
        world.reset();
        if (dx::maplookup::mapName(failed) != dx::maplookup::mapName(entry)) {
            dx::savegame::selectLevel(entry);
            world = std::make_unique<dx::World>();
            world->viewport = viewport;
            world->inputHook = [](dx::UObject* player, float delta) {
                dx::console::readInput(player, delta);
            };
            bool loaded = world->loadMap(entry);
            viewport = world->viewport;
            if (loaded) {
                dx::console::resetInput(world->player());
                return;
            }
            world.reset();
        }
        dx::log::error("DevLoad", "cannot open {}, quitting", entry);
        running = false;
    }

    // UGameEngine::PaintProgress: one frame of the world with the
    // screen flash cleared and scaled to 0.1, shown at once. The console's PostRender prints
    // LOADING or SAVING over it from the level's LevelAction.
    void Game::paintProgress()
    {
        if (!world) {
            return;
        }
        int width = 0;
        int height = 0;
        SDL_GetWindowSizeInPixels(window, &width, &height);
        progressFrame = true;
        render(std::max(width, 1), std::max(height, 1));
        progressFrame = false;
        renderer->applyBrightness(dx::settings::get(dx::settings::brightness));
        SDL_GL_SwapWindow(window);
    }

    // UGameEngine::LoadMap before it leaves a level: the level says
    // it is loading, any pause is lifted, the player's menu flag is cleared so the console
    // does not hold the message back, and the frame is painted.
    void Game::paintLoading()
    {
        if (!world || !world->levelInfo || !world->levelInfo->data) {
            return;
        }
        dx::levelinfo::LevelAction(world->levelInfo) = 1;
        dx::levelinfo::Pauser(world->levelInfo).clear();
        if (dx::UObject* player = world->player()) {
            dx::playerpawn::bShowMenu(player) = 0;
        }
        paintProgress();
        dx::levelinfo::LevelAction(world->levelInfo) = 0;
    }

    void Game::handleTravel()
    {
        if (!pendingMap.empty()) {
            std::string url = dx::travelUrl(world ? world->url : std::string(), pendingMap, pendingTravelType);
            pendingMap.clear();
            openMap(url);
            return;
        }
        if (world && world->travelPending) {
            std::string url = dx::travelUrl(world->url, world->travelUrl, world->travelType);
            bool items = world->travelItems;
            world->travelPending = false;
            openMap(url, items);
        }
    }
}
