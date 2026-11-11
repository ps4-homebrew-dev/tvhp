#pragma once

#include <RmlUi/Core/Input.h>

// Every key the UI reacts to, in one place. A pad (gamepad.cpp turns its
// buttons into keys), a remote (SDL reports an HDMI-CEC remote's buttons as
// keys) and a keyboard all end up on the same commands.
//
// The keys follow what a remote sends: its "back" arrives as Escape, so
// Escape is the cancel key and Circle is Escape too; its play/pause arrives
// as Space, so Space is a transport key and Square is Tab.

namespace keymap {

  // A remote's rewind and fast forward, for which RmlUi has no keys.
  constexpr int KI_MEDIA_REWIND = Rml::Input::KI_FIRST_CUSTOM_KEY + 0;
  constexpr int KI_MEDIA_FAST_FORWARD = Rml::Input::KI_FIRST_CUSTOM_KEY + 1;
  // L1 and R1, apart from Page Up/Down (L2/R2, a remote's P+/P-).
  constexpr int KI_TAB_PREV = Rml::Input::KI_FIRST_CUSTOM_KEY + 2;
  constexpr int KI_TAB_NEXT = Rml::Input::KI_FIRST_CUSTOM_KEY + 3;
  static_assert(KI_TAB_NEXT <= Rml::Input::KI_LAST_CUSTOM_KEY,
                "custom key identifiers must stay inside RmlUi's range");

  // What a key means; what a command does is up to the view.
  enum class Cmd {
    None,

    // Navigation.
    Up,
    Down,
    Left,
    Right,
    PageUp,   // L2        | P+
    PageDown, // R2        | P-
    TabPrev,  // L1
    TabNext,  // R1
    Home,
    End,

    // Actions: the pad's button | the remote's | the keyboard's.
    Ok,        // Cross     | enter          | Enter
    Back,      // Circle    | back, prev     | Escape, Backspace
    Secondary, // Square    | display        | Tab
    Remove,    // Triangle  | red            | Delete
    Options,   // Options   | menu, context  | Insert   (or hold Cross / Enter)
    Guide,     // Select    | guide          | F9 (no pad button on a PS4)

    // A remote's transport keys; a pad does without.
    PlayPause,   // play/pause, pause
    Stop,        // stop
    SeekBack,    // rewind
    SeekForward, // fast forward
    SkipBack,    // |<
    SkipForward, // >|
    Audio,       // audio: the next audio track
  };

  constexpr Cmd Command(int key) {
    switch (key) {
    case Rml::Input::KI_UP:                 return Cmd::Up;
    case Rml::Input::KI_DOWN:               return Cmd::Down;
    case Rml::Input::KI_LEFT:               return Cmd::Left;
    case Rml::Input::KI_RIGHT:              return Cmd::Right;
    case KI_TAB_PREV:                       return Cmd::TabPrev;
    case KI_TAB_NEXT:                       return Cmd::TabNext;
    case Rml::Input::KI_PRIOR:              return Cmd::PageUp;
    case Rml::Input::KI_NEXT:               return Cmd::PageDown;
    case Rml::Input::KI_HOME:               return Cmd::Home;
    case Rml::Input::KI_END:                return Cmd::End;

    case Rml::Input::KI_RETURN:             return Cmd::Ok;
    case Rml::Input::KI_NUMPADENTER:        return Cmd::Ok;

    // Backspace (a remote's "prev" too) is Back as well; a view with text
    // fields tests for Escape instead.
    case Rml::Input::KI_ESCAPE:             return Cmd::Back;
    case Rml::Input::KI_BACK:               return Cmd::Back;

    case Rml::Input::KI_TAB:                return Cmd::Secondary;
    case Rml::Input::KI_F4:                 return Cmd::Secondary;

    case Rml::Input::KI_DELETE:             return Cmd::Remove;
    case Rml::Input::KI_F5:                 return Cmd::Remove;

    case Rml::Input::KI_APPS:               return Cmd::Options;
    case Rml::Input::KI_INSERT:             return Cmd::Options;

    case Rml::Input::KI_F9:                 return Cmd::Guide;

    // An HDMI-CEC remote's audio key arrives as F2 (its display key as F4).
    case Rml::Input::KI_F2:                 return Cmd::Audio;

    case Rml::Input::KI_SPACE:              return Cmd::PlayPause;
    case Rml::Input::KI_PAUSE:              return Cmd::PlayPause;
    case Rml::Input::KI_MEDIA_PLAY_PAUSE:   return Cmd::PlayPause;
    case Rml::Input::KI_MEDIA_STOP:         return Cmd::Stop;
    case Rml::Input::KI_MEDIA_PREV_TRACK:   return Cmd::SkipBack;
    case Rml::Input::KI_MEDIA_NEXT_TRACK:   return Cmd::SkipForward;
    case KI_MEDIA_REWIND:                   return Cmd::SeekBack;
    case KI_MEDIA_FAST_FORWARD:             return Cmd::SeekForward;

    default:                                return Cmd::None;
    }
  }

} // namespace keymap
