// Copyright 2026 QuaternAR
// SPDX-License-Identifier: MIT
//
// The plain-words table of status codes, and the two decisions of the session owner that need no
// QarOS call: which peer is a render target, and which mode a start uses.

#include <gtest/gtest.h>

#include <string>

#include "qar_qar_adapter/onboarding_status.hpp"
#include "qar_qar_adapter/target_rule.hpp"

namespace
{

using qar_qar_adapter::HubMode;
using qar_qar_adapter::OnboardingContext;
using qar_qar_adapter::describe_status;

TEST(OnboardingStatus, EveryCodeOfTheTableHasAMessageAndASymbol)
{
  const int codes[] = {
    qar_qar_adapter::kStatusNoRuntime,
    QAR_STATUS_ONBOARDING_FAILED,
    QAR_STATUS_ONBOARDING_HUB_UNREACHABLE,
    QAR_STATUS_ONBOARDING_APPROVAL_DENIED,
    QAR_STATUS_ONBOARDING_APPROVAL_TIMEOUT,
    QAR_STATUS_ONBOARDING_HUB_NOT_AUTHENTIC,
    QAR_STATUS_ONBOARDING_APP_IDENTITY_MISMATCH,
    QAR_STATUS_PAKE_ERROR,
    QAR_STATUS_ONBOARDING_SESSION_NOT_FOUND,
    QAR_STATUS_ONBOARDING_RE_PAIR_REQUIRED,
    QAR_STATUS_ONBOARDING_RENEWAL_POSSESSION_UNPROVEN,
    QAR_STATUS_ONBOARDING_SESSION_STILL_ACTIVE,
    QAR_STATUS_ARGUMENT_NOT_SUPPORTED,
    QAR_STATUS_NOT_IMPLEMENTED,
    QAR_STATUS_TIMEOUT,
    QAR_STATUS_HUB_LIFECYCLE_OPERATION_RUNNING,
    QAR_STATUS_SECURE_STORAGE_ERROR,
    QAR_STATUS_SECURE_STORAGE_NOT_FOUND,
    QAR_STATUS_SECURE_STORAGE_TAMPERED,
    QAR_STATUS_SECURE_STORAGE_PROTECTOR_MISMATCH,
    QAR_STATUS_SECURE_STORAGE_PROTECTOR_UNAVAILABLE,
    QAR_STATUS_SECURE_STORAGE_INVALID_KEY,
    QAR_STATUS_SECURE_STORAGE_LOCKED,
    QAR_STATUS_HUB_TARGET_APP_DEVICE_NOT_INVITABLE,
    QAR_STATUS_MEDIA_STREAMING_STREAM_IS_PAUSED,
    QAR_STATUS_MEDIA_STREAMING_STREAM_IS_RECONNECTING,
    QAR_STATUS_MEDIA_STREAMING_NO_ACTIVE_TRANSFERS,
    QAR_STATUS_RENDERING_PRODUCER_APP_VOLUME_UNRESOLVED,
  };
  for (const int code : codes) {
    const auto text = describe_status(code, OnboardingContext::kLocal, "the detail");
    EXPECT_EQ(text.code, code);
    EXPECT_FALSE(text.message.empty()) << code;
    EXPECT_EQ(text.symbol.find("QAR_STATUS_"), code == qar_qar_adapter::kStatusNoRuntime ?
      std::string::npos : 0u) << code;
    EXPECT_EQ(text.detail, "the detail");
    for (const char c : text.message) {
      EXPECT_LT(static_cast<unsigned char>(c), 128u) << "ASCII only: " << text.message;
    }
  }
}

TEST(OnboardingStatus, ACodeNobodyListedStillGetsAMessageThatNamesIt)
{
  const auto text = describe_status(4242, OnboardingContext::kLocal, "odd");
  EXPECT_EQ(text.symbol, "QAR_STATUS_4242");
  EXPECT_NE(text.message.find("QAR_STATUS_4242"), std::string::npos);
  EXPECT_NE(text.message.find("odd"), std::string::npos);
  EXPECT_TRUE(text.retry_offered);
}

TEST(OnboardingStatus, AHubThatIsNotAuthenticIsNotRetried)
{
  const auto text = describe_status(
    QAR_STATUS_ONBOARDING_HUB_NOT_AUTHENTIC, OnboardingContext::kLocal, nullptr);
  EXPECT_FALSE(text.retry_offered);
  EXPECT_NE(text.message.find("reinstall"), std::string::npos);
  EXPECT_FALSE(describe_status(
      QAR_STATUS_NOT_IMPLEMENTED, OnboardingContext::kLocal, nullptr).retry_offered);
  EXPECT_FALSE(describe_status(
      QAR_STATUS_HUB_TARGET_APP_DEVICE_NOT_INVITABLE, OnboardingContext::kInvite,
      nullptr).retry_offered);
  EXPECT_TRUE(describe_status(
      QAR_STATUS_SECURE_STORAGE_LOCKED, OnboardingContext::kRejoin, nullptr).retry_offered);
}

TEST(OnboardingStatus, AnUnreachableHubReadsDifferentlyForThisPcAndForAnotherPc)
{
  const auto local = describe_status(
    QAR_STATUS_ONBOARDING_HUB_UNREACHABLE, OnboardingContext::kLocal, nullptr);
  const auto remote = describe_status(
    QAR_STATUS_ONBOARDING_HUB_UNREACHABLE, OnboardingContext::kRemote, nullptr,
    "hub.example:19120");
  EXPECT_NE(local.message.find("Start QAROS"), std::string::npos);
  EXPECT_NE(remote.message.find("hub.example:19120"), std::string::npos);
  EXPECT_NE(remote.message.find("firewall"), std::string::npos);
  EXPECT_NE(local.message, remote.message);
}

TEST(OnboardingStatus, TheFailureOfAnOnboardingQuotesTheRuntimesDetail)
{
  const auto text = describe_status(
    QAR_STATUS_ONBOARDING_FAILED, OnboardingContext::kLocal, "certificate refused");
  EXPECT_NE(text.message.find("certificate refused"), std::string::npos);
  EXPECT_NE(text.message.find("qar-streaming-c.dll"), std::string::npos);
}

TEST(OnboardingStatus, AnArgumentTheHubRefusedIsExplainedPerCall)
{
  const auto remote = describe_status(
    QAR_STATUS_ARGUMENT_NOT_SUPPORTED, OnboardingContext::kRemote, nullptr);
  const auto invite = describe_status(
    QAR_STATUS_ARGUMENT_NOT_SUPPORTED, OnboardingContext::kInvite, nullptr);
  EXPECT_NE(remote.message.find("port"), std::string::npos);
  EXPECT_NE(invite.message.find("device"), std::string::npos);
}

TEST(TargetRule, OnlyAnOnlineOrUnknownTargetThatIsNotSelfIsRendered)
{
  using qar_qar_adapter::is_render_target;
  const QarPeerPresence here[] = {QAR_PEER_PRESENCE_UNKNOWN, QAR_PEER_PRESENCE_ONLINE};
  const QarPeerPresence gone[] = {
    QAR_PEER_PRESENCE_OFFLINE, QAR_PEER_PRESENCE_LEAVING, QAR_PEER_PRESENCE_RETIRED};
  const QarPeerRole others[] = {
    QAR_PEER_ROLE_INTERNAL, QAR_PEER_ROLE_SOURCE, QAR_PEER_ROLE_RUNTIME, QAR_PEER_ROLE_CONTROL};
  for (const auto presence : here) {
    EXPECT_TRUE(is_render_target(QAR_PEER_ROLE_TARGET, presence, false));
    EXPECT_FALSE(is_render_target(QAR_PEER_ROLE_TARGET, presence, true)) << "self";
    for (const auto role : others) {
      EXPECT_FALSE(is_render_target(role, presence, false));
    }
  }
  for (const auto presence : gone) {
    EXPECT_FALSE(is_render_target(QAR_PEER_ROLE_TARGET, presence, false));
    EXPECT_TRUE(qar_qar_adapter::is_gone(presence));
  }
  EXPECT_FALSE(qar_qar_adapter::is_gone(QAR_PEER_PRESENCE_ONLINE));
  EXPECT_FALSE(qar_qar_adapter::is_gone(QAR_PEER_PRESENCE_UNKNOWN));
}

TEST(TargetRule, APresenceTheHeaderDoesNotNameReadsAsUnknown)
{
  const auto odd = static_cast<QarPeerPresence>(99);
  EXPECT_TRUE(qar_qar_adapter::is_render_target(QAR_PEER_ROLE_TARGET, odd, false));
  EXPECT_FALSE(qar_qar_adapter::is_gone(odd));
}

TEST(HubModeRule, AnExplicitRequestWinsAndAutoLooksAtThePairingAndTheInstall)
{
  using qar_qar_adapter::resolve_hub_mode;
  for (const bool identity : {false, true}) {
    for (const bool installed : {false, true}) {
      EXPECT_EQ(resolve_hub_mode(HubMode::kLocal, identity, installed), HubMode::kLocal);
      EXPECT_EQ(resolve_hub_mode(HubMode::kRemote, identity, installed), HubMode::kRemote);
    }
  }
  const char * reason = nullptr;
  EXPECT_EQ(resolve_hub_mode(HubMode::kAuto, true, true, &reason), HubMode::kRemote);
  EXPECT_NE(std::string(reason).find("pairing"), std::string::npos);
  EXPECT_EQ(resolve_hub_mode(HubMode::kAuto, true, false), HubMode::kRemote);
  EXPECT_EQ(resolve_hub_mode(HubMode::kAuto, false, true, &reason), HubMode::kLocal);
  EXPECT_NE(std::string(reason).find("installed"), std::string::npos);
  EXPECT_EQ(resolve_hub_mode(HubMode::kAuto, false, false), HubMode::kRemote);
}

}  // namespace
