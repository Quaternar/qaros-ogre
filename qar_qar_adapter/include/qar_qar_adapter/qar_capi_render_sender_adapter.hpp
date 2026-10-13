// Copyright 2026 Quaternar
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
//  opengl + qar_render_sender_show_frame to submit; lifecycle library_init ->
//  runtime_create -> create_session -> session_join -> peer_subscribe_updates).
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
#include "qar_ar_core/qar_streaming_c.hpp"
#include "qar_ar_core/pointing_ray.hpp"
#include "qar_ar_core/spatial_frames.hpp"

#include "qar_ogre_streaming/qar_render_sender_adapter.hpp"
#include "qar_ogre_streaming/types.hpp"
#include "qar_qar_adapter/qar_capi_session_owner.hpp"

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

/// @brief A device type that can be invited into the session.
///
/// The SDK builds one connection string per device *type*
/// (`qar_session_invite_connection_string_*`), and `qar_session_invite_peer` then
/// invites it. Inviting a real device this way is strictly better than waiting for it
/// to onboard itself: the invite returns the peer id, so the device lands in our
/// allow-list and is admitted exactly (no timing heuristic needed).
///
/// Only HoloLens takes a parameter — its `hostname`, which is also the only way we
/// can name *which* physical device to invite. Everything else is parameterless, which
/// is why a second Visualizer cannot be requested (see
/// docs/QAROS_INTEGRATION_NOTES.md#peers-invites-and-admission).
enum class PeerKind
{
  kVisualizer,  ///< The desktop Visualizer (the Hub's own window). Parameterless.
  kHoloLens,    ///< HoloLens 2, addressed by device hostname.
  kQuest,       ///< Meta Quest. Parameterless.
  kZed          ///< ZED stereo camera; joins as a camera-visualizer peer.
};

/// @brief Lower-case name (`visualizer`, `hololens`, `quest`, `zed`).
const char * to_string(PeerKind kind);
/// @brief Parse one of the names above (case-insensitive). @return false if unknown.
bool parse_peer_kind(const std::string & text, PeerKind & out);
/// @brief True if @ref PeerKind::kHoloLens, i.e. a non-empty hostname is required.
bool peer_kind_needs_hostname(PeerKind kind);

/// @brief Which peers surfaced by `qar_peer_subscribe_updates` get a render stream.
///
/// A session's peer subscription surfaces **everything** that joins: the invited
/// Visualizer(s) *and* internal runtime apps; one invited Visualizer can show up
/// as five distinct peer ids. Building a render sender for an
/// internal peer mismatches app ids (the runtime then rejects the Visualizer's own
/// frames) and multiplies GUI-thread render work, so the set must be filtered.
///
/// Our own peer id (from `qar_session_get_my_spec`) is excluded under every policy —
/// we never stream to ourselves.
enum class PeerAdmission
{
  /// Only peers WE invited via `qar_session_invite_peer`. Safe default: exact ids,
  /// no guessing. It admits every Visualizer this producer spawned, so N invites =>
  /// N peers, but it cannot admit a Visualizer that joined the session on its own
  /// (e.g. from another machine, using the serialized invite).
  kInvitedOnly,

  /// Every peer except ourselves. Use to DISCOVER what a real peer set looks like —
  /// it will also admit the runtime's internal peers, which is exactly the failure
  /// mode kInvitedOnly exists to avoid. Diagnostic, not a production setting.
  kAny,

  /// Invited peers, plus any peer whose `qar_peer_spec_get_display_name` contains
  /// @ref QarCApiSenderFactory::Config::admission_name_filter (case-insensitive).
  ///
  /// **Unusable in practice**: display names are
  /// auto-generated *and change between updates for the same peer id* — one peer
  /// reported `clever_darwin`, then `competent_tesla`, then `determined_tesla`. Kept
  /// only in case a future SDK gives peers stable names.
  kInvitedOrNamed,

  /// Invited peers, plus any peer that first appears **after** the connect settle
  /// window (@ref QarCApiSenderFactory::Config::baseline_settle_ms).
  ///
  /// **This is the policy for a demo with real onboarded peers** (HoloLens, METALENSE
  /// 2, Quest, Android, or a Visualizer on another machine). Those join through
  /// QAROS *onboarding* — a pairing code or a push invite from the Hub — never
  /// through `qar_session_invite_peer`, so @ref kInvitedOnly would silently refuse
  /// them and the headset would see nothing.
  ///
  /// It works on timing, which is the only signal the SDK leaves us: the runtime's
  /// own internal peers are all present within moments of the session coming up,
  /// whereas a human onboarding a headset takes far longer. Everything seen during
  /// the window becomes the baseline and is refused; anything new afterwards is a
  /// consumer and is admitted. With nothing new joining it behaves exactly like
  /// @ref kInvitedOnly.
  ///
  /// A heuristic, and labelled as one — it exists because the SDK exposes no peer
  /// role/kind field (see docs/QAROS_INTEGRATION_NOTES.md#peers-invites-and-admission).
  /// Replace it the moment one is available.
  kInvitedOrNew
};

/// @brief Factory that bootstraps the QAR runtime/session and mints one
/// QarCApiRenderSenderAdapter per peer via qar_render_sender_create.
///
/// On construction it runs the minimal real-QAR lifecycle (library_init ->
/// runtime_create -> create_session -> session_join) and subscribes to peer-spec
/// updates (qar_peer_subscribe_updates, the push form of qar_query_peer_specs). New
/// peers (the viewer's "Add user") surface through poll_new_peer_requests(), which
/// drains its queue; closed peers (the viewer's "Delete user") through
/// poll_closed_peers(), which consumes nothing: it lists a closed peer on every call
/// until forget() is called for it. The producer polls both on its render thread.
class QarCApiSenderFactory : public qar_ogre_streaming::IQarRenderSenderFactory,
  public IQarSessionOwner
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

    /// @brief Folder the QAR runtime spawns app binaries (the Visualizer) from,
    /// set as QarRuntimeInit.runtime_binaries_folder_path. Should be
    /// `<QAR_SDK_ROOT>/bin`. Empty => leave the QAR default (cwd, with
    /// a runtime warning). The factory keeps a copy alive for the C string.
    std::string runtime_binaries_folder_path;

    /// @brief GL submit mode (see @ref GlSubmitMode) propagated to every per-peer adapter.
    GlSubmitMode submit_mode = kDefaultGlSubmitMode;

    /// @brief Optional render-into copy hook (see IQarGlTextureCopyHook); null ok.
    std::shared_ptr<IQarGlTextureCopyHook> copy_hook;

    /// @brief Restrict streaming to peers we invited ourselves.
    ///
    /// Arms the allow-list BEFORE qar_peer_subscribe_updates, so the runtime's own
    /// internal peers (which surface immediately and in numbers — a single Visualizer
    /// can show up as 5 ids) are dropped instead of being handed a sender.
    /// Without it the first sender gets built for whichever internal peer arrives
    /// first and the real consumer's frames are app-id-rejected.
    ///
    /// This is INDEPENDENT of whether we auto-invite anything (@ref
    /// auto_invite_visualizers): a session that invites nothing at connect still
    /// needs the filter, because peers invited later from the panel must be the only
    /// ones admitted. Keep it on for every run.
    bool invite_flow_admission = true;

    /// @brief Real devices to invite right after the session comes up, as
    /// `kind[:hostname]` entries separated by commas — e.g.
    /// `hololens:HOLOLENS-A2B3,hololens:HOLOLENS-C4D5,quest`.
    ///
    /// Preferred over waiting for a device to onboard itself: `qar_session_invite_peer`
    /// returns the peer id, so the device joins our allow-list and is admitted exactly,
    /// with no need for the timing heuristic of @ref PeerAdmission::kInvitedOrNew.
    /// A failing entry is logged and skipped — one unreachable headset must not stop
    /// the session or the other devices.
    std::string invite_peers;

    /// @brief How many Visualizer peers to invite automatically at connect.
    ///
    /// DEFAULT 0 — connecting to QarOS opens the session and nothing else. Visualizers
    /// are spawned on demand from the panel ("Invite peer"), because the real use case
    /// is often real devices only, with no Visualizer window at all; and because
    /// inviting one is a blocking call we should not force into every connect.
    ///
    /// One QAR Visualizer == one physical client head (a pair of eye cameras), so N
    /// simultaneous users means N Visualizers, each with its own stereo stream. Each
    /// invite yields a distinct peer id, all added to the admission allow-list.
    uint32_t auto_invite_visualizers = 0;

    /// @brief Payload appended to the Visualizer connection string ("visualizer:").
    ///
    /// The SDK's qar_session_invite_connection_string_visualizer() builds a BARE
    /// "visualizer:" — the payload slot after the colon is left empty, which is why
    /// every such invite resolves to the same peer id and a second Visualizer cannot
    /// be opened on one machine. The other builders show the slot is real: HoloLens
    /// puts a hostname there. QarOS launches one mixer process per GUID (see
    /// %TEMP%\quaternar\mixerAndTargetApp_<guid>_stdout.log), so naming a distinct
    /// GUID is the vendor-suggested way to address a distinct instance.
    ///
    /// DEFAULT "auto" = mint a fresh UUID per invite, which is what makes inviting
    /// and streaming to several Visualizers at once work (two independent
    /// instances, each with its own stream; one can crash and recover without
    /// disturbing the other). Empty restores the stock
    /// behaviour where every invite collapses onto a single peer. Anything else is
    /// used verbatim. An explicit hostname passed to invite_peer() takes precedence,
    /// so the panel's host field can carry a specific GUID.
    std::string visualizer_conn_suffix = "auto";

    /// @brief Which subscription peers get a render stream (see @ref PeerAdmission).
    PeerAdmission admission = PeerAdmission::kInvitedOnly;

    /// @brief Substring matched against a peer's display name under
    /// @ref PeerAdmission::kInvitedOrNamed (case-insensitive). Empty => that policy
    /// degrades to kInvitedOnly rather than admitting everything.
    std::string admission_name_filter;

    /// @brief How long after the session comes up peers still count as "already
    /// there" under @ref PeerAdmission::kInvitedOrNew.
    ///
    /// Long enough that every internal runtime peer has surfaced (they arrive within
    /// moments of `qar_peer_subscribe_updates`), short enough that a human cannot
    /// have onboarded a headset yet. 5 s is comfortably both.
    uint32_t baseline_settle_ms = 5000;

    /// @brief Hard ceiling on concurrently streamed peers (C8 designs for 8). Extra
    /// peers are refused with a logged warning instead of quietly overloading the
    /// render tick — every peer costs two full scene renders per frame.
    uint32_t max_peers = 8;

    /// @brief JSON object handed to the launcher via
    /// `QarSessionCreateInit.launcher_config_json` (the runtime spawns and configures
    /// the launcher itself; do NOT start `qar-runtime-launcher.exe` by hand).
    ///
    /// Vendor guidance: `{"enableMixer": false}` disables the mixer
    /// and gives a **direct RViz -> Visualizer** connection, which is the recommended
    /// starting point for debugging. Default is `enableMixer: true` on the QAR side.
    /// Empty => pass NULL (runtime default).
    std::string launcher_config_json = R"({"enableMixer": true})";

    /// @brief Create ONE app volume at connect and key every render
    /// stream to it (`QarRenderSenderInit.app_volume_id`).
    ///
    /// The app volume is the mixer's unit of work: it is the named box in the room
    /// that reserves the region our content renders into, and the mixer composites
    /// each incoming stream into the volume its `app_volume_id` names.
    ///
    /// **Turning this off does not give a working stream.** The 2026-09-21 SDK
    /// documents `app_volume_id` as Required and rejects NULL (which is what
    /// `qar_render_sender_init_default()` leaves) with
    /// `QAR_STATUS_APP_VOLUME_INVALID_ID`, so with this off `create_sender()`
    /// refuses and says why rather than building a stream the runtime will
    /// reject. Kept as a flag only so that refusal is legible instead of being an
    /// unexplained failure deep in the SDK.
    bool create_app_volume = true;

    /// @brief `QarAppVolumeInit.display_name` for the volume we create.
    std::string app_volume_name = "RViz Scene";

    /// @brief Log the app volume's gesture configuration read-back at connect
    /// time.
    ///
    /// `QarAppVolumeInit.gesture_configuration` is ALWAYS left NULL: the volume
    /// then gets the runtime's own default
    /// (`qar_app_volume_gesture_configuration_default()`) with zero chance of an
    /// unfilled field silently overriding one: a zeroed rule's
    /// `controller_ray_style.line_style` is
    /// `QAR_APP_VOLUME_CONTROLLER_RAY_LINE_STYLE_NONE`, i.e. an invisible ray.
    /// This flag only controls whether we then read the configuration back
    /// from the runtime and log it for diagnostics. Off saves that one read-
    /// back round trip at connect time; it does not change what the volume
    /// does.
    bool app_volume_pick_gestures = true;

    /// @brief Volume extent in ROOM meters: the cuboid reserving the region our
    /// content renders into. Nothing clips the content to it, but gestures and
    /// layout are derived from the volume.
    ///
    /// 40 m because real sensor data is large: an Ouster OS1 frame is 1024x32 =
    /// 32768 points spanning ~120 m, and the volume covers (extent / app_scale)
    /// app meters: a 2 m box would cover only the nearest 20 m of a 1:10 scene.
    float app_volume_width_m = 40.0f;
    float app_volume_length_m = 40.0f;
    float app_volume_height_m = 40.0f;

    /// @brief Height of the box CENTRE above the room floor, in meters. The volume
    /// pose is the cuboid centre, so 0 would bury half the box in the floor.
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

    /// @brief Folder for the QAR library's own log files
    /// (`QarLibraryInit.log_folder_path`). Empty => `%TEMP%/quaternar` is used.
    /// The vendor wants full TRACE-level library logs with any bug report.
    std::string log_folder_path;

    /// @brief Severity for the QAR library log. The vendor wants TRACE with bug reports.
    ///
    /// The factory ALSO exports this as the QAR_LOG_SEVERITY environment variable
    /// before `qar_library_init`, because the vendor library reads the variable in
    /// preference to `QarLibraryInit.log_severity` — a stale `trace` in the environment
    /// otherwise beats a requested `warn` (see
    /// `resolve_library_logging` in env_config.hpp, which the panel uses to fill this and
    /// @ref enable_console_logging).
    QarLogSeverity log_severity = QAR_LOG_SEVERITY_TRACE;

    /// @brief `QarLibraryInit.enable_console_logging`: duplicate every library log line to
    /// stdout (RViz's stdout). True keeps
    /// the vendor's bug-report profile; the quiet profile turns it off so a run does not
    /// write the same lines twice. The file under @ref log_folder_path is unaffected.
    bool enable_console_logging = true;

    /// @brief Absolute path of the QAR implementation DLL to load at runtime when the
    /// header is compiled with `QAR_ENABLE_DYNAMIC_LOADING` (vendor-recommended: the
    /// app links NO import lib and resolves `qar_impl_*` from the DLL itself). Empty =>
    /// `<runtime_binaries_folder_path>/qar-streaming-c.dll`.
    std::string sdk_library_path;
  };

  explicit QarCApiSenderFactory(const Config & config = {});
  ~QarCApiSenderFactory() override;

  QarCApiSenderFactory(const QarCApiSenderFactory &) = delete;
  QarCApiSenderFactory & operator=(const QarCApiSenderFactory &) = delete;

  /// @brief True if the QAR runtime/session bootstrap succeeded.
  bool ready() const override { return ready_; }
  const std::string & last_error() const { return last_error_; }

  /// @brief The QarOS session, borrowed — null until @ref ready.
  ///
  /// Exposed for the GUI-panel path (QarCApiPanelSenderFactory), which is
  /// REQUIRED to share this session rather than create its own:
  /// QarGuiPanelStreamIdExt only accepts a stream id from a video sender owned
  /// by the same session as the panel. Callers must not destroy it.
  QarSession * session() const override { return session_; }

  /// @brief The app volume this factory created at connect, if it created one.
  ///
  /// The spatial gesture source needs it: `qar_app_volumes_subscribe_gesture_events`
  /// is per-volume, and this is the only volume we own. False when the volume
  /// was not created (`Config::create_app_volume` off, or the create call
  /// failed) — the caller then simply has no gestures to subscribe to.
  bool app_volume(QarAppVolumeId & out_id) const
  {
    if (!has_app_volume_) { return false; }
    out_id = app_volume_;
    return true;
  }

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
  /// guess that gets staler every time the operator touches the scene.
  ///
  /// Reading them back keeps the diagnostic honest, and is the right model for
  /// anything genuinely stated in room metres. Gesture points, view poses and
  /// hand poses are NOT: they arrive in app content space and need no placement
  /// (see docs/QAROS_INTEGRATION_NOTES.md#app-content-space and
  /// qar_ar_core/spatial_frames.hpp), so the pick path never applies it.
  ///
  /// Cheap enough to call per Qt tick (three getters on a handle we look up by
  /// id), and deliberately NOT cached: a cached placement is the same stale
  /// guess with extra steps.
  ///
  /// @return false when there is no volume, or when any accessor
  ///         refused — @p out is then left untouched, and a caller
  ///         keeps whatever placement it already had rather than silently
  ///         reverting to identity.
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

  /// @brief Append the ids of the peers whose stream QAR reported closed (thread-safe).
  ///
  /// Not a drain: nothing is consumed, so every call lists the same peers again until
  /// @ref forget is called for each of them. A peer is listed while the factory still
  /// holds its entry and its adapter is alive and closed.
  void poll_closed_peers(std::vector<QarPeerId> & out);

  /// @brief Forget a peer's adapter bookkeeping once its stream is destroyed. The peer
  /// stops being listed by @ref poll_closed_peers, and its place under max_peers is free.
  void forget(const QarPeerId & peer_id);

  // Called by the QAR peer-update callback (on the QAR runtime's thread), and by the
  // producer to RE-QUEUE a peer whose stream creation must be retried next tick.
  // Applies the admission policy and de-duplicates against the pending queue.
  // @param display_name The peer's display name when known (peer-update callback),
  //        null on the re-queue path.
  void on_render_request(const QarPeerId & peer_id, const char * display_name = nullptr);

  /// @brief Invite one more peer into the QarOS session.
  ///
  /// Adds its peer id to the invited allow-list, so it is admitted under every policy
  /// and gets its own stereo stream. This is how a physical client head is added after
  /// the session is already running.
  ///
  /// @param kind      Device type to invite.
  /// @param hostname  Required for @ref PeerKind::kHoloLens (the device's hostname),
  ///                  ignored for the other kinds.
  /// @param[out] out_peer Receives the invited peer id on success.
  /// @return empty on success, else a human-readable reason (not ready, missing
  ///         hostname, at the max_peers ceiling, or the QAR call failed).
  std::string invite_additional_peer(
    PeerKind kind, const std::string & hostname, QarPeerId & out_peer);

  /// @brief `invite_additional_peer(PeerKind::kVisualizer, "", out_peer)`.
  /// With the default `visualizer_conn_suffix` ("auto") every call opens a new
  /// Visualizer; with it empty, repeat invites return the peer that already exists
  /// (docs/QAROS_INTEGRATION_NOTES.md#peers-invites-and-admission).
  std::string invite_additional_visualizer(QarPeerId & out_peer);

  /// @brief Peer ids we have invited so far (thread-safe copy).
  std::vector<QarPeerId> invited_peers() const;

  /// @brief Number of peers admitted by the policy so far (diagnostics).
  std::size_t admitted_peer_count() const;

  /// @brief Georeference our app volume from a WGS84 geodetic fix.
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
  /// @return Empty on success, otherwise a human-readable reason.
  std::string set_geo_anchor_wgs84(
    double latitude_deg, double longitude_deg, double altitude_m);

  /// @brief Georeference our app volume from an ECEF frame that is already known.
  ///
  /// The path for a REP-105 @c earth->map transform, which is by definition already
  /// ECEF: pass its translation as the origin and its rotation's basis vectors as
  /// the axes, and no geodetic conversion is needed or wanted.
  ///
  /// Axes must be unit length and right-handed. @return empty on success.
  std::string set_geo_anchor_ecef(
    const double origin_xyz[3],
    const double axis_x[3], const double axis_y[3], const double axis_z[3]);

  /// @brief Drop the volume's world anchor (back to un-georeferenced).
  std::string clear_geo_anchor();

  /// @brief Whether a world anchor is currently set on our volume.
  bool has_geo_anchor() const { return has_geo_anchor_; }

private:
  Config config_;
  bool ready_ = false;
  std::string last_error_;

  QarRuntime * runtime_ = nullptr;
  QarSessionInvite * invite_ = nullptr;  ///< created-session invite (host side)
  QarSession * session_ = nullptr;

  /// Guards pending_requests_, adapters_, invited_peers_ and admitted_peers_, which
  /// the QAR runtime's callback thread and the render thread both touch. Mutable so
  /// the const diagnostics accessors can lock it.
  /// The session's one AR pointing ray, written by whichever stream is
  /// rendering and read by the spatial gesture seam. See @ref pointing_ray_slot.
  std::shared_ptr<qar_ar_core::PointingRaySlot> pointing_ray_slot_ =
    std::make_shared<qar_ar_core::PointingRaySlot>();

  mutable std::mutex mutex_;
  std::vector<QarPeerId> pending_requests_;

  // In the invite-a-Visualizer flow the peer subscription surfaces MANY
  // peers (internal runtime apps AND every invited Visualizer). Creating senders for
  // the internal peers builds them with the wrong app id (the runtime then rejects
  // the Visualizer's frames) and multiplies GUI-thread render work, so admission is
  // filtered — see PeerAdmission and admit_locked().
  //
  // invite_flow_ is set (invite mode) BEFORE subscribing, so on_render_request
  // drops every subscription peer until at least one invited Visualizer id is known.
  // Without this, internal peers that surface during setup get queued before the
  // invited ids exist (a race) and the first sender is built for the wrong app id.
  bool invite_flow_ = false;
  /// Peers we invited ourselves — always admitted. One entry per physical client head.
  std::vector<QarPeerId> invited_peers_;
  /// Our own peer id (qar_session_get_my_spec); never admitted.
  QarPeerId self_peer_{};
  bool has_self_peer_ = false;

  /// The single app volume every render stream is keyed to (@ref
  /// Config::create_app_volume). Created once at connect, after the session is
  /// joined and BEFORE any sender exists, because create_sender() must be able to
  /// point QarRenderSenderInit::app_volume_id at it. Stays valid for the whole
  /// session; the runtime retires it when the session ends.
  QarAppVolumeId app_volume_{};
  bool has_app_volume_ = false;
  /// Whether a QarGeoAnchorFrame is currently attached to app_volume_.
  bool has_geo_anchor_ = false;
  /// Peers the policy has admitted at least once (dedupe + max_peers accounting).
  std::vector<QarPeerId> admitted_peers_;

  /// Peers already present when the session came up — the runtime's own internal
  /// apps. Used only by @ref PeerAdmission::kInvitedOrNew, which admits what is NOT
  /// in here. Frozen once baseline_settle_ms has elapsed since ready_.
  std::vector<QarPeerId> baseline_peers_;
  std::chrono::steady_clock::time_point connected_at_{};
  bool connected_at_set_ = false;
  /// One-shot log line when the baseline freezes, so the log shows what was in it.
  bool baseline_logged_ = false;

  /// Apply the admission policy. Caller must hold mutex_. @p display_name may be null
  /// (unknown), in which case the name policy cannot match.
  bool admit_locked(const QarPeerId & peer_id, const char * display_name);

  /// True once the settle window has elapsed, i.e. baseline_peers_ is complete and
  /// later arrivals count as consumers. Logs the baseline once, on the transition.
  /// Caller must hold mutex_.
  bool baseline_frozen_locked();

  /// Invite ONE peer of @p kind and record its id. Caller must NOT hold mutex_.
  /// Returns an empty string on success, else the failure text.
  std::string invite_one_peer_locked_free(
    PeerKind kind, const std::string & hostname, QarPeerId & out_peer);
  /// True when THIS factory performed the dynamic qar_library_load() (so the dtor
  /// unloads it again). Only meaningful with QAR_ENABLE_DYNAMIC_LOADING.
  bool loaded_library_ = false;

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
  struct GlSenderContext
  {
    QarCApiSenderFactory * factory = nullptr;
    void * context = nullptr;  ///< HGLRC sharing Ogre's object namespace.
  };
  /// Owned per-sender GL state; outlives the QAR streams (the producer destroys the
  /// manager, and therefore every sender, before releasing this factory).
  std::vector<std::unique_ptr<GlSenderContext>> gl_sender_contexts_;

  /// Create a fresh WGL context on the latched device context, sharing Ogre's object
  /// namespace. @return the HGLRC as void*, or null on failure (Win32 only).
  void * create_shared_gl_context();

  // QAR calls these on its internal graphics thread (QarOpenGlContextCallbacks, one
  // shared struct for all three context types since the 2026-09-21 SDK). They bind
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
    QarRenderSender * stream = nullptr;   ///< valid only once `done` is true
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

  /// How long a pending creation may sit before we log it as stuck. Not a hard
  /// cancel — the SDK owns the wait — but it turns an invisible hang into a
  /// diagnosable one, which is exactly what the mixer bug lacked.
  static constexpr double kPendingSenderWarnSeconds = 5.0;
};

}  // namespace qar_qar_adapter

#endif  // QAR_QAR_ADAPTER__QAR_CAPI_RENDER_SENDER_ADAPTER_HPP_
