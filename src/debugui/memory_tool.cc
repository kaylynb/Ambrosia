#include "debugui/memory_tool.h"
#include "core/settings.h"
#include "debugui/imgui_setup.h"

#include "core/gc.h"
#include "core/name.h"
#include "core/objects.h"
#include "core/text.h"

#include <imgui.h>
#include <implot.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace dx::debugui {
    namespace {
        struct ClassCount {
            std::string name;
            std::string package;
            size_t objects = 0;
            size_t destroyed = 0;
            // Script property data only: what the class's size says each object holds.
            uint64_t bytes = 0;
        };

        struct Snapshot {
            std::vector<ClassCount> classes;
            size_t table = 0;
            size_t objects = 0;
            size_t destroyed = 0;
            uint64_t bytes = 0;
            size_t names = 0;
        };

        struct PlotState {
            int seconds = 120;
            bool follow = true;
            // Series rebuilt from the samples each frame the plot is shown.
            std::vector<double> times;
            std::vector<double> live;
            std::vector<double> allocated;
            std::vector<double> freed;
            std::vector<double> levels;
        };

        struct Memory {
            Snapshot snapshot;
            bool live = true;
            int top = 40;
            char filter[64] = "";
            std::chrono::steady_clock::time_point taken;
            bool takenOnce = false;
            // The sort the table asked for last: column and direction.
            int sortColumn = 2;
            bool ascending = false;
            PlotState plot;
        };

        Snapshot take()
        {
            Snapshot snapshot;
            std::unordered_map<const UClass*, size_t> index;
            const std::vector<UObject*>& table = objects::all();
            snapshot.table = table.size();
            for (const UObject* object : table) {
                if (!object) {
                    continue;
                }
                snapshot.objects++;
                uint64_t size = object->data && object->cls ? object->cls->size : 0;
                snapshot.bytes += size;
                snapshot.destroyed += object->destroyed ? 1u : 0u;
                auto [found, added] = index.emplace(object->cls, snapshot.classes.size());
                if (added) {
                    ClassCount count;
                    count.name = object->cls ? object->cls->name.str() : std::string("(no class)");
                    count.package = object->cls && object->cls->outer ? object->cls->outer->name.str() : std::string();
                    snapshot.classes.push_back(std::move(count));
                }
                ClassCount& count = snapshot.classes[found->second];
                count.objects++;
                count.destroyed += object->destroyed ? 1u : 0u;
                count.bytes += size;
            }
            snapshot.names = Name::count();
            return snapshot;
        }

        void sort(Memory& memory)
        {
            int column = memory.sortColumn;
            bool ascending = memory.ascending;
            std::stable_sort(memory.snapshot.classes.begin(), memory.snapshot.classes.end(), [column, ascending](const ClassCount& a, const ClassCount& b) {
                auto less = [column](const ClassCount& x, const ClassCount& y) {
                    switch (column) {
                    case 0:
                        return x.name < y.name;
                    case 1:
                        return x.package < y.package;
                    case 3:
                        return x.destroyed < y.destroyed;
                    case 4:
                        return x.bytes < y.bytes;
                    default:
                        return x.objects < y.objects;
                    }
                };
                return ascending ? less(a, b) : less(b, a);
            });
        }

        std::string kilobytes(uint64_t bytes)
        {
            char text[32];
            std::snprintf(text, sizeof(text), "%.1f KB", static_cast<double>(bytes) / 1024.0);
            return text;
        }

        std::string megabytes(uint64_t bytes)
        {
            char text[32];
            std::snprintf(text, sizeof(text), "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
            return text;
        }

        void collector()
        {
            gc::Stats stats = gc::stats();
            ImGui::Text("Garbage collector: %s", gc::enabled() ? "on" : "off (GarbageCollection=False)");
            if (stats.collections == 0) {
                ImGui::TextDisabled("Level-change collections: none yet");
            } else {
                ImGui::Text("Level-change collections: %zu, %zu objects freed, %.2f ms each on average; the last freed %zu in %.2f ms and set %zu references to freed memory to None", stats.collections, stats.collectedObjects, stats.collectMilliseconds / static_cast<double>(stats.collections), stats.lastCollectedObjects, stats.lastCollectMilliseconds, stats.lastDanglingCleared);
            }
            if (stats.frees == 0) {
                ImGui::TextDisabled("Freed during play: nothing yet");
            } else {
                ImGui::Text("Freed during play: %zu objects in %zu calls, %.3f ms each on average, longest %.3f ms this level", stats.freedObjects, stats.frees, stats.freeMilliseconds / static_cast<double>(stats.frees), stats.longestFreeMilliseconds);
            }
            if (stats.releases != 0) {
                ImGui::Text("Released with references cleared (tests, tools): %zu, %zu objects, %.3f ms each on average", stats.releases, stats.releasedObjects, stats.releaseMilliseconds / static_cast<double>(stats.releases));
            }
            bool keep = gc::keepFreed();
            if (ImGui::Checkbox("Keep freed objects until the level change", &keep)) {
                gc::setKeepFreed(keep);
                settings::set(settings::keepFreedObjects, keep);
            }
            itemHelp("Windows, actors and AI events freed during play are kept, marked and cleared at the next level change, and each script read of one is logged: for finding such reads, and as a workaround for a crash they cause. Saved as KeepFreedObjects under [Engine.GameEngine].");
            if (keep) {
                ImGui::SameLine();
                ImGui::TextDisabled("%zu held", gc::keptFreedObjects());
            }
        }

        // What has been freed during play, by class: where the churn is.
        void freedTable()
        {
            std::vector<gc::ClassCount> freed = gc::freedByClass();
            if (freed.empty()) {
                return;
            }
            if (!ImGui::CollapsingHeader("Freed during play, by class")) {
                return;
            }
            ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit;
            if (!ImGui::BeginTable("freed", 3, flags, ImVec2(0, 160))) {
                return;
            }
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Class", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Freed");
            ImGui::TableSetupColumn("Script data");
            ImGui::TableHeadersRow();
            for (const gc::ClassCount& count : freed) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(count.className.c_str());
                ImGui::TableNextColumn();
                ImGui::Text("%zu", count.freed);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(kilobytes(count.freedBytes).c_str());
            }
            ImGui::EndTable();
        }

        // Script data the object table holds over time, with how fast it is made and deleted,
        // and a line at each level change.
        void memoryPlot(PlotState& plot)
        {
            gc::DataTotals totals = gc::dataTotals();
            constexpr double MB = 1024.0 * 1024.0;
            ImGui::Text("Script data: %.1f MB live; %.1f MB made in %llu blocks, %.1f MB deleted in %llu, since start", static_cast<double>(totals.allocatedBytes - totals.freedBytes) / MB, static_cast<double>(totals.allocatedBytes) / MB, static_cast<unsigned long long>(totals.allocations), static_cast<double>(totals.freedBytes) / MB, static_cast<unsigned long long>(totals.frees));
            if (!ImGui::CollapsingHeader("Script data over time", ImGuiTreeNodeFlags_DefaultOpen)) {
                return;
            }
            ImGui::Checkbox("Follow", &plot.follow);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(200);
            ImGui::SliderInt("Seconds shown", &plot.seconds, 10, 600);

            const std::vector<gc::MemorySample>& samples = gc::memorySamples();
            plot.times.clear();
            plot.live.clear();
            plot.allocated.clear();
            plot.freed.clear();
            for (size_t i = 0; i < samples.size(); i++) {
                const gc::MemorySample& sample = samples[i];
                plot.times.push_back(sample.seconds);
                plot.live.push_back(static_cast<double>(sample.allocatedBytes - sample.freedBytes) / MB);
                // Rates over the interval since the sample before, in KB a second.
                double made = 0;
                double deleted = 0;
                if (i > 0) {
                    double interval = std::max(sample.seconds - samples[i - 1].seconds, 1e-3);
                    made = static_cast<double>(sample.allocatedBytes - samples[i - 1].allocatedBytes) / 1024.0 / interval;
                    deleted = static_cast<double>(sample.freedBytes - samples[i - 1].freedBytes) / 1024.0 / interval;
                }
                plot.allocated.push_back(made);
                plot.freed.push_back(deleted);
            }
            plot.levels.clear();
            for (const gc::LevelMemory& level : gc::levelMemory()) {
                plot.levels.push_back(level.seconds);
            }
            if (!ImPlot::BeginPlot("##memory", ImVec2(-1, 220))) {
                return;
            }
            ImPlot::SetupAxis(ImAxis_X1, "seconds");
            ImPlot::SetupAxis(ImAxis_Y1, "live MB", ImPlotAxisFlags_AutoFit);
            ImPlot::SetupAxis(ImAxis_Y2, "KB/s", ImPlotAxisFlags_AuxDefault | ImPlotAxisFlags_AutoFit);
            if (plot.follow) {
                // The last `seconds`, or the start of the clock until there is that much.
                double now = plot.times.empty() ? gc::memoryClock() : plot.times.back();
                double start = std::max(0.0, now - plot.seconds);
                ImPlot::SetupAxisLimits(ImAxis_X1, start, start + plot.seconds, ImPlotCond_Always);
            }
            ImPlot::SetupLegend(ImPlotLocation_NorthWest, ImPlotLegendFlags_Horizontal);
            int count = static_cast<int>(plot.times.size());
            if (count > 0) {
                ImPlot::SetAxes(ImAxis_X1, ImAxis_Y1);
                ImPlot::PlotShaded("live", plot.times.data(), plot.live.data(), count, 0.0, ImPlotSpec(ImPlotProp_FillAlpha, 0.25f));
                ImPlot::PlotLine("live", plot.times.data(), plot.live.data(), count);
                ImPlot::SetAxes(ImAxis_X1, ImAxis_Y2);
                ImPlot::PlotLine("made", plot.times.data(), plot.allocated.data(), count);
                ImPlot::PlotLine("deleted", plot.times.data(), plot.freed.data(), count);
            }
            if (!plot.levels.empty()) {
                ImPlot::SetAxes(ImAxis_X1, ImAxis_Y1);
                ImPlot::PlotInfLines("level change", plot.levels.data(), static_cast<int>(plot.levels.size()));
                for (const gc::LevelMemory& level : gc::levelMemory()) {
                    ImPlot::TagX(level.seconds, ImVec4(0.9f, 0.7f, 0.2f, 1.0f), "%s", level.map.c_str());
                }
            }
            ImPlot::EndPlot();
        }

        // A row per level change, as the log's memlog lines give them.
        void levelHistory()
        {
            const std::vector<gc::LevelMemory>& levels = gc::levelMemory();
            if (levels.empty()) {
                return;
            }
            if (!ImGui::CollapsingHeader("Level changes", ImGuiTreeNodeFlags_DefaultOpen)) {
                return;
            }
            ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit;
            if (!ImGui::BeginTable("levels", 8, flags, ImVec2(0, 160))) {
                return;
            }
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Map", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Process");
            ImGui::TableSetupColumn("Objects");
            ImGui::TableSetupColumn("Script data");
            ImGui::TableSetupColumn("Collected");
            ImGui::TableSetupColumn("Collect ms");
            ImGui::TableSetupColumn("Freed in level");
            ImGui::TableSetupColumn("Longest free ms");
            ImGui::TableHeadersRow();
            for (auto it = levels.rbegin(); it != levels.rend(); ++it) {
                const gc::LevelMemory& level = *it;
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(level.map.c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(level.processBytes ? megabytes(level.processBytes).c_str() : "?");
                ImGui::TableNextColumn();
                ImGui::Text("%zu", level.objects);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(kilobytes(level.scriptBytes).c_str());
                ImGui::TableNextColumn();
                ImGui::Text("%zu", level.collected);
                ImGui::TableNextColumn();
                ImGui::Text("%.1f", level.collectMilliseconds);
                ImGui::TableNextColumn();
                ImGui::Text("%zu", level.freedInLevel);
                ImGui::TableNextColumn();
                ImGui::Text("%.3f", level.longestFreeMilliseconds);
            }
            ImGui::EndTable();
        }

        void draw(Memory& memory)
        {
            auto now = std::chrono::steady_clock::now();
            bool refresh = ImGui::Button("Refresh");
            ImGui::SameLine();
            ImGui::Checkbox("Every second", &memory.live);
            if (refresh || !memory.takenOnce || (memory.live && now - memory.taken >= std::chrono::seconds(1))) {
                memory.snapshot = take();
                memory.taken = now;
                memory.takenOnce = true;
                sort(memory);
            }
            const Snapshot& snapshot = memory.snapshot;
            ImGui::Text("Objects: %zu (%zu destroyed, not yet freed) in a table of %zu; script data %s", snapshot.objects, snapshot.destroyed, snapshot.table, kilobytes(snapshot.bytes).c_str());
            ImGui::Text("Names: %zu", snapshot.names);
            collector();
            memoryPlot(memory.plot);
            levelHistory();
            freedTable();
            ImGui::Separator();
            ImGui::SetNextItemWidth(scaled(160));
            ImGui::SliderInt("Classes shown", &memory.top, 5, 500);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(scaled(200));
            ImGui::InputText("Filter", memory.filter, sizeof(memory.filter));
            ImGui::SameLine();
            ImGui::TextDisabled("%zu classes", snapshot.classes.size());
            ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Sortable | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingFixedFit;
            if (!ImGui::BeginTable("classes", 5, flags, ImVec2(0, 0))) {
                return;
            }
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Class", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Package");
            ImGui::TableSetupColumn("Objects", ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_PreferSortDescending);
            ImGui::TableSetupColumn("Destroyed", ImGuiTableColumnFlags_PreferSortDescending);
            ImGui::TableSetupColumn("Script data", ImGuiTableColumnFlags_PreferSortDescending);
            ImGui::TableHeadersRow();
            if (ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs()) {
                if (specs->SpecsDirty && specs->SpecsCount > 0) {
                    memory.sortColumn = specs->Specs[0].ColumnIndex;
                    memory.ascending = specs->Specs[0].SortDirection == ImGuiSortDirection_Ascending;
                    sort(memory);
                    specs->SpecsDirty = false;
                }
            }
            std::string filter = lowered(memory.filter);
            int shown = 0;
            for (const ClassCount& count : snapshot.classes) {
                if (shown >= memory.top) {
                    break;
                }
                if (!filter.empty() && lowered(count.name).find(filter) == std::string::npos && lowered(count.package).find(filter) == std::string::npos) {
                    continue;
                }
                shown++;
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(count.name.c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(count.package.c_str());
                ImGui::TableNextColumn();
                ImGui::Text("%zu", count.objects);
                ImGui::TableNextColumn();
                if (count.destroyed) {
                    ImGui::Text("%zu", count.destroyed);
                }
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(kilobytes(count.bytes).c_str());
            }
            ImGui::EndTable();
        }
    }

    void addMemoryTool(Overlay& overlay)
    {
        auto memory = std::make_shared<Memory>();
        Tool tool;
        tool.menu = "Engine";
        tool.title = "Memory";
        tool.name = "memory";
        tool.width = 720;
        tool.height = 560;
        tool.draw = [memory](const Frame&) {
            draw(*memory);
        };
        overlay.addTool(std::move(tool));
    }
}
