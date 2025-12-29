#include "fonts.h"
#include "imgui.h"

void InitFonts() {
    ImGuiIO& io = ImGui::GetIO();
    
    // Khởi tạo font Bold
    ImFontConfig config;
    config.SizePixels = 20.0f;
    config.OversampleH = 2;
    config.OversampleV = 1;
    config.PixelSnapH = true;
    Bold = io.Fonts->AddFontDefault(&config);
    
    // Khởi tạo font combo_arrow
    config.SizePixels = 15.0f;
    combo_arrow = io.Fonts->AddFontDefault(&config);
    
    // Khởi tạo font tab_icons
    config.SizePixels = 15.0f;
    tab_icons = io.Fonts->AddFontDefault(&config);
    
    // Build font atlas
    io.Fonts->Build();
} 