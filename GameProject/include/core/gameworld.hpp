#pragma once

#include "map.hpp"
#include "Iattackable.hpp"
#include "behavior.hpp"
#include <functional>
#include <chrono>
#include <thread>
#include <atomic>
#include <condition_variable>
#include <shared_mutex>
#include <cassert>



class TimeManager {
private:
    std::chrono::steady_clock::time_point lastTick;
    float deltatime;
    bool initialized;
public:
    TimeManager();

    void reset();
    void tick();
    float getDeltaTime() const { return deltatime; }
};

class TaskPool {
public:
    using Job = std::function<void(GameWorld&)>;
private:
    GameWorld* worldPtr = nullptr;
    std::vector<std::thread> workers;
    std::queue<Job>          jobs;
    std::mutex               queueMutex;
    std::condition_variable  cv;
    std::atomic<bool>        stopping;

    void workerLoop();

public:
    TaskPool();
    TaskPool(std::size_t threadCount = 0);
    ~TaskPool();

    void submit(Job job);

    void shutdown();
    void init(GameWorld* world);
};

class MovementSystem {
public:
    MovementSystem() = default;

    void update(GameWorld& world, float dt);

};

class VisionSystem {
public:
    VisionSystem() = default;

    void update(GameWorld& world);
};
class AttackSystem {
public:
    AttackSystem() = default;

    void update(GameWorld& world, float dt);
};

class CleanupSystem {
public:
    CleanupSystem() = default;

    void update(GameWorld& world);
};

class BaseSystem {
public:
    BaseSystem();

    void update(GameWorld& world, float dt);
    void spawnUnit(UnitType t, Base& base, GameWorld& world) const;

private:

    mutable std::mt19937 rng;
    Coord findSpawnPos(const Base& base, const GameWorld& world) const;
};

class RenderSystem {
public:
    RenderSystem() = default;
    ~RenderSystem() = default;

    void render(const GameWorld& world);
    //for ASCII

private:
    std::vector<std::string> lastBuffer;
};

class GameWorld {
private:
    Map map;
    std::shared_ptr<Base> baseA;
    std::shared_ptr<Base> baseB;

    std::vector<std::shared_ptr<Unit>> unitsA;
    std::vector<std::shared_ptr<Unit>> unitsB;

    std::vector<std::weak_ptr<IAttackable>> enemiesA;
    std::vector<std::weak_ptr<IAttackable>> enemiesB;
     // --- 系统层 ---
    MovementSystem movementSystem;
    VisionSystem   visionSystem;
    AttackSystem   attackSystem;
    CleanupSystem  cleanupSystem;
    BaseSystem     baseSystem;

    // 统一时间与任务
    TimeManager    timeManager;
    TaskPool       taskPool;

    // --- 渲染相关（暂时只做接口占位） ---
    std::unique_ptr<RenderSystem> renderSystem;
    std::thread                   renderThread;
    std::atomic<bool>             renderRunning{false};

    mutable std::shared_mutex worldMutex;

    
    friend class TaskPool;
    friend class MovementSystem;
    friend class VisionSystem;
    friend class AttackSystem;
    friend class CleanupSystem;
    friend class BaseSystem;
    friend class RenderSystem;
    friend class Game;

public:
    GameWorld();
    ~GameWorld();
    
    void update();

    void startRenderThread();
    void stopRenderThread();


    const BaseSystem& getBaseSystem() const { return baseSystem; }
    
    bool isTileFree(const Coord& c) const; 
    
    void rebuildEnemies();


    
};