#include "deusex/menu_backdrop.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <optional>
#include <queue>
#include <string_view>
#include <utility>
#include <vector>

// Ported from a Python prototype (since removed), whose style it keeps; the numbers were measured from, and the shading fitted to, MenuSoundBackground and MenuGameOptionsBackground.
// CMake builds this file with -ffp-contract=off where the compiler would otherwise fuse a multiply
// and an add (GCC on the Switch's ARM), so its floating point is the same everywhere.

namespace dx::deusex {
    namespace {
        // ---------------------------------------------------------------------------------
        // Style.

        struct Shading {
            double topLeft = 69.0; // the base grey at (0, 0)
            double fallX = 0.043;  // per pixel to the right, in a 537 by 408 window
            double fallY = 0.06;   // per pixel down
            double baseMin = 22.0;
            double glow = 1.2;    // the glow next to a cut-out, times the base
            double glowSat = 4.0; // the glow is soft-clipped: g -> sat * (1 - exp(-g / sat))
            double scan = 0.037;  // even rows times 1 + scan, odd rows times 1 - scan
            double noise = 0.8;
            double lo = 16.0;
            double hi = 165.0;
        };

        struct Routing {
            int pitch = 2; // the traces run on this grid
            int clear = 3; // pixels kept free between a trace and a slot or another trace
            // A* costs, in tenths of a vertical step.
            int turn = 70;
            int nearCost = 30;  // per step inside another trace's clearance
            int horizontal = 9; // longer runs along the rows
            int vertical = 10;
            int laneMax = 3;    // a slot's trace keeps up to this much more from the slots
            int edgePorts = 10; // traces in from the window's edges, per 537x408
            int couplers = 2;
            double bracketChance = 0.1; // a port routes back into its own slot further along
            double linkChance = 0.45;   // to another slot; else to the nearest trace or edge
            double bevelChance = 0.3;   // a corner cut at 45 degrees
            int plates = 5;
            int plateMinW = 50;
            int plateMinH = 34;
            int plateMaxW = 170;
            int plateMaxH = 100;
        };

        constexpr int referenceWidth = 537;
        constexpr int referenceHeight = 408;

        // PCG32 (O'Neill), with ranges made here: the numbers are the same everywhere.
        class Random {
        public:
            explicit Random(uint64_t seed)
                : increment((seed << 1u) | 1u)
            {
                next();
                state += seed ^ 0x853c49e6748fea9bull;
                next();
            }

            uint32_t next()
            {
                uint64_t old = state;
                state = old * 6364136223846793005ull + increment;
                auto shifted = static_cast<uint32_t>(((old >> 18u) ^ old) >> 27u);
                auto rotation = static_cast<uint32_t>(old >> 59u);
                return (shifted >> rotation) | (shifted << ((32u - rotation) & 31u));
            }

            // In [lo, hi); lo when that is empty.
            int range(int lo, int hi)
            {
                if (hi <= lo) {
                    return lo;
                }
                return lo + static_cast<int>((static_cast<uint64_t>(next()) * static_cast<uint64_t>(hi - lo)) >> 32u);
            }

            // In [0, 1).
            double unit()
            {
                return static_cast<double>(next()) * (1.0 / 4294967296.0);
            }

            bool chance(double p)
            {
                return unit() < p;
            }

            template <typename T>
            void shuffle(std::vector<T>& items)
            {
                for (size_t i = items.size(); i > 1; i--) {
                    std::swap(items[i - 1], items[static_cast<size_t>(range(0, static_cast<int>(i)))]);
                }
            }

        private:
            uint64_t state = 0;
            uint64_t increment;
        };

        // e^x from adds, multiplies and divides alone (with std::floor and std::ldexp, which are
        // exact), so it is the same on every platform as the C library's need not be.
        double exactExp(double x)
        {
            if (x < -700.0) {
                return 0.0;
            }
            // e^x = 2^n e^r, |r| <= ln 2 / 2, where twelve terms of e^r's series are exact to the
            // last bit; 1 / k! is worked out by the compiler, the same everywhere.
            constexpr std::array<double, 13> inverseFactorial = [] {
                std::array<double, 13> c {};
                double f = 1.0;
                for (size_t k = 0; k < c.size(); k++) {
                    f = k ? f * static_cast<double>(k) : 1.0;
                    c[k] = 1.0 / f;
                }
                return c;
            }();
            double n = std::floor(x * 1.4426950408889634 + 0.5);
            double r = x - n * 0.6931471805599453;
            double sum = inverseFactorial.back();
            for (size_t k = inverseFactorial.size() - 1; k-- > 0;) {
                sum = sum * r + inverseFactorial[k];
            }
            return std::ldexp(sum, static_cast<int>(n));
        }

        int floorDiv(int a, int b)
        {
            int q = a / b;
            return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
        }

        int ceilDiv(int a, int b)
        {
            return -floorDiv(-a, b);
        }

        int sign(int v)
        {
            return (v > 0) - (v < 0);
        }

        struct Point {
            int x = 0;
            int y = 0;
            bool operator==(const Point&) const = default;
        };

        using Polyline = std::vector<Point>;

        enum class Side { Top,
            Bottom,
            Left,
            Right };

        struct Chamfer {
            int tl = 0;
            int tr = 0;
            int bl = 0;
            int br = 0;

            int largest() const
            {
                return std::max({ tl, tr, bl, br });
            }
        };

        struct Item {
            MenuSlot::Kind kind = MenuSlot::Kind::Button;
            int x = 0;
            int y = 0;
            int w = 0;
            int h = 0;
            Chamfer chamfer;
            int outline = 0;   // 0 is a solid slot
            float dim = -1.0f; // an outlined slot's inside, times the base; below 0 for none

            int x1() const
            {
                return x + w;
            }

            int y1() const
            {
                return y + h;
            }
        };

        // A button or a slider's track: one cut-out, which takes tabs, the connector and more traces.
        bool solid(MenuSlot::Kind kind)
        {
            return kind == MenuSlot::Kind::Button || kind == MenuSlot::Kind::Track;
        }

        // The originals' slot shapes, measured from MenuSoundBackground.
        Item itemFor(const MenuSlot& slot)
        {
            Item item;
            item.kind = slot.kind;
            item.x = slot.x;
            item.y = slot.y;
            item.w = slot.width;
            item.h = slot.height;
            // Chamfers top-left, top-right, bottom-left, bottom-right.
            switch (slot.kind) {
            case MenuSlot::Kind::Button:
                item.chamfer = { 5, 0, 0, 6 };
                break;
            case MenuSlot::Kind::Track:
                item.chamfer = { 3, 3, 3, 3 };
                break;
            case MenuSlot::Kind::Field:
                item.chamfer = { 2, 0, 0, 6 };
                item.outline = 3;
                item.dim = 1.0f;
                break;
            case MenuSlot::Kind::TextBox:
                item.chamfer = { 3, 0, 0, 0 };
                item.outline = 1;
                item.dim = 0.6f;
                break;
            }
            return item;
        }

        bool inChamfered(int x, int y, int w, int h, const Chamfer& c)
        {
            return x + y >= c.tl && (w - 1 - x) + y >= c.tr && x + (h - 1 - y) >= c.bl && (w - 1 - x) + (h - 1 - y) >= c.br;
        }

        // A mask, a byte a pixel.
        struct Mask {
            int w = 0;
            int h = 0;
            std::vector<uint8_t> bits;

            Mask() = default;

            Mask(int width, int height)
                : w(width)
                , h(height)
                , bits(static_cast<size_t>(width) * static_cast<size_t>(height), 0)
            {
            }

            uint8_t& at(int x, int y)
            {
                return bits[static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)];
            }

            uint8_t at(int x, int y) const
            {
                return bits[static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)];
            }
        };

        // Counts of a mask's set pixels in any rectangle, at once.
        class AreaSum {
        public:
            explicit AreaSum(const Mask& mask)
                : w(mask.w)
                , h(mask.h)
                , sums(static_cast<size_t>(mask.w + 1) * static_cast<size_t>(mask.h + 1), 0)
            {
                for (int y = 0; y < h; y++) {
                    int row = 0;
                    for (int x = 0; x < w; x++) {
                        row += mask.at(x, y) ? 1 : 0;
                        sum(x + 1, y + 1) = sum(x + 1, y) + row;
                    }
                }
            }

            // Whether any pixel within `radius` (a square) of (x, y) is set.
            bool any(int x, int y, int radius) const
            {
                int x0 = std::max(0, x - radius);
                int y0 = std::max(0, y - radius);
                int x1 = std::min(w, x + radius + 1);
                int y1 = std::min(h, y + radius + 1);
                if (x0 >= x1 || y0 >= y1) {
                    return false;
                }
                return sum(x1, y1) - sum(x0, y1) - sum(x1, y0) + sum(x0, y0) > 0;
            }

        private:
            int& sum(int x, int y)
            {
                return sums[static_cast<size_t>(y) * static_cast<size_t>(w + 1) + static_cast<size_t>(x)];
            }

            int sum(int x, int y) const
            {
                return sums[static_cast<size_t>(y) * static_cast<size_t>(w + 1) + static_cast<size_t>(x)];
            }

            int w;
            int h;
            std::vector<int> sums;
        };

        // What is cut out, what new traces may join, what they keep clear of, and the flat insides.
        struct Canvas {
            int w;
            int h;
            Mask hole;
            Mask trace;
            Mask keep;
            Mask interior;
            Mask drawn;             // what the screen draws itself: kept clear of, never cut
            std::vector<float> dim; // below 0: the base
            // What has changed since the router last looked: traces added, which it takes in where they are;
            // what traces keep clear of, for which it looks again at all of it; and anything taken away.
            std::vector<Point> newTrace;
            bool keepChanged = true;
            bool erased = true;

            Canvas(int width, int height)
                : w(width)
                , h(height)
                , hole(width, height)
                , trace(width, height)
                , keep(width, height)
                , interior(width, height)
                , drawn(width, height)
                , dim(static_cast<size_t>(width) * static_cast<size_t>(height), -1.0f)
            {
            }

            bool inside(int x, int y) const
            {
                return x >= 0 && y >= 0 && x < w && y < h;
            }

            void addItem(const Item& it)
            {
                int t = it.outline;
                // The inside is cut as much as the outside, as in the originals, so the outline is
                // as thick across a corner as along a side.
                Chamfer inner = it.chamfer;
                for (int y = 0; y < it.h; y++) {
                    for (int x = 0; x < it.w; x++) {
                        int px = it.x + x;
                        int py = it.y + y;
                        if (!inside(px, py) || !inChamfered(x, y, it.w, it.h, it.chamfer)) {
                            continue;
                        }
                        keep.at(px, py) = 1;
                        bool in = t > 0 && x >= t && y >= t && x < it.w - t && y < it.h - t && inChamfered(x - t, y - t, it.w - 2 * t, it.h - 2 * t, inner);
                        if (in) {
                            interior.at(px, py) = 1;
                            if (it.dim >= 0.0f) {
                                dim[static_cast<size_t>(py) * static_cast<size_t>(w) + static_cast<size_t>(px)] = it.dim;
                            }
                        } else {
                            hole.at(px, py) = 1;
                        }
                    }
                }
            }

            // Cuts a polyline of level, upright and 45 degree segments.
            void plot(const Polyline& points, bool joinable = true)
            {
                keepChanged = keepChanged || !joinable;
                for (size_t i = 0; i + 1 < points.size(); i++) {
                    Point a = points[i];
                    Point b = points[i + 1];
                    int n = std::max(std::abs(b.x - a.x), std::abs(b.y - a.y));
                    int sx = sign(b.x - a.x);
                    int sy = sign(b.y - a.y);
                    for (int k = 0; k <= n; k++) {
                        int x = a.x + sx * k;
                        int y = a.y + sy * k;
                        if (inside(x, y)) {
                            hole.at(x, y) = 1;
                            if (joinable && !trace.at(x, y)) {
                                trace.at(x, y) = 1;
                                newTrace.push_back({ x, y });
                            }
                        }
                    }
                }
            }

            void rectHole(int x0, int y0, int x1, int y1)
            {
                keepChanged = true;
                for (int y = std::max(0, y0); y < std::min(h, y1); y++) {
                    for (int x = std::max(0, x0); x < std::min(w, x1); x++) {
                        hole.at(x, y) = 1;
                    }
                }
            }

            void setKeep(int x0, int y0, int x1, int y1)
            {
                keepChanged = true;
                for (int y = std::max(0, y0); y < std::min(h, y1); y++) {
                    for (int x = std::max(0, x0); x < std::min(w, x1); x++) {
                        keep.at(x, y) = 1;
                    }
                }
            }

            // Nothing cut out or inside a slot in [x0, x1) by [y0, y1), widened by `margin`.
            bool isFree(int x0, int y0, int x1, int y1, int margin) const
            {
                for (int y = std::max(0, y0 - margin); y < std::min(h, y1 + margin); y++) {
                    for (int x = std::max(0, x0 - margin); x < std::min(w, x1 + margin); x++) {
                        if (hole.at(x, y) || interior.at(x, y)) {
                            return false;
                        }
                    }
                }
                return true;
            }
        };

        // ---------------------------------------------------------------------------------
        // Routing: A* on a grid of every `pitch` pixels.

        struct Port {
            Point edge;  // the first pixel outside the slot (or on the window's edge)
            Point start; // the grid cell routing starts from
        };

        class Router {
        public:
            Router(Canvas& canvas, const Routing& config)
                : cv(canvas)
                , cfg(config)
                , gw((canvas.w - 1) / config.pitch + 1)
                , gh((canvas.h - 1) / config.pitch + 1)
            {
                refresh();
            }

            int gridWidth() const
            {
                return gw;
            }

            int gridHeight() const
            {
                return gh;
            }

            // Takes in what has been cut since: the slots, insides and holes that are not traces
            // are kept `clear` away from, and other traces cost more to pass near.
            void refresh()
            {
                bool changed = cv.erased || cv.keepChanged || !cv.newTrace.empty();
                // What traces keep clear of, when that has changed (a pad, a tab, a bar) or
                // something was taken away: looked at again whole.
                if (cv.erased || cv.keepChanged) {
                    Mask keepSource(cv.w, cv.h);
                    for (size_t i = 0; i < keepSource.bits.size(); i++) {
                        keepSource.bits[i] = (cv.keep.bits[i] || cv.interior.bits[i] || cv.drawn.bits[i] || (cv.hole.bits[i] && !cv.trace.bits[i])) ? 1 : 0;
                    }
                    keepSum.emplace(keepSource);
                    lanes.clear();
                    cv.keepChanged = false;
                }
                int reach = cfg.clear + 1;
                if (cv.erased) {
                    AreaSum traceSum(cv.trace);
                    size_t cells = static_cast<size_t>(gw) * static_cast<size_t>(gh);
                    traceGrid.assign(cells, 0);
                    nearTrace.assign(cells, 0);
                    for (int gy = 0; gy < gh; gy++) {
                        for (int gx = 0; gx < gw; gx++) {
                            size_t i = cell(gx, gy);
                            traceGrid[i] = cv.trace.at(gx * cfg.pitch, gy * cfg.pitch);
                            nearTrace[i] = !traceGrid[i] && traceSum.any(gx * cfg.pitch, gy * cfg.pitch, reach);
                        }
                    }
                    cv.erased = false;
                } else {
                    // New traces only add: each is a trace cell where it is on the grid and
                    // makes the cells `reach` around it near one, as looking again whole would.
                    for (Point t : cv.newTrace) {
                        if (t.x % cfg.pitch == 0 && t.y % cfg.pitch == 0) {
                            size_t i = cell(t.x / cfg.pitch, t.y / cfg.pitch);
                            traceGrid[i] = 1;
                            nearTrace[i] = 0;
                        }
                        int gx0 = std::max(0, ceilDiv(t.x - reach, cfg.pitch));
                        int gx1 = std::min(gw - 1, floorDiv(t.x + reach, cfg.pitch));
                        int gy0 = std::max(0, ceilDiv(t.y - reach, cfg.pitch));
                        int gy1 = std::min(gh - 1, floorDiv(t.y + reach, cfg.pitch));
                        for (int gy = gy0; gy <= gy1; gy++) {
                            for (int gx = gx0; gx <= gx1; gx++) {
                                size_t i = cell(gx, gy);
                                if (!traceGrid[i]) {
                                    nearTrace[i] = 1;
                                }
                            }
                        }
                    }
                }
                cv.newTrace.clear();
                if (changed) {
                    copper = {};
                }
                setLane(0);
            }

            // Keeps the next route `extra` pixels further from the slots, so the traces between
            // two rows are not all the same distance from them.
            void setLane(int extra)
            {
                lane = extra;
                if (lanes.size() <= static_cast<size_t>(extra)) {
                    lanes.resize(static_cast<size_t>(extra) + 1);
                }
                std::vector<uint8_t>& made = lanes[static_cast<size_t>(extra)];
                if (made.empty()) {
                    made.assign(static_cast<size_t>(gw) * static_cast<size_t>(gh), 0);
                    for (int gy = 0; gy < gh; gy++) {
                        for (int gx = 0; gx < gw; gx++) {
                            made[cell(gx, gy)] = keepSum->any(gx * cfg.pitch, gy * cfg.pitch, cfg.clear + extra);
                        }
                    }
                }
                blocked = made;
            }

            // A port on a slot's side, `at` pixels along it (snapped to the grid).
            std::optional<Port> port(const Item& it, Side side, int at) const
            {
                int p = cfg.pitch;
                int c = cfg.clear + lane;
                at = floorDiv(at, p) * p;
                Point edge;
                Point start;
                Point end;
                if (side == Side::Top || side == Side::Bottom) {
                    int ey = side == Side::Top ? it.y - 1 : it.y1();
                    int gy = side == Side::Top ? floorDiv(ey - c - 1, p) : ceilDiv(ey + c + 1, p);
                    edge = { at, ey };
                    start = { at / p, gy };
                    end = { at, gy * p };
                } else {
                    int ex = side == Side::Left ? it.x - 1 : it.x1();
                    int gx = side == Side::Left ? floorDiv(ex - c - 1, p) : ceilDiv(ex + c + 1, p);
                    edge = { ex, at };
                    start = { gx, at / p };
                    end = { gx * p, at };
                }
                if (!inGrid(start) || !cv.inside(edge.x, edge.y) || cv.hole.at(edge.x, edge.y) || blocked[cell(start.x, start.y)]) {
                    return std::nullopt;
                }
                // The stub out to the start must not cut into anything.
                int n = std::max(std::abs(end.x - edge.x), std::abs(end.y - edge.y));
                int sx = sign(end.x - edge.x);
                int sy = sign(end.y - edge.y);
                for (int k = 0; k <= n; k++) {
                    int x = edge.x + sx * k;
                    int y = edge.y + sy * k;
                    if (!cv.inside(x, y) || cv.hole.at(x, y) || cv.interior.at(x, y)) {
                        return std::nullopt;
                    }
                }
                return Port { edge, start };
            }

            // A trace coming in from one of the window's edges.
            std::optional<Port> edgePort(Side side, int at) const
            {
                int p = cfg.pitch;
                at = floorDiv(at, p) * p;
                Point edge;
                Point start;
                if (side == Side::Top) {
                    edge = { at, 0 };
                    start = { at / p, 1 };
                } else if (side == Side::Bottom) {
                    edge = { at, cv.h - 1 };
                    start = { at / p, (cv.h - 1) / p - 1 };
                } else if (side == Side::Left) {
                    edge = { 0, at };
                    start = { 1, at / p };
                } else {
                    edge = { cv.w - 1, at };
                    start = { (cv.w - 1) / p - 1, at / p };
                }
                if (!inGrid(start) || !cv.inside(edge.x, edge.y)) {
                    return std::nullopt;
                }
                size_t i = cell(start.x, start.y);
                if (blocked[i] || nearTrace[i] || cv.hole.at(edge.x, edge.y)) {
                    return std::nullopt;
                }
                return Port { edge, start };
            }

            // From `a` to `b`, or with none to the nearest trace (or the top, bottom or right
            // edge, with `border`). Nothing when there is no way, or the way to `b` is more than
            // `maxRatio` times as long as straight there.
            std::optional<Polyline> route(const Port& a, const Port* b, double maxRatio, bool border)
            {
                std::vector<Point> cells;
                if (b) {
                    // To one cell: the straight distance there is the heuristic.
                    size_t target = cell(b->start.x, b->start.y);
                    Point to = b->start;
                    cells = search(
                        a.start, [&](size_t i) { return i == target; },
                        [&](int x, int y) { return std::abs(x - to.x) + std::abs(y - to.y); });
                } else {
                    const Copper& goal = copperGoal(border);
                    if (!goal.any) {
                        return std::nullopt;
                    }
                    cells = search(
                        a.start, [&](size_t i) { return goal.goal[i] != 0; },
                        [&](int x, int y) { return goal.distance[cell(x, y)]; });
                }
                if (cells.empty()) {
                    return std::nullopt;
                }
                if (b && maxRatio > 0.0) {
                    int straight = std::abs(a.start.x - b->start.x) + std::abs(a.start.y - b->start.y);
                    if (static_cast<double>(cells.size()) > maxRatio * straight + 40.0) {
                        return std::nullopt;
                    }
                }
                Polyline points { a.edge };
                for (Point c : cells) {
                    points.push_back({ c.x * cfg.pitch, c.y * cfg.pitch });
                }
                if (b) {
                    points.push_back(b->edge);
                } else {
                    Point last = points.back();
                    if (cells.back().x == gw - 1) {
                        points.push_back({ cv.w - 1, last.y });
                    } else if (cells.back().y == gh - 1) {
                        points.push_back({ last.x, cv.h - 1 });
                    } else if (cells.back().y == 0) {
                        points.push_back({ last.x, 0 });
                    }
                }
                return simplify(points);
            }

            static Polyline simplify(const Polyline& points)
            {
                Polyline out { points.front() };
                for (size_t i = 1; i < points.size(); i++) {
                    Point q = points[i];
                    if (q == out.back()) {
                        continue;
                    }
                    if (out.size() >= 2) {
                        Point a = out[out.size() - 2];
                        Point b = out.back();
                        if (sign(b.x - a.x) == sign(q.x - b.x) && sign(b.y - a.y) == sign(q.y - b.y)) {
                            out.back() = q;
                            continue;
                        }
                    }
                    out.push_back(q);
                }
                return out;
            }

        private:
            size_t cell(int gx, int gy) const
            {
                return static_cast<size_t>(gy) * static_cast<size_t>(gw) + static_cast<size_t>(gx);
            }

            bool inGrid(Point c) const
            {
                return c.x >= 0 && c.y >= 0 && c.x < gw && c.y < gh;
            }

            // The nearest trace (or edge) as a goal, with each cell's steps to it ignoring what is
            // in the way, for the heuristic. Kept until something is cut, for each lane.
            struct Copper {
                std::vector<uint8_t> goal;
                std::vector<int> distance;
                bool any = false;
            };

            const Copper& copperGoal(bool border)
            {
                Copper& made = copper[border ? 1 : 0];
                if (!made.goal.empty() && copperLane[border ? 1 : 0] == lane) {
                    return made;
                }
                copperLane[border ? 1 : 0] = lane;
                made.goal = traceGrid;
                if (border) {
                    for (int gx = 0; gx < gw; gx++) {
                        made.goal[cell(gx, 0)] = made.goal[cell(gx, gh - 1)] = 1;
                    }
                    for (int gy = 0; gy < gh; gy++) {
                        made.goal[cell(gw - 1, gy)] = 1;
                    }
                }
                constexpr int unreached = 1 << 28;
                made.distance.assign(made.goal.size(), unreached);
                made.any = false;
                for (size_t i = 0; i < made.goal.size(); i++) {
                    if (blocked[i] && !traceGrid[i]) {
                        made.goal[i] = 0;
                    }
                    if (made.goal[i]) {
                        made.distance[i] = 0;
                        made.any = true;
                    }
                }
                std::vector<int>& d = made.distance;
                for (int gy = 0; gy < gh; gy++) {
                    for (int gx = 0; gx < gw; gx++) {
                        int& here = d[cell(gx, gy)];
                        if (gx > 0) {
                            here = std::min(here, d[cell(gx - 1, gy)] + 1);
                        }
                        if (gy > 0) {
                            here = std::min(here, d[cell(gx, gy - 1)] + 1);
                        }
                    }
                }
                for (int gy = gh - 1; gy >= 0; gy--) {
                    for (int gx = gw - 1; gx >= 0; gx--) {
                        int& here = d[cell(gx, gy)];
                        if (gx < gw - 1) {
                            here = std::min(here, d[cell(gx + 1, gy)] + 1);
                        }
                        if (gy < gh - 1) {
                            here = std::min(here, d[cell(gx, gy + 1)] + 1);
                        }
                    }
                }
                return made;
            }

            // The cells from `start` to the nearest goal, or none, with `steps(x, y)` never more
            // than the steps from there to a goal. Never across another trace: one it meets is
            // the end, when it is a goal.
            template <typename IsGoal, typename Steps>
            std::vector<Point> search(Point start, IsGoal isGoal, Steps steps)
            {
                size_t states = static_cast<size_t>(gw) * static_cast<size_t>(gh) * 4;
                // Each search's own marks, so nothing is cleared between searches.
                if (stamp.size() != states) {
                    stamp.assign(states, 0);
                    best.assign(states, 0);
                    came.assign(states, -1);
                }
                epoch++;
                auto bestOf = [&](int s) { return stamp[static_cast<size_t>(s)] == epoch ? best[static_cast<size_t>(s)] : (1 << 30); };
                auto set = [&](int s, int g, int from) {
                    stamp[static_cast<size_t>(s)] = epoch;
                    best[static_cast<size_t>(s)] = g;
                    came[static_cast<size_t>(s)] = from;
                };
                int cheapest = std::min(cfg.horizontal, cfg.vertical);
                using Entry = std::pair<int, int>; // f, state
                open.clear();
                auto push = [&](int f, int s) {
                    open.push_back({ f, s });
                    std::push_heap(open.begin(), open.end(), std::greater<>());
                };
                constexpr std::array<Point, 4> dirs { { { 1, 0 }, { 0, 1 }, { -1, 0 }, { 0, -1 } } };
                size_t startCell = cell(start.x, start.y);
                for (int d = 0; d < 4; d++) {
                    int s = static_cast<int>(startCell) * 4 + d;
                    set(s, 0, -1);
                    push(steps(start.x, start.y) * cheapest, s);
                }
                int half = cfg.pitch / 2;
                while (!open.empty()) {
                    std::pop_heap(open.begin(), open.end(), std::greater<>());
                    Entry top = open.back();
                    open.pop_back();
                    int f = top.first;
                    int s = top.second;
                    int g = bestOf(s);
                    size_t here = static_cast<size_t>(s / 4);
                    int x = static_cast<int>(here % static_cast<size_t>(gw));
                    int y = static_cast<int>(here / static_cast<size_t>(gw));
                    if (f > g + steps(x, y) * cheapest) {
                        continue; // a stale entry
                    }
                    int d = s % 4;
                    if (isGoal(here) && here != startCell) {
                        std::vector<Point> path { { x, y } };
                        for (int at = s; came[static_cast<size_t>(at)] >= 0;) {
                            at = came[static_cast<size_t>(at)];
                            Point c { (at / 4) % gw, (at / 4) / gw };
                            if (!(c == path.back())) {
                                path.push_back(c);
                            }
                        }
                        std::reverse(path.begin(), path.end());
                        return path;
                    }
                    for (int nd = 0; nd < 4; nd++) {
                        if ((nd + 2) % 4 == d) {
                            continue;
                        }
                        int nx = x + dirs[static_cast<size_t>(nd)].x;
                        int ny = y + dirs[static_cast<size_t>(nd)].y;
                        if (nx < 0 || ny < 0 || nx >= gw || ny >= gh) {
                            continue;
                        }
                        size_t there = cell(nx, ny);
                        bool goal = isGoal(there);
                        if ((blocked[there] || traceGrid[there]) && !goal && there != startCell) {
                            continue;
                        }
                        // Nor along the window's very edge: a trace only ends there.
                        if (!goal && (nx == 0 || ny == 0 || nx == gw - 1 || ny == gh - 1)) {
                            continue;
                        }
                        // Nor through a 45 degree trace between two grid points.
                        if (half && !goal && cv.trace.at(x * cfg.pitch + dirs[static_cast<size_t>(nd)].x * half, y * cfg.pitch + dirs[static_cast<size_t>(nd)].y * half)) {
                            continue;
                        }
                        int step = (nd % 2 == 0) ? cfg.horizontal : cfg.vertical;
                        int ng = g + step + (nearTrace[there] ? cfg.nearCost : 0) + (nd != d ? cfg.turn : 0);
                        int ns = static_cast<int>(there) * 4 + nd;
                        if (ng < bestOf(ns)) {
                            set(ns, ng, s);
                            push(ng + steps(nx, ny) * cheapest, ns);
                        }
                    }
                }
                return {};
            }

            Canvas& cv;
            const Routing& cfg;
            int gw;
            int gh;
            int lane = 0;
            std::optional<AreaSum> keepSum;
            std::vector<uint8_t> blocked;
            std::vector<uint8_t> traceGrid;
            std::vector<uint8_t> nearTrace;
            std::vector<std::vector<uint8_t>> lanes; // blocked, for each lane
            std::array<Copper, 2> copper;            // without and with the window's edges
            std::array<int, 2> copperLane {};
            std::vector<uint32_t> stamp;
            uint32_t epoch = 0;
            std::vector<int> best;
            std::vector<int> came;
            std::vector<std::pair<int, int>> open;
        };

        // Cuts some corners of a level-and-upright polyline at 45 degrees, never the end segments.
        Polyline bevel(const Polyline& points, Random& rng, double chance)
        {
            if (points.size() < 4) {
                return points;
            }
            Polyline out { points[0], points[1] };
            for (size_t i = 2; i + 1 < points.size(); i++) {
                Point a = out.back();
                Point b = points[i];
                Point c = points[i + 1];
                int before = std::abs(b.x - a.x) + std::abs(b.y - a.y);
                int after = std::abs(c.x - b.x) + std::abs(c.y - b.y);
                int k = rng.range(2, 7);
                if (rng.chance(chance) && before > k + 3 && after > k + 3) {
                    out.push_back({ b.x - sign(b.x - a.x) * k, b.y - sign(b.y - a.y) * k });
                    out.push_back({ b.x + sign(c.x - b.x) * k, b.y + sign(c.y - b.y) * k });
                } else {
                    out.push_back(b);
                }
            }
            out.push_back(points.back());
            return out;
        }

        struct Rect {
            int x0 = 0;
            int y0 = 0;
            int x1 = 0;
            int y1 = 0;
        };

        // The largest all-set rectangle of a grid (exclusive ends), or none.
        std::optional<Rect> largestRect(const std::vector<uint8_t>& free, int gw, int gh)
        {
            std::vector<int> heights(static_cast<size_t>(gw), 0);
            std::optional<Rect> best;
            int area = 0;
            std::vector<std::pair<int, int>> stack; // start, height
            for (int y = 0; y < gh; y++) {
                for (int x = 0; x < gw; x++) {
                    heights[static_cast<size_t>(x)] = free[static_cast<size_t>(y) * static_cast<size_t>(gw) + static_cast<size_t>(x)] ? heights[static_cast<size_t>(x)] + 1 : 0;
                }
                stack.clear();
                for (int x = 0; x <= gw; x++) {
                    int height = x < gw ? heights[static_cast<size_t>(x)] : 0;
                    int start = x;
                    while (!stack.empty() && stack.back().second >= height) {
                        auto [sx, sh] = stack.back();
                        stack.pop_back();
                        if (sh * (x - sx) > area) {
                            area = sh * (x - sx);
                            best = Rect { sx, y - sh + 1, x, y + 1 };
                        }
                        start = sx;
                    }
                    stack.push_back({ start, height });
                }
            }
            return best;
        }

        // ---------------------------------------------------------------------------------
        // The originals' small parts.

        // Tabs: a thinner strip on a button's top or bottom edge, as under some original buttons.
        void addTabs(Canvas& cv, const std::vector<Item>& items, Random& rng)
        {
            for (const Item& it : items) {
                if (!solid(it.kind) || rng.unit() > 0.3) {
                    continue;
                }
                int w = rng.range(it.w / 5, it.w / 2);
                int x0 = rng.range(it.x + 8, std::max(it.x + 9, it.x1() - w - 8));
                int t = rng.range(2, 4);
                int y0;
                int y1;
                bool ok;
                if (rng.chance(0.5)) {
                    y0 = it.y1();
                    y1 = it.y1() + t;
                    ok = cv.isFree(x0, y1, x0 + w, y1 + 3, 1);
                } else {
                    y0 = it.y - t;
                    y1 = it.y;
                    ok = cv.isFree(x0, y0 - 3, x0 + w, y0, 1);
                }
                if (ok) {
                    cv.rectHole(x0, y0, x0 + w, y1);
                    cv.setKeep(x0, y0, x0 + w, y1);
                }
            }
        }

        // The connector (MenuSoundBackground at x 175-189): on a button's top edge two 15px bars
        // 2px thick, a row of five dots, and two lines up into the trace above, which has a
        // pixel's gap between them. One, where a button has a level trace 12 to 40px straight
        // above it with nothing between; the top row is tried first, as in the originals.
        bool addConnector(Canvas& cv, std::vector<Item> buttons, Random& rng)
        {
            rng.shuffle(buttons);
            std::stable_sort(buttons.begin(), buttons.end(), [](const Item& a, const Item& b) { return a.y < b.y; });
            for (const Item& it : buttons) {
                for (int attempt = 0; attempt < 20; attempt++) {
                    int x = rng.range(it.x + 20, std::max(it.x + 21, it.x1() - 20));
                    int t = it.y;
                    if (x - 8 < 0 || x + 8 >= cv.w || t - 9 < 0) {
                        continue;
                    }
                    auto rowClear = [&](int y) {
                        for (int dx = -8; dx <= 8; dx++) {
                            if (cv.hole.at(x + dx, y)) {
                                return false;
                            }
                        }
                        return true;
                    };
                    int yt = t - 1;
                    while (yt >= 0 && rowClear(yt)) {
                        yt--;
                    }
                    if (yt < 0 || t - yt < 12 || t - yt > 40) {
                        continue;
                    }
                    bool level = true;
                    for (int dx = -3; dx <= 3; dx++) {
                        level = level && cv.trace.at(x + dx, yt);
                    }
                    bool clear = true;
                    for (int y = yt + 1; y < t; y++) {
                        for (int dx = -8; dx <= 8; dx++) {
                            clear = clear && !cv.interior.at(x + dx, y);
                        }
                    }
                    if (!level || !clear) {
                        continue;
                    }
                    cv.rectHole(x - 7, t - 2, x + 8, t);
                    cv.rectHole(x - 7, t - 5, x + 8, t - 3);
                    for (int dx = -4; dx <= 4; dx += 2) {
                        cv.hole.at(x + dx, t - 7) = 1;
                    }
                    cv.plot({ { x - 1, yt + 1 }, { x - 1, t - 9 } }, false);
                    cv.plot({ { x + 1, yt + 1 }, { x + 1, t - 9 } }, false);
                    cv.hole.at(x, yt) = 0;
                    cv.trace.at(x, yt) = 0;
                    cv.erased = true;
                    cv.setKeep(x - 8, yt + 1, x + 9, t);
                    return true;
                }
            }
            return false;
        }

        // The double bar (MenuSoundBackground at x 52-83, y 48-54): a trace runs into the middle
        // of one 3px bar and on out of the middle of a second the same length beside it, a pixel
        // away, stepping back to its own row after. On a long level run, where there is room.
        bool addCoupler(Canvas& cv, const Polyline& points, Random& rng)
        {
            int length = rng.range(26, 36);
            constexpr int k = 10;
            for (size_t i = 0; i + 1 < points.size(); i++) {
                Point a = points[i];
                Point b = points[i + 1];
                if (a.y != b.y || std::abs(b.x - a.x) < length + k + 24) {
                    continue;
                }
                int s = b.x > a.x ? 1 : -1;
                int d = rng.chance(0.5) ? 1 : -1; // the second bar below or above
                int y = a.y;
                int lo = std::min(a.x, b.x);
                int hi = std::max(a.x, b.x);
                int xa = s > 0 ? rng.range(lo + 10, hi - length - k - 10) : rng.range(lo + length + k + 10, hi - 10);
                int xb = xa + s * (length - 1); // the bars' far end
                int xc = xa + s * (length + k); // back on the trace's row
                int bx0 = std::min(xa, xc) - 2;
                int bx1 = std::max(xa, xc) + 3;
                int by0 = d > 0 ? y - 3 : y - 7;
                int by1 = d > 0 ? y + 8 : y + 4;
                if (by0 < 0 || by1 > cv.h || bx0 < 0 || bx1 > cv.w) {
                    continue;
                }
                bool room = true;
                for (int yy = by0; yy < by1 && room; yy++) {
                    for (int xx = bx0; xx < bx1 && room; xx++) {
                        room = !cv.interior.at(xx, yy) && (yy == y || !cv.hole.at(xx, yy));
                    }
                }
                if (!room) {
                    continue;
                }
                int ya = y + 4 * d;
                int left = std::min(xa, xb);
                int right = std::max(xa, xb) + 1;
                cv.rectHole(left, y - 1, right, y + 2);
                cv.rectHole(left, ya - 1, right, ya + 2);
                // Off the trace's own row from the bars on, until it steps back.
                for (int x = xb + s; x != xc + s; x += s) {
                    cv.hole.at(x, y) = 0;
                    cv.trace.at(x, y) = 0;
                }
                cv.erased = true;
                cv.plot({ { xb, ya }, { xc - s * 4, ya }, { xc, y } });
                return true;
            }
            return false;
        }

        // No line in the originals ends in the open. Whatever still does (an overshoot past a
        // junction, a trace a pixel short of another) is cut back to where it meets something:
        // a line end is a cut-out with one cut-out neighbour, not on the window's edge, and slots
        // and what is kept clear with them (the connector, whose lines stop short of its dots
        // on purpose) are left alone. Then whatever that leaves floating goes too.
        void pruneLooseEnds(Canvas& cv)
        {
            int w = cv.w;
            int h = cv.h;
            auto onEdge = [&](int x, int y) { return x == 0 || y == 0 || x == w - 1 || y == h - 1; };
            Mask count(w, h);
            auto neighbours = [&](int x, int y) {
                int n = 0;
                for (int dy = -1; dy <= 1; dy++) {
                    for (int dx = -1; dx <= 1; dx++) {
                        if ((dx || dy) && cv.inside(x + dx, y + dy) && cv.hole.at(x + dx, y + dy)) {
                            n++;
                        }
                    }
                }
                return n;
            };
            // An end: one neighbour, or two that share a side (a pixel stub on a line's end, whose
            // pixels each have two neighbours). A corner's two touch only diagonally and stay.
            auto spur = [&](int x, int y) {
                Point found[2];
                int n = 0;
                for (int dy = -1; dy <= 1; dy++) {
                    for (int dx = -1; dx <= 1; dx++) {
                        if ((dx || dy) && cv.inside(x + dx, y + dy) && cv.hole.at(x + dx, y + dy) && n < 2) {
                            found[n++] = { x + dx, y + dy };
                        }
                    }
                }
                return n == 2 && std::abs(found[0].x - found[1].x) + std::abs(found[0].y - found[1].y) == 1;
            };
            auto loose = [&](int x, int y) {
                if (!cv.hole.at(x, y) || onEdge(x, y) || cv.keep.at(x, y)) {
                    return false;
                }
                return count.at(x, y) == 1 || (count.at(x, y) == 2 && spur(x, y));
            };
            std::deque<Point> queue;
            for (int y = 0; y < h; y++) {
                for (int x = 0; x < w; x++) {
                    count.at(x, y) = static_cast<uint8_t>(neighbours(x, y));
                    if (loose(x, y)) {
                        queue.push_back({ x, y });
                    }
                }
            }
            while (!queue.empty()) {
                Point p = queue.front();
                queue.pop_front();
                if (!loose(p.x, p.y)) {
                    continue;
                }
                cv.hole.at(p.x, p.y) = 0;
                cv.trace.at(p.x, p.y) = 0;
                for (int dy = -1; dy <= 1; dy++) {
                    for (int dx = -1; dx <= 1; dx++) {
                        int x = p.x + dx;
                        int y = p.y + dy;
                        if ((dx || dy) && cv.inside(x, y)) {
                            count.at(x, y)--;
                            if (loose(x, y)) {
                                queue.push_back({ x, y });
                            }
                        }
                    }
                }
            }
            // What is joined to a slot, the connector or the window's edge stays.
            Mask held(w, h);
            for (int y = 0; y < h; y++) {
                for (int x = 0; x < w; x++) {
                    if (cv.hole.at(x, y) && (cv.keep.at(x, y) || onEdge(x, y))) {
                        held.at(x, y) = 1;
                        queue.push_back({ x, y });
                    }
                }
            }
            while (!queue.empty()) {
                Point p = queue.front();
                queue.pop_front();
                for (int dy = -1; dy <= 1; dy++) {
                    for (int dx = -1; dx <= 1; dx++) {
                        int x = p.x + dx;
                        int y = p.y + dy;
                        if (cv.inside(x, y) && cv.hole.at(x, y) && !held.at(x, y)) {
                            held.at(x, y) = 1;
                            queue.push_back({ x, y });
                        }
                    }
                }
            }
            for (size_t i = 0; i < cv.hole.bits.size(); i++) {
                if (cv.hole.bits[i] && !held.bits[i]) {
                    cv.hole.bits[i] = 0;
                    cv.trace.bits[i] = 0;
                }
            }
        }

        // ---------------------------------------------------------------------------------
        // Shading.

        // The glow next to the cut-outs and off the window's left and top edges: an
        // exp(-r / 2.2) blur of them, as the prototype's, made here from six triple box blurs
        // (each a box 2r+1 wide, three times across and three times down) in whole numbers,
        // weighted as fitted to it (within 0.007 of it, next to a slot 0.85).
        std::vector<double> glowField(const Mask& hole)
        {
            constexpr std::array<int, 6> radii { 1, 2, 3, 4, 6, 9 };
            constexpr std::array<double, 6> weights { 0.34344, 0.43444, 0.54701, 0.39348, 0.24007, 0.00886 };
            constexpr int pad = 3 * 9 + 2;
            int pw = hole.w + 2 * pad;
            int ph = hole.h + 2 * pad;
            auto at = [pw](int x, int y) { return static_cast<size_t>(y) * static_cast<size_t>(pw) + static_cast<size_t>(x); };
            // At most 19^6 after the six sums, well inside 32 bits; the running sums down a column
            // get 64.
            std::vector<int32_t> source(static_cast<size_t>(pw) * static_cast<size_t>(ph), 0);
            for (int y = 0; y < ph; y++) {
                for (int x = 0; x < pw; x++) {
                    int ix = x - pad;
                    int iy = y - pad;
                    bool in = ix >= 0 && iy >= 0 && ix < hole.w && iy < hole.h;
                    source[at(x, y)] = (x < pad || y < pad || (in && hole.at(ix, iy))) ? 1 : 0;
                }
            }
            std::vector<double> glow(static_cast<size_t>(hole.w) * static_cast<size_t>(hole.h), 0.0);
            std::vector<int32_t> a;
            std::vector<int32_t> b(source.size());
            std::vector<int64_t> line(static_cast<size_t>(pw) + 1);
            std::vector<int64_t> down(static_cast<size_t>(pw) * static_cast<size_t>(ph + 1));
            for (size_t r = 0; r < radii.size(); r++) {
                int radius = radii[r];
                a = source;
                // A box sum 2r+1 long along each row, zero past its ends.
                auto across = [&]() {
                    for (int y = 0; y < ph; y++) {
                        line[0] = 0;
                        for (int x = 0; x < pw; x++) {
                            line[static_cast<size_t>(x) + 1] = line[static_cast<size_t>(x)] + a[at(x, y)];
                        }
                        for (int x = 0; x < pw; x++) {
                            int lo = std::max(0, x - radius);
                            int hi = std::min(pw, x + radius + 1);
                            b[at(x, y)] = static_cast<int32_t>(line[static_cast<size_t>(hi)] - line[static_cast<size_t>(lo)]);
                        }
                    }
                    std::swap(a, b);
                };
                // And down each column, from running sums kept a row at a time.
                auto downward = [&]() {
                    std::fill(down.begin(), down.begin() + pw, 0);
                    for (int y = 0; y < ph; y++) {
                        for (int x = 0; x < pw; x++) {
                            down[at(x, y + 1)] = down[at(x, y)] + a[at(x, y)];
                        }
                    }
                    for (int y = 0; y < ph; y++) {
                        int lo = std::max(0, y - radius);
                        int hi = std::min(ph, y + radius + 1);
                        for (int x = 0; x < pw; x++) {
                            b[at(x, y)] = static_cast<int32_t>(down[at(x, hi)] - down[at(x, lo)]);
                        }
                    }
                    std::swap(a, b);
                };
                for (int i = 0; i < 3; i++) {
                    across();
                }
                for (int i = 0; i < 3; i++) {
                    downward();
                }
                double side = 2.0 * radius + 1.0;
                double scale = weights[r] / (side * side * side * side * side * side);
                for (int y = 0; y < hole.h; y++) {
                    for (int x = 0; x < hole.w; x++) {
                        glow[static_cast<size_t>(y) * static_cast<size_t>(hole.w) + static_cast<size_t>(x)] += static_cast<double>(a[at(x + pad, y + pad)]) * scale;
                    }
                }
            }
            return glow;
        }

        std::vector<uint8_t> shade(const Canvas& cv, const Shading& sh, Random& rng)
        {
            int w = cv.w;
            int h = cv.h;
            std::vector<double> glow = glowField(cv.hole);
            double fallX = sh.fallX * referenceWidth / w;
            double fallY = sh.fallY * referenceHeight / h;
            std::vector<uint8_t> bgra(static_cast<size_t>(w) * static_cast<size_t>(h) * 4, 0);
            for (int y = 0; y < h; y++) {
                double row = (y % 2 == 0) ? 1.0 + sh.scan : 1.0 - sh.scan;
                for (int x = 0; x < w; x++) {
                    size_t i = static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x);
                    double base = std::max(sh.topLeft - fallX * x - fallY * y, sh.baseMin);
                    double v;
                    if (cv.interior.bits[i]) {
                        // Field and text box insides are flat.
                        v = base * (cv.dim[i] >= 0.0f ? static_cast<double>(cv.dim[i]) : 1.0);
                    } else {
                        double g = std::max(glow[i], 0.0);
                        g = sh.glowSat * (1.0 - exactExp(-g / sh.glowSat));
                        v = base * (1.0 + sh.glow * g);
                    }
                    // Noise from the sum of four even ones, near enough a bell and no library call.
                    double noise = (rng.unit() + rng.unit() + rng.unit() + rng.unit() - 2.0) * 1.7320508075688772 * sh.noise;
                    v = std::clamp(v * row + noise, sh.lo, sh.hi);
                    if (cv.hole.bits[i]) {
                        continue;
                    }
                    auto grey = static_cast<uint8_t>(std::floor(v + 0.5));
                    bgra[i * 4 + 0] = grey;
                    bgra[i * 4 + 1] = grey;
                    bgra[i * 4 + 2] = grey;
                    bgra[i * 4 + 3] = 255;
                }
            }
            return bgra;
        }

        Side sideFacing(const Item& a, const Item& b)
        {
            if (b.y >= a.y1()) {
                return Side::Top;
            }
            if (b.y1() <= a.y) {
                return Side::Bottom;
            }
            return 2 * b.x + b.w > 2 * a.x + a.w ? Side::Left : Side::Right;
        }

        // Where along a slot's side a port may go: off the corners and their chamfers.
        std::pair<int, int> sideSpan(const Item& it, Side side)
        {
            const Chamfer& c = it.chamfer;
            if (side == Side::Top || side == Side::Bottom) {
                int margin = c.largest() + 4;
                return { it.x + margin, it.x1() - margin };
            }
            int c0 = side == Side::Left ? c.tl : c.tr;
            int c1 = side == Side::Left ? c.bl : c.br;
            return { it.y + c0 + 3, it.y1() - c1 - 3 };
        }
    }

    Backdrop generateBackdrop(const MenuLayout& layout, uint64_t seed)
    {
        Backdrop out;
        out.width = layout.width;
        out.height = layout.height;
        if (layout.width < 32 || layout.height < 32) {
            out.bgra.assign(static_cast<size_t>(std::max(0, layout.width)) * static_cast<size_t>(std::max(0, layout.height)) * 4, 0);
            return out;
        }
        Random rng(seed);
        const Shading sh;
        const Routing rc;
        int W = layout.width;
        int H = layout.height;
        Canvas cv(W, H);
        std::vector<Item> items;
        for (const MenuSlot& slot : layout.slots) {
            items.push_back(itemFor(slot));
            cv.addItem(items.back());
        }
        if (layout.drawn.size() == cv.drawn.bits.size()) {
            cv.drawn.bits = layout.drawn;
        }

        // The corner of the frame, as on every original: not lines round the whole window, but
        // one down the left from the top edge to the first slot against it, which it turns into,
        // and one along the top from there for about half the width, ending on a line in from
        // the top edge. Traces come in from the rest of the edges below (the edge ports).
        constexpr int frameLeft = 4;
        constexpr int frameTop = 4;
        int across = rng.range(W * 2 / 5, W * 11 / 20);
        cv.plot({ { across, 0 }, { across, frameTop }, { frameLeft, frameTop }, { frameLeft, 0 } });
        const Item* first = nullptr;
        for (const Item& it : items) {
            if (it.x <= 12 && it.h >= 12 && it.y > frameTop + 8 && (!first || it.y < first->y)) {
                first = &it;
            }
        }
        if (first) {
            int at = first->y + first->h * 4 / 5;
            cv.plot({ { frameLeft, frameTop }, { frameLeft, at }, { first->x - 1, at } });
        }
        // The window's corners cut: a little at the top, the bottom left at 45 degrees.
        constexpr int corner = 12;
        for (int i = 0; i < corner; i++) {
            for (int x = 0; x < corner - i; x++) {
                cv.hole.at(x, H - 1 - i) = 1;
            }
        }
        for (Point p : { Point { 0, 0 }, Point { 1, 0 }, Point { 0, 1 }, Point { W - 4, 0 }, Point { W - 3, 0 }, Point { W - 2, 0 }, Point { W - 1, 0 }, Point { W - 2, 1 }, Point { W - 1, 1 }, Point { W - 1, 2 }, Point { W - 1, 3 } }) {
            cv.hole.at(p.x, p.y) = 1;
        }

        addTabs(cv, items, rng);
        std::vector<Item> buttons;
        for (const Item& it : items) {
            if (solid(it.kind)) {
                buttons.push_back(it);
            }
        }
        addConnector(cv, buttons, rng);
        Router router(cv, rc);
        int couplers = 0;

        // Pads, where a trace meets another and at the corner of a marked-off area: cut after
        // the loose ends, so a pad never holds up the cutting back of a line that leads nowhere,
        // and only where the lines still meet (at least "ways" of the four sides cut).
        struct Pad {
            Point at;
            int ways;
        };

        std::vector<Pad> pads;

        // Ports, mostly on the tops and bottoms: buttons one or two, fields and text boxes up to two.
        struct Want {
            size_t item;
            Side side;
            int at;
        };

        std::vector<Want> ports;
        for (size_t i = 0; i < items.size(); i++) {
            const Item& it = items[i];
            constexpr std::array<int, 5> buttonPorts { 1, 1, 2, 2, 2 };
            constexpr std::array<int, 4> otherPorts { 0, 1, 1, 2 };
            int n = solid(it.kind) ? buttonPorts[static_cast<size_t>(rng.range(0, 5))] : otherPorts[static_cast<size_t>(rng.range(0, 4))];
            for (int k = 0; k < n; k++) {
                double roll = rng.unit();
                Side side = roll < 0.4 ? Side::Top : roll < 0.8 ? Side::Bottom
                    : roll < 0.9                                ? Side::Left
                                                                : Side::Right;
                auto [lo, hi] = sideSpan(it, side);
                if (hi - lo < 4) {
                    continue;
                }
                ports.push_back({ i, side, rng.range(lo, hi) });
            }
        }
        rng.shuffle(ports);

        for (const Want& want : ports) {
            const Item& it = items[want.item];
            router.refresh();
            router.setLane(rng.range(0, rc.laneMax + 1));
            std::optional<Port> a = router.port(it, want.side, want.at);
            if (!a) {
                continue;
            }
            std::optional<Polyline> points;
            double roll = rng.unit();
            if (roll < rc.bracketChance && (want.side == Side::Top || want.side == Side::Bottom)) {
                // Back into the same slot further along: a bracket that runs beside it.
                auto [lo, hi] = sideSpan(it, want.side);
                std::vector<int> candidates;
                for (int x = lo; x < hi; x += rc.pitch) {
                    if (std::abs(x - want.at) >= 30) {
                        candidates.push_back(x);
                    }
                }
                if (!candidates.empty()) {
                    std::optional<Port> b = router.port(it, want.side, candidates[static_cast<size_t>(rng.range(0, static_cast<int>(candidates.size())))]);
                    if (b) {
                        points = router.route(*a, &*b, 1.3, true);
                    }
                }
            } else if (roll < rc.bracketChance + rc.linkChance && items.size() > 1) {
                // To another slot, a near one more likely than a far one.
                std::vector<double> weights;
                double total = 0.0;
                for (size_t o = 0; o < items.size(); o++) {
                    double weight = 0.0;
                    if (o != want.item) {
                        double dx = (items[o].x + items[o].w / 2.0) - (it.x + it.w / 2.0);
                        double dy = (items[o].y + items[o].h / 2.0) - (it.y + it.h / 2.0);
                        double d = std::sqrt(dx * dx + dy * dy) + 40.0;
                        weight = 1.0 / (d * d);
                    }
                    weights.push_back(weight);
                    total += weight;
                }
                double pick = rng.unit() * total;
                size_t other = 0;
                for (; other + 1 < weights.size() && (pick >= weights[other] || weights[other] == 0.0); other++) {
                    pick -= weights[other];
                }
                if (other != want.item) {
                    const Item& o = items[other];
                    Side side = sideFacing(it, o);
                    auto [lo, hi] = sideSpan(o, side);
                    if (hi - lo >= 4) {
                        std::optional<Port> b = router.port(o, side, rng.range(lo, hi));
                        if (b) {
                            points = router.route(*a, &*b, 1.8, true);
                        }
                    }
                }
            }
            if (!points) {
                points = router.route(*a, nullptr, 0.0, true);
            }
            if (!points) {
                continue;
            }
            Polyline line = bevel(*points, rng, rc.bevelChance);
            cv.plot(line);
            if (couplers < rc.couplers && rng.chance(0.2) && addCoupler(cv, line, rng)) {
                couplers++;
            }
            // A pad, now and then, where it ends.
            Point end = line.back();
            if (rng.chance(0.25) && end.x >= 1 && end.x < W - 2 && end.y >= 1 && end.y < H - 2) {
                pads.push_back({ end, 3 });
            }
        }

        // The empty areas, each marked off by an L of single lines running on until they meet
        // something, as in the originals: no far sides, no lines across, no area inside another.
        Mask used(W, H);
        for (int n = 0; n < rc.plates; n++) {
            router.refresh();
            int p = rc.pitch;
            Mask busySource(W, H);
            for (size_t i = 0; i < busySource.bits.size(); i++) {
                busySource.bits[i] = (cv.hole.bits[i] || cv.interior.bits[i] || cv.drawn.bits[i]) ? 1 : 0;
            }
            AreaSum busy(busySource);
            int gw = router.gridWidth();
            int gh = router.gridHeight();
            std::vector<uint8_t> free(static_cast<size_t>(gw) * static_cast<size_t>(gh), 0);
            for (int gy = 0; gy < gh; gy++) {
                for (int gx = 0; gx < gw; gx++) {
                    // Not along the window's edges, where only traces in from them run.
                    bool edge = gx * p < 8 || gy * p < 8 || gx * p >= W - 8 || gy * p >= H - 8;
                    free[static_cast<size_t>(gy) * static_cast<size_t>(gw) + static_cast<size_t>(gx)] = !edge && !busy.any(gx * p, gy * p, 10) && !used.at(gx * p, gy * p);
                }
            }
            std::optional<Rect> found = largestRect(free, gw, gh);
            if (!found) {
                break;
            }
            int x0 = found->x0 * p;
            int y0 = found->y0 * p;
            int x1 = found->x1 * p;
            int y1 = found->y1 * p;
            if (x1 - x0 < rc.plateMinW || y1 - y0 < rc.plateMinH) {
                break;
            }
            // A big empty area takes several, not one: cut this one down against a corner of it.
            if (x1 - x0 > rc.plateMaxW) {
                int w = rng.range(std::max(rc.plateMinW, rc.plateMaxW / 2), rc.plateMaxW + 1) / p * p;
                if (rng.chance(0.5)) {
                    x1 = x0 + w;
                } else {
                    x0 = x1 - w;
                }
            }
            if (y1 - y0 > rc.plateMaxH) {
                int h = rng.range(std::max(rc.plateMinH, rc.plateMaxH / 2), rc.plateMaxH + 1) / p * p;
                if (rng.chance(0.5)) {
                    y1 = y0 + h;
                } else {
                    y0 = y1 - h;
                }
            }
            auto run = [&](Point from, int dx, int dy) {
                while (cv.inside(from.x + dx, from.y + dy) && !cv.hole.at(from.x + dx, from.y + dy) && !cv.interior.at(from.x + dx, from.y + dy) && !cv.drawn.at(from.x + dx, from.y + dy)) {
                    from = { from.x + dx, from.y + dy };
                }
                return from;
            };
            int ex = run({ x0, y0 }, 1, 0).x;
            int ey = run({ x0, y0 }, 0, 1).y;
            // An L with a leg that stops at once is a stub going nowhere, which the cutting back
            // of loose ends cannot see (its pixels touch at the corners): the area is left plain.
            if (ex - x0 < 8 || ey - y0 < 8) {
                for (int y = y0; y < std::min(y1, H); y++) {
                    for (int x = x0; x < std::min(x1, W); x++) {
                        used.at(x, y) = 1;
                    }
                }
                continue;
            }
            cv.plot({ { x0, ey }, { x0, y0 }, { ex, y0 } });
            for (int y = y0; y <= ey; y++) {
                for (int x = x0; x <= ex; x++) {
                    used.at(x, y) = 1;
                }
            }
            if (rng.chance(0.4)) {
                pads.push_back({ { x0, y0 }, 2 });
            }
        }

        // Traces in from the window's edges, joining the others; more for a bigger window.
        int wanted = static_cast<int>(std::floor(static_cast<double>(rc.edgePorts) * W * H / (referenceWidth * referenceHeight) + 0.5));
        int made = 0;
        for (int attempt = 0; attempt < wanted * 3 && made < wanted; attempt++) {
            double roll = rng.unit();
            // Mostly the top and left, as in the originals.
            Side side = roll < 0.35 ? Side::Top : roll < 0.65 ? Side::Left
                : roll < 0.8                                  ? Side::Bottom
                                                              : Side::Right;
            bool upright = side == Side::Left || side == Side::Right;
            int at = rng.range(8, (upright ? H - 16 : W) - 8);
            router.refresh();
            std::optional<Port> a = router.edgePort(side, at);
            if (!a) {
                continue;
            }
            std::optional<Polyline> points = router.route(*a, nullptr, 0.0, false);
            if (!points) {
                continue;
            }
            cv.plot(bevel(*points, rng, rc.bevelChance));
            made++;
        }

        pruneLooseEnds(cv);
        for (const Pad& pad : pads) {
            Point p = pad.at;
            if (p.x < 2 || p.y < 2 || p.x >= W - 2 || p.y >= H - 2 || !cv.hole.at(p.x, p.y)) {
                continue;
            }
            // Counted past the pad's own edge, so a line it would cover whole does not count.
            int ways = cv.hole.at(p.x - 2, p.y) + cv.hole.at(p.x + 2, p.y) + cv.hole.at(p.x, p.y - 2) + cv.hole.at(p.x, p.y + 2);
            if (ways >= pad.ways) {
                cv.rectHole(p.x - 1, p.y - 1, p.x + 2, p.y + 2);
            }
        }
        out.bgra = shade(cv, sh, rng);
        return out;
    }

    uint64_t backdropSeed(std::string_view screen)
    {
        // Seeds picked by looking (--dump-menu-layouts writes each screen's art); any other
        // screen takes its name's FNV-1a.
        struct Picked {
            std::string_view screen;
            uint64_t seed;
        };

        constexpr std::array<Picked, 1> picked { {
            { "MenuScreenSound", 7 },
        } };
        for (const Picked& p : picked) {
            if (p.screen == screen) {
                return p.seed;
            }
        }
        uint64_t hash = 14695981039346656037ull;
        for (char c : screen) {
            hash ^= static_cast<uint8_t>(c);
            hash *= 1099511628211ull;
        }
        return hash;
    }
}
