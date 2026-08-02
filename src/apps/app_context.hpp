/*
 * mayhem-b200 — shared application context.
 *
 * The firmware reaches its radio and audio through file-scope globals in the
 * portapack namespace. Doing the same here, but behind an accessor, keeps app
 * constructors short without scattering extern declarations everywhere.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_APP_CONTEXT_H__
#define __MB200_APP_CONTEXT_H__

namespace radio {
class UsrpRadio;
class ReceiverModel;
class TransmitterModel;
}  // namespace radio

namespace audio {
class AudioOut;
class AudioIn;
}  // namespace audio

namespace ui {
class NavigationView;
}

namespace app {

struct Context {
    radio::UsrpRadio* radio{nullptr};
    radio::ReceiverModel* receiver{nullptr};
    radio::TransmitterModel* transmitter{nullptr};
    audio::AudioOut* audio_out{nullptr};
    audio::AudioIn* audio_in{nullptr};
    ui::NavigationView* nav{nullptr};
};

/* Populated once by main() before the first view is pushed. */
Context& globals();

}  // namespace app

#endif /*__MB200_APP_CONTEXT_H__*/
