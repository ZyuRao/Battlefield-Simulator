#include <iostream>
#include "core/map.hpp"

int main()
{
    int width = 80;
    int height = 40;

    MapGenerator gen(width, height);
    Map map = gen.generate();

    std::cout << "Generated Map:\n";
    map.print();

    return 0;
}
