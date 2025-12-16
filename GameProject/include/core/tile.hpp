#pragma once
#include <string>

enum class TileType {
    PLAIN,
    FOREST,
    HILL,
    SWAMP,
    RIVER,
    MOUNTAIN
};

struct TileAttributes {
    bool passable;          // 是否可通行
    float moveCost;         // 移动代价
    int visionBonus;        // 视野增益
    int attackBonus;        // 攻击加成
};

class Tile {
private:
    TileType type;
    TileAttributes attr;    // 统一管理地形属性

public:
    Tile() : type(TileType::PLAIN), attr(makeAttributes(TileType::PLAIN)) {}
    Tile(TileType t) : type(t), attr(makeAttributes(t)) {}

    TileType getType() const { return type; }
    const TileAttributes& getAttr() const { return attr; }

    void setType(TileType t) { 
        type = t; 
        attr = makeAttributes(t);
    }

    bool isPassable() const { return attr.passable; }
    float getMoveCost() const { return attr.moveCost; }
    int getVisionBonus() const { return attr.visionBonus; }
    int getAttackBonus() const { return attr.attackBonus; }

    std::string getSymbol() const {
        switch(type) {
            case TileType::PLAIN:     return ".";
            case TileType::FOREST:    return "$";
            case TileType::HILL:      return "^";
            case TileType::SWAMP:     return "#";
            case TileType::RIVER:     return "~";
            case TileType::MOUNTAIN:  return "|";
        }
        return "?";
    }

    std::string getName() const {
        switch(type) {
            case TileType::PLAIN:     return "plain";
            case TileType::FOREST:    return "forest";
            case TileType::SWAMP:     return "swamp";
            case TileType::RIVER:     return "river";
            case TileType::MOUNTAIN:  return "mountain";
        }
        return "unknown";
    }

private:
    private:
    static TileAttributes makeAttributes(TileType t) {
        switch(t) {

        case TileType::PLAIN:
            return {true, 1.0f, 0, 0};

        case TileType::FOREST:
            return {true, 1.5f, -1, -1};   // 密林阻视野

        case TileType::HILL:
            return {true, 2.0f, +1, -1};

        case TileType::SWAMP:
            return {true, 2.5f, -3, -2};  // 移动极慢，攻击受限

        case TileType::RIVER:
            return {false, 0.0f, 0, 0};   // 默认不可走（需要桥）

        case TileType::MOUNTAIN:
            return {false, 0.0f, +2, +2};  // 可通行但代价最高，视野极佳

        }
        return {true, 1.0f, 0, 0};
    }

};


// ================== Design Notes ==================
// 模块功能：
// Tile 类用于描述游戏地图中的单个格子，包含地形类型、可通行性判断、字符渲染符号等。
// 该模块是地图系统(Map)的最基本构成单位。

// 设计模式：无特定设计模式。
// 但地形类型使用 enum class 优雅地区分类型，并方便未来扩展其他地形。

// 扩展接口：
// 1. 可轻松添加更多地形
// 2. 可为图形渲染加入纹理ID或颜色信息。
// 3. isPassable() 可扩展为“不同单位可通行不同地形”，例如空军可越过河流。
// 4. getSymbol() 可被图形渲染器替代为图像ID，不修改核心逻辑。
