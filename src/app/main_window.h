#pragma once

#include "controls.h"
#include "drop_target.h"
#include "unlocker/scanner.h"
#include "unlocker/unlock.h"

#include <windows.h>
#include <commctrl.h>

#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace unlocker::ui {

class MainWindow final : private DropHandler {
public:
    MainWindow() = default;
    ~MainWindow() override;
    MainWindow(const MainWindow&) = delete;
    MainWindow& operator=(const MainWindow&) = delete;

    bool create(HINSTANCE instance, int show_cmd);
    void open(std::vector<std::filesystem::path> targets);
    void open(const std::filesystem::path& target) { open(std::vector<std::filesystem::path>{target}); }
    // Kurz Ziele weiterer Instanzen annehmen (Mehrfachauswahl im klassischen Kontextmenü, siehe instance.h).
    void start_collecting();
    HWND hwnd() const noexcept { return hwnd_; }
    HACCEL accelerators() const noexcept { return accelerators_; }

private:
    enum class StatusKind { Neutral, Busy, Ok, Warning, Error };

    // Sichtbare Listenzeile: Sperre oder Gruppenkopf (Prozess mit mehreren Einträgen).
    struct Row {
        bool group = false;
        std::uint32_t pid = 0;
        std::size_t locker = 0;  // Index in result_->lockers (Gruppe: erster Eintrag)
        std::size_t count = 0;   // Gruppe: Anzahl Einträge
    };

    // Ziel-Chip im Header (bei mehreren Zielen).
    struct Chip {
        RECT rect{};
        RECT close{};
        std::size_t target = 0;
        bool overflow = false;   // "+n"-Chip ohne Schließen
    };

    static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
    LRESULT handle_message(UINT msg, WPARAM wparam, LPARAM lparam);

    // DropHandler
    void on_drag_over_changed(bool over) override;
    void on_files_dropped(std::vector<std::filesystem::path> paths) override;

    void on_create();
    void on_destroy();
    void on_paint();
    void on_erase_background(HDC hdc);
    void on_dpi_changed(UINT dpi, const RECT& suggested);
    void on_command(int id);
    LRESULT on_notify(const NMHDR& hdr);
    void on_scan_result(unsigned generation, std::unique_ptr<ScanResult> result, bool final);
    void on_timer(UINT_PTR id);
    bool on_copy_data(const COPYDATASTRUCT& data);
    void on_accent_changed();
    void on_mouse_move(POINT pt);
    bool on_left_click(POINT pt);
    LRESULT draw_row(NMLVCUSTOMDRAW& cd);
    void fill_row_tip(NMLVGETINFOTIPW& tip) const;
    void draw_chips(HDC hdc);

    void create_fonts();
    void create_image_list();
    void create_tooltip();
    void update_tooltip();
    void layout();
    void fit_last_column();
    int visual_rightmost_visible() const;
    void update_buttons();
    void start_scan();
    void set_busy(bool busy);
    void rebuild_rows();
    void fill_list();
    void start_reveal();
    float reveal_progress(int item) const;
    void toggle_group(std::uint32_t pid);
    void set_all_groups(bool collapsed);
    void set_group_collapsed(std::uint32_t pid, bool collapsed);
    bool group_initially_collapsed(std::uint32_t pid, std::size_t count);
    void sort_by(int column);
    void restore_selection(const std::vector<Locker>& keep);
    void sync_chip_tooltips();
    void set_tooltip_tool(UINT id, const RECT& rect, const std::wstring& text);
    int icon_for(const std::wstring& image_path);
    std::vector<Locker> selected_lockers() const;
    bool unlock_before_action(const wchar_t* instruction, const wchar_t* ok_text);
    void do_open_dialog();
    void do_unlock(UnlockMode mode);
    void do_delete();
    void do_delete_on_reboot();
    void do_rename();
    void do_move_or_copy(bool copy);
    void do_show_in_explorer();
    void do_more_menu();
    void do_row_menu(int item, POINT screen);
    void do_header_menu(POINT screen);
    void set_column_visible(int column, bool visible);
    void do_elevate();
    void do_toggle_shell_menu();
    void on_shell_toggled(bool enabled, long hr, const std::wstring& text);
    void remove_target(std::size_t index);
    void clear_targets(std::wstring status, StatusKind kind);
    void set_status(std::wstring text, StatusKind kind);
    void update_title();
    void invalidate_header();
    void invalidate_status();
    void apply_settings_columns();
    void save_settings() const;
    std::wstring targets_label() const;
    std::wstring quoted_targets() const;

    RECT title_rect() const;      // Mica-Zone
    RECT subtitle_rect() const;   // Pfad bzw. Chips (solide)
    RECT progress_rect() const;
    RECT card_rect() const;
    RECT status_rect() const;
    int dp(int value) const noexcept { return MulDiv(value, static_cast<int>(dpi_), 96); }

    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    HWND list_ = nullptr;
    HWND tooltip_ = nullptr;
    HWND btn_open_ = nullptr;
    HWND btn_more_ = nullptr;
    HWND btn_unlock_ = nullptr;
    HWND btn_kill_ = nullptr;
    HWND btn_delete_ = nullptr;
    HWND btn_elevate_ = nullptr;
    HACCEL accelerators_ = nullptr;
    HFONT font_title_ = nullptr;
    HFONT font_icon_large_ = nullptr;
    HFONT font_icon_small_ = nullptr;
    controls::Fonts fonts_;
    HIMAGELIST icons_ = nullptr;
    std::map<std::wstring, int> icon_cache_;
    DropTarget* drop_target_ = nullptr;

    UINT dpi_ = 96;
    bool mica_ = false;
    bool elevated_ = false;
    bool scanning_ = false;
    bool busy_ = false;
    bool drag_over_ = false;
    bool shell_busy_ = false;    // Kontextmenü wird gerade ein-/ausgeschaltet
    bool tracking_mouse_ = false;
    unsigned scan_generation_ = 0;
    unsigned progress_tick_ = 0;
    ULONGLONG reveal_start_ = 0;
    int sort_column_ = 1;        // PID
    bool sort_ascending_ = true;
    int hot_chip_ = -1;          // Chip unter der Maus (gesamte Fläche)
    bool hot_close_ = false;     // Maus liegt auf dem Schließen-Kreuz des Chips
    std::set<int> hidden_columns_;
    int column_width_[4]{};      // gemerkte Breite ausgeblendeter Spalten

    std::vector<std::filesystem::path> targets_;
    std::unique_ptr<ScanResult> result_;
    std::vector<Row> rows_;
    std::set<std::size_t> clipped_rows_;       // Zeilen (Index in rows_), deren Inhalt beim Zeichnen gekürzt wurde → Tooltip
    std::set<std::uint32_t> collapsed_;        // eingeklappte Gruppen dieser Sitzung (PID)
    std::set<std::uint32_t> seen_groups_;      // Gruppen, deren Startzustand schon bestimmt wurde
    std::set<std::wstring> collapsed_names_;   // persistent: Prozessnamen (klein), deren Gruppen eingeklappt starten
    std::vector<RECT> chip_tip_rects_;         // registrierte Tooltip-Tools der Chips (zur Änderungserkennung)
    std::size_t chip_tip_count_ = 0;
    std::vector<Chip> chips_;
    std::wstring status_;
    StatusKind status_kind_ = StatusKind::Neutral;
};

}  // namespace unlocker::ui
