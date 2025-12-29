#include "Console.h"
#include <ctime>
#include <cstdio>

Console& Console::Get() {
    static Console instance;
    return instance;
}

// Helper for time string
static std::string GetTimeStr() {
    time_t now = time(0);
    struct tm tstruct;
    char buf[80];
    tstruct = *localtime(&now);
    strftime(buf, sizeof(buf), "%H:%M:%S", &tstruct);
    return std::string(buf);
}

void Console::log(const std::string& text) {
    std::lock_guard<std::mutex> lock(logMutex);
    outputArr.push_back({ text, GetTimeStr(), 0 });
}

void Console::logError(const std::string& text) {
    std::lock_guard<std::mutex> lock(logMutex);
    outputArr.push_back({ text, GetTimeStr(), 1 });
}

void Console::logInfo(const std::string& text) {
    std::lock_guard<std::mutex> lock(logMutex);
    outputArr.push_back({ text, GetTimeStr(), 2 });
}

void Console::clearLogs() {
    std::lock_guard<std::mutex> lock(logMutex);
    outputArr.clear();
}

void Console::Render() {
    // Log controls: Clear and Auto-scroll
    if (ImGui::Button("Clear")) clearLogs();
    ImGui::SameLine();
    // Assuming 'autoScroll' is a public member of Console
    ImGui::Checkbox("Auto-scroll", &autoScroll);
    ImGui::Separator();

    // Log display area (Scrolling Region)
    float footer_height_to_reserve = ImGui::GetStyle().ItemSpacing.y + ImGui::GetFrameHeightWithSpacing();
    // BeginChild creates the scrollable area, filling the remaining vertical space
    ImGui::BeginChild("ScrollingRegion", ImVec2(0, -footer_height_to_reserve), false, ImGuiWindowFlags_HorizontalScrollbar);

    {
        std::lock_guard<std::mutex> lock(logMutex);
        
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4, 1)); // Tighter spacing for log lines

        for (const auto& output : outputArr)
        {
            // Determine Color based on integer type (0=Log, 1=Error, 2=Success)
            ImVec4 color = ImVec4(1.0f, 1.0f, 1.0f, 1.0f); // Default White

            if (output.type == 1) { // Error
                color = ImVec4(1.0f, 0.4f, 0.4f, 1.0f); // Red
            } else if (output.type == 2) { // Success
                color = ImVec4(0.4f, 1.0f, 0.4f, 1.0f); // Green
            } else { // Log/Info (Type 0)
                color = ImVec4(0.4f, 0.8f, 1.0f, 1.0f); // Blue/Cyan
            }

            ImGui::PushStyleColor(ImGuiCol_Text, color);

            // Format: [Time] Text (incorporating drawTime logic from previous fixes)
            std::string line = output.text;
            if (!output.time.empty()) {
                line = "[" + output.time + "] " + line;
            }

            ImGui::TextUnformatted(line.c_str());
            ImGui::PopStyleColor();
        }
        
        ImGui::PopStyleVar();

        // Auto-scroll logic
        if (autoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
            ImGui::SetScrollHereY(1.0f);
    }
    
    ImGui::EndChild();
}
