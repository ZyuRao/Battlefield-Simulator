#pragma once

#include "map.hpp"
#include "Iattackable.hpp"
#include "behavior.hpp"
#include <functional>
#include <chrono>
#include <thread>
#include <atomic>
#include <condition_variable>



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
    using Job = std::function<void()>;
private:
    std::vector<std::thread> workers;
    std::queue<Job>          jobs;
    std::mutex               queueMutex;
    std::condition_variable  cv;
    std::atomic<bool>        stopping;

public:
    explicit TaskPool(std::size_t threadCount = 0);
    ~TaskPool();

    void submit(Job job);

    void shutdown();
};

class MovementSystem {
public:
    MovementSystem() = default;

    void update(GameWorld& world, float dt);

};

class VisionSystem {
pubilc:
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
    BaseSystem() = default;

    void update(GameWorld& world, float dt);
    void spawnUnit(UnitType t, Base& base, GameWorld& world);

private:
    findSpawnPos(const Base& base, const GameWorld& world) const;
};

class RenderSystem {
public:
    RenderSystem() = default;
    ~RenderSystem() = default;

    void render(const GameWorld& world);
    //for ASCII
};

class GameWorld {
private:
    Map map;
    std::unique_ptr<Base> baseA;
    std::unique_ptr<Base> baseB;

    std::vector<std::unique_ptr<Unit>> unitsA;
    std::vector<std::unique_ptr<Unit>> unitsB;

    std::vector<IAttackable*> enemiesA;
    std::vector<IAttackable*> enemiesB;

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
    std::atomic<bool>             renderRunning;
public:
    GameWorld();
    ~GameWorld()
    
    void update();

    void startRenderThread();
    void stopRenderThread();

    
    bool isTileFree(const Coord& c) const;                    

};