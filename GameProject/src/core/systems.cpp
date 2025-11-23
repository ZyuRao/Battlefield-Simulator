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


void BaseSystem::update(GameWorld& world, float dt) {
    if (world.baseA && !world.baseA->isDestroyed()) {
        world.baseA->update(dt, world);
    }

    if (world.baseB && !world.baseB->isDestroyed()) {
        world.baseB->update(dt, world);
    }
}

void BaseSystem::spawnUnit(UnitType t, Base& base, GameWorld& world) {
    Coord spawnPos = findSpawnPos(base, world);
    auto u = std::make_unique<Unit>(t, spawnPos, base.getFaction());

    if(base.getFaction() == Faction::A) {
        world.unitsA.push_back(std::move(u));
    } else {
        world.unitsB.pusn_back(std::move(u));
    }
}

Coord BaseSystem::findSpawnPos(const Base& base, const GameWorld& world) {
    Coord c = base.getPos();

    std::vector<Coord> nbrs;
    world.map.getNeighbors8(world.map, c, nbrs);
    for(const Coord& n : nbrs) {
        if(world.map.getTile(n).isPassable() && world.isTileFree(n)) {
            return n;
        }
    }

    if(world.map.getTile(c).isPassable() && world.isTileFree(c)) return c
}

void MovementSystem::update(GameWorld& world, float dt)
{
    // A 阵营
    for (auto& u : world.getUnitsA()) {
        if (u->isAlive()) {
            u->behavior->tickMovement(*u, dt, world.map);
        }
    }

    // B 阵营
    for (auto& u : world.getUnitsB()) {
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
            u->behavior->tickAttack(*u, dt, world.map);
        }
    }

    // B 攻击
    for (auto& u : world.unitsB) {
        if (u->isAlive()) {
            u->behavior->tickAttack(*u, dt, world.map);
        }
    }
}

void CleanupSystem::update(GameWorld& world)
{
    // 清理单位 A
    world.unitsA.erase(
        std::remove_if(world.unitsA.begin(), world.unitsA.end(),
            [](const std::unique_ptr<Unit>& u){
                return u->isDestroyed();
            }),
        world.unitsA.end()
    );

    // 清理单位 B
    world.unitsB.erase(
        std::remove_if(world.unitsB.begin(), world.unitsB.end(),
            [](const std::unique_ptr<Unit>& u){
                return u->isDestroyed();
            }),
        world.unitsB.end()
    );
}
