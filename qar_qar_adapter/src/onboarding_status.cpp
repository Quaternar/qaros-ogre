// Copyright 2026 QuaternAR
// SPDX-License-Identifier: MIT

#include "qar_qar_adapter/onboarding_status.hpp"

#include "qar_ar_core/qar_streaming_c.hpp"

namespace qar_qar_adapter
{

namespace
{

struct Named
{
  int code;
  const char * symbol;
};

constexpr Named kNames[] = {
  {kStatusNoRuntime, "NO_RUNTIME"},
  {QAR_STATUS_NOT_IMPLEMENTED, "QAR_STATUS_NOT_IMPLEMENTED"},
  {QAR_STATUS_ARGUMENT_NOT_SUPPORTED, "QAR_STATUS_ARGUMENT_NOT_SUPPORTED"},
  {QAR_STATUS_TIMEOUT, "QAR_STATUS_TIMEOUT"},
  {QAR_STATUS_MEDIA_STREAMING_STREAM_IS_PAUSED, "QAR_STATUS_MEDIA_STREAMING_STREAM_IS_PAUSED"},
  {QAR_STATUS_MEDIA_STREAMING_STREAM_IS_RECONNECTING,
    "QAR_STATUS_MEDIA_STREAMING_STREAM_IS_RECONNECTING"},
  {QAR_STATUS_MEDIA_STREAMING_NO_ACTIVE_TRANSFERS,
    "QAR_STATUS_MEDIA_STREAMING_NO_ACTIVE_TRANSFERS"},
  {QAR_STATUS_RENDERING_PRODUCER_APP_VOLUME_UNRESOLVED,
    "QAR_STATUS_RENDERING_PRODUCER_APP_VOLUME_UNRESOLVED"},
  {QAR_STATUS_PAKE_ERROR, "QAR_STATUS_PAKE_ERROR"},
  {QAR_STATUS_ONBOARDING_SESSION_NOT_FOUND, "QAR_STATUS_ONBOARDING_SESSION_NOT_FOUND"},
  {QAR_STATUS_ONBOARDING_FAILED, "QAR_STATUS_ONBOARDING_FAILED"},
  {QAR_STATUS_ONBOARDING_HUB_UNREACHABLE, "QAR_STATUS_ONBOARDING_HUB_UNREACHABLE"},
  {QAR_STATUS_ONBOARDING_SESSION_STILL_ACTIVE, "QAR_STATUS_ONBOARDING_SESSION_STILL_ACTIVE"},
  {QAR_STATUS_ONBOARDING_APPROVAL_DENIED, "QAR_STATUS_ONBOARDING_APPROVAL_DENIED"},
  {QAR_STATUS_ONBOARDING_APPROVAL_TIMEOUT, "QAR_STATUS_ONBOARDING_APPROVAL_TIMEOUT"},
  {QAR_STATUS_ONBOARDING_HUB_NOT_AUTHENTIC, "QAR_STATUS_ONBOARDING_HUB_NOT_AUTHENTIC"},
  {QAR_STATUS_ONBOARDING_APP_IDENTITY_MISMATCH, "QAR_STATUS_ONBOARDING_APP_IDENTITY_MISMATCH"},
  {QAR_STATUS_ONBOARDING_RE_PAIR_REQUIRED, "QAR_STATUS_ONBOARDING_RE_PAIR_REQUIRED"},
  {QAR_STATUS_ONBOARDING_RENEWAL_POSSESSION_UNPROVEN,
    "QAR_STATUS_ONBOARDING_RENEWAL_POSSESSION_UNPROVEN"},
  {QAR_STATUS_HUB_TARGET_APP_DEVICE_NOT_INVITABLE,
    "QAR_STATUS_HUB_TARGET_APP_DEVICE_NOT_INVITABLE"},
  {QAR_STATUS_HUB_LIFECYCLE_OPERATION_RUNNING, "QAR_STATUS_HUB_LIFECYCLE_OPERATION_RUNNING"},
  {QAR_STATUS_SECURE_STORAGE_ERROR, "QAR_STATUS_SECURE_STORAGE_ERROR"},
  {QAR_STATUS_SECURE_STORAGE_NOT_FOUND, "QAR_STATUS_SECURE_STORAGE_NOT_FOUND"},
  {QAR_STATUS_SECURE_STORAGE_TAMPERED, "QAR_STATUS_SECURE_STORAGE_TAMPERED"},
  {QAR_STATUS_SECURE_STORAGE_PROTECTOR_MISMATCH, "QAR_STATUS_SECURE_STORAGE_PROTECTOR_MISMATCH"},
  {QAR_STATUS_SECURE_STORAGE_PROTECTOR_UNAVAILABLE,
    "QAR_STATUS_SECURE_STORAGE_PROTECTOR_UNAVAILABLE"},
  {QAR_STATUS_SECURE_STORAGE_INVALID_KEY, "QAR_STATUS_SECURE_STORAGE_INVALID_KEY"},
  {QAR_STATUS_SECURE_STORAGE_LOCKED, "QAR_STATUS_SECURE_STORAGE_LOCKED"},
};

std::string quoted_detail(const char * detail)
{
  return (detail != nullptr && *detail != '\0') ? std::string(detail) : std::string("no detail");
}

}  // namespace

std::string status_symbol(int code)
{
  for (const Named & named : kNames) {
    if (named.code == code) {
      return named.symbol;
    }
  }
  return "QAR_STATUS_" + std::to_string(code);
}

StatusText describe_status(
  int code, OnboardingContext context, const char * detail, const char * hub_address)
{
  StatusText out;
  out.code = code;
  out.symbol = status_symbol(code);
  if (detail != nullptr) {
    out.detail = detail;
  }
  const std::string address =
    (hub_address != nullptr && *hub_address != '\0') ? hub_address : "the address you typed";

  switch (code) {
    case kStatusNoRuntime:
      out.message =
        "QAROS is not installed on this PC (and QAROS_RUNTIME_DIR is not set). Install QAROS, "
        "then connect again.";
      out.retry_offered = true;
      break;
    case QAR_STATUS_ONBOARDING_FAILED:
      out.message = "Joining QAROS failed: " + quoted_detail(detail) +
        ". If a qar-streaming-c.dll lies next to the plugin or the program, remove it.";
      out.retry_offered = true;
      break;
    case QAR_STATUS_ONBOARDING_HUB_UNREACHABLE:
      if (context == OnboardingContext::kLocal) {
        out.message =
          "The QAROS Hub is not running and could not be started. Start QAROS from the Start "
          "menu, then connect again.";
      } else {
        out.message = "The QAROS Hub at " + address +
          " does not answer. Check host, port (shown with the code), that the Hub runs, and the "
          "firewall (TCP/UDP 19120-19200).";
      }
      out.retry_offered = true;
      break;
    case QAR_STATUS_ONBOARDING_APPROVAL_DENIED:
      out.message =
        "QAROS denied this app (Deny was clicked, the prompt was closed, or the approval was "
        "revoked). Connect again and click Allow.";
      out.retry_offered = true;
      break;
    case QAR_STATUS_ONBOARDING_APPROVAL_TIMEOUT:
      out.message =
        "Nobody answered the approval prompt in the QAROS Hub in time. Connect again and click "
        "Allow in the QAROS window.";
      out.retry_offered = true;
      break;
    case QAR_STATUS_ONBOARDING_HUB_NOT_AUTHENTIC:
      out.message =
        "The program answering as the QAROS Hub is not the installed, signed QAROS. Not retried: "
        "reinstall QAROS.";
      out.retry_offered = false;
      break;
    case QAR_STATUS_ONBOARDING_APP_IDENTITY_MISMATCH:
      out.message =
        "This program does not match what was approved (moved, copied or changed). Revoke the "
        "old approval in the Visualizer (Source Applications) and connect again.";
      out.retry_offered = true;
      break;
    case QAR_STATUS_PAKE_ERROR:
      out.message =
        "The code was wrong or has expired (codes live about 10 s and work once). Type the code "
        "the Hub shows at that moment.";
      out.retry_offered = true;
      break;
    case QAR_STATUS_ONBOARDING_SESSION_NOT_FOUND:
      if (context == OnboardingContext::kForget) {
        out.message = "No pairing with that Hub is stored.";
      } else {
        out.message =
          "This PC is not paired with that Hub. Type the Hub's current code.";
      }
      out.retry_offered = true;
      break;
    case QAR_STATUS_ONBOARDING_RE_PAIR_REQUIRED:
      out.message =
        "The pairing has expired or was revoked. Pair again with a code from the Hub.";
      out.retry_offered = true;
      break;
    case QAR_STATUS_ONBOARDING_RENEWAL_POSSESSION_UNPROVEN:
      out.message = "The stored pairing is damaged. Pair again with a code.";
      out.retry_offered = true;
      break;
    case QAR_STATUS_ONBOARDING_SESSION_STILL_ACTIVE:
      out.message = "Disconnect first, then forget.";
      out.retry_offered = true;
      break;
    case QAR_STATUS_ARGUMENT_NOT_SUPPORTED:
      if (context == OnboardingContext::kInvite) {
        out.message =
          "The device address is not valid, or the Hub cannot invite that kind of device.";
      } else {
        out.message =
          "Host or port is not valid (the port is required; it is shown with the Hub's code).";
      }
      out.retry_offered = true;
      break;
    case QAR_STATUS_NOT_IMPLEMENTED:
      out.message = "This-PC mode works on Windows only; use another PC's Hub with a code.";
      out.retry_offered = false;
      break;
    case QAR_STATUS_TIMEOUT:
      out.message = "QAROS did not answer in time.";
      out.retry_offered = true;
      break;
    case QAR_STATUS_HUB_LIFECYCLE_OPERATION_RUNNING:
      out.message = "The Hub is busy (session change). Trying again in a moment.";
      out.retry_offered = true;
      break;
    case QAR_STATUS_SECURE_STORAGE_LOCKED:
      out.message = "The stored pairing is in use by another program. Try again in a moment.";
      out.retry_offered = true;
      break;
    case QAR_STATUS_SECURE_STORAGE_ERROR:
    case QAR_STATUS_SECURE_STORAGE_NOT_FOUND:
    case QAR_STATUS_SECURE_STORAGE_TAMPERED:
    case QAR_STATUS_SECURE_STORAGE_PROTECTOR_MISMATCH:
    case QAR_STATUS_SECURE_STORAGE_PROTECTOR_UNAVAILABLE:
    case QAR_STATUS_SECURE_STORAGE_INVALID_KEY:
      out.message = "The stored pairing cannot be read on this PC / user (" + out.symbol +
        "). Forget the pairing and pair again.";
      out.retry_offered = false;
      break;
    case QAR_STATUS_HUB_TARGET_APP_DEVICE_NOT_INVITABLE:
      out.message = "The Hub cannot invite that kind of device.";
      out.retry_offered = false;
      break;
    case QAR_STATUS_MEDIA_STREAMING_STREAM_IS_PAUSED:
      out.message = "paused by the receiver (its display is not visible)";
      out.retry_offered = false;
      break;
    case QAR_STATUS_MEDIA_STREAMING_STREAM_IS_RECONNECTING:
      out.message = "the receiver lost the stream and is reconnecting";
      out.retry_offered = false;
      break;
    case QAR_STATUS_MEDIA_STREAMING_NO_ACTIVE_TRANSFERS:
      out.message = "no receiver is attached to the stream";
      out.retry_offered = false;
      break;
    case QAR_STATUS_RENDERING_PRODUCER_APP_VOLUME_UNRESOLVED:
      out.message = "app volume not resolved yet";
      out.retry_offered = false;
      break;
    default:
      out.message = "QAROS reported " + out.symbol + ": " + quoted_detail(detail) + ".";
      out.retry_offered = true;
      break;
  }
  return out;
}

}  // namespace qar_qar_adapter
