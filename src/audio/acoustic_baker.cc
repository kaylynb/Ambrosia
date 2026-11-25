#include "audio/acoustic_baker.h"

#include "core/log.h"
#include "core/profile.h"
#include "core/workers.h"

#include <chrono>
#include <format>
#include <mutex>

namespace dx::audio::acoustics {
    const char* bakeStateName(BakeState state)
    {
        switch (state) {
        case BakeState::Idle:
            return "no level";
        case BakeState::Unavailable:
            return "unavailable";
        case BakeState::Checking:
            return "checking the cache";
        case BakeState::NotBaked:
            return "not baked";
        case BakeState::Baking:
            return "baking";
        case BakeState::Ready:
            return "in use";
        case BakeState::Failed:
            return "failed";
        }
        return "";
    }

    namespace {
        constexpr const char* NothingToBake = "the level has no floor to place probes on, so there is nothing to bake";

        // The file's size and age, for the report.
        void describeFile(BakeReport& report)
        {
            std::error_code error;
            report.fileBytes = 0;
            report.fileAgeSeconds = -1;
            if (!std::filesystem::is_regular_file(report.path, error)) {
                return;
            }
            report.fileBytes = std::filesystem::file_size(report.path, error);
            auto written = std::filesystem::last_write_time(report.path, error);
            if (!error) {
                report.fileAgeSeconds = std::chrono::duration<double>(std::filesystem::file_time_type::clock::now() - written).count();
            }
        }
    }

    struct Baker::Impl {
        mutable DX_LOCKABLE(mutex, "Acoustic baker");

        // The level, as last handed over (game thread), and the report and hand-over (both
        // threads, under mutex).
        std::string map;
        std::shared_ptr<const Geometry> geometry;
        std::vector<Material> materials;
        BakeSettings settings;
        BakeReport report;
        std::shared_ptr<const BakedData> ready;
        bool fresh = false;
        bool dropped = false;
        // Last, so it is stopped before the rest goes.
        workers::Worker job;

        ~Impl()
        {
            finish();
        }

        void finish()
        {
            job.stop();
        }

        void publish(std::shared_ptr<const BakedData> data)
        {
            std::lock_guard<dx::profile::Mutex> lock(mutex);
            ready = data;
            fresh = true;
            report.data = std::move(data);
            report.probes = report.data ? report.data->probes.size() : 0;
            report.state = BakeState::Ready;
        }

        void run(const std::stop_token& stop, std::string level, std::shared_ptr<const Geometry> shape, std::vector<Material> table, BakeSettings bake, bool baking, bool force)
        {
            CacheKey key = cacheKey(*shape, table, bake);
            std::filesystem::path path = cachePath(level);
            if (!force) {
                CacheRead read = readCache(path, key);
                {
                    std::lock_guard<dx::profile::Mutex> lock(mutex);
                    report.cache = read.state;
                    report.reason = read.reason;
                    describeFile(report);
                }
                if (read.state == CacheState::Valid) {
                    publish(std::move(read.data));
                    return;
                }
                if (read.state == CacheState::Stale || read.state == CacheState::Damaged) {
                    // A damaged file is broken data; a stale one is only out of date.
                    log::write(read.state == CacheState::Damaged ? log::Level::Error : log::Level::Warning, "DevAudio", std::format("AmbrosiaRaytracedAudio: the baked acoustics for {} are {} ({}), so they are not used{}", level, cacheStateName(read.state), read.reason, baking ? " and are baked again" : ""));
                }
                if (!baking) {
                    std::lock_guard<dx::profile::Mutex> lock(mutex);
                    report.state = BakeState::NotBaked;
                    return;
                }
            }
            if (placeProbes(*shape, bake).empty()) {
                std::lock_guard<dx::profile::Mutex> lock(mutex);
                report.state = BakeState::NotBaked;
                report.reason = NothingToBake;
                return;
            }
            {
                std::lock_guard<dx::profile::Mutex> lock(mutex);
                report.state = BakeState::Baking;
                report.stage = BakeStage::Probes;
                report.progress = 0;
            }
            auto start = std::chrono::steady_clock::now();
            std::string error;
            std::unique_ptr<BakedData> data = acoustics::bake(*shape, table, bake, [this](BakeStage stage, float progress) {
                std::lock_guard<dx::profile::Mutex> lock(mutex);
                report.stage = stage;
                report.progress = progress; }, &error, stop);
            if (stop.stop_requested()) {
                return;
            }
            double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            if (!data) {
                log::error("DevAudio", "AmbrosiaRaytracedAudio: baking {} failed: {}", level, error);
                std::lock_guard<dx::profile::Mutex> lock(mutex);
                report.state = BakeState::Failed;
                report.reason = error;
                return;
            }
            std::string writeError;
            bool written = writeCache(path, key, *data, &writeError);
            if (!written) {
                log::error("DevAudio", "AmbrosiaRaytracedAudio: the baked acoustics for {} are used but not kept: {}", level, writeError);
            }
            {
                std::lock_guard<dx::profile::Mutex> lock(mutex);
                report.bakeSeconds = seconds;
                report.cache = written ? CacheState::Valid : report.cache;
                report.reason = written ? std::string() : writeError;
                describeFile(report);
            }
            publish(std::shared_ptr<const BakedData>(std::move(data)));
        }

        void start(bool force, bool baking)
        {
            finish();
            {
                std::lock_guard<dx::profile::Mutex> lock(mutex);
                if (report.data) {
                    dropped = true;
                }
                ready.reset();
                fresh = false;
                report = {};
                report.map = map;
                if (!geometry) {
                    return;
                }
                if (!bakingAvailable()) {
                    report.state = BakeState::Unavailable;
                    report.reason = "Steam Audio's baking is not available";
                    return;
                }
                report.state = BakeState::Checking;
                report.path = cachePath(map);
            }
            job.start("Acoustic bake", [this, level = map, shape = geometry, table = materials, bake = settings, baking, force](std::stop_token stop) {
                DX_ZONE_NAMED("Acoustic bake job");
                run(stop, level, shape, table, bake, baking, force);
            });
        }
    };

    Baker::Baker()
        : impl(std::make_unique<Impl>())
    {
    }

    Baker::~Baker() = default;

    void Baker::setLevel(const std::string& map, std::shared_ptr<const Geometry> geometry, const std::vector<Material>& materials, const BakeSettings& settings, bool background)
    {
        impl->map = map;
        impl->geometry = std::move(geometry);
        impl->materials = materials;
        impl->settings = settings;
        impl->start(false, background);
    }

    void Baker::rebake()
    {
        if (impl->geometry) {
            impl->start(true, true);
        }
    }

    void Baker::setMaterials(const std::vector<Material>& materials)
    {
        bool same = materials.size() == impl->materials.size();
        for (size_t i = 0; same && i < materials.size(); i++) {
            const Material& a = materials[i];
            const Material& b = impl->materials[i];
            same = a.absorption == b.absorption && a.scattering == b.scattering && a.transmission == b.transmission;
        }
        if (same) {
            return;
        }
        impl->materials = materials;
        // Only looked up again, never baked on its own: a table being tuned changes many times.
        if (impl->geometry) {
            impl->start(false, false);
        }
    }

    std::shared_ptr<const BakedData> Baker::take(bool& dropped)
    {
        std::lock_guard<dx::profile::Mutex> lock(impl->mutex);
        dropped = impl->dropped;
        impl->dropped = false;
        if (!impl->fresh) {
            return nullptr;
        }
        impl->fresh = false;
        dropped = false;
        return impl->ready;
    }

    BakeReport Baker::report() const
    {
        std::lock_guard<dx::profile::Mutex> lock(impl->mutex);
        BakeReport report = impl->report;
        if (report.fileBytes > 0 && report.fileAgeSeconds >= 0) {
            describeFile(report);
        }
        return report;
    }

    void Baker::wait()
    {
        impl->job.join();
    }

    BatchResult bakeToCache(const std::string& map, const Geometry& geometry, const std::vector<Material>& materials, const BakeSettings& settings, bool force, const BakeProgress& progress)
    {
        BatchResult result;
        CacheKey key = cacheKey(geometry, materials, settings);
        std::filesystem::path path = cachePath(map);
        if (!force) {
            CacheRead read = readCache(path, key);
            if (read.state == CacheState::Valid) {
                result.ok = true;
                result.skipped = true;
                result.probes = read.data->probes.size();
                result.spacing = read.data->spacing;
                std::error_code error;
                result.bytes = std::filesystem::file_size(path, error);
                result.message = "already baked";
                return result;
            }
            if (read.state != CacheState::Missing) {
                result.message = std::string(cacheStateName(read.state)) + " (" + read.reason + "), baked again; ";
            }
        }
        // A level with no floor (a menu's or the credits' backdrop) has nothing to bake.
        if (placeProbes(geometry, settings).empty()) {
            result.ok = true;
            result.skipped = true;
            result.message = NothingToBake;
            return result;
        }
        auto start = std::chrono::steady_clock::now();
        std::string error;
        std::unique_ptr<BakedData> data = bake(geometry, materials, settings, progress, &error);
        result.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        if (!data) {
            result.message += error;
            return result;
        }
        if (!writeCache(path, key, *data, &error)) {
            result.message += error;
            return result;
        }
        result.ok = true;
        result.probes = data->probes.size();
        result.spacing = data->spacing;
        std::error_code code;
        result.bytes = std::filesystem::file_size(path, code);
        result.message += path.string();
        return result;
    }
}
