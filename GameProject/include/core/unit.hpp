# pragma once
#include <vector>
#include <string>
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

class Unit {
protected:
    UnitType type;
    Coord pos;
    float hp;

    UnitState state;
    UnitCommandType pendingCmd;
    Unit* target;
    Coord pendingMoveTarget;
    float attackCd;
    float moveAccumulator;

    std::vector<Coord> path;
    size_t pathIdx;

    UnitStats baseStats;

public:
    Unit(UnitType t, const const Coord& basePos);

    void update(float dt,
                const Map& map, const std::vector<Unit*>& enemies);
    
    void issueMoveCommand(const Coord& dst);
    void issueAttackCommand(Unit* t);
    void issueStop();

    bool isAlive() const { return state != UnitState::Dead; }
    const Coord& getPos() const { return pos; }
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

    void takeDamage(float dmg);

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