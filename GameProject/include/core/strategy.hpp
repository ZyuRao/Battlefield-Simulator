#pragma once 
#include "map.hpp"
#include "Iattackable.hpp"
#include "gameworld.hpp"
#include <algorithm>



class ICommandStrategy {
public:
    virtual ~ICommandStrategy() = default;

    virtual void issueMove(const Coord& dst) = 0;
    virtual void issueAttack(IAttackable* t) = 0;
    virtual void issueStop() = 0;

    virtual UnitCommandType getPendingType() const = 0;
    virtual Coord getPendingMoveTarget() const = 0;
    virtual IAttackable* getPendingAttackTarget() const = 0;

    virtual void clear() = 0;
};

class IMovementStrategy {
public:
    virtual ~IMovementStrategy() = default;

    virtual void setPath(const std::vector<Coord>& newPath) = 0;
    virtual void update(float dt, Unit& u, const Map& map) = 0;
};

class IAttackStrategy {
public:
    virtual ~IAttackStrategy() = default;

    virtual void setTarget(IAttackable* t) = 0;
    virtual void update(float dt, Unit& u, const Map& map) = 0;
};

class IVisionStrategy {
    virtual ~IVisionStrategy() = default;
    virtual void updateVision(Unit& u, const Map& map,
            std ::vector<IAttackable*>& visibleEnemies) = 0;
};

class ISpawnStrategy {
    virtual ~ISpawnStrategy() = default;
    virtual void updateSpawn(Base& base, float dt, GameWorld& world) = 0;
};

class IStateMachine {
public:
    virtual ~IStateMachine() = default;
    virtual UnitState get() const = 0;
    virtual void set(UnitState s) = 0;
};

class DefaultCommandStrategy : public ICommandStrategy {
private: 
    UnitCommandType pending = UnitCommandType::None;
    Coord pendingMove;
    IAttackable* pendingTarget;

public:
    void issueMove(const Coord& dst) override {
        pending = UnitCommandType::MoveTo;
        pendingMove = dst;
    }
    void issueAttack(IAttackable* t) override {
        pending = UnitCommandType::AttrackUnit;
        pendingTarget = t;
    }

    void issueStop() override {
        pending = UnitCommandType::Stop;
    }

    void clear() override {
        pending = UnitCommandType::None;
        pendingTarget = nullptr;
    }

    UnitCommandType getPendingType() const override { return pending; }
    Coord getPendingMoveTarget() const override { return pendingMove; }
    IAttackable* getPendingAttackTarget() const override { return pendingTarget; }

};

class DefaultStateMachine : public IStateMachine {
private:
    UnitState st = UnitState::Idle;
public:
    UnitState get() const override { return st; }
    void set(UnitState s) override { st = s; } 
};

class DefaultMovementStrategy : public IMovementStrategy {
private:
    std::vector<Coord> path;
    size_t idx = 0;
    float accumulator = 0.f;
    Coord lastTarget;

public:
    void setPath(const std::vector<Coord>& newPath) override {
        path = newPath;
        idx = 0;
        accumulator = 0;
    }

    void update(float dt, Unit& u, const Map& map) override;

private:
    void stepMove(float dt, Unit& u, const Map& map);
};


class DefaultAttackStrategy : public IAttackStrategy {
private:
    float cd = 0.f;
    IAttackable* target = nullptr;

public:
    void setTarget(IAttackable* t) override { target = t; }
    void update(float dt, Unit& u, const Map& map) override;
};


