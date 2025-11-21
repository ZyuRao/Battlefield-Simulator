# pragma once
#include <vector>
#include <string>
#include "gameworld.hpp"
#include "utils/vec2.hpp"
#include "map.hpp"

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
    AttrackUnit,
    Hold,
    Stop
};

enum class UnitType {
    Infantry, //步兵：中等移动、中等血量、近战
    Archer,   // 弓箭手：远程、血量低
    Knight    // 骑士：高血、高护甲、移动快
};


enum class Faction {
    A,
    B
};


struct UnitStats {
    float maxHP;
    float attack;
    float armor;
    float attackRange;
    float visionRange;
    float moveSpeed;

    static UnitStats getStats(UnitType t) {
        switch(t) {
        case UnitType::Infantry:
            return {120.f, 12.f, 2.f, 2.0f, 4.5f, 1.0f};
        case UnitType::Archer:
            return { 80.f, 20.f, 1.f, 6.0f, 7.0f, 1.0f};
        case UnitType::Knight:
            return {120.f, 18.f, 4.f, 2.0f, 5.0f, 1.6f};
        }
        return {100,10,2,1,4,1};
    }
};

class IAttackable {
public:
    virtual void takeDamage(float dmg) = 0;
    virtual bool isDestroyed() const = 0;
    virtual Coord getPos() const = 0;
    virtual ~IAttackable() = default;
};


class Base : public IAttackable {
private:
    Coord pos;
    Faction faction;
    float maxHp;
    float hp;

    bool destroyed = false;
    float productCd;
    float baseProductTime;

public:
    Base(const Coord& p, Faction f) : pos(p), faction(f), 
            productCd(0.f), baseProductTime(3.f) {}

    void update(float dt, GameWorld& world);

    Unit* produceUnit(UnitType type);

    Coord getPos() const override { return pos;}
    Faction getFaction() const { return faction; }

    void takeDamage(float dmg) override {
        hp -= dmg;
        if(hp <= 0 && !destroyed) destroyed = true;
    }
    bool isDestroyed() const override { return destroyed;}
    std::string getSymbol() const {
        return faction == Faction::A ? "A" : "B";
    }
};

class Unit : public IAttackable {
protected:
    UnitType type;
    Coord pos;
    float hp;

    UnitState state;
    UnitCommandType pendingCmd;
    IAttackable* target;
    Coord pendingMoveTarget;
    float attackCd;
    float moveAccumulator;

    std::vector<Coord> path;
    size_t pathIdx;

    UnitStats baseStats;
    Faction owner;

public:
    Unit(UnitType t, const Coord& basePos);

    void update(float dt,
                const Map& map, const std::vector<Unit*>& enemies);
    
    void issueMoveCommand(const Coord& dst);
    void issueAttackCommand(Unit* t);
    void issueStop();

    bool isAlive() const { return state != UnitState::Dead; }
    Coord getPos() const override { return pos; }
    UnitType getType() const { return type; }
    float getHP() const { return hp; }
    UnitState getState() const { return state; }

    std::string getSymbol() const {
        switch(type) {
            case UnitType::Infantry: return "I";
            case UnitType::Archer: return "A";
            case UnitType::Knight: return "K";
        }
        return "?";
    }

    void takeDamage(float dmg) override;
    bool isDestroyed() const override { return state == UnitState::Dead; }

private:
    void updateIdle(float dt,
                    const Map& map,
                    const std::vector<Unit*>& enemies);
    void updateMoving(float dt, const Map& map);
    void updateAttacking(float dt, const Map& map);
    void updateChasing(float dt, const Map& map);

    void computePath(const Coord& dst, const Map& map);
    void moveStep(float dt, const Map& map);

    Unit* findNearestEnemy(const Map& map, const std::vector<Unit*>& enemies) const;
    bool inAttackRange(Unit* e, const Map& map) const;
};