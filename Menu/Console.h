#pragma once
#include <vector>
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

    std::vector<Output> outputArr;
    std::mutex logMutex;
    bool autoScroll = true;
    bool visible = true;

    void log(const std::string& text);
    void logError(const std::string& text);
    void logInfo(const std::string& text); // Using for Success/Highlight
    void clearLogs();
    
    void Render(); // Call this in your ImGui loop
};
