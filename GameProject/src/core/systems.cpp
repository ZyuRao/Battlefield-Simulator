#include "gameworld.hpp"

TimeManager::TimeManager()
    : deltatime(0.f), initialized(false) {}

void TimeManager::reset() {
    initialized = false;
    deltatime = 0.f;
}

void TimeManager::tick() {
    auto now = std::chrono::steady_clock::now();
    if(!initialized) {
        lastTick = now;
        initialized = true;
        deltatime = 0.f;
        return;
    }
    std::chrono::duration<float> dt = now - lastTick;
    deltatime = dt.count();
    lastTick = now;

    // 可拓展接口：限制最大 dt（例如防止卡顿导致 1 秒跳帧）
    // const float maxDt = 0.1f;
    // if (deltaTime > maxDt) 
    //     deltaTime = maxDt;
}

static std::size_t decideThreadCount(std::size_t requested) {
    if(requested > 0) return requested;
    std::size_t hc = std::thread::hardware_concurrency();
    if(hc == 0) hc = 4;
    return hc;
}

TaskPool::TaskPool(std::size_t threadCount)
    : stopping(false)
{
    threadCount = decideThreadCount(threadCount);

    for(std::size_t i = 0; i < threadCount; i++){
        workers.emplace_back([this]() {
            while(true) {
                Job job;

                {
                    std::unique_lock<std::mutex> lock(queueMutex);

                    cv.wait(lock, [&]() {
                        return stopping || !jobs.empty();
                    });

                    if(stopping && jobs.empty()) return;

                    job = std::move(jobs.front());
                    jobs.pop();
                }

                job();
            }
        });
    }
}

TaskPool::~TaskPool() {
    shutdown();
}

void TaskPool::shutdown() {
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        stopping = true;
    }

    cv.notify_all();

    for(auto& t : workers) {
        if(t.joinable()) {
            t.join;
        }
    }

    workers.clear();
}

TaskPool::submit(Job job) {
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        jobs.push(std::move(job));
    }

    cv.notify_one();
}
