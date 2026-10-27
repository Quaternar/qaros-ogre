// Copyright 2026 QuaternAR
// SPDX-License-Identifier: MIT
//
// What a status code of the QarOS C API means for the person at the keyboard. The table maps one
// code, in the context where it was returned, to a sentence for the status strip and a flag that
// says whether trying again can help. Pure text: no QarOS call.

#ifndef QAR_QAR_ADAPTER__ONBOARDING_STATUS_HPP_
#define QAR_QAR_ADAPTER__ONBOARDING_STATUS_HPP_

#include <string>

namespace qar_qar_adapter
{

/// @brief The status of the cause "there is no QAROS runtime on this PC". It is not a code of the
/// C API: the library could not be loaded at all.
constexpr int kStatusNoRuntime = -1;

/// @brief The call that returned a code. The same code reads differently in different calls.
enum class OnboardingContext
{
  kLocal,    ///< Joining the Hub of this PC as a source app.
  kRemote,   ///< Pairing with the Hub of another PC by a code.
  kRejoin,   ///< Joining again with the stored pairing.
  kForget,   ///< Erasing the stored pairing.
  kInvite,   ///< Asking the Hub to start the app of a device.
  kStream    ///< A video or render stream of an open session.
};

/// @brief One status code, in plain words.
struct StatusText
{
  int code = 0;                 ///< A QarStatusCode, or a negative cause of this program.
  std::string symbol;           ///< For example "QAR_STATUS_ONBOARDING_APPROVAL_TIMEOUT".
  std::string message;          ///< Plain words for the status strip and the tab.
  std::string detail;           ///< The runtime's own message, for the log only.
  bool retry_offered = false;   ///< Trying the same action again can help.
};

/// @brief The name of a status code as the header spells it ("QAR_STATUS_<n>" when unnamed).
std::string status_symbol(int code);

/// @brief The sentence for @p code returned by a call of kind @p context.
/// @param detail The runtime's message for the result; only copied into @ref StatusText::detail
///        and, where a sentence quotes it, into the sentence. May be null.
/// @param hub_address "host:port" of the Hub a code pairing aimed at; only the remote texts use
///        it. May be null.
StatusText describe_status(
  int code, OnboardingContext context, const char * detail, const char * hub_address = nullptr);

}  // namespace qar_qar_adapter

#endif  // QAR_QAR_ADAPTER__ONBOARDING_STATUS_HPP_
