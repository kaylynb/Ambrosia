#include "render/gpu_timers.h"

#include "core/profile.h"

#include <glad/gl.h>

#include <array>
#include <vector>

namespace dx::gputimers {
    namespace {
        constexpr int RingFrames = FrameLag + 1;
        constexpr int MaxPairs = 64;

        struct Pair {
            Stage stage = Stage::Scene;
            bool ended = false;
        };

        struct FrameQueries {
            // Two queries per pair: start and end.
            std::vector<GLuint> queries;
            std::vector<Pair> pairs;
        };

        bool on = false;
        int current = 0;
        std::array<FrameQueries, RingFrames> ring;
        std::array<double, static_cast<size_t>(Stage::Count)> lastMs {};
        uint32_t late = 0;

        void freeAll()
        {
            for (FrameQueries& frame : ring) {
                if (!frame.queries.empty()) {
                    glDeleteQueries(static_cast<GLsizei>(frame.queries.size()), frame.queries.data());
                }
                frame.queries.clear();
                frame.pairs.clear();
            }
            lastMs = {};
            late = 0;
        }

        // Sums the frame's pairs into ms, or false while the GPU has not finished it.
        bool read(FrameQueries& frame, std::array<double, static_cast<size_t>(Stage::Count)>& ms)
        {
            ms = {};
            if (frame.pairs.empty()) {
                return true;
            }
            // Timestamps complete in order, so the last one written stands for all.
            size_t last = 0;
            for (size_t i = 0; i < frame.pairs.size(); i++) {
                last = frame.pairs[i].ended ? i * 2 + 1 : i * 2;
            }
            GLint available = 0;
            glGetQueryObjectiv(frame.queries[last], GL_QUERY_RESULT_AVAILABLE, &available);
            if (!available) {
                return false;
            }
            for (size_t i = 0; i < frame.pairs.size(); i++) {
                if (!frame.pairs[i].ended) {
                    continue;
                }
                GLuint64 start = 0;
                GLuint64 end = 0;
                glGetQueryObjectui64v(frame.queries[i * 2], GL_QUERY_RESULT, &start);
                glGetQueryObjectui64v(frame.queries[i * 2 + 1], GL_QUERY_RESULT, &end);
                if (end > start) {
                    ms[static_cast<size_t>(frame.pairs[i].stage)] += static_cast<double>(end - start) / 1.0e6;
                }
            }
            return true;
        }
    }

    void setEnabled(bool enable)
    {
        if (on && !enable) {
            freeAll();
        }
        on = enable;
    }

    bool enabled()
    {
        return on;
    }

    Scope::Scope(Stage stage)
    {
        if (!on) {
            return;
        }
        FrameQueries& frame = ring[static_cast<size_t>(current)];
        if (frame.pairs.size() >= MaxPairs) {
            return;
        }
        pair = static_cast<int>(frame.pairs.size());
        if (frame.queries.size() < frame.pairs.size() * 2 + 2) {
            size_t had = frame.queries.size();
            frame.queries.resize(had + 2);
            glGenQueries(2, frame.queries.data() + had);
        }
        frame.pairs.push_back({ stage, false });
        glQueryCounter(frame.queries[static_cast<size_t>(pair) * 2], GL_TIMESTAMP);
    }

    Scope::~Scope()
    {
        if (pair < 0 || !on) {
            return;
        }
        FrameQueries& frame = ring[static_cast<size_t>(current)];
        if (static_cast<size_t>(pair) >= frame.pairs.size()) {
            return;
        }
        glQueryCounter(frame.queries[static_cast<size_t>(pair) * 2 + 1], GL_TIMESTAMP);
        frame.pairs[static_cast<size_t>(pair)].ended = true;
    }

    void endFrame()
    {
        if (!on) {
            return;
        }
        current = (current + 1) % RingFrames;
        // The slot about to be reused holds the frame issued FrameLag frames ago.
        FrameQueries& oldest = ring[static_cast<size_t>(current)];
        std::array<double, static_cast<size_t>(Stage::Count)> ms {};
        if (read(oldest, ms)) {
            lastMs = ms;
        } else {
            late++;
        }
        oldest.pairs.clear();
        auto at = [](Stage stage) {
            return lastMs[static_cast<size_t>(stage)];
        };
        DX_PLOT("GPU scene", at(Stage::Scene));
        DX_PLOT("GPU mirrors", at(Stage::Mirrors));
        DX_PLOT("GPU translucent", at(Stage::Translucent));
        DX_PLOT("GPU post chain", at(Stage::PostChain));
        DX_PLOT("GPU HUD", at(Stage::Hud));
        DX_PLOT("GPU main view", at(Stage::Scene) - at(Stage::Mirrors) - at(Stage::PostChain));
        DX_PLOT("GPU timer frames late", late);
    }
}
