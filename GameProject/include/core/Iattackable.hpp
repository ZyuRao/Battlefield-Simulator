# pragma once
#include <vector>
#include <string>
#include <memory>
#include "utils/vec2.hpp"

class BaseBehavior;
class UnitBehavior;
class GameWorld;  
class Map;


enum class UnitType {
    Infantry, //步兵：中等移动、中等血量、近战
    Archer,   // 弓箭手：远程、血量低
    Knight    // 骑士：高血、高护甲、移动快
};


enum class Faction {
    A,
    B
};

enum AttackableType {
    UNIT,
    BASE
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
    virtual AttackableType getAttackType() const = 0;
    virtual ~IAttackable() = default;
    virtual Faction getFaction() const = 0;
};


class Base : public IAttackable {
private:
    Coord pos;
    Faction faction;
    float maxHp = 500.f;
    float hp = maxHp;


    std::unique_ptr<BaseBehavior> behavior;

    friend class RenderSystem;
public:
    float baseProductTime = 2.f;

    Base(const Coord& p, Faction f);
    void update(float dt, GameWorld& world);


    Coord getPos() const override { return pos; }
    AttackableType getAttackType() const override {
        return AttackableType::BASE;
    }
    Faction getFaction() const { return faction; }

    std::string getSymbol() const {
        return faction == Faction::A ? "A" : "B";
    }

    void takeDamage(float dmg) override;
    bool isDestroyed() const override;
};

class Unit : public IAttackable {
public: 
    UnitType type;
    Coord pos;
    float hp;

    UnitStats baseStats;
    Faction owner;

    std::unique_ptr<UnitBehavior> behavior;
    //行为策略组合

    friend class RenderSystem;
public:
    Unit(UnitType t, const Coord& start, Faction faction);

    Coord getPos() const override { return pos; }

    AttackableType getAttackType() const override {
        return AttackableType::UNIT;
    }

    Faction getFaction() const { return owner; }
    void takeDamage(float dmg) override;

    bool isDestroyed() const override;

    bool isAlive() const;

    std::string getSymbol() const {
        switch (type) {
            case UnitType::Infantry: return "I";
            case UnitType::Archer:   return "A";
            case UnitType::Knight:   return "K";
        }
        return "?";
    }
    
};