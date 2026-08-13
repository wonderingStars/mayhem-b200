/*
 * mayhem-b200 — web portal panel provider for Weather/TPMS.
 *
 * Publishes WeatherView's recent-entries table as a generic table panel: the
 * same six columns the app draws on the 240x320 screen, in the same order, with
 * the same per-cell text. Every value is read straight off a
 * tpms::RecentEntry that src/apps/ui_weather.cpp already filled in — the FSK/OOK
 * demodulation, the Manchester decode, the three Schrader frame formats and
 * their checksums all ran long before this file sees an entry, and none of it is
 * repeated or re-derived here.
 *
 * The app itself gains exactly one thing: WeatherView::entries(), a const
 * accessor for the list it already holds, because the container is a private
 * member and a provider is handed only a ui::View&. That is the same accessor
 * AprsTableView and AdsbRxView already expose for the same reason.
 *
 * UNITS. The app's Pres and Temp columns are drawn in whichever units the
 * operator picked from the two OptionsFields at the top of the screen, and
 * those fields — like the pressure_unit_/temp_unit_ members they set — are
 * private, with no accessor. Widening the app further to read them is more than
 * the one line this provider is allowed, so the panel publishes the units the
 * entry natively stores and the app itself starts in: KILOPASCAL and CELSIUS.
 * The numbers are real readings either way, but an operator who has switched
 * the device to PSI/Fahrenheit will see a different number in the browser than
 * on the screen. Making the two agree needs an accessor for the unit selection,
 * which is a separate change.
 *
 * It lives here rather than in the app's own .cpp so that portal concerns stay
 * out of the ported app; ProviderRegistrar (app_bridge.hpp) is designed for
 * exactly this and self-registers at static-init time, so nothing else has to
 * know this file exists.
 *
 * THREADING: a provider is only ever called from AppBridge::refresh(), i.e.
 * on the UI thread, so walking the view tree and reading the entries list here
 * races nothing — see app_bridge.hpp's file header.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "app_bridge.hpp"

#include "../apps/app_context.hpp"
#include "../apps/ui_navigation.hpp"
#include "../apps/ui_weather.hpp"
#include "../core/string_format.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace remote {
namespace {

/* WeatherView's own columns_ initializer, repeated (src/apps/ui_weather.hpp).
 * It cannot be read back off a running view — the RecentEntriesColumns object
 * is held only by private members of the view, its table and its header — so
 * the list is duplicated here and pinned by a test rather than being free to
 * drift. The widths are the app's declared ones; RecentEntriesView resolves the
 * zero-width ID column to whatever is left on screen, which is a layout
 * decision TableData does not carry. */
ui::RecentEntriesColumns weather_columns() {
    return ui::RecentEntriesColumns{
        {"Tp", 2}, {"ID", 0}, {"Pres", 4}, {"Temp", 4}, {"Cnt", 3}, {"Fl", 2}};
}

/* One row, laid out to agree cell-for-cell with the line WeatherView's own
 * on_draw paints. Tp really is the numeric Reading::Type code rather than its
 * name — that is what the app draws in that column, and the browser showing the
 * name instead would be showing a different table.
 *
 * What is deliberately not copied is only the fixed-width screen's alignment:
 * the ID is padded out to the resolved column width, the type code is
 * right-justified in two characters and the pressure, temperature and count are
 * right-justified in three, with single spaces between the columns. A browser
 * table lays its own cells out and would only have to strip all of that again.
 * The "+++" ceiling past 999 receptions is not alignment — it is a real limit on
 * what the app will show — so it stays.
 *
 * A format that does not carry a field leaves that cell empty, exactly as the
 * device leaves it blank: the OOK 8k192 Schrader frame has no temperature, and
 * neither of the FSK/GMC formats carries flags. An absent reading is emphatically
 * not 0 kPa or 0 C. The reception count is a genuine counter, so a zero there
 * would be a real value and is published as one. */
std::vector<std::string> weather_row(const app::tpms::RecentEntry& e) {
    return {
        to_string_dec_uint(static_cast<uint32_t>(e.type)),
        to_string_hex(e.id, 8),
        e.has_pressure ? trim(app::tpms::format_pressure(e.last_pressure,
                                                         app::tpms::PressureUnit::Kpa))
                       : std::string{},
        e.has_temperature ? trim(app::tpms::format_temperature(e.last_temperature,
                                                              app::tpms::TempUnit::Celsius))
                          : std::string{},
        (e.received_count > 999) ? std::string{"+++"}
                                 : to_string_dec_uint(e.received_count),
        e.has_flags ? to_string_hex(e.last_flags, 2) : std::string{},
    };
}

/* The provider is handed nav->top(), which is the TpmsDetailView whenever the
 * operator has opened a sensor's detail page on the device. WeatherView stays
 * on the stack underneath it, so walk down for it rather than letting the
 * browser's view go blank the moment someone touches the device. */
app::WeatherView* find_weather_view(ui::View& top) {
    if (auto* v = dynamic_cast<app::WeatherView*>(&top)) return v;

    auto& ctx = app::globals();
    if (ctx.nav == nullptr) return nullptr;
    for (size_t i = 0; i < ctx.nav->depth(); i++) {
        if (auto* v = dynamic_cast<app::WeatherView*>(ctx.nav->at_depth(i))) return v;
    }
    return nullptr;
}

}  // namespace

/* Exposed rather than left in the anonymous namespace above so that
 * tests/test_provider_subcar_tt_wx.cpp can drive it with entries the app's own
 * decoders produced. The formatting is the part that has to keep agreeing with
 * the device screen, and that is only worth asserting against the app's own
 * output. */
TableData weather_table_data(const app::tpms::RecentEntries& entries) {
    return table_data_from_entries(weather_columns(), entries, weather_row);
}

namespace {

PanelData weather_panel(ui::View& view) {
    PanelData panel;

    app::WeatherView* app_view = find_weather_view(view);
    if (app_view == nullptr) {
        panel.kind = PanelKind::Screen;
        panel.screen.message = "Weather/TPMS is not the open app.";
        return panel;
    }

    panel.kind = PanelKind::Table;
    panel.table = weather_table_data(app_view->entries());
    return panel;
}

/* The id is the one src/apps/ui_weather.cpp registers with app::Registrar. Get
 * it wrong and the provider is never reached: the app keeps showing the portal's
 * placeholder card and nothing anywhere reports an error. */
const ProviderRegistrar reg_weather{"weather", PanelKind::Table, weather_panel};

}  // namespace
}  // namespace remote
