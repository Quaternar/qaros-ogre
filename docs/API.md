# API reference

This page lists the public headers of the three modules, the types and functions that a host uses, and the rules that come with them. [Architecture](ARCHITECTURE.md) explains how the parts fit together, and [Configuration](CONFIGURATION.md) lists the environment variables and the default values of the configuration structs. The code on this page is compiled with the repository; its parameters stand for the objects that your program owns.

## Linking the package

A consumer finds the package with `find_package`, names the components that it needs and links their imported targets. Each component is a static library, so a program links only what it uses:

```cmake
find_package(qar_ogre_adapter 0.1.0 CONFIG REQUIRED
  COMPONENTS qar_ar_core qar_ogre_streaming render panels url gestures)
```

The target that compiles the code of this page links all of them in the same way:

```cmake
target_link_libraries(qar_ogre_adapter_doc_snippets PRIVATE
  qar_ogre_adapter::qar_ar_core
  qar_ogre_adapter::qar_ogre_streaming
  qar_ogre_adapter::render
  qar_ogre_adapter::panels
  qar_ogre_adapter::url
  qar_ogre_adapter::gestures
)
```

| Component | Target | What it brings with it |
|---|---|---|
| `qar_ar_core` | `qar_ogre_adapter::qar_ar_core` | C++17, the include folder of the module and the compile definition `QAR_ENABLE_OPENGL` |
| `qar_ogre_streaming` | `qar_ogre_adapter::qar_ogre_streaming` | `qar_ar_core` and Ogre |
| `capi` | `qar_ogre_adapter::capi` | `qar_ar_core` and the compile definition `QAR_ENABLE_DYNAMIC_LOADING` |
| `session` | `qar_ogre_adapter::session` | `capi` and `qar_ar_core` |
| `render` | `qar_ogre_adapter::render` | `capi`, `session`, `qar_ar_core` and `qar_ogre_streaming` |
| `panels` | `qar_ogre_adapter::panels` | `capi` and `qar_ar_core` |
| `url` | `qar_ogre_adapter::url` | `capi` and `qar_ar_core` |
| `gestures` | `qar_ogre_adapter::gestures` | `capi` and `qar_ar_core` |

Include a header by its path below the `include` folder of its module, for example `qar_ogre_streaming/types.hpp`. Include the QarOS C header only through `qar_ar_core/qar_streaming_c.hpp`: the vendored header has one include guard, so its first inclusion fixes which optional parts exist, and the wrapper defines `QAR_ENABLE_OPENGL` before it.

The code is in the namespaces `qar_ar_core`, `qar_ogre_streaming` and `qar_qar_adapter`. The first two define a `Status` and a `Result` each, and the types are not interchangeable: `qar_ogre_streaming::Status` has `RenderThreadRequired`, and `qar_ar_core::Status` has `GuiThreadRequired` and `SenderFailed`.

## Header map

Every public header of the repository, by module. The sections below describe the types.

**`qar_ar_core`**: a core with no dependency but the C++17 standard library and the QarOS header.

| Header | What it declares |
|---|---|
| `qar_streaming_c.hpp` | the one include of the vendored QarOS header |
| `panel_types.hpp` | `Status`, `Result`, `PanelDesc`, `PanelPlacement`, `PanelPixels`, `PanelNotReadyCause`, `not_ready_split_text`, the diagnostics records of a panel host and `PanelDiagnosticsSink` |
| `qar_panel_sender.hpp` | the seam of a panel that streams pixels: `IQarPanelSenderAdapter`, `IQarPanelSenderFactory`, `PanelCloseReport` |
| `qar_url_panel.hpp` | the seam of a panel whose content is a URL: `IQarUrlPanelAdapter`, `IQarUrlPanelFactory`, `UrlPanelDesc` |
| `common_name.hpp` | the stable ASCII common names of the QarOS panels and of the app volume: `NameScope`, `make_name_scope`, `make_panel_common_name`, `make_volume_common_name` |
| `panel_attach.hpp` | the decision about a panel that `qar_gui_panels_get_or_create` returned: `PanelProbe`, `PanelAttachDecision`, `decide_panel_attach` |
| `panel_input_source.hpp` | the pointer seam: `PanelPointerSample`, `IQarPanelInputSource`, `QueuedPanelInputSource`, the mapping from the QarOS enumerations |
| `pointing_ray.hpp` | the aim of the operator: `PointingRay`, its builders, `HandJointsProbe` and `PointingRaySlot` |
| `spatial_frames.hpp` | the change between the room frame and the ROS world: `RoomToRosTransform`, `ScenePlacement` and the conversion functions |
| `spatial_pick.hpp` | spatial selection: `PickTarget`, `resolve_pick_point`, the point, pose and area sessions, `make_nav_goal` |
| `timing_log.hpp` | the opt-in CSV log of stage timings: `TimingLog`, `TimingSink`, `timing_epoch_us` |

**`qar_ogre_streaming`**: renders an Ogre scene as stereo frames; it needs Ogre and `qar_ar_core`.

| Header | What it declares |
|---|---|
| `types.hpp` | `Status`, `Result`, `StereoStreamConfig`, `QarFrameInfo`, `EyeView`, `QarOpenGlStereoFrame` and the other value types |
| `ogre_qar_streaming_manager.hpp` | `OgreQarStreamingManager`, `OgreQarStreamingConfig` |
| `ogre_qar_render_scheduler.hpp` | `OgreQarRenderScheduler`: the batched render tick |
| `render_budget.hpp` | `RenderBudgetConfig`, `RenderBudget`: pacing and shedding |
| `ogre_qar_stereo_stream.hpp` | `OgreQarStereoStream`: one peer's stream, its batch phases and the eye poses of its last frame; `compute_eye_render_params`, `assemble_separated_stereo_frame` |
| `ogre_stereo_render_target.hpp` | `OgreStereoRenderTarget`: the colour and depth textures of one eye |
| `ogre_opengl_texture_bridge.hpp` | `OgreOpenGlTextureBridge` (native texture id, frame marshalling) and `GlFenceHook` |
| `gl_interop.hpp` | the OpenGL entry points of the texture hand-off: fences and read-back |
| `gl_fence_hook.hpp` | `IGlFenceHook`: the fence step of the batch |
| `qar_render_sender_adapter.hpp` | the seam of a render sender: `IQarRenderSenderAdapter`, `IQarRenderSenderFactory` |
| `qar_pose_conversion.hpp` | `qar_pose_to_ogre`, `qar_position_to_ogre`, `qar_orientation_to_ogre`, `make_projection_from_qar_fov`, `Vec3`, `Quat`, `Mat4` |
| `qar_scene_lighting.hpp` | `QarSceneLighting`, `SceneLightingConfig`: the lighting rig of the shared scene |
| `spatial_pick_visual.hpp` | `QarSpatialPickVisual` and the pure builders of the overlay geometry |
| `diagnostics.hpp` | `OgreQarDiagnosticsSink`, `StreamDiagnostics`, `DiagnosticsSnapshot`, `SkipReason` |
| `render_thread_executor.hpp`, `render_lock.hpp`, `clock.hpp` | the seams `IRenderThreadExecutor`, `IRenderLock` and `IClock`, with `SteadyClock` |
| `mock_qar_frame_info_source.hpp` | `MockQarFrameInfoSource`: a deterministic, animated generator of per-eye pose and field of view |

Four more headers belong to the header-only target `qar_ogre_adapter::qar_ogre_streaming_test_support`, which is not part of the shipped library: `immediate_render_thread_executor.hpp`, `manual_clock.hpp`, `mock_qar_render_sender_adapter.hpp` and `recording_render_lock.hpp`. They are test doubles of `IRenderThreadExecutor`, `IClock`, `IQarRenderSenderAdapter` and `IRenderLock`; [Test doubles](TESTING.md#test-doubles) describes them.

**`qar_qar_adapter`**: the QarOS C API side of the seams, in six components.

| Header | Component | What it declares |
|---|---|---|
| `qar_capi_session_owner.hpp` | `capi` | `IQarSessionOwner` |
| `qar_capi_input_slots.hpp` | `capi` | the arena of callback slots |
| `qar_session_owner.hpp` | `session` | `QarSessionOwner`, `SessionOwnerConfig`, `SessionState`, `SessionEvent`, `TargetInfo`, `ITargetListener`, `ILibraryLoader`, `HubEndpoint`, `DeviceKind`, `wait_for_session` |
| `identity_store.hpp` | `session` | `IdentityRecord`, `default_identity_dir`, `read_identity`, `write_identity_atomic`, `remove_identity` |
| `onboarding_status.hpp` | `session` | `StatusText`, `OnboardingContext`, `describe_status` |
| `target_rule.hpp` | `session` | `HubMode`, `is_render_target`, `is_gone`, `resolve_hub_mode` |
| `gui_call_guard.hpp` | `capi` | `GuiCallTimer`, `QAR_CALL`, `QAR_GUI_TIMED`, `mark_gui_thread`, `mark_owner_thread` |
| `panel_state_guard.hpp` | `panels`, `url` | `PanelStateKeeper`: the decision to ask a panel for its state again |
| `qar_capi_render_sender_adapter.hpp` | `render` | `QarCApiSenderFactory`, `QarCApiRenderSenderAdapter`, `GlSubmitMode`, `IQarGlTextureCopyHook` |
| `geo_anchor.hpp` | `render` | `geodetic_to_ecef_enu`, `geo_fix_moved` |
| `env_config.hpp` | `render` | the parsing functions for the text of environment variables |
| `render_timing.hpp` | `render` | `RenderTimingState`: the stage timing of the 3D stream |
| `qar_capi_panel_sender_adapter.hpp` | `panels` | `QarCApiPanelSenderAdapter`, `QarCApiPanelSenderFactory` |
| `qar_capi_panel_input_source.hpp` | `panels` | `QarCApiPanelInputSource`, `PanelInputStats` |
| `qar_capi_url_panel_adapter.hpp` | `url` | `QarCApiUrlPanelAdapter`, `QarCApiUrlPanelFactory` |
| `frame_stall_probe.hpp` | `render` | `FrameStallDetector`, `StallReport`, `mean_rgba8`, `format_stall_line`, and the words of the lines that a sender prints when it starts |
| `panel_state_log.hpp` | `panels`, `url` | the words of the lines about the state of a panel: `PanelRuntimeRecord`, `RuntimeRecordLog`, `runtime_record_line`, `reconnecting_line`, `url_panel_state_line` |
| `log_time.hpp` | `render`, `panels`, `url` | `wall_clock_hms_ms`, `hms_ms_of`, `thread_number` and `LogOnce` |
| `qar_capi_spatial_gesture_source.hpp` | `gestures` | `QarCApiSpatialGestureSource`, `SpatialGestureStats` |

The vendored `third_party/qar/include/qar_streaming.h` is the QarOS C header; [The vendored header](SDK_COMPATIBILITY.md#the-vendored-header) says how it is used.

## qar_ar_core

The module uses the types of the QarOS C API and calls none of its functions, so linking it needs neither the SDK library nor the loader.

### The panel seam

A library that shows a panel in AR talks to QarOS only through two seams, and `qar_qar_adapter` implements both. `IQarPanelSenderFactory::create_panel_sender` takes a `PanelDesc` and returns an `IQarPanelSenderAdapter`, or null with the reason in `last_error()`. The adapter is driven once for each tick: `try_begin_frame` reports whether QarOS has a destination ready, and only then does `submit_frame` follow. A panel that is not ready is a state to poll again, not an error. The calls that change the panel, `set_placement` and `set_state`, queue their work, and the outcome of each comes back through `drain_results`, which the host calls on every tick.

`PanelDesc` fixes the size of the pixel buffer for the life of the sender, so a size change needs a new sender, and its common name is the stable name under which QarOS finds the panel again (`make_panel_common_name`, made from the key and a `NameScope`). `content_width` and `content_height` name the part of the buffer that the widget paints, and zero means the whole buffer. `PanelPlacement` holds the pose and the `QarGuiPanelSize`; the pose is a QarOS panel-space pose, whose origin is the midpoint of the top edge, so the panel hangs below it. In the code, `factory` is the panel sender factory of the host and `pixels` the captured picture of the widget.

```cpp
qar_ar_core::PanelDesc desc;
desc.key = "status";                                    // the key that the host uses for the panel
desc.display_name = "Status";                           // the title that QarOS shows
desc.buffer_width = 1024;                               // the size of the buffer is fixed for the sender
desc.buffer_height = 768;
desc.placement.pose.position = QarVector3{0.0f, 1.6f, -1.0f};
desc.placement.size = QarGuiPanelSize{0.8f, 0.6f, 1.0f};

const std::shared_ptr<qar_ar_core::IQarPanelSenderAdapter> sender = factory->create_panel_sender(desc);
if (!sender) {
  std::fprintf(stderr, "panel: %s\n", factory->last_error().c_str());
  return;
}

// Once for each tick, on the thread of the host that owns the widget.
bool ready = false;
if (sender->try_begin_frame(ready) && ready) {
  qar_ar_core::PanelPixels frame;
  frame.data = pixels;
  frame.width = desc.buffer_width;
  frame.height = desc.buffer_height;
  frame.stride_bytes = desc.buffer_width * 4u;          // RGBA: four bytes for each pixel
  sender->submit_frame(frame);                          // copies the pixels; the buffer stays the host's
}

sender->close();                                        // the one teardown, in a fixed order
```

An adapter can also hide and show its panel without closing it. `set_state(state)` changes the state of the QarOS panel with `qar_gui_panels_set_state` to `QAR_GUI_PANEL_STATE_VISIBLE`, `QAR_GUI_PANEL_STATE_MINIMIZED` or `QAR_GUI_PANEL_STATE_HIDDEN`, and the panel, its video sender and its subscriptions stay. It returns false, with the reason in `last_error()`, when the state did not change, and `supports_state()` tells whether the adapter can change a state at all. Both answer false by default, so that an implementation without panel states keeps compiling and its caller falls back to a close. Each change that succeeds prints a line, which [Reading the logs](TROUBLESHOOTING.md#the-logs-of-this-repository) describes.

When `try_begin_frame` reports `out_ready == false`, `not_ready_cause()` says why, until the next call. The answer is a `PanelNotReadyCause`: `NoDestination` when the runtime answered success and no frame cycle, `NoActiveTransfers` when it answered status 604 and no receiver is attached to the stream, and `Reconnecting` when it answered status 603 and a receiver that lost the stream connects again; the default is `NoDestination`. The first time a sender that has shown frames hears 603, one line says so, with the number of frames that it has shown.

`close()` is the one teardown, in a fixed order: it cancels the input subscriptions, closes the QarOS panel unless the operator already closed it in AR, and destroys the video sender. The returned `PanelCloseReport` says what each step did, and a second call returns an empty report and touches nothing. `detach()` does the first and the last step and leaves the QarOS panel as it is, so that the next session finds it again under its common name; `supports_detach()` tells whether an adapter can do that. `decide_panel_attach` decides what to do with the panel that `qar_gui_panels_get_or_create` returned: it is new, it is an earlier panel of the same name and is reused with its content, size and pose brought up to date, or its name is taken by a panel that cannot be reused, and the repeat with `qar_gui_panels_get_or_create` appends `-v2` for a panel of the other content type or `-r1` to `-r8` for a closed one.

The URL seam has the same shape without frames: `IQarUrlPanelFactory::create_url_panel` takes a `UrlPanelDesc` and returns an `IQarUrlPanelAdapter` with `navigate`, `set_placement`, `set_state`, `close` and `detach`. QarOS renders the page in its own browser, so nothing is streamed, and the pose goes to QarOS unchanged, as a panel-space pose. `UrlPanelDesc::visible_to_peers` lists the peers that see the panel, and an empty list means every peer; the adapter passes the list to QarOS only when it is not empty. The adapter prints a line for each change of the state of the panel.

`panel_types.hpp` also defines the records that a panel host reports its state in, `PanelStreamDiagnostics` for one panel and `PanelDiagnosticsSnapshot` for all of them, and the sink `PanelDiagnosticsSink`, whose `on_message` must not block. A record says whether the panel is hidden in AR (`ar_hidden`, with the counters `visibility_changes` and `visibility_failures` and the reason of the last failure), whether it is parked (`parked`), how the streak of not-ready ticks divides by cause (`not_ready_streak_no_destination`, `not_ready_streak_no_transfers` and `not_ready_streak_reconnecting`, which `not_ready_split_text` writes as three numbers separated by slashes), and the brightness of the frame most recently measured (`luminance_all`, `luminance_top`, `luminance_rest` and `alpha_mean`, each from 0 to 255 and negative before the first measure). The snapshot counts the hidden panels (`hidden_panels`), the parked panels (`parked_panels`) and the parks and the revivals of the session.

### Pointer samples

Every AR pointer enters through one type, `PanelPointerSample`, and one interface, `IQarPanelInputSource`: the consumer calls `drain` on its own thread and gets everything received since the previous call, oldest first. `drain_closed_panels` lists the panels that the operator closed in AR. A sample carries either room-space data (`has_room_point`, `has_room_start_point`, `has_room_ray`) or an already resolved panel coordinate (`has_panel_uv` with the key of the panel, `u` to the right and `v` downwards, from 0 to 1).

`PointerGesture` is a narrowing of the two QarOS kind enumerations to what a widget can act on, and `PointerPhase` mirrors the phases of QarOS one to one:

| QarOS kind | `PointerGesture` | Phases |
|---|---|---|
| `QAR_GUI_PANEL_GESTURE_KIND_TAP` | `Click` | `Instant` |
| `QAR_GUI_PANEL_GESTURE_KIND_DRAG` | `Click` | `Started`, `Updated`, `Ended`, `Canceled` |
| `QAR_GUI_PANEL_GESTURE_KIND_HOVER` | `Hover` | `Started`, `Updated`, `Ended` |
| `QAR_GUI_PANEL_GESTURE_KIND_LONG_PRESS` | `LongPress` | `Instant` |
| `QAR_GESTURE_CLICK` (app volume) | `Click` | any |
| `QAR_GESTURE_HOVER` (app volume) | `Hover` | any |

The manipulation kinds of an app volume have no `PointerGesture`: they move the volume and are not pointer input. Only `Click` may commit something, which `gesture_commits_pick` states; a hover previews. The conversion functions are inline in the header (`pointer_phase_from_qar`, `pointer_gesture_from_panel_kind`, `pointer_gesture_from_volume_kind`, `pointer_hand_from_qar`, `pointer_sample_from_gesture_fields`), so that a test needs no QarOS. `QueuedPanelInputSource` is a thread-safe queue that implements the interface: any thread calls `push`, the consumer drains.

### The pointing ray

A `PointingRay` is an origin, a unit direction, the `PointingRayKind` that produced it and an `error` text when it is not `valid`. `EyeThroughHand` runs from the eye through a hand joint and needs the hand to be at least `min_separation_m`, 0.15 m, from the eye. `HandForward` is the forward axis of the hand orientation, for the case without an eye pose. `GestureStartToAction` runs from the start point of an app volume gesture through its action point.

`pointing_ray_from` builds the first two from `PointingRayInputs`. `pointing_ray_inputs_from_hands` fills the inputs from the hands of a QarOS frame: it takes the preferred hand, the other hand when the preferred one is not tracked, and the palm when the requested joint has no valid position. `pointing_ray_from_gesture_points` builds the third and refuses a gesture whose action point is its start point, because the runtime then intersected nothing. In the code, `hands` and `eye_position` are what QarOS reports for the frame, and `slot` is a `PointingRaySlot`.

```cpp
// The eye pose of this frame and the hands of the device give the aim: the eye through the index tip.
qar_ar_core::PointingRayInputs inputs = qar_ar_core::pointing_ray_inputs_from_hands(
  hands, QAR_HANDEDNESS_RIGHT, qar_ar_core::HandRayJoint::IndexTip);
inputs.has_eye = true;
inputs.eye_position = eye_position;
const qar_ar_core::PointingRay ray = qar_ar_core::pointing_ray_from(inputs);
if (!ray.valid) {
  std::printf("no ray: %s\n", ray.error.c_str());
}
slot->publish(ray, qar_ar_core::hand_joints_probe_from(hands));   // the last writer wins

// On any other thread:
const qar_ar_core::PointingRay latest = slot->latest();
std::printf("ray kind: %s\n", qar_ar_core::to_string(latest.kind));
```

`HandJointsProbe` copies the valid joint positions of both hands, and `nearest_hand_joint` and `hand_joint_distance` measure from a point to them. `PointingRay::aim_to_reported_m` is the distance from the reconstructed ray to the action point that QarOS reported for the same aim, or negative when it was not computed. `PointingRaySlot` hands one ray from the thread that reads the hands to the thread that handles gestures: `publish` replaces the value, `latest` returns a copy.

### Spatial frames

QarOS reports poses and gesture points in the axes of the room: right-handed, +Y up, -Z forward. A ROS world has +X forward, +Y left and +Z up. `RoomToRosTransform` carries the map, which takes a room point (x, y, z) to the ROS point (-z, -x, y) and then applies the viewer anchor: a yaw about +Z, a height along +Z and a stand-off. `room_to_ros_from_environment` reads the variables `QAR_POSE_WORLD`, `QAR_POSE_YAW_DEG`, `QAR_POSE_HEIGHT_M` and `QAR_POSE_BACK_M`, the ones that the camera conversion reads, so that the camera and the picker use one map. Without `QAR_POSE_WORLD=zup` the transform is disabled and is the identity map.

```cpp
// The camera and the picker must use one map: both read the same variables, once, here.
const qar_ar_core::RoomToRosTransform transform = qar_ar_core::room_to_ros_from_environment();

const QarVector3 room_point{1.0f, 0.5f, -2.0f};            // metres, +Y up, -Z forward
const QarVector3 ros_point = qar_ar_core::room_point_to_ros(transform, room_point);
const QarVector3 back = qar_ar_core::ros_point_to_room(transform, ros_point);   // the exact inverse
const QarVector3 ros_direction =
  qar_ar_core::room_direction_to_ros(transform, QarVector3{0.0f, 0.0f, -1.0f});
const QarQuaternion ros_orientation =
  qar_ar_core::room_orientation_to_ros(transform, QarQuaternion{0.0f, 0.0f, 0.0f, 1.0f});
std::printf("ros %.2f %.2f %.2f, room again %.2f, forward x %.2f, w %.2f\n", ros_point.x, ros_point.y,
  ros_point.z, back.x, ros_direction.x, ros_orientation.w);
```

The functions come in pairs that are exact inverses: `room_point_to_ros` and `ros_point_to_room`, `room_direction_to_ros` and `ros_direction_to_room`. A direction takes the rotations but no translation, so it keeps its length. `room_orientation_to_ros` maps a `QarQuaternion`, and a degenerate input gives the identity. `ScenePlacement` describes where QarOS puts the scene (scale and two poses); the pick path leaves the placement of its transform at the identity, because gesture points arrive in app content space ([Design notes](ARCHITECTURE.md#gesture-points-are-in-app-content-space)).

### The pick model

`spatial_pointer_from_sample` brings a `PanelPointerSample` across the frame change; a sample that carries only a panel coordinate comes back with `valid` false. A `PickTarget` names what the pointer is resolved against: `GroundPlane`, the kind that a default `PickTarget` has, a `Plane`, a fixed `Depth` along the ray, or the `ActionPoint` that QarOS reported. `resolve_pick_point` is the core, and its result names the `PickMethod` that was used:

| Target | The pointer has a ray | No ray |
|---|---|---|
| `GroundPlane`, `Plane` | `RayPlane`: the straight ray of this library meets the plane | `ProjectedPoint`: the reported point, projected onto the plane |
| `Depth` | `RayDepth`: `depth_m` along the ray | `ActionPoint`: the reported point as it is |
| `ActionPoint` | `ActionPoint`: the reported point wins | `ActionPoint` |

A plane target always lands on its plane. The `ActionPoint` target refuses a point that is not a scene hit, because QarOS reports the start point, which is the hand, when the aim intersected nothing: the result has `valid` false and `refused_no_intersection` set.

Three sessions are state machines over `resolve_pick_point`. `update` takes a `SpatialPointer` and returns true when the session changed; the state is `Idle`, `Pending`, `Orienting`, `Committed` or `Canceled`; a session locks onto the pointer id that started it. `PointPickSession` previews the point under the pointer and commits it on release. `PosePickSession` fixes the position at the press and takes the heading from the drag; its `PosePickConfig` defaults are a minimum drag of 0.05 m, a commit without heading and a default yaw of 0. `AreaPickSession` makes a rectangle, a circle or a polygon on the plane of the target. `make_nav_goal` turns a pose pick into a `NavGoal`, flattened to the ground, in the frame `map` unless `NavGoalOptions` says otherwise. In the code, `source` is the pointer source of the host.

```cpp
const qar_ar_core::RoomToRosTransform transform = qar_ar_core::room_to_ros_from_environment();
qar_ar_core::PointPickSession point_pick(qar_ar_core::PickTarget::action_point());
qar_ar_core::PosePickSession pose_pick(qar_ar_core::PickTarget::ground());

std::vector<qar_ar_core::PanelPointerSample> samples;
source->drain(samples);
for (const qar_ar_core::PanelPointerSample & sample : samples) {
  const qar_ar_core::SpatialPointer pointer = qar_ar_core::spatial_pointer_from_sample(sample, transform);
  if (!pointer.valid) {
    continue;                                              // a pointer on a panel is not a spatial one
  }
  point_pick.update(pointer);                              // a host feeds the session of the kind of pick
  pose_pick.update(pointer);                               // that the operator asked for
}

if (point_pick.state() == qar_ar_core::PickState::Committed) {
  const qar_ar_core::SpatialPoint & point = point_pick.result();
  std::printf("picked %.2f %.2f %.2f by %s\n", point.position.x, point.position.y, point.position.z,
    qar_ar_core::to_string(point.method));
}
if (pose_pick.state() == qar_ar_core::PickState::Committed) {
  const qar_ar_core::NavGoal goal = qar_ar_core::make_nav_goal(pose_pick.result());
  std::printf("goal in frame %s, yaw %.2f rad\n", goal.frame_id.c_str(), goal.yaw_rad);
}
```

### The timing log

`TimingLog` is a set of CSV files for the stages of a stream, written by one background thread. It exists only when the variable `QAR_VIDEO_TIMING_DIR` names a folder: `open` returns null otherwise, and every hook is then one null test. A row is a list of 64-bit integers that `TimingSink::append` queues under a mutex, with no formatting and no file I/O on the calling thread. A sink keeps at most 65536 rows in memory and counts the rest in `rows_dropped`. The host calls `TimingLog::shutdown` from its destructor, which joins the writer and flushes.

## qar_ogre_streaming

The module renders an Ogre scene from the eye poses of QarOS and hands the frames to a sender. Its public headers forward-declare the Ogre types, so a file that includes only the types does not include an Ogre header.

### Manager and streams

`OgreQarStreamingManager` is the entry point. Its constructor throws `std::invalid_argument` when `scene_manager`, `render_thread_executor` or `qar_sender_factory` of the `OgreQarStreamingConfig` is null; the diagnostics sink, the render budget, the render lock, the clock, the fence hook and the lighting rig are optional. In the code, `scene`, `executor` and `senders` are the Ogre scene manager, the render thread executor and the sender factory of the host, and `sink` is a diagnostics sink.

```cpp
qar_ogre_streaming::OgreQarStreamingConfig config;
config.scene_manager = scene;                        // required
config.render_thread_executor = executor;            // required
config.qar_sender_factory = senders;                 // required
config.diagnostics = sink;                           // optional
config.render_budget.max_batch_milliseconds = 12.0;  // optional: the time budget of one batch
config.fence_hook = std::make_shared<qar_ogre_streaming::GlFenceHook>();   // optional

try {
  qar_ogre_streaming::OgreQarStreamingManager manager(config);
  manager.set_streaming_active(true);
} catch (const std::invalid_argument & refused) {
  std::printf("manager: %s\n", refused.what());      // a required field was null
}
```

The library reports through `OgreQarDiagnosticsSink` on the render thread, so a sink must not block:

```cpp
// The library reports through this interface, on the render thread: a sink must not block.
class StderrSink : public qar_ogre_streaming::OgreQarDiagnosticsSink
{
public:
  void on_message(
    qar_ogre_streaming::DiagnosticSeverity severity, const std::string & category,
    const std::string & message, const std::string & peer_id_hex) override
  {
    std::fprintf(stderr, "[%s] %s %s %s\n", qar_ogre_streaming::to_string(severity), category.c_str(),
      peer_id_hex.c_str(), message.c_str());
  }
};
```

A `StereoStreamConfig` names the peer, the size of each eye, the frame rate cap and the formats; `validate` refuses a zero size, a zero frame rate and a layout other than separated textures.

```cpp
qar_ogre_streaming::StereoStreamConfig config;
config.peer_id = peer;
config.width = 1920;                 // pixels of each eye
config.height = 1080;
config.max_fps = 60;                 // the cap of this stream; the default is 72
const qar_ogre_streaming::Result valid = config.validate();
if (!valid.ok()) {
  std::printf("stream config refused: %s (%s)\n", valid.message.c_str(),
    qar_ogre_streaming::to_string(valid.status));
}
```

`OgreQarStereoStream::last_eye_poses(left, right)` gives the eye poses of the last frame that the stream accepted, as the sender reported them, which is the app content space for a sender that is bound to an app volume, in the convention of QarOS and not converted to Ogre space. Unlike the head position of the frame that is being rendered it stays readable after the frame is done, a frame that the sender declined does not replace it, and it returns false before the first accepted frame.

`create_stream` returns `AlreadyExists` for a peer that has a stream, `InvalidArgument` for an invalid configuration and `InvalidState` when the sender factory returns null; `destroy_stream` returns `NotFound` for an unknown peer. A new stream is not active: `set_active(true)` on the stream, and `set_streaming_active(true)` on the manager, come before `render_tick` renders anything. `render_tick` runs on the render thread and returns `RenderThreadRequired` anywhere else, without touching Ogre. `diagnostics_snapshot` returns a copy of all counters, and `SkipReason` says why a tick produced no frame: `SenderNotReady`, `SenderError`, `TargetInitFailed` or `BudgetShed`. In the code, `manager` is the manager and `peer` is the id of a peer that QarOS asked a stream for.

```cpp
qar_ogre_streaming::StereoStreamConfig stream;
stream.peer_id = peer;
const qar_ogre_streaming::Result created = manager->create_stream(stream);
if (!created.ok()) {
  std::printf("create_stream: %s (%s)\n", created.message.c_str(),
    qar_ogre_streaming::to_string(created.status));  // for example AlreadyExists: one stream for each peer
  return;
}
manager->get_stream(peer)->set_active(true);        // a stream renders only while it is active

// On the render thread, once for each tick of the host:
const qar_ogre_streaming::Result ticked = manager->render_tick();
const qar_ogre_streaming::DiagnosticsSnapshot snapshot = manager->diagnostics_snapshot();
for (const qar_ogre_streaming::StreamDiagnostics & item : snapshot.streams) {
  std::printf("%s: %llu frames, %.1f fps, last skip: %s\n", item.peer_id_hex.c_str(),
    static_cast<unsigned long long>(item.frames_submitted), item.achieved_fps,
    qar_ogre_streaming::to_string(item.last_skip_reason));
}
if (!ticked.ok()) {
  std::printf("render_tick: %s\n", ticked.message.c_str());
}

manager->destroy_stream(peer);
```

The host supplies four small interfaces. `IRenderThreadExecutor` has `post_to_render_thread` and `is_render_thread`; [Getting started](GETTING_STARTED.md#use-the-library-from-your-own-ogre-application) shows one for a program whose main thread renders. `IRenderLock` has `lock_render` and `unlock_render`, which the scheduler takes once around a whole batch, and none means no locking. `IClock` has `now_seconds` and is a `SteadyClock` by default. `IGlFenceHook` has `insert_fence`, and `GlFenceHook` is its implementation; no hook means no fence step.

`RenderBudget` decides which due streams render in a tick: a stream that has not rendered for 1 / `floor_fps` seconds is always admitted, and the others are admitted while the estimated cost stays within `max_batch_milliseconds` and `max_streams_per_tick`; the rest are shed and counted. It has no clock of its own, because the caller passes the time in seconds. [Render scheduling and budget](ARCHITECTURE.md#render-scheduling-and-budget) describes the policy.

### Pose and projection

`qar_pose_to_ogre` and `qar_orientation_to_ogre` reorder the quaternion from xyzw to wxyz and keep the rotation; the position is copied unchanged. `qar_pose_to_ogre` returns `InvalidArgument` for a quaternion shorter than `kMinQuaternionNorm`, and `qar_orientation_to_ogre` falls back to the identity. `make_projection_from_qar_fov` builds an off-axis projection, row-major, with the depth range of OpenGL, and returns `InvalidArgument` for a degenerate frustum: a near distance that is not positive, a far distance not beyond the near one, an empty angle range, or an angle of 90 degrees or more. `compute_eye_render_params` does both for one `EyeView` without Ogre. [Pose, projection and world basis](ARCHITECTURE.md#pose-projection-and-world-basis) has the conventions. In the code, `pose` and `fov` are the pose and the field of view that QarOS reports for one eye.

```cpp
// One eye of a frame, in the two calls that the stream makes: the pose, then the off-axis projection.
qar_ogre_streaming::Vec3 position;
qar_ogre_streaming::Quat orientation;                // w, x, y, z: the order of Ogre
const qar_ogre_streaming::Result posed = qar_ogre_streaming::qar_pose_to_ogre(pose, position, orientation);
qar_ogre_streaming::Mat4 projection;                 // row-major, the depth range of OpenGL
const qar_ogre_streaming::Result projected =
  qar_ogre_streaming::make_projection_from_qar_fov(fov, 0.1f, 500.0f, projection);
if (!posed.ok() || !projected.ok()) {
  std::printf("degenerate eye: %s %s\n", posed.message.c_str(), projected.message.c_str());
}

// Or both at once, as plain values and without Ogre:
qar_ogre_streaming::EyeView eye;
eye.pose = pose;
eye.fov = fov;
eye.near_far = QarNearFar{0.1f, 500.0f};
const qar_ogre_streaming::EyeRenderParams params = qar_ogre_streaming::compute_eye_render_params(eye);
std::printf("pose valid: %d, projection valid: %d\n", params.pose_valid ? 1 : 0, params.projection_valid ? 1 : 0);
```

### Lighting and overlay

`QarSceneLighting` adds a stable light rig to the shared scene. `SceneLightingMode` is `kOff`, `kOmni` (the default), `kPerPeer` or `kOmniAndPerPeer`, and `parse_scene_lighting_mode` accepts `off`, `omni`, `per-peer` and `both`, in any case, with `_` or `-`. The class creates only lights and scene nodes of its own and restores the ambient colour and the light that it suppressed in `shutdown`; the manager applies the rig in `set_streaming_active(true)` and restores the scene when streaming stops. The constructor throws `std::invalid_argument` for a null scene or an invalid configuration. In the code, `scene` is the Ogre scene manager and `config` the configuration of the manager.

```cpp
qar_ogre_streaming::SceneLightingConfig lighting;
qar_ogre_streaming::parse_scene_lighting_mode("both", lighting.mode);   // off, omni, per-peer or both
lighting.suppress_light_name = "MainDirectional";    // the headlight of the host, hidden while streaming
config.scene_lighting = std::make_shared<qar_ogre_streaming::QarSceneLighting>(scene, lighting);
```

`QarSpatialPickVisual` draws the aim and the pending pick into the shared scene, so that the headset shows them. The geometry comes from pure functions (`append_ray`, `append_point_marker`, `append_pose_marker`, `append_polygon`, `append_circle`); `stage` hands a finished `PickVisualGeometry` over from any thread and `apply_pending` draws it on the render thread.

## Component capi

The component holds the loader of the QarOS library, the slot arena, `IQarSessionOwner` and the timer of QarOS calls. The translation unit `qar_capi_loader.cpp` defines the function tables with `QAR_IMPLEMENT_DYNAMIC_LOADING()`, which the QarOS header allows once in a program, and every other component links the component. Nothing links an import library: `qar_library_load` resolves the functions at run time, and must be called before any other QarOS call. The session owner calls it with no path, which takes the installed QAROS; [The QarOS SDK](INSTALL.md#the-qaros-sdk) lists the order of the lookup.

`IQarSessionOwner` has the members `ready()`, `session()` and `post_session_task()`. A panel takes its stream from a video sender of the same session, so the panel factories borrow the session of an owner and never create one. `QarSessionOwner` is the owner of a program, and `QarCApiSenderFactory` forwards the calls to it. A QarOS call that can wait for the Hub, such as a query, a change of a panel or a subscription, must not run on a GUI thread, so a consumer hands it to `post_session_task(name, task, dropped)`: the thread that owns the session runs the tasks one at a time, in the order they were posted, and runs `dropped` instead when the session is gone or the drain budget of a disconnect, 8 seconds, has run out. A host without an executor gets the default of the interface, which runs the task at once on the calling thread.

```cpp
// A host that streams panels without a 3D scene opens its own QarOS session and answers the two questions of
// the owner interface. The panel factories borrow the session; they never destroy it.
class HostSessionOwner : public qar_qar_adapter::IQarSessionOwner
{
public:
  explicit HostSessionOwner(QarSession * session) : session_(session) {}

  bool ready() const override { return session_ != nullptr; }
  QarSession * session() const override { return session_; }

private:
  QarSession * session_;   // created and joined by the host, and destroyed by it after the last panel
};
```

Every gesture subscription passes QarOS a `user_state` pointer to a slot of a process-wide arena. A slot is never freed or reused, so the pointer always addresses live memory. `retire_slot` clears the owner under the mutex of the slot, and once it returns no callback is inside the owner and none will be again. `slot_arena_size` counts the slots, and at 4096 slots `mint_slot` returns null and the subscription fails.

The macro `QAR_CALL(fn, args...)` times one call of the QarOS C API, and `QAR_GUI_TIMED(name)` times a scope. On the thread that `mark_gui_thread()` marked, a call that takes longer than 20 milliseconds, or the value of `QAR_GUI_CALL_WARN_MS`, prints one line that starts with `[qar-gui-call]` and ends with `took %.0f ms on the GUI thread (limit %.0f ms)`, the first 5 times for each call name and then once in 100. On the thread that `mark_owner_thread` marked, a call of 500 milliseconds or longer goes to the report function of that thread.

## Component session

The component holds the session owner. `QarSessionOwner` loads the library, creates the runtime and joins the QAROS Hub on a thread of its own, keeps the table of targets, and hands the open session to one consumer thread, so that the consumer, the GUI thread of RViz or the main thread of a console program, never blocks on QAROS to connect.

| Member | What it does |
|---|---|
| `connect()`, `disconnect()`, `cancel()` | start joining; release the session and keep the stored pairing; stop the wait for an approval, a rejoin or a pairing |
| `submit_code(code, hub)` | pairs with the Hub of another PC with a pairing code; the code is a secret, it is not stored or logged, and a host or a port of 0 is refused |
| `forget()` | disconnects, erases the stored pairing in the runtime and deletes the identity file; remote mode only |
| `configure(mode, remote)` | sets the Hub mode and the host and port that the next `connect()` uses; while the owner joins, runs or stops, the values are kept for the next `connect()` |
| `set_target_listener(listener)` | sets who hears about targets; a new listener first hears of the targets that have a stream |
| `poll(out)` | moves the events since the last call into `out`: state changes, progress, targets that come and go, the answers of an invite and of a forget, and log lines |
| `state()`, `effective_mode()`, `has_remote_identity()`, `targets()`, `self_peer()`, `runtime_description()` | what the owner knows |
| `invite_device(kind, host)` | asks the Hub of this PC to start the app of a HoloLens or a headset; it returns an empty text when the request started and the refusal otherwise, and the answer arrives as an event |
| `ready()`, `session()`, `post_session_task()` | the interface `IQarSessionOwner` |
| `task_stats()` | the counters of the task queue for the heartbeat line |

`SessionState` runs from `kIdle` through `kLoading`, `kWaitingForApproval` (this-PC mode: the Hub asks the person at the PC to allow the program), `kRejoining`, `kNeedsCode`, `kPairing` and `kRunning` to `kReconnecting` (joined, but the Hub does not answer; the session stays valid), `kStopping` and `kFailed`. `HubMode` is `kAuto`, `kLocal` or `kRemote`: an explicit request wins, and in auto mode a stored pairing means remote, an installed QAROS means local, and anything else means remote. A source app on the PC of the Hub is `kLocal`: the Hub asks for approval once and no code is needed. A Hub on another PC is `kRemote`: a pairing code once, then a stored pairing.

`SessionOwnerConfig` names the program (`program_tag`, which names the folders), a second copy of it (`instance`), the display name that the Hub shows in its approval prompt, the mode, the remote host and port, the folder of the identity (`identity_dir`; empty means `default_identity_dir(program_tag, instance)`), the library file (`library_path`), the approval timeout (`approval_timeout_ms`, 0 for the default of the Hub and at most 600000), the cap on served targets (`max_targets`, 8), and the log settings of the library. A test replaces the loader of the real library with an `ILibraryLoader`.

The identity of a program is kept in the folder `<base>\<program>` or `<base>\<program>.<instance>`, where the base is the environment variable `QAR_PANEL_IDENTITY_DIR` when it is set and `%LOCALAPPDATA%\Quaternar\QarOS-ROS` otherwise. `identity.txt` there holds the onboarding id and the host and port of the paired Hub; it is written in one step and replaces the earlier file whole, and a program that joins the Hub of its own PC writes none. The folder also holds `runtime`, the storage folder that the session owner gives to QAROS in every mode. `describe_status(code, context, detail, hub_address)` turns a status code of a call into plain words for the status strip, and says whether trying the same action again can help.

## Component render

The component holds `QarCApiSenderFactory`, which is the `IQarRenderSenderFactory` of the manager, an `IQarSessionOwner` and an `ITargetListener`, and `QarCApiRenderSenderAdapter`, the render sender of one peer. The factory is made from a `QarSessionOwner` that the program keeps alive, and it must be owned by a `std::shared_ptr`; it borrows the session of the owner once the owner is ready, and it keeps only a weak reference to the owner. `begin_attach()` starts the attach on the thread that owns the session: it gets or creates the app volume, reads its placement once, subscribes to the placement and registers the factory as the listener of targets, and `attach_state()` says where it is. `attach()` does the same and waits, so only a console program calls it. The fields of `Config` that most hosts set are these, and [Configuration](CONFIGURATION.md#runtime-configuration-structs) lists every field and its default:

```cpp
qar_qar_adapter::QarCApiSenderFactory::Config config;
config.copy_hook = qar_qar_adapter::make_gl_copy_image_hook();   // without it QarOS shows a black scene
config.near_far = QarNearFar{0.1f, 200.0f};                      // reported with every frame
config.max_peers = 4;                                            // a fifth peer is refused with a warning
config.name_scope = qar_ar_core::make_name_scope("fleet-map", "", host);   // names the app volume
config.app_volume_name = "Fleet map";
config.app_volume_width_m = 20.0f;                               // the box of the scene, in metres
config.app_volume_length_m = 20.0f;
config.app_volume_height_m = 10.0f;
config.app_volume_center_y_m = 5.0f;                             // the centre of the box above room y = 0
```

| Member | What it does |
|---|---|
| `begin_attach()`, `attach_state()`, `attach()`, `attached()`, `last_error()` | the attach to the session of the owner and its outcome; `last_error()` says what failed |
| `detach()` | stops listening and lets go of the app volume; call it after every sender is destroyed and before the owner disconnects |
| `capture_gl_context()` | latches the WGL context that is current on the calling thread; call it on the render thread once for each tick |
| `poll_new_peer_requests(out)` | appends the targets that need a stream, and forgets them |
| `create_sender(peer)` | the render sender of a peer; the first call starts an asynchronous creation and returns null, and later calls poll it |
| `on_render_request(peer)` | asks again for a peer whose creation is not done |
| `poll_closed_peers(out)`, `forget(peer)` | appends the peers whose stream is over; it lists them until `forget` is called after the stream is destroyed |
| `app_volume(out)`, `scene_placement(out)`, `pointing_ray_slot()` | the app volume of the factory, where QarOS shows the scene (the last placement that was seen, never a call), and the slot of the aiming ray |
| `set_geo_anchor_wgs84`, `set_geo_anchor_ecef`, `clear_geo_anchor`, `take_geo_anchor_result`, `has_geo_anchor` | the geo anchor of the app volume: the setters queue the change and return an empty text when it was queued, and `take_geo_anchor_result` returns the outcome once |
| `session()`, `ready()`, `post_session_task()` | the borrowed session and the executor of the owner |

The geo anchor setters return an empty text on success and the reason otherwise. In the code, `factory` is the factory of the host and `owner` its session owner.

```cpp
if (!factory->attach()) {
  std::printf("QarOS: %s\n", factory->last_error().c_str());   // no session, or no app volume: nothing else works
  return;
}

// Ask the Hub of this PC to start the app on one more device. An empty text means that the request
// started; the answer arrives later as an event of the owner.
const std::string refused = owner->invite_device(qar_qar_adapter::DeviceKind::kHoloLens, "HOLOLENS-C4D5");

// Georeference the app volume: latitude, longitude, and the height above the WGS84 ellipsoid.
const std::string anchor_error = factory->set_geo_anchor_wgs84(47.3769, 8.5417, 408.0);
std::printf("invite: %s, geo anchor: %s\n", refused.c_str(), anchor_error.c_str());   // empty: it worked
```

A target gets a stream when the session owner serves it, `max_peers` of the `Config` limits the streams on top of the cap of the owner, and a further target is refused with a logged warning. A stream needs the QarOS textures to be written: `make_gl_copy_image_hook()` returns the blit that copies the rendered textures into them, and null on a platform without WGL, and without a hook the adapter skips the copy and QarOS shows a black scene. `GlSubmitMode` is `kRenderIntoQarTextures`, the default and the only mode that works against the SDK, or `kSupplyOwnTextureIds`. The hook can also read the mean colour of the left colour texture of a frame back from the GPU, `mean_left_colour(frame, rgb)`, each channel from 0 to 255; it reads the whole texture, so it is for a line about a single frame and never for every frame, and the default cannot read textures back and reports false. The adapter uses it once, for the first frame that follows a stall of two seconds or more, and prints the colour in the line about that frame. A sender binds its own OpenGL context on the device context that it was created on, and a sender with a missing handle binds nothing: nothing stands in for it, neither the handles of the factory nor the context of Ogre, which is current on the render thread and must never be bound on a second one.

When the sender factory returns null for a peer, the manager logs one Error line for the first null and one summary line, Info or Warning, when the wait ends, with the number of nulls and the milliseconds that it took.

`geodetic_to_ecef_enu` converts a latitude, a longitude and a height above the WGS84 ellipsoid into an ECEF origin and the East, North and Up axes, and returns false for a fix that is not a number or out of range. `geo_fix_moved` tells whether two fixes are far enough apart to anchor again, and returns true when a fix is unusable.

## Components panels, url and gestures

All three need only `qar_ar_core` and `capi`: no Ogre and no Qt. The two panel factories take an `IQarSessionOwner`, which must outlive them. A panel factory returns null from `create_panel_sender` or `create_url_panel` when the owner is not ready, and `last_error()` says so.

**`panels`.** `QarCApiPanelSenderFactory` creates a video sender of the session, takes its stream id, and adds a QarOS panel whose content is that stream. The panel and its video sender are made, changed and destroyed by tasks on the thread that owns the session, so the calls of the adapter return at once and `drain_results` reports the outcomes; a panel whose name is taken by an earlier panel is reused (`reattached()`), and `common_name()` says the name it ended up with. When it was given a `QarCApiPanelInputSource`, every panel that it creates subscribes to its gestures at once, and `close()` cancels the subscription first. A panel without input is better than no panel: a failed subscription is logged and the panel still streams. The source drains pointer samples, and the keys of the panels that the operator closed in AR, on the thread of the host. It has one reader for each panel (`qar_gui_panels_subscribe_all_gesture_events`), because separate readers have no order between them. An open gesture that stays silent for `set_lifecycle_timeout` seconds, 0.75 by default, is cancelled with a synthetic sample. The source also follows the record that the runtime keeps of each panel: from the creation of the panel, each change of its state, of the stream that is its content or of its size is read in the callback, queued, and printed by the next `drain`, with `match` or `MISMATCH` against the stream that the sender was built with. In the code, `owner` is an `IQarSessionOwner`.

```cpp
// One input source serves all panels; the factory hands it to every panel that it creates.
auto input = std::make_shared<qar_qar_adapter::QarCApiPanelInputSource>(owner->session());
qar_qar_adapter::QarCApiPanelSenderFactory factory(owner, input);

qar_ar_core::PanelDesc desc;
desc.key = "status";
desc.buffer_width = 1024;
desc.buffer_height = 768;
desc.content_width = 1000;                           // the part of the buffer that the widget paints;
desc.content_height = 760;                           // zero means the whole buffer
const std::shared_ptr<qar_ar_core::IQarPanelSenderAdapter> sender = factory.create_panel_sender(desc);
if (!sender) {
  std::printf("panel: %s\n", factory.last_error().c_str());
  return;
}

// The panel is subscribed to its gestures. On the GUI thread, once for each tick:
std::vector<qar_ar_core::PanelPointerSample> samples;
input->drain(samples);
std::vector<std::string> closed;
input->drain_closed_panels(closed);                  // the keys of the panels that the operator closed in AR
std::printf("%zu samples, %zu closed, %zu subscriptions\n", samples.size(), closed.size(),
  input->subscription_count());
```

The adapters keep the state they asked for. When QarOS reports the opposite one, `HIDDEN` for `VISIBLE` or the reverse, within 5 seconds of the change, the adapter asks again after a hold of one second, at most 3 times within 10 seconds, and a confirming report or the adapter's own state restarts the count; a state that the operator sets later in AR, and `MINIMIZED` or `CLOSED`, are never contested. The decision is the pure `PanelStateKeeper` of `panel_state_guard.hpp`, and the guard prints a line for each ask and a warning when QarOS keeps reporting the opposite after the last ask.

**`url`.** `QarCApiUrlPanelFactory` adds a QarOS panel whose content is a URL, which the browser of QarOS loads. It refuses an empty URL and a size that is not positive, and it needs no input source: the page talks to its own server. Its adapter works like the panel adapter: the calls queue their work for the thread that owns the session and `drain_results` reports the outcomes. In the code, `owner` is an `IQarSessionOwner`.

```cpp
qar_qar_adapter::QarCApiUrlPanelFactory factory(owner);
qar_ar_core::UrlPanelDesc desc;
desc.url = "http://127.0.0.1:8080/controls";         // the browser of QarOS loads this page
const std::shared_ptr<qar_ar_core::IQarUrlPanelAdapter> panel = factory.create_url_panel(desc);
if (panel) {
  panel->set_state(QAR_GUI_PANEL_STATE_MINIMIZED);
  panel->close();
} else {
  std::printf("url panel: %s\n", factory.last_error().c_str());
}
```

**`gestures`.** `QarCApiSpatialGestureSource` turns the gestures of the app volume that the factory created into pointer samples. The volume keeps the default gesture configuration of the runtime, which the factory passes as NULL, and `subscribe` registers one reader for all kinds with `qar_app_volumes_subscribe_all_gesture_events`; if that call fails it falls back to one reader for `QAR_GESTURE_CLICK` and one for `QAR_GESTURE_HOVER`. A manipulation kind is counted in `events_other_kind` and never queued. `stats().events_received` is the number to read first: it counts every callback before any filter, so zero means that QarOS sends nothing. In the code, `factory` is the `QarCApiSenderFactory` of the host.

```cpp
QarAppVolumeId volume{};
if (!factory->app_volume(volume)) {
  return;                                            // no app volume, so no gestures to subscribe to
}
qar_qar_adapter::QarCApiSpatialGestureSource gestures(factory->session());
gestures.set_pointing_ray_slot(factory->pointing_ray_slot());   // the ray from the hands, as a fallback
if (!gestures.subscribe(volume)) {
  std::printf("gestures: %s\n", gestures.last_error().c_str());
  return;
}

std::vector<qar_ar_core::PanelPointerSample> samples;
gestures.drain(samples);                             // on the thread of the host, once for each tick
const qar_qar_adapter::SpatialGestureStats stats = gestures.stats();
std::printf("%llu events received, %llu queued\n", static_cast<unsigned long long>(stats.events_received),
  static_cast<unsigned long long>(stats.events_queued));
gestures.unsubscribe();
```

## Environment helpers

The functions of `env_config.hpp` take the raw text of a variable and never read the environment, so they run without a QarOS session. A host passes the result of `std::getenv(name)`, and a null pointer means that the variable is not set. `read_environment(name)` and `environment_is_set(name)` read the environment themselves, in wide characters on Windows, so that a folder such as `TEMP` with letters outside the code page of the account arrives whole.

| Function | Rule |
|---|---|
| `parse_bool_env(value, default)` | `1`, `true`, `on`, `yes` are true and `0`, `false`, `off`, `no` are false, in any case and without trimming; anything else, an empty text and null give the default |
| `frame_trace_requested(value)` | true for any non-null text, an empty one included |
| `parse_tick_ms_env(value, default)` | a finite number from 0 to 100000, fractions allowed; null, text without a leading number, `nan`, `inf`, a negative and a larger number give the default |
| `parse_log_severity(text, out)` | `trace`, `debug`, `info`, `warn`, `warning`, `error`, `fatal`, in any case, trimmed; null, empty text, numbers and other words return false and leave `out` untouched |
| `resolve_library_logging(panel, vendor, console, default)` | the severity comes from the first of the host's variable, the vendor's variable and the default; an unusable value is skipped and reported in `warning`; the console copy is on for `trace`, `debug` and `info`, off above, unless the console variable says otherwise |
| `zenoh_logging_allowed(severity)` | true only for `trace` and `debug` |

`resolve_library_logging` sets `environment_overridden` when the vendor's variable differs from the decision, because the QarOS library obeys the variable and not the init struct. A host puts the decision into `SessionOwnerConfig::log_severity`, and the session owner exports that severity as `QAR_LOG_SEVERITY` before `qar_library_init`.

```cpp
// The helpers take the raw text of a variable and never read the environment. A host passes the result of
// std::getenv(name) and a null pointer means that the variable is not set.
const bool prime = qar_qar_adapter::parse_bool_env("off", true);        // 1/true/on/yes and 0/false/off/no
const bool trace = qar_qar_adapter::frame_trace_requested("any text");  // set to anything: requested
const double tick_ms = qar_qar_adapter::parse_tick_ms_env("0.5", 0.1);  // the second argument is the fallback

qar_qar_adapter::LogSeverity severity = qar_qar_adapter::LogSeverity::kTrace;
const bool known = qar_qar_adapter::parse_log_severity(" Warn ", severity);   // case and spaces do not matter

// The severity of the host, the severity that QarOS reads itself and the console switch decide how the
// library of QarOS logs.
const qar_qar_adapter::LibraryLogging logging = qar_qar_adapter::resolve_library_logging(
  "warn", "trace", nullptr, qar_qar_adapter::LogSeverity::kTrace);
std::printf("%d %d %.1f %d | %s from %s, console %d, overwrite the variable of QarOS %d, zenoh %d\n", prime,
  trace, tick_ms, known, qar_qar_adapter::log_severity_name(logging.severity), logging.severity_source,
  logging.console, logging.environment_overridden,
  qar_qar_adapter::zenoh_logging_allowed(logging.severity));
```

## Errors and thread rules

| Call | Thread | Blocking | Failure is reported as |
|---|---|---|---|
| constructor of `QarSessionOwner`, `connect`, `submit_code`, `forget`, `configure` | any | none: the work runs on the thread of the owner | a `SessionEvent` from `poll`; nothing is thrown |
| `poll`, `invite_device`, `targets`, `ready`, `session` | the consumer thread | none | an event, or the refusal as text |
| `post_session_task` | any | none | `false` when no session runs |
| `begin_attach`, `attach_state`, `detach` of `QarCApiSenderFactory` | the consumer thread | none (`attach` waits and is for console programs) | `false` and `last_error()` |
| `create_sender`, `capture_gl_context` | the render thread | none: the creation is asynchronous | null or false; the reason is a line on stderr |
| constructors of `OgreQarStreamingManager` and `QarSceneLighting` | any | none | `std::invalid_argument` |
| `render_tick`, the phases of a stream, `QarSceneLighting` | the render thread, under the render lock of the host | GL work | a `Result`; `RenderThreadRequired` off the render thread |
| `QarSpatialPickVisual` | `stage` on any thread, the rest on the render thread | none | a `Result` from `initialize` |
| calls of `IQarPanelSenderAdapter` and of `IQarUrlPanelAdapter` | the GUI thread of the host | none: the work is queued for the thread of the owner, and `try_begin_frame` does not block | `false` and `last_error()`; the outcome of a queued call arrives through `drain_results`; `close` returns a report |
| `subscribe_panel`, `unsubscribe_panel` of the panel input source | the thread that owns the session | each can wait for the Hub | `false` and `last_error()` |
| `drain` of the panel input source | the GUI thread of the host | none | none |
| `QueuedPanelInputSource`, `PointingRaySlot`, `TimingSink::append` | any | one short mutex | none |
| the callbacks of QarOS | a thread of the QarOS library | they only convert and queue, take one lock, and never subscribe | none |

[Threading model](ARCHITECTURE.md#threading-model) explains why the rules are what they are.

## What the package installs

```powershell
Get-ChildItem -Recurse -File "$env:QAR_PREFIX" | ForEach-Object { $_.FullName.Substring("$env:QAR_PREFIX".Length + 1) }
```
```text
include\qar_streaming.h
include\qar_ar_core\common_name.hpp
include\qar_ar_core\panel_attach.hpp
include\qar_ar_core\panel_input_source.hpp
include\qar_ar_core\panel_types.hpp
include\qar_ar_core\pointing_ray.hpp
include\qar_ar_core\qar_panel_sender.hpp
include\qar_ar_core\qar_streaming_c.hpp
include\qar_ar_core\qar_url_panel.hpp
include\qar_ar_core\spatial_frames.hpp
include\qar_ar_core\spatial_pick.hpp
include\qar_ar_core\timing_log.hpp
include\qar_ogre_streaming\clock.hpp
include\qar_ogre_streaming\diagnostics.hpp
include\qar_ogre_streaming\gl_fence_hook.hpp
include\qar_ogre_streaming\gl_interop.hpp
include\qar_ogre_streaming\mock_qar_frame_info_source.hpp
include\qar_ogre_streaming\ogre_opengl_texture_bridge.hpp
include\qar_ogre_streaming\ogre_qar_render_scheduler.hpp
include\qar_ogre_streaming\ogre_qar_stereo_stream.hpp
include\qar_ogre_streaming\ogre_qar_streaming_manager.hpp
include\qar_ogre_streaming\ogre_stereo_render_target.hpp
include\qar_ogre_streaming\qar_pose_conversion.hpp
include\qar_ogre_streaming\qar_render_sender_adapter.hpp
include\qar_ogre_streaming\qar_scene_lighting.hpp
include\qar_ogre_streaming\render_budget.hpp
include\qar_ogre_streaming\render_lock.hpp
include\qar_ogre_streaming\render_thread_executor.hpp
include\qar_ogre_streaming\spatial_pick_visual.hpp
include\qar_ogre_streaming\types.hpp
include\qar_qar_adapter\env_config.hpp
include\qar_qar_adapter\frame_stall_probe.hpp
include\qar_qar_adapter\geo_anchor.hpp
include\qar_qar_adapter\gui_call_guard.hpp
include\qar_qar_adapter\identity_store.hpp
include\qar_qar_adapter\log_time.hpp
include\qar_qar_adapter\onboarding_status.hpp
include\qar_qar_adapter\panel_state_guard.hpp
include\qar_qar_adapter\panel_state_log.hpp
include\qar_qar_adapter\qar_capi_input_slots.hpp
include\qar_qar_adapter\qar_capi_panel_input_source.hpp
include\qar_qar_adapter\qar_capi_panel_sender_adapter.hpp
include\qar_qar_adapter\qar_capi_render_sender_adapter.hpp
include\qar_qar_adapter\qar_capi_session_owner.hpp
include\qar_qar_adapter\qar_capi_spatial_gesture_source.hpp
include\qar_qar_adapter\qar_capi_url_panel_adapter.hpp
include\qar_qar_adapter\qar_session_owner.hpp
include\qar_qar_adapter\render_timing.hpp
include\qar_qar_adapter\target_rule.hpp
lib\qar_ar_core.lib
lib\qar_ogre_streaming.lib
lib\qar_qar_adapter_capi.lib
lib\qar_qar_adapter_gestures.lib
lib\qar_qar_adapter_panels.lib
lib\qar_qar_adapter_render.lib
lib\qar_qar_adapter_session.lib
lib\qar_qar_adapter_url.lib
share\qar_ogre_adapter\LICENSE
share\qar_ogre_adapter\LICENSE.qar_streaming_header
share\qar_ogre_adapter\cmake\qar_ogre_adapterConfig.cmake
share\qar_ogre_adapter\cmake\qar_ogre_adapterConfigVersion.cmake
share\qar_ogre_adapter\cmake\qar_ogre_adapterTargets-relwithdebinfo.cmake
share\qar_ogre_adapter\cmake\qar_ogre_adapterTargets.cmake
```

The command lists every file below the install prefix, relative to it. The package puts the headers of the three modules and the vendored `qar_streaming.h` below `include`, the static libraries below `lib`, and the CMake package files, with the licence of the package and the licence of the QarOS header, below `share/qar_ogre_adapter`.
