#pragma once
#include <cmath>
#include <iostream>

struct Vec2 {
    int x = 0,y = 0;

    Vec2() = default;
    Vec2(int x_, int y_) : x(x_), y(y_) {}

    Vec2 operator+(const Vec2& other) const {
        return Vec2{x + other.x, y + other.y};
    }

    Vec2 operator-(const Vec2& other) const {
        return Vec2{x - other.x, y - other.y};
    }

    bool operator==(const Vec2& other) const {
        return x == other.x && y == other.y;
    }

    bool operator!=(const Vec2& other) const {
        return !(*this == other);
    }

    int mhtDistanceTo(const Vec2& other) const {
        return std::abs(x - other.x) + std::abs(y - other.y);
    }

    static int mhtDistance(const Vec2& a, const Vec2& b) {
        return a.mhtDistanceTo(b);
    }

    double distanceTo(const Vec2& other) const {
        int dx = x - other.x;
        int dy = y - other.y;
        return std::sqrt(dx*dx + dy*dy);
    }

    static int distance(const Vec2& a, const Vec2& b) {
        return a.distanceTo(b);
    }

    friend std::ostream& operator<<(std::ostream& os, const Vec2& v) {
        os << "(" << v.x << ", " << v.y << ")";
        return os;
    }

};

// ================== Design Notes ==================
// 模块功能：
// Vec2 是整个项目中的基础二维坐标结构，描述地图格子、单位位置、基地位置等。
// 该类型非常轻量，采用 struct + inline 成员函数，性能高、易复用。

// 采用的模式：
// 无特定设计模式，但遵循轻量值类型（Value Type）设计原则。
// 运算符重载使 Vec2 在地图、移动、AI 路径等模块中更易使用。

// 扩展接口：
// - 已预留 manhattanDistance 和 distanceTo 两种距离计算，未来可支持视野判断、射程判断。
// - 可扩展旋转、插值、方向向量、归一化等方法，用于更复杂的图形化或物理系统。
