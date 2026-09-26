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

void Console::push(const std::string& text, int type) {
    const std::string time = GetTimeStr();

    std::lock_guard<std::mutex> lock(logMutex);

    /*
    * One entry per line: the list clipper in Render() expects every entry to be one line high. Messages like "GameName: X\n" or
    * "\n\nGenerating SDK took ..." were drawn taller than that, so the end of the log was cut off and the newest lines were hidden.
    */
    size_t start = 0;
    while (start <= text.size())
    {
        size_t end = text.find('\n', start);
        if (end == std::string::npos)
            end = text.size();

        std::string line = text.substr(start, end - start);
        if (!line.empty() && line.back() == '\r')
            line.pop_back();

        if (!line.empty())
        {
            outputArr.push_back({ std::move(line), time, type });
            totalLines++;
        }

        start = end + 1;
    }

    while (outputArr.size() > MaxLines)
        outputArr.pop_front();
}

void Console::log(const std::string& text) {
    push(text, 0);
}

void Console::logError(const std::string& text) {
    push(text, 1);
}

void Console::logInfo(const std::string& text) {
    push(text, 2);
}

std::string Console::GetAllText() {
    std::lock_guard<std::mutex> lock(logMutex);

    std::string result;
    for (const auto& output : outputArr)
        result += "[" + output.time + "] " + output.text + "\n";

    return result;
}

void Console::clearLogs() {
    std::lock_guard<std::mutex> lock(logMutex);
    outputArr.clear();
    autoScroll = true;
}

void Console::Render() {
    // Log controls: Clear and Auto-scroll
    if (ImGui::Button("Clear")) clearLogs();
    ImGui::SameLine();
    ImGui::Checkbox("Auto-scroll", &autoScroll);
    ImGui::Separator();

    // Log display area, fills the rest of the window (it used to reserve room for a footer that doesn't exist)
    ImGui::BeginChild("ScrollingRegion", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar);

    {
        std::lock_guard<std::mutex> lock(logMutex);
        
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4, 1)); // Tighter spacing for log lines

        /* Only submit the visible lines */
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(outputArr.size()));

        while (clipper.Step())
        {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++)
            {
                const Output& output = outputArr[i];

                // Determine Color based on integer type (0=Log, 1=Error, 2=Success)
                ImVec4 color = ImVec4(0.4f, 0.8f, 1.0f, 1.0f); // Log/Info (Type 0): Blue/Cyan

                if (output.type == 1) { // Error
                    color = ImVec4(1.0f, 0.4f, 0.4f, 1.0f); // Red
                } else if (output.type == 2) { // Success
                    color = ImVec4(0.4f, 1.0f, 0.4f, 1.0f); // Green
                }

                ImGui::PushStyleColor(ImGuiCol_Text, color);

                const std::string line = output.time.empty() ? output.text : "[" + output.time + "] " + output.text;
                ImGui::TextUnformatted(line.c_str());

                ImGui::PopStyleColor();
            }
        }

        ImGui::PopStyleVar();

        /* Dragging the log (or its scrollbar) up pauses auto-scroll, dragging it back to the end resumes it */
        const bool bAtBottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - ImGui::GetTextLineHeightWithSpacing();
        const bool bUserScrolls = ImGui::IsWindowHovered() && (ImGui::IsMouseDragging(ImGuiMouseButton_Left) || ImGui::GetIO().MouseWheel != 0.0f);

        if (bUserScrolls)
            autoScroll = bAtBottom;

        /* The cursor is behind the last line here: keep the newest line at the bottom of the view whenever lines were added */
        if (autoScroll && (renderedLines != totalLines || !bAtBottom))
            ImGui::SetScrollHereY(1.0f);

        renderedLines = totalLines;
    }
    
    ImGui::EndChild();
}
