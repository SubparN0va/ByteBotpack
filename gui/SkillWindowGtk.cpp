#include "SkillControl.h"
#include "SkillPersistence.h"
#include <gtk/gtk.h>
#include <algorithm>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <vector>

// GTK widgets and events stay on the main thread. RLBot's network thread only
// updates SkillControl under its mutex; the panel takes snapshots when refreshing.
// https://docs.gtk.org/gtk3/func.init_check.html
struct SkillWindow::Impl {
    struct RowWidgets {
        Impl* owner;
        unsigned slot;
        GtkWidget *box, *caption, *rank, *minus, *plus;
    };
    std::shared_ptr<SkillControl> control;
    bool standalone, updating = false;
    GtkWidget *window = nullptr, *subtitle, *roster, *apply, *debug, *footer, *details, *debugScroll;
    std::vector<std::unique_ptr<RowWidgets>> widgets;
    std::string lastDebug;
    std::chrono::steady_clock::time_point refreshed{};

    static GtkWidget* Label(const char* text) {
        auto label = gtk_label_new(text);
        gtk_label_set_xalign(GTK_LABEL(label), 0.f);
        return label;
    }
    static void Pack(GtkWidget* parent, GtkWidget* child, bool expand = false) {
        gtk_box_pack_start(GTK_BOX(parent), child, expand, expand, 0);
    }
    explicit Impl(std::shared_ptr<SkillControl> shared, bool preview)
        : control(std::move(shared)), standalone(preview) {
        if (!gtk_init_check(nullptr, nullptr))
            throw std::runtime_error("Cannot open the Linux display. Run in a desktop session, or use --no-ui.");
        g_object_set(gtk_settings_get_default(), "gtk-application-prefer-dark-theme", TRUE, nullptr);
        if (standalone) {
            std::lock_guard lock(control->mutex);
            const auto choice = control->selectedChoice;
            control->rows[0] = {0, 0, ChoiceBotName(choice), choice.Level(), choice, choice.mode};
            control->status = "Save this default before starting a new match";
        }
        window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
        gtk_widget_set_name(window, "kilobyte-panel");
        gtk_window_set_title(GTK_WINDOW(window), "Kilobyte Skill Selection");
        gtk_window_set_default_size(GTK_WINDOW(window), 740, -1);
        gtk_window_set_focus_on_map(GTK_WINDOW(window), standalone);
        g_signal_connect(window, "destroy", G_CALLBACK(+[](GtkWidget*, gpointer data) {
            static_cast<Impl*>(data)->window = nullptr;
        }), this);
        auto content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
        gtk_container_set_border_width(GTK_CONTAINER(content), 24);
        gtk_container_add(GTK_CONTAINER(window), content);
        auto heading = Label("Kilobyte Skill Selection");
        gtk_style_context_add_class(gtk_widget_get_style_context(heading), "heading");
        Pack(content, heading);
        subtitle = Label(""); Pack(content, subtitle);
        auto scroll = gtk_scrolled_window_new(nullptr, nullptr);
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
        gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroll), TRUE);
        gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(scroll), 64);
        gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scroll), 440);
        roster = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
        gtk_container_add(GTK_CONTAINER(scroll), roster); Pack(content, scroll, true);
        Pack(content, gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
        auto buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
        apply = gtk_button_new_with_label(standalone ? "Save default" : "Apply & respawn");
        gtk_widget_set_name(apply, "apply-ranks");
        gtk_style_context_add_class(gtk_widget_get_style_context(apply), "suggested-action");
        g_signal_connect(apply, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
            static_cast<Impl*>(data)->Apply();
        }), this);
        Pack(buttons, apply, true);
        debug = gtk_check_button_new_with_label("Show debug");
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(debug), control->debug.load());
        g_signal_connect(debug, "toggled", G_CALLBACK(+[](GtkToggleButton* button, gpointer data) {
            auto* self = static_cast<Impl*>(data);
            self->control->debug = gtk_toggle_button_get_active(button);
            gtk_widget_set_visible(self->debugScroll, self->control->debug.load());
        }), this);
        Pack(buttons, debug); Pack(content, buttons);
        footer = Label(""); gtk_label_set_line_wrap(GTK_LABEL(footer), TRUE); Pack(content, footer);
        debugScroll = gtk_scrolled_window_new(nullptr, nullptr);
        gtk_widget_set_size_request(debugScroll, -1, 200);
        details = gtk_text_view_new();
        gtk_text_view_set_editable(GTK_TEXT_VIEW(details), FALSE);
        gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(details), FALSE);
        gtk_text_view_set_monospace(GTK_TEXT_VIEW(details), TRUE);
        gtk_container_add(GTK_CONTAINER(debugScroll), details); Pack(content, debugScroll);
        auto css = gtk_css_provider_new();
        gtk_css_provider_load_from_data(css,
            "#kilobyte-panel { background: #1d2223; color: #d8d0bf; }"
            "#kilobyte-panel .heading { font-size: 24px; font-weight: 600; }"
            "#kilobyte-panel button.suggested-action { background: #2b747a; color: #fff; }", -1, nullptr);
        gtk_style_context_add_provider_for_screen(gtk_widget_get_screen(window), GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        g_object_unref(css);
        Refresh();
        gtk_widget_show_all(window);
        gtk_widget_set_visible(debugScroll, control->debug.load());
    }
    ~Impl() { if (window) gtk_widget_destroy(window); }

    void AddRow(unsigned slot) {
        auto row = std::make_unique<RowWidgets>();
        row->owner = this; row->slot = slot;
        row->box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
        row->caption = Label(""); Pack(row->box, row->caption);
        auto line = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
        row->rank = gtk_combo_box_text_new();
        gtk_widget_set_name(row->rank, ("rank-" + std::to_string(slot)).c_str());
        for (int rank = 0; rank <= MaxSelectableRank; ++rank)
            gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(row->rank), (std::to_string(rank) + " / " + SkillLabel(rank)).c_str());
        g_signal_connect(row->rank, "changed", G_CALLBACK(+[](GtkComboBox* combo, gpointer data) {
            auto* row = static_cast<RowWidgets*>(data);
            row->owner->Edit(*row, gtk_combo_box_get_active(combo), false);
        }), row.get());
        Pack(line, row->rank, true);
        row->minus = gtk_button_new_with_label("−"); row->plus = gtk_button_new_with_label("+");
        for (auto button : {row->minus, row->plus}) {
            gtk_widget_set_size_request(button, 44, -1); Pack(line, button);
            g_signal_connect(button, "clicked", G_CALLBACK(+[](GtkButton* button, gpointer data) {
                auto* row = static_cast<RowWidgets*>(data);
                row->owner->Edit(*row, GTK_WIDGET(button) == row->minus ? -1 : 1, true);
            }), row.get());
        }
        Pack(row->box, line); Pack(roster, row->box);
        gtk_widget_show_all(row->box); widgets.push_back(std::move(row));
    }
    void Edit(const RowWidgets& row, int value, bool relative) {
        if (!window || updating || (!relative && value < 0)) return;
        {
            std::lock_guard lock(control->mutex);
            const auto found = control->rows.find(row.slot);
            if (found == control->rows.end() || control->applying) return;
            const int rank = relative ? found->second.pending.rank + value : value;
            found->second.pending = SkillChoice::FromLevel(std::clamp(rank, 0, MaxSelectableRank));
        }
        Refresh();
    }
    void Apply() {
        if (standalone) {
            SkillChoice choice;
            { std::lock_guard lock(control->mutex); choice = control->rows.at(0).pending; }
            control->Select(choice);
            std::lock_guard lock(control->mutex);
            auto& row = control->rows.at(0);
            row.active = choice.Level(); row.activeMode = choice.mode; row.name = ChoiceBotName(choice);
            control->status = control->saveError.empty() ? "Default saved for the next match" : control->saveError;
        } else if (control->applyPending) control->applyPending();
        Refresh();
    }
    void Refresh() {
        if (!window) return;
        std::map<unsigned, SkillRow> rows;
        std::map<unsigned, SkillTelemetry> telemetry;
        bool busy, ready; std::string status, error;
        {
            std::lock_guard lock(control->mutex);
            rows = control->rows; telemetry = control->telemetry;
            busy = control->applying; ready = control->canRespawn;
            status = control->status; error = control->saveError;
        }
        std::vector<unsigned> slots;
        for (const auto& [slot, row] : rows) slots.push_back(slot);
        std::stable_sort(slots.begin(), slots.end(), [&rows](unsigned a, unsigned b) { return rows.at(a).team < rows.at(b).team; });
        bool rebuild = slots.size() != widgets.size();
        for (size_t i = 0; !rebuild && i < slots.size(); ++i) rebuild = slots[i] != widgets[i]->slot;
        updating = true;
        if (rebuild) {
            for (const auto& row : widgets) gtk_widget_destroy(row->box);
            widgets.clear();
            for (auto slot : slots) AddRow(slot);
        }
        int dirty = 0;
        for (const auto& item : widgets) {
            const auto& row = rows.at(item->slot);
            dirty += row.active != row.pending.Level() || row.activeMode != row.pending.mode;
            const auto caption = standalone ? "Next match" : std::string(row.team ? "Orange / " : "Blue / ") + row.name;
            gtk_label_set_text(GTK_LABEL(item->caption), caption.c_str());
            if (gtk_combo_box_get_active(GTK_COMBO_BOX(item->rank)) != row.pending.rank)
                gtk_combo_box_set_active(GTK_COMBO_BOX(item->rank), row.pending.rank);
            gtk_widget_set_sensitive(item->rank, !busy);
            gtk_widget_set_sensitive(item->minus, !busy && row.pending.rank > 0);
            gtk_widget_set_sensitive(item->plus, !busy && row.pending.rank < MaxSelectableRank);
        }
        updating = false;
        const auto hint = rows.empty() ? "Waiting for Kilobyte bots..." : dirty ?
            std::to_string(dirty) + (dirty == 1 ? " pending change" : " pending changes") : "Choose a rank for each Kilobyte bot";
        gtk_label_set_text(GTK_LABEL(subtitle), hint.c_str());
        gtk_button_set_label(GTK_BUTTON(apply), busy ? "Respawning..." : standalone ? "Save default" : "Apply & respawn");
        gtk_widget_set_sensitive(apply, !busy && (standalone || ready));
        if (!error.empty()) status += "\n" + error;
        gtk_label_set_text(GTK_LABEL(footer), status.c_str());
        std::ostringstream text; text << std::fixed << std::setprecision(2);
        for (const auto& [slot, item] : telemetry) {
            text << item.name << " / " << (item.team ? "Orange" : "Blue") << " / " << item.teammates << 'v' << item.opponents
                 << " / " << item.teamScore << ':' << item.opponentScore << '\n'
                 << "Effective level " << item.effectiveLevel << " / Kilobyte\n";
            if (item.scriptedKickoff) text << "Scripted kickoff / drive + boost / policy sampling paused\n\n";
            else text << "Temperature " << item.settings.sampling.temperature << " / Top K " << item.settings.sampling.topK
                      << " / Cutoff " << 100 * item.settings.sampling.minRelativeProbability << "%\n"
                      << "Best share " << 100 * item.settings.sampling.bestActionProbability << "% target / "
                      << 100 * item.sampling.finalBestProbability << "% actual\n"
                      << "Decision " << item.settings.decisionTicks << " ticks / Delay 7 / Kickoff guard " << (item.kickoff ? "ON" : "off") << "\n\n";
        }
        if (telemetry.empty()) text << "Waiting for live policy diagnostics.\nPending selections do not alter the active bots.";
        const auto value = text.str();
        if (value != lastDebug) {
            auto adjustment = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(debugScroll));
            const auto position = gtk_adjustment_get_value(adjustment);
            gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(details)), value.c_str(), int(value.size()));
            gtk_adjustment_set_value(adjustment, position);
            lastDebug = value;
        }
        refreshed = std::chrono::steady_clock::now();
    }
    bool Pump() {
        for (int i = 0; window && gtk_events_pending() && i < 100; ++i) gtk_main_iteration_do(FALSE);
        if (window && std::chrono::steady_clock::now() - refreshed >= std::chrono::milliseconds(200)) Refresh();
        return window != nullptr;
    }
};

SkillWindow::SkillWindow(std::shared_ptr<SkillControl> control, bool standalone)
    : impl(std::make_unique<Impl>(std::move(control), standalone)) {}
SkillWindow::~SkillWindow() = default;
bool SkillWindow::Pump() { return impl->Pump(); }
void SkillWindow::Wait() {
    while (Pump()) std::this_thread::sleep_for(std::chrono::milliseconds(10));
}
