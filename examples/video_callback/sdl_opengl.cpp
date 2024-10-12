// 
//  Render a video into a custom OpenGL buffer using libVLC 4.0 and SDL 2.0.
// 
//  2021-11
//
//  Compile :
//  g++ -o sdl_opengl sdl_opengl.cpp -std=c++17 $(pkg-config --cflags --libs sdl2) -lGL -L $VLC4_PATH/lib -lvlc -I $VLC4_PATH/include/
//  

#include "vlcpp/vlc.hpp"

#include <cassert>
#include <cstdlib>

#include <array>
#include <initializer_list>
#include <iostream>
#include <mutex>
#include <string_view>

#include "SDL2/SDL.h"
#include "SDL2/SDL_syswm.h"

#define GL_GLEXT_PROTOTYPES 1
#include "SDL2/SDL_opengl.h"

/* -------------------------------------------------------------------------- */

#define DEBUG_LOG_FUNCTION()  if constexpr(false) { std::cerr << __FUNCTION__ << std::endl; }
#define CHECK_SDL_ERRORS()    if constexpr(false) { if (auto err = SDL_GetError(); strlen(err) > 0) { std::cerr << err << std::endl; exit(-1); } }
#define CHECK_GL_ERRORS()     assert(glGetError() == GL_NO_ERROR)

/* -------------------------------------------------------------------------- */

/* Default window resolution */
constexpr unsigned int kWidth  = 800u;
constexpr unsigned int kHeight = 600u;

/* "Big Buck Bunny" movie's uri from the Blender Foundation. */
constexpr char const* kVideoURI{
  "https://video.blender.org/static/webseed/bf1f3fb5-b119-4f9f-9930-8e20e892b898-360.mp4"
  // "file:///home/user/Videos/short/dune-intro.mp4"
};

constexpr bool kEnablePlaylistShuffling = false;

/* -------------------------------------------------------------------------- */

/**
 * Helper to capture frames from a VLC instance.
 */
class FrameCapture : public VLC::VideoOutput::Callbacks {
 public:
  FrameCapture(SDL_Window *window, SDL_GLContext *shared_gl_context) 
    : win_(window)
    , fbos_{}
    , textures_{}
  {
    ctx_ = *shared_gl_context;
  }

  /* Acquire next frame for rendering. */
  GLuint getNextFrame() {
    std::lock_guard<std::mutex> lock(frame_acquisition_mutex_);
    if (frame_acquired_) {
      std::swap(frame_present_id_, frame_swap_id_);
      frame_acquired_ = false;
    }
    return textures_.at(frame_present_id_);
  }

 public:
  // --- Callbacks ----

  /* Called on video initialization, this cannot use the GL context. */
  bool onSetup(const libvlc_video_setup_device_cfg_t *cfg, libvlc_video_setup_device_info_t *out) final {
    DEBUG_LOG_FUNCTION();

    frame_width_ = 0;
    frame_height_ = 0;
    return true;
  }

  /* Called when custom buffer could be released. */
  void onCleanup() final {
    DEBUG_LOG_FUNCTION();

    if ((frame_width_ > 0) && (frame_height_ > 0)) {
      glDeleteTextures(textures_.size(), textures_.data());
      glDeleteFramebuffers(fbos_.size(), fbos_.data());
    }
  }

  /* Called to prepare custom buffers for capture and specify output configuration. */
  bool onUpdateOutput(const libvlc_video_render_cfg_t *cfg, libvlc_video_output_cfg_t *out) final {
    DEBUG_LOG_FUNCTION();

    // Detect frame resize.
    if ((frame_width_ != cfg->width) || (frame_height_ != cfg->height)) {
      std::cerr << " >> Size changed : " << cfg->width << " " << cfg->height << std::endl;
    }

    glGenTextures(textures_.size(), textures_.data());
    glGenFramebuffers(fbos_.size(), fbos_.data());

    frame_width_ = cfg->width;
    frame_height_ = cfg->height;

    for (size_t i = 0; i < textures_.size(); ++i) {
      auto const& tex = textures_[i];
      glBindTexture(GL_TEXTURE_2D, tex);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, frame_width_, frame_height_, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  
      auto const& fbo = fbos_[i];
      glBindFramebuffer(GL_FRAMEBUFFER, fbo);
      glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
      CHECK_GL_ERRORS();
    }
    glBindTexture(GL_TEXTURE_2D, 0);

    if (GL_FRAMEBUFFER_COMPLETE != glCheckFramebufferStatus(GL_FRAMEBUFFER)) {
      std::cerr << std::endl << "Fatal Error : incomplete fbo." << std::endl << std::endl;
      exit(EXIT_FAILURE);
      return false;
    }
    CHECK_GL_ERRORS();        

    // Bind the draw framebuffer.
    glBindFramebuffer(GL_FRAMEBUFFER, fbos_.at(frame_render_id_));
    
    // [required]
    out->opengl_format = GL_RGBA;
    out->full_range    = true;
    out->colorspace    = libvlc_video_colorspace_BT709;
    out->primaries     = libvlc_video_primaries_BT709;
    out->transfer      = libvlc_video_transfer_func_SRGB;

    return true;
  }

  /* Called on each new frame. */
  void onSwap() final {
    DEBUG_LOG_FUNCTION();

    std::lock_guard<std::mutex> lock(frame_acquisition_mutex_);
    std::swap(frame_render_id_, frame_swap_id_);
    glBindFramebuffer(GL_FRAMEBUFFER, fbos_.at(frame_render_id_));
    frame_acquired_ = true;
  }

  /* Called to set the OpenGL context.  */
  bool onMakeCurrent(bool enter) final {
    DEBUG_LOG_FUNCTION();

    return 0 == SDL_GL_MakeCurrent(win_, (enter) ? ctx_ : 0);
  }

  /* Called by VLC to retrieve OpenGL's extensions functions. */
  void* onGetProcAddress(char const* funcname) final {
    return SDL_GL_GetProcAddress(funcname);
  }

 private:
  SDL_Window *win_;
  SDL_GLContext ctx_;

  // TripleBuffer for frame Rendering / Presentation.
  std::array<GLuint, 3> fbos_;
  std::array<GLuint, 3> textures_;
  uint32_t frame_render_id_  = 0u;
  uint32_t frame_swap_id_    = 1u;
  uint32_t frame_present_id_ = 2u;

  std::mutex frame_acquisition_mutex_;
  bool frame_acquired_ = false;

  uint32_t frame_width_  = 0u;
  uint32_t frame_height_ = 0u;
};

/* -------------------------------------------------------------------------- */

template<typename T>
std::string_view toString(T enumClass);

template<>
std::string_view toString(VLC::Media::ParsedStatus enumClass) {
  switch (enumClass) {
    case VLC::Media::ParsedStatus::Skipped:
      return "Skipped";

    case VLC::Media::ParsedStatus::Failed:
      return "Failed";

    case VLC::Media::ParsedStatus::Done:
      return "Done";

    case VLC::Media::ParsedStatus::Timeout:
      return "Timeout";

    default:
      return "VLC::Media::ParsedStatus::Unknown";
  }
}

template<>
std::string_view toString(VLC::Media::Type enumClass) {
  switch (enumClass) {
    case VLC::Media::Type::Unknown:
      return "Unknown";

    case VLC::Media::Type::File:
      return "File";

    case VLC::Media::Type::Directory:
      return "Directory";

    case VLC::Media::Type::Disc:
      return "Disc";

    case VLC::Media::Type::Stream:
      return "Stream";
    
    case VLC::Media::Type::Playlist:
      return "Playlist";

    default:
      return "VLC::Media::Type::Unknown";
  }
}

/**
 * Tiny VLC player instance.
 */
class VLCPlayer {
 public:
  explicit VLCPlayer(std::initializer_list<const char*> args)
    : current_media_id_(0)
  {
    // Instance.
    instance_ = VLC::Instance( static_cast<int>(args.size()), args.begin() );
    // instance_.logSet([](int lvl, const libvlc_log_t*, std::string message) {});

    // Media Player
    mediaplayer_ = VLC::MediaPlayer(instance_);
    
    // Setup some player events callbacks.
    auto &em = mediaplayer_.eventManager();

    em.onMediaChanged([this](VLC::MediaPtr media_ptr) {
      std::cerr << " > media changed : " << media_ptr->mrl() << std::endl;
    });

    em.onOpening([this]() {
      std::cerr << " > opening." << std::endl;
    });

    em.onBuffering([this](float percent) {
      std::cerr << " > loading : " << percent << " %" << std::endl;
    });

    em.onPlaying([this]() {
      std::cerr << " > play" << std::endl;
    });

    em.onPaused([this]() {
      std::cerr << " > paused" << std::endl;
    });
    
    em.onStopped([this]() {
      std::cerr << " > stopped" << std::endl;
    });
  }

  /* Set the SDL window as default output for the mediaplayer. */
  void embedToWindow(SDL_Window *window) {
    assert(nullptr != window);

    SDL_SysWMinfo wmInfo;
    SDL_VERSION(&wmInfo.version);
    SDL_GetWindowWMInfo(window, &wmInfo);
#if defined(__linux__)
    mediaplayer_.setXwindow(wmInfo.info.x11.window);
#elif defined(__APPLE__)
    mediaplayer_.setNsobject(wmInfo.info.cocoa.window);
#elif defined(_WIN32)
    mediaplayer_.setHwnd(wmInfo.info.win.window);
#else
#error "unsupported platform"
#endif
  }

  /* Add a media to the playlist given its URI. */
  void addMedia(std::string_view uri) noexcept {
    auto media = VLC::Media(uri.data(), VLC::Media::FromLocation);
    // mediaplayer_ = VLC::MediaPlayer(instance_, media); //
    
    auto &em = media.eventManager();

    em.onMetaChanged([this](libvlc_meta_t meta) {
      // std::cerr << "\t+ meta changed." << std::endl;
    });

    em.onSubItemAdded([this](VLC::MediaPtr media_ptr) {
      // std::cerr << "\t+ subitem added." << std::endl;
    });

    em.onDurationChanged([this](int64_t duration) {
      // std::cerr << "\t+ duration :" << duration << std::endl;
    });

    em.onParsedChanged([this](VLC::Media::ParsedStatus status) {
      auto media = currentMedia();
      
      std::cerr << "\t+ parsed status : " << toString(status) << std::endl;
      std::cerr << "\t   | type : " << toString(media.type()) << std::endl;

      // Upon entering a playlist, change the media to one of its item.
      // [todo : add all subitems to the player medialist]
      if (VLC::Media::Type::Playlist == media.type()) {
        auto medialist = media.subitems();

        medialist->lock();
        std::cerr << "\t   | subitems count : " << medialist->count() << std::endl;
        if (medialist->count() > 0) {
          int index = 0;

          // shuffle playlist.
          if constexpr(kEnablePlaylistShuffling) {
            float const rnd = (rand() / static_cast<double>(RAND_MAX));
            index = static_cast<int>(rnd * medialist->count());
          }
          auto media_ptr = medialist->itemAtIndex(index);
          
          // stop();
          mediaplayer_.setMedia(*media_ptr);
          mediaplayer_.play();
        }
        medialist->unlock();
      }
    });

    medias_.push_back( media );

    // Parsed the media, in case its a network playlist.
    media.parseRequest(instance_, VLC::Media::ParseFlags::Network, 0);
  }

  /* Launch the media player on the current track. */
  void play() {
    assert( !medias_.empty() );
    
    auto &media = currentMedia();
    mediaplayer_.setMedia(media);
    mediaplayer_.play();
  }

  void setVolume(int volume) noexcept {
    mediaplayer_.setVolume(volume);
  }

  /* Stop the media player. */
  void stop() noexcept {
    mediaplayer_.stopAsync();
  }

  /* Defines callbacks to output video. */
  void bindOutputCallbacks(VLC::VideoOutput::Callbacks &h) {
#if 0

    // (Work in Progress)

    mediaplayer_.setVideoOutputGLCallbacks(
      // setup
      [&h](const libvlc_video_setup_device_cfg_t *cfg, libvlc_video_setup_device_info_t *out) { return h.setup(cfg, out); },

      // cleanup
      [&h]() { h.cleanup(); }, 
      
      // resize
      nullptr, 
      
      // update_output
      [&h](const libvlc_video_render_cfg_t *cfg, libvlc_video_output_cfg_t *out) { return h.update_output(cfg, out); }, 
      
      // swap
      [&h]() { h.swap(); }, 
      
      // makeCurrent
      [&h](bool enter) { return h.make_current(enter); }, 
      
      // getProcAddress
      [&h](char const* funcname) { return h.get_proc_address(funcname); }
    );

#else

    libvlc_video_set_output_callbacks(
      mediaplayer_,
      libvlc_video_engine_opengl,
     
      // Setup
      [](void **data, const libvlc_video_setup_device_cfg_t *cfg, libvlc_video_setup_device_info_t *out) { 
        return ((VLC::VideoOutput::Callbacks*)*data)->onSetup(cfg, out); 
      },

      // Cleanup
      [](void *data) { 
        ((VLC::VideoOutput::Callbacks*)data)->onCleanup();
      }, 
      
      // Resize
      nullptr, 
      
      // UpdateOutput
      [](void *data, const libvlc_video_render_cfg_t *cfg, libvlc_video_output_cfg_t *out) { 
        return ((VLC::VideoOutput::Callbacks*)data)->onUpdateOutput(cfg, out); 
      },
      
      // Swap
      [](void *data) {
        ((VLC::VideoOutput::Callbacks*)data)->onSwap();
      },
      
      // MakeCurrent
      [](void *data, bool current) {
        return ((VLC::VideoOutput::Callbacks*)data)->onMakeCurrent(current);
      },
      
      // GetProcAddress
      [](void *data, char const* funcname) {
        return ((VLC::VideoOutput::Callbacks*)data)->onGetProcAddress(funcname);
      },
      
      // Metadata
      nullptr, 

      // SelectPlane
      nullptr,

      (void*)&h
    );

#endif
  }

  /* Return the current media in the player. */
  VLC::Media& currentMedia() {
    assert( current_media_id_ < medias_.size() );
    return medias_.at(current_media_id_);
  }

 private:
  VLC::Instance instance_;
  VLC::MediaPlayer mediaplayer_;

  // [todo : change for a VLC::MediaList]
  std::vector<VLC::Media> medias_;
  int current_media_id_;
};

/* -------------------------------------------------------------------------- */

/**
 * Handle graphics for the main thread.
 */
class Renderer {
 private:
    static constexpr std::array<GLfloat, 16> kVertices{
      -0.75f, +0.75f,   0.f, 1.f,
      -0.75f, -0.75f,   0.f, 0.f,
      +0.75f, +0.75f,   1.f, 1.f,
      +0.75f, -0.75f,   1.f, 0.f
    };

    static constexpr std::array<GLchar const*, 512> kVertexShader{R"glsl(
#version 150

attribute vec4 aPosition;
attribute vec2 aTexCoord;

varying vec2 vTexCoord;

void main() {
  vTexCoord = aTexCoord;
  gl_Position = vec4(aPosition.xyz, 1.0);
})glsl"};

    static constexpr std::array<GLchar const*, 512> kFragmentShader{R"glsl(
#version 150

uniform sampler2D uFrameTexture;

varying vec2 vTexCoord;

void main() {
  gl_FragColor = texture(uFrameTexture, vTexCoord);
})glsl"};

 public:
  explicit Renderer(SDL_Window *window) 
    : renderer_(nullptr)
    , vao_(0u)
    , vbo_(0u)
    , program_(0u)
  {
    // Create a 2D rendering context for the SDL window.
    // [This will change the current OpenGL context]
    renderer_ = SDL_CreateRenderer(
      window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_TARGETTEXTURE
    );

    // Vertex Array.
    glGenVertexArrays(1, &vao_);
    glGenBuffers(1, &vbo_);

    // Vertex buffer.
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    const GLsizei bytesize = kVertices.size() * sizeof(kVertices[0]);
    glBufferData(GL_ARRAY_BUFFER, bytesize, kVertices.data(), GL_STATIC_DRAW);
  
    // Program Shader.
    program_ = glCreateProgram();
    const GLuint vshader = glCreateShader(GL_VERTEX_SHADER);
    const GLuint fshader = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(vshader, 1, kVertexShader.data(), nullptr);
    glShaderSource(fshader, 1, kFragmentShader.data(), nullptr);
    glCompileShader(vshader);
    glCompileShader(fshader);
    glAttachShader(program_, vshader);
    glAttachShader(program_, fshader);
    glDeleteShader(vshader);
    glDeleteShader(fshader);
    glLinkProgram(program_);
    
    glUseProgram(program_);

    // Bind mesh attributes.
    const GLsizei stride = 4 * sizeof(float);
    // (position)
    GLint pos_attrib = glGetAttribLocation(program_, "aPosition");
    glEnableVertexAttribArray(pos_attrib);
    glVertexAttribPointer(pos_attrib, 2, GL_FLOAT, GL_FALSE, stride, 0);
    // (texcoord)
    GLint uv_attrib = glGetAttribLocation(program_, "aTexCoord");
    glEnableVertexAttribArray(uv_attrib);
    glVertexAttribPointer(uv_attrib, 2, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<GLvoid*>(2*sizeof(float)));

    // Set texture uniform.
    const int texture_unit = 0;
    const GLint loc = glGetUniformLocation(program_, "uFrameTexture");
    glActiveTexture(GL_TEXTURE0 + texture_unit);
    glUniform1i(loc, texture_unit);

    glUseProgram(0u);
    glBindVertexArray(0u);

    CHECK_GL_ERRORS();
  }

  ~Renderer() {
    if (!renderer_) {
      return;
    }

    glDeleteVertexArrays(1, &vao_);
    glDeleteBuffers(1, &vbo_);
    glDeleteProgram(program_);

    SDL_DestroyRenderer(renderer_);
  }

  /* Render video frame as a texture. */
  void renderFrame(GLuint frame_texture_id) const {
    glViewport(0, 0, kWidth, kHeight); //

    glClearColor(0.85f, 0.75f, 0.5f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    glUseProgram(program_);
    glBindTexture(GL_TEXTURE_2D, frame_texture_id);

    glBindVertexArray(vao_);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0u);

    glBindTexture(GL_TEXTURE_2D, 0);
    glUseProgram(0u);

    CHECK_GL_ERRORS();
  }

 private:
  SDL_Renderer *renderer_;

  GLuint vao_;
  GLuint vbo_;
  GLuint program_;
};

/* -------------------------------------------------------------------------- */

int main(int argc, char *argv[]) {
  // Check VLC plugin path env.
  // if constexpr (true) {
  //   std::cerr << "VLC_PLUGIN_PATH=" << (char*)getenv("VLC_PLUGIN_PATH") << std::endl;
  //   char env[]{ "VLC_PLUGIN_PATH=/usr/local/lib/vlc/plugins/" };
  //   putenv(env);
  // }

  // Initialize SDL.
  // SDL_SetHint(SDL_HINT_RENDER_DRIVER, "opengl");
  // if (SDL_Init(SDL_INIT_VIDEO) < 0) {
  //   std::cerr << "Could not initialize SDL: " << SDL_GetError() << std::endl;
  //   return EXIT_FAILURE;
  // }
  // atexit(SDL_Quit);

  // Set OpenGL attributes.
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
  SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
  // SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);

  // Create a SDL window for OpenGL.
  SDL_Window *window = SDL_CreateWindow(
    "SDL-OpenGL window",
    SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
    kWidth, kHeight,
    SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN 
  );
  if (!window) {
    std::cerr << "Couldn't create the window: " << SDL_GetError() << std::endl;
    return EXIT_FAILURE;
  }

  // Create an OpenGL context associated with the window. and used by the VLC instance.
  SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 0);
  auto gl_context = SDL_GL_CreateContext(window);
  SDL_GL_SetSwapInterval(0);

  // Create a renderer for window display, the new context will have access to gl_context data.
  SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 1);
  Renderer renderer(window);

  // [!important!] Enable v-sync on the main thread rendering-context to prevent stalling.
  SDL_GL_SetSwapInterval(1);

  // Create a VLC mini player.
  VLCPlayer vlc({
    // "-vv",
    "--no-xlib",
    "--video", 
    "--audio",
    "--no-osd",
    "--hw-dec", 
    ":demux=h264", "--h264-fps=30",
    ":demux=hevc", "--hevc-fps=30",
    "--file-caching=300",
    "--network-caching=1000",
    "--fps-fps=60",
  });
  
  // [Optional] Embed the vlc player into the window by default (requires --no-xlib).
  // if constexpr(true) {
  //   vlc.embedToWindow(window);
  // }

  // Create a custom frame capture and bind it to the VLC player.
  FrameCapture frame_capture(window, &gl_context);
  vlc.bindOutputCallbacks(frame_capture);

  // Launch a media.
  auto const uri = (argc > 1) ? argv[1] : kVideoURI;
  vlc.addMedia(uri);
  vlc.play();

  // Main thread loop.
  for (bool done = false; !done;) {

    // Events.
    for (SDL_Event event; SDL_PollEvent(&event);) {
      switch(event.type) {
        case SDL_QUIT:
          done = true;
        break;

        case SDL_KEYDOWN:
          switch (event.key.keysym.sym) {
            case SDLK_ESCAPE:
            case SDLK_q:
              done = true;
              break;
          }
        break;
      }
    }

    // Retrieve next frame from video and render it.
    const GLuint frame_texture_id = frame_capture.getNextFrame();
    renderer.renderFrame( frame_texture_id );
    
    SDL_GL_SwapWindow(window);
  }

  // Stop the media.
  vlc.stop();

  return EXIT_SUCCESS;
}

/* -------------------------------------------------------------------------- */
