#include "UserMenu.h"
#include "../main.h"
#include "Console.h"
#include <thread>
#include <vector>

// Forward declaration if StartDump isn't in main.h
void StartDump();

void UserMenu::RenderMenu()
{
    CGFloat screenWidth = ([UIApplication sharedApplication].windows[0].rootViewController.view.frame.size.width);
    CGFloat screenHeight = ([UIApplication sharedApplication].windows[0].rootViewController.view.frame.size.height);

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
        if (ImGui::Button("Start Dump"))
        {
            // Run in a detached thread to prevent freezing the UI
            std::thread([]{
                StartDump();
            }).detach();
        }
        ImGui::SameLine();
        if (ImGui::Button("Copy to Clipboard")) {
            ImGui::LogToClipboard();
        }
        
        ImGui::Separator();

        Console::Get().Render();
    }
    ImGui::End();
}
