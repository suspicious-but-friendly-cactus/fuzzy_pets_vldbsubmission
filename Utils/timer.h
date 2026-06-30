#include <chrono>

class Timer {
public:
    Timer(){
        start = std::chrono::high_resolution_clock::now();
    }
    ~Timer(){
        Stop();
    }
    double Stop(){
        std::chrono::time_point<std::chrono::high_resolution_clock> end = std::chrono::high_resolution_clock::now();

        auto startTime = std::chrono::time_point_cast<std::chrono::microseconds>(start).time_since_epoch().count();
        auto endTime = std::chrono::time_point_cast<std::chrono::microseconds>(end).time_since_epoch().count();

        auto duration = endTime - startTime;
        return duration;
    }
private:
    std::chrono::time_point<std::chrono::high_resolution_clock> start;
};