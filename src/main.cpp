// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <sys/stat.h>

#include <RmlUi/Core.h>

#include "RmlUi/RmlUi_Backend.h"
#include "app.h"

#define SCREEN_WIDTH  1920
#define SCREEN_HEIGHT 1080


typedef struct SceAppContentInitParam {
  char reserved[32];
} SceAppContentInitParam;

typedef struct SceAppContentBootParam {
  char     reserved1[4];
  uint32_t attr;
  char     reserved2[32];
} SceAppContentBootParam;

typedef struct SceAppContentMountPoint {
  char data[16];
} SceAppContentMountPoint;


extern "C" {
  int sceSystemServiceLoadExec(const char* path, char* const argv[]);
  int sceAppContentInitialize(const SceAppContentInitParam *initParam,
			      SceAppContentBootParam *bootParam);
  int sceAppContentTemporaryDataMount2(uint32_t option, SceAppContentMountPoint *mountPoint);
  int sceAppContentTemporaryDataUnmount(const SceAppContentMountPoint *mountPoint);
  int sceSysmoduleLoadModule(uint64_t);
}

#if defined(__ORBIS__)
static SceAppContentMountPoint  g_tmpmnt = {0};

__attribute__((constructor))
static void OrbisInit(void) {
  SceAppContentInitParam ip = {};
  SceAppContentBootParam bp = {};

  sceSysmoduleLoadModule(0x00b4);
  sceAppContentInitialize(&ip, &bp);
  sceAppContentTemporaryDataMount2(0, &g_tmpmnt);
}
#endif


static void PrintUsage(const char* argv0) {
  std::printf(
    "Usage: %s [options]\n"
    "  -a, --assets DIR   RML documents, stylesheets and icons (default: ./assets)\n"
    "  -f, --fonts DIR    fonts (default: ./fonts)\n"
    "  -c, --config FILE  saved servers (default: ./tvhp.cfg)\n"
    "  -h, --help         this text\n",
    argv0);
}

static int Run(int argc, char* argv[]) {
  App::Options options;
  std::string fonts_dir;

#if defined(__ORBIS__)
  options.assets_dir = "/app0/assets";
  fonts_dir = "/app0/fonts";
  options.config_path = "/temp0/tvhp.cfg";
#else
  const std::string cwd = std::filesystem::current_path();
  options.assets_dir = cwd + "/assets";
  fonts_dir = cwd + "/fonts";
#endif

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
      fonts_dir = argv[++i];
    } else if (arg("-c", "--config") && i + 1 < argc) {
      options.config_path = argv[++i];
    } else {
      std::fprintf(stderr, "%s: unknown option '%s'\n", argv[0], argv[i]);
      PrintUsage(argv[0]);
      return 1;
    }
  }

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

  Rml::LoadFontFace(fonts_dir + "/LatoLatin-Bold.ttf", false);
  Rml::LoadFontFace(fonts_dir + "/LatoLatin-BoldItalic.ttf", false);
  Rml::LoadFontFace(fonts_dir + "/LatoLatin-Italic.ttf", false);
  Rml::LoadFontFace(fonts_dir + "/LatoLatin-Regular.ttf", false);
  Rml::LoadFontFace(fonts_dir + "/NotoEmoji-VariableFont_wght.ttf", true);

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

int main(int argc, char* argv[]) {
  const int status = Run(argc, argv);
#if defined(__ORBIS__)
  // A PS4 process that returns from main() is reported as a crash; asking
  // the system to close the title is the clean way out.
  sceSystemServiceLoadExec("exit", nullptr);
#endif
  return status;
}


