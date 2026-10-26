// Copyright 2026 QuaternAR
// SPDX-License-Identifier: MIT

#ifndef QAR_OGRE_STREAMING_TEST_SUPPORT__MOCK_QAR_RENDER_SENDER_ADAPTER_HPP_
#define QAR_OGRE_STREAMING_TEST_SUPPORT__MOCK_QAR_RENDER_SENDER_ADAPTER_HPP_

#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "qar_ogre_streaming/mock_qar_frame_info_source.hpp"
#include "qar_ogre_streaming/qar_render_sender_adapter.hpp"
#include "qar_ogre_streaming/types.hpp"

namespace qar_ogre_streaming
{
namespace test_support
{

/// @brief Records every begin_frame()/submit_opengl_stereo_frame() call with its
/// full arguments and returns scriptable results.
///
/// Used by unit tests and the standalone sample to verify that the library drives
/// the Qar adapter correctly without a real Qar runtime.
class MockQarRenderSenderAdapter : public IQarRenderSenderAdapter
{
public:
  MockQarRenderSenderAdapter() = default;
  explicit MockQarRenderSenderAdapter(const QarPeerId & peer_id) : peer_id_(peer_id) {}

  // --- Scripting -----------------------------------------------------------

  /// @brief Value written into out_info on the next begin_frame() call.
  void set_next_frame_info(const QarFrameInfo & info) { scripted_frame_info_ = info; }

  /// @brief Drive begin_frame() from an ANIMATED frame source instead of the
  /// single scripted value. When set, each begin_frame() returns
  /// source->frame_at(next_frame_index_++), so the sender yields the same
  /// deterministic per-eye poses/FOVs the source generates. Pass nullptr to
  /// revert to the scripted single-value behavior.
  void set_frame_source(std::shared_ptr<const MockQarFrameInfoSource> source)
  {
    frame_source_ = std::move(source);
  }

  /// @brief Frame index the source will be queried with on the next begin_frame().
  uint64_t next_frame_index() const { return next_frame_index_; }

  /// @brief Return value for begin_frame() (default true).
  void set_begin_frame_result(bool result) { begin_frame_result_ = result; }

  /// @brief Return value for submit_opengl_stereo_frame() (default true).
  void set_submit_result(bool result) { submit_result_ = result; }

  /// @brief Detail string reported by last_skip_detail() (default empty), so a
  /// test can prove the adapter's account of a skip reaches the diagnostics.
  void set_skip_detail(std::string detail) { skip_detail_ = std::move(detail); }

  // --- Recording -----------------------------------------------------------

  std::size_t begin_frame_calls() const { return begin_frame_calls_; }
  std::size_t submit_calls() const { return submitted_frames_.size(); }

  /// @brief Full copy of every submitted frame, in call order.
  const std::vector<QarOpenGlStereoFrame> & submitted_frames() const
  {
    return submitted_frames_;
  }

  /// @brief The QarFrameInfo values handed back on each begin_frame() call.
  const std::vector<QarFrameInfo> & begin_frame_infos() const { return begin_frame_infos_; }

  const QarPeerId & peer_id() const { return peer_id_; }

  // --- IQarRenderSenderAdapter --------------------------------------------

  bool begin_frame(QarFrameInfo & out_info) override
  {
    ++begin_frame_calls_;
    if (frame_source_) {
      out_info = frame_source_->frame_at(next_frame_index_++);
    } else {
      out_info = scripted_frame_info_;
    }
    begin_frame_infos_.push_back(out_info);
    return begin_frame_result_;
  }

  bool submit_opengl_stereo_frame(const QarOpenGlStereoFrame & frame) override
  {
    submitted_frames_.push_back(frame);
    return submit_result_;
  }

  const char * last_skip_detail() const override { return skip_detail_.c_str(); }

  // Recorded rather than acted on: this test double has no real "priming" behavior
  // (that is an implementation detail of QarCApiRenderSenderAdapter), but recording
  // the calls lets tests verify the panel -> manager -> stream -> adapter plumbing
  // reaches the sender, without needing a live QarSession.
  void set_prime_enabled(bool enabled) override
  {
    prime_enabled_ = enabled;
    ++set_prime_enabled_calls_;
  }
  void set_tick_ms(double tick_ms) override
  {
    tick_ms_ = tick_ms;
    ++set_tick_ms_calls_;
  }

  bool prime_enabled() const { return prime_enabled_; }
  double tick_ms() const { return tick_ms_; }
  std::size_t set_prime_enabled_calls() const { return set_prime_enabled_calls_; }
  std::size_t set_tick_ms_calls() const { return set_tick_ms_calls_; }

private:
  QarPeerId peer_id_{};

  QarFrameInfo scripted_frame_info_{};
  std::shared_ptr<const MockQarFrameInfoSource> frame_source_;
  uint64_t next_frame_index_ = 0;
  bool begin_frame_result_ = true;
  bool submit_result_ = true;
  std::string skip_detail_;

  std::size_t begin_frame_calls_ = 0;
  std::vector<QarFrameInfo> begin_frame_infos_;
  std::vector<QarOpenGlStereoFrame> submitted_frames_;

  bool prime_enabled_ = true;
  double tick_ms_ = 0.1;
  std::size_t set_prime_enabled_calls_ = 0;
  std::size_t set_tick_ms_calls_ = 0;
};

/// @brief Factory handing out MockQarRenderSenderAdapter instances and keeping a
/// handle to each so tests can inspect recorded calls after the manager owns them.
class MockQarRenderSenderFactory : public IQarRenderSenderFactory
{
public:
  /// @brief When set to true, create_sender() returns nullptr (to exercise the
  /// manager's sender-creation-failure path).
  void set_fail_creation(bool fail) { fail_creation_ = fail; }

  std::shared_ptr<IQarRenderSenderAdapter> create_sender(const QarPeerId & peer_id) override
  {
    if (fail_creation_) {
      return nullptr;
    }
    auto adapter = std::make_shared<MockQarRenderSenderAdapter>(peer_id);
    created_.push_back(adapter);
    return adapter;
  }

  /// @brief Every adapter created so far (shared with the manager).
  const std::vector<std::shared_ptr<MockQarRenderSenderAdapter>> & created() const
  {
    return created_;
  }

private:
  bool fail_creation_ = false;
  std::vector<std::shared_ptr<MockQarRenderSenderAdapter>> created_;
};

}  // namespace test_support
}  // namespace qar_ogre_streaming

#endif  // QAR_OGRE_STREAMING_TEST_SUPPORT__MOCK_QAR_RENDER_SENDER_ADAPTER_HPP_
