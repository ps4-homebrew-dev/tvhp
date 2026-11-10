// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <sys/stat.h>

// Makes main() SDL_main() on a PS4, where SDL2main sets up the XDG
// directories, hides the splash screen, and exits to the home screen.
#include <SDL.h>

#include <RmlUi/Core.h>

#include "RmlUi/RmlUi_Backend.h"
#include "app.h"

#define SCREEN_WIDTH  1920
#define SCREEN_HEIGHT 1080


static void PrintUsage(const char* argv0) {
  std::printf(
    "Usage: %s [options]\n"
    "  -a, --assets DIR   RML documents, stylesheets and icons (default: ./assets)\n"
    "  -f, --fonts DIR    fonts (default: ./fonts)\n"
    "  -c, --config FILE  saved servers (default: ./tvhp.cfg; on a PS4 in\n"
    "                     the preference folder, under /download0)\n"
    "  -C, --cache DIR    channel icons (default: $XDG_CACHE_HOME/tvhp)\n"
    "  -h, --help         this text\n",
    argv0);
}

extern "C" int SDL_main(int argc, char* argv[]) {
  App::Options options;

#if defined(__SCE__)
  options.assets_dir = "/app0/assets";
  options.fonts_dir = "/app0/fonts";
  // libcurl's certificates, for channel logos on https (see CMakeLists.txt).
  ::setenv("SSL_CERT_FILE", "/app0/ca-bundle.crt", 0);

  // The saved servers go in the preference folder, which SDL2main puts in
  // $XDG_DATA_HOME: on the title's download data, /download0, for this
  // non-game app (see CMakeLists.txt), where they outlive the app and its
  // updates.
  if (char* pref = SDL_GetPrefPath("", "tvhp")) {
    options.config_path = std::string(pref) + "tvhp.cfg";
    SDL_free(pref);
  } else {
    std::fprintf(stderr, "No folder for the saved servers: %s\n", SDL_GetError());
  }
#else
  const std::string cwd = std::filesystem::current_path();
  options.assets_dir = cwd + "/assets";
  options.fonts_dir = cwd + "/fonts";
#endif

  // The channel icons: $XDG_CACHE_HOME/tvhp (on the PS4 also in
  // /download0), or ~/.cache/tvhp.
  if (const char* cache = std::getenv("XDG_CACHE_HOME"); cache && *cache)
    options.cache_dir = std::string(cache) + "/tvhp";
  else if (const char* home = std::getenv("HOME"); home && *home)
    options.cache_dir = std::string(home) + "/.cache/tvhp";

  for (int i = 1; i < argc; i++) {
    auto arg = [&](const char* s, const char* l) {
      return !std::strcmp(argv[i], s) || !std::strcmp(argv[i], l);
    };
    if (arg("-h", "--help")) {
      PrintUsage(argv[0]);
      return 0;
    } else if (arg("-a", "--assets") && i + 1 < argc) {
      options.assets_dir = argv[++i];
    } else if (arg("-f", "--fonts") && i + 1 < argc) {
      options.fonts_dir = argv[++i];
    } else if (arg("-c", "--config") && i + 1 < argc) {
      options.config_path = argv[++i];
    } else if (arg("-C", "--cache") && i + 1 < argc) {
      options.cache_dir = argv[++i];
    } else {
      std::fprintf(stderr, "%s: unknown option '%s'\n", argv[0], argv[i]);
      PrintUsage(argv[0]);
      return 1;
    }
  }

  std::fprintf(stderr, "tvhp: servers in %s, icons in %s\n", options.config_path.c_str(),
               options.cache_dir.empty() ? "(none)" : options.cache_dir.c_str());

  Rml::Context* ctx;
  std::string err;
  App app;

  if (!Backend::Initialize("tvhp", SCREEN_WIDTH, SCREEN_HEIGHT, false)) {
    std::fprintf(stderr, "Failed to initialize the backend\n");
    return -1;
  }

  Rml::SetSystemInterface(Backend::GetSystemInterface());
  Rml::SetRenderInterface(Backend::GetRenderInterface());
  Rml::Initialise();

  if (!(ctx=Rml::CreateContext("main", Rml::Vector2i(SCREEN_WIDTH, SCREEN_HEIGHT)))) {
    Rml::Log::Message(Rml::Log::LT_ERROR, "Failed to create main context");
    Rml::Shutdown();
    Backend::Shutdown();
    return -1;
  }

  Rml::LoadFontFace(options.fonts_dir + "/LatoLatin-Bold.ttf", false);
  Rml::LoadFontFace(options.fonts_dir + "/LatoLatin-BoldItalic.ttf", false);
  Rml::LoadFontFace(options.fonts_dir + "/LatoLatin-Italic.ttf", false);
  Rml::LoadFontFace(options.fonts_dir + "/LatoLatin-Regular.ttf", false);
  // Fallbacks, for characters the text font lacks: the UI's icons (in the
  // Private Use Area, see src/icons.h), then emoji in titles and names.
  Rml::LoadFontFace(options.fonts_dir + "/lucide.ttf", true);
  Rml::LoadFontFace(options.fonts_dir + "/NotoEmoji-VariableFont_wght.ttf", true);

  if (!app.Initialize(ctx, options, err)) {
    Rml::Log::Message(Rml::Log::LT_ERROR, "Failed to initialize app: %s", err.c_str());
    Rml::Shutdown();
    Backend::Shutdown();
    return -1;
  }

  while (Backend::ProcessEvents(ctx)) {
    app.Update();
    ctx->Update();

    // The video goes straight to the backbuffer; the document, whose body
    // is transparent while watching, is composited over it.
    Backend::BeginFrame([&app](int width, int height) { app.RenderVideo(width, height); });
    ctx->Render();
    Backend::PresentFrame();
  }

  app.Shutdown();
  Rml::Shutdown();
  Backend::Shutdown();

  return 0;
}

#if defined(__linux__)
int main(int argc, char* argv[]) {
  return SDL_main(argc, argv);
}
#endif
