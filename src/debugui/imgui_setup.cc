#include "debugui/imgui_setup.h"

#include "core/platform.h"

#include <imgui.h>
#include <imgui_impl_opengl3.h>
#include <imgui_impl_sdl3.h>
#include <implot.h>

#include <algorithm>
#include <array>
#include <cmath>

namespace dx::debugui {
    namespace {
        float current = 1.0f;
    }

    void startImGui(SDL_Window* window, SDL_GLContext context)
    {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        // ImPlot keeps its own context beside ImGui's, for the overlay's and editor's plots.
        ImPlot::CreateContext();
        ImGui::GetIO().IniFilename = nullptr;
        ImGui::StyleColorsDark();
        current = 1.0f;
        ImGui::GetIO().Fonts->AddFontDefault();
        ImGui_ImplSDL3_InitForOpenGL(window, context);
        // The backend starts and stops SDL's text input through this handler as fields are
        // focused; without it, it never does.
        if (!platform::overlayTextInput()) {
            ImGui::GetPlatformIO().Platform_SetImeDataFn = nullptr;
        }
        ImGui_ImplOpenGL3_Init("#version 330 core");
    }

    void stopImGui()
    {
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplSDL3_Shutdown();
        ImPlot::DestroyContext();
        ImGui::DestroyContext();
    }

    float autoScale(SDL_Window* window)
    {
        float content = window ? SDL_GetWindowDisplayScale(window) : 0.0f;
        if (!(content > 0.0f)) {
            content = 1.0f;
        }
        int width = 0;
        int height = 0;
        if (window) {
            SDL_GetWindowSizeInPixels(window, &width, &height);
        }
        float byHeight = std::floor(static_cast<float>(height) / 1080.0f * 4.0f) / 4.0f;
        return std::clamp(std::max(content, byHeight), 1.0f, 4.0f);
    }

    void setScale(float scale)
    {
        scale = std::clamp(scale, 0.5f, 4.0f);
        if (scale == current) {
            return;
        }
        // ScaleAllSizes multiplies, so the sizes start again from the unscaled style.
        ImGuiStyle style;
        ImGui::StyleColorsDark(&style);
        style.ScaleAllSizes(scale);
        style.FontScaleDpi = scale;
        ImGui::GetStyle() = style;
        current = scale;
    }

    float scale()
    {
        return current;
    }

    float scaled(float size)
    {
        return size * current;
    }

    void helpMarker(const char* text)
    {
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        itemHelp(text);
    }

    void itemHelp(const char* text)
    {
        if (text[0] && ImGui::BeginItemTooltip()) {
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 32.0f);
            ImGui::TextUnformatted(text);
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }
    }

    std::span<const float> scaleChoices()
    {
        static constexpr std::array<float, 8> choices = { 1.0f, 1.25f, 1.5f, 1.75f, 2.0f, 2.5f, 3.0f, 4.0f };
        return choices;
    }
}
