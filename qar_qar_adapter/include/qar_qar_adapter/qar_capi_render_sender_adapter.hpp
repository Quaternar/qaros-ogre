// Copyright 2026 QuaternAR
// SPDX-License-Identifier: MIT
//
// ===========================================================================
//  QarCApiRenderSenderAdapter / QarCApiSenderFactory
// ===========================================================================
//
//  The production implementation of qar_ogre_streaming's
//  IQarRenderSenderAdapter / IQarRenderSenderFactory seam, backed by the QAR
//  streaming C API (qar_streaming.h). It calls only QAR C functions
//  (qar_render_sender_begin_frame -> per-view pose/FOV; qar_render_sender_frame_
//  opengl + qar_render_sender_show_frame to submit). The session itself belongs
//  to a QarSessionOwner, which joins the QAROS Hub and reports the render targets;
//  the factory borrows its session and builds one sender per target.
//
//  qar_ogre_streaming (the reusable library) has NO knowledge of this class or
//  of the QAR runtime binary — it talks to QAR only through the abstract
//  IQarRenderSenderAdapter. This adapter lives in the producer (host) layer.

#ifndef QAR_QAR_ADAPTER__QAR_CAPI_RENDER_SENDER_ADAPTER_HPP_
#define QAR_QAR_ADAPTER__QAR_CAPI_RENDER_SENDER_ADAPTER_HPP_

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// Warning-suppressed include of the vendored QAR C header, which defines the
// OpenGL producer path natively (via QAR_ENABLE_OPENGL, set in the wrapper).
// types.hpp brings the library-internal GlVideoTexture/GlFrameSync types.
#include "qar_ar_core/common_name.hpp"
#include "qar_ar_core/qar_streaming_c.hpp"
#include "qar_ar_core/pointing_ray.hpp"
#include "qar_ar_core/spatial_frames.hpp"

#include "qar_ogre_streaming/qar_render_sender_adapter.hpp"
#include "qar_ogre_streaming/types.hpp"
#include "qar_qar_adapter/frame_stall_probe.hpp"
#include "qar_qar_adapter/log_time.hpp"
#include "qar_qar_adapter/qar_capi_session_owner.hpp"
#include "qar_qar_adapter/qar_session_owner.hpp"

namespace qar_qar_adapter
{


/// @brief Which side owns the GL textures that carry a submitted frame.
///
/// The SDK's `qar_render_sender_frame_opengl(stream, QarVideoFrameOpenGl*
/// out_frame)` is named `out_frame` and documented as "get frame write access":
/// QAR HANDS the producer texture ids to render/copy into
/// (kRenderIntoQarTextures). The other mode SUPPLIES QAR our own extracted Ogre
/// GL ids (kSupplyOwnTextureIds); against the SDK it cannot work, see
/// @ref kDefaultGlSubmitMode.
enum class GlSubmitMode
{
  kRenderIntoQarTextures,   ///< QAR owns the textures; we copy Ogre color/depth INTO them.
  kSupplyOwnTextureIds      ///< We pass our own Ogre GL ids; QAR samples/reads them.
};

/// @brief Default submit mode: kRenderIntoQarTextures.
///
/// The SDK signature settles the semantics:
///
///     /** @brief Get OpenGL-backed frame write access. */
///     qar_render_sender_frame_opengl(QarRenderSender*, QarVideoFrameOpenGl* out_frame)
///
/// `out_frame` is an OUT parameter: QAR owns the textures and hands us their ids to
/// WRITE INTO. Supplying our own ids therefore cannot work against the SDK —
/// QAR overwrites them: both modes deliver frames with a correct receiver frame
/// counter, and both show a BLACK scene, because in neither case does anything write
/// QAR's textures. Render-into is only complete together with an IQarGlTextureCopyHook
/// (see make_gl_copy_image_hook() and
/// docs/QAROS_INTEGRATION_NOTES.md#render-into-and-the-gl-context).
inline constexpr GlSubmitMode kDefaultGlSubmitMode =
  GlSubmitMode::kRenderIntoQarTextures;

/// @brief Optional seam that performs the GL texture copy for the render-INTO
/// mode without dragging GL/Ogre into this (deliberately GL-free) producer layer.
///
/// In kRenderIntoQarTextures mode the adapter first calls
/// `qar_render_sender_frame_opengl` to obtain the QAR-owned destination texture
/// ids, then asks this hook to copy the rendered Ogre color/depth textures
/// (carried in @p ogre_src) into those QAR ids (@p qar_dst, already populated by
/// QAR). The hook is implemented by the graphics layer that has a current GL
/// context (an FBO blit inside the render lock, see make_gl_copy_image_hook()).
///
/// Render-into is the SDK's semantic (see kDefaultGlSubmitMode), so this hook is
/// REQUIRED — without it QAR's textures are never written and the Visualizer shows
/// a black scene.
class IQarGlTextureCopyHook
{
public:
  virtual ~IQarGlTextureCopyHook() = default;

  /// @return true if every needed view was copied Ogre->QAR successfully.
  virtual bool copy_frame_into(
    const qar_ogre_streaming::QarOpenGlStereoFrame & ogre_src,
    const QarVideoFrameOpenGl & qar_dst) = 0;

  /// @brief The mean colour of the left colour texture of @p ogre_src, each channel 0 to 255,
  /// read back from the GPU. It reads the whole texture, so it is for a diagnostic line about a
  /// single frame and never for every frame. Same thread and GL context as @ref copy_frame_into.
  /// The default cannot read textures back and reports false.
  /// @return true if @p rgb holds the mean colour.
  virtual bool mean_left_colour(
    const qar_ogre_streaming::QarOpenGlStereoFrame & ogre_src, float (& rgb)[3])
  {
    (void)ogre_src;
    rgb[0] = rgb[1] = rgb[2] = 0.0f;
    return false;
  }
};

/// @brief The default render-into copy hook: an FBO blit (glBlitFramebuffer) from
/// our Ogre textures into QAR's destination ids, for all four views (L/R colour,
/// L/R depth).
///
/// Must be invoked on the render thread with Ogre's GL context current — which is
/// where the submit path runs. QAR's texture ids are valid there because QAR is
/// given a context sharing Ogre's object namespace (see capture_gl_context()).
/// Requires GL 3.0+ (framebuffer objects); RViz reports 4.6.
///
/// @return null on platforms without WGL (the adapter then skips the copy).
std::shared_ptr<IQarGlTextureCopyHook> make_gl_copy_image_hook();

/// @brief Opt-in per-frame CSV trace + pose re-read state (QAR_ADAPTER_FRAME_TRACE).
/// Fully defined in the .cpp (Pimpl) so this header stays free of <cstdio> and CSV
/// formatting details; only ever seen through a null-checked unique_ptr.
class FrameTraceState;

/// @brief Opt-in per-stream stage timing for the 3D stereo stream (QAR_VIDEO_TIMING_DIR):
/// the begin-frame wait's not-ready runs and the submit of every frame, as `render3d.csv`.
/// Pimpl for the same reason as @ref FrameTraceState; null when timing is off.
class RenderTimingState;

/// @brief One peer's render sender, implemented against the QAR C API.
class QarCApiRenderSenderAdapter : public qar_ogre_streaming::IQarRenderSenderAdapter
{
public:
  /// @param stream      Owned QarRenderSender handle (via qar_render_sender_create).
  /// @param near_far    Producer-chosen clip planes reported to QAR each show_frame
  ///                    (QAR begin_frame provides pose+FOV only, never near/far).
  /// @param submit_mode See @ref GlSubmitMode; defaults to @ref kDefaultGlSubmitMode.
  /// @param copy_hook   Only used in kRenderIntoQarTextures mode; may be null (then
  ///                    the copy step is skipped and flagged).
  /// @param peer_id     Names the per-frame trace CSV (peer-hex8); may
  ///                    be left default (a directly-constructed/test adapter then
  ///                    falls back to a per-process stream index for the filename).
  /// @param log_folder_path Preferred directory for the trace CSV
  ///                    (QarCApiSenderFactory::Config::log_folder_path); empty falls
  ///                    back to %TEMP%\quaternar, matching the SDK library log default.
  QarCApiRenderSenderAdapter(
    QarRenderSender * stream, const QarNearFar & near_far,
    GlSubmitMode submit_mode = kDefaultGlSubmitMode,
    std::shared_ptr<IQarGlTextureCopyHook> copy_hook = nullptr,
    float depth_scale = 1.0f,
    std::shared_ptr<qar_ar_core::PointingRaySlot> pointing_ray_slot = nullptr,
    QarPeerId peer_id = {},
    std::string log_folder_path = {});
  ~QarCApiRenderSenderAdapter() override;

  QarCApiRenderSenderAdapter(const QarCApiRenderSenderAdapter &) = delete;
  QarCApiRenderSenderAdapter & operator=(const QarCApiRenderSenderAdapter &) = delete;

  bool begin_frame(qar_ogre_streaming::QarFrameInfo & out_info) override;
  bool submit_opengl_stereo_frame(
    const qar_ogre_streaming::QarOpenGlStereoFrame & frame) override;

  /// @brief True once QAR reported the stream closed (peer left / removed). The
  /// producer destroys the corresponding stream when this flips true.
  bool is_closed() const { return closed_.load(); }

  const char * last_skip_detail() const override { return last_skip_detail_.c_str(); }

  // --- Diagnostics live switches (see qar_render_sender_adapter.hpp) ---------
  void set_prime_enabled(bool enabled) override { prime_enabled_ = enabled; }
  void set_tick_ms(double tick_ms) override { tick_ms_ = tick_ms; }
  std::string frame_trace_path() const override;
  std::uint64_t frame_trace_rows() const override;
  std::uint64_t frame_trace_changed_rows() const override;

private:
  // Populate the QarVideoFrameOpenGl views + texture sizes/formats from our stereo
  // frame. In supply-own mode texture ids are our Ogre ids; in render-into mode the
  // ids are left 0 for QAR to fill. Returns the number of textures/views.
  void fill_frame_layout(QarVideoFrameOpenGl & gl,
    const qar_ogre_streaming::QarOpenGlStereoFrame & frame, bool supply_ids) const;

  /// @brief Poll the device's hands and publish the AR pointing ray.
  ///
  /// Called once per ready frame, immediately after the per-eye poses are read,
  /// because that is the one place both halves of the ray exist at once: the
  /// eye pose comes off this frame's QarRenderFrameInfo and the hands come off
  /// `qar_render_sender_last_hands`, which the vendor documents as drain-and-
  /// return-the-last and therefore wants polled exactly once per frame.
  ///
  /// The gesture callback thread cannot do this itself: it has no render sender
  /// and no frame info, and polling the hands from two threads would make each
  /// one read "untracked" on the calls the other drained. @ref
  /// qar_ar_core::PointingRaySlot is the hand-off.
  void publish_pointing_ray(const QarPose & eye_pose);

  /// @brief Start (or advance) the begin-frame wait for the NEXT frame.
  ///
  /// qar_render_sender_try_begin_frame STARTS the wait on the first call and only
  /// POLLS it on later calls. Polling it once per render tick therefore costs two
  /// ticks per frame — one to start the wait, one to observe it ready — which caps
  /// throughput at half the tick rate regardless of how cheap the render is.
  /// Priming right after show_frame lets the wait run during the inter-tick gap, so
  /// the next tick's poll finds it already ready and each frame costs one tick.
  void prime_next_frame();

  /// @brief After a frame has been shown: if it is the first one after a stall, read its
  /// colour back once and print the one line about it (see frame_stall_probe.hpp). Costs
  /// nothing on any other frame.
  void probe_after_stall(const qar_ogre_streaming::QarOpenGlStereoFrame & frame);

  /// @brief After the first frame has been shown: print the one line with the thread that
  /// showed it and the texture ids of the frame it was shown with. @p gl is the frame as
  /// the runtime filled it (its ids are the runtime's in render-into mode).
  void note_first_show(const QarVideoFrameOpenGl & gl);

  QarRenderSender * stream_ = nullptr;
  QarNearFar near_far_{};
  float depth_scale_ = 1.0f;  ///< reported per show_frame; see Config::depth_scale
  GlSubmitMode submit_mode_ = kDefaultGlSubmitMode;
  std::shared_ptr<IQarGlTextureCopyHook> copy_hook_;
  std::atomic<bool> closed_{false};
  std::uint64_t frame_index_ = 0;
  /// One-shot guard for the "render-into without a copy hook" warning.
  bool warned_no_copy_hook_ = false;
  /// Where the AR pointing ray is published for the gesture seam; may be null
  /// (a stream built without a picker).
  std::shared_ptr<qar_ar_core::PointingRaySlot> pointing_ray_slot_;
  /// One line the first time hands arrive and the first time they are refused,
  /// so the log says whether the device sends hands at all without printing
  /// at frame rate.
  bool logged_hands_tracked_ = false;
  bool logged_hands_missing_ = false;
  /// Frame info already observed ready by prime_next_frame(), awaiting the next
  /// begin_frame() call. Owned here: destroyed on consumption and in the dtor.
  QarRenderFrameInfo * primed_info_ = nullptr;
  /// Why the last begin_frame() handed back no frame. The only place the
  /// difference between "the viewer has not asked us to render" and "the SDK
  /// errored" is visible from outside the adapter — see last_skip_detail().
  std::string last_skip_detail_;
  /// How many begin_frame() calls in a row have found the wait still pending. A
  /// handful is normal pacing; thousands is the producer wedged.
  std::uint64_t consecutive_pending_ = 0;
  /// Finds the first frame after a stall of the frames shown; see probe_after_stall().
  FrameStallDetector stall_detector_;
  /// The first eight hex digits of the peer id, the name the log lines of this stream carry.
  std::string peer_label_;
  /// Lets the first frame shown print the line with its thread and texture ids, once.
  LogOnce first_show_noted_;

  // --- Diagnostics live switches + per-frame trace (QAR_ADAPTER_*) -----------
  bool prime_enabled_ = true;   ///< QAR_ADAPTER_PRIME; guards the prime_next_frame() call.
  double tick_ms_ = 0.1;        ///< QAR_PANEL_TICK_MS; informational, CSV column only.
  /// Null when tracing is off (the default), which keeps the per-frame cost on the
  /// default path to a single null check. See the free-standing FrameTraceState
  /// class above.
  std::unique_ptr<FrameTraceState> frame_trace_;
  /// Null unless QAR_VIDEO_TIMING_DIR is set; then every try_begin_frame result and every
  /// submit is queued (never written) for render3d*.csv. See RenderTimingState.
  std::unique_ptr<RenderTimingState> timing_;
};

/// @brief Factory that mints one QarCApiRenderSenderAdapter per render target via
/// qar_render_sender_create_async.
///
/// The QAROS session is not the factory's: a QarSessionOwner joins the Hub and keeps the list of
/// targets (the headsets and Visualizer receivers that show content). The factory borrows the
/// owner's session once the owner is ready (attach()), hears about targets through
/// ITargetListener, and hands them to the producer: new targets through
/// poll_new_peer_requests(), which drains its queue; targets that left, and streams QAR reported
/// closed, through poll_closed_peers(), which consumes nothing: it lists a peer on every call
/// until forget() is called for it. The producer polls both on its render thread.
///
/// A factory must be owned by a std::shared_ptr: attach() registers a weak reference to it as the
/// owner's target listener.
///
/// The factory does not own the session owner: it keeps a weak reference, so the last reference to
/// the owner is never dropped by a factory, whichever thread releases the factory. The program
/// keeps the owner alive for as long as it uses a factory; a factory that outlives its owner sees
/// ready() false and detaches without a call.
///
/// No call of the consumer thread waits for the Hub. The app volume, the first read of its
/// placement, the placement subscription and the world anchor are QarOS calls that run on the
/// thread that owns the session (QarSessionOwner::post_session_task); the factory keeps what they
/// found in a state of its own that both threads read.
class QarCApiSenderFactory : public qar_ogre_streaming::IQarRenderSenderFactory,
  public IQarSessionOwner,
  public ITargetListener,
  public std::enable_shared_from_this<QarCApiSenderFactory>
{
public:
  struct Config
  {
    QarNearFar near_far{0.1f, 500.0f};  ///< reported per show_frame

    /// @brief Value passed as QarRenderFrameShow.depth_scale each frame.
    ///
    /// Our depth texture holds METRIC linear eye-space distance in metres.
    /// If the consumer instead expects a normalised [0,1] depth, every surface beyond
    /// 1 m saturates and the whole scene renders in one flat colour — the flat-colour
    /// depth symptom. Setting this to 1/far converts metres to [0,1] over the frustum.
    /// See docs/ARCHITECTURE.md#depth-encoding for the encodings and what the
    /// Visualizer expects.
    float depth_scale = 1.0f;

    /// @brief GL submit mode (see @ref GlSubmitMode) propagated to every per-peer adapter.
    GlSubmitMode submit_mode = kDefaultGlSubmitMode;

    /// @brief Optional render-into copy hook (see IQarGlTextureCopyHook); null ok.
    std::shared_ptr<IQarGlTextureCopyHook> copy_hook;

    /// @brief Hard ceiling on concurrently streamed peers (default 8), on top of the
    /// owner's cap on served targets. Extra peers are refused with a logged warning instead
    /// of quietly overloading the render tick — every peer costs four scene renders per frame
    /// (color and depth, two eyes).
    uint32_t max_peers = 8;

    /// @brief Get ONE app volume at attach() and key every render
    /// stream to it (`QarRenderSenderInit.app_volume_id`).
    ///
    /// The app volume is the mixer's unit of work: it is the named box in the room
    /// that reserves the region our content renders into, and the mixer composites
    /// each incoming stream into the volume its `app_volume_id` names.
    ///
    /// **Turning this off does not give a working stream.** The installed header
    /// documents `app_volume_id` as Required and rejects NULL (which is what
    /// `qar_render_sender_init_default()` leaves) with
    /// `QAR_STATUS_APP_VOLUME_INVALID_ID`, so with this off `create_sender()`
    /// refuses and says why rather than building a stream the runtime will
    /// reject. Kept as a flag only so that refusal is legible instead of being an
    /// unexplained failure deep in the SDK.
    bool create_app_volume = true;

    /// @brief `QarAppVolumeInit.display_name` for the volume we create.
    std::string app_volume_name = "RViz Scene";

    /// @brief The program, instance and host that the stable ASCII common name of the app volume
    /// is made from (qar_ar_core::make_volume_common_name). The same name finds the same volume
    /// again at the next start.
    qar_ar_core::NameScope name_scope;

    /// @brief Log the app volume's gesture configuration read-back at attach time.
    ///
    /// `QarAppVolumeInit.gesture_configuration` is ALWAYS left NULL: the volume
    /// then gets the runtime's own default
    /// (`qar_app_volume_gesture_configuration_default()`) with zero chance of an
    /// unfilled field silently overriding one: a zeroed rule's
    /// `controller_ray_style.line_style` is
    /// `QAR_APP_VOLUME_CONTROLLER_RAY_LINE_STYLE_NONE`, i.e. an invisible ray.
    /// This flag only controls whether we then read the configuration back
    /// from the runtime and log it for diagnostics. Off saves that one read-
    /// back round trip; it does not change what the volume does.
    bool app_volume_pick_gestures = true;

    /// @brief Volume extent in ROOM meters: the cuboid reserving the region our
    /// content renders into. Nothing clips the content to it, but gestures and
    /// layout are derived from the volume.
    ///
    /// 10 m per axis: the box spans 5 m on each side of its centre, and the centre is the app
    /// origin (the app pose is the identity), which is where an RViz scene has its ground; see
    /// @ref app_volume_center_y_m. The volume covers (extent / app_scale) app meters, so a
    /// scene that reaches further needs a larger box or a smaller app_scale. Real sensor data
    /// is large - an Ouster OS1 frame is 1024x32 = 32768 points spanning ~120 m.
    float app_volume_width_m = 10.0f;
    float app_volume_length_m = 10.0f;
    float app_volume_height_m = 10.0f;

    /// @brief Height of the box CENTRE above the origin of room space (y = 0), in
    /// meters. The volume pose is the cuboid centre, so at 0 the lower half of the box
    /// lies below y = 0, and the QAR C API header does not call y = 0 the floor.
    float app_volume_center_y_m = 1.0f;

    /// @brief Room-meters per app-meter (`QarAppVolumeInit.app_scale`). 1.0 renders
    /// 1 m of RViz scene as 1 m in the room. Tunable live via QAR_PANEL_VOLUME_SCALE
    /// because the right value depends on the scene.
    ///
    /// 1.0 rather than 1:10, because shrinking scales FEATURES as well as extent:
    /// at 0.1 a 10 cm point-cloud box renders 1 cm across and reads as a single
    /// pixel — present, correct, and far too small to see. Shrink the volume to
    /// taste, but remember it costs feature legibility, not just room.
    float app_volume_scale = 1.0f;

    /// @brief Folder for the per-frame trace files of the senders (QAR_ADAPTER_FRAME_TRACE).
    /// Empty => `%TEMP%/quaternar`. The library's own log folder is
    /// SessionOwnerConfig::log_folder_path.
    std::string log_folder_path;
  };

  /// @param owner The session owner whose session this factory borrows (not owned: see the class
  ///        comment); the factory detaches in its destructor.
  QarCApiSenderFactory(std::shared_ptr<QarSessionOwner> owner, const Config & config);
  ~QarCApiSenderFactory() override;

  QarCApiSenderFactory(const QarCApiSenderFactory &) = delete;
  QarCApiSenderFactory & operator=(const QarCApiSenderFactory &) = delete;

  /// @brief Where the attach is.
  enum class AttachState { kIdle, kPending, kAttached, kFailed };

  /// @brief Starts the attach: the app volume's get-or-create, the first placement read and the
  /// placement subscription run on the thread that owns the session, and the factory registers
  /// as the owner's target listener there. Consumer thread; it does not wait for the volume.
  /// @return false, with @ref last_error, when the owner is not ready or the factory is not owned
  /// by a std::shared_ptr. Calling it in kFailed retries.
  bool begin_attach();

  /// @brief Where the attach is. Consumer thread; poll it on every tick while it is kPending.
  /// kFailed comes with @ref last_error.
  AttachState attach_state() const;

  /// @brief begin_attach() and a wait until attach_state() leaves kPending. BLOCKS for as long as
  /// the volume takes (seconds): for console programs only, never on a GUI thread.
  /// @return false, with @ref last_error, when the attach did not succeed.
  bool attach();

  /// @brief Stops listening and lets go of the volume. Call on the consumer thread, after every
  /// sender is destroyed and before the owner disconnects. Returns after a delivery of a target
  /// change that is running has ended, so the factory can be released right after it. A render
  /// sender whose creation is still in flight is destroyed when its result arrives.
  void detach();

  /// @brief True once the attach succeeded and detach() was not called.
  bool attached() const;

  /// @brief True while the owner's session is up (the owner's own answer).
  bool ready() const override
  {
    const std::shared_ptr<QarSessionOwner> owner = owner_.lock();
    return owner != nullptr && owner->ready();
  }
  /// @brief The reason of the last failure (a copy: the thread that owns the session writes it).
  std::string last_error() const;

  /// @brief The owner's QarOS session, borrowed — null until the owner is ready.
  ///
  /// Exposed for the GUI-panel path (QarCApiPanelSenderFactory), which is
  /// REQUIRED to share this session rather than create its own:
  /// QarGuiPanelStreamIdExt only accepts a stream id from a video sender owned
  /// by the same session as the panel. Callers must not destroy it.
  QarSession * session() const override
  {
    const std::shared_ptr<QarSessionOwner> owner = owner_.lock();
    return owner != nullptr ? owner->session() : nullptr;
  }

  /// @brief Queues @p task on the owner's thread (see IQarSessionOwner::post_session_task).
  /// @return false when the owner is gone or no session is running.
  bool post_session_task(
    const char * name, SessionTask task, std::function<void()> dropped = {}) override;

  /// @brief The app volume this factory got at attach(), if it did.
  ///
  /// The spatial gesture source needs it: `qar_app_volumes_subscribe_gesture_events`
  /// is per-volume, and this is the only volume we own. False when the volume
  /// was not made (`Config::create_app_volume` off, or the call failed) — the caller
  /// then simply has no gestures to subscribe to.
  bool app_volume(QarAppVolumeId & out_id) const;

  /// @brief Where QarOS is displaying our scene RIGHT NOW, and at what size.
  ///
  /// ## Why this is a read-back and not a copy of @ref Config
  ///
  /// We create the volume with `app_scale = 1.0` and an identity `app_pose`,
  /// and leave `gesture_configuration` NULL, which is the runtime's own
  /// default and already carries the two mapping rules that let the operator
  /// change both: dual-pointer-distance -> `app_scale` and
  /// single-pointer-6DoF -> `app_pose`, which is grab-to-move and
  /// pinch-to-zoom. From the moment the operator uses either, the RUNTIME owns
  /// those two fields — the QarOS session log publishes
  /// `qar/storage/appVolume/appPose/updated/<our id>` and
  /// `.../appScale/updated/<our id>` — and our creation-time numbers are a
  /// guess that gets staler every time the operator touches the scene. A volume
  /// that existed already keeps the placement it had.
  ///
  /// Reading them back keeps the diagnostic honest, and is the right model for
  /// anything genuinely stated in room metres. Gesture points, view poses and
  /// hand poses are NOT: they arrive in app content space and need no placement
  /// (see docs/QAROS_INTEGRATION_NOTES.md#app-content-space and
  /// qar_ar_core/spatial_frames.hpp), so the pick path never applies it.
  ///
  /// ## Why it is a cache
  ///
  /// Every read of the placement from the runtime is a query that waits for the Hub, and on the
  /// installed QAROS one query takes about 2 seconds. So this copies the last placement that was
  /// seen and never calls QarOS: the attach reads it once, the runtime's update subscription
  /// (qar_app_volumes_subscribe_updates) replaces it when the volume changes, and a cache that is
  /// older than a second is refreshed with the "latest" getters on the thread that owns the
  /// session. The value is as fresh as the last of those.
  ///
  /// While nothing is known (the attach could not read the placement) it also asks the owner's
  /// thread for a new read, at most once every ten seconds, so the cache fills once a read works.
  ///
  /// @return false when there is no volume or nothing was read yet - @p out is then left
  ///         untouched, and a caller keeps whatever placement it already had rather than
  ///         silently reverting to identity.
  bool scene_placement(qar_ar_core::ScenePlacement & out) const;

  /// @brief Where every stream this factory builds publishes the AR pointing
  /// ray, and where the spatial gesture seam reads it.
  ///
  /// One slot per session rather than one per peer: the ray is a property of the
  /// person wearing the headset, not of a stream, and the picker asks "where is
  /// the operator aiming right now" with no peer in the question. Never null, so
  /// a caller can wire it before any stream exists.
  const std::shared_ptr<qar_ar_core::PointingRaySlot> & pointing_ray_slot() const
  {
    return pointing_ray_slot_;
  }

  std::shared_ptr<qar_ogre_streaming::IQarRenderSenderAdapter> create_sender(
    const QarPeerId & peer_id) override;

  /// @brief Latch the calling thread's *current* WGL context, if there is one.
  ///
  /// `create_sender()` must hand QAR our GL context (QarStreamParamsOpenGlWgl), but it
  /// runs from the panel's QTimer tick — on the render thread, yet *between* Ogre
  /// frames, where nothing guarantees a context is still current. RViz renders its
  /// main view on that same thread continuously, so calling this every tick latches
  /// the context the moment one IS current; `create_sender()` then uses the latched
  /// handles. Cheap (two thread-local WGL queries) and idempotent.
  ///
  /// @return true once a context is known (freshly captured or latched earlier).
  bool capture_gl_context();

  /// @brief Drain peer ids the viewer requested a stream for (thread-safe).
  void poll_new_peer_requests(std::vector<QarPeerId> & out);

  /// @brief Append the ids of the peers whose stream is over (thread-safe): the target
  /// left the session, or QAR reported the stream closed.
  ///
  /// Not a drain: nothing is consumed, so every call lists the same peers again until
  /// @ref forget is called for each of them.
  void poll_closed_peers(std::vector<QarPeerId> & out);

  /// @brief Forget a peer's bookkeeping once its stream is destroyed. The peer
  /// stops being listed by @ref poll_closed_peers, and its place under max_peers is free.
  /// A request still queued for the peer is dropped, so a producer that queued it again for a
  /// peer that left is not asked for it every tick. A target that is still here is requested
  /// again, so a stream that QAR closed is rebuilt.
  void forget(const QarPeerId & peer_id);

  /// @brief Queue a peer for a stream. The producer calls it to RE-QUEUE a peer whose stream
  /// creation must be retried next tick. A peer already queued is not queued twice.
  void on_render_request(const QarPeerId & peer_id);

  // ITargetListener: called on a QAR thread; both only queue.
  void on_target_up(const TargetInfo & target) override;
  void on_target_down(const QarPeerId & id, QarPeerPresence presence) override;

  /// @brief Number of peers that have a stream or are queued for one (diagnostics).
  std::size_t served_peer_count() const;

  /// @brief The outcome of the last anchor change that ran, once. Consumer thread.
  /// @return true when one arrived; @p out_error is empty for success.
  bool take_geo_anchor_result(std::string & out_error);

  /// @brief Georeference our app volume from a WGS84 geodetic fix. Queues the change for the
  /// thread that owns the session; the outcome arrives through take_geo_anchor_result().
  ///
  /// This is how ROS geolocation reaches QarOS. The SDK anchors a volume with a
  /// @c QarGeoAnchorFrame — an ECEF WGS84 origin plus the three unit axes of the
  /// app's local frame expressed in ECEF — so a lat/lon/alt fix (the payload of
  /// @c sensor_msgs/NavSatFix) is converted here: the origin to ECEF, and the axes
  /// to the East/North/Up basis at that point, permuted into the ROS convention
  /// where local +X is East, +Y is North and +Z is Up (REP-103).
  ///
  /// @param latitude_deg  WGS84 latitude, degrees, north positive.
  /// @param longitude_deg WGS84 longitude, degrees, east positive.
  /// @param altitude_m    Height above the WGS84 ellipsoid, metres. NavSatFix is
  ///                      specified as ellipsoidal height, NOT height above MSL.
  /// @return Empty when the change was queued, otherwise why not (no session, no volume, a bad
  /// fix).
  std::string set_geo_anchor_wgs84(
    double latitude_deg, double longitude_deg, double altitude_m);

  /// @brief Georeference our app volume from an ECEF frame that is already known.
  ///
  /// The path for a REP-105 @c earth->map transform, which is by definition already
  /// ECEF: pass its translation as the origin and its rotation's basis vectors as
  /// the axes, and no geodetic conversion is needed or wanted.
  ///
  /// Axes must be unit length and right-handed. @return empty when the change was queued.
  std::string set_geo_anchor_ecef(
    const double origin_xyz[3],
    const double axis_x[3], const double axis_y[3], const double axis_z[3]);

  /// @brief Drop the volume's world anchor (back to un-georeferenced). Queued like the setters.
  std::string clear_geo_anchor();

  /// @brief Whether a world anchor is currently set on our volume (the last change that ran).
  bool has_geo_anchor() const;

private:
  friend struct QarCApiSenderFactoryTestAccess;

  Config config_;
  std::weak_ptr<QarSessionOwner> owner_;

  /// What the thread that owns the session and the consumer thread share: the outcome of the
  /// attach, the volume, the placement cache and the anchor requests. Defined in the .cpp. The
  /// tasks hold it by shared_ptr, never the factory, so the factory is released on the consumer
  /// thread only. A detach hands it to nobody new: the factory gets a fresh one.
  struct CrossThread;
  struct AttachJob;
  struct PlacementSubscription;
  std::shared_ptr<CrossThread> shared_;

  // The tasks. Each runs on the thread that owns the session.
  static void run_attach(QarSession * session, const AttachJob & job);
  static void run_placement_refresh(
    QarSession * session, const std::shared_ptr<CrossThread> & shared);
  static void run_placement_unsubscribe(const std::shared_ptr<CrossThread> & shared);
  static void run_geo_anchor(QarSession * session, const std::shared_ptr<CrossThread> & shared);
  /// The runtime calls this on a library thread when an app volume changed.
  static void placement_update_trampoline(QarAppVolume * handle, void * user_state);
  /// One query for the placement of @p volume. Blocks for as long as the Hub takes.
  static bool read_placement_by_query(
    QarSession * session, const QarAppVolumeId & volume, qar_ar_core::ScenePlacement & out);

  std::string queue_geo_request(bool clear, const QarGeoAnchorFrame * frame);
  void set_error(const std::string & text);

  /// The session's one AR pointing ray, written by whichever stream is
  /// rendering and read by the spatial gesture seam. See @ref pointing_ray_slot.
  std::shared_ptr<qar_ar_core::PointingRaySlot> pointing_ray_slot_ =
    std::make_shared<qar_ar_core::PointingRaySlot>();

  /// Guards pending_requests_, adapters_, served_peers_ and closed_peers_, which the QAR
  /// threads (the owner's target callbacks) and the render thread both touch. Mutable so the
  /// const diagnostics accessors can lock it. Nobody may end a QAR subscription while holding it.
  mutable std::mutex mutex_;
  std::vector<QarPeerId> pending_requests_;

  /// Peers that have a stream or are queued for one (max_peers accounting, de-duplication).
  std::vector<QarPeerId> served_peers_;
  /// Peers that left the session while the producer may still hold a stream for them.
  std::vector<QarPeerId> closed_peers_;

  /// The id of the app volume that streams are keyed to, as create_sender() copied it: the
  /// runtime reads QarRenderSenderInit::app_volume_id through a pointer, so the id has a place
  /// that lives as long as the factory. The volume itself is found at the attach (see
  /// Config::create_app_volume) and persists in the Hub after this program ends.
  QarAppVolumeId app_volume_{};

  /// WGL handles latched by capture_gl_context() (HDC / HGLRC, Win32 only). Held as
  /// void* so this header stays free of <windows.h>. The Ogre render context lives
  /// as long as the RViz render window, so latching it once is valid.
  void * gl_device_context_ = nullptr;
  void * gl_render_context_ = nullptr;

  /// One GL context handed to ONE QAR render sender, plus a back-pointer to the
  /// factory that owns the device context. This is the `user_state` the
  /// QarOpenGlContextCallbacks receive.
  ///
  /// Each context is a SECOND context on Ogre's device context, sharing Ogre's object
  /// namespace (wglShareLists), so QAR sees our textures. Ogre's own context can never
  /// be handed over: it is permanently current on the RViz render thread, and QAR
  /// drives its sender transform on an internal graphics thread that must make the
  /// context current there to resolve GL entry points. (Handing over Ogre's failed with
  /// "Could not load OpenGL function: glGetStringi" and a stream restart loop.)
  ///
  /// **One context PER SENDER, never one shared across senders.** A WGL context can be
  /// current on only ONE thread at a time, so if two peers' graphics threads were given
  /// the same context, the second either fails to bind or steals the binding from the
  /// first — the same failure again, in multi-peer form. They still share Ogre's object
  /// namespace with each other, so every peer sees the same textures.
  ///
  /// Public so that a test can pin which handles a sender binds. The runtime's graphics
  /// thread reads this struct while the render thread goes on latching the factory's own
  /// handles every tick, so everything the binding needs is stored here, once, when the
  /// sender is created, and nothing is read from the factory.
public:
  struct GlSenderContext
  {
    QarCApiSenderFactory * factory = nullptr;
    void * context = nullptr;  ///< HGLRC sharing Ogre's object namespace.
    void * hdc = nullptr;      ///< HDC that context was created on.
    std::string peer;          ///< Short peer id, named by the line of the first bind.
    LogOnce bound;             ///< Lets the first successful bind print its line, once.

    /// The pair the runtime's thread makes current: this sender's own context and the device
    /// context it was created on. @return false, with whatever is set returned, when either
    /// is missing; there is no fall back to the factory's handles or to Ogre's context, which
    /// is current on the render thread and must never be bound on a second one.
    bool own_handles(void *& out_hdc, void *& out_context) const
    {
      out_hdc = hdc;
      out_context = context;
      return hdc != nullptr && context != nullptr;
    }
  };

private:
  /// Owned per-sender GL state; outlives the QAR streams (the producer destroys the
  /// manager, and therefore every sender, before releasing this factory).
  std::vector<std::unique_ptr<GlSenderContext>> gl_sender_contexts_;

  /// Create a fresh WGL context on the latched device context, sharing Ogre's object
  /// namespace. @return the HGLRC as void*, or null on failure (Win32 only).
  void * create_shared_gl_context();

  // QAR calls these on its internal graphics thread (QarOpenGlContextCallbacks, one
  // shared struct for all three context types). They bind
  // that sender's own context and resolve GL functions there.
  // user_state = the sender's GlSenderContext*.
  //
  // There is no `unbind`: capture_current / restore_current already express "put
  // the thread back as you found it", and restore_current with
  // has_context == false IS the unbind case.
  static void * gl_get_proc_address_cb(void * user_state, const char * name);
  static bool gl_make_current_cb(void * user_state);
  static QarOpenGlContextThreadState gl_capture_current_cb(void * user_state);
  static bool gl_restore_current_cb(
    void * user_state, const QarOpenGlContextThreadState * state);

  struct PeerAdapter
  {
    QarPeerId peer_id{};
    std::weak_ptr<QarCApiRenderSenderAdapter> adapter;
  };
  std::vector<PeerAdapter> adapters_;

  /// @brief One in-flight qar_render_sender_create_async() per peer.
  ///
  /// Sender creation is NOT quick and is not even guaranteed to finish: it negotiates
  /// a transfer with the receiver, and a mismatch there leaves the synchronous
  /// qar_render_sender_create() blocked forever (an OpenGL sender against a
  /// Vulkan-only mixer parks in "waiting for receiver borrow signal" and freezes
  /// RViz, because create_sender() runs on the Qt GUI thread). The async variant
  /// hands us the result on a QAR thread instead, so the GUI thread only ever polls.
  struct PendingSender
  {
    QarPeerId peer_id{};
    /// Written by the QAR callback thread, read by the render tick — hence atomics.
    std::atomic<bool> done{false};
    std::atomic<bool> failed{false};
    /// Set by detach(): nobody polls this creation, so whoever finds the stream
    /// destroys it.
    std::atomic<bool> abandoned{false};
    /// The number of the thread that ran the callback (thread_number()), published before
    /// `done`; the render tick prints it, the callback itself prints nothing.
    std::atomic<unsigned long long> callback_thread{0};
    /// Valid only once `done` is true. Whoever takes it (exchange to null) owns it.
    std::atomic<QarRenderSender *> stream{nullptr};
    std::chrono::steady_clock::time_point started{};
    bool warned = false;                  ///< "still pending" logged once
    /// The init block and its chained OpenGL params MUST outlive the async call, so
    /// they live here rather than on create_sender()'s stack: `init.header.next`
    /// points at `gl_params.header`, and both are read by the SDK after we return.
    QarRenderSenderInit init{};
    QarStreamParamsOpenGlWgl gl_params{};
  };
  /// shared_ptr because the callback may outlive our interest in it (e.g. the peer
  /// went away while creation was in flight) and must never touch freed state.
  std::vector<std::shared_ptr<PendingSender>> pending_senders_;

  /// Trampoline for qar_render_sender_create_async: runs on a QAR thread.
  static void sender_created_trampoline(
    QarResult status, QarRenderSender * stream, void * user_state);

  /// Gives up a creation: a stream that arrived already is destroyed now, one that arrives later
  /// is destroyed by the trampoline.
  static void abandon_pending(PendingSender & pending);

  /// How long a pending creation may sit before we log it as stuck. Not a hard
  /// cancel — the SDK owns the wait — but it turns an invisible hang into a
  /// diagnosable one, which is exactly what the mixer bug lacked.
  static constexpr double kPendingSenderWarnSeconds = 5.0;
};

}  // namespace qar_qar_adapter

#endif  // QAR_QAR_ADAPTER__QAR_CAPI_RENDER_SENDER_ADAPTER_HPP_
