#include "engine/world.h"

#include "core/gc.h"
#include "core/objects.h"
#include "engine/actor.h"

#include <algorithm>
#include <cstdio>
#include <deque>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace dx {
    std::string World::travelString(UObject* player) const
    {
        std::string out;
        std::deque<UObject*> queue { player };
        std::unordered_set<UObject*> seen { player };
        while (!queue.empty()) {
            UObject* object = queue.front();
            queue.pop_front();
            char header[512];
            std::snprintf(header, sizeof(header), "Class=%s Name=%s Ptr=%08x\r\n{\r\n", object->cls->pathName().c_str(), object->name.c_str(), object->handle);
            out += header;
            for (UProperty* property : object->cls->properties) {
                if (!(property->propertyFlags & CPF_Travel)) {
                    continue;
                }
                for (int32_t i = 0; i < property->arrayDim; i++) {
                    size_t offset = property->offset + static_cast<size_t>(i) * property->elementSize();
                    if (object->cls->defaultObject && property->equals(object->data + offset, object->cls->defaultObject->data + offset)) {
                        continue;
                    }
                    std::string text;
                    if (property->type == PropType::Object) {
                        UObject* value = *reinterpret_cast<UObject**>(object->data + offset);
                        if (value) {
                            char pointer[16];
                            std::snprintf(pointer, sizeof(pointer), "%08x ", value->handle);
                            text = pointer + value->name.str();
                            if (seen.insert(value).second) {
                                queue.push_back(value);
                            }
                        } else {
                            text = "None";
                        }
                    } else {
                        text = property->toText(object->data + offset);
                        std::replace(text.begin(), text.end(), '\n', '\\');
                    }
                    out += property->name.str();
                    if (property->arrayDim > 1) {
                        out += "[" + std::to_string(i) + "]";
                    }
                    out += "=" + text + "\r\n";
                }
            }
            out += "}\r\n";
        }
        return out;
    }

    void addTravelReferences(std::vector<TravelActor>& travel, ReferenceVisitor& references)
    {
        for (TravelActor& entry : travel) {
            references(entry.cls);
            references(entry.original);
            for (TravelValue& value : entry.values) {
                references(value.property);
                if (value.property) {
                    gc::addValueReferences(value.property, value.data.data(), references);
                }
            }
        }
    }

    PendingTravel::PendingTravel(PendingTravel&& other) noexcept
        : actors(std::exchange(other.actors, {}))
    {
    }

    PendingTravel& PendingTravel::operator=(PendingTravel&& other) noexcept
    {
        if (this != &other) {
            World::releaseTravel(actors);
            actors = std::exchange(other.actors, {});
        }
        return *this;
    }

    PendingTravel::~PendingTravel()
    {
        World::releaseTravel(actors);
    }

    void PendingTravel::addReferences(ReferenceVisitor& references)
    {
        addTravelReferences(actors, references);
    }

    std::vector<TravelActor> PendingTravel::take()
    {
        return std::exchange(actors, {});
    }

    PendingTravel World::collectTravel()
    {
        PendingTravel pending;
        std::vector<TravelActor>& result = pending.actors;
        UObject* player = localPlayer;
        if (!player || player->destroyed) {
            return pending;
        }
        std::vector<UObject*> travellers;
        std::deque<UObject*> queue { player };
        std::unordered_set<UObject*> seen { player };
        while (!queue.empty()) {
            UObject* object = queue.front();
            queue.pop_front();
            bool isActor = object->isA(engineClasses().actor);
            if (isActor && !actor::bTravel(object)) {
                continue;
            }
            if (isActor && !object->destroyed) {
                travellers.push_back(object);
            }
            for (UProperty* property : object->cls->properties) {
                if (!(property->propertyFlags & CPF_Travel) || property->type != PropType::Object) {
                    continue;
                }
                for (int32_t i = 0; i < property->arrayDim; i++) {
                    size_t offset = property->offset + static_cast<size_t>(i) * property->elementSize();
                    if (object->cls->defaultObject && property->equals(object->data + offset, object->cls->defaultObject->data + offset)) {
                        continue;
                    }
                    UObject* value = *reinterpret_cast<UObject**>(object->data + offset);
                    if (value && seen.insert(value).second) {
                        queue.push_back(value);
                    }
                }
            }
        }
        for (UObject* a : travellers) {
            TravelActor entry;
            entry.cls = a->cls;
            entry.original = a;
            entry.isPlayer = a == player;
            for (UProperty* property : a->cls->properties) {
                if (!(property->propertyFlags & CPF_Travel)) {
                    continue;
                }
                for (int32_t i = 0; i < property->arrayDim; i++) {
                    size_t offset = property->offset + static_cast<size_t>(i) * property->elementSize();
                    if (a->cls->defaultObject && property->equals(a->data + offset, a->cls->defaultObject->data + offset)) {
                        continue;
                    }
                    TravelValue value;
                    value.property = property;
                    value.index = i;
                    value.data.resize(property->elementSize() + 16);
                    property->construct(value.data.data());
                    property->copy(value.data.data(), a->data + offset);
                    entry.values.push_back(std::move(value));
                }
            }
            result.push_back(std::move(entry));
        }
        return pending;
    }

    void World::releaseTravel(std::vector<TravelActor>& travel)
    {
        for (TravelActor& entry : travel) {
            for (TravelValue& value : entry.values) {
                if (value.property) {
                    value.property->destroy(value.data.data());
                }
            }
        }
        travel.clear();
    }

    std::vector<UObject*> World::applyTravel(UObject* player)
    {
        std::vector<UObject*> accepted;
        if (incomingTravel.empty()) {
            return accepted;
        }
        std::unordered_map<UObject*, UObject*> remap;
        std::vector<std::pair<TravelActor*, UObject*>> targets;
        for (TravelActor& entry : incomingTravel) {
            UObject* target = nullptr;
            if (entry.isPlayer) {
                target = player;
            } else if (entry.cls) {
                target = spawn(entry.cls, player, Name(), actor::Location(player), actor::Rotation(player), nullptr, true);
                if (target) {
                    accepted.push_back(target);
                }
            }
            if (target) {
                // A collection sets the original to None once it is deleted; None references
                // must not be taken for it.
                if (entry.original) {
                    remap[entry.original] = target;
                }
                targets.emplace_back(&entry, target);
            }
        }
        for (auto& [entry, target] : targets) {
            for (TravelValue& value : entry->values) {
                UProperty* property = value.property;
                if (!property || !target->cls->isChildOf(cast<UStruct>(property->outer))) {
                    continue;
                }
                uint8_t* destination = target->data + property->offset + static_cast<size_t>(value.index) * property->elementSize();
                if (property->type == PropType::Object) {
                    UObject* reference = *reinterpret_cast<UObject**>(value.data.data());
                    auto mapped = remap.find(reference);
                    if (mapped != remap.end()) {
                        reference = mapped->second;
                    } else if (reference && reference->isA(engineClasses().actor)) {
                        reference = nullptr;
                    }
                    *reinterpret_cast<UObject**>(destination) = reference;
                } else {
                    property->copy(destination, value.data.data());
                }
            }
        }
        rehomeCarried(targets, remap);
        releaseTravel(incomingTravel);
        return accepted;
    }

    // Travel carries objects that are not actors (notes, goals, keys, conversation history) by
    // reference, and script made most of them inside the player that added them (new(Self)), so
    // their outer is an actor of the level being left. The original keeps that whole level alive
    // through them; Ambrosia moves them under the actor's copy here, or under the transient package
    // when that actor did not travel, so the old level can go. Only what travel values reach is
    // looked through, staying inside the old level's package.
    void World::rehomeCarried(const std::vector<std::pair<TravelActor*, UObject*>>& targets, const std::unordered_map<UObject*, UObject*>& remap)
    {
        std::unordered_set<const UObject*> packages;
        for (const auto& [entry, target] : targets) {
            if (entry->original && !gc::wasFreed(entry->original)) {
                packages.insert(entry->original->outermost());
            }
        }
        if (packages.empty()) {
            return;
        }
        UClass* actorClass = engineClasses().actor;
        auto isActor = [&](const UObject* object) {
            return actorClass && object->cls && object->isA(actorClass);
        };
        // A level restarted on the same map has the same package object, so the old level's
        // actors are told apart from this one's by the actor list.
        std::unordered_set<const UObject*> here(actors.begin(), actors.end());
        auto oldActor = [&](const UObject* object) {
            return isActor(object) && !here.count(object);
        };
        // Whether a reference is worth following: a live object of the old level, not an actor.
        auto carried = [&](UObject* object) {
            if (!object || gc::wasFreed(object) || objects::atHandle(object->handle) != object || isActor(object)) {
                return false;
            }
            return packages.count(object->outermost()) != 0;
        };

        struct Collect final : ReferenceVisitor {
            std::vector<UObject*> found;

            void visit(UObject*& reference) override
            {
                if (reference) {
                    found.push_back(reference);
                }
            }
        };

        std::deque<UObject*> queue;
        std::unordered_set<UObject*> seen;
        auto offer = [&](UObject* object) {
            if (carried(object) && seen.insert(object).second) {
                queue.push_back(object);
            }
        };
        for (const auto& [entry, target] : targets) {
            Collect values;
            gc::addPropertyReferences(target->cls, target->data, values);
            for (UObject* object : values.found) {
                offer(object);
            }
        }
        UObject* transient = nullptr;
        while (!queue.empty()) {
            UObject* object = queue.front();
            queue.pop_front();
            // The link to move is the one that hangs from an actor of the old level.
            UObject* link = object;
            int guard = 0;
            while (link->outer && !isActor(link->outer) && guard++ < 64) {
                link = link->outer;
            }
            if (link->outer && oldActor(link->outer)) {
                auto moved = remap.find(link->outer);
                UObject* home = moved != remap.end() ? moved->second : nullptr;
                if (!home) {
                    if (!transient) {
                        transient = objects::packageObject(Name("Transient"));
                    }
                    home = transient;
                }
                link->outer = home;
                objects::syncObjectProperties(link);
            }
            Collect references;
            object->addReferences(references);
            for (UObject* next : references.found) {
                offer(next);
            }
        }
    }
}
