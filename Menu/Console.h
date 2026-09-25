#pragma once
#include <deque>
#include <string>
#include <mutex>
#include "../ImGui/imgui.h"

// Define Log Types
struct Output {
    std::string text;
    std::string time;
    int type; // 0=Log, 1=Error, 2=Success
};

class Console {
public:
    static Console& Get(); // Singleton accessor

    /* Oldest lines are dropped beyond this, rendering an unbounded log every frame stalls the game's main thread. */
    static constexpr size_t MaxLines = 20000;

    std::deque<Output> outputArr;
    std::mutex logMutex;
    bool autoScroll = true;
    bool visible = true;

    void log(const std::string& text);
    void logError(const std::string& text);
    void logInfo(const std::string& text); // Using for Success/Highlight
    void clearLogs();

    std::string GetAllText();

    void Render(); // Call this in your ImGui loop

private:
    void push(const std::string& text, int type);
};
