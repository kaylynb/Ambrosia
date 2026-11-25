#include "debugui/performance_tool.h"
#include "debugui/imgui_setup.h"

#include "core/config.h"
#include "core/log.h"
#include "core/profile.h"
#include "core/settings.h"
#include "render/gpu_timers.h"

#include <imgui.h>
#include <implot.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace dx::debugui {
    namespace {
        struct Performance {
            // Who asked for the recorder: the open tool, or the console's switch.
            bool toolRecording = false;
            bool consoleRecording = false;
            // The tool's own pause, which keeps it from turning the recorder on.
            bool paused = false;
            bool sinceStart = false;
            float graphMs = 50;
            int sortColumn = -1;
            bool ascending = false;
            std::string lastFile;
            std::string lastError;
            int recordSeconds = 20;
        };

        enum Column {
            ColumnZone,
            ColumnThread,
            ColumnMean,
            ColumnWorst,
            ColumnShare,
            ColumnCalls,
        };

        // To a path performanceFile gave, which is not resolved again: on the Switch a full path
        // such as sdmc:/switch/ambrosia/user/Profiles/Frames0000.csv has no root directory, so it
        // would not count as absolute and would go under the Profiles folder a second time.
        std::string write(Performance& performance, const std::string& path)
        {
            std::string error;
            if (!profile::writeCsv(path, error)) {
                performance.lastError = error;
                return error;
            }
            performance.lastFile = path;
            performance.lastError.clear();
            return "wrote " + path;
        }

        void stopToolRecording(Performance& performance)
        {
            double progress = 0;
            if (performance.toolRecording && !performance.consoleRecording && !profile::timedRecording(progress)) {
                profile::setRecording(false);
            }
            performance.toolRecording = false;
        }

        void graph(Performance& performance, const profile::Summary& summary)
        {
            size_t frames = profile::historySize();
            ImGui::Text("Frame: mean %.2f ms (%.1f fps), worst %.2f ms, 1%% low %.2f ms (%.1f fps), over %zu frames", summary.meanFrameMs, summary.meanFrameMs > 0 ? 1000.0 / summary.meanFrameMs : 0.0, summary.worstFrameMs, summary.onePercentLowMs, summary.onePercentLowMs > 0 ? 1000.0 / summary.onePercentLowMs : 0.0, summary.frames);
            ImGui::SetNextItemWidth(scaled(160));
            ImGui::SliderFloat("Graph top (ms)", &performance.graphMs, 5.0f, 200.0f, "%.0f");
            // ImPlot, as the Memory window's plot: frame times in milliseconds, with lines at 60,
            // 30 and 20 frames a second.
            if (ImPlot::BeginPlot("##frames", ImVec2(-1, scaled(120)), ImPlotFlags_NoLegend | ImPlotFlags_NoMenus | ImPlotFlags_NoBoxSelect)) {
                ImPlot::SetupAxis(ImAxis_X1, nullptr, ImPlotAxisFlags_NoTickLabels);
                ImPlot::SetupAxis(ImAxis_Y1, "ms");
                ImPlot::SetupAxisLimits(ImAxis_X1, 0, static_cast<double>(std::max<size_t>(frames, 1)), ImPlotCond_Always);
                ImPlot::SetupAxisLimits(ImAxis_Y1, 0, static_cast<double>(performance.graphMs), ImPlotCond_Always);
                static const double rates[] = { 1000.0 / 60.0, 1000.0 / 30.0, 1000.0 / 20.0 };
                ImPlot::PlotInfLines("fps", rates, 3, ImPlotSpec(ImPlotProp_Flags, ImPlotInfLinesFlags_Horizontal, ImPlotProp_LineColor, ImVec4(1, 1, 1, 0.25f)));
                auto frameMs = [](int index, void*) {
                    return ImPlotPoint(index, static_cast<double>(profile::historyFrame(static_cast<size_t>(index)).frameMs));
                };
                ImPlot::PlotLineG("frame", frameMs, nullptr, static_cast<int>(frames));
                ImPlot::EndPlot();
            }
            itemHelp("Frame times; the faint lines are 16.7 ms (60 fps), 33.3 ms (30 fps) and 50 ms (20 fps).");
            if (frames) {
                ImGui::TextDisabled("last %.2f ms", static_cast<double>(profile::historyFrame(frames - 1).frameMs));
            }
        }

        double sortKey(const profile::ZoneStats& stats, int column)
        {
            switch (column) {
            case ColumnWorst:
                return stats.worstMs;
            case ColumnShare:
                return stats.share;
            case ColumnCalls:
                return stats.callsPerFrame;
            default:
                return stats.meanMs;
            }
        }

        void zoneRows(const Performance& performance, const std::vector<profile::ZoneInfo>& zones, const std::vector<std::vector<int>>& children, const profile::Summary& summary, const std::vector<int>& siblings)
        {
            std::vector<int> order = siblings;
            int column = performance.sortColumn;
            bool ascending = performance.ascending;
            // Unsorted, zones stay in the order first met, which is the order they run in.
            if (column >= 0) {
                std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
                    if (column == ColumnZone || column == ColumnThread) {
                        int compared = column == ColumnZone ? std::string(zones[static_cast<size_t>(a)].name).compare(zones[static_cast<size_t>(b)].name) : zones[static_cast<size_t>(a)].thread.compare(zones[static_cast<size_t>(b)].thread);
                        return ascending ? compared < 0 : compared > 0;
                    }
                    double x = sortKey(summary.zones[static_cast<size_t>(a)], column);
                    double y = sortKey(summary.zones[static_cast<size_t>(b)], column);
                    return ascending ? x < y : x > y;
                });
            }
            for (int zone : order) {
                const profile::ZoneStats& stats = summary.zones[static_cast<size_t>(zone)];
                const std::vector<int>& below = children[static_cast<size_t>(zone)];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAllColumns | ImGuiTreeNodeFlags_DefaultOpen;
                if (below.empty()) {
                    flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
                }
                ImGui::PushID(zone);
                bool open = ImGui::TreeNodeEx(zones[static_cast<size_t>(zone)].name, flags);
                ImGui::PopID();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(zones[static_cast<size_t>(zone)].thread.c_str());
                ImGui::TableNextColumn();
                ImGui::Text("%.3f", stats.meanMs);
                ImGui::TableNextColumn();
                ImGui::Text("%.3f", stats.worstMs);
                ImGui::TableNextColumn();
                ImGui::Text("%.1f%%", stats.share * 100.0);
                ImGui::TableNextColumn();
                ImGui::Text("%.1f", stats.callsPerFrame);
                if (open && !below.empty()) {
                    zoneRows(performance, zones, children, summary, below);
                    ImGui::TreePop();
                }
            }
        }

        void zoneTable(Performance& performance, const profile::Summary& summary)
        {
            std::vector<profile::ZoneInfo> zones = profile::zones();
            std::vector<std::vector<int>> children(zones.size());
            std::vector<int> roots;
            for (size_t i = 0; i < zones.size(); i++) {
                if (zones[i].parent >= 0) {
                    children[static_cast<size_t>(zones[i].parent)].push_back(static_cast<int>(i));
                } else {
                    roots.push_back(static_cast<int>(i));
                }
            }
            ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Sortable | ImGuiTableFlags_SortTristate | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingFixedFit;
            if (!ImGui::BeginTable("zones", 6, flags, ImVec2(0, ImGui::GetContentRegionAvail().y * 0.7f))) {
                return;
            }
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Zone", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_NoHide);
            ImGui::TableSetupColumn("Thread");
            ImGui::TableSetupColumn("Mean ms", ImGuiTableColumnFlags_PreferSortDescending);
            ImGui::TableSetupColumn("Worst ms", ImGuiTableColumnFlags_PreferSortDescending);
            ImGui::TableSetupColumn("Share", ImGuiTableColumnFlags_PreferSortDescending);
            ImGui::TableSetupColumn("Calls/frame", ImGuiTableColumnFlags_PreferSortDescending);
            ImGui::TableHeadersRow();
            if (ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs()) {
                if (specs->SpecsDirty) {
                    performance.sortColumn = specs->SpecsCount > 0 ? specs->Specs[0].ColumnIndex : -1;
                    performance.ascending = specs->SpecsCount > 0 && specs->Specs[0].SortDirection == ImGuiSortDirection_Ascending;
                    specs->SpecsDirty = false;
                }
            }
            zoneRows(performance, zones, children, summary, roots);
            ImGui::EndTable();
        }

        void valueTable(const profile::Summary& summary)
        {
            std::vector<profile::ValueInfo> values = profile::values();
            if (values.empty()) {
                ImGui::TextDisabled("No DX_TIME totals or DX_PLOT values yet.");
                return;
            }
            ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit;
            if (!ImGui::BeginTable("values", 5, flags, ImVec2(0, 0))) {
                return;
            }
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Total or plot", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Kind");
            ImGui::TableSetupColumn("Mean");
            ImGui::TableSetupColumn("Worst");
            ImGui::TableSetupColumn("Last");
            ImGui::TableHeadersRow();
            for (size_t i = 0; i < values.size() && i < summary.values.size(); i++) {
                const profile::ValueStats& stats = summary.values[i];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(values[i].name);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(values[i].time ? "ms per frame" : "plot");
                ImGui::TableNextColumn();
                ImGui::Text("%.3f", stats.mean);
                ImGui::TableNextColumn();
                ImGui::Text("%.3f", stats.worst);
                ImGui::TableNextColumn();
                ImGui::Text("%.3f", stats.last);
            }
            ImGui::EndTable();
        }

        // Frame Timing has no menu choice: Steady is the game's, Original is for comparing
        // with Deus Ex's frame-rate quirks (docs/differences.md, "Frame-rate dependence").
        void frameTiming()
        {
            int timing = settings::get(settings::frameTiming);
            ImGui::TextUnformatted("Frame timing:");
            ImGui::SameLine();
            if (ImGui::RadioButton("Steady", timing == 0) && timing != 0) {
                settings::set({ settings::to(settings::frameTiming, 0) });
            }
            ImGui::SameLine();
            if (ImGui::RadioButton("Original", timing == 1) && timing != 1) {
                settings::set({ settings::to(settings::frameTiming, 1) });
            }
            ImGui::SameLine();
            ImGui::TextDisabled("FrameTiming in the ini, saved");
        }

        void draw(Performance& performance)
        {
            frameTiming();
            if (!performance.paused && !profile::isRecording()) {
                profile::setRecording(true);
                performance.toolRecording = true;
            }
            bool recording = profile::isRecording();
            if (ImGui::Checkbox("Pause", &performance.paused)) {
                if (performance.paused) {
                    stopToolRecording(performance);
                    if (performance.consoleRecording) {
                        performance.consoleRecording = false;
                        profile::setRecording(false);
                    }
                }
            }
            ImGui::SameLine();
            ImGui::TextColored(recording ? ImVec4(0.5f, 1.0f, 0.5f, 1.0f) : ImVec4(1.0f, 0.6f, 0.4f, 1.0f), recording ? "recording" : "not recording");
            ImGui::SameLine();
            if (ImGui::Button("Reset")) {
                profile::reset();
            }
            ImGui::SameLine();
            if (ImGui::Button("Write CSV")) {
                write(performance, performanceFile(""));
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(scaled(90));
            ImGui::InputInt("s", &performance.recordSeconds);
            performance.recordSeconds = std::clamp(performance.recordSeconds, 1, 600);
            ImGui::SameLine();
            double progress = 0;
            if (profile::timedRecording(progress)) {
                ImGui::ProgressBar(static_cast<float>(progress), ImVec2(scaled(160), 0), "timed recording");
            } else if (ImGui::Button("Record and write")) {
                Performance* target = &performance;
                profile::recordFor(static_cast<double>(performance.recordSeconds), 0, [target] {
                    write(*target, "");
                });
            }
            if (!performance.lastError.empty()) {
                ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f), "%s", performance.lastError.c_str());
            } else if (!performance.lastFile.empty()) {
                ImGui::TextDisabled("Last file: %s", performance.lastFile.c_str());
            }
            int over = performance.sinceStart ? 1 : 0;
            ImGui::RadioButton("Over the last frames kept", &over, 0);
            ImGui::SameLine();
            ImGui::RadioButton("Since recording started", &over, 1);
            performance.sinceStart = over == 1;
            profile::Summary summary = profile::summarize(performance.sinceStart);
            graph(performance, summary);
            ImGui::TextUnformatted("Zones");
            helpMarker("CPU milliseconds per frame. A zone's time includes the zones under it; zones on other threads are their own roots.");
            zoneTable(performance, summary);
            valueTable(summary);
        }
    }

    std::string performanceFile(const std::string& name)
    {
        std::filesystem::path given(name);
        if (!name.empty() && given.is_absolute()) {
            return given.string();
        }
        std::filesystem::path folder = config::userDirectory() / "Profiles";
        if (!name.empty()) {
            std::filesystem::path file = folder / given;
            if (!file.has_extension()) {
                file += ".csv";
            }
            return file.string();
        }
        std::error_code error;
        for (int index = 0; index < 10000; index++) {
            char file[32];
            std::snprintf(file, sizeof(file), "Frames%04d.csv", index);
            if (!std::filesystem::exists(folder / file, error)) {
                return (folder / file).string();
            }
        }
        return (folder / "Frames.csv").string();
    }

    void addPerformanceTool(Overlay& overlay)
    {
        auto performance = std::make_shared<Performance>();
        Tool tool;
        tool.menu = "Engine";
        tool.title = "Performance";
        tool.name = "performance";
        tool.width = 820;
        tool.height = 720;
        tool.draw = [performance](const Frame&) {
            draw(*performance);
        };
        tool.closed = [performance] {
            stopToolRecording(*performance);
        };
        overlay.addTool(std::move(tool));
        // Hidden, the tool is not drawn, so it does not keep the recorder on.
        overlay.onClose([performance] {
            stopToolRecording(*performance);
        });
        overlay.addSwitch("perf-record", [performance](bool on) {
            performance->consoleRecording = on;
            if (on) {
                profile::setRecording(true);
            } else {
                performance->toolRecording = false;
                profile::setRecording(false);
            }
        });
        overlay.addSwitch("gpu-timers", [](bool on) {
            gputimers::setEnabled(on);
        });
        overlay.addCommand("perf-reset", [](const std::string&) {
            profile::reset();
            return std::string("frame recording reset");
        });
        overlay.addCommand("perf-csv", [performance](const std::string& value) {
            return write(*performance, performanceFile(value == "default" || value == "now" ? std::string() : value));
        });
        overlay.addCommand("perf-record-for", [performance](const std::string& given) {
            // N seconds, or N frames with an f after it, then optionally the file's name.
            size_t space = given.find(' ');
            std::string value = given.substr(0, space);
            std::string name = space == std::string::npos ? std::string() : given.substr(space + 1);
            char* end = nullptr;
            double amount = std::strtod(value.c_str(), &end);
            bool frames = end && *end == 'f';
            if (end == value.c_str() || amount <= 0 || (*end != '\0' && !(frames && end[1] == '\0'))) {
                return std::string("DebugOverlay perf-record-for <seconds>, or <frames>f, then optionally a file name");
            }
            std::string path = performanceFile(name);
            profile::recordFor(frames ? 0.0 : amount, frames ? static_cast<int>(amount) : 0, [performance, path] {
                std::string reply = write(*performance, path);
                log::info("DevPerf", "frame recording: {}", reply);
            });
            char reply[512];
            std::snprintf(reply, sizeof(reply), "recording %g %s; the file goes to %s", amount, frames ? "frames" : "seconds", path.c_str());
            return std::string(reply);
        });
    }
}
