#include "core/gameworld.hpp"
#include <iostream>
#include <string>
#include <unordered_set>
#include <cstdint>
#include <cassert>
#include <cstdio>
#include <cstdlib>

#ifdef _WIN32
    #include <windows.h>
#else
    #include <thread>
    #include <chrono>
#endif
#ifndef NDEBUG
void TaskGroup::debugAbort(const char* msg, const TaskGroup* group, int value) {
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true, std::memory_order_relaxed)) {
        std::fprintf(stderr,
                     "[TaskGroup] %s group=%p remaining=%d\n",
                     msg,
                     static_cast<const void*>(group),
                     value);
        std::fflush(stderr);
    }
    std::abort();
}
#endif

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
    return (hc == 0 ? 4 : hc);
}

TaskGroup::~TaskGroup() {
#ifndef NDEBUG
    alive.store(false, std::memory_order_release);
    int remaining = count.load(std::memory_order_acquire);
    if (remaining != 0) {
        TaskGroup::debugAbort("destroy with remaining tasks", this, remaining);
    }
#else
    alive.store(false, std::memory_order_release);
#endif
}

void TaskGroup::add(int n) {
#ifndef NDEBUG
    if (!alive.load(std::memory_order_acquire)) {
        TaskGroup::debugAbort("add on dead group", this, count.load(std::memory_order_relaxed));
    }
#endif
    if (n <= 0) return;
    count.fetch_add(n, std::memory_order_relaxed);
}

void TaskGroup::done() {
#ifndef NDEBUG
    if (!alive.load(std::memory_order_acquire)) {
        debugAbort("done on dead group", this, count.load(std::memory_order_relaxed));
    }
#endif
    int prev = count.fetch_sub(1, std::memory_order_acq_rel);
#ifndef NDEBUG
    if (prev <= 0) {
        debugAbort("done underflow", this, prev);
    }
#endif
    if (prev == 1) {
        std::lock_guard<std::mutex> lock(mutex);
        cv.notify_all();
    }
}

void TaskGroup::wait() {
    std::unique_lock<std::mutex> lock(mutex);
    cv.wait(lock, [&]() {
        return count.load(std::memory_order_acquire) == 0;
    });
}

thread_local std::size_t TaskPool::tlsWorkerIndex = TaskPool::kInvalidWorkerIndex;

std::size_t TaskPool::workerIndex() {
    return tlsWorkerIndex;
}

TaskPool::TaskPool() : stopping(false), requestedThreads(0) {}

void TaskPool::init() {
    bool expected = false;
    if (!initialized.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return;
    }
    if (!workers.empty()) return;
    std::size_t tc = decideThreadCount(requestedThreads);
    workers.reserve(tc);
    for (std::size_t i = 0; i < tc; i++) {
        workers.emplace_back(&TaskPool::workerLoop, this, i);
    }
}

TaskPool::TaskPool(std::size_t threadCount)
    : stopping(false), requestedThreads(threadCount) {}

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
            t.join();
        }
    }

    workers.clear();
}

void TaskPool::submit(Job job, std::shared_ptr<TaskGroup> group) {
    // Ensure group accounting happens on the submitting thread.
    if (group) group->add(1);
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        jobs.push([job = std::move(job), group = std::move(group)]() mutable {
            job();
#ifndef NDEBUG
            if (group && !group->isAlive()) {
                debugAbort("worker before done on dead group", group.get(), group->debugCount());
            }
#endif
            if (group) group->done();
        });
    }

    cv.notify_one();
}

void TaskPool::workerLoop(std::size_t workerIndex) {
    tlsWorkerIndex = workerIndex;
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
}

BaseSystem::BaseSystem() {
    std::random_device rd;
    rng.seed(rd());
}

void BaseSystem::update(WorldDataContext& data, float dt) {
    if (data.baseA && !data.baseA->isDestroyed()) {
        data.baseA->update(dt, data, *this);
    }

    if (data.baseB && !data.baseB->isDestroyed()) {
        data.baseB->update(dt, data, *this);
    }
}

void BaseSystem::spawnUnit(UnitType t, Base& base, WorldDataContext& data) const{
    Coord spawnPos = findSpawnPos(base, data);
    if(spawnPos.x < 0 || spawnPos.y < 0) return;
    Faction fac = base.getFaction();

    try {
        auto makeUnit = [&](std::vector<std::shared_ptr<Unit>>& bucket) {
            auto u = UnitFactory::create(t, spawnPos, fac);
            data.registerUnit(u);
            bucket.emplace_back(std::move(u));
        };
        if (fac == Faction::A) {
            makeUnit(data.unitsA);
        } else {
            makeUnit(data.unitsB);
        }
    } catch (const std::exception& e) {
        // 极端情况下 new 失败也不要把程序崩了
        std::cerr << "[BaseSystem] spawnUnit failed: " << e.what() << "\n";
        return;
    }
}

Coord BaseSystem::findSpawnPos(const Base& base, const WorldDataContext& data) const {
    Coord c = base.getPos();

    std::vector<Coord> cand;
    std::vector<Coord> nbrs;
    cand.reserve(9);
    cand.push_back(c);
    data.map.getNeighbors8(data.map, c, nbrs);
    for(auto c : nbrs) {
        cand.push_back(c);
    }
    std::vector<Coord> valid;
    valid.reserve(cand.size());
    for(auto c : cand) {
        if (!data.map.inBounds(c)) continue;
        const Tile& t = data.map.getTile(c);
        if (!t.isPassable()) continue;
        if (!data.isTileFree(c)) continue;   // 不要踩到别的 unit/base
        valid.push_back(c);
    }
    if(valid.empty()) return Coord{-1, -1};

    std::uniform_int_distribution<std::size_t> dist(0,valid.size() - 1);
    return valid[dist(rng)];
}

void MovementSystem::update(WorldDataContext& data, float dt)
{
    const float maxMoveDt = 0.05f;
    if (dt > maxMoveDt) dt = maxMoveDt;

    std::unordered_set<std::uint64_t> occ;
    occ.reserve(data.unitsA.size() + data.unitsB.size() + 4);

    auto occupyIf = [&](bool ok, const Coord& c) {
        if (ok) occ.insert(Coord::packCoord(c));
    };

    // 先把基地、所有单位当前位置登记为“已占用”
    occupyIf(data.baseA && !data.baseA->isDestroyed(), data.baseA->getPos());
    occupyIf(data.baseB && !data.baseB->isDestroyed(), data.baseB->getPos());
    for (auto& u : data.unitsA) occupyIf(u && u->isAlive(), u->getPos());
    for (auto& u : data.unitsB) occupyIf(u && u->isAlive(), u->getPos());

    auto stepUnits = [&](std::vector<std::shared_ptr<Unit>>& units) {
        for (auto& u : units) {
            if (!u || !u->isAlive()) continue;
            Coord prev = u->getPos();

            // 让单位按自身逻辑尝试移动（dt 很小的话基本只会走 0/1 格）
            u->behavior->tickMovement(*u, dt, data.map);

            Coord now = u->getPos();
            if (now == prev) continue;

            const auto kPrev = Coord::packCoord(prev);
            const auto kNow  = Coord::packCoord(now);

            // 释放旧位置，再检查新位置是否被占
            occ.erase(kPrev);

            if (occ.find(kNow) != occ.end()) {
                // 冲突：回滚
                u->pos = prev;            // pos 在 Unit 里是 public（最小侵入）
                occ.insert(kPrev);
            } else {
                // 成功：占用新位置
                occ.insert(kNow);
            }
        }
    };

    // 为了减少“固定顺序”导致的偏置，每帧交替先更新 A/B
    static bool flip = false;
    flip = !flip;
    if (!flip) {
        stepUnits(data.unitsA);
        stepUnits(data.unitsB);
    } else {
        stepUnits(data.unitsB);
        stepUnits(data.unitsA);
    }
}

void VisionSystem::update(WorldDataContext& data)
{
    // A 阵营视野
   
    for (auto& u : data.unitsA) {
        if (u->isAlive()) {
            std::vector<std::weak_ptr<IAttackable>> forced;
            data.appendForcedReveals(Faction::A, forced);
            u->behavior->tickVision(*u, data.map, data.enemiesA, forced);
        }
    }

    // B 阵营视野
    for (auto& u : data.unitsB) {
        if (u->isAlive()) {
            std::vector<std::weak_ptr<IAttackable>> forced;
            data.appendForcedReveals(Faction::B, forced);
            u->behavior->tickVision(*u, data.map, data.enemiesB, forced);
        }
    }
}

void AttackSystem::update(WorldDataContext& data, float dt)
{
    // A 攻击
    for (auto& u : data.unitsA) {
        if (u->isAlive()) {
            u->behavior->tickAttack(*u, dt, data.map, data.enemiesA, data);
        }
    }

    // B 攻击
    for (auto& u : data.unitsB) {
        if (u->isAlive()) {
            u->behavior->tickAttack(*u, dt, data.map, data.enemiesB, data);
        }
    }
}

void CleanupSystem::update(WorldDataContext& data)
{
    // 清理单位 A
    data.unitsA.erase(
        std::remove_if(data.unitsA.begin(), data.unitsA.end(),
            [](const std::shared_ptr<Unit>& u){
                return u->isDestroyed();
            }),
        data.unitsA.end()
    );

    // 清理单位 B
    data.unitsB.erase(
        std::remove_if(data.unitsB.begin(), data.unitsB.end(),
            [](const std::shared_ptr<Unit>& u){
                return u->isDestroyed();
            }),
        data.unitsB.end()
    );

     auto cleanWeakVec = [](std::vector<std::weak_ptr<IAttackable>>& v) {
        v.erase(
            std::remove_if(v.begin(), v.end(),
                [](const std::weak_ptr<IAttackable>& w) { return w.expired(); }),
            v.end()
        );
    };

    cleanWeakVec(data.enemiesA);
    cleanWeakVec(data.enemiesB);
}
