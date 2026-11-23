// reforce extension (PLAN 5R.3): 3D models loaded on a worker. See ModelLoader.hpp.
#include "app/ModelLoader.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>

#include "engine/Log.hpp"
#include "engine/ModelFile.hpp"

namespace reforce::app {

ModelLoader::ModelLoader(const render::ModelPipeline& pipeline, int maxTexture, int cache)
    : pipeline_(pipeline), maxTexture_(maxTexture), cache_(std::max(1, cache)) {
    worker_ = std::thread([this] { run(); });
}

ModelLoader::~ModelLoader() {
    {
        std::lock_guard<std::mutex> lock(m_);
        stop_ = true;
        queue_.clear();
    }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
}

std::string ModelLoader::key(const std::filesystem::path& file, const std::array<float, 3>& front,
                             const std::array<float, 3>& up) {
    char axes[64];
    std::snprintf(axes, sizeof axes, "|%g,%g,%g|%g,%g,%g", front[0], front[1], front[2], up[0], up[1], up[2]);
    return file.lexically_normal().string() + axes;
}

void ModelLoader::request(const std::filesystem::path& file, const std::array<float, 3>& front,
                          const std::array<float, 3>& up) {
    const std::string k = key(file, front, up);
    std::lock_guard<std::mutex> lock(m_);
    ++clock_;
    for (auto& e : entries_)
        if (e.key == k) {
            e.used = clock_;
            return;
        }
    Entry e;
    e.key = k;
    e.file = file;
    e.front = front;
    e.up = up;
    e.used = clock_;
    entries_.push_back(std::move(e));
    queue_.push_back(k);
    cv_.notify_one();
}

ModelLoader::State ModelLoader::find(const std::string& k, std::shared_ptr<const render::ModelGpu>* out,
                                     std::string* error) {
    std::unique_lock<std::mutex> lock(m_, std::try_to_lock);  // never block the render thread
    if (!lock.owns_lock()) return State::Loading;
    for (const auto& e : entries_)
        if (e.key == k) {
            if (out) *out = e.gpu;
            if (error) *error = e.error;
            return e.state;
        }
    return State::Unknown;
}

void ModelLoader::run() {
    for (;;) {
        Entry job;
        {
            std::unique_lock<std::mutex> lock(m_);
            cv_.wait(lock, [&] { return stop_ || !queue_.empty(); });
            if (stop_) return;
            const std::string k = queue_.front();
            queue_.pop_front();
            auto it = std::find_if(entries_.begin(), entries_.end(), [&](const Entry& e) { return e.key == k; });
            if (it == entries_.end()) continue;
            job.key = it->key;
            job.file = it->file;
            job.front = it->front;
            job.up = it->up;
        }
        const auto t0 = std::chrono::steady_clock::now();
        engine::ModelLoadOptions lo;
        lo.front = job.front;
        lo.up = job.up;
        std::string error;
        std::shared_ptr<const render::ModelGpu> gpu;
        if (auto model = engine::loadModel(job.file, lo, &error)) {
            for (const auto& w : model->warnings) rlog("[reforce] model %s: %s\n", job.file.filename().c_str(), w.c_str());
            gpu = pipeline_.upload(*model, maxTexture_, &error);
        }
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        if (gpu)
            rlog("[reforce] model %s loaded: %zu triangles, %.0f MB textures, %.0f ms\n", job.file.filename().c_str(),
                 gpu->triangles(), double(gpu->textureBytes()) / 1048576.0, ms);
        else
            rlog("[reforce] model %s failed: %s\n", job.file.string().c_str(), error.c_str());
        std::lock_guard<std::mutex> lock(m_);
        auto it = std::find_if(entries_.begin(), entries_.end(), [&](const Entry& e) { return e.key == job.key; });
        if (it == entries_.end()) continue;
        it->state = gpu ? State::Ready : State::Failed;
        it->gpu = std::move(gpu);
        it->error = error;
        // Keep at most cache_ ready models: drop the least recently requested others.
        for (;;) {
            int ready = 0;
            for (const auto& e : entries_) ready += e.state == State::Ready;
            if (ready <= cache_) break;
            auto victim = entries_.end();
            for (auto e = entries_.begin(); e != entries_.end(); ++e)
                if (e->state == State::Ready && (victim == entries_.end() || e->used < victim->used)) victim = e;
            rlog("[reforce] model %s evicted from the cache\n", victim->file.filename().c_str());
            entries_.erase(victim);
        }
    }
}

}  // namespace reforce::app
