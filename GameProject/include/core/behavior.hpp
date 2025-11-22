#pragma once 
#include "map.hpp"
#include "Iattackable.hpp"
#include "gameworld.hpp"
#include <algorithm>
#include <memory>


// 前置声明
class Unit;
class Base;
class GameWorld;

/*====================================
    Enum: UnitState / CommandType
=====================================*/
enum class UnitState {
    Idle,
    Moving,
    Attacking,
    Chasing,
    Dead
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
    virtual void issueAttack(IAttackable* t) = 0;
    virtual void issueStop() = 0;

    virtual UnitCommandType pendingType() const = 0;
    virtual Coord pendingMoveTarget() const = 0;
    virtual IAttackable* pendingAttackTarget() const = 0;

    virtual void clear() = 0;
};

class DefaultCommandBehavior : public ICommandBehavior {
private:
    UnitCommandType pending = UnitCommandType::None;
    Coord moveTarget{};
    IAttackable* attackTarget = nullptr;
public:
    void issueMove(const Coord& dst) override;
    void issueAttack(IAttackable* t) override;
    void issueStop() override;

    UnitCommandType pendingType() const override;
    Coord pendingMoveTarget() const override;
    IAttackable* pendingAttackTarget() const override;

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
};

class DefaultMovementBehavior : public IMovementBehavior {
private:
    std::vector<Coord> path;
    size_t idx = 0;
    float accumulator = 0.f;
    Coord lastChaseTarget;
    bool hasLastTarget;
public:
    void setMoveTarget(const Coord& dst, const Map& map, Unit& u) override;
    void update(Unit& u, float dt, const Map& map) override;
    Coord getLastTarget() const { return lastChaseTarget; }
    bool hasLastTarget() const { return hasLast; }
    void setLastTarget(const Coord& c) {
        lastChaseTarget = c;
        hasLast = true;
    }
};

/*====================================
    Interface: AttackBehavior
=====================================*/
class IAttackBehavior {
public:
    virtual ~IAttackBehavior() = default;
    virtual void setTarget(IAttackable* t) = 0;
    virtual IAttackable* getTarget() const = 0;
    virtual void update(Unit& u, float dt, const Map& map
                        const std::vector<IAttackable*>& visibleEnemies) = 0;
};

class DefaultAttackBehavior : public IAttackBehavior {
private:
    IAttackable* target = nullptr;
    float cd = 0.f;

    IAttackable* findNearest(const Unit& self,
                             const Map& map,
                             const std::vector<IAttackable*>& visibleEnemies) const;

    bool inAttackRange(const Unit& self,
                       const Map& map,
                       IAttackable* t) const;
public:
    IAttackable* getTarget() const override {return target};
    void setTarget(IAttackable* t) override;
    void update(Unit& u, float dt, const Map& map,
                const std::vector<IAttackable*>& visibleEnemies) override;
};

/*====================================
    Interface: VisionBehavior（未来扩展）
=====================================*/
class IVisionBehavior {
public:
    virtual ~IVisionBehavior() = default;
    virtual void updateVisible(Unit& self, 
                               const Map& map,
                               const std::vector<IAttackable*>& enemies) = 0;
    virtual const std::vector<IAttackable*>& getVisible() const = 0;
};

class DefaultVisionBehavior : public IVisionBehavior {
private:
    std::vector<IAttackable*> visible;
public:
    void updateVisible(Unit& self,
                       const Map& map,
                       const std::vector<IAttackable*>& allEnemies) override;
    const std::vector<IAttackable*>& getVisible() const override;
};

/*====================================
    UnitBehavior（大脑）
=====================================*/
class UnitBehavior {
private:
    std::unique_ptr<IMovementBehavior> movement;
    std::unique_ptr<IAttackBehavior> attack;
    std::unique_ptr<ICommandBehavior> command;
    std::unique_ptr<IVisionBehavior> vision;
    std::unique_ptr<IStateMachine> stateMachine;

public:
    UnitBehavior();

    void issueMove(const Coord& dst);
    void issueAttack(IAttackable* t);
    void issueStop();

    bool isDead() const;
    void onKilled(Unit& u);

    void update(Unit& u, float dt, const Map& map,
                const std::vector<IAttackable*>& enemies);
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
}

class BaseCommandBehavior {
private:
    std::queue<UnitType> pendingQueue;
public:
    void issueProduce(UnitType t);
    bool hasPending() const;
    UnitType nestPending() const;
    void pop() ;
    void clear();
};

class PeriodicProductionBehavior {
private:
    floar timer = 0.f;
    float period = 3.f;
public:
    void reset(float p);
    bool triggered(float dt);
}


class BaseSpawnBehavior {
private:
    UnitType current;
    float cd = 0.f;
public:
    void begin(UnitType t, Base& self);
    bool update(Base& self, float dt, GameWorld& world);
    Unittype type() const;

};

class BaseBehavior {
private:
    std::unique_ptr<BaseSpawnBehavior> spawn;
    std::unique_ptr<BaseCommandBehavior> command;
    std::unique_ptr<BaseStateMachine> stateMachine;
    std::unique_ptr<PeriodicProductionBehavior> periodic;

    void reqSpawn(Base& self, GameWorld& world, UnitType t);
public:
    BaseBehavior();
    void issueProduce(UnitType t);
    bool isDead() const;
    void onKilled(Base& self);

    void update(Base& self, float dt, GameWorld& world);
};


