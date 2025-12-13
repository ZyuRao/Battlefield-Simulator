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
#include <windows.h>
#include <SFML/Graphics.hpp>



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
    RenderSystem();

    void renderAscii(const GameWorld& world);
    //for ASCII
    void renderSfml(const GameWorld& world, sf::RenderWindow& window);

private:
    std::vector<std::string> lastBuffer;

    struct Layout {
        float tileSize;
        float offsetX;
        float offsetY;
        float hudX;
    };

    sf::Font hudFont;
    bool fontLoaded = false;

    // 工具函数
    void ensureFontLoaded();

    Layout   computeLayout(const GameWorld& world,
                           const sf::RenderWindow& window) const;
    sf::Color tileColor(TileType t) const;
    sf::Color factionColor(Faction f) const;
    sf::Color unitTypeColor(UnitType t) const;

    void drawMapLayer(const GameWorld& world,
                      sf::RenderWindow& window,
                      const Layout& layout);

    void drawBaseLayer(const GameWorld& world,
                       sf::RenderWindow& window,
                       const Layout& layout);

    void drawUnitLayer(const GameWorld& world,
                       sf::RenderWindow& window,
                       const Layout& layout);

    void drawHpBar(sf::RenderWindow& window,
                   sf::Vector2f center,
                   float width,
                   float hp,
                   float maxHp) const;

    void drawUnitIcon(sf::RenderWindow& window,
                      sf::Vector2f center,
                      UnitType type,
                      Faction faction);

    void drawHud(const GameWorld& world,
                 sf::RenderWindow& window,
                 const Layout& layout);
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