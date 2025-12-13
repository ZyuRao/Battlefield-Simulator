#include "core/gameworld.hpp"
#include <iostream>
#include <string>
#include <windows.h>


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


TaskPool::TaskPool() : stopping(false) {}

void TaskPool::init(GameWorld* world) {
    worldPtr = world;

    std::size_t tc = decideThreadCount(workers.size());
    for(std::size_t i = 0; i < tc; i++) {
        workers.emplace_back(&TaskPool::workerLoop, this);
    }
}

TaskPool::TaskPool(std::size_t threadCount)
    : stopping(false)
{
    workers.reserve(decideThreadCount(threadCount));
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
            t.join();
        }
    }

    workers.clear();
}

void TaskPool::submit(Job job) {
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        jobs.push(std::move(job));
    }

    cv.notify_one();
}

void TaskPool::workerLoop() {
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

        {
            std::shared_lock<std::shared_mutex> lock(worldPtr->worldMutex);
            job(*worldPtr);
        }
    }
}

BaseSystem::BaseSystem() {
    std::random_device rd;
    rng.seed(rd());
}

void BaseSystem::update(GameWorld& world, float dt) {
    if (world.baseA && !world.baseA->isDestroyed()) {
        world.baseA->update(dt, world);
    }

    if (world.baseB && !world.baseB->isDestroyed()) {
        world.baseB->update(dt, world);
    }
}

void BaseSystem::spawnUnit(UnitType t, Base& base, GameWorld& world) const{
    Coord spawnPos = findSpawnPos(base, world);
    if(spawnPos.x < 0 || spawnPos.y < 0) return;
    Faction fac = base.getFaction();

    try {
        if (fac == Faction::A) {
            world.unitsA.emplace_back(
                std::make_shared<Unit>(t, spawnPos, fac)
            );
        } else {
            world.unitsB.emplace_back(
                std::make_shared<Unit>(t, spawnPos, fac)
            );
        }
    } catch (const std::exception& e) {
        // 极端情况下 new 失败也不要把程序崩了
        std::cerr << "[BaseSystem] spawnUnit failed: " << e.what() << "\n";
        return;
    }
}

Coord BaseSystem::findSpawnPos(const Base& base, const GameWorld& world) const {
    Coord c = base.getPos();

    std::vector<Coord> cand;
    std::vector<Coord> nbrs;
    cand.reserve(9);
    cand.push_back(c);
    world.map.getNeighbors8(world.map, c, nbrs);
    for(auto c : nbrs) {
        cand.push_back(c);
    }
    std::vector<Coord> valid;
    valid.reserve(cand.size());
    for(auto c : cand) {
        if (!world.map.inBounds(c)) continue;
        const Tile& t = world.map.getTile(c);
        if (!t.isPassable()) continue;
        if (!world.isTileFree(c)) continue;   // 不要踩到别的 unit/base
        valid.push_back(c);
    }
    if(valid.empty()) return Coord{-1, -1};

    std::uniform_int_distribution<std::size_t> dist(0,valid.size() - 1);
    return valid[dist(rng)];
}

void MovementSystem::update(GameWorld& world, float dt)
{
    // A 阵营
    for (auto& u : world.unitsA) {
        if (u->isAlive()) {
            u->behavior->tickMovement(*u, dt, world.map);
        }
    }

    // B 阵营
    for (auto& u : world.unitsB) {
        if (u->isAlive()) {
            u->behavior->tickMovement(*u, dt, world.map);
        }
    }
}

void VisionSystem::update(GameWorld& world)
{
    // A 阵营视野
   
    for (auto& u : world.unitsA) {
        if (u->isAlive()) {
            u->behavior->tickVision(*u, world.map, world.enemiesA);
        }
    }

    // B 阵营视野
    for (auto& u : world.unitsB) {
        if (u->isAlive()) {
            u->behavior->tickVision(*u, world.map, world.enemiesB);
        }
    }
}

void AttackSystem::update(GameWorld& world, float dt)
{
    // A 攻击
    for (auto& u : world.unitsA) {
        if (u->isAlive()) {
            u->behavior->tickAttack(*u, dt, world.map, world.enemiesA);
        }
    }

    // B 攻击
    for (auto& u : world.unitsB) {
        if (u->isAlive()) {
            u->behavior->tickAttack(*u, dt, world.map, world.enemiesB);
        }
    }
}

void CleanupSystem::update(GameWorld& world)
{
    // 清理单位 A
    world.unitsA.erase(
        std::remove_if(world.unitsA.begin(), world.unitsA.end(),
            [](const std::shared_ptr<Unit>& u){
                return u->isDestroyed();
            }),
        world.unitsA.end()
    );

    // 清理单位 B
    world.unitsB.erase(
        std::remove_if(world.unitsB.begin(), world.unitsB.end(),
            [](const std::shared_ptr<Unit>& u){
                return u->isDestroyed();
            }),
        world.unitsB.end()
    );

     auto cleanWeakVec = [](std::vector<std::weak_ptr<IAttackable>>& v) {
        v.erase(
            std::remove_if(v.begin(), v.end(),
                [](const std::weak_ptr<IAttackable>& w) { return w.expired(); }),
            v.end()
        );
    };

    cleanWeakVec(world.enemiesA);
    cleanWeakVec(world.enemiesB);
}


