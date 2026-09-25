#include "UserMenu.h"
#include "../main.h"
#include "Console.h"
#include <thread>
#include <vector>

void UserMenu::RenderMenu()
{
    UIView* const MainView = GetMainView();
    const CGRect ScreenBounds = MainView ? MainView.frame : [[UIScreen mainScreen] bounds];

    CGFloat screenWidth = ScreenBounds.size.width;

    CGFloat windowWidth = 500;
    CGFloat windowHeight = 350;
    CGFloat xPos = screenWidth - windowWidth - 10.0;
    CGFloat yPos = 10.0;

    ImGui::SetNextWindowPos(ImVec2(xPos, yPos), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(windowWidth, windowHeight), ImGuiCond_FirstUseEver);
    
    ImGui::StyleColorsClassic();

    if (ImGui::Begin("Dumper Console", NULL, ImGuiWindowFlags_NoCollapse))
    {
        const bool bIsDumping = IsDumpRunning();

        // --- Top Bar (Buttons) ---
        if (bIsDumping)
            ImGui::BeginDisabled();

        if (ImGui::Button(bIsDumping ? "Dumping..." : "Start Dump"))
        {
            // Run in a detached thread to prevent freezing the UI. StartDump() refuses to run twice at the same time.
            std::thread([]{
                StartDump();
            }).detach();
        }

        if (bIsDumping)
            ImGui::EndDisabled();

        ImGui::SameLine();
        if (ImGui::Button("Copy to Clipboard")) {
            const std::string AllText = Console::Get().GetAllText();
            [UIPasteboard generalPasteboard].string = [NSString stringWithUTF8String:AllText.c_str()] ?: @"";
        }
        
        ImGui::Separator();

        Console::Get().Render();
    }
    ImGui::End();
}
