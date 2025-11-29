#include "core/gameworld.hpp"
#include <iostream>
#include <string>
#include <windows.h>
namespace {
    void clearScreen() {
        #ifdef _WIN32
            HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
            if (hOut == INVALID_HANDLE_VALUE) return;

            CONSOLE_SCREEN_BUFFER_INFO csbi;
            if (!GetConsoleScreenBufferInfo(hOut, &csbi)) return;

            DWORD cellCount = csbi.dwSize.X * csbi.dwSize.Y;
            DWORD count;
            COORD home = {0, 0};

            FillConsoleOutputCharacter(hOut, ' ', cellCount, home, &count);
            FillConsoleOutputAttribute(hOut, csbi.wAttributes, cellCount, home, &count);
            SetConsoleCursorPosition(hOut, home);
        #else
            std::cout << "\x1b[2J\x1b[H";
        #endif
    }
}

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
    auto u = std::make_unique<Unit>(t, spawnPos, base.getFaction());

    if(base.getFaction() == Faction::A) {
        world.unitsA.push_back(std::move(u));
    } else {
        world.unitsB.push_back(std::move(u));
    }
}

Coord BaseSystem::findSpawnPos(const Base& base, const GameWorld& world) const {
    Coord c = base.getPos();

    std::vector<Coord> nbrs;
    world.map.getNeighbors8(world.map, c, nbrs);
    for(const Coord& n : nbrs) {
        if(world.map.getTile(n).isPassable() && world.isTileFree(n)) {
            return n;
        }
    }

    if(world.map.getTile(c).isPassable() && world.isTileFree(c)) return c;
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

static void moveCursor(int row, int col) {
    std::cout << "\x1b[" << row << ";" << col << "H";
}

void RenderSystem::render(const GameWorld& world) {
    const int W = world.map.getWidth();
    const int H = world.map.getHeight();

    
    std::vector<std::string> buffer(H, std::string(W, ' '));

    // 1. 生成本帧 buffer
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            auto sym = world.map.getTile({x,y}).getSymbol();
            buffer[y][x] = sym.empty() ? ' ' : sym[0];
        }
    }

    if (world.baseA && !world.baseA->isDestroyed())
        buffer[world.baseA->getPos().y][world.baseA->getPos().x] = 'A';

    if (world.baseB && !world.baseB->isDestroyed())
        buffer[world.baseB->getPos().y][world.baseB->getPos().x] = 'B';

    for (auto& u : world.unitsA)
        if (u->isAlive()) {
            auto p = u->getPos();
            buffer[p.y][p.x] = u->getSymbol()[0];
        }

    for (auto& u : world.unitsB)
        if (u->isAlive()) {
            auto p = u->getPos();
            buffer[p.y][p.x] = u->getSymbol()[0];
        }

    // 2. 如果是第一次渲染/尺寸变化 → 简单粗暴清屏全画一次
    if (lastBuffer.size() != buffer.size()) {
        clearScreen();
        for (int y = 0; y < H; ++y) {
            std::cout << buffer[y] << "\n";
        }
        lastBuffer = buffer;
        std::cout.flush();
        return;
    }

    // 3. 之后只重画“变了的那几行”
    for (int y = 0; y < H; ++y) {
        if (buffer[y] != lastBuffer[y]) {
            moveCursor(y + 1, 1);      // 行号从 1 开始
            std::cout << buffer[y];
        }
    }

    lastBuffer = buffer;
    std::cout.flush();
}
