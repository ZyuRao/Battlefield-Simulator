#include "core/Iattackable.hpp"
#include "core/gameworld.hpp"
#include <chrono>
#include <algorithm>
#include <iostream>


namespace {
    constexpr int MAP_WIDTH = 60;
    constexpr int MAP_HEIGHT = 40;

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
    : map(MAP_HEIGHT, MAP_HEIGHT)
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
    baseA = std::make_unique<Base>(baseAPos, Faction::A);
    baseB = std::make_unique<Base>(baseBPos, Faction::B);

    enemiesA.clear();
    enemiesB.clear();

    taskPool.init(this);
    renderSystem = std::make_unique<RenderSystem>();
}


GameWorld::~GameWorld() {
    stopRenderThread();
}

void GameWorld::update() {
    std::unique_lock lock(worldMutex);

    timeManager.tick();
    float dt = timeManager.getDeltaTime();

    baseSystem.update(*this, dt);
    rebuildEnemies();
    visionSystem.update(*this);

    movementSystem.update(*this, dt);

    attackSystem.update(*this, dt);
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

    renderThread = std::thread([this]() {
        while(renderRunning.load()) {
            {
                std::shared_lock lock(worldMutex);
                renderSystem->render(*this);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
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
        enemiesA.push_back(baseB.get());  
    }
    for (auto& u : unitsB) {
        enemiesA.push_back(u.get());      
    }

    if (baseA) {
        enemiesB.push_back(baseA.get());
    }
    for (auto& u : unitsA) {
        enemiesB.push_back(u.get());
    }
}

