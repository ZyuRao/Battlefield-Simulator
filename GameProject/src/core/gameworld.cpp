#include "core/Iattackable.hpp"
#include "core/gameworld.hpp"
#include <chrono>
#include <algorithm>
#include <iostream>


namespace {
    constexpr int MAP_WIDTH = 40;
    constexpr int MAP_HEIGHT = 10;

    // 在一个矩形区域内寻找第一个可通行格子
    bool findPassableInRegion(const Map& map,
                              int x0, int y0, int x1, int y1,
                              Coord& out)
    {
        x0 = std::max(0, x0);
        y0 = std::max(0, y0);
        x1 = std::min(map.getWidth()  - 1, x1);
        y1 = std::min(map.getHeight() - 1, y1);

        for (int y = y0; y <= y1; ++y) {
            for (int x = x0; x <= x1; ++x) {
                Coord c{x, y};
                if (map.getTile(c).isPassable()) {
                    out = c;
                    return true;
                }
            }
        }
        return false;
    }
    bool pickBasePositions(const Map& map, Coord& baseA, Coord& baseB)
    {
        const int w = map.getWidth();
        const int h = map.getHeight();

        // 左上角附近
        Coord a;
        if (!findPassableInRegion(map,
                                  1, 1,
                                  std::min(5, w-2), std::min(5, h-2),
                                  a)) {
            return false;
        }

        // 右下角附近
        Coord b;
        if (!findPassableInRegion(map,
                                  std::max(w-6, 1), std::max(h-6, 1),
                                  w-2, h-2,
                                  b)) {
            return false;
        }

        if (!map.isReachable(a, b)) {
            return false;
        }

        baseA = a;
        baseB = b;
        return true;
    }
} 

GameWorld::GameWorld()
    : map(MAP_HEIGHT, MAP_HEIGHT), baseSystem()
    , renderRunning(false), taskPool(4)
{
    MapGenerator gen(MAP_WIDTH, MAP_HEIGHT);

    bool ok = false;
    Coord baseAPos;
    Coord baseBPos;

    while (!ok) {
        Map candidate = gen.generate();

        if (pickBasePositions(candidate, baseAPos, baseBPos)) {
            map = std::move(candidate);
            ok = true;
        }
    }
    baseA = std::make_shared<Base>(baseAPos, Faction::A);
    baseB = std::make_shared<Base>(baseBPos, Faction::B);

    enemiesA.clear();
    enemiesB.clear();

    
    taskPool.init(this);
    renderSystem = std::make_unique<RenderSystem>();
}


GameWorld::~GameWorld() {
    stopRenderThread();
}

void GameWorld::update() {
    static int tick = 0;
    ++tick;
    // std::cout << "\n[World] Tick " << tick << "\n";

    std::unique_lock<std::shared_mutex> lock(worldMutex);
    timeManager.tick();
    float dt = timeManager.getDeltaTime();
    // std::cout << "  dt=" << dt << "\n";

    // std::cout << "  BaseSystem...\n";
    baseSystem.update(*this, dt);
    rebuildEnemies();

    // std::cout << "  VisionSystem...\n";
    visionSystem.update(*this);

    // std::cout << "  MovementSystem...\n";
    movementSystem.update(*this, dt);

    // std::cout << "  AttackSystem...\n";
    attackSystem.update(*this, dt);

    // std::cout << "  CleanupSystem...\n";
    cleanupSystem.update(*this);
}


bool GameWorld::isTileFree(const Coord& c) const {
    for(auto& u : unitsA) {
        if(u->isAlive() && u->getPos() == c) return false;

    }

    for(auto& u : unitsB) {
        if(u->isAlive() && u->getPos() == c) return false;
    }

    if (baseA && !baseA->isDestroyed() && baseA->getPos() == c)
        return false;
    if (baseB && !baseB->isDestroyed() && baseB->getPos() == c)
        return false;

    return true;
}

void GameWorld::startRenderThread() {
    if(renderRunning.load()) return;

    renderRunning.store(true);
    if(!renderSystem) renderSystem = std::make_unique<RenderSystem>();

    renderThread = std::thread([this]() {
        while(renderRunning.load()) {
            {
                std::shared_lock<std::shared_mutex> lock(worldMutex);
                renderSystem->render(*this);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(80));
        }
    });
}

void GameWorld::stopRenderThread() {
    if(!renderRunning.load()) return;

    renderRunning.store(false);
    if(renderThread.joinable()) {
        renderThread.join();
    }
}

void GameWorld::rebuildEnemies() {
    enemiesA.clear();
    enemiesB.clear();

    if (baseB) {
        enemiesA.emplace_back(baseB);  
    }
    for (auto& u : unitsB) {
        enemiesA.emplace_back(u);      
    }

    if (baseA) {
        enemiesB.emplace_back(baseA);
    }
    for (auto& u : unitsA) {
        enemiesB.emplace_back(u);
    }
}

