#pragma once
#include <vector>
#include "gameworld.hpp"
#include "vec2.hpp"
#include "unit.hpp"

enum class Faction {
    A,
    B
};


class Base {
private:
    Coord pos;
    Faction faction;
    float maxHp;
    float hp;

    float productCd;
    float baseProductTime;

public:
    Base(const Coord& p, Faction f) : pos(p), faction(f), 
            productCd(0.f), baseProductTime(3.f) {}

    void update(float dt, GameWorld& world);

    Unit* produceUnit(UnitType type);

    Coord getPos() const { return pos;}
    Faction getFaction() const { return faction; }

    std::string getSymbol() const {
        return faction == Faction::A ? "A" : "B";
    }
};