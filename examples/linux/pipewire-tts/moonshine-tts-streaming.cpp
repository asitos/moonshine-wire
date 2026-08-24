// moonshine-tts-streaming.cpp
// Ultra-low-latency real-time TTS streaming for robotics.

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/utils/ringbuffer.h>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <poll.h>

#include <atomic>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <cmath>

#include "moonshine-cpp.h"

static std::atomic<bool> g_shutdown{false};
static void handleSignal(int) { g_shutdown.store(true, std::memory_order_relaxed); }

static uint64_t now_ms() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

template <typename T>
class ThreadSafeQueue {
 public:
  void push(T item) {
    {
      std::lock_guard<std::mutex> lk(mu_);
      q_.push_back(std::move(item));
    }
    cv_.notify_one();
  }

  bool pop(T &out) {
    std::unique_lock<std::mutex> lk(mu_);
    cv_.wait(lk, [this] { return !q_.empty() || g_shutdown.load(); });
    if (q_.empty()) return false;
    out = std::move(q_.front());
    q_.pop_front();
    return true;
  }

  void clear() {
    std::lock_guard<std::mutex> lk(mu_);
    q_.clear();
  }

  void notifyShutdown() { cv_.notify_all(); }

 private:
  std::mutex mu_;
  std::condition_variable cv_;
  std::deque<T> q_;
};

class AudioRing {
 public:
  static constexpr uint32_t kCapacity = 1u << 17;

  AudioRing() {
    spa_ringbuffer_init(&rb_);
    buf_.resize(kCapacity, 0.0f);
  }

  void write(const float *src, uint32_t n_frames) {
    uint32_t written = 0;
    while (written < n_frames) {
      if (clear_req_.load(std::memory_order_acquire)) {
        return; // Abort write on clear
      }
      uint32_t index;
      int32_t space = static_cast<int32_t>(kCapacity) - static_cast<int32_t>(spa_ringbuffer_get_write_index(&rb_, &index));
      if (space <= 0) {
        if (g_shutdown.load(std::memory_order_relaxed)) return;
        struct timespec ts = {0, 500000};
        nanosleep(&ts, nullptr);
        continue;
      }
      uint32_t chunk = std::min(static_cast<uint32_t>(space), n_frames - written);
      uint32_t pos  = index & (kCapacity - 1);
      uint32_t tail = kCapacity - pos;
      if (chunk <= tail) {
        std::memcpy(&buf_[pos], src + written, chunk * sizeof(float));
      } else {
        std::memcpy(&buf_[pos], src + written, tail * sizeof(float));
        std::memcpy(&buf_[0], src + written + tail, (chunk - tail) * sizeof(float));
      }
      spa_ringbuffer_write_update(&rb_, index + chunk);
      written += chunk;
    }
  }

  void read(float *dst, uint32_t n_frames) {
    if (clear_req_.load(std::memory_order_acquire)) {
      uint32_t w_index;
      spa_ringbuffer_get_write_index(&rb_, &w_index);
      spa_ringbuffer_read_update(&rb_, w_index);
      clear_req_.store(false, std::memory_order_release);
      std::memset(dst, 0, n_frames * sizeof(float));
      return;
    }

    uint32_t index;
    int32_t avail = spa_ringbuffer_get_read_index(&rb_, &index);
    uint32_t readable = (avail > 0) ? static_cast<uint32_t>(avail) : 0;
    uint32_t chunk = std::min(readable, n_frames);
    
    if (chunk > 0) {
      uint32_t pos  = index & (kCapacity - 1);
      uint32_t tail = kCapacity - pos;
      if (chunk <= tail) {
        std::memcpy(dst, &buf_[pos], chunk * sizeof(float));
      } else {
        std::memcpy(dst, &buf_[pos], tail * sizeof(float));
        std::memcpy(dst + tail, &buf_[0], (chunk - tail) * sizeof(float));
      }
      spa_ringbuffer_read_update(&rb_, index + chunk);
    }
    if (chunk < n_frames) {
      std::memset(dst + chunk, 0, (n_frames - chunk) * sizeof(float));
    }
  }

  void clear() {
    clear_req_.store(true, std::memory_order_release);
  }

 private:
  spa_ringbuffer rb_;
  std::vector<float> buf_;
  std::atomic<bool> clear_req_{false};
};

class PipeWireSink {
 public:
  PipeWireSink(AudioRing &ring, uint32_t sample_rate)
      : ring_(ring), sample_rate_(sample_rate) {
    loop_ = pw_main_loop_new(nullptr);
    if (!loop_) throw std::runtime_error("pw_main_loop_new failed");

    pw_loop *pw_loop_ptr = pw_main_loop_get_loop(loop_);
    stream_ = pw_stream_new_simple(
        pw_loop_ptr, "moonshine-tts-streaming",
        pw_properties_new(
            PW_KEY_MEDIA_TYPE, "Audio",
            PW_KEY_MEDIA_CATEGORY, "Playback",
            PW_KEY_MEDIA_ROLE, "Music",
            PW_KEY_NODE_LATENCY, "256/24000", // Low latency quantum
            nullptr),
        &kStreamEvents, this);
    if (!stream_) throw std::runtime_error("pw_stream_new_simple failed");

    uint8_t buffer[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));

    struct spa_audio_info_raw info = {};
    info.format   = SPA_AUDIO_FORMAT_F32;
    info.rate     = sample_rate_;
    info.channels = 1;

    const struct spa_pod *params[1];
    params[0] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &info);

    int ret = pw_stream_connect(
        stream_, PW_DIRECTION_OUTPUT, PW_ID_ANY,
        static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT |
                                     PW_STREAM_FLAG_MAP_BUFFERS |
                                     PW_STREAM_FLAG_RT_PROCESS),
        params, 1);
    if (ret < 0) throw std::runtime_error("pw_stream_connect failed");

    loop_thread_ = std::thread([this] { pw_main_loop_run(loop_); });
  }

  ~PipeWireSink() {
    if (loop_) pw_main_loop_quit(loop_);
    if (loop_thread_.joinable()) loop_thread_.join();
    if (stream_) pw_stream_destroy(stream_);
    if (loop_) pw_main_loop_destroy(loop_);
  }

 private:
  static void onProcess(void *userdata) {
    auto *self = static_cast<PipeWireSink *>(userdata);
    struct pw_buffer *b = pw_stream_dequeue_buffer(self->stream_);
    if (!b) return;

    struct spa_buffer *buf = b->buffer;
    float *dst = static_cast<float *>(buf->datas[0].data);
    if (!dst) {
      pw_stream_queue_buffer(self->stream_, b);
      return;
    }

    uint32_t stride = sizeof(float);
    uint32_t n_frames = buf->datas[0].maxsize / stride;
    if (b->requested > 0 && static_cast<uint32_t>(b->requested) < n_frames)
      n_frames = static_cast<uint32_t>(b->requested);

    self->ring_.read(dst, n_frames);

    buf->datas[0].chunk->offset = 0;
    buf->datas[0].chunk->stride = static_cast<int32_t>(stride);
    buf->datas[0].chunk->size   = n_frames * stride;
    pw_stream_queue_buffer(self->stream_, b);
  }
  static const struct pw_stream_events kStreamEvents;

  AudioRing &ring_;
  uint32_t sample_rate_;
  pw_main_loop *loop_ = nullptr;
  pw_stream *stream_ = nullptr;
  std::thread loop_thread_;
};

const struct pw_stream_events PipeWireSink::kStreamEvents = {
    .version = PW_VERSION_STREAM_EVENTS,
    .process = PipeWireSink::onProcess,
};

struct SocketServer {
  int fd = -1;
  std::string path;

  void bind_and_listen(const std::string& socket_path) {
    if (socket_path.size() >= sizeof(sockaddr_un::sun_path)) {
      throw std::runtime_error("Socket path too long: " + socket_path);
    }
    if (::unlink(socket_path.c_str()) != 0 && errno != ENOENT) {
      std::cerr << "Failed to unlink stale socket: " << std::strerror(errno) << "\n";
      std::exit(1);
    }
    int sock_fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (sock_fd < 0) {
      throw std::runtime_error(std::string("socket() failed: ") + std::strerror(errno));
    }
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    socket_path.copy(addr.sun_path, socket_path.size());
    addr.sun_path[socket_path.size()] = '\0';
    if (::bind(sock_fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(sa_family_t) + socket_path.size() + 1) != 0) {
      ::close(sock_fd);
      throw std::runtime_error(std::string("bind() failed: ") + std::strerror(errno));
    }
    if (::listen(sock_fd, 8) != 0) {
      ::close(sock_fd);
      throw std::runtime_error(std::string("listen() failed: ") + std::strerror(errno));
    }
    fd = sock_fd;
    path = socket_path;
  }

  void close_and_unlink() {
    if (fd >= 0) {
      ::close(fd);
      fd = -1;
    }
    if (!path.empty()) {
      ::unlink(path.c_str());
      path.clear();
    }
  }

  ~SocketServer() { close_and_unlink(); }
};

class LineSocketServer {
 public:
  explicit LineSocketServer(const std::string &path) {
    server_.bind_and_listen(path);
    int flags = ::fcntl(server_.fd, F_GETFL, 0);
    ::fcntl(server_.fd, F_SETFL, flags | O_NONBLOCK);
    accept_thread_ = std::thread(&LineSocketServer::acceptLoop, this);
  }
  ~LineSocketServer() {
    g_shutdown.store(true);
    if (accept_thread_.joinable()) accept_thread_.join();
  }
  void broadcast(const std::string &line) {
    std::string msg = line + "\n";
    std::lock_guard<std::mutex> lk(clients_mu_);
    auto it = client_fds_.begin();
    while (it != client_fds_.end()) {
      ssize_t sent = ::send(*it, msg.data(), msg.size(), MSG_NOSIGNAL);
      if (sent <= 0) { ::close(*it); it = client_fds_.erase(it); }
      else { ++it; }
    }
  }
 private:
  void acceptLoop() {
    while (!g_shutdown.load()) {
      int client_fd = ::accept4(server_.fd, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
      if (client_fd >= 0) {
        int flags = ::fcntl(client_fd, F_GETFL, 0);
        ::fcntl(client_fd, F_SETFL, flags & ~O_NONBLOCK); // Blocking sends
        std::lock_guard<std::mutex> lk(clients_mu_);
        client_fds_.push_back(client_fd);
      } else {
        struct pollfd pfd = {server_.fd, POLLIN, 0};
        ::poll(&pfd, 1, 100);
      }
    }
  }
  SocketServer server_;
  std::mutex clients_mu_;
  std::vector<int> client_fds_;
  std::thread accept_thread_;
};

struct TextChunk {
  std::string text;
  uint64_t enqueue_time;
};

class TextSocketServer {
 public:
  TextSocketServer(const std::string &path, ThreadSafeQueue<TextChunk> &queue, AudioRing &ring)
      : queue_(queue), ring_(ring) {
    server_.bind_and_listen(path);
    listen_thread_ = std::thread(&TextSocketServer::listenLoop, this);
  }
  ~TextSocketServer() {
    if (listen_thread_.joinable()) listen_thread_.join();
    for (auto &t : client_threads_) if (t.joinable()) t.join();
  }

 private:
  void listenLoop() {
    while (!g_shutdown.load()) {
      struct pollfd pfd = {server_.fd, POLLIN, 0};
      if (::poll(&pfd, 1, 200) > 0) {
        int client_fd = ::accept4(server_.fd, nullptr, nullptr, SOCK_CLOEXEC);
        if (client_fd >= 0) {
          std::lock_guard<std::mutex> lk(client_threads_mu_);
          client_threads_.emplace_back([this, client_fd] { clientLoop(client_fd); });
        }
      }
    }
  }

  void clientLoop(int fd) {
    char tmp[1024];
    std::string current_chunk;
    while (!g_shutdown.load()) {
      struct pollfd pfd = {fd, POLLIN, 0};
      if (::poll(&pfd, 1, 200) <= 0) continue;

      ssize_t n = ::recv(fd, tmp, sizeof(tmp), 0);
      if (n <= 0) break;

      for (ssize_t i = 0; i < n; ++i) {
        char c = tmp[i];
        if (c == '\x18') { // ASCII CAN (Cancel) -> Preempt!
          queue_.clear();
          ring_.clear();
          current_chunk.clear();
          std::cout << "[IPC] Received CANCEL signal, flushed queues.\n";
          continue;
        }

        current_chunk += c;
        // Adaptive chunking: Flush on punctuation or newline, or space if too long
        bool is_punct = (c == '\n' || c == '.' || c == '?' || c == '!' || c == ';' || c == ':');
        bool is_long_space = (c == ' ' && current_chunk.size() > 60);
        
        if (is_punct || is_long_space) {
          if (current_chunk.size() > 2) { // Avoid trivial chunks
            queue_.push({current_chunk, now_ms()});
            current_chunk.clear();
          }
        }
      }
    }
    ::close(fd);
  }

  SocketServer server_;
  ThreadSafeQueue<TextChunk> &queue_;
  AudioRing &ring_;
  std::mutex client_threads_mu_;
  std::vector<std::thread> client_threads_;
  std::thread listen_thread_;
};

class SynthesisWorker {
 public:
  SynthesisWorker(moonshine::TextToSpeech &tts,
                  moonshine::GraphemeToPhonemizer &g2p,
                  ThreadSafeQueue<TextChunk> &queue,
                  AudioRing &ring,
                  LineSocketServer &phoneme_server)
      : tts_(tts), g2p_(g2p), queue_(queue), ring_(ring), phoneme_server_(phoneme_server) {
    thread_ = std::thread(&SynthesisWorker::run, this);
  }
  ~SynthesisWorker() {
    queue_.notifyShutdown();
    if (thread_.joinable()) thread_.join();
  }

 private:
  void run() {
    while (true) {
      TextChunk chunk;
      if (!queue_.pop(chunk)) break;

      uint64_t t_start = now_ms();
      
      // 1. Phoneme generation
      std::string ipa = g2p_.toIpa(chunk.text);
      phoneme_server_.broadcast(ipa);
      uint64_t t_phoneme = now_ms();

      // 2. TTS inference
      moonshine::TtsSynthesisResult res = tts_.synthesizeFromPhonemes(ipa);
      uint64_t t_infer = now_ms();

      // 3. Audio buffering
      ring_.write(res.samples.data(), res.samples.size());
      uint64_t t_audio = now_ms();

      std::cout << "[TTFA] Text queued -> first PCM out: " << (t_audio - chunk.enqueue_time) << "ms "
                << "(G2P: " << (t_phoneme - t_start) << "ms, TTS: " << (t_infer - t_phoneme) << "ms)\n";
    }
  }

  moonshine::TextToSpeech &tts_;
  moonshine::GraphemeToPhonemizer &g2p_;
  ThreadSafeQueue<TextChunk> &queue_;
  AudioRing &ring_;
  LineSocketServer &phoneme_server_;
  std::thread thread_;
};

static void usage(const char* prog) {
  std::cerr << "Usage: " << prog << " [OPTIONS]\n"
            << "Daemon options:\n"
            << "  --input-socket PATH        (default: /tmp/moonshine-tts.sock)\n"
            << "  --phoneme-socket PATH      (default: /tmp/moonshine-phonemes.sock)\n"
            << "TTS options:\n"
            << "  --model-root DIR           (default: .)\n"
            << "  --lang LANG                (default: en_us)\n"
            << "  --voice ID\n"
            << "  --speed N\n";
}

int main(int argc, char *argv[]) {
  std::string input_sock = "/tmp/moonshine-tts.sock";
  std::string phoneme_sock = "/tmp/moonshine-phonemes.sock";
  std::string lang = "en_us";
  
  std::vector<std::pair<std::string, std::string>> pairs;
  for (int i = 1; i < argc;) {
    std::string a = argv[i];
    if (a == "-h" || a == "--help") { usage(argv[0]); return 0; }
    if (a.rfind("--", 0) == 0 && i + 1 < argc) {
      pairs.emplace_back(a.substr(2), argv[i + 1]);
      i += 2;
    } else {
      std::cerr << "Invalid argument: " << a << "\n";
      usage(argv[0]);
      return 2;
    }
  }

  std::vector<std::pair<std::string, std::string>> tts_pairs;
  for (const auto& [k, v] : pairs) {
    if (k == "input-socket") input_sock = v;
    else if (k == "phoneme-socket") phoneme_sock = v;
    else tts_pairs.emplace_back(k, v);
  }

  
  struct sigaction sa = {};
  sa.sa_handler = handleSignal;
  ::sigaction(SIGINT,  &sa, nullptr);
  ::sigaction(SIGTERM, &sa, nullptr);
  ::signal(SIGPIPE, SIG_IGN);

  pw_init(nullptr, nullptr);

  try {
    std::cout << "[Init] Loading Moonshine TTS engine...\n";
    moonshine::TextToSpeech tts(lang, tts_pairs);
    moonshine::GraphemeToPhonemizer g2p(lang, tts_pairs);

    std::cout << "[Init] Warming up model...\n";
    moonshine::TtsSynthesisResult warmup = tts.synthesizeFromPhonemes(g2p.toIpa("hello"));

    AudioRing ring;
    PipeWireSink pw_sink(ring, warmup.sampleRateHz);
    LineSocketServer phoneme_server(phoneme_sock);
    ThreadSafeQueue<TextChunk> text_queue;
    TextSocketServer text_server(input_sock, text_queue, ring);
    SynthesisWorker worker(tts, g2p, text_queue, ring, phoneme_server);

    std::cout << "[Ready] Listening on " << input_sock << "\n";
    while (!g_shutdown.load()) {
      struct timespec ts = {0, 50000000};
      nanosleep(&ts, nullptr);
    }
  } catch (const std::exception &e) {
    std::cerr << "Fatal error: " << e.what() << "\n";
    pw_deinit();
    return 1;
  }

  pw_deinit();
  return 0;
}
