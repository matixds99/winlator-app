#include "gamemusic.h"
#include "gothic.h"

#include <Tempest/Sound>
#include <Tempest/Log>

#include "game/definitions/musicdefinitions.h"
#include "dmusic/mixer.h"
#include "resources.h"
#include "dmusic.h"
#include <algorithm>
#include <atomic>
#include <cstring>
#include <cstdio>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#if defined(__ANDROID__)
#include <sys/resource.h>
#include <unistd.h>
#endif
#ifdef OPENGOTHIC_RECORDED_MUSIC
#define OV_EXCLUDE_STATIC_CALLBACKS
#include <minivorbis/minivorbis.h>
#endif

using namespace Tempest;

// NB fix 37: renderSound is called on the mixer thread of OpenAL, which mixes the music and all the effects. On Android
// OpenAL can not give that thread a real-time priority (SCHED_RR is not allowed for an app), so it ran at the usual
// priority and during the loading of the world the workers took the processor from it - the sound stopped for a moment.
// The first call on that thread raises its priority to the one Android gives to audio (THREAD_PRIORITY_AUDIO = -16).
static void boostAudioThread() {
#if defined(__ANDROID__)
  static thread_local bool done = false;
  if(done)
    return;
  done = true;
  if(setpriority(PRIO_PROCESS, gettid(), -16)!=0)
    setpriority(PRIO_PROCESS, gettid(), -10);
#endif
  }

static constexpr uint16_t SAMPLE_RATE = 44100;

struct GameMusic::RecordedMusic : Tempest::SoundProducer {
#ifdef OPENGOTHIC_RECORDED_MUSIC
  struct Source {
    std::vector<uint8_t> bytes;
    size_t offset = 0;
    OggVorbis_File file = {};
    bool opened = false;
    int rate = 0, channels = 0;
    ~Source() { if(opened) ov_clear(&file); }
    explicit Source(const std::string& path) {
      // Resolve the KM directory explicitly: basename-only searches can pick
      // a similarly named ambient sound from a different VDF directory.
      auto* entry = Resources::vdfsIndex().resolve(path);
      if(entry==nullptr)
        throw std::runtime_error("Music file not found: "+path);
      auto input = entry->open_read();
      if(input==nullptr)
        throw std::runtime_error("Cannot read OGG music: "+path);
      input->seek(0,zenkit::Whence::END);
      bytes.resize(input->tell());
      input->seek(0,zenkit::Whence::BEG);
      if(input->read(bytes.data(),bytes.size())!=bytes.size())
        throw std::runtime_error("Incomplete OGG music: "+path);
      ov_callbacks cb = {};
      cb.read_func = [](void* dst, size_t size, size_t n, void* p) -> size_t {
        auto& s = *static_cast<Source*>(p);
        if(size==0) return 0;
        const size_t count = std::min(n,(s.bytes.size()-s.offset)/size);
        std::memcpy(dst,s.bytes.data()+s.offset,count*size);
        s.offset += count*size;
        return count;
        };
      cb.seek_func = [](void* p, ogg_int64_t offset, int mode) -> int {
        auto& s = *static_cast<Source*>(p);
        const int64_t base = mode==SEEK_SET ? 0 : mode==SEEK_CUR ? int64_t(s.offset) : mode==SEEK_END ? int64_t(s.bytes.size()) : -1;
        if(base<0 || offset < -base || offset>int64_t(s.bytes.size())-base) return -1;
        s.offset = size_t(base+offset);
        return 0;
        };
      cb.tell_func = [](void* p) -> long { return long(static_cast<Source*>(p)->offset); };
      if(ov_open_callbacks(this,&file,nullptr,0,cb)!=0)
        throw std::runtime_error("Invalid OGG music: "+path);
      opened = true;
      const auto* info = ov_info(&file,-1);
      if(info==nullptr) {
        ov_clear(&file);
        opened = false;
        throw std::runtime_error("Missing OGG music header: "+path);
        }
      rate = int(info->rate);
      channels = info->channels;
      if(rate<=0 || rate>65535 || (channels!=1 && channels!=2)) {
        ov_clear(&file);
        opened = false;
        throw std::runtime_error("Unsupported OGG music format: "+path);
        }
      uint32_t fingerprint = 2166136261u;
      for(const auto byte:bytes)
        fingerprint = (fingerprint^byte)*16777619u;
      Log::i("CoM music data: ",path," bytes=",bytes.size()," fnv1a32=",fingerprint,
             " rate=",rate," channels=",channels);
      }
    };
  std::unique_ptr<Source> source;
  double loopStart = 0;
  float gain = 0.f, target = 1.f;
  uint64_t ramp = 0;
  std::atomic<int32_t> fadeOut{-1};

  RecordedMusic(std::unique_ptr<Source> input, int loopMs, int fadeMs)
    : SoundProducer(uint16_t(input->rate),2), source(std::move(input)) {
    const double duration = ov_time_total(&source->file,-1);
    loopStart = double(loopMs)/1000.0;
    if(loopStart<0 || loopStart>=duration)
      loopStart = 0;
    ramp = uint64_t(std::max(fadeMs,0))*uint64_t(source->rate)/1000;
    if(ramp==0) gain = 1.f;
    }

  void renderSound(int16_t* out, size_t frames) override {
    boostAudioThread();
    if(const int ms = fadeOut.exchange(-1); ms>=0) {
      target = 0.f;
      ramp = std::max(uint64_t(1),uint64_t(ms)*uint64_t(source->rate)/1000);
      }
    size_t done = 0;
    bool sought = false;
    while(done<frames) {
      int16_t buffer[2048];
      const size_t wanted = std::min(frames-done,size_t(1024));
      int section = 0;
      const long bytes = ov_read(&source->file,reinterpret_cast<char*>(buffer),int(wanted*size_t(source->channels)*2),0,2,1,&section);
      if(bytes<=0) {
        if(bytes<0 || sought || ov_time_seek(&source->file,loopStart)!=0) {
          std::fill(out+done*2,out+frames*2,int16_t(0));
          return;
          }
        sought = true;
        continue;
        }
      sought = false;
      const size_t count = size_t(bytes)/(size_t(source->channels)*2);
      if(count==0) {
        std::fill(out+done*2,out+frames*2,int16_t(0));
        return;
        }
      for(size_t i=0; i<count; ++i) {
        if(ramp>0) {
          gain += (target-gain)/float(ramp);
          --ramp;
          }
        const size_t at = i*size_t(source->channels);
        out[(done+i)*2]   = int16_t(float(buffer[at])*gain);
        out[(done+i)*2+1] = int16_t(float(buffer[at+(source->channels==2 ? 1 : 0)])*gain);
        }
      done += count;
      }
    }
#else
  RecordedMusic() : SoundProducer(SAMPLE_RATE,2) {}
  void renderSound(int16_t* out, size_t frames) override { boostAudioThread(); std::fill(out,out+frames*2,int16_t(0)); }
#endif
  };

// NB fix 38: the music of a theme is loaded on a thread of its own. Loading it needs the lock of the resources, which the
// loading of the world holds for a long time: in the callback of OpenAL (fix 36 before) the whole sound waited, on the
// thread of the game the loading screen would wait. The callback starts the new theme when it is loaded.
struct ThemeLoader {
  ~ThemeLoader() { finish(); }

  void start(std::function<void(const std::string&)> fn) {
    load = std::move(fn);
    th   = std::thread([this]{ run(); });
    }

  void finish() {
    {
      std::lock_guard<std::mutex> g(sync);
      stop = true;
    }
    cv.notify_all();
    if(th.joinable())
      th.join();
    }

  void request(const std::string& file) {
    {
      std::lock_guard<std::mutex> g(sync);
      if(want==file || busy==file)
        return;
      want = file;
    }
    cv.notify_one();
    }

  bool isLoading(const std::string& file) {
    std::lock_guard<std::mutex> g(sync);
    return want==file || busy==file;
    }

  private:
    void run() {
      std::unique_lock<std::mutex> l(sync);
      while(true) {
        cv.wait(l,[this]{ return stop || !want.empty(); });
        if(stop)
          return;
        busy = std::move(want);
        want.clear();
        const std::string file = busy;
        l.unlock();
        load(file);     // stores the result before busy is cleared: isReady sees one or the other
        l.lock();
        busy.clear();
        }
      }

    std::function<void(const std::string&)> load;
    std::mutex              sync;
    std::condition_variable cv;
    std::string             want, busy;
    bool                    stop = false;
    std::thread             th;
  };

struct GameMusic::MusicProvider : Tempest::SoundProducer {
  using Tempest::SoundProducer::SoundProducer;

  public:
    virtual void stopTheme() {
      std::lock_guard<std::recursive_mutex> guard(pendingSync);
      pendingMusic.reset();
      }

    void setEnabled(bool b) {
      if(enable == b)
        return;

      std::lock_guard<std::recursive_mutex> guard(pendingSync);
      if(b) {
        hasPending = true;
        reloadTheme = true;
        enable.store(true);
        } else {
        enable.store(false);
        stopTheme();
        }
      }

    bool isEnabled() const {
      return enable.load();
      }

    void playTheme(const zenkit::IMusicTheme &theme, GameMusic::Tags tags) {
      // NB fix 36/38: the music of the theme is loaded ahead, on the thread of the loader (see ThemeLoader)
      preload(theme);
      std::lock_guard<std::recursive_mutex> guard(pendingSync);
      reloadTheme  = !pendingMusic || pendingMusic->file != theme.file;
      pendingMusic = theme;
      pendingTags  = tags;
      hasPending   = true;
      }

  protected:
    virtual void preload(const zenkit::IMusicTheme& /*theme*/) {}
    // false: the music of the theme is still being loaded - the theme stays pending, the callback asks again (fix 38)
    virtual bool isReady(const std::string& /*file*/) { return true; }

    bool updateTheme(zenkit::IMusicTheme& theme, Tags& tags) {
      bool reloadTheme = false;

      std::lock_guard<std::recursive_mutex> guard(pendingSync);
      if(hasPending && pendingMusic && enable.load()) {
        if(this->reloadTheme && !isReady(pendingMusic->file))
          return false;
        hasPending  = false;
        reloadTheme = this->reloadTheme;
        theme       = this->pendingMusic.value();
        tags        = this->pendingTags;
        }

      this->reloadTheme = false;
      return reloadTheme;
      }

    static zenkit::MusicTransitionEffect computeTransitionEffect(Tags nextTags, Tags currTags, zenkit::MusicTransitionEffect transtype) {
      const int cur  = currTags & (Tags::Std | Tags::Fgt | Tags::Thr);
      const int next = nextTags & (Tags::Std | Tags::Fgt | Tags::Thr);

      //zenkit::MusicTransitionEffect embellishment = transtype;
      zenkit::MusicTransitionEffect embellishment = zenkit::MusicTransitionEffect::NONE;
      if(next == Tags::Std) {
        if(cur != Tags::Std)
          embellishment = zenkit::MusicTransitionEffect::BREAK;
        }
      else if(next == Tags::Fgt) {
        if(cur == Tags::Thr)
          embellishment = zenkit::MusicTransitionEffect::FILL;
        }
      else if(next == Tags::Thr) {
        if(cur == Tags::Fgt)
          embellishment = zenkit::MusicTransitionEffect::NONE;
        }

      return embellishment;
      }

  private:
    std::atomic_bool     enable{true};

    std::recursive_mutex pendingSync;
    bool                 hasPending  = false;
    bool                 reloadTheme = false;
    Tags                 pendingTags = Tags::Day;
    std::optional<zenkit::IMusicTheme> pendingMusic;

  protected:
    Tags                 currentTags = Tags::Day;
  };

struct GameMusic::OpenGothicMusicProvider : GameMusic::MusicProvider {
  // NB fix 38: the music provider of OpenGothic (the default one, soundProviderIndex=0) loaded the patterns of the theme
  // (Resources::loadDxMusic, under the lock of the resources) in the callback of OpenAL - fix 36 was only for GothicKit
  OpenGothicMusicProvider(uint16_t rate, uint16_t channels) : GameMusic::MusicProvider(rate, channels) {
    loader.start([this](const std::string& file) {
      std::optional<Dx8::PatternList> p;
      try {
        p = Resources::loadDxMusic(file);
        }
      catch(std::exception&) {
        p.reset();
        }
      std::lock_guard<std::mutex> g(preSync);
      prePattern = std::move(p);
      preFile    = file;
      });
    }

  ~OpenGothicMusicProvider() override {
    loader.finish();
    }

  void preload(const zenkit::IMusicTheme& theme) override {
    if(theme.file.empty())
      return;
    {
      std::lock_guard<std::mutex> g(preSync);
      // the loading screen asks for SYS_Loading every frame: not loaded again while it plays
      if(theme.file==playingFile || preFile==theme.file)
        return;
    }
    loader.request(theme.file);
    }

  bool isReady(const std::string& file) override {
    if(file.empty() || loader.isLoading(file))
      return file.empty();
    {
      std::lock_guard<std::mutex> g(preSync);
      if(preFile==file)
        return true;
    }
    loader.request(file);
    return false;
    }

  void renderSound(int16_t *out, size_t n) override {
    boostAudioThread();
    if(!isEnabled()) {
      std::memset(out, 0, n * sizeof(out[0]) * 2);
      return;
      }
    updateTheme();
    mix.mix(out, n);
    }

  void updateTheme() {
    zenkit::IMusicTheme theme;
    Tags                tags;
    if(!GameMusic::MusicProvider::updateTheme(theme, tags))
      return;

    if(theme.file.empty()) {
      stopTheme();
      return;
      }

    try {
      if(/*reloadTheme*/true) {
        std::optional<Dx8::PatternList> pre;
        bool                            loaded = false;
        {
          std::lock_guard<std::mutex> g(preSync);
          if(preFile==theme.file) {
            loaded = true;
            pre    = std::move(prePattern);
            prePattern.reset();
            preFile.clear();
            }
          playingFile = theme.file;
        }
        if(loaded && !pre.has_value())
          throw std::runtime_error("unable to load sound");
        Dx8::PatternList p = pre.has_value() ? std::move(*pre) : Resources::loadDxMusic(theme.file);

        Dx8::Music m;
        m.addPattern(p);

        auto effect = computeTransitionEffect(tags, currentTags, theme.transtype);
        Dx8::DMUS_EMBELLISHT_TYPES em = computeEmbellishment(effect);
        mix.setMusic(m, em);
        currentTags = tags;
        }
      mix.setMusicVolume(theme.vol);
      }
    catch (std::runtime_error &) {
      Log::e("unable to load sound: \"", theme.file, "\"");
      stopTheme();
      }
    catch (std::bad_alloc &) {
      Log::e("out of memory for sound: \"", theme.file, "\"");
      stopTheme();
      }
    }

  void stopTheme() override {
    GameMusic::MusicProvider::stopTheme();
    {
      std::lock_guard<std::mutex> g(preSync);
      playingFile.clear();
    }
    }

  static Dx8::DMUS_EMBELLISHT_TYPES computeEmbellishment(zenkit::MusicTransitionEffect ef) {
    switch (ef) {
      case zenkit::MusicTransitionEffect::UNKNOWN:
      case zenkit::MusicTransitionEffect::NONE:
        return Dx8::DMUS_EMBELLISHT_NORMAL;
      case zenkit::MusicTransitionEffect::GROOVE:
        return Dx8::DMUS_EMBELLISHT_NORMAL;
      case zenkit::MusicTransitionEffect::FILL:
        return Dx8::DMUS_EMBELLISHT_FILL;
      case zenkit::MusicTransitionEffect::BREAK:
        return Dx8::DMUS_EMBELLISHT_BREAK;
      case zenkit::MusicTransitionEffect::INTRO:
        return Dx8::DMUS_EMBELLISHT_INTRO;
      case zenkit::MusicTransitionEffect::END:
      case zenkit::MusicTransitionEffect::END_AND_INTO:
        return Dx8::DMUS_EMBELLISHT_END;
      }
    return Dx8::DMUS_EMBELLISHT_NORMAL;
    }

  private:
    Dx8::Mixer mix;
    std::mutex                      preSync;
    std::optional<Dx8::PatternList> prePattern;  // empty with preFile set: the loading failed
    std::string                     preFile;
    std::string                     playingFile;
    ThemeLoader                     loader;
  };

static std::pair<DmTiming, DmEmbellishmentType> computeEmbellishmentAndTiming(const zenkit::MusicTransitionEffect effect, const zenkit::MusicTransitionType type) {
  DmEmbellishmentType embellishment = DmEmbellishment_NONE;
  switch (effect) {
    case zenkit::MusicTransitionEffect::UNKNOWN:
    case zenkit::MusicTransitionEffect::NONE:
      embellishment = DmEmbellishment_NONE;
      break;
    case zenkit::MusicTransitionEffect::GROOVE:
      embellishment = DmEmbellishment_GROOVE;
      break;
    case zenkit::MusicTransitionEffect::FILL:
      embellishment = DmEmbellishment_FILL;
      break;
    case zenkit::MusicTransitionEffect::BREAK:
      embellishment = DmEmbellishment_BREAK;
      break;
    case zenkit::MusicTransitionEffect::INTRO:
      embellishment = DmEmbellishment_INTRO;
      break;
    case zenkit::MusicTransitionEffect::END:
      embellishment = DmEmbellishment_END;
      break;
    case zenkit::MusicTransitionEffect::END_AND_INTO:
      embellishment = DmEmbellishment_END_AND_INTRO;
      break;
    }

  DmTiming timing = DmTiming_MEASURE;
  switch (type) {
    case zenkit::MusicTransitionType::UNKNOWN:
    case zenkit::MusicTransitionType::MEASURE:
      timing = DmTiming_MEASURE;
      break;
    case zenkit::MusicTransitionType::IMMEDIATE:
      timing = DmTiming_INSTANT;
      break;
    case zenkit::MusicTransitionType::BEAT:
      timing = DmTiming_BEAT;
      break;
    }

  return std::make_pair(timing, embellishment);
  }

struct GameMusic::GothicKitMusicProvider : GameMusic::MusicProvider {
  GothicKitMusicProvider(uint16_t rate, uint16_t channels) : GameMusic::MusicProvider(rate, channels) {
    /*
    Dm_setRandomNumberGenerator([](void*) -> uint32_t {
      return 0;
      }, nullptr);
    */

    DmResult rv = DmPerformance_create(&performance, rate);
    if(rv != DmResult_SUCCESS) {
      Log::e("Unable to create DmPerformance object. Out of memory?");
      }
    loader.start([this](const std::string& file) {
      DmSegment* sgt = Resources::loadMusicSegment(file.c_str());
      std::lock_guard<std::mutex> g(preSync);
      if(preSgt!=nullptr)
        DmSegment_release(preSgt);
      preSgt  = sgt;
      preFile = file;
      });
    }

  ~GothicKitMusicProvider() override {
    loader.finish();
    DmPerformance_release(performance);
    if(preSgt!=nullptr)
      DmSegment_release(preSgt);
    }

  void preload(const zenkit::IMusicTheme& theme) override {
    if(theme.file.empty())
      return;
    {
      std::lock_guard<std::mutex> g(preSync);
      // the loading screen asks for SYS_Loading every frame: not loaded again while it plays (NB fix 38)
      if(theme.file==playingFile || preFile==theme.file)
        return;
    }
    loader.request(theme.file);
    }

  bool isReady(const std::string& file) override {
    if(file.empty() || loader.isLoading(file))
      return file.empty();
    {
      std::lock_guard<std::mutex> g(preSync);
      if(preFile==file)
        return true;
    }
    loader.request(file);
    return false;
    }

  void renderSound(int16_t *out, size_t n) override {
    boostAudioThread();
    if(!isEnabled()) {
      std::memset(out, 0, n * sizeof(out[0]) * 2);
      return;
      }
    updateTheme();
    DmPerformance_renderPcm(performance, out, n * 2, DmRenderOptions(DmRender_SHORT | DmRender_STEREO));
    }

  void updateTheme() {
    zenkit::IMusicTheme theme;
    Tags                tags;
    if(!GameMusic::MusicProvider::updateTheme(theme, tags))
      return;

    if(theme.file.empty()) {
      stopTheme();
      return;
      }

    auto effect = computeTransitionEffect(tags, currentTags, theme.transtype);
    auto [timing, embellishment] = computeEmbellishmentAndTiming(effect, theme.transsubtype);

    DmSegment* sgt    = nullptr;
    bool       loaded = false;
    {
      std::lock_guard<std::mutex> g(preSync);
      if(preFile==theme.file) {
        loaded = true;
        sgt    = preSgt;    // the ownership goes to the callback, released below (nullptr: the loading failed)
        preSgt = nullptr;
        preFile.clear();
        }
      playingFile = theme.file;
    }
    if(!loaded)
      sgt = Resources::loadMusicSegment(theme.file.c_str());
    DmResult rv = DmPerformance_playTransition(performance, sgt, embellishment, timing);
    if(rv != DmResult_SUCCESS) {
      Log::e("Failed to play theme: ", theme.file);
      stopTheme();
      }

    DmPerformance_setVolume(performance, theme.vol);
    DmSegment_release(sgt);
    currentTags = tags;
    }

  void stopTheme() override {
    GameMusic::MusicProvider::stopTheme();
    {
      std::lock_guard<std::mutex> g(preSync);
      playingFile.clear();
    }
    DmPerformance_playTransition(performance, nullptr, DmEmbellishment_NONE, DmTiming_INSTANT);
    }

  DmEmbellishmentType computeEmbellishment(Tags nextTags, Tags currTags, zenkit::MusicTransitionEffect transtype) const {
    const int cur  = currTags & (Tags::Std | Tags::Fgt | Tags::Thr);
    const int next = nextTags & (Tags::Std | Tags::Fgt | Tags::Thr);

    DmEmbellishmentType embellishment = DmEmbellishment_NONE;
    switch (transtype) {
      case zenkit::MusicTransitionEffect::UNKNOWN:
      case zenkit::MusicTransitionEffect::NONE:
        embellishment = DmEmbellishment_NONE;
        break;
      case zenkit::MusicTransitionEffect::GROOVE:
        embellishment = DmEmbellishment_GROOVE;
        break;
      case zenkit::MusicTransitionEffect::FILL:
        embellishment = DmEmbellishment_FILL;
        break;
      case zenkit::MusicTransitionEffect::BREAK:
        embellishment = DmEmbellishment_BREAK;
        break;
      case zenkit::MusicTransitionEffect::INTRO:
        embellishment = DmEmbellishment_INTRO;
        break;
      case zenkit::MusicTransitionEffect::END:
        embellishment = DmEmbellishment_END;
        break;
      case zenkit::MusicTransitionEffect::END_AND_INTO:
        embellishment = DmEmbellishment_END_AND_INTRO;
        break;
      }

    if(next == Tags::Std) {
      if(cur != Tags::Std)
        embellishment = DmEmbellishmentType::DmEmbellishment_BREAK;
      }
    else if(next == Tags::Fgt) {
      if(cur == Tags::Thr)
        embellishment = DmEmbellishmentType::DmEmbellishment_FILL;
      }
    else if(next == Tags::Thr) {
      if(cur == Tags::Fgt)
        embellishment = DmEmbellishmentType::DmEmbellishment_NONE;
      }

    return embellishment;
    }

  private:
    DmPerformance *performance = nullptr;
    std::mutex     preSync;
    DmSegment*     preSgt = nullptr;
    std::string    preFile;
    std::string    playingFile;
    ThemeLoader    loader;
  };

static constexpr int PROVIDER_UNINITIALIZED = -1;
static constexpr int PROVIDER_OPENGOTHIC = 0;
// static constexpr int PROVIDER_GOTHICKIT = 1;

GameMusic *GameMusic::instance = nullptr;

GameMusic::GameMusic() {
  instance = this;
  provider = PROVIDER_UNINITIALIZED;

  Gothic::inst().onSettingsChanged.bind(this, &GameMusic::setupSettings);
  setupSettings();
  }

GameMusic::~GameMusic() {
  outgoingSound = SoundEffect();
  recordedSound = SoundEffect();
  sound = SoundEffect();
  instance = nullptr;
  Gothic::inst().onSettingsChanged.ubind(this, &GameMusic::setupSettings);
  }

GameMusic &GameMusic::inst() {
  return *instance;
  }

GameMusic::Tags GameMusic::mkTags(GameMusic::Tags daytime, GameMusic::Tags mode) {
  return Tags(daytime | mode);
  }

void GameMusic::setEnabled(bool e) {
  const bool wasEnabled = enabled;
  enabled = e;
  const bool hasMusic = zoneMusicActive || overrideTheme.has_value();
  impl->setEnabled(e && recordedImpl==nullptr && hasMusic);
  // Stop the OpenAL source as well as the legacy provider. Its streaming
  // buffer may still contain samples prepared before the provider was disabled.
  if(!e || recordedImpl!=nullptr || !hasMusic)
    sound.pause();
  else if(!wasEnabled)
    sound.play();
  if(recordedImpl!=nullptr) {
    if(!e)
      recordedSound.pause();
    else if(!wasEnabled)
      recordedSound.play();
    }
  if(!e)
    outgoingSound.pause();
  }

bool GameMusic::isEnabled() const {
  return enabled;
  }

void GameMusic::setMusic(GameMusic::Music m) {
  const char *clsTheme = "";
  switch (m) {
    case GameMusic::SysLoading:
      clsTheme = "SYS_Loading";
      break;
    }
  if(auto theme = Gothic::musicDef()[clsTheme])
    setMusic(*theme, GameMusic::mkTags(GameMusic::Std, GameMusic::Day));
  }

void GameMusic::setMusic(const zenkit::IMusicTheme &theme, Tags tags) {
  zoneMusicActive = true;
  currentMusic.theme = theme;
  currentMusic.tags  = tags;
  playCurrent();
  }

void GameMusic::clearZoneMusic() {
  zoneMusicActive = false;
  playCurrent();
  }

void GameMusic::overrideTrack(uint32_t instance) {
  if(const auto* track = Gothic::musicDef().recordedTrack(instance)) {
    overrideTheme = track->theme;
    playCurrent();
    }
  else
    Log::e("CoM music: unknown track instance ",instance);
  }

void GameMusic::clearTrackOverride() {
  overrideTheme.reset();
  playCurrent();
  }

void GameMusic::playCurrent() {
  if(!zoneMusicActive && !overrideTheme) {
    recordedSound = SoundEffect();
    outgoingSound = SoundEffect();
    recordedImpl = nullptr;
    recordedFile.clear();
    impl->setEnabled(false);
    sound.pause();
    return;
    }
  const auto& theme = overrideTheme ? *overrideTheme : currentMusic.theme;
  const auto* track = Gothic::musicDef().recordedTrack(theme.file);
  if(track!=nullptr) {
#ifdef OPENGOTHIC_RECORDED_MUSIC
    if(recordedFile==theme.file)
      return;
    try {
      auto input = std::make_unique<RecordedMusic::Source>(theme.file);
      auto producer = std::make_unique<RecordedMusic>(std::move(input),track->loopStart,track->fadeIn);
      auto* implementation = producer.get();
      auto next = device.load(std::move(producer));
      outgoingSound = SoundEffect();
      if(recordedImpl!=nullptr) {
        recordedImpl->fadeOut.store(recordedFadeOut);
        outgoingSound = std::move(recordedSound);
        }
      recordedSound = std::move(next);
      recordedImpl = implementation;
      recordedFile = theme.file;
      recordedFadeOut = track->fadeOut;
      recordedSound.setVolume(Gothic::settingsGetF("SOUND","musicVolume"));
      impl->setEnabled(false);
      sound.pause();
      if(enabled)
        recordedSound.play();
      Log::i("CoM music: playing ",theme.file);
      }
    catch(const std::exception& e) {
      Log::e("CoM music: ",e.what());
      }
#else
    Log::e("CoM OGG music requires audio/Vorbis support");
#endif
    return;
    }
  recordedSound = SoundEffect();
  outgoingSound = SoundEffect();
  recordedImpl = nullptr;
  recordedFile.clear();
  impl->playTheme(theme,currentMusic.tags);
  impl->setEnabled(enabled);
  if(enabled)
    sound.play();
  else
    sound.pause();
  }

void GameMusic::stopMusic() {
  setEnabled(false);
  }

void GameMusic::setupSettings() {
  const int   musicEnabled  = Gothic::settingsGetI("SOUND",    "musicEnabled");
  const float musicVolume   = Gothic::settingsGetF("SOUND",    "musicVolume");
  const int   providerIndex = Gothic::settingsGetI("INTERNAL", "soundProviderIndex");

  if(providerIndex != provider || musicEnabled!=isEnabled()) {
    Log::i("Switching music provider to ", providerIndex == PROVIDER_OPENGOTHIC ? "'OpenGothic'" : "'GothicKit'");
    sound = SoundEffect();

    std::unique_ptr<MusicProvider> p;
    if(providerIndex == PROVIDER_OPENGOTHIC) {
      p = std::make_unique<OpenGothicMusicProvider>(SAMPLE_RATE, 2);
      } else {
      p = std::make_unique<GothicKitMusicProvider>(SAMPLE_RATE, 2);
      }

    provider = providerIndex;
    impl = p.get();
    if(recordedImpl==nullptr)
      impl->playTheme(currentMusic.theme, currentMusic.tags);
    else
      impl->setEnabled(false);

    sound = device.load(std::move(p));
    if(recordedImpl==nullptr)
      sound.play();
    }

  setEnabled(musicEnabled != 0);
  sound.setVolume(musicVolume);
  recordedSound.setVolume(musicVolume);
  outgoingSound.setVolume(musicVolume);
  }
