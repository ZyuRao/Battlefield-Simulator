#include "core/Iattackable.hpp"
#include "core/gameworld.hpp"
#include "core/render_config.hpp"
#include <chrono>
#include <algorithm>
#include <SFML/Graphics.hpp>
#include <optional>
#include <iterator>
#include <cstdint>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <cmath>
#include <cctype>
#include <limits>


constexpr int MAP_WIDTH = 30;
constexpr int MAP_HEIGHT = 30;

using UnitSnapshot = AiScoring::UnitSnapshot;
using AttackableSnapshot = AiScoring::AttackableSnapshot;
using TargetChoice = AiScoring::TargetChoice;
using ReachableGrid = MapQuery::ReachableGrid;

struct MapPlacement {
    static bool pickBasePositions(const Map& map, Coord& baseA, Coord& baseB);

private:
    static bool findPassableInRegion(const Map& map,
                                     int x0, int y0, int x1, int y1,
                                     Coord& out);
};

bool MapPlacement::findPassableInRegion(const Map& map,
                                        int x0, int y0, int x1, int y1,
                                        Coord& out) {
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

bool MapPlacement::pickBasePositions(const Map& map, Coord& baseA, Coord& baseB) {
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

WorldState::WorldState(int width, int height)
    : map(width, height) {}

void WorldEventBus::subscribe(Handler handler) {
    handlers.push_back(std::move(handler));
}

void WorldEventBus::publish(const WorldEvent& event) {
    pending.push_back(event);
}

void WorldEventBus::drain(WorldControlContext& control, WorldRuntimeContext& runtime) {
    if (pending.empty()) return;
    std::vector<WorldEvent> events;
    events.swap(pending);
    if (handlers.empty()) return;
    for (const auto& evt : events) {
        for (auto& handler : handlers) {
            handler(evt, control, runtime);
        }
    }
}

WorldRuntime::WorldRuntime()
    : taskPool(4) {}

void WorldRuntime::enqueueUiEvent(std::optional<std::string> input,
                                  std::optional<std::string> feedback) {
    UiEvent evt;
    evt.input = std::move(input);
    evt.feedback = std::move(feedback);
    {
        std::lock_guard<std::mutex> lock(uiEventMutex);
        uiEvents.push(std::move(evt));
    }
}

WorldDataContext::WorldDataContext(WorldState& state)
    : map(state.map),
      baseA(state.baseA),
      baseB(state.baseB),
      unitsA(state.unitsA),
      unitsB(state.unitsB),
      enemiesA(state.enemiesA),
      enemiesB(state.enemiesB),
      forcedVisibleForA(state.forcedVisibleForA),
      forcedVisibleForB(state.forcedVisibleForB),
      lastVisionIntents(state.lastVisionIntents),
      lastTargetHints(state.lastTargetHints),
      lastIncomingDamageA(state.lastIncomingDamageA),
      lastIncomingDamageB(state.lastIncomingDamageB),
      lastLockedTargetsA(state.lastLockedTargetsA),
      lastLockedTargetsB(state.lastLockedTargetsB),
      nextUnitId(state.nextUnitId),
      nextBaseId(state.nextBaseId) {}

WorldRuntimeContext::WorldRuntimeContext(WorldRuntime& runtime)
    : runtime(runtime),
      taskPool(runtime.taskPool),
      renderThread(runtime.renderThread),
      renderRunning(runtime.renderRunning),
      paused(runtime.paused),
      gameEnded(runtime.gameEnded),
      gameEndTimestampMs(runtime.gameEndTimestampMs),
      quitRequested(runtime.quitRequested),
      worldMutex(runtime.worldMutex),
      uiEventMutex(runtime.uiEventMutex),
      uiEvents(runtime.uiEvents),
      eventBus(runtime.eventBus) {}

WorldControlContext::WorldControlContext(ControlState& control)
    : commandQueue(control.commandQueue),
      lastCommandInput(control.lastCommandInput),
      lastCommandFeedback(control.lastCommandFeedback),
      selectedUnitIds(control.selectedUnitIds),
      controlMode(control.controlMode),
      pendingTarget(control.pendingTarget),
      awaitingProductionChoice(control.awaitingProductionChoice),
      productionChoiceBase(control.productionChoiceBase),
      productionInputBuffer(control.productionInputBuffer) {}

// State + Command anchor: UI state controls input routing; commands execute on commit.
struct InputStateMachine {
    enum class UiState { Idle, Targeting, Production };

    WorldDataContext& data;
    WorldControlContext& control;
    WorldRuntimeContext& runtime;
    RenderSystem& render;
    sf::RenderWindow& window;

    InputStateMachine(WorldDataContext& d,
                      WorldControlContext& c,
                      WorldRuntimeContext& r,
                      RenderSystem& rs,
                      sf::RenderWindow& w)
        : data(d), control(c), runtime(r), render(rs), window(w) {}

    void handleEvent(const sf::Event& event) {
        if (event.is<sf::Event::Closed>()) {
            window.close();
            runtime.renderRunning.store(false);
            runtime.requestQuit();
            return;
        }
        if (auto key = event.getIf<sf::Event::KeyPressed>()) {
            handleKeyPressed(*key);
        }
        if (auto text = event.getIf<sf::Event::TextEntered>()) {
            handleTextEntered(*text);
        }
        if (auto mouse = event.getIf<sf::Event::MouseButtonPressed>()) {
            handleMousePressed(*mouse);
        }
    }

private:
    struct InputCommand {
        virtual ~InputCommand() = default;
        virtual void execute(WorldDataContext& data,
                             WorldControlContext& control,
                             WorldRuntimeContext& runtime) = 0;
    };

    struct MoveCommand final : InputCommand {
        Coord dest{};
        explicit MoveCommand(const Coord& d) : dest(d) {}

        void execute(WorldDataContext& data,
                     WorldControlContext& control,
                     WorldRuntimeContext& runtime) override {
            for (int id : control.selectedUnitIds) {
                auto u = data.findUnit(id);
                if (u) u->issueMove(dest);
            }
            postUi(runtime, "click move",
                   "Move to " + std::to_string(dest.x) + "," + std::to_string(dest.y));
        }
    };

    struct AttackCommand final : InputCommand {
        std::shared_ptr<IAttackable> target;
        explicit AttackCommand(std::shared_ptr<IAttackable> t) : target(std::move(t)) {}

        void execute(WorldDataContext& data,
                     WorldControlContext& control,
                     WorldRuntimeContext& runtime) override {
            if (!target) return;
            for (int id : control.selectedUnitIds) {
                auto u = data.findUnit(id);
                if (u) u->issueAttackTarget(target);
            }
            postUi(runtime, "click attack", "Attack target set");
        }
    };

    struct CancelCommand final : InputCommand {
        void execute(WorldDataContext&,
                     WorldControlContext& control,
                     WorldRuntimeContext& runtime) override {
            control.cancelTargeting();
            runtime.resume();
        }
    };

    struct SelectCommand final : InputCommand {
        int unitId = -1;
        explicit SelectCommand(int id) : unitId(id) {}

        void execute(WorldDataContext&,
                     WorldControlContext& control,
                     WorldRuntimeContext& runtime) override {
            if (unitId < 0) return;
            control.setSelection({unitId});
            postUi(runtime, "click select",
                   "Selected #" + std::to_string(unitId));
        }
    };

    UiState state() const {
        if (control.awaitingProductionChoice) return UiState::Production;
        if (control.controlMode == ControlState::ControlMode::Targeting) {
            return UiState::Targeting;
        }
        return UiState::Idle;
    }

    void handleKeyPressed(const sf::Event::KeyPressed& key) {
        using sf::Keyboard::Key;
        if (state() == UiState::Production) {
            if (key.code == Key::Backspace) {
                control.handleProductionBackspace(runtime);
                return;
            }
            if (key.code == Key::P) {
                control.cancelProductionChoice(runtime);
                postUiInput(runtime, "production cancel (P)");
                return;
            }
            if (key.code == Key::Enter) {
                control.commitProductionChoice(runtime);
                return;
            }
            if (key.code == Key::Escape) {
                control.cancelProductionChoice(runtime);
                return;
            }
        }

        if (state() == UiState::Targeting) {
            if (key.code == Key::Enter) {
                commitTargeting();
                return;
            }
            if (key.code == Key::Escape) {
                std::unique_ptr<InputCommand> cmd = std::make_unique<CancelCommand>();
                std::unique_lock<std::shared_mutex> lock(runtime.worldMutex);
                cmd->execute(data, control, runtime);
                return;
            }
        }

        if (key.code == Key::Enter) {
            if (render.inputActive) {
                if (!render.inputBuffer.empty()) {
                    control.enqueueCommand(render.inputBuffer);
                    postUiInput(runtime, render.inputBuffer);
                }
                render.inputBuffer.clear();
                render.inputActive = false;
            } else {
                render.inputBuffer.clear();
                render.inputActive = true;
            }
        } else if (key.code == Key::Backspace) {
            if (render.inputActive && !render.inputBuffer.empty()) {
                render.inputBuffer.pop_back();
            }
        } else if (key.code == Key::Escape) {
            if (render.inputActive) {
                render.inputBuffer.clear();
                render.inputActive = false;
            } else {
                window.close();
                runtime.renderRunning.store(false);
                runtime.requestQuit();
            }
        } else if (key.code == Key::Q) {
            window.close();
            runtime.renderRunning.store(false);
            runtime.requestQuit();
        } else if (key.code == Key::P) {
            runtime.togglePause();
            postUi(runtime, "toggle pause",
                   runtime.paused.load() ? "Paused" : "Resumed");
        }
    }

    void handleTextEntered(const sf::Event::TextEntered& text) {
        if (state() == UiState::Production) {
            char32_t uni = text.unicode;
            if (uni >= U'0' && uni <= U'9') {
                control.handleProductionDigit(static_cast<char>(uni), runtime);
            }
            return;
        }

        if (render.inputActive) {
            char32_t uni = text.unicode;
            if (uni >= 32 && uni < 127) {
                render.inputBuffer.push_back(static_cast<char>(uni));
            }
        }
    }

    void handleMousePressed(const sf::Event::MouseButtonPressed& mouse) {
        if (state() == UiState::Production &&
            mouse.button != sf::Mouse::Button::Middle) {
            return;
        }

        if (mouse.button == sf::Mouse::Button::Middle) {
            runtime.togglePause();
            postUi(runtime, "mouse pause",
                   runtime.paused.load() ? "Paused" : "Resumed");
            return;
        }

        auto coordOpt = render.pixelToTile(data, window, mouse.position);
        if (!coordOpt) return;
        Coord clicked = *coordOpt;

        if (mouse.button == sf::Mouse::Button::Left) {
            handleLeftClick(clicked);
        } else if (mouse.button == sf::Mouse::Button::Right) {
            handleRightClick(clicked);
        }
    }

    void handleLeftClick(const Coord& clicked) {
        std::unique_lock<std::shared_mutex> lock(runtime.worldMutex);
        if (state() == UiState::Targeting) {
            handleTargetingClick(clicked);
            return;
        }

        std::shared_ptr<Base> baseTarget;
        if (data.baseA && !data.baseA->isDestroyed() && data.baseA->getPos() == clicked) {
            baseTarget = data.baseA;
        } else if (data.baseB && !data.baseB->isDestroyed() && data.baseB->getPos() == clicked) {
            baseTarget = data.baseB;
        }
        if (baseTarget) {
            control.beginProductionChoice(baseTarget, runtime);
            return;
        }

        std::shared_ptr<Unit> pick;
        for (auto& u : data.unitsA) {
            if (u && u->isAlive() && u->getPos() == clicked) {
                pick = u;
                break;
            }
        }
        if (pick) {
            std::unique_ptr<InputCommand> cmd = std::make_unique<SelectCommand>(pick->id);
            cmd->execute(data, control, runtime);
            enterTargeting();
        }
    }

    void handleRightClick(const Coord& clicked) {
        if (state() == UiState::Targeting) return;

        std::unique_lock<std::shared_mutex> lock(runtime.worldMutex);
        if (control.selectedUnitIds.empty()) return;

        std::shared_ptr<IAttackable> target;
        if (data.baseB && !data.baseB->isDestroyed() && data.baseB->getPos() == clicked) {
            target = data.baseB;
        }
        if (!target) {
            for (auto& u : data.unitsB) {
                if (u && u->isAlive() && u->getPos() == clicked) {
                    target = u;
                    break;
                }
            }
        }

        std::unique_ptr<InputCommand> cmd;
        if (target) {
            cmd = std::make_unique<AttackCommand>(target);
        } else {
            cmd = std::make_unique<MoveCommand>(clicked);
        }
        if (cmd) {
            cmd->execute(data, control, runtime);
            runtime.pause();
        }
    }

    void handleTargetingClick(const Coord& clicked) {
        auto hitUnit = findUnitAt(clicked);
        auto selFactionOpt = selectedFaction();
        if (hitUnit && selFactionOpt.has_value() &&
            hitUnit->getFaction() == *selFactionOpt) {
            std::unique_ptr<InputCommand> cmd = std::make_unique<SelectCommand>(hitUnit->id);
            cmd->execute(data, control, runtime);
            control.pendingTarget.reset();
        } else if (hitUnit) {
            control.pendingTarget = ControlState::PendingTarget{
                ControlState::PendingTarget::Kind::Unit,
                hitUnit->getPos(),
                hitUnit->id
            };
        } else {
            control.pendingTarget = ControlState::PendingTarget{
                ControlState::PendingTarget::Kind::Tile,
                clicked,
                -1
            };
        }
    }

    void commitTargeting() {
        std::unique_lock<std::shared_mutex> lock(runtime.worldMutex);
        if (!control.pendingTarget || control.selectedUnitIds.empty()) {
            return;
        }
        auto selFactionOpt = selectedFaction();
        if (!selFactionOpt.has_value()) {
            std::unique_ptr<InputCommand> cmd = std::make_unique<CancelCommand>();
            cmd->execute(data, control, runtime);
            return;
        }
        Faction selFaction = *selFactionOpt;
        Faction enemyFaction =
            (selFaction == Faction::A) ? Faction::B : Faction::A;

        std::unique_ptr<InputCommand> cmd;
        if (control.pendingTarget->kind == ControlState::PendingTarget::Kind::Unit) {
            auto targetUnit = data.findUnit(control.pendingTarget->unitId);
            if (targetUnit && targetUnit->isAlive()) {
                if (targetUnit->getFaction() == enemyFaction) {
                    cmd = std::make_unique<AttackCommand>(targetUnit);
                } else {
                    cmd = std::make_unique<MoveCommand>(targetUnit->getPos());
                }
            } else {
                cmd = std::make_unique<MoveCommand>(control.pendingTarget->tile);
            }
        } else {
            auto target = resolveEnemyAt(enemyFaction, control.pendingTarget->tile);
            if (target) {
                cmd = std::make_unique<AttackCommand>(target);
            } else {
                cmd = std::make_unique<MoveCommand>(control.pendingTarget->tile);
            }
        }

        if (cmd) {
            cmd->execute(data, control, runtime);
        }
        control.commitTargeting();
        runtime.resume();
    }

    void enterTargeting() {
        control.enterTargeting();
        runtime.pause();
    }

    static void postUi(WorldRuntimeContext& runtime,
                       const std::string& input,
                       const std::string& feedback) {
        runtime.enqueueUiEvent(std::optional<std::string>(input),
                               std::optional<std::string>(feedback));
    }

    static void postUiInput(WorldRuntimeContext& runtime,
                            const std::string& input) {
        runtime.enqueueUiEvent(std::optional<std::string>(input), std::nullopt);
    }

    std::optional<Faction> selectedFaction() const {
        if (control.selectedUnitIds.empty()) return std::nullopt;
        auto u = data.findUnit(control.selectedUnitIds.front());
        if (!u) return std::nullopt;
        return u->getFaction();
    }

    std::shared_ptr<Unit> findUnitAt(const Coord& coord) const {
        for (auto& u : data.unitsA) {
            if (u && u->isAlive() && u->getPos() == coord) return u;
        }
        for (auto& u : data.unitsB) {
            if (u && u->isAlive() && u->getPos() == coord) return u;
        }
        return nullptr;
    }

    std::shared_ptr<IAttackable> resolveEnemyAt(Faction enemyFaction,
                                                const Coord& coord) const {
        if (enemyFaction == Faction::A) {
            if (data.baseA && !data.baseA->isDestroyed() && data.baseA->getPos() == coord) {
                return data.baseA;
            }
            for (auto& u : data.unitsA) {
                if (u && u->isAlive() && u->getPos() == coord) return u;
            }
            return nullptr;
        }
        if (data.baseB && !data.baseB->isDestroyed() && data.baseB->getPos() == coord) {
            return data.baseB;
        }
        for (auto& u : data.unitsB) {
            if (u && u->isAlive() && u->getPos() == coord) return u;
        }
        return nullptr;
    }
};

void WorldRuntimeContext::start(WorldDataContext& data,
                                WorldControlContext& control,
                                SystemsBundle& systems) {
    if (renderRunning.load()) return;

    renderRunning.store(true);
    if (!systems.render) systems.render = std::make_unique<RenderSystem>();
    systems.render->clock.restart();

    renderThread = std::thread([this, &data, &control, &systems]() {
        RenderSystem& renderSystem = *systems.render;

        const unsigned W = static_cast<unsigned>(data.map.getWidth());
        const unsigned H = static_cast<unsigned>(data.map.getHeight());

        const float hud  = RenderConfig::HUD_WIDTH;
        const float pad  = 40.f;

        const sf::VideoMode desktop = sf::VideoMode::getDesktopMode();
        const float maxW = std::floor(static_cast<float>(desktop.size.x) * 0.9f);
        const float maxH = std::floor(static_cast<float>(desktop.size.y) * 0.9f);

        const float mapMaxW = std::max(0.f, maxW - (hud + pad));
        const float mapMaxH = std::max(0.f, maxH - pad);

        int tileSize = RenderConfig::TILE_SIZE;
        if (static_cast<float>(tileSize) * W > mapMaxW ||
            static_cast<float>(tileSize) * H > mapMaxH) {
            float fit = std::floor(std::min(mapMaxW / static_cast<float>(W),
                                            mapMaxH / static_cast<float>(H)));
            int tileFit = std::max(static_cast<int>(fit), 24);
            if (tileFit >= RenderConfig::NATIVE_TILE_SIZE) tileSize = RenderConfig::NATIVE_TILE_SIZE;
            else if (tileFit >= 48) tileSize = 48;
            else if (tileFit >= 32) tileSize = 32;
            else tileSize = tileFit;
        }
        RenderConfig::TILE_SIZE = tileSize;

        const float mapPixelW = static_cast<float>(tileSize) * W;
        const float mapPixelH = static_cast<float>(tileSize) * H;
        const unsigned winW = static_cast<unsigned>(
            std::floor(std::min(mapPixelW + hud + pad, maxW)));
        const unsigned winH = static_cast<unsigned>(
            std::floor(std::min(mapPixelH + pad, maxH)));

        sf::RenderWindow window(
            sf::VideoMode({winW, winH}),
            "Battlefield Simulator",
            sf::Style::Titlebar | sf::Style::Close
        );
        window.setPosition(sf::Vector2i{60, 60});
        window.setFramerateLimit(60);

        InputStateMachine input(data, control, *this, renderSystem, window);

        auto shutdownStart = std::chrono::steady_clock::time_point{};
        bool shutdownArmed = false;
        while (renderRunning.load()) {
            if (!window.isOpen()) {
                renderRunning.store(false);
                requestQuit();
                break;
            }

            while (const std::optional event = window.pollEvent()) {
                input.handleEvent(*event);
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
                renderSystem.renderSfml(data, control, *this, window);
            }
            window.display();
        }
    });
}

void WorldRuntimeContext::stop() {
    renderRunning.store(false);
    if (renderThread.joinable()) {
        renderThread.join();
    }
}

void WorldRuntimeContext::join() {
    if (renderThread.joinable()) {
        renderThread.join();
    }
}

GameWorld::GameWorld()
    : state(MAP_WIDTH, MAP_HEIGHT)
{
    WorldDataContext data(state);
    MapGenerator gen(MAP_WIDTH, MAP_HEIGHT);

    bool ok = false;
    Coord baseAPos;
    Coord baseBPos;

    while (!ok) {
        Map candidate = gen.generate();

        if (MapPlacement::pickBasePositions(candidate, baseAPos, baseBPos)) {
            state.map = std::move(candidate);
            ok = true;
        }
    }
    state.baseA = std::make_shared<Base>(baseAPos, Faction::A);
    state.baseB = std::make_shared<Base>(baseBPos, Faction::B);
    data.registerBase(state.baseA);
    data.registerBase(state.baseB);

    state.enemiesA.clear();
    state.enemiesB.clear();

    
    runtime.taskPool.init();
    systems.render = std::make_unique<RenderSystem>();
    auto* runtimePtr = &runtime;
    runtime.eventBus.subscribe([runtimePtr](const WorldEvent& evt,
                                            WorldControlContext&,
                                            WorldRuntimeContext&) {
        if (!runtimePtr) return;
        if (evt.type == WorldEventType::BaseDestroyed) {
            const char* baseLabel = (evt.faction == Faction::A) ? "A" : "B";
            runtimePtr->enqueueUiEvent(std::nullopt,
                                       std::string("Base ") + baseLabel + " destroyed");
        } else if (evt.type == WorldEventType::GameEnded) {
            const char* winner = (evt.faction == Faction::A) ? "A" : "B";
            runtimePtr->enqueueUiEvent(std::nullopt,
                                       std::string("Winner: ") + winner);
        }
    });
}


GameWorld::~GameWorld() {
    WorldRuntimeContext runtimeCtx(runtime);
    runtimeCtx.stop();
}

void GameWorld::update(float dt) {
    WorldDataContext data(state);
    WorldRuntimeContext runtimeCtx(runtime);
    WorldControlContext controlCtx(control);

    runtimeCtx.drainUiEvents(controlCtx);
    runtimeCtx.eventBus.drain(controlCtx, runtimeCtx);
    {
        std::unique_lock<std::shared_mutex> lock(runtimeCtx.worldMutex);
        controlCtx.processCommands(data, runtimeCtx);
        if (runtimeCtx.paused.load() || runtimeCtx.gameEnded.load()) {
            return;
        }

        data.decayForcedReveals(dt);
        systems.base.update(data, dt);
        data.rebuildEnemies();

        std::vector<std::shared_ptr<Unit>> allUnits;
        allUnits.reserve(state.unitsA.size() + state.unitsB.size());
        for (auto& u : state.unitsA) allUnits.push_back(u);
        for (auto& u : state.unitsB) allUnits.push_back(u);
        std::sort(allUnits.begin(), allUnits.end(),
                  [](const std::shared_ptr<Unit>& a, const std::shared_ptr<Unit>& b) {
                      return a->id < b->id;
                  });
        for (auto& u : allUnits) {
            if (u && u->behavior) {
                u->behavior->applyPendingCommand(*u, data.map);
            }
        }
    }

    const std::size_t localCount = std::max<std::size_t>(1, runtimeCtx.taskPool.workerCount());

    // Snapshot stage: read-only data for planning.
    std::unordered_map<AttackableKey, float, AttackableKeyHash> incomingDamageA;
    std::unordered_map<AttackableKey, float, AttackableKeyHash> incomingDamageB;
    std::unordered_map<AttackableKey, int, AttackableKeyHash> lockedTargetsA;
    std::unordered_map<AttackableKey, int, AttackableKeyHash> lockedTargetsB;
    Coord baseAPos{};
    Coord baseBPos{};
    bool baseAAlive = false;
    bool baseBAlive = false;

    std::vector<UnitSnapshot> unitsASnap;
    std::vector<UnitSnapshot> unitsBSnap;
    std::vector<AttackableSnapshot> enemiesASnap;
    std::vector<AttackableSnapshot> enemiesBSnap;
    std::unordered_map<AttackableKey, std::size_t, AttackableKeyHash> enemyIndexA;
    std::unordered_map<AttackableKey, std::size_t, AttackableKeyHash> enemyIndexB;
    std::vector<AttackableKey> forcedAKeys;
    std::vector<AttackableKey> forcedBKeys;
    std::unordered_set<std::uint64_t> occSnap;
    {
        std::shared_lock<std::shared_mutex> lock(runtimeCtx.worldMutex);
        incomingDamageA = state.lastIncomingDamageA;
        incomingDamageB = state.lastIncomingDamageB;
        lockedTargetsA = state.lastLockedTargetsA;
        lockedTargetsB = state.lastLockedTargetsB;

        baseAAlive = state.baseA && !state.baseA->isDestroyed();
        baseBAlive = state.baseB && !state.baseB->isDestroyed();
        if (baseAAlive) baseAPos = state.baseA->pos;
        if (baseBAlive) baseBPos = state.baseB->pos;

        occSnap.reserve(state.unitsA.size() + state.unitsB.size() + 2);
        buildUnitSnapshots(state.unitsA, unitsASnap, &occSnap);
        buildUnitSnapshots(state.unitsB, unitsBSnap, &occSnap);
        if (baseAAlive) occSnap.insert(Coord::packCoord(baseAPos));
        if (baseBAlive) occSnap.insert(Coord::packCoord(baseBPos));

        buildEnemySnapshots(state.unitsB, state.baseB, enemiesASnap);
        buildEnemySnapshots(state.unitsA, state.baseA, enemiesBSnap);
        buildForcedKeys(state.forcedVisibleForA, forcedAKeys);
        buildForcedKeys(state.forcedVisibleForB, forcedBKeys);
    }

    auto byUnitId = [](const UnitSnapshot& a, const UnitSnapshot& b) {
        return a.id < b.id;
    };
    std::sort(unitsASnap.begin(), unitsASnap.end(), byUnitId);
    std::sort(unitsBSnap.begin(), unitsBSnap.end(), byUnitId);

    auto byAttackableKey = [](const AttackableSnapshot& a, const AttackableSnapshot& b) {
        if (a.key.type != b.key.type) return a.key.type < b.key.type;
        return a.key.id < b.key.id;
    };
    std::sort(enemiesASnap.begin(), enemiesASnap.end(), byAttackableKey);
    std::sort(enemiesBSnap.begin(), enemiesBSnap.end(), byAttackableKey);
    rebuildEnemyIndex(enemiesASnap, enemyIndexA);
    rebuildEnemyIndex(enemiesBSnap, enemyIndexB);

    auto keyLess = [](const AttackableKey& a, const AttackableKey& b) {
        if (a.type != b.type) return a.type < b.type;
        return a.id < b.id;
    };
    std::sort(forcedAKeys.begin(), forcedAKeys.end(), keyLess);
    forcedAKeys.erase(std::unique(forcedAKeys.begin(), forcedAKeys.end()), forcedAKeys.end());
    std::sort(forcedBKeys.begin(), forcedBKeys.end(), keyLess);
    forcedBKeys.erase(std::unique(forcedBKeys.begin(), forcedBKeys.end()), forcedBKeys.end());

    // Plan stage (Vision): tasks read snapshots only; no world writes here.
    auto visionGroup = std::make_shared<TaskGroup>();
    std::vector<IntentBuffer> visionLocals(localCount);

    auto scheduleVision = [&](const UnitSnapshot& unit,
                              const std::vector<AttackableSnapshot>& enemies,
                              const std::unordered_map<AttackableKey, std::size_t, AttackableKeyHash>& enemyIndex,
                              const std::vector<AttackableKey>& forcedKeys,
                              const std::vector<UnitSnapshot>& allies,
                              const std::unordered_map<AttackableKey, float, AttackableKeyHash>& incomingDamage,
                              const std::unordered_map<AttackableKey, int, AttackableKeyHash>& lockedCounts,
                              const std::unordered_set<std::uint64_t>& occ) {
        if (unit.hp <= 0.f) return;
        runtimeCtx.taskPool.submit([&, unit]() {
            std::vector<const AttackableSnapshot*> visible =
                AiTargetScoring::collectVisibleEnemies(unit, state.map, enemies, enemyIndex, forcedKeys);
            ReachableGrid reachable = MapQuery::buildReachableGrid(state.map, unit.pos);

            VisionIntent visionIntent;
            visionIntent.unitId = unit.id;
            visionIntent.visibleEnemyIds.reserve(visible.size());
            for (const auto* e : visible) {
                if (e && e->key.id >= 0) visionIntent.visibleEnemyIds.push_back(e->key.id);
            }

            TargetHint hint;
            hint.unitId = unit.id;
            TargetChoice choice = AiTargetScoring::chooseTarget(unit, visible, allies,
                                               incomingDamage, lockedCounts,
                                               AiScoring::kTargetParams, state.map, reachable,
                                               occ);
            hint.targetId = choice.target ? choice.target->key.id : -1;

            std::size_t idx = TaskPool::workerIndex();
            if (idx == TaskPool::kInvalidWorkerIndex || idx >= visionLocals.size()) {
                idx = 0;
            }
            IntentBuffer& buffer = visionLocals[idx];
            buffer.visionIntents.push_back(std::move(visionIntent));
            buffer.targetHints.push_back(hint);
        }, visionGroup);
    };

    for (const auto& u : unitsASnap) {
        scheduleVision(u, enemiesASnap, enemyIndexA, forcedAKeys, unitsASnap,
                       incomingDamageA, lockedTargetsA, occSnap);
    }
    for (const auto& u : unitsBSnap) {
        scheduleVision(u, enemiesBSnap, enemyIndexB, forcedBKeys, unitsBSnap,
                       incomingDamageB, lockedTargetsB, occSnap);
    }
    visionGroup->wait();

    // Apply stage (Vision): main thread writes only, deterministic order.
    {
        std::unique_lock<std::shared_mutex> lock(runtimeCtx.worldMutex);
        std::vector<VisionIntent> mergedVision;
        std::vector<TargetHint> mergedHints;
        for (auto& buffer : visionLocals) {
            mergedVision.insert(mergedVision.end(),
                                std::make_move_iterator(buffer.visionIntents.begin()),
                                std::make_move_iterator(buffer.visionIntents.end()));
            mergedHints.insert(mergedHints.end(),
                               std::make_move_iterator(buffer.targetHints.begin()),
                               std::make_move_iterator(buffer.targetHints.end()));
        }

        auto byUnitId = [](const auto& a, const auto& b) {
            return a.unitId < b.unitId;
        };
        std::sort(mergedVision.begin(), mergedVision.end(), byUnitId);
        std::sort(mergedHints.begin(), mergedHints.end(), byUnitId);
        state.lastVisionIntents = std::move(mergedVision);
        state.lastTargetHints = std::move(mergedHints);
    }

    // Plan stage (Movement): tasks read snapshots only; no world writes here.
    auto moveGroup = std::make_shared<TaskGroup>();
    std::vector<IntentBuffer> moveLocals(localCount);

    auto scheduleMove = [&](const UnitSnapshot& unit,
                            const std::vector<AttackableSnapshot>& enemies,
                            const std::unordered_map<AttackableKey, std::size_t, AttackableKeyHash>& enemyIndex,
                            const std::vector<AttackableKey>& forcedKeys,
                            const std::vector<UnitSnapshot>& allies,
                            const std::unordered_map<AttackableKey, float, AttackableKeyHash>& incomingDamage,
                            const std::unordered_map<AttackableKey, int, AttackableKeyHash>& lockedCounts,
                            bool baseAlive,
                            const Coord& basePos,
                            const std::unordered_set<std::uint64_t>& occ) {
        if (unit.hp <= 0.f) return;
        runtimeCtx.taskPool.submit([&, unit]() {
            MoveIntent intent = MovementPlanner::planMoveIntent(unit, dt, state.map, enemies, enemyIndex,
                                               forcedKeys, incomingDamage,
                                               lockedCounts, allies,
                                               baseAlive, basePos, occ);

            std::size_t idx = TaskPool::workerIndex();
            if (idx == TaskPool::kInvalidWorkerIndex || idx >= moveLocals.size()) {
                idx = 0;
            }
            moveLocals[idx].moveIntents.push_back(std::move(intent));
        }, moveGroup);
    };

    for (const auto& u : unitsASnap) {
        scheduleMove(u, enemiesASnap, enemyIndexA, forcedAKeys, unitsASnap,
                     incomingDamageA, lockedTargetsA, baseAAlive, baseAPos,
                     occSnap);
    }
    for (const auto& u : unitsBSnap) {
        scheduleMove(u, enemiesBSnap, enemyIndexB, forcedBKeys, unitsBSnap,
                     incomingDamageB, lockedTargetsB, baseBAlive, baseBPos,
                     occSnap);
    }
    moveGroup->wait();

    // Apply stage (Movement): main thread writes only, deterministic order.
    {
        std::unique_lock<std::shared_mutex> lock(runtimeCtx.worldMutex);
        std::vector<MoveIntent> mergedMoves;
        for (auto& buffer : moveLocals) {
            mergedMoves.insert(mergedMoves.end(),
                               std::make_move_iterator(buffer.moveIntents.begin()),
                               std::make_move_iterator(buffer.moveIntents.end()));
        }

        std::sort(mergedMoves.begin(), mergedMoves.end(),
                  [](const MoveIntent& a, const MoveIntent& b) {
                      return a.unitId < b.unitId;
                  });

        std::vector<std::shared_ptr<Unit>> allUnits;
        allUnits.reserve(state.unitsA.size() + state.unitsB.size());
        for (auto& u : state.unitsA) allUnits.push_back(u);
        for (auto& u : state.unitsB) allUnits.push_back(u);
        std::sort(allUnits.begin(), allUnits.end(),
                  [](const std::shared_ptr<Unit>& a, const std::shared_ptr<Unit>& b) {
                      return a->id < b->id;
                  });

        std::unordered_map<int, std::shared_ptr<Unit>> unitById;
        unitById.reserve(allUnits.size());
        for (auto& u : allUnits) {
            if (u) unitById[u->id] = u;
        }

        for (auto& intent : mergedMoves) {
            auto it = unitById.find(intent.unitId);
            if (it == unitById.end()) continue;
            auto& unit = it->second;
            if (!unit || !unit->behavior) continue;
            IMovementBehavior* movement = unit->behavior->getMovementBehavior();
            if (!movement) continue;
            movement->applyState(std::move(intent.nextState));
            unit->behavior->locomotionState =
                intent.setIdle ? LocomotionState::Idle : LocomotionState::Pathing;
            unit->behavior->moveReason =
                intent.setIdle ? MoveReason::None : intent.reason;
            unit->behavior->commandMoveActive =
                intent.commandMove && !intent.setIdle;
            unit->behavior->retreating = intent.retreating;
            unit->behavior->retreatTimer = intent.retreatTimer;
            unit->behavior->retreatAnchor = intent.retreatAnchor;
            unit->behavior->hasRetreatAnchor = intent.hasRetreatAnchor;
        }

        struct CoordKey {
            int x;
            int y;
        };
        struct CoordLess {
            bool operator()(const CoordKey& a, const CoordKey& b) const {
                if (a.y != b.y) return a.y < b.y;
                return a.x < b.x;
            }
        };

        std::map<CoordKey, std::vector<MoveIntent*>, CoordLess> byTarget;
        for (auto& intent : mergedMoves) {
            if (!intent.hasMove) continue;
            if (!state.map.inBounds(intent.to)) continue;
            if (!state.map.getTile(intent.to).isPassable()) continue;
            CoordKey key{intent.to.x, intent.to.y};
            byTarget[key].push_back(&intent);
        }

        auto movePriority = [](const MoveIntent* a, const MoveIntent* b) {
            if (a->commandMove != b->commandMove) return a->commandMove > b->commandMove;
            return a->unitId < b->unitId;
        };

        std::vector<MoveIntent*> winners;
        winners.reserve(byTarget.size());
        for (auto& entry : byTarget) {
            auto& candidates = entry.second;
            std::sort(candidates.begin(), candidates.end(), movePriority);
            winners.push_back(candidates.front());
        }

        std::sort(winners.begin(), winners.end(),
                  [](const MoveIntent* a, const MoveIntent* b) {
                      return a->unitId < b->unitId;
                  });

        std::unordered_set<int> primaryWinners;
        primaryWinners.reserve(winners.size());
        for (const auto* intent : winners) {
            primaryWinners.insert(intent->unitId);
        }

        std::unordered_set<std::uint64_t> occ;
        occ.reserve(state.unitsA.size() + state.unitsB.size() + 4);
        auto occupyIf = [&](bool ok, const Coord& c) {
            if (ok) occ.insert(Coord::packCoord(c));
        };
        occupyIf(state.baseA && !state.baseA->isDestroyed(), state.baseA->getPos());
        occupyIf(state.baseB && !state.baseB->isDestroyed(), state.baseB->getPos());
        for (auto& u : state.unitsA) occupyIf(u && u->isAlive(), u->getPos());
        for (auto& u : state.unitsB) occupyIf(u && u->isAlive(), u->getPos());

        for (const auto& intent : mergedMoves) {
            auto it = unitById.find(intent.unitId);
            if (it == unitById.end()) continue;
            auto& unit = it->second;
            if (!unit || !unit->isAlive()) continue;
            Coord from = unit->getPos();
            if (intent.candidateCount <= 0) continue;

            int startIdx = primaryWinners.count(intent.unitId) ? 0 : 1;
            Coord chosen = from;
            bool hasChoice = false;
            for (int i = startIdx; i < intent.candidateCount; ++i) {
                Coord cand = intent.candidates[static_cast<std::size_t>(i)];
                if (!state.map.inBounds(cand)) continue;
                if (!state.map.getTile(cand).isPassable()) continue;
                if (cand != from && occ.find(Coord::packCoord(cand)) != occ.end()) continue;
                chosen = cand;
                hasChoice = true;
                break;
            }
            if (!hasChoice || chosen == from) continue;

            auto kPrev = Coord::packCoord(from);
            auto kNow = Coord::packCoord(chosen);
            occ.erase(kPrev);
            if (occ.find(kNow) != occ.end()) {
                occ.insert(kPrev);
                continue;
            }
            unit->pos = chosen;
            occ.insert(kNow);
        }

        for (auto& unit : allUnits) {
            if (!unit || !unit->behavior || !unit->isAlive()) continue;
            if (unit->behavior->retreating) {
                unit->behavior->retreatTimer =
                    std::max(0.0f, unit->behavior->retreatTimer - dt);
            } else {
                unit->behavior->retreatTimer = 0.f;
                unit->behavior->hasRetreatAnchor = false;
            }
        }
    }

    // Snapshot stage for Attack planning (post-move/commands).
    std::vector<UnitSnapshot> unitsASnapAtk;
    std::vector<UnitSnapshot> unitsBSnapAtk;
    std::vector<AttackableSnapshot> enemiesASnapAtk;
    std::vector<AttackableSnapshot> enemiesBSnapAtk;
    std::unordered_map<AttackableKey, std::size_t, AttackableKeyHash> enemyIndexAAtk;
    std::unordered_map<AttackableKey, std::size_t, AttackableKeyHash> enemyIndexBAtk;
    std::unordered_set<std::uint64_t> occAtk;
    {
        std::shared_lock<std::shared_mutex> lock(runtimeCtx.worldMutex);
        occAtk.reserve(state.unitsA.size() + state.unitsB.size() + 2);
        buildUnitSnapshots(state.unitsA, unitsASnapAtk, &occAtk);
        buildUnitSnapshots(state.unitsB, unitsBSnapAtk, &occAtk);
        if (state.baseA && !state.baseA->isDestroyed()) occAtk.insert(Coord::packCoord(state.baseA->pos));
        if (state.baseB && !state.baseB->isDestroyed()) occAtk.insert(Coord::packCoord(state.baseB->pos));
        buildEnemySnapshots(state.unitsB, state.baseB, enemiesASnapAtk);
        buildEnemySnapshots(state.unitsA, state.baseA, enemiesBSnapAtk);
    }

    std::sort(unitsASnapAtk.begin(), unitsASnapAtk.end(), byUnitId);
    std::sort(unitsBSnapAtk.begin(), unitsBSnapAtk.end(), byUnitId);
    std::sort(enemiesASnapAtk.begin(), enemiesASnapAtk.end(), byAttackableKey);
    std::sort(enemiesBSnapAtk.begin(), enemiesBSnapAtk.end(), byAttackableKey);
    rebuildEnemyIndex(enemiesASnapAtk, enemyIndexAAtk);
    rebuildEnemyIndex(enemiesBSnapAtk, enemyIndexBAtk);

    // Plan stage (Attack): tasks read snapshots only; no world writes here.
    auto attackGroup = std::make_shared<TaskGroup>();
    std::vector<IntentBuffer> attackLocals(localCount);

    auto scheduleAttack = [&](const UnitSnapshot& unit,
                              const std::vector<AttackableSnapshot>& enemies,
                              const std::unordered_map<AttackableKey, std::size_t, AttackableKeyHash>& enemyIndex,
                              const std::vector<AttackableKey>& forcedKeys,
                              const std::vector<UnitSnapshot>& allies,
                              const std::unordered_map<AttackableKey, float, AttackableKeyHash>& incomingDamage,
                              const std::unordered_map<AttackableKey, int, AttackableKeyHash>& lockedCounts,
                              const std::unordered_set<std::uint64_t>& occ) {
        if (unit.hp <= 0.f) return;
        runtimeCtx.taskPool.submit([&, unit]() {
            AttackIntent intent = CombatPlanner::planAttackIntent(unit, dt, state.map, enemies,
                                                   enemyIndex, forcedKeys,
                                                   incomingDamage, lockedCounts,
                                                   allies, occ);

            std::size_t idx = TaskPool::workerIndex();
            if (idx == TaskPool::kInvalidWorkerIndex || idx >= attackLocals.size()) {
                idx = 0;
            }
            attackLocals[idx].attackIntents.push_back(std::move(intent));
        }, attackGroup);
    };

    for (const auto& u : unitsASnapAtk) {
        scheduleAttack(u, enemiesASnapAtk, enemyIndexAAtk, forcedAKeys, unitsASnapAtk,
                       incomingDamageA, lockedTargetsA, occAtk);
    }
    for (const auto& u : unitsBSnapAtk) {
        scheduleAttack(u, enemiesBSnapAtk, enemyIndexBAtk, forcedBKeys, unitsBSnapAtk,
                       incomingDamageB, lockedTargetsB, occAtk);
    }
    attackGroup->wait();

    // Apply stage (Attack): main thread writes only, deterministic order.
    {
        std::unique_lock<std::shared_mutex> lock(runtimeCtx.worldMutex);
        std::vector<std::shared_ptr<Unit>> allUnits;
        allUnits.reserve(state.unitsA.size() + state.unitsB.size());
        for (auto& u : state.unitsA) allUnits.push_back(u);
        for (auto& u : state.unitsB) allUnits.push_back(u);
        std::sort(allUnits.begin(), allUnits.end(),
                  [](const std::shared_ptr<Unit>& a, const std::shared_ptr<Unit>& b) {
                      return a->id < b->id;
                  });

        std::vector<std::weak_ptr<IAttackable>> forcedAAtk;
        std::vector<std::weak_ptr<IAttackable>> forcedBAtk;
        forcedAAtk.reserve(state.forcedVisibleForA.size());
        forcedBAtk.reserve(state.forcedVisibleForB.size());
        for (const auto& r : state.forcedVisibleForA) {
            if (!r.target.expired()) forcedAAtk.push_back(r.target);
        }
        for (const auto& r : state.forcedVisibleForB) {
            if (!r.target.expired()) forcedBAtk.push_back(r.target);
        }

        for (auto& u : allUnits) {
            if (!u || !u->behavior) continue;
            const auto& enemies = (u->getFaction() == Faction::A) ? state.enemiesA : state.enemiesB;
            const auto& forced = (u->getFaction() == Faction::A) ? forcedAAtk : forcedBAtk;
            u->behavior->updateVision(*u, state.map, enemies, forced);
        }

        std::vector<AttackIntent> mergedAttacks;
        for (auto& buffer : attackLocals) {
            mergedAttacks.insert(mergedAttacks.end(),
                                 std::make_move_iterator(buffer.attackIntents.begin()),
                                 std::make_move_iterator(buffer.attackIntents.end()));
        }

        std::sort(mergedAttacks.begin(), mergedAttacks.end(),
                  [](const AttackIntent& a, const AttackIntent& b) {
                      return a.attackerId < b.attackerId;
                  });

        auto resolveTarget = [&](int id, AttackableType type) -> std::shared_ptr<IAttackable> {
            if (id < 0) return nullptr;
            if (type == AttackableType::BASE) {
                auto base = data.findBase(id, Faction::A);
                if (!base) base = data.findBase(id, Faction::B);
                return base;
            }
            return data.findUnit(id);
        };

        std::map<std::pair<int, int>, float> damageByTarget;
        std::unordered_map<AttackableKey, float, AttackableKeyHash> incomingDamageAFrame;
        std::unordered_map<AttackableKey, float, AttackableKeyHash> incomingDamageBFrame;
        std::unordered_map<AttackableKey, int, AttackableKeyHash> lockedTargetsAFrame;
        std::unordered_map<AttackableKey, int, AttackableKeyHash> lockedTargetsBFrame;
        std::vector<int> attackersToReveal;
        std::vector<int> attackersDealtDamage;
        std::vector<int> damagedUnits;
        attackersToReveal.reserve(mergedAttacks.size());
        attackersDealtDamage.reserve(mergedAttacks.size());

        for (const auto& intent : mergedAttacks) {
            auto attacker = data.findUnit(intent.attackerId);
            if (!attacker || !attacker->behavior) continue;

            IAttackBehavior* attackBehavior = attacker->behavior->getAttackBehavior();
            if (!attackBehavior) continue;

            attackBehavior->setCooldown(intent.nextCooldown);
            if (intent.nextTargetId >= 0) {
                auto target = resolveTarget(intent.nextTargetId, intent.nextTargetType);
                attackBehavior->setTarget(std::weak_ptr<IAttackable>(target));
            } else {
                attackBehavior->setTarget(std::weak_ptr<IAttackable>{});
            }

            if (intent.nextTargetId >= 0) {
                AttackableKey key;
                key.id = intent.nextTargetId;
                key.type = intent.nextTargetType;
                if (attacker->getFaction() == Faction::A) {
                    lockedTargetsAFrame[key] += 1;
                } else {
                    lockedTargetsBFrame[key] += 1;
                }
                attacker->behavior->combatState = CombatState::Engaging;
                attacker->behavior->combatAction = intent.action;
            } else {
                attacker->behavior->combatState = CombatState::None;
                attacker->behavior->combatAction = CombatAction::None;
            }
            attacker->behavior->commitTimer = intent.nextCommitTimer;
            attacker->behavior->commitTargetId = intent.nextCommitTargetId;
            attacker->behavior->commitTargetType = intent.nextCommitTargetType;

            if (intent.didAttack && intent.targetId >= 0) {
                auto key = std::make_pair(static_cast<int>(intent.targetType), intent.targetId);
                damageByTarget[key] += intent.damage;
                attackersToReveal.push_back(intent.attackerId);
                AttackableKey dmgKey;
                dmgKey.id = intent.targetId;
                dmgKey.type = intent.targetType;
                if (attacker->getFaction() == Faction::A) {
                    incomingDamageAFrame[dmgKey] += intent.damage;
                } else {
                    incomingDamageBFrame[dmgKey] += intent.damage;
                }
                auto damageTarget = resolveTarget(intent.targetId, intent.targetType);
                if (damageTarget) {
                    attackersDealtDamage.push_back(intent.attackerId);
                    if (intent.targetType == AttackableType::UNIT) {
                        damagedUnits.push_back(intent.targetId);
                    }
                }
            }
        }

        const bool baseAAliveBefore = state.baseA && !state.baseA->isDestroyed();
        const bool baseBAliveBefore = state.baseB && !state.baseB->isDestroyed();

        for (const auto& entry : damageByTarget) {
            AttackableType type = static_cast<AttackableType>(entry.first.first);
            int targetId = entry.first.second;
            auto target = resolveTarget(targetId, type);
            if (target) {
                target->takeDamage(entry.second);
                WorldEvent evt;
                evt.type = WorldEventType::UnitDamaged;
                evt.targetType = type;
                evt.targetId = targetId;
                evt.value = entry.second;
                evt.faction = target->getFaction();
                evt.pos = target->getPos();
                runtimeCtx.eventBus.publish(evt);
            }
        }

        const bool baseAAliveAfter = state.baseA && !state.baseA->isDestroyed();
        const bool baseBAliveAfter = state.baseB && !state.baseB->isDestroyed();
        if (baseAAliveBefore && !baseAAliveAfter) {
            WorldEvent evt;
            evt.type = WorldEventType::BaseDestroyed;
            evt.faction = Faction::A;
            runtimeCtx.eventBus.publish(evt);
            if (baseBAliveAfter) {
                WorldEvent ended;
                ended.type = WorldEventType::GameEnded;
                ended.faction = Faction::B;
                runtimeCtx.eventBus.publish(ended);
            }
        }
        if (baseBAliveBefore && !baseBAliveAfter) {
            WorldEvent evt;
            evt.type = WorldEventType::BaseDestroyed;
            evt.faction = Faction::B;
            runtimeCtx.eventBus.publish(evt);
            if (baseAAliveAfter) {
                WorldEvent ended;
                ended.type = WorldEventType::GameEnded;
                ended.faction = Faction::A;
                runtimeCtx.eventBus.publish(ended);
            }
        }

        std::sort(attackersToReveal.begin(), attackersToReveal.end());
        attackersToReveal.erase(
            std::unique(attackersToReveal.begin(), attackersToReveal.end()),
            attackersToReveal.end()
        );
        for (int attackerId : attackersToReveal) {
            auto attacker = data.findUnit(attackerId);
            if (attacker) {
                data.revealAttacker(*attacker);
            }
        }

        for (auto& u : allUnits) {
            if (!u || !u->behavior) continue;
            const auto& enemies = (u->getFaction() == Faction::A) ? state.enemiesA : state.enemiesB;
            u->behavior->postAttackStateUpdate(*u, state.map, enemies);
        }

        std::sort(attackersDealtDamage.begin(), attackersDealtDamage.end());
        attackersDealtDamage.erase(
            std::unique(attackersDealtDamage.begin(), attackersDealtDamage.end()),
            attackersDealtDamage.end()
        );
        std::sort(damagedUnits.begin(), damagedUnits.end());
        damagedUnits.erase(
            std::unique(damagedUnits.begin(), damagedUnits.end()),
            damagedUnits.end()
        );

        auto findUnitAny = [&](int id) -> std::shared_ptr<Unit> {
            for (auto& u : state.unitsA) {
                if (u && u->id == id) return u;
            }
            for (auto& u : state.unitsB) {
                if (u && u->id == id) return u;
            }
            return nullptr;
        };
        for (int id : damagedUnits) {
            auto u = findUnitAny(id);
            if (!u || u->isAlive()) continue;
            WorldEvent evt;
            evt.type = WorldEventType::UnitDied;
            evt.unitId = id;
            evt.faction = u->getFaction();
            evt.pos = u->getPos();
            runtimeCtx.eventBus.publish(evt);
        }

        for (auto& u : allUnits) {
            if (!u || !u->behavior || !u->isAlive()) continue;
            u->behavior->timeSinceDamaged += dt;
            u->behavior->timeSinceDealtDamage += dt;

            if (std::binary_search(damagedUnits.begin(), damagedUnits.end(), u->id)) {
                u->behavior->timeSinceDamaged = 0.f;
            }
            if (std::binary_search(attackersDealtDamage.begin(), attackersDealtDamage.end(), u->id)) {
                u->behavior->timeSinceDealtDamage = 0.f;
            }

            if (u->behavior->timeSinceDamaged > AiScoring::kOocParams.delay &&
                u->behavior->timeSinceDealtDamage > AiScoring::kOocParams.delay) {
                const auto& enemyUnits = (u->getFaction() == Faction::A) ? state.unitsB : state.unitsA;
                float threat = AiTargetScoring::computeLocalThreatWorld(*u, enemyUnits, AiScoring::kThreatRadius);
                if (threat < AiScoring::kOocParams.threatThreshold) {
                    float regen = u->baseStats.maxHP * AiScoring::kOocParams.regenRate * dt;
                    u->hp = std::min(u->baseStats.maxHP, u->hp + regen);
                }
            }
        }

        state.lastIncomingDamageA = std::move(incomingDamageAFrame);
        state.lastIncomingDamageB = std::move(incomingDamageBFrame);
        state.lastLockedTargetsA = std::move(lockedTargetsAFrame);
        state.lastLockedTargetsB = std::move(lockedTargetsBFrame);

        systems.cleanup.update(data);

        // 清理选中列表中已不存在的单位
        std::unordered_set<int> alive;
        for (auto& u : state.unitsA) if (u && u->isAlive()) alive.insert(u->id);
        for (auto& u : state.unitsB) if (u && u->isAlive()) alive.insert(u->id);
        controlCtx.selectedUnitIds.erase(
            std::remove_if(controlCtx.selectedUnitIds.begin(), controlCtx.selectedUnitIds.end(),
                [&](int id){ return alive.find(id) == alive.end(); }),
            controlCtx.selectedUnitIds.end()
        );
    }
}

void GameWorld::appendUnitSnapshot(
    const std::shared_ptr<Unit>& u,
    std::vector<AiScoring::UnitSnapshot>& out,
    std::unordered_set<std::uint64_t>* occ) {
    if (!u || !u->isAlive()) return;

    AiScoring::UnitSnapshot snap;
    snap.id = u->id;
    snap.type = u->type;
    snap.faction = u->owner;
    snap.pos = u->pos;
    snap.hp = u->hp;
    snap.maxHp = u->baseStats.maxHP;
    snap.stats = u->baseStats;

    if (u->behavior) {
        if (auto* attack = u->behavior->getAttackBehavior()) {
            snap.cooldown = attack->getCooldown();
            snap.currentTarget = AttackableKey::from(attack->getTarget().lock());
        }

        snap.commitTimer = u->behavior->commitTimer;
        snap.commitTarget.id = u->behavior->commitTargetId;
        snap.commitTarget.type = u->behavior->commitTargetType;

        snap.commandAttackActive = u->behavior->commandAttackActive;
        snap.commandTarget.id = u->behavior->commandAttackTargetId;
        snap.commandTarget.type = u->behavior->commandAttackTargetType;

        snap.retreating = u->behavior->retreating;
        snap.retreatTimer = u->behavior->retreatTimer;
        snap.retreatAnchor = u->behavior->retreatAnchor;
        snap.hasRetreatAnchor = u->behavior->hasRetreatAnchor;

        snap.timeSinceDamaged = u->behavior->timeSinceDamaged;
        snap.timeSinceDealtDamage = u->behavior->timeSinceDealtDamage;

        snap.commandMoveActive = u->behavior->commandMoveActive;
        snap.locomotionState = u->behavior->locomotionState;
        snap.combatState = u->behavior->combatState;
        snap.moveReason = u->behavior->moveReason;

        if (auto* movement = u->behavior->getMovementBehavior()) {
            snap.movementState = movement->snapshot();
        }
    }

    out.push_back(std::move(snap));
    if (occ) occ->insert(Coord::packCoord(u->pos));
}

void GameWorld::buildUnitSnapshots(
    const std::vector<std::shared_ptr<Unit>>& units,
    std::vector<AiScoring::UnitSnapshot>& out,
    std::unordered_set<std::uint64_t>* occ) {
    for (const auto& u : units) {
        appendUnitSnapshot(u, out, occ);
    }
}

void GameWorld::buildEnemySnapshots(
    const std::vector<std::shared_ptr<Unit>>& units,
    const std::shared_ptr<Base>& base,
    std::vector<AiScoring::AttackableSnapshot>& out) {
    if (base && !base->isDestroyed()) {
        AiScoring::AttackableSnapshot snap;
        snap.key.id = base->id;
        snap.key.type = AttackableType::BASE;
        snap.type = AttackableType::BASE;
        snap.faction = base->faction;
        snap.unitType = UnitType::Infantry;
        snap.pos = base->pos;
        snap.hp = base->hp;
        snap.maxHp = base->maxHp;
        snap.attack = 0.f;
        snap.attackRange = 0.f;
        snap.armor = 0.f;
        snap.isBase = true;
        out.push_back(std::move(snap));
    }

    for (const auto& u : units) {
        if (!u || !u->isAlive()) continue;
        AiScoring::AttackableSnapshot snap;
        snap.key.id = u->id;
        snap.key.type = AttackableType::UNIT;
        snap.type = AttackableType::UNIT;
        snap.faction = u->owner;
        snap.unitType = u->type;
        snap.pos = u->pos;
        snap.hp = u->hp;
        snap.maxHp = u->baseStats.maxHP;
        snap.attack = u->baseStats.attack;
        snap.attackRange = u->baseStats.attackRange;
        snap.armor = u->baseStats.armor;
        snap.isBase = false;
        out.push_back(std::move(snap));
    }
}

void GameWorld::rebuildEnemyIndex(
    const std::vector<AiScoring::AttackableSnapshot>& snaps,
    std::unordered_map<AttackableKey, std::size_t, AttackableKeyHash>& index) {
    index.clear();
    index.reserve(snaps.size());
    for (std::size_t i = 0; i < snaps.size(); ++i) {
        index[snaps[i].key] = i;
    }
}

void GameWorld::buildForcedKeys(
    const std::vector<ForcedReveal>& forced,
    std::vector<AttackableKey>& out) {
    for (const auto& r : forced) {
        auto target = r.target.lock();
        if (!target) continue;
        out.push_back(AttackableKey::from(target));
    }
}


bool WorldDataContext::isTileFree(const Coord& c) const {
    for (auto& u : unitsA) {
        if (u->isAlive() && u->getPos() == c) return false;
    }

    for (auto& u : unitsB) {
        if (u->isAlive() && u->getPos() == c) return false;
    }

    if (baseA && !baseA->isDestroyed() && baseA->getPos() == c)
        return false;
    if (baseB && !baseB->isDestroyed() && baseB->getPos() == c)
        return false;

    return true;
}

void WorldDataContext::rebuildEnemies() {
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

std::shared_ptr<Unit> WorldDataContext::findUnit(int id) const {
    if (id < 0) return nullptr;
    for (auto& u : unitsA) {
        if (u && u->id == id && u->isAlive()) return u;
    }
    for (auto& u : unitsB) {
        if (u && u->id == id && u->isAlive()) return u;
    }
    return nullptr;
}

std::shared_ptr<Base> WorldDataContext::findBase(int id, Faction fac) const {
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

std::shared_ptr<IAttackable> WorldDataContext::findAttackable(int id) const {
    if (id < 0) return nullptr;
    auto u = findUnit(id);
    if (u) return u;

    if (baseA && baseA->getId() == id && !baseA->isDestroyed()) return baseA;
    if (baseB && baseB->getId() == id && !baseB->isDestroyed()) return baseB;
    return nullptr;
}

int WorldDataContext::registerUnit(const std::shared_ptr<Unit>& u) {
    if (!u) return -1;
    u->id = nextUnitId++;
    return u->id;
}

int WorldDataContext::registerBase(const std::shared_ptr<Base>& b) {
    if (!b) return -1;
    b->setId(nextBaseId++);
    return b->getId();
}

void WorldDataContext::addForcedReveal(Faction viewer,
                                       const std::shared_ptr<IAttackable>& target,
                                       float durationSeconds) {
    if (!target) return;
    auto& bucket = (viewer == Faction::A) ? forcedVisibleForA : forcedVisibleForB;
    bucket.push_back({target, durationSeconds});
}

void WorldDataContext::decayForcedReveals(float dt) {
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

void WorldDataContext::appendForcedReveals(Faction viewer,
                                           std::vector<std::weak_ptr<IAttackable>>& out) const {
    const auto& bucket = (viewer == Faction::A) ? forcedVisibleForA : forcedVisibleForB;
    for (const auto& r : bucket) {
        if (!r.target.expired()) {
            out.push_back(r.target);
        }
    }
}

void WorldDataContext::revealAttacker(const Unit& u) {
    auto attacker = findUnit(u.id);
    if (!attacker) return;
    Faction viewer = (u.owner == Faction::A) ? Faction::B : Faction::A;
    addForcedReveal(viewer, attacker, 2.0f);
}

bool WorldRuntimeContext::shouldQuit() const {
    return quitRequested.load();
}

void WorldRuntimeContext::requestQuit() {
    quitRequested.store(true);
}

bool WorldRuntimeContext::isRenderRunning() const {
    return renderRunning.load();
}

bool WorldRuntimeContext::isPaused() const {
    return paused.load();
}

void WorldRuntimeContext::pause() {
    paused.store(true);
}

void WorldRuntimeContext::resume() {
    paused.store(false);
}

void WorldRuntimeContext::togglePause() {
    if (paused.load()) {
        resume();
    } else {
        pause();
    }
}

void WorldRuntimeContext::markGameOver() {
    gameEnded.store(true);
    const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    gameEndTimestampMs.store(nowMs);
    pause();
}

bool WorldRuntimeContext::hasGameEnded() const {
    return gameEnded.load();
}

long long WorldRuntimeContext::gameEndMs() const {
    return gameEndTimestampMs.load();
}

void WorldRuntimeContext::enqueueUiEvent(std::optional<std::string> input,
                                         std::optional<std::string> feedback) {
    runtime.enqueueUiEvent(std::move(input), std::move(feedback));
}

void WorldRuntimeContext::drainUiEvents(WorldControlContext& control) {
    std::queue<WorldRuntime::UiEvent> local;
    {
        std::lock_guard<std::mutex> lock(uiEventMutex);
        std::swap(local, uiEvents);
    }
    while (!local.empty()) {
        const WorldRuntime::UiEvent& evt = local.front();
        if (evt.input.has_value()) {
            control.lastCommandInput = *evt.input;
        }
        if (evt.feedback.has_value()) {
            control.lastCommandFeedback = *evt.feedback;
        }
        local.pop();
    }
}

void WorldControlContext::enqueueCommand(const std::string& line) {
    commandQueue.push(line);
}

void WorldControlContext::processCommands(WorldDataContext& data, WorldRuntimeContext& runtime) {
    std::string line;
    while (commandQueue.tryPop(line)) {
        lastCommandInput = line;
        Command parsed;
        std::string err;
        if (!parseCommand(line, parsed, err)) {
            lastCommandFeedback = "ERR: " + err;
            continue;
        }
        CommandResult r = executeCommand(parsed, data, *this, runtime);
        if (r.ok) {
            lastCommandFeedback = r.normalized + " -> " + r.message;
        } else {
            lastCommandFeedback = "ERR: " + r.message;
        }
    }
}

void WorldControlContext::setSelection(const std::vector<int>& ids) {
    selectedUnitIds = ids;
}

void WorldControlContext::clearSelection() {
    selectedUnitIds.clear();
}

std::optional<UnitType> WorldControlContext::unitTypeFromCode(int code) const {
    if (code < 0) return std::nullopt;
    // Extend this list in order (1,2,3,...) to add more unit codes.
    static const std::vector<UnitType> mapping = {
        UnitType::Infantry,
        UnitType::Archer,
        UnitType::Knight
    };

    if (code >= 1 && code <= static_cast<int>(mapping.size())) {
        return mapping[static_cast<std::size_t>(code - 1)];
    }
    return std::nullopt;
}

void WorldControlContext::beginProductionChoice(const std::shared_ptr<Base>& base,
                                                WorldRuntimeContext& runtime) {
    if (!base) return;
    awaitingProductionChoice = true;
    productionChoiceBase = base;
    productionInputBuffer.clear();
    runtime.pause();
    runtime.enqueueUiEvent("production choice",
                           "Enter unit code then press Enter; Esc to cancel");
}

bool WorldControlContext::handleProductionDigit(char digit, WorldRuntimeContext& runtime) {
    if (!std::isdigit(static_cast<unsigned char>(digit))) return false;
    productionInputBuffer.push_back(digit);
    runtime.enqueueUiEvent(std::nullopt,
                           "Production code: " + productionInputBuffer + " \n\t(Enter to confirm)");
    return true;
}

bool WorldControlContext::handleProductionBackspace(WorldRuntimeContext& runtime) {
    if (productionInputBuffer.empty()) return false;
    productionInputBuffer.pop_back();
    runtime.enqueueUiEvent(std::nullopt,
                           productionInputBuffer.empty()
                               ? "Production code cleared"
                               : "Production code: " + productionInputBuffer);
    return true;
}

bool WorldControlContext::commitProductionChoice(WorldRuntimeContext& runtime) {
    auto basePtr = productionChoiceBase.lock();
    if (!basePtr) {
        cancelProductionChoice(runtime);
        return false;
    }

    if (productionInputBuffer.empty()) {
        runtime.enqueueUiEvent(std::nullopt, "Please enter a numeric code");
        return false;
    }

    auto ignoreInvalid = [&](const std::string& msg) {
        awaitingProductionChoice = false;
        productionChoiceBase.reset();
        productionInputBuffer.clear();
        runtime.resume();
        runtime.enqueueUiEvent(std::nullopt, msg);
    };

    int code = -1;
    try {
        code = std::stoi(productionInputBuffer);
    } catch (...) {
        ignoreInvalid("Invalid code, ignored");
        return false;
    }

    auto type = unitTypeFromCode(code);
    if (!type.has_value()) {
        ignoreInvalid("Unsupported code ignored: " + productionInputBuffer);
        return false;
    }

    basePtr->issueProduce(*type);
    runtime.enqueueUiEvent(std::nullopt,
                           "Queued " +
                               std::string(*type == UnitType::Infantry ? "Infantry" :
                                           *type == UnitType::Archer   ? "Archer"   : "Knight"));
    awaitingProductionChoice = false;
    productionChoiceBase.reset();
    productionInputBuffer.clear();
    runtime.resume();
    return true;
}

void WorldControlContext::cancelProductionChoice(WorldRuntimeContext& runtime) {
    awaitingProductionChoice = false;
    productionChoiceBase.reset();
    productionInputBuffer.clear();
    runtime.enqueueUiEvent(std::nullopt, "Production canceled");
    runtime.resume();
}

void WorldControlContext::resetTargeting() {
    pendingTarget.reset();
    controlMode = ControlState::ControlMode::Idle;
}

void WorldControlContext::enterTargeting() {
    pendingTarget.reset();
    controlMode = ControlState::ControlMode::Targeting;
}

void WorldControlContext::cancelTargeting() {
    resetTargeting();
}

void WorldControlContext::commitTargeting() {
    resetTargeting();
}

std::shared_ptr<Unit> UnitFactory::create(UnitType type,
                                          const Coord& start,
                                          Faction faction) {
    return std::make_shared<Unit>(type, start, faction);
}
