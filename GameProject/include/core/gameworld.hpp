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
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <queue>
#ifdef _WIN32
    #include <windows.h>
#endif
#include <SFML/Graphics.hpp>
#include "command.hpp"


class GameWorld;
struct WorldDataContext;
struct WorldRuntimeContext;
struct WorldControlContext;
struct SystemsBundle;
struct InputStateMachine;


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

    void update(WorldDataContext& data, float dt);

};

class VisionSystem {
public:
    VisionSystem() = default;

    void update(WorldDataContext& data);
};
class AttackSystem {
public:
    AttackSystem() = default;

    void update(WorldDataContext& data, float dt);
};

class CleanupSystem {
public:
    CleanupSystem() = default;

    void update(WorldDataContext& data);
};

struct VisionIntent {
    int unitId = -1;
    std::vector<int> visibleEnemyIds;
};

struct TargetHint {
    int unitId = -1;
    int targetId = -1;
};

struct AttackableKey {
    int id = -1;
    AttackableType type = AttackableType::UNIT;

    bool operator==(const AttackableKey& other) const {
        return id == other.id && type == other.type;
    }
};

struct AttackableKeyHash {
    std::size_t operator()(const AttackableKey& key) const {
        std::size_t h1 = std::hash<int>{}(key.id);
        std::size_t h2 = std::hash<int>{}(static_cast<int>(key.type));
        return h1 ^ (h2 + 0x9e3779b9u + (h1 << 6) + (h1 >> 2));
    }
};

struct MoveIntent {
    int unitId = -1;
    Coord from{};
    Coord to{};
    bool hasMove = false;
    bool commandMove = false;
    bool setIdle = false;
    MoveReason reason = MoveReason::None;
    std::array<Coord, 3> candidates{};
    int candidateCount = 0;
    bool retreating = false;
    float retreatTimer = 0.f;
    Coord retreatAnchor{};
    bool hasRetreatAnchor = false;
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
    float nextCommitTimer = 0.f;
    int nextCommitTargetId = -1;
    AttackableType nextCommitTargetType = AttackableType::UNIT;
    CombatAction action = CombatAction::None;
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

struct AiScoring {
    struct UnitSnapshot {
        int id = -1;
        UnitType type = UnitType::Infantry;
        Faction faction = Faction::A;
        Coord pos{};
        float hp = 0.f;
        float maxHp = 0.f;
        UnitStats stats{};
        float cooldown = 0.f;
        AttackableKey currentTarget{};
        float commitTimer = 0.f;
        AttackableKey commitTarget{};
        bool commandAttackActive = false;
        AttackableKey commandTarget{};
        bool retreating = false;
        float retreatTimer = 0.f;
        Coord retreatAnchor{};
        bool hasRetreatAnchor = false;
        float timeSinceDamaged = 0.f;
        float timeSinceDealtDamage = 0.f;
        bool commandMoveActive = false;
        LocomotionState locomotionState = LocomotionState::Idle;
        CombatState combatState = CombatState::None;
        MoveReason moveReason = MoveReason::None;
        IMovementBehavior::MovementState movementState;
    };

    struct AttackableSnapshot {
        AttackableKey key{};
        AttackableType type = AttackableType::UNIT;
        Faction faction = Faction::A;
        UnitType unitType = UnitType::Infantry;
        Coord pos{};
        float hp = 0.f;
        float maxHp = 0.f;
        float attack = 0.f;
        float attackRange = 0.f;
        float armor = 0.f;
        bool isBase = false;
    };

    struct TargetScoreParams {
        float distanceWeight = 1.2f;
        float threatWeight = 1.0f;
        float ttkWeight = 1.4f;
        float executeWeight = 0.9f;
        float preferenceWeight = 1.2f;
        float overkillWeight = 1.1f;
        float lockWeight = 0.6f;
        float baseWeight = 0.8f;
        float baseThreatPenalty = 1.2f;
        float baseFinishBonus = 0.7f;
        float commitBonus = 0.8f;
        float commitDuration = 0.6f;
        float retreatHpFrac = 0.35f;
        float threatRetreat = 32.f;
        float kiteRangeMargin = 0.8f;
        float kiteExtraDist = 1.5f;
        float siegeOpportunity = 1.25f;
        float siegeHoldTime = 0.8f;
        float losPenalty = 1.2f;
        float unreachablePenalty = 1.8f;
        float availabilityWeight = 0.9f;
        int availabilityCap = 8;
    };

    struct ActionParams {
        float retreatHpFrac = 0.35f;
        float threatRetreat = 32.f;
        float kiteMargin = 0.8f;
        float kiteExtraDist = 1.5f;
        float siegeOpportunity = 1.25f;
    };

    struct RetreatParams {
        float hpEnter = 0.35f;
        float hpExit = 0.55f;
        float threatEnter = 32.f;
        float threatExit = 22.f;
        float retreatMinTime = 0.8f;
        int maxRetreatDist = 12;
        int sampleRadius = 6;
        float retreatFireThreat = 24.f;
    };

    struct OocParams {
        float delay = 2.0f;
        float regenRate = 0.08f;
        float threatThreshold = 12.f;
    };

    struct ReachableGrid {
        int width = 0;
        int height = 0;
        std::vector<std::uint8_t> reachable;

        bool isReachable(const Coord& c) const {
            if (c.x < 0 || c.x >= width || c.y < 0 || c.y >= height) return false;
            std::size_t idx = static_cast<std::size_t>(c.y) * width + c.x;
            return reachable[idx] != 0;
        }
    };

    struct TargetChoice {
        const AttackableSnapshot* target = nullptr;
        float score = -1e30f;
        float baseOpportunity = 0.f;
    };

    static const TargetScoreParams kTargetParams;
    static const ActionParams kActionParams;
    static const RetreatParams kRetreatParams;
    static const OocParams kOocParams;
    static constexpr float kThreatRadius = 6.0f;
};

class BaseSystem {
public:
    BaseSystem();

    void update(WorldDataContext& data, float dt);
    void spawnUnit(UnitType t, Base& base, WorldDataContext& data) const;

private:

    mutable std::mt19937 rng;
    Coord findSpawnPos(const Base& base, const WorldDataContext& data) const;
};

struct UnitFactory {
    static std::shared_ptr<Unit> create(UnitType type,
                                        const Coord& start,
                                        Faction faction);
};

class RenderSystem {
public:
    RenderSystem();

    void renderAscii(const WorldDataContext& data);
    //for ASCII
    void renderSfml(const WorldDataContext& data,
                    const WorldControlContext& control,
                    const WorldRuntimeContext& runtime,
                    sf::RenderWindow& window);

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

    Layout   computeLayout(const WorldDataContext& data,
                           const sf::RenderWindow& window) const;
    sf::Color tileColor(TileType t) const;
    sf::Color factionColor(Faction f) const;
    sf::Color unitTypeColor(UnitType t) const;

    void drawMapLayer(const WorldDataContext& data,
                      sf::RenderWindow& window,
                      const Layout& layout);

    void drawBaseLayer(const WorldDataContext& data,
                       const WorldControlContext& control,
                       sf::RenderWindow& window,
                       const Layout& layout);

    void drawUnitLayer(const WorldDataContext& data,
                       const WorldControlContext& control,
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

    void drawHud(const WorldDataContext& data,
                 const WorldControlContext& control,
                 const WorldRuntimeContext& runtime,
                 sf::RenderWindow& window,
                 const Layout& layout);

    void drawCommandPanel(const WorldControlContext& control,
                          sf::RenderWindow& window,
                          const Layout& layout);

    void drawIntroOverlay(sf::RenderWindow& window);
    void drawWinOverlay(const WorldDataContext& data,
                        sf::RenderWindow& window,
                        const Layout& layout);

    std::optional<Coord> pixelToTile(const WorldDataContext& data,
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
    friend struct WorldRuntimeContext;
    friend struct InputStateMachine;
};

struct ForcedReveal {
    std::weak_ptr<IAttackable> target;
    float timeLeft = 0.f;
};

enum class WorldEventType {
    UnitDamaged,
    UnitDied,
    BaseDestroyed,
    GameEnded,
    CommandIssued,
    UnitSelected
};

struct WorldEvent {
    WorldEventType type = WorldEventType::UnitDamaged;
    int unitId = -1;
    AttackableType targetType = AttackableType::UNIT;
    int targetId = -1;
    float value = 0.f;
    Faction faction = Faction::A;
    Coord pos{};
};

struct WorldEventBus {
    using Handler = std::function<void(const WorldEvent&,
                                       WorldControlContext&,
                                       WorldRuntimeContext&)>;
    void subscribe(Handler handler);
    void publish(const WorldEvent& event);
    void drain(WorldControlContext& control, WorldRuntimeContext& runtime);

private:
    std::vector<WorldEvent> pending;
    std::vector<Handler> handlers;
};

struct WorldState {
    WorldState(int width, int height);
    Map map;
    std::shared_ptr<Base> baseA;
    std::shared_ptr<Base> baseB;
    std::vector<std::shared_ptr<Unit>> unitsA;
    std::vector<std::shared_ptr<Unit>> unitsB;
    std::vector<std::weak_ptr<IAttackable>> enemiesA;
    std::vector<std::weak_ptr<IAttackable>> enemiesB;
    std::vector<ForcedReveal> forcedVisibleForA;
    std::vector<ForcedReveal> forcedVisibleForB;
    std::vector<VisionIntent> lastVisionIntents;
    std::vector<TargetHint>   lastTargetHints;
    std::unordered_map<AttackableKey, float, AttackableKeyHash> lastIncomingDamageA;
    std::unordered_map<AttackableKey, float, AttackableKeyHash> lastIncomingDamageB;
    std::unordered_map<AttackableKey, int, AttackableKeyHash> lastLockedTargetsA;
    std::unordered_map<AttackableKey, int, AttackableKeyHash> lastLockedTargetsB;
    int nextUnitId = 1;
    int nextBaseId = 1;
};

struct WorldRuntime {
    struct UiEvent {
        std::optional<std::string> input;
        std::optional<std::string> feedback;
    };

    TaskPool taskPool;
    std::thread renderThread;
    std::atomic<bool> renderRunning{false};
    std::atomic<bool> paused{false};
    std::atomic<bool> gameEnded{false};
    std::atomic<long long> gameEndTimestampMs{0};
    std::atomic<bool> quitRequested{false};
    mutable std::shared_mutex worldMutex;
    std::mutex uiEventMutex;
    std::queue<UiEvent> uiEvents;
    WorldEventBus eventBus;

    WorldRuntime();
};

struct ControlState {
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
    bool                   awaitingProductionChoice = false;
    std::weak_ptr<Base>    productionChoiceBase;
    std::string            productionInputBuffer;
};

struct SystemsBundle {
    MovementSystem movement;
    VisionSystem vision;
    AttackSystem attack;
    CleanupSystem cleanup;
    BaseSystem base;
    std::unique_ptr<RenderSystem> render;
};

struct WorldDataContext {
    Map& map;
    std::shared_ptr<Base>& baseA;
    std::shared_ptr<Base>& baseB;
    std::vector<std::shared_ptr<Unit>>& unitsA;
    std::vector<std::shared_ptr<Unit>>& unitsB;
    std::vector<std::weak_ptr<IAttackable>>& enemiesA;
    std::vector<std::weak_ptr<IAttackable>>& enemiesB;
    std::vector<ForcedReveal>& forcedVisibleForA;
    std::vector<ForcedReveal>& forcedVisibleForB;
    std::vector<VisionIntent>& lastVisionIntents;
    std::vector<TargetHint>&   lastTargetHints;
    std::unordered_map<AttackableKey, float, AttackableKeyHash>& lastIncomingDamageA;
    std::unordered_map<AttackableKey, float, AttackableKeyHash>& lastIncomingDamageB;
    std::unordered_map<AttackableKey, int, AttackableKeyHash>& lastLockedTargetsA;
    std::unordered_map<AttackableKey, int, AttackableKeyHash>& lastLockedTargetsB;
    int& nextUnitId;
    int& nextBaseId;

    explicit WorldDataContext(WorldState& state);

    bool isTileFree(const Coord& c) const;
    void rebuildEnemies();
    std::shared_ptr<Unit> findUnit(int id) const;
    std::shared_ptr<Base> findBase(int id, Faction fac) const;
    std::shared_ptr<IAttackable> findAttackable(int id) const;
    int registerUnit(const std::shared_ptr<Unit>& u);
    int registerBase(const std::shared_ptr<Base>& b);
    void addForcedReveal(Faction viewer,
                         const std::shared_ptr<IAttackable>& target,
                         float durationSeconds);
    void decayForcedReveals(float dt);
    void appendForcedReveals(Faction viewer,
                             std::vector<std::weak_ptr<IAttackable>>& out) const;
    void revealAttacker(const Unit& u);
};

struct WorldRuntimeContext {
    TaskPool& taskPool;
    std::thread& renderThread;
    std::atomic<bool>& renderRunning;
    std::atomic<bool>& paused;
    std::atomic<bool>& gameEnded;
    std::atomic<long long>& gameEndTimestampMs;
    std::atomic<bool>& quitRequested;
    std::shared_mutex& worldMutex;
    std::mutex& uiEventMutex;
    std::queue<WorldRuntime::UiEvent>& uiEvents;
    WorldEventBus& eventBus;

    explicit WorldRuntimeContext(WorldRuntime& runtime);

    bool shouldQuit() const;
    void requestQuit();
    bool isRenderRunning() const;
    bool isPaused() const;
    void pause();
    void resume();
    void togglePause();
    void markGameOver();
    bool hasGameEnded() const;
    long long gameEndMs() const;

    void enqueueUiEvent(std::optional<std::string> input,
                        std::optional<std::string> feedback);
    void drainUiEvents(WorldControlContext& control);

    void start(WorldDataContext& data,
               WorldControlContext& control,
               SystemsBundle& systems);
    void stop();
    void join();
};

struct WorldControlContext {
    CommandQueue& commandQueue;
    std::string& lastCommandInput;
    std::string& lastCommandFeedback;
    std::vector<int>& selectedUnitIds;
    ControlState::ControlMode& controlMode;
    std::optional<ControlState::PendingTarget>& pendingTarget;
    bool& awaitingProductionChoice;
    std::weak_ptr<Base>& productionChoiceBase;
    std::string& productionInputBuffer;

    explicit WorldControlContext(ControlState& control);

    void enqueueCommand(const std::string& line);
    void processCommands(WorldDataContext& data, WorldRuntimeContext& runtime);
    void setSelection(const std::vector<int>& ids);
    void clearSelection();
    std::optional<UnitType> unitTypeFromCode(int code) const;
    void beginProductionChoice(const std::shared_ptr<Base>& base,
                               WorldRuntimeContext& runtime);
    bool handleProductionDigit(char digit, WorldRuntimeContext& runtime);
    bool handleProductionBackspace(WorldRuntimeContext& runtime);
    bool commitProductionChoice(WorldRuntimeContext& runtime);
    void cancelProductionChoice(WorldRuntimeContext& runtime);

    void resetTargeting();
    void enterTargeting();
    void cancelTargeting();
    void commitTargeting();
};

class GameWorld {
private:
    WorldState state;
    WorldRuntime runtime;
    ControlState control;
    SystemsBundle systems;
    TimeManager    timeManager;

    static void appendUnitSnapshot(
        const std::shared_ptr<Unit>& u,
        std::vector<AiScoring::UnitSnapshot>& out,
        std::unordered_set<std::uint64_t>* occ);
    static void buildUnitSnapshots(
        const std::vector<std::shared_ptr<Unit>>& units,
        std::vector<AiScoring::UnitSnapshot>& out,
        std::unordered_set<std::uint64_t>* occ);
    static void buildEnemySnapshots(
        const std::vector<std::shared_ptr<Unit>>& units,
        const std::shared_ptr<Base>& base,
        std::vector<AiScoring::AttackableSnapshot>& out);
    static void rebuildEnemyIndex(
        const std::vector<AiScoring::AttackableSnapshot>& snaps,
        std::unordered_map<AttackableKey, std::size_t, AttackableKeyHash>& index);
    static void buildForcedKeys(
        const std::vector<ForcedReveal>& forced,
        std::vector<AttackableKey>& out);

    
    friend class RenderSystem;
    friend class Game;

public:
    GameWorld();
    ~GameWorld();
    
    void update(float dt);
};
