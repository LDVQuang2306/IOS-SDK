#include "UserMenu.h"
#include "../main.h"
#include "Console.h"
#include <thread>
#include <vector>


void UserMenu::RenderMenu()
{
    // windows[0] can be missing while the game swaps windows, the screen bounds are always valid
    CGFloat screenWidth = [UIScreen mainScreen].bounds.size.width;

    CGFloat windowWidth = 500;
    CGFloat windowHeight = 350;
    CGFloat xPos = screenWidth - windowWidth - 10.0;
    CGFloat yPos = 10.0;

    ImGui::SetNextWindowPos(ImVec2(xPos, yPos), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(windowWidth, windowHeight), ImGuiCond_FirstUseEver);
    
    ImGui::StyleColorsClassic();

    if (ImGui::Begin("Dumper Console", NULL, ImGuiWindowFlags_NoCollapse))
    {
        // --- Top Bar (Buttons) ---
        if (ImGui::Button(IsDumpRunning() ? "Dumping...###StartDump" : "Start Dump###StartDump"))
        {
            // Non-blocking, the dump runs on its own large-stack thread. Repeated taps are ignored while it runs.
            StartDump();
        }
        ImGui::SameLine();
        if (ImGui::Button("Copy to Clipboard")) {
            // ImGui's clipboard isn't the iOS pasteboard, and the log window only renders visible lines
            const std::string allText = Console::Get().GetAllText();
            [UIPasteboard generalPasteboard].string = [NSString stringWithUTF8String:allText.c_str()] ?: @"";
        }
        
        ImGui::Separator();

        Console::Get().Render();
    }
    ImGui::End();
}
