#include "ZeldaOnlineClient.hpp"
#include "ZeldaOnlineRoomWindow.hpp"
#include "soh/SohGui/SohGui.hpp"
#include "soh/SohGui/SohMenu.h"
#include "soh/util.h"

namespace SohGui {
extern std::shared_ptr<SohMenu> mSohMenu;
extern std::shared_ptr<ZeldaOnline::ZeldaOnlineRoomWindow> mZeldaOnlineRoomWindow;
} // namespace SohGui

static void ShowZeldaOnlineWindow() {
    if (SohGui::mZeldaOnlineRoomWindow == nullptr)
        return;

    SohGui::mZeldaOnlineRoomWindow->Show();
    SohGui::mZeldaOnlineRoomWindow->CenterAndExpand();
}

void ZeldaOnlineMainMenu(WidgetInfo& info) {
    bool isEnabled = CVarGetInteger("gZeldaOnline.Enabled", 1) != 0;

    ImGui::SeparatorText("Zelda Online");

    const char* buttonLabel = isEnabled ? "Disable" : "Enable";
    UIWidgets::PushStyleButton(isEnabled ? UIWidgets::ColorValues.at(UIWidgets::Colors::Red)
                                         : UIWidgets::ColorValues.at(UIWidgets::Colors::Green));

    if (ImGui::Button(buttonLabel, ImVec2(-1.0f, 0.0f))) {
        if (isEnabled) {
            CVarSetInteger("gZeldaOnline.Enabled", 0);

            if (SohGui::mZeldaOnlineRoomWindow != nullptr)
                SohGui::mZeldaOnlineRoomWindow->Hide();

            if (ZeldaOnline::ZeldaOnlineClient::Instance != nullptr)
                ZeldaOnline::ZeldaOnlineClient::Instance->Disable();
        } else {
            CVarSetInteger("gZeldaOnline.Enabled", 1);
            ShowZeldaOnlineWindow();
            if (ZeldaOnline::ZeldaOnlineClient::Instance != nullptr)
                ZeldaOnline::ZeldaOnlineClient::Instance->Enable();
        }

        Ship::Context::GetRawInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
    }

    UIWidgets::PopStyleButton();

    if (!isEnabled)
        return;

    ImGui::Spacing();

    UIWidgets::PushStyleButton(UIWidgets::ColorValues.at(UIWidgets::Colors::Blue));

    if (ImGui::Button("Open Window", ImVec2(-1.0f, 0.0f)))
        ShowZeldaOnlineWindow();

    UIWidgets::PopStyleButton();

    ImGui::Spacing();

    // See LiveMenus.h
    UIWidgets::CVarCheckbox("Keep the game running in menus", "gZeldaOnline.LiveMenus",
                            UIWidgets::CheckboxOptions().DefaultValue(true).Tooltip(
                                "While online, the pause menu and the ocarina don't stop the game: enemies and other "
                                "players keep moving, Dark Souls style. Link stands still while the menu is open, and "
                                "the menu closes by itself on death, scene changes, cutscenes and textboxes. Off "
                                "pauses as in the original game. Takes effect the next time a menu opens."));
}

void RegisterZeldaOnlineMenu() {
    WidgetPath path = { "Network", "Zelda Online", SECTION_COLUMN_1 };
    SohGui::mSohMenu->AddWidget(path, "ZeldaOnlineMainMenu", WIDGET_CUSTOM)
        .CustomFunction(ZeldaOnlineMainMenu)
        .HideInSearch(true);
}

static RegisterMenuInitFunc menuInitFunc(RegisterZeldaOnlineMenu);