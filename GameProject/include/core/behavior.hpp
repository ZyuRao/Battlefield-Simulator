#pragma once 
#include <vector>
#include <algorithm>
#include <random>
#include <memory>
#include <queue>
#include <array>
#include <cstddef>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include "utils/vec2.hpp"
#include "Iattackable.hpp"
#include "map.hpp"


// 前置声明
class IAttackable;
class Map;
class Unit;
class Base;
class GameWorld;
class BaseSystem;
enum class UnitType;
struct WorldDataContext;

/*====================================
    Enum: UnitState / CommandType
=====================================*/
enum class UnitState {
    Idle,
    Moving,
    Attacking,
    Chasing,
    Wandering,
    Dead
};

enum class LifeState {
    Alive,
    Dead
};

enum class LocomotionState {
    Idle,
    Pathing
};

enum class CombatState {
    None,
    Engaging
};

enum class MoveReason {
    None,
    Command,
    Wander,
    Chase,
    Kite,
    Retreat,
    Siege
};

enum class CombatAction {
    None,
    Attack,
    Chase,
    Kite,
    Retreat,
    Siege
};

enum class UnitCommandType {
    None,
    MoveTo,
    AttackUnit,
    Stop
};

enum class BaseState {
    Idle,
    Producing,
    Dead
};

/*====================================
    Interface: StateMachine
=====================================*/
class IStateMachine {
public:
    virtual ~IStateMachine() = default;
    virtual UnitState get() const = 0;
    virtual void set(UnitState s) = 0;
};

class DefaultStateMachine : public IStateMachine {
private:
    UnitState state = UnitState::Idle;
public:
    UnitState get() const override;
    void set(UnitState s) override;
};

/*====================================
    Interface: CommandBehavior
=====================================*/
class ICommandBehavior {
public:
    virtual ~ICommandBehavior() = default;

    virtual void issueMove(const Coord& dst) = 0;
    virtual void issueAttack(const std::shared_ptr<IAttackable>& t) = 0;
    virtual void issueStop() = 0;

    virtual UnitCommandType pendingType() const = 0;
    virtual Coord pendingMoveTarget() const = 0;
    virtual std::weak_ptr<IAttackable> pendingAttackTarget() const = 0;

    virtual void clear() = 0;
};

class DefaultCommandBehavior : public ICommandBehavior {
private:
    UnitCommandType pending = UnitCommandType::None;
    Coord moveTarget{};
    std::weak_ptr<IAttackable> attackTarget;
public:
    void issueMove(const Coord& dst) override;
    void issueAttack(const std::shared_ptr<IAttackable>& t) override;
    void issueStop() override;

    UnitCommandType pendingType() const override;
    Coord pendingMoveTarget() const override;
    std::weak_ptr<IAttackable> pendingAttackTarget() const override;

    void clear() override;
};

/*====================================
    Interface: MovementBehavior
=====================================*/
class IMovementBehavior {
public:
    virtual ~IMovementBehavior() = default;
    virtual void setMoveTarget(const Coord& dst, const Map& map, Unit& u) = 0;
    virtual void update(Unit& u, float dt, const Map& map) = 0;

    virtual std::vector<Coord>& usePath() = 0;
    virtual Coord getLastTarget() const = 0;
    virtual bool hasLastTarget() const = 0;
    virtual void setLastTarget(const Coord& c) = 0;

    struct MovementState {
        std::vector<Coord> path;
        std::size_t idx = 0;
        float accumulator = 0.f;
        Coord lastTarget{};
        bool hasLast = false;
    };

    virtual MovementState snapshot() const = 0;
    virtual void applyState(MovementState state) = 0;
};

struct AttackableKey {
    int id = -1;
    AttackableType type = AttackableType::UNIT;

    bool operator==(const AttackableKey& other) const {
        return id == other.id && type == other.type;
    }

    static AttackableKey from(const std::shared_ptr<IAttackable>& target);
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

struct AiTargetScoring {
    static float unitPreference(UnitType self, UnitType target);
    static float basePreference(UnitType self);
    static float threatScore(const AiScoring::AttackableSnapshot& target);
    static float distanceScore(float dist);
    static float ttkScore(const AiScoring::UnitSnapshot& self,
                          const AiScoring::AttackableSnapshot& target);
    static bool containsKey(const std::vector<const AiScoring::AttackableSnapshot*>& visible,
                            const AttackableKey& key);
    static std::vector<const AiScoring::AttackableSnapshot*> collectVisibleEnemies(
        const AiScoring::UnitSnapshot& unit,
        const Map& map,
        const std::vector<AiScoring::AttackableSnapshot>& enemies,
        const std::unordered_map<AttackableKey, std::size_t, AttackableKeyHash>& enemyIndex,
        const std::vector<AttackableKey>& forcedKeys);
    static float computeLocalThreat(const AiScoring::UnitSnapshot& self,
                                    const std::vector<const AiScoring::AttackableSnapshot*>& enemies,
                                    float radius);
    static float computeLocalThreatWorld(const Unit& self,
                                         const std::vector<std::shared_ptr<Unit>>& enemies,
                                         float radius);
    static int countNearbyAllies(const std::vector<AiScoring::UnitSnapshot>& allies,
                                 const Coord& pos,
                                 int radius);
    static int countNearbyEnemies(const std::vector<const AiScoring::AttackableSnapshot*>& enemies,
                                  const Coord& pos,
                                  int radius);
    static float baseOpportunityScore(const AiScoring::UnitSnapshot& self,
                                      const AiScoring::AttackableSnapshot& base,
                                      const std::vector<AiScoring::UnitSnapshot>& allies,
                                      const std::vector<const AiScoring::AttackableSnapshot*>& enemies,
                                      const AiScoring::TargetScoreParams& params,
                                      float localThreat);
    static float firingPositionAvailability(const AiScoring::UnitSnapshot& self,
                                            const AiScoring::AttackableSnapshot& target,
                                            const Map& map,
                                            const MapQuery::ReachableGrid& reachable,
                                            const std::unordered_set<std::uint64_t>& occ,
                                            int cap);
    static AiScoring::TargetChoice chooseTarget(
        const AiScoring::UnitSnapshot& self,
        const std::vector<const AiScoring::AttackableSnapshot*>& visible,
        const std::vector<AiScoring::UnitSnapshot>& allies,
        const std::unordered_map<AttackableKey, float, AttackableKeyHash>& incomingDamage,
        const std::unordered_map<AttackableKey, int, AttackableKeyHash>& lockedCounts,
        const AiScoring::TargetScoreParams& params,
        const Map& map,
        const MapQuery::ReachableGrid& reachable,
        const std::unordered_set<std::uint64_t>& occ);
    static float scoreTargetCandidate(
        const AiScoring::UnitSnapshot& self,
        const AiScoring::AttackableSnapshot& target,
        const std::vector<AiScoring::UnitSnapshot>& allies,
        const std::vector<const AiScoring::AttackableSnapshot*>& visible,
        const std::unordered_map<AttackableKey, float, AttackableKeyHash>& incomingDamage,
        const std::unordered_map<AttackableKey, int, AttackableKeyHash>& lockedCounts,
        const AiScoring::TargetScoreParams& params,
        const Map& map,
        const MapQuery::ReachableGrid& reachable,
        const std::unordered_set<std::uint64_t>& occ,
        float localThreat,
        float* baseOppOut);
};

struct CombatPlanner {
    static CombatAction decideCombatAction(const AiScoring::UnitSnapshot& self,
                                           const AiScoring::AttackableSnapshot* target,
                                           float dist,
                                           bool inRange,
                                           float cdAfter,
                                           float localThreat,
                                           float baseOpportunity,
                                           const AiScoring::ActionParams& params);
    static float computeAttackDamage(const AiScoring::UnitSnapshot& self, const Map& map);
    static float computeAttackCooldown(const AiScoring::UnitSnapshot& self, const Map& map);
    static AttackIntent planAttackIntent(
        const AiScoring::UnitSnapshot& unit,
        float dt,
        const Map& map,
        const std::vector<AiScoring::AttackableSnapshot>& enemies,
        const std::unordered_map<AttackableKey, std::size_t, AttackableKeyHash>& enemyIndex,
        const std::vector<AttackableKey>& forcedKeys,
        const std::unordered_map<AttackableKey, float, AttackableKeyHash>& incomingDamage,
        const std::unordered_map<AttackableKey, int, AttackableKeyHash>& lockedCounts,
        const std::vector<AiScoring::UnitSnapshot>& allies,
        const std::unordered_set<std::uint64_t>& occ);
};

struct MovementPlanner {
    static float movementSpend(const AiScoring::UnitSnapshot& unit, const Tile& tile);
    static MoveIntent planMoveIntent(
        const AiScoring::UnitSnapshot& unit,
        float dt,
        const Map& map,
        const std::vector<AiScoring::AttackableSnapshot>& enemies,
        const std::unordered_map<AttackableKey, std::size_t, AttackableKeyHash>& enemyIndex,
        const std::vector<AttackableKey>& forcedKeys,
        const std::unordered_map<AttackableKey, float, AttackableKeyHash>& incomingDamage,
        const std::unordered_map<AttackableKey, int, AttackableKeyHash>& lockedCounts,
        const std::vector<AiScoring::UnitSnapshot>& allies,
        bool baseAlive,
        const Coord& basePos,
        const std::unordered_set<std::uint64_t>& occ);

private:
    static float computeThreatAtCoord(const Coord& pos,
                                      const std::vector<AiScoring::AttackableSnapshot>& enemies,
                                      float radius);
    static float heatAt(const std::vector<float>& heat, int width, const Coord& c);
    static void addHeat(std::vector<float>& heat, int width, int height,
                        const Coord& c, float value);
    static std::vector<float> buildHeatMap(const Map& map,
                                           const std::vector<AiScoring::UnitSnapshot>& allies,
                                           const std::unordered_set<std::uint64_t>& occ,
                                           int selfId);
    static Coord applyAnchorSlot(const Map& map,
                                 const std::unordered_set<std::uint64_t>& occ,
                                 const Coord& anchor,
                                 int unitId,
                                 const Coord& selfPos);
    static Coord stepAlongManhattan(const Coord& from, const Coord& to, int steps);
    static void fillMoveCandidates(const AiScoring::UnitSnapshot& unit,
                                   const Map& map,
                                   const Coord& goal,
                                   const std::vector<float>& heat,
                                   const std::unordered_set<std::uint64_t>& occ,
                                   MoveIntent& intent);
    static bool selectAttackPosition(const AiScoring::UnitSnapshot& self,
                                     const AiScoring::AttackableSnapshot& target,
                                     const Map& map,
                                     const std::vector<AiScoring::AttackableSnapshot>& enemies,
                                     const MapQuery::ReachableGrid& reachable,
                                     const std::unordered_set<std::uint64_t>& occ,
                                     int minDist,
                                     int maxDist,
                                     Coord& outPos);
    static Coord chooseSafeSpot(const AiScoring::UnitSnapshot& self,
                                const Map& map,
                                const std::vector<AiScoring::AttackableSnapshot>& enemies,
                                int sampleRadius);
    static void rebuildPathState(const AiScoring::UnitSnapshot& unit,
                                 const Map& map,
                                 const Coord& target,
                                 IMovementBehavior::MovementState& state);
    static void rebuildPathStateHeat(const AiScoring::UnitSnapshot& unit,
                                     const Map& map,
                                     const Coord& target,
                                     const std::vector<float>& heat,
                                     IMovementBehavior::MovementState& state);
};

class DefaultMovementBehavior : public IMovementBehavior {
private:
    std::vector<Coord> path;
    size_t idx = 0;
    float accumulator = 0.f;
    Coord lastTarget;
    bool hasLast;
public:
    void setMoveTarget(const Coord& dst, const Map& map, Unit& u) override;
    void update(Unit& u, float dt, const Map& map) override;
    Coord getLastTarget() const override {return lastTarget; }
    bool hasLastTarget() const override { return hasLast; }
    void setLastTarget(const Coord& c) override {
        lastTarget = c;
        hasLast = true;
    }
    std::vector<Coord>& usePath() { return path; }
    MovementState snapshot() const override;
    void applyState(MovementState state) override;
};

/*====================================
    Interface: AttackBehavior
=====================================*/
class IAttackBehavior {
public:
    virtual ~IAttackBehavior() = default;
    virtual void setTarget(const std::weak_ptr<IAttackable>& t) = 0;
    virtual std::weak_ptr<IAttackable> getTarget() const = 0;
    virtual float getCooldown() const = 0;
    virtual void setCooldown(float value) = 0;
    virtual void update(Unit& u, float dt, const Map& map, 
                        const std::vector<std::shared_ptr<IAttackable>>& visibleEnemies,
                        WorldDataContext& data) = 0;
    virtual std::shared_ptr<IAttackable> findNearest(const Unit& self,
                             const Map& map,
                             const std::vector<std::shared_ptr<IAttackable>>& visibleEnemies) const = 0;

    virtual bool inAttackRange(const Unit& self,
                       const Map& map,
                       const std::shared_ptr<IAttackable>& t) const = 0;
};

class DefaultAttackBehavior : public IAttackBehavior {
private:
    std::weak_ptr<IAttackable> target;
    float cd = 0.f;
  
public:
    std::shared_ptr<IAttackable> findNearest(const Unit& self,
                             const Map& map,
                             const std::vector<std::shared_ptr<IAttackable>>& visibleEnemies) const;

    bool inAttackRange(const Unit& self,
                       const Map& map,
                       const std::shared_ptr<IAttackable>& t) const;
    std::weak_ptr<IAttackable> getTarget() const override {return target; }
    float getCooldown() const override { return cd; }
    void setCooldown(float value) override { cd = value; }
    void setTarget(const std::weak_ptr<IAttackable>& t) override;
    void update(Unit& u, float dt, const Map& map,
                const std::vector<std::shared_ptr<IAttackable>>& visibleEnemies,
                WorldDataContext& data) override;
};

/*====================================
    Interface: VisionBehavior（未来扩展）
=====================================*/
class IVisionBehavior {
public:
    virtual ~IVisionBehavior() = default;
    virtual void updateVisible(Unit& self, 
                               const Map& map,
                               const std::vector<std::weak_ptr<IAttackable>>& enemies,
                               const std::vector<std::weak_ptr<IAttackable>>& forcedVisible) = 0;
    virtual const std::vector<std::shared_ptr<IAttackable>>& getVisible() const = 0;
};

class DefaultVisionBehavior : public IVisionBehavior {
private:
    std::vector<std::shared_ptr<IAttackable>> visible;
public:
    void updateVisible(Unit& self,
                       const Map& map,
                       const std::vector<std::weak_ptr<IAttackable>>& allEnemies,
                       const std::vector<std::weak_ptr<IAttackable>>& forcedVisible) override;
    const std::vector<std::shared_ptr<IAttackable>>& getVisible() const override;
};

/* ====================================
    UnitBehavior（大脑）
===================================== */
class UnitBehavior {
private:
    float idleAccum = 0.0f;
    std::mt19937 rng;
    std::unique_ptr<IMovementBehavior> movement;
    std::unique_ptr<IAttackBehavior> attack;
    std::unique_ptr<ICommandBehavior> command;
    std::unique_ptr<IVisionBehavior> vision;
    std::unique_ptr<IStateMachine> stateMachine;
    bool commandMoveActive = false;
    bool commandAttackActive = false;
    int commandAttackTargetId = -1;
    AttackableType commandAttackTargetType = AttackableType::UNIT;
    LifeState lifeState = LifeState::Alive;
    LocomotionState locomotionState = LocomotionState::Idle;
    CombatState combatState = CombatState::None;
    MoveReason moveReason = MoveReason::None;
    CombatAction combatAction = CombatAction::None;
    float commitTimer = 0.f;
    int commitTargetId = -1;
    AttackableType commitTargetType = AttackableType::UNIT;
    bool retreating = false;
    float retreatTimer = 0.f;
    Coord retreatAnchor{};
    bool hasRetreatAnchor = false;
    float timeSinceDamaged = 0.f;
    float timeSinceDealtDamage = 0.f;

public:
    UnitBehavior();

    void issueMove(const Coord& dst);
    void issueAttack(const std::shared_ptr<IAttackable>& t);
    void issueStop();

    bool isDead() const;
    void onKilled(Unit& u);

    void tickVision(Unit& u, const Map& map, 
                    const std::vector<std::weak_ptr<IAttackable>>& enenmies,
                    const std::vector<std::weak_ptr<IAttackable>>& forcedVisible);
    void tickMovement(Unit& u, float dt, const Map& map);
    void tickAttack(Unit& u, float dt, const Map& map,
        const std::vector<std::weak_ptr<IAttackable>>& visibleEnemies,
        WorldDataContext& data);
    UnitState getState() const;
    void setState(UnitState state);
    void applyPendingCommand(Unit& u, const Map& map);
    void updateVision(Unit& u, const Map& map,
                      const std::vector<std::weak_ptr<IAttackable>>& enemies,
                      const std::vector<std::weak_ptr<IAttackable>>& forcedVisible);
    void postAttackStateUpdate(Unit& u, const Map& map,
                               const std::vector<std::weak_ptr<IAttackable>>& enemies);
    IAttackBehavior* getAttackBehavior() { return attack.get(); }
    const IAttackBehavior* getAttackBehavior() const { return attack.get(); }
    IMovementBehavior* getMovementBehavior() { return movement.get(); }
    const IMovementBehavior* getMovementBehavior() const { return movement.get(); }
    bool isCommandMoveActive() const { return commandMoveActive; }
    void setCommandMoveActive(bool value) { commandMoveActive = value; }

    friend class GameWorld;
};



class BaseStateMachine {
public:
    virtual ~BaseStateMachine() = default;
    virtual BaseState get() const = 0;
    virtual void set(BaseState s) = 0;
};

class DefaultBaseStateMachine : public BaseStateMachine {
private:
    BaseState state = BaseState::Idle;
public:
    BaseState get() const override;
    void set(BaseState s) override;
};

class BaseCommandBehavior {
private:
    std::queue<UnitType> pendingQueue;
public:
    void issueProduce(UnitType t);
    bool hasPending() const;
    UnitType nextPending() const;
    void pop() ;
    void clear();
};

class PeriodicProductionBehavior {
private:
    float timer = 0.f;
    float period = 3.f;

    std::array<UnitType, 3> cycle {
        UnitType::Infantry,
        UnitType::Archer,
        UnitType::Knight
    };
    std::size_t idx = 0;
public:
    void reset(float p);
    bool triggered(float dt);

    UnitType nextType();
};


class BaseSpawnBehavior {
private:
    UnitType currentType;
    float cd = 0.f;
public:
    void begin(UnitType t, Base& self);
    bool update(Base& self, float dt, WorldDataContext& data);
    UnitType type() const;

};

class BaseBehavior {
private:
    std::unique_ptr<BaseSpawnBehavior> spawn;
    std::unique_ptr<BaseCommandBehavior> command;
    std::unique_ptr<BaseStateMachine> stateMachine;
    std::unique_ptr<PeriodicProductionBehavior> periodic;

    void reqSpawn(Base& self, WorldDataContext& data,
                  const BaseSystem& baseSystem,
                  UnitType t);
public:
    BaseBehavior();
    void issueProduce(UnitType t);
    bool isDead() const;
    void onKilled(Base& self);

    void update(Base& self, float dt, WorldDataContext& data,
                const BaseSystem& baseSystem);
};
