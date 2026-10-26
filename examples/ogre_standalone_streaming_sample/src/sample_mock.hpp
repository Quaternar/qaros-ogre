// Copyright 2026 QuaternAR
// SPDX-License-Identifier: MIT

#ifndef QAR_OGRE_SAMPLE__SAMPLE_MOCK_HPP_
#define QAR_OGRE_SAMPLE__SAMPLE_MOCK_HPP_

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "qar_ogre_streaming/diagnostics.hpp"
#include "qar_ogre_streaming/mock_qar_frame_info_source.hpp"
#include "qar_ogre_streaming/qar_render_sender_adapter.hpp"
#include "qar_ogre_streaming/types.hpp"
#include "qar_ogre_streaming_test_support/mock_qar_render_sender_adapter.hpp"

namespace qar_ogre_sample
{

/// @brief Factory handing out the library's recording MockQarRenderSenderAdapter,
/// one per peer, each driven by its OWN deterministic MockQarFrameInfoSource with a
/// DISTINCT orbit phase — so N peers frame the same scene from N viewpoints.
///
/// This mirrors AnimatedMockSenderFactory of the RViz plugin (qar_rviz_plugin) exactly, only
/// it reuses the library's test_support MockQarRenderSenderAdapter (which records every
/// submitted frame, so the sample can log real Qar frame metadata) instead of a
/// plugin-private sender. It implements the SAME public seam
/// (qar_ogre_streaming::IQarRenderSenderFactory) that OgreQarStreamingConfig takes.
class SampleMockSenderFactory : public qar_ogre_streaming::IQarRenderSenderFactory
{
public:
  struct PeerSource
  {
    QarPeerId peer_id{};
    std::shared_ptr<qar_ogre_streaming::MockQarFrameInfoSource> source;
    std::shared_ptr<qar_ogre_streaming::test_support::MockQarRenderSenderAdapter> sender;
  };

  explicit SampleMockSenderFactory(const qar_ogre_streaming::MockQarFrameInfoSource::Config & base)
  : base_(base)
  {
  }

  std::shared_ptr<qar_ogre_streaming::IQarRenderSenderAdapter> create_sender(
    const QarPeerId & peer_id) override
  {
    const std::uint32_t index = static_cast<std::uint32_t>(sources_.size());
    qar_ogre_streaming::MockQarFrameInfoSource::Config cfg = base_;
    apply_peer_offset(cfg, index);
    auto source = std::make_shared<qar_ogre_streaming::MockQarFrameInfoSource>(cfg);
    auto sender =
      std::make_shared<qar_ogre_streaming::test_support::MockQarRenderSenderAdapter>(peer_id);
    sender->set_frame_source(source);  // drive begin_frame() from the animated source
    sources_.push_back(PeerSource{peer_id, source, sender});
    return sender;
  }

  const std::vector<PeerSource> & sources() const { return sources_; }

  std::shared_ptr<const qar_ogre_streaming::MockQarFrameInfoSource> source_for(
    const QarPeerId & peer_id) const
  {
    for (const PeerSource & ps : sources_) {
      if (qar_ogre_streaming::peer_id_equal(ps.peer_id, peer_id)) {
        return ps.source;
      }
    }
    return nullptr;
  }

private:
  // Spread up to 8 peers around the orbit so views clearly differ (peer 0 = base).
  static void apply_peer_offset(
    qar_ogre_streaming::MockQarFrameInfoSource::Config & cfg, std::uint32_t index)
  {
    if (index == 0) {
      return;
    }
    constexpr float kTwoPi = 6.2831853071795864769f;
    cfg.start_yaw_rad += static_cast<float>(index) * (kTwoPi / 8.0f);
    const float pitch_delta = 0.12f * static_cast<float>(index % 3) - 0.12f;
    cfg.pitch_rad += pitch_delta;
  }

  qar_ogre_streaming::MockQarFrameInfoSource::Config base_{};
  std::vector<PeerSource> sources_;
};

/// @brief Diagnostics sink that prints the library's structured messages to stdout.
/// The RViz plugin implements OgreQarDiagnosticsSink by appending to its Qt log;
/// the sample implements the SAME interface with a plain printf.
class StdoutDiagnosticsSink : public qar_ogre_streaming::OgreQarDiagnosticsSink
{
public:
  void on_message(
    qar_ogre_streaming::DiagnosticSeverity severity,
    const std::string & category,
    const std::string & message,
    const std::string & peer_id_hex) override
  {
    std::printf(
      "[lib/%s] %s: %s%s%s\n",
      qar_ogre_streaming::to_string(severity), category.c_str(), message.c_str(),
      peer_id_hex.empty() ? "" : " peer=", peer_id_hex.c_str());
  }
};

}  // namespace qar_ogre_sample

#endif  // QAR_OGRE_SAMPLE__SAMPLE_MOCK_HPP_
