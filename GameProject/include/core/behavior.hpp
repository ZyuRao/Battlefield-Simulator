#pragma once 
#include <vector>
#include <algorithm>
#include <random>
#include <memory>
#include <queue>
#include <array>
#include <cstddef>
#include "utils/vec2.hpp"
#include "Iattackable.hpp"


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
