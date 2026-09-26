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

    /* Oldest lines are dropped after this many, a full dump logs a lot and rendering every line each frame freezes the game */
    static constexpr size_t MaxLines = 5000;

    std::deque<Output> outputArr;
    std::mutex logMutex;
    bool autoScroll = true;
    bool visible = true;

    /* Number of lines ever added (outputArr stops growing at MaxLines), used to scroll to the newest line */
    size_t totalLines = 0;
    size_t renderedLines = 0;

    void log(const std::string& text);
    void logError(const std::string& text);
    void logInfo(const std::string& text); // Using for Success/Highlight
    void clearLogs();

    std::string GetAllText(); // Every stored line, for "Copy to Clipboard"
    
    void Render(); // Call this in your ImGui loop

private:
    void push(const std::string& text, int type);
};
