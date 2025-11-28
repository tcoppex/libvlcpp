//
//  Render a video into a custom OpenGL buffer using libVLC 4.0 and GLFW 3
//
// Compile with:
// g++ -std=c++17 glfw_sample.cpp -lGLEW -lGLESv2 -lglfw -lm -lvlc -I ../../
// -L $VLC4_PATH/lib -Wl,-rpath,$VLC4_PATH/lib -I $VLC4_PATH/include/ -I ../../
//

#include <array>
#include <iostream>
#include <mutex>
#include <initializer_list>

#define USE_OPENGL_COMPATIBLE 1

#if USE_OPENGL_COMPATIBLE
#include <GL/glew.h>
#else
#define GLFW_INCLUDE_ES31
#endif

#include <GLFW/glfw3.h>

#include "vlcpp/vlc.hpp"

// ----------------------------------------------------------------------------

#define DEBUG_LOG_FUNCTION()  if constexpr(true) { std::cerr << __FUNCTION__ << std::endl; }
#define DEBUG_LOG()           std::cerr << __LINE__ << std::endl;
#define CHECK_GL_ERRORS()     if (checkGLError()) {DEBUG_LOG();}

// ----------------------------------------------------------------------------

constexpr unsigned int kWidth  = 1920u;
constexpr unsigned int kHeight = 1080u;

constexpr char const* kVideoURI{
    "https://video.blender.org/static/webseed/bf1f3fb5-b119-4f9f-9930-8e20e892b898-360.mp4"
};

constexpr GLfloat vertices[]{
    0.5f,  0.5f, 0.0f,
   -0.5f,  0.5f, 0.0f,
   -0.5f, -0.5f, 0.0f,
    0.5f, -0.5f, 0.0f
};

constexpr GLuint indices[]{
    0, 1, 2,
    2, 3, 0
};

char const* vertexShaderSrc = R"(
    #version 300 es
    layout(location = 0) in vec3 position;
    out vec2 vTexCoords;
    void main() {
        gl_Position = vec4(1.75 * position.xyz, 1.0);
        vTexCoords = (position.xy + vec2(0.5));
    }
)";

char const* fragmentShaderSrc = R"(
    #version 300 es
    precision mediump float;
    uniform sampler2D uTexture;
    in vec2 vTexCoords;
    out vec4 fragColor;
    void main() {
        fragColor = texture(uTexture, vTexCoords);
    }
)";

// ----------------------------------------------------------------------------

bool checkGLError() {
    GLenum err;
    while ((err = glGetError()) != GL_NO_ERROR) {
        switch (err) {
        case GL_INVALID_OPERATION:
            std::cerr << "GL_INVALID_OPERATION: An operation is not allowed in the current state." << std::endl;
            break;
        case GL_INVALID_ENUM:
            std::cerr << "GL_INVALID_ENUM: An unacceptable value is specified for an enumerated argument." << std::endl;
            break;
        case GL_INVALID_VALUE:
            std::cerr << "GL_INVALID_VALUE: A numeric argument is out of range." << std::endl;
            break;
        case GL_OUT_OF_MEMORY:
            std::cerr << "GL_OUT_OF_MEMORY: There is not enough memory left to execute the command." << std::endl;
            break;
        default:
            std::cerr << "Unknown OpenGL error: " << err << std::endl;
            break;
        }
        return true;
    }
    return false;
}


// ----------------------------------------------------------------------------

class FrameCapture : public VLC::VideoOutput::Callbacks {
 public:
    FrameCapture(GLFWwindow *shared_ctx)
        : shared_ctx_(shared_ctx)
    {}

    virtual ~FrameCapture() {
    }

    GLuint getNextFrame() {
        std::lock_guard<std::mutex> lock(frame_acquisition_mutex_);
        if (frame_acquired_) {
            std::swap(frame_present_id_, frame_swap_id_);
            frame_acquired_ = false;
        }
        return textures_.at(frame_present_id_);
    }

 public:
    bool onSetup(const libvlc_video_setup_device_cfg_t *cfg, libvlc_video_setup_device_info_t *out) final {
        frame_width_ = 0;
        frame_height_ = 0;
        return true;
    }

    void onCleanup() final {
        if ((frame_width_ > 0) && (frame_height_ > 0)) {
            glDeleteTextures(textures_.size(), textures_.data());
            glDeleteFramebuffers(fbos_.size(), fbos_.data());
        }
        CHECK_GL_ERRORS();
    }

    bool onUpdateOutput(const libvlc_video_render_cfg_t *cfg, libvlc_video_output_cfg_t *out) final {
        if ((frame_width_ != cfg->width) || (frame_height_ != cfg->height)) {
            std::cerr << "Frame size changed: " << cfg->width << " " << cfg->height << std::endl;
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
        }

        glBindTexture(GL_TEXTURE_2D, 0);

        if (GL_FRAMEBUFFER_COMPLETE != glCheckFramebufferStatus(GL_FRAMEBUFFER)) {
            std::cerr << std::endl << "Fatal Error : incomplete fbo." << std::endl << std::endl;
            // return false;
            exit(EXIT_FAILURE);
        }
        glBindFramebuffer(GL_FRAMEBUFFER, fbos_.at(frame_render_id_));
        CHECK_GL_ERRORS();

        // [required]
        out->opengl_format = GL_RGBA;
        out->full_range    = true;
        out->colorspace    = libvlc_video_colorspace_BT709;
        out->primaries     = libvlc_video_primaries_BT709;
        out->transfer      = libvlc_video_transfer_func_SRGB;
        out->orientation   = libvlc_video_orient_top_left;

        return true;
    }

    void onSwap() final {
        std::lock_guard<std::mutex> lock(frame_acquisition_mutex_);
        std::swap(frame_render_id_, frame_swap_id_);
        glBindFramebuffer(GL_FRAMEBUFFER, fbos_.at(frame_render_id_));
        frame_acquired_ = true;
        CHECK_GL_ERRORS();
    }

    bool onMakeCurrent(bool enter) final {
        GLFWwindow *new_ctx = enter ? shared_ctx_ : nullptr;
        glfwMakeContextCurrent(new_ctx);
        return glfwGetCurrentContext() == new_ctx;
    }

    void* onGetProcAddress(char const* funcname) final {
        auto func_ptr = glfwGetProcAddress(funcname);
        return reinterpret_cast<void*>(func_ptr);
    }

 private:
    GLFWwindow* shared_ctx_ = nullptr;

    std::array<GLuint, 3> fbos_;
    std::array<GLuint, 3> textures_;
    uint32_t frame_render_id_  = 0u;
    uint32_t frame_swap_id_    = 1u;
    uint32_t frame_present_id_ = 2u;

    uint32_t frame_width_  = 0u;
    uint32_t frame_height_ = 0u;

    std::mutex frame_acquisition_mutex_;
    bool frame_acquired_ = false;
};

// ----------------------------------------------------------------------------

class VLCPlayer {
 public:
    explicit VLCPlayer(std::initializer_list<const char*> args)
        : current_media_id_(0)
    {
        instance_ = VLC::Instance(static_cast<int>(args.size()), args.begin());

        mediaplayer_ = VLC::MediaPlayer(instance_);

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

    ~VLCPlayer() {
        stop();
    }

    void addMedia(std::string_view uri) noexcept {
        auto media = VLC::Media(uri.data(), VLC::Media::FromLocation);

        auto &em = media.eventManager();
        em.onMetaChanged([this](libvlc_meta_t meta) {
            std::cerr << "\t+ meta changed." << std::endl;
        });
        em.onSubItemAdded([this](VLC::MediaPtr media_ptr) {
            std::cerr << "\t+ subitem added." << std::endl;
        });
        em.onDurationChanged([this](int64_t duration) {
            std::cerr << "\t+ duration :" << duration << std::endl;
        });
        em.onParsedChanged([this](VLC::Media::ParsedStatus status) {
            auto media = currentMedia();

            if (VLC::Media::Type::Playlist == media.type()) {
                auto medialist = media.subitems();
                medialist->lock();
                std::cerr << "\t   | subitems count : " << medialist->count() << std::endl;
                if (medialist->count() > 0) {
                    auto media_ptr = medialist->itemAtIndex(0);
                    mediaplayer_.setMedia(*media_ptr);
                    mediaplayer_.play();
                }
                medialist->unlock();
            }
        });
        medias_.push_back( media );
        media.parseRequest(instance_, VLC::Media::ParseFlags::Network, 0);
    }

    void play() {
        assert( !medias_.empty() );

        auto &media = currentMedia();
        mediaplayer_.setMedia(media);
        mediaplayer_.play();
    }

    void setVolume(int volume) noexcept {
        mediaplayer_.setVolume(volume);
    }

    void stop() noexcept {
        mediaplayer_.stopAsync();
    }

    void bindOutputCallbacks(VLC::VideoOutput::Callbacks *h) {
        mediaplayer_.setVideoOutputGLCallbacks(
            [h](const libvlc_video_setup_device_cfg_t *cfg, libvlc_video_setup_device_info_t *out) {
                return h->onSetup(cfg, out);
            },
            [h]() { h->onCleanup(); },
            nullptr,
            [h](const libvlc_video_render_cfg_t *cfg, libvlc_video_output_cfg_t *out) { return h->onUpdateOutput(cfg, out); },
            [h]() { h->onSwap(); },
            [h](bool enter) { return h->onMakeCurrent(enter); },
            [h](char const* funcname) { return h->onGetProcAddress(funcname); }
        );
    }

    VLC::Media& currentMedia() {
        return medias_.at(current_media_id_);
    }

 private:
    VLC::Instance instance_;
    VLC::MediaPlayer mediaplayer_;
    std::vector<VLC::Media> medias_;
    int current_media_id_;
};

// ----------------------------------------------------------------------------

GLuint createShaderProgram() {
    GLuint vertexShader = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vertexShader, 1, &vertexShaderSrc, nullptr);
    glCompileShader(vertexShader);

    GLuint fragmentShader = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fragmentShader, 1, &fragmentShaderSrc, nullptr);
    glCompileShader(fragmentShader);

    GLuint program = glCreateProgram();
    glAttachShader(program, vertexShader);
    glAttachShader(program, fragmentShader);
    glLinkProgram(program);

    glDeleteShader(vertexShader);
    glDeleteShader(fragmentShader);

    CHECK_GL_ERRORS();

    return program;
}

// ----------------------------------------------------------------------------

int main(int argc, char *argv[]) {
    if (!glfwInit()) {
        std::cerr << "Failed to initialize GLFW" << std::endl;
        return EXIT_FAILURE;
    }

    // Create a main OpenGL es3 context for display.
#if USE_OPENGL_COMPATIBLE
    glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_API);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
    // **must** be in compatible profile to work due to VLC.
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_COMPAT_PROFILE);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
#else
    glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_ES_API);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
#endif

    glfwWindowHint(GLFW_VISIBLE, GLFW_TRUE);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);
    glfwWindowHint(GLFW_DOUBLEBUFFER, GLFW_TRUE);
    GLFWwindow* window = glfwCreateWindow(kWidth, kHeight, "OpenGL x VLC", nullptr, nullptr);
    if (!window) {
        const char* description;
        glfwGetError(&description);
        std::cerr << "Failed to create window: " << description << std::endl;
        glfwTerminate();
        return EXIT_FAILURE;
    }

    // Create a shared context which will be use by the VLC thread.
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    glfwWindowHint(GLFW_DOUBLEBUFFER, GLFW_FALSE);
    GLFWwindow* shared_ctx = glfwCreateWindow(4, 4, "Shared Context", nullptr, window);
    if (!shared_ctx) {
        std::cerr << "Failed to create the shared context." << std::endl;
        glfwDestroyWindow(window);
        glfwTerminate();
        return EXIT_FAILURE;
    }
    glfwMakeContextCurrent(shared_ctx);
    glfwSwapInterval(0);

    // Switch back to the display context to setup objects.
    glfwMakeContextCurrent(window);

    // Enable v-sync on the main thread's rendering-context to prevent stalling.
    glfwSwapInterval(1);

#if USE_OPENGL_COMPATIBLE
    // Initialize GLEW
    GLenum err = glewInit();
    if (err != GLEW_OK) {
        std::cerr << "Failed to initialize GLEW: " << glewGetErrorString(err) << std::endl;
        glfwDestroyWindow(window);
        glfwTerminate();
        return EXIT_FAILURE;
    }
    glGetError();
#endif

    GLuint program(0u), VAO(0u), VBO(0u), IBO(0u);
    {
        program = createShaderProgram();
        glUseProgram(program);

        glGenVertexArrays(1, &VAO);
        glGenBuffers(1, &VBO);
        glGenBuffers(1, &IBO);

        glBindVertexArray(VAO);

        glBindBuffer(GL_ARRAY_BUFFER, VBO);
        glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);

        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, IBO);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);

        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(GLfloat), nullptr);
        glEnableVertexAttribArray(0);

        glClearColor(0.4, 0.9, 0.5, 1.0);

        CHECK_GL_ERRORS();
    }

    // Create a VLC mini player.
    VLCPlayer *vlc = new VLCPlayer({
        "--no-xlib",
        "--video",
        "--audio",
        // "--no-osd",
        "--hw-dec",
        ":demux=h264", "--h264-fps=30",
        ":demux=hevc", "--hevc-fps=30",
        "--file-caching=300",
        "--network-caching=1000",
        "--fps-fps=60",
        "--quiet",
        // "-vv",
    });

    // Create a custom frame capture and bind it to the VLC player.
    auto frameCapture = new FrameCapture(shared_ctx);
    vlc->bindOutputCallbacks(frameCapture);

    // Launch a media.
    vlc->addMedia((argc > 1) ? argv[1] : kVideoURI);
    vlc->play();

    // Mainloop
    while (!glfwWindowShouldClose(window)) {
        glClear(GL_COLOR_BUFFER_BIT);

        // main rendering.
        {
            glUseProgram(program);
            glBindVertexArray(VAO);

            // Retrieve next frame from video and render it.
            if (auto frame_texture_id = frameCapture->getNextFrame(); frame_texture_id > 0) {
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, frame_texture_id);
                glUniform1i(glGetUniformLocation(program, "uTexture"), 0);
            }
            glDrawElements(GL_TRIANGLES, std::size(indices), GL_UNSIGNED_INT, 0);

            glBindVertexArray(0u);
            glUseProgram(0u);
        }
        CHECK_GL_ERRORS();

        glfwSwapBuffers(window);
        glfwPollEvents();
    }
    delete vlc;
    delete frameCapture;

    glDeleteVertexArrays(1, &VAO);
    glDeleteBuffers(1, &VBO);
    glDeleteBuffers(1, &IBO);
    glDeleteProgram(program);

    glfwMakeContextCurrent(window);
    glfwDestroyWindow(window);

    glfwMakeContextCurrent(shared_ctx);
    glfwDestroyWindow(shared_ctx);

    glfwTerminate();

    return EXIT_SUCCESS;
}

// ----------------------------------------------------------------------------