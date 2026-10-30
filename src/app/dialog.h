#pragma once

#include "controls.h"
#include "strings.h"

#include <windows.h>

#include <string>

// Modaler Bestätigungs-/Hinweisdialog im App-Stil. Ersetzt TaskDialog, der den Dark Mode ignoriert.
namespace unlocker::ui::dialog {

enum class Icon { Info, Warning, Error };

struct Options {
    std::wstring instruction;          // Überschrift
    std::wstring content;              // mehrzeiliger Text
    std::wstring ok_text = i18n::tr(i18n::S::BtnOk);
    std::wstring cancel_text;          // leer = nur OK-Button
    Icon icon = Icon::Info;
    bool default_cancel = true;        // Enter/Fokus auf Abbrechen (sicherer Standard bei destruktiven Aktionen)
    bool has_input = false;            // einzeiliges Eingabefeld unter dem Text
    std::wstring input;                // Vorbelegung; nach OK der eingegebene Text
    std::wstring input_select;         // optional: dieser Teil der Vorbelegung wird selektiert (z. B. Name ohne Endung)
};

// true, wenn OK gewählt wurde. options.input enthält danach die Eingabe.
bool show(HWND owner, Options& options, const controls::Fonts& fonts);

inline bool prompt(HWND owner, std::wstring instruction, std::wstring content, std::wstring ok_text,
                   std::wstring& value, std::wstring select_part, const controls::Fonts& fonts) {
    Options o;
    o.instruction = std::move(instruction);
    o.content = std::move(content);
    o.ok_text = std::move(ok_text);
    o.cancel_text = i18n::tr(i18n::S::BtnCancel);
    o.icon = Icon::Info;
    o.default_cancel = false;
    o.has_input = true;
    o.input = value;
    o.input_select = std::move(select_part);
    if (!show(owner, o, fonts)) return false;
    value = o.input;
    return true;
}

inline bool confirm(HWND owner, std::wstring instruction, std::wstring content, std::wstring ok_text,
                    const controls::Fonts& fonts, Icon icon = Icon::Warning) {
    Options o;
    o.instruction = std::move(instruction);
    o.content = std::move(content);
    o.ok_text = std::move(ok_text);
    o.cancel_text = i18n::tr(i18n::S::BtnCancel);
    o.icon = icon;
    return show(owner, o, fonts);
}

inline void inform(HWND owner, std::wstring instruction, std::wstring content, const controls::Fonts& fonts,
                   Icon icon = Icon::Info) {
    Options o;
    o.instruction = std::move(instruction);
    o.content = std::move(content);
    o.icon = icon;
    o.default_cancel = false;
    show(owner, o, fonts);
}

}  // namespace unlocker::ui::dialog
