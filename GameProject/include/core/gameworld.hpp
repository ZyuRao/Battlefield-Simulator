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
#include <optional>
#include <array>
#include <cstddef>
#include <unordered_map>
#include <mutex>
#include <queue>
#ifdef _WIN32
    #include <windows.h>
#endif
#include <SFML/Graphics.hpp>
#include "command.hpp"



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

class TaskGroup {
private:
    std::atomic<int> count{0};
    std::atomic<bool> alive{true};
    std::mutex mutex;
    std::condition_variable cv;

public:
    ~TaskGroup();
    void add(int n = 1);
    void done();
    void wait();
    bool isAlive() const { return alive.load(std::memory_order_acquire); }
    int debugCount() const { return count.load(std::memory_order_acquire); }
};

class TaskPool {
public:
    using Job = std::function<void()>;
    static constexpr std::size_t kInvalidWorkerIndex =
        static_cast<std::size_t>(-1);
private:
    std::vector<std::thread> workers;
    std::queue<Job>          jobs;
    std::mutex               queueMutex;
    std::condition_variable  cv;
    std::atomic<bool>        stopping;
    std::size_t              requestedThreads = 0;
    std::atomic<bool>        initialized{false};
    static thread_local std::size_t tlsWorkerIndex;

    void workerLoop(std::size_t workerIndex);

public:
    TaskPool();
    TaskPool(std::size_t threadCount = 0);
    ~TaskPool();

    void submit(Job job, std::shared_ptr<TaskGroup> group = nullptr);

    void shutdown();
    void init();
    std::size_t workerCount() const { return workers.size(); }
    static std::size_t workerIndex();
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

struct VisionIntent {
    int unitId = -1;
    std::vector<int> visibleEnemyIds;
};

struct TargetHint {
    int unitId = -1;
    int targetId = -1;
};

struct MoveIntent {
    int unitId = -1;
    Coord from{};
    Coord to{};
    bool hasMove = false;
    bool commandMove = false;
    bool setIdle = false;
    IMovementBehavior::MovementState nextState;
};

struct AttackIntent {
    int attackerId = -1;
    int targetId = -1;
    AttackableType targetType = AttackableType::UNIT;
    float damage = 0.f;
    int nextTargetId = -1;
    AttackableType nextTargetType = AttackableType::UNIT;
    float nextCooldown = 0.f;
    bool didAttack = false;
};

struct ProduceIntent {
    int baseId = -1;
    Faction faction = Faction::A;
    UnitType type = UnitType::Infantry;
};

struct IntentBuffer {
    std::vector<VisionIntent> visionIntents;
    std::vector<TargetHint> targetHints;
    std::vector<MoveIntent> moveIntents;
    std::vector<AttackIntent> attackIntents;
    std::vector<ProduceIntent> produceIntents;

    void clear() {
        visionIntents.clear();
        targetHints.clear();
        moveIntents.clear();
        attackIntents.clear();
        produceIntents.clear();
    }
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
    sf::Texture tilesheetTexture;
    bool texturesLoaded = false;

    static constexpr std::size_t kTileTypeCount = 6;
    static constexpr std::size_t kUnitTypeCount = 3;
    struct TileAtlas {
        sf::IntRect plain{};
        sf::IntRect swamp{};
        sf::IntRect river{};
        sf::IntRect hillBase{};
        sf::IntRect mountBase{};
        sf::IntRect forest{};
        sf::IntRect hillOverlay{};
        sf::IntRect mountOverlay{};
    };
    TileAtlas tileAtlas{};
    std::array<sf::IntRect, kTileTypeCount> tileRects{};
    std::array<sf::IntRect, kUnitTypeCount> unitRectsA{};
    std::array<sf::IntRect, kUnitTypeCount> unitRectsB{};
    sf::IntRect baseRect{};

    // 工具函数
    void ensureFontLoaded();
    void loadTextures();
    void initAtlasMapping();
    sf::IntRect tilesheetRect(int col, int row) const;
    sf::IntRect tileRectFor(TileType t) const;
    sf::IntRect unitRectFor(UnitType t, Faction f) const;
    sf::Vector2f tileTopLeft(const Layout& layout, const Coord& c) const;
    sf::Vector2f tileCenter(const Layout& layout, const Coord& c) const;

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

    void drawCommandPanel(const GameWorld& world,
                          sf::RenderWindow& window,
                          const Layout& layout);

    void drawIntroOverlay(sf::RenderWindow& window);
    void drawWinOverlay(const GameWorld& world,
                        sf::RenderWindow& window,
                        const Layout& layout);

    std::optional<Coord> pixelToTile(const GameWorld& world,
                                     const sf::RenderWindow& window,
                                     const sf::Vector2i& pixel) const;

    void drawSelectionRing(sf::RenderWindow& window,
                           sf::Vector2f center,
                           float radius,
                           Faction f);
    void drawFactionRing(sf::RenderWindow& window,
                         sf::Vector2f center,
                         float radius,
                         float thickness,
                         Faction f);

    sf::Clock clock;
    bool inputActive = false;
    std::string inputBuffer;
    friend class GameWorld;
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
    std::atomic<bool>             paused{false};
    std::atomic<bool>             gameEnded{false};
    std::atomic<long long>        gameEndTimestampMs{0};

    // --- 命令和交互状态 ---
    enum class ControlMode { Idle, Targeting };
    struct PendingTarget {
        enum class Kind { Tile, Unit };
        Kind kind = Kind::Tile;
        Coord tile{};
        int unitId = -1;
    };
    CommandQueue           commandQueue;
    std::string            lastCommandInput;
    std::string            lastCommandFeedback;
    std::vector<int>       selectedUnitIds;
    ControlMode            controlMode = ControlMode::Idle;
    std::optional<PendingTarget> pendingTarget;
    std::atomic<bool>      quitRequested{false};
    int                    nextUnitId = 1;
    int                    nextBaseId = 1;
    bool                   awaitingProductionChoice = false;
    std::weak_ptr<Base>    productionChoiceBase;
    std::string            productionInputBuffer;

    mutable std::shared_mutex worldMutex;
    struct UiEvent {
        std::optional<std::string> input;
        std::optional<std::string> feedback;
    };
    std::mutex               uiEventMutex;
    std::queue<UiEvent>      uiEvents;
    struct ForcedReveal {
        std::weak_ptr<IAttackable> target;
        float                      timeLeft;
    };
    std::vector<ForcedReveal> forcedVisibleForA;
    std::vector<ForcedReveal> forcedVisibleForB;
    std::vector<VisionIntent> lastVisionIntents;
    std::vector<TargetHint>   lastTargetHints;

    
    friend class TaskPool;
    friend class MovementSystem;
    friend class VisionSystem;
    friend class AttackSystem;
    friend class CleanupSystem;
    friend class BaseSystem;
    friend class RenderSystem;
    friend class Game;
    friend CommandResult executeCommand(const Command& cmd, GameWorld& world);

public:
    GameWorld();
    ~GameWorld();
    
    void update(float dt);

    void startRenderThread();
    void stopRenderThread();
    void waitRenderThread();


    const BaseSystem& getBaseSystem() const { return baseSystem; }
    
    bool isTileFree(const Coord& c) const; 
    
    void rebuildEnemies();

    // 命令/选中/辅助接口
    void enqueueCommand(const std::string& line);
    void processCommands();
    void drainUiEvents();
    void enqueueUiEvent(std::optional<std::string> input,
                        std::optional<std::string> feedback);

    std::shared_ptr<Unit> findUnit(int id) const;
    std::shared_ptr<Base> findBase(int id, Faction fac) const;
    std::shared_ptr<IAttackable> findAttackable(int id) const;

    void setSelection(const std::vector<int>& ids);
    void clearSelection();
    const std::vector<int>& getSelection() const { return selectedUnitIds; }

    const std::string& getLastCommandInput() const { return lastCommandInput; }
    const std::string& getLastCommandFeedback() const { return lastCommandFeedback; }

    bool shouldQuit() const { return quitRequested.load(std::memory_order_acquire); }
    void requestQuit() { quitRequested.store(true, std::memory_order_release); }
    bool isRenderRunning() const { return renderRunning.load(); }
    bool isPaused() const { return paused.load(); }
    void pause();
    void resume();
    void togglePause();
    void markGameOver();
    bool hasGameEnded() const { return gameEnded.load(); }
    long long gameEndMs() const { return gameEndTimestampMs.load(); }
    void addForcedReveal(Faction viewer, const std::shared_ptr<IAttackable>& target, float durationSeconds);
    void decayForcedReveals(float dt);
    void appendForcedReveals(Faction viewer, std::vector<std::weak_ptr<IAttackable>>& out) const;
    void revealAttacker(const Unit& u);
    void beginProductionChoice(const std::shared_ptr<Base>& base);
    bool handleProductionDigit(char digit);
    bool handleProductionBackspace();
    bool commitProductionChoice();
    void cancelProductionChoice();
    std::optional<UnitType> unitTypeFromCode(int code) const;

    int registerUnit(const std::shared_ptr<Unit>& u);
    int registerBase(const std::shared_ptr<Base>& b);

    
};
