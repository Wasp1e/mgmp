// mgmp_lang.h -- the language of the mod's own words.
//
// The game keeps its language in its settings.txt, next to the saves:
// %APPDATA%\Glaiel Games\Mewgenics\<SteamID>\settings.txt, a line
// "current_language ru". The mod reads the same line, so it speaks the
// language this player's game does: Russian for "ru", English for anything
// else. With no settings to read, Windows' own UI language decides.
#pragma once

namespace mgmp {

// The game's language code ("ru", "en", "sp"...), re-read every few seconds so
// a change in the game's options shows without a restart. Never null.
const char* lang_code();

// The game is in Russian.
bool lang_ru();

// The Russian or the English of one of the mod's words, for this player.
inline const char* tr(const char* ru, const char* en) { return lang_ru() ? ru : en; }

} // namespace mgmp
