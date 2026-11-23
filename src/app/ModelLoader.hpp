#pragma once
// reforce extension (PLAN 5R.3, docs/model-design.md "Loader"): loads 3D models on a worker thread.
//
// request() queues a model (file + front / up axes); the worker parses it (engine::loadModel) and uploads it
// (render::ModelPipeline::upload), so neither ever runs on the render thread. find() is a non-blocking look-up for the
// render thread. Up to `cache` models stay resident (least recently requested evicted first; a live run keeps its
// own shared_ptr, so eviction never frees a model in use).

#include <array>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "render/ModelPipeline.hpp"

namespace reforce::app {

class ModelLoader {
public:
    enum class State { Unknown, Loading, Ready, Failed };

    ModelLoader(const render::ModelPipeline& pipeline, int maxTexture, int cache);
    ~ModelLoader();
    ModelLoader(const ModelLoader&) = delete;
    ModelLoader& operator=(const ModelLoader&) = delete;

    static std::string key(const std::filesystem::path& file, const std::array<float, 3>& front,
                           const std::array<float, 3>& up);
    // Queues the model unless it is loading or loaded; marks it recently used.
    void request(const std::filesystem::path& file, const std::array<float, 3>& front, const std::array<float, 3>& up);
    // Render thread: the state and, when Ready, the model. `error` gets the failure.
    State find(const std::string& key, std::shared_ptr<const render::ModelGpu>* out, std::string* error = nullptr);

private:
    struct Entry {
        std::string key;
        std::filesystem::path file;
        std::array<float, 3> front{}, up{};
        State state = State::Loading;
        std::shared_ptr<const render::ModelGpu> gpu;
        std::string error;
        unsigned long long used = 0;
    };
    void run();

    const render::ModelPipeline& pipeline_;
    int maxTexture_, cache_;
    std::mutex m_;
    std::condition_variable cv_;
    std::vector<Entry> entries_;
    std::deque<std::string> queue_;
    unsigned long long clock_ = 0;
    bool stop_ = false;
    std::thread worker_;
};

}  // namespace reforce::app
