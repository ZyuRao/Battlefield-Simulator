#pragma once
#include <iostream>
#include "gameworld.hpp"

class Game {
private:
    bool running = false;
    GameWorld world;

public:
    Game() : running(false) {}
    ~Game() {
        world.stopRenderThread();
    }

    void run() {
        running = true;
        std::cout << "[Main] GameProject started.\n";
        std::cout << "[Game] world created\n";

        world.startRenderThread();
        
        while(running) {
            world.update();

            std::this_thread::sleep_for(std::chrono::milliseconds(10));

            if(world.baseA->isDestroyed() || world.baseB->isDestroyed()) {
                running = false;
                std::string winner = world.baseA->isDestroyed() ? "A" : "B";
                std::cout << "Winner:" << winner << std::endl;
            }
        }

        world.stopRenderThread();
    }
};