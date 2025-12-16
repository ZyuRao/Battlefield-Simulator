#include "core/Iattackable.hpp"
#include "core/gameworld.hpp"
#include <chrono>
#include <algorithm>
#include <iostream>
#include <SFML/Graphics.hpp>
#include <optional>
#include <unordered_set>


namespace {
    constexpr int MAP_WIDTH = 30;
    constexpr int MAP_HEIGHT = 30;

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
    : map(MAP_WIDTH, MAP_HEIGHT), baseSystem()
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
    registerBase(baseA);
    registerBase(baseB);

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

    processCommands();
    if (paused.load() || gameEnded.load()) {
        timeManager.reset();
        return;
    }

    decayForcedReveals(dt);
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

    // 清理选中列表中已不存在的单位
    std::unordered_set<int> alive;
    for (auto& u : unitsA) if (u && u->isAlive()) alive.insert(u->id);
    for (auto& u : unitsB) if (u && u->isAlive()) alive.insert(u->id);
    selectedUnitIds.erase(
        std::remove_if(selectedUnitIds.begin(), selectedUnitIds.end(),
            [&](int id){ return alive.find(id) == alive.end(); }),
        selectedUnitIds.end()
    );
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
    renderSystem->clock.restart();

    renderThread = std::thread([this]() {
        const unsigned W = static_cast<unsigned>(map.getWidth());
        const unsigned H = static_cast<unsigned>(map.getHeight());

        const unsigned tile = 26;     // 你想要的视觉密度
        const unsigned hud  = 260;
        const unsigned pad  = 40;

        sf::RenderWindow window(
            sf::VideoMode({hud + pad + tile * W, pad + tile * H}),
            "Battlefield Simulator",
            sf::Style::Titlebar | sf::Style::Close
        );
        window.setPosition(sf::Vector2i{60, 60});
        window.setFramerateLimit(60);
        auto shutdownStart = std::chrono::steady_clock::time_point{};
        bool shutdownArmed = false;
        while(renderRunning.load()) {
            if (!window.isOpen()) {
                renderRunning.store(false);
                requestQuit();
                break;
            }

            while(const std::optional event = window.pollEvent()) {
                if(event->is<sf::Event::Closed>()) {
                    window.close();
                    renderRunning.store(false);
                    requestQuit();
                }

                if (auto key = event->getIf<sf::Event::KeyPressed>()) {
                    using sf::Keyboard::Key;
                    if (key->code == Key::Enter) {
                        if (renderSystem->inputActive) {
                            if (!renderSystem->inputBuffer.empty()) {
                                enqueueCommand(renderSystem->inputBuffer);
                                lastCommandInput = renderSystem->inputBuffer;
                            }
                            renderSystem->inputBuffer.clear();
                            renderSystem->inputActive = false;
                        } else {
                            renderSystem->inputBuffer.clear();
                            renderSystem->inputActive = true;
                        }
                    } else if (key->code == Key::Backspace) {
                        if (renderSystem->inputActive && !renderSystem->inputBuffer.empty()) {
                            renderSystem->inputBuffer.pop_back();
                        }
                    } else if (key->code == Key::Escape) {
                        if (renderSystem->inputActive) {
                            renderSystem->inputBuffer.clear();
                            renderSystem->inputActive = false;
                        } else {
                            window.close();
                            renderRunning.store(false);
                            requestQuit();
                        }
                    } else if (key->code == Key::Q) {
                        window.close();
                        renderRunning.store(false);
                        requestQuit();
                    } else if (key->code == Key::P) {
                        togglePause();
                        lastCommandInput = "toggle pause";
                        lastCommandFeedback = paused.load() ? "Paused" : "Resumed";
                    }
                }

                if (renderSystem->inputActive) {
                    if (const auto text = event->getIf<sf::Event::TextEntered>()) {
                        char32_t uni = text->unicode;
                        if (uni >= 32 && uni < 127) {
                            renderSystem->inputBuffer.push_back(static_cast<char>(uni));
                        }
                    }
                }

                if (const auto mouse = event->getIf<sf::Event::MouseButtonPressed>()) {
                    // 生产选择优先处理
                    if (renderSystem->awaitingProductionChoice) {
                        std::unique_lock<std::shared_mutex> lock(worldMutex);
                        auto basePtr = renderSystem->productionChoiceBase.lock();
                        renderSystem->awaitingProductionChoice = false;
                        renderSystem->productionChoiceBase.reset();

                        std::optional<UnitType> chosen;
                        if (mouse->button == sf::Mouse::Button::Left) chosen = UnitType::Infantry;
                        else if (mouse->button == sf::Mouse::Button::Right) chosen = UnitType::Archer;
                        else if (mouse->button == sf::Mouse::Button::Middle) chosen = UnitType::Knight;

                        if (basePtr && chosen.has_value()) {
                            basePtr->issueProduce(*chosen);
                            pause();
                            lastCommandInput = "mouse production";
                            lastCommandFeedback = "生产 " +
                                std::string(*chosen == UnitType::Infantry ? "Infantry" :
                                            *chosen == UnitType::Archer   ? "Archer"   : "Knight");
                        } else {
                            lastCommandInput = "mouse production";
                            lastCommandFeedback = "生产取消";
                        }
                        continue;
                    }

                    // 双击检测
                    bool isDouble = false;
                    {
                        float since = renderSystem->clickClock.getElapsedTime().asSeconds();
                        sf::Vector2i pos = mouse->position;
                        int dx = pos.x - renderSystem->lastClickPos.x;
                        int dy = pos.y - renderSystem->lastClickPos.y;
                        float dist2 = static_cast<float>(dx * dx + dy * dy);
                        if (renderSystem->hasLastClick &&
                            mouse->button == renderSystem->lastClickButton &&
                            since <= renderSystem->doubleClickThreshold &&
                            dist2 <= renderSystem->doubleClickDistance * renderSystem->doubleClickDistance) {
                            isDouble = true;
                        }
                        renderSystem->lastClickPos = pos;
                        renderSystem->lastClickButton = mouse->button;
                        renderSystem->hasLastClick = true;
                        renderSystem->clickClock.restart();
                    }

                    auto coordOpt = renderSystem->pixelToTile(*this, window, mouse->position);

                    if (mouse->button == sf::Mouse::Button::Middle && !isDouble) {
                        togglePause();
                        lastCommandInput = "mouse pause";
                        lastCommandFeedback = paused.load() ? "Paused" : "Resumed";
                        continue;
                    }

                    if (!coordOpt) continue;
                    Coord clicked = *coordOpt;

                    if (isDouble) {
                        std::shared_ptr<Base> baseTarget;
                        {
                            std::shared_lock<std::shared_mutex> lock(worldMutex);
                            if (baseA && !baseA->isDestroyed() && baseA->getPos() == clicked) {
                                baseTarget = baseA;
                            } else if (baseB && !baseB->isDestroyed() && baseB->getPos() == clicked) {
                                baseTarget = baseB;
                            }
                        }
                        if (baseTarget) {
                            renderSystem->awaitingProductionChoice = true;
                            renderSystem->productionChoiceBase = baseTarget;
                            pause();
                            lastCommandInput = "double click base";
                            lastCommandFeedback = "选择生产：左-Infantry 右-Archer 中键-Knight";
                            continue;
                        }
                    }

                    if (mouse->button == sf::Mouse::Button::Left) {
                        std::unique_lock<std::shared_mutex> lock(worldMutex);
                        std::shared_ptr<Unit> pick;
                        for (auto& u : unitsA) {
                            if (u && u->isAlive() && u->getPos() == clicked) {
                                pick = u;
                                break;
                            }
                        }
                        if (pick) {
                            selectedUnitIds = {pick->id};
                            pause();
                            lastCommandInput = "click select";
                            lastCommandFeedback = "Selected #" + std::to_string(pick->id);
                        }
                    } else if (mouse->button == sf::Mouse::Button::Right) {
                        std::unique_lock<std::shared_mutex> lock(worldMutex);
                        if (selectedUnitIds.empty()) continue;

                        std::shared_ptr<IAttackable> target;
                        if (baseB && !baseB->isDestroyed() && baseB->getPos() == clicked) {
                            target = baseB;
                        }
                        if (!target) {
                            for (auto& u : unitsB) {
                                if (u && u->isAlive() && u->getPos() == clicked) {
                                    target = u;
                                    break;
                                }
                            }
                        }

                        if (target) {
                            for (int id : selectedUnitIds) {
                                auto u = findUnit(id);
                                if (u) u->issueAttackTarget(target);
                            }
                            pause();
                            lastCommandInput = "click attack";
                            lastCommandFeedback = "Attack target set";
                        } else {
                            for (int id : selectedUnitIds) {
                                auto u = findUnit(id);
                                if (u) u->issueMove(clicked);
                            }
                            pause();
                            lastCommandInput = "click move";
                            lastCommandFeedback = "Move to " + std::to_string(clicked.x) + "," + std::to_string(clicked.y);
                        }
                    }
                }
            }

            if (!shutdownArmed && gameEnded.load()) {
                shutdownArmed = true;
                auto ms = gameEndTimestampMs.load();
                if (ms > 0) {
                    shutdownStart = std::chrono::steady_clock::time_point(std::chrono::milliseconds(ms));
                } else {
                    shutdownStart = std::chrono::steady_clock::now();
                }
            }

            if (shutdownArmed) {
                auto elapsed = std::chrono::steady_clock::now() - shutdownStart;
                if (elapsed >= std::chrono::seconds(5)) {
                    window.close();
                    renderRunning.store(false);
                    requestQuit();
                }
            }

            if (!renderRunning.load() || !window.isOpen()) {
                continue;
            }

            window.clear(sf::Color(18, 24, 32));
            {
                std::shared_lock<std::shared_mutex> lock(worldMutex);
                renderSystem->renderSfml(*this, window);
            }
            window.display();
        }
    });
}

void GameWorld::stopRenderThread() {
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

void GameWorld::enqueueCommand(const std::string& line) {
    commandQueue.push(line);
}

void GameWorld::processCommands() {
    std::string line;
    while (commandQueue.tryPop(line)) {
        lastCommandInput = line;
        Command parsed;
        std::string err;
        if (!parseCommand(line, parsed, err)) {
            lastCommandFeedback = "ERR: " + err;
            continue;
        }
        CommandResult r = executeCommand(parsed, *this);
        if (r.ok) {
            lastCommandFeedback = r.normalized + " -> " + r.message;
        } else {
            lastCommandFeedback = "ERR: " + r.message;
        }
    }
}

std::shared_ptr<Unit> GameWorld::findUnit(int id) const {
    if (id < 0) return nullptr;
    for (auto& u : unitsA) {
        if (u && u->id == id && u->isAlive()) return u;
    }
    for (auto& u : unitsB) {
        if (u && u->id == id && u->isAlive()) return u;
    }
    return nullptr;
}

std::shared_ptr<Base> GameWorld::findBase(int id, Faction fac) const {
    if (baseA && !baseA->isDestroyed() &&
        ((id >= 0 && baseA->getId() == id) || (id < 0 && fac == Faction::A))) {
        return baseA;
    }
    if (baseB && !baseB->isDestroyed() &&
        ((id >= 0 && baseB->getId() == id) || (id < 0 && fac == Faction::B))) {
        return baseB;
    }
    return nullptr;
}

std::shared_ptr<IAttackable> GameWorld::findAttackable(int id) const {
    if (id < 0) return nullptr;
    auto u = findUnit(id);
    if (u) return u;

    if (baseA && baseA->getId() == id && !baseA->isDestroyed()) return baseA;
    if (baseB && baseB->getId() == id && !baseB->isDestroyed()) return baseB;
    return nullptr;
}

void GameWorld::setSelection(const std::vector<int>& ids) {
    selectedUnitIds = ids;
}

void GameWorld::clearSelection() {
    selectedUnitIds.clear();
}

int GameWorld::registerUnit(const std::shared_ptr<Unit>& u) {
    if (!u) return -1;
    u->id = nextUnitId++;
    return u->id;
}

int GameWorld::registerBase(const std::shared_ptr<Base>& b) {
    if (!b) return -1;
    b->setId(nextBaseId++);
    return b->getId();
}

void GameWorld::pause() {
    paused.store(true);
}

void GameWorld::resume() {
    paused.store(false);
}

void GameWorld::togglePause() {
    if (paused.load()) {
        resume();
    } else {
        pause();
    }
}

void GameWorld::markGameOver() {
    gameEnded.store(true);
    const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    gameEndTimestampMs.store(nowMs);
    pause();
}

void GameWorld::addForcedReveal(Faction viewer, const std::shared_ptr<IAttackable>& target, float durationSeconds) {
    if (!target) return;
    auto& bucket = (viewer == Faction::A) ? forcedVisibleForA : forcedVisibleForB;
    bucket.push_back({target, durationSeconds});
}

void GameWorld::decayForcedReveals(float dt) {
    auto decay = [&](std::vector<ForcedReveal>& arr) {
        for (auto& r : arr) r.timeLeft -= dt;
        arr.erase(std::remove_if(arr.begin(), arr.end(),
                                 [](const ForcedReveal& r) {
                                     return r.timeLeft <= 0.f || r.target.expired();
                                 }),
                  arr.end());
    };
    decay(forcedVisibleForA);
    decay(forcedVisibleForB);
}

void GameWorld::appendForcedReveals(Faction viewer, std::vector<std::weak_ptr<IAttackable>>& out) const {
    const auto& bucket = (viewer == Faction::A) ? forcedVisibleForA : forcedVisibleForB;
    for (const auto& r : bucket) {
        if (!r.target.expired()) {
            out.push_back(r.target);
        }
    }
}

void GameWorld::revealAttacker(const Unit& u) {
    auto attacker = findUnit(u.id);
    if (!attacker) return;
    Faction viewer = (u.owner == Faction::A) ? Faction::B : Faction::A;
    addForcedReveal(viewer, attacker, 2.0f);
}

void GameWorld::waitRenderThread() {
    if (renderThread.joinable()) {
        renderThread.join();
    }
}
