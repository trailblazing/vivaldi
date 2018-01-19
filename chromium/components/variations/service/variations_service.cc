// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/variations/service/variations_service.h"

#include <stddef.h>
#include <stdint.h>

#include <utility>
#include <vector>

#include "base/base_switches.h"
#include "base/build_time.h"
#include "base/command_line.h"
#include "base/memory/ptr_util.h"
#include "base/metrics/histogram_macros.h"
#include "base/metrics/sparse_histogram.h"
#include "base/strings/string_util.h"
#include "base/sys_info.h"
#include "base/task_runner_util.h"
#include "base/task_scheduler/post_task.h"
#include "base/threading/sequenced_worker_pool.h"
#include "base/timer/elapsed_timer.h"
#include "base/values.h"
#include "base/version.h"
#include "build/build_config.h"
#include "components/data_use_measurement/core/data_use_user_data.h"
#include "components/metrics/metrics_state_manager.h"
#include "components/network_time/network_time_tracker.h"
#include "components/pref_registry/pref_registry_syncable.h"
#include "components/prefs/pref_registry_simple.h"
#include "components/prefs/pref_service.h"
#include "components/variations/pref_names.h"
#include "components/variations/proto/variations_seed.pb.h"
#include "components/variations/service/variations_field_trial_creator.h"
#include "components/variations/variations_seed_processor.h"
#include "components/variations/variations_seed_simulator.h"
#include "components/variations/variations_switches.h"
#include "components/variations/variations_url_constants.h"
#include "net/base/load_flags.h"
#include "net/base/net_errors.h"
#include "net/base/network_change_notifier.h"
#include "net/base/url_util.h"
#include "net/http/http_response_headers.h"
#include "net/http/http_status_code.h"
#include "net/http/http_util.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "net/url_request/url_fetcher.h"
#include "net/url_request/url_request_status.h"
#include "ui/base/device_form_factor.h"
#include "url/gurl.h"

namespace variations {
namespace {

// TODO(mad): To be removed when we stop updating the NetworkTimeTracker.
// For the HTTP date headers, the resolution of the server time is 1 second.
const int64_t kServerTimeResolutionMs = 1000;

// Whether the VariationsService should always be created, even in Chromium
// builds.
bool g_enabled_for_testing = false;

// Returns a string that will be used for the value of the 'osname' URL param
// to the variations server.
std::string GetPlatformString() {
#if defined(OS_WIN)
  return "win";
#elif defined(OS_IOS)
  return "ios";
#elif defined(OS_MACOSX)
  return "mac";
#elif defined(OS_CHROMEOS)
  return "chromeos";
#elif defined(OS_ANDROID)
  return "android";
#elif defined(OS_LINUX) || defined(OS_BSD) || defined(OS_SOLARIS)
  // Default BSD and SOLARIS to Linux to not break those builds, although these
  // platforms are not officially supported by Chrome.
  return "linux";
#else
#error Unknown platform
#endif
}

// Gets the restrict parameter from either the client or |policy_pref_service|.
std::string GetRestrictParameterPref(VariationsServiceClient* client,
                                     PrefService* policy_pref_service) {
  std::string parameter;
  if (client->OverridesRestrictParameter(&parameter) || !policy_pref_service)
    return parameter;

  return policy_pref_service->GetString(prefs::kVariationsRestrictParameter);
}

enum ResourceRequestsAllowedState {
  RESOURCE_REQUESTS_ALLOWED,
  RESOURCE_REQUESTS_NOT_ALLOWED,
  RESOURCE_REQUESTS_ALLOWED_NOTIFIED,
  RESOURCE_REQUESTS_NOT_ALLOWED_EULA_NOT_ACCEPTED,
  RESOURCE_REQUESTS_NOT_ALLOWED_NETWORK_DOWN,
  RESOURCE_REQUESTS_NOT_ALLOWED_COMMAND_LINE_DISABLED,
  RESOURCE_REQUESTS_ALLOWED_ENUM_SIZE,
};

// Records UMA histogram with the current resource requests allowed state.
void RecordRequestsAllowedHistogram(ResourceRequestsAllowedState state) {
  UMA_HISTOGRAM_ENUMERATION("Variations.ResourceRequestsAllowed", state,
                            RESOURCE_REQUESTS_ALLOWED_ENUM_SIZE);
}

enum VariationsSeedExpiry {
  VARIATIONS_SEED_EXPIRY_NOT_EXPIRED,
  VARIATIONS_SEED_EXPIRY_FETCH_TIME_MISSING,
  VARIATIONS_SEED_EXPIRY_EXPIRED,
  VARIATIONS_SEED_EXPIRY_ENUM_SIZE,
};

// Converts ResourceRequestAllowedNotifier::State to the corresponding
// ResourceRequestsAllowedState value.
ResourceRequestsAllowedState ResourceRequestStateToHistogramValue(
    web_resource::ResourceRequestAllowedNotifier::State state) {
  using web_resource::ResourceRequestAllowedNotifier;
  switch (state) {
    case ResourceRequestAllowedNotifier::DISALLOWED_EULA_NOT_ACCEPTED:
      return RESOURCE_REQUESTS_NOT_ALLOWED_EULA_NOT_ACCEPTED;
    case ResourceRequestAllowedNotifier::DISALLOWED_NETWORK_DOWN:
      return RESOURCE_REQUESTS_NOT_ALLOWED_NETWORK_DOWN;
    case ResourceRequestAllowedNotifier::DISALLOWED_COMMAND_LINE_DISABLED:
      return RESOURCE_REQUESTS_NOT_ALLOWED_COMMAND_LINE_DISABLED;
    case ResourceRequestAllowedNotifier::ALLOWED:
      return RESOURCE_REQUESTS_ALLOWED;
  }
  NOTREACHED();
  return RESOURCE_REQUESTS_NOT_ALLOWED;
}

// Returns the header value for |name| from |headers| or an empty string if not
// set.
std::string GetHeaderValue(const net::HttpResponseHeaders* headers,
                           const base::StringPiece& name) {
  std::string value;
  headers->EnumerateHeader(nullptr, name, &value);
  return value;
}

// Returns the list of values for |name| from |headers|. If the header in not
// set, return an empty list.
std::vector<std::string> GetHeaderValuesList(
    const net::HttpResponseHeaders* headers,
    const base::StringPiece& name) {
  std::vector<std::string> values;
  size_t iter = 0;
  std::string value;
  while (headers->EnumerateHeader(&iter, name, &value)) {
    values.push_back(value);
  }
  return values;
}

// Looks for delta and gzip compression instance manipulation flags set by the
// server in |headers|. Checks the order of flags and presence of unknown
// instance manipulations. If successful, |is_delta_compressed| and
// |is_gzip_compressed| contain compression flags and true is returned.
bool GetInstanceManipulations(const net::HttpResponseHeaders* headers,
                              bool* is_delta_compressed,
                              bool* is_gzip_compressed) {
  std::vector<std::string> ims = GetHeaderValuesList(headers, "IM");
  const auto delta_im = std::find(ims.begin(), ims.end(), "x-bm");
  const auto gzip_im = std::find(ims.begin(), ims.end(), "gzip");
  *is_delta_compressed = delta_im != ims.end();
  *is_gzip_compressed = gzip_im != ims.end();

  // The IM field should not have anything but x-bm and gzip.
  size_t im_count = (*is_delta_compressed ? 1 : 0) +
      (*is_gzip_compressed ? 1 : 0);
  if (im_count != ims.size()) {
    DVLOG(1) << "Unrecognized instance manipulations in "
             << base::JoinString(ims, ",")
             << "; only x-bm and gzip are supported";
    return false;
  }

  // The IM field defines order in which instance manipulations were applied.
  // The client requests and supports gzip-compressed delta-compressed seeds,
  // but not vice versa.
  if (*is_delta_compressed && *is_gzip_compressed && delta_im > gzip_im) {
    DVLOG(1) << "Unsupported instance manipulations order: "
             << "requested x-bm,gzip but received gzip,x-bm";
    return false;
  }

  return true;
}

}  // namespace

VariationsService::VariationsService(
    std::unique_ptr<VariationsServiceClient> client,
    std::unique_ptr<web_resource::ResourceRequestAllowedNotifier> notifier,
    PrefService* local_state,
    metrics::MetricsStateManager* state_manager,
    const UIStringOverrider& ui_string_overrider)
    : client_(std::move(client)),
      local_state_(local_state),
      state_manager_(state_manager),
      policy_pref_service_(local_state),
      initial_request_completed_(false),
      disable_deltas_for_next_request_(false),
      resource_request_allowed_notifier_(std::move(notifier)),
      request_count_(0),
      field_trial_creator_(local_state, client_.get(), ui_string_overrider),
      weak_ptr_factory_(this) {
  DCHECK(client_.get());
  DCHECK(resource_request_allowed_notifier_.get());

  resource_request_allowed_notifier_->Init(this);

  // Increment the crash streak if the previous session crashed.
  // Note that the streak is not cleared if the previous run didn’t crash.
  // Instead, it’s incremented on each crash until Chrome is able to
  // successfully fetch a new seed. This way, a seed update that mostly
  // destabilizes Chrome will still result in a fallback to safe mode.
  int num_crashes = local_state->GetInteger(prefs::kVariationsCrashStreak);
  if (!state_manager_->clean_exit_beacon()->exited_cleanly()) {
    ++num_crashes;
    local_state->SetInteger(prefs::kVariationsCrashStreak, num_crashes);
  }

  // After three failures in a row -- either consistent crashes or consistent
  // failures to fetch the seed -- assume that the current seed is bad, and fall
  // back to the safe seed. However, ignore any number of failures if the
  // --force-fieldtrials flag is set, as this flag is only used by developers,
  // and there's no need to make the development process flakier.
  const int kMaxFailuresBeforeRevertingToSafeSeed = 3;
  int num_failures_to_fetch =
      local_state->GetInteger(prefs::kVariationsFailedToFetchSeedStreak);
  bool fall_back_to_safe_mode =
      (num_crashes >= kMaxFailuresBeforeRevertingToSafeSeed ||
       num_failures_to_fetch >= kMaxFailuresBeforeRevertingToSafeSeed) &&
      !base::CommandLine::ForCurrentProcess()->HasSwitch(
          ::switches::kForceFieldTrials);
  UMA_HISTOGRAM_BOOLEAN("Variations.SafeMode.FellBackToSafeMode",
                        fall_back_to_safe_mode);
  UMA_HISTOGRAM_SPARSE_SLOWLY("Variations.SafeMode.Streak.Crashes",
                              std::min(std::max(num_crashes, 0), 100));
  UMA_HISTOGRAM_SPARSE_SLOWLY(
      "Variations.SafeMode.Streak.FetchFailures",
      std::min(std::max(num_failures_to_fetch, 0), 100));
}

VariationsService::~VariationsService() {
}

void VariationsService::PerformPreMainMessageLoopStartup() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  StartRepeatedVariationsSeedFetch();
}

void VariationsService::StartRepeatedVariationsSeedFetch() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  // Initialize the Variations server URL.
  variations_server_url_ =
      GetVariationsServerURL(policy_pref_service_, restrict_mode_);

  // Check that |CreateTrialsFromSeed| was called, which is necessary to
  // retrieve the serial number that will be sent to the server.
  DCHECK(field_trial_creator_.create_trials_from_seed_called());

  DCHECK(!request_scheduler_.get());
  request_scheduler_.reset(VariationsRequestScheduler::Create(
      base::Bind(&VariationsService::FetchVariationsSeed,
                 weak_ptr_factory_.GetWeakPtr()),
      local_state_));
  // Note that the act of starting the scheduler will start the fetch, if the
  // scheduler deems appropriate.
  request_scheduler_->Start();
}

std::string VariationsService::LoadPermanentConsistencyCountry(
    const base::Version& version,
    const std::string& latest_country) {
  return field_trial_creator_.LoadPermanentConsistencyCountry(version,
                                                              latest_country);
}

void VariationsService::AddObserver(Observer* observer) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  observer_list_.AddObserver(observer);
}

void VariationsService::RemoveObserver(Observer* observer) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  observer_list_.RemoveObserver(observer);
}

void VariationsService::OnAppEnterForeground() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  // On mobile platforms, initialize the fetch scheduler when we receive the
  // first app foreground notification.
  if (!request_scheduler_)
    StartRepeatedVariationsSeedFetch();
  request_scheduler_->OnAppEnterForeground();
}

void VariationsService::SetRestrictMode(const std::string& restrict_mode) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  // This should be called before the server URL has been computed.
  DCHECK(variations_server_url_.is_empty());
  restrict_mode_ = restrict_mode;
}

void VariationsService::SetCreateTrialsFromSeedCalledForTesting(bool called) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  field_trial_creator_.SetCreateTrialsFromSeedCalledForTesting(called);
}

GURL VariationsService::GetVariationsServerURL(
    PrefService* policy_pref_service,
    const std::string& restrict_mode_override) {
  std::string server_url_string(
      base::CommandLine::ForCurrentProcess()->GetSwitchValueASCII(
          switches::kVariationsServerURL));
  if (server_url_string.empty())
    server_url_string = kDefaultServerUrl;
  GURL server_url = GURL(server_url_string);

  const std::string restrict_param =
      !restrict_mode_override.empty()
          ? restrict_mode_override
          : GetRestrictParameterPref(client_.get(), policy_pref_service);
  if (!restrict_param.empty()) {
    server_url = net::AppendOrReplaceQueryParameter(server_url,
                                                    "restrict",
                                                    restrict_param);
  }

  server_url = net::AppendOrReplaceQueryParameter(server_url, "osname",
                                                  GetPlatformString());

  DCHECK(server_url.is_valid());
  return server_url;
}

// static
std::string VariationsService::GetDefaultVariationsServerURLForTesting() {
  return kDefaultServerUrl;
}

// static
void VariationsService::RegisterPrefs(PrefRegistrySimple* registry) {
  VariationsSeedStore::RegisterPrefs(registry);
  registry->RegisterInt64Pref(prefs::kVariationsLastFetchTime, 0);
  // This preference will only be written by the policy service, which will fill
  // it according to a value stored in the User Policy.
  registry->RegisterStringPref(prefs::kVariationsRestrictParameter,
                               std::string());
  // This preference keeps track of the country code used to filter
  // permanent-consistency studies.
  registry->RegisterListPref(prefs::kVariationsPermanentConsistencyCountry);

  // Prefs tracking failures along the way to fetching a seed, used to implement
  // Safe Mode.
  registry->RegisterIntegerPref(prefs::kVariationsCrashStreak, 0);
  registry->RegisterIntegerPref(prefs::kVariationsFailedToFetchSeedStreak, 0);
}

// static
void VariationsService::RegisterProfilePrefs(
    user_prefs::PrefRegistrySyncable* registry) {
  // This preference will only be written by the policy service, which will fill
  // it according to a value stored in the User Policy.
  registry->RegisterStringPref(prefs::kVariationsRestrictParameter,
                               std::string());
}

// static
std::unique_ptr<VariationsService> VariationsService::Create(
    std::unique_ptr<VariationsServiceClient> client,
    PrefService* local_state,
    metrics::MetricsStateManager* state_manager,
    const char* disable_network_switch,
    const UIStringOverrider& ui_string_overrider) {
  std::unique_ptr<VariationsService> result;
#if !defined(GOOGLE_CHROME_BUILD)
  // Unless the URL was provided, unsupported builds should return NULL to
  // indicate that the service should not be used.
  if (!base::CommandLine::ForCurrentProcess()->HasSwitch(
          switches::kVariationsServerURL) &&
      !g_enabled_for_testing) {
    DVLOG(1) << "Not creating VariationsService in unofficial build without --"
             << switches::kVariationsServerURL << " specified.";
    return result;
  }
#endif
  result.reset(new VariationsService(
      std::move(client),
      base::MakeUnique<web_resource::ResourceRequestAllowedNotifier>(
          local_state, disable_network_switch),
      local_state, state_manager, ui_string_overrider));
  return result;
}

// static
void VariationsService::EnableForTesting() {
  g_enabled_for_testing = true;
}

void VariationsService::DoActualFetch() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  // Pessimistically assume the fetch will fail. The failure streak will be
  // reset upon success.
  int num_failures_to_fetch =
      local_state_->GetInteger(prefs::kVariationsFailedToFetchSeedStreak);
  local_state_->SetInteger(prefs::kVariationsFailedToFetchSeedStreak,
                           num_failures_to_fetch + 1);

  // Normally, there shouldn't be a |pending_request_| when this fires. However
  // it's not impossible - for example if Chrome was paused (e.g. in a debugger
  // or if the machine was suspended) and OnURLFetchComplete() hasn't had a
  // chance to run yet from the previous request. In this case, don't start a
  // new request and just let the previous one finish.
  if (pending_seed_request_)
    return;

  net::NetworkTrafficAnnotationTag traffic_annotation =
      net::DefineNetworkTrafficAnnotation("chrome_variations_service", R"(
        semantics {
          sender: "Chrome Variations Service"
          description:
            "Retrieves the list of Google Chrome's Variations from the server, "
            "which will apply to the next Chrome session upon a restart."
          trigger:
            "Requests are made periodically while Google Chrome is running."
          data: "The operating system name."
          destination: GOOGLE_OWNED_SERVICE
        }
        policy {
          cookies_allowed: false
          setting: "This feature cannot be disabled by settings."
          policy_exception_justification:
            "Not implemented, considered not required."
        })");
  pending_seed_request_ =
      net::URLFetcher::Create(0, variations_server_url_, net::URLFetcher::GET,
                              this, traffic_annotation);
  data_use_measurement::DataUseUserData::AttachToFetcher(
      pending_seed_request_.get(),
      data_use_measurement::DataUseUserData::VARIATIONS);
  pending_seed_request_->SetLoadFlags(net::LOAD_DO_NOT_SEND_COOKIES |
                                      net::LOAD_DO_NOT_SEND_AUTH_DATA |
                                      net::LOAD_DO_NOT_SAVE_COOKIES);
  pending_seed_request_->SetRequestContext(client_->GetURLRequestContext());
  bool enable_deltas = false;
  if (!field_trial_creator_.seed_store().variations_serial_number().empty() &&
      !disable_deltas_for_next_request_) {
    // Tell the server that delta-compressed seeds are supported.
    enable_deltas = true;

    // Get the seed only if its serial number doesn't match what we have.
    pending_seed_request_->AddExtraRequestHeader(
        "If-None-Match:" +
        field_trial_creator_.seed_store().variations_serial_number());
  }
  // Tell the server that delta-compressed and gzipped seeds are supported.
  const char* supported_im = enable_deltas ? "A-IM:x-bm,gzip" : "A-IM:gzip";
  pending_seed_request_->AddExtraRequestHeader(supported_im);

  pending_seed_request_->Start();

  const base::TimeTicks now = base::TimeTicks::Now();
  base::TimeDelta time_since_last_fetch;
  // Record a time delta of 0 (default value) if there was no previous fetch.
  if (!last_request_started_time_.is_null())
    time_since_last_fetch = now - last_request_started_time_;
  UMA_HISTOGRAM_CUSTOM_COUNTS("Variations.TimeSinceLastFetchAttempt",
                              time_since_last_fetch.InMinutes(), 1,
                              base::TimeDelta::FromDays(7).InMinutes(), 50);
  UMA_HISTOGRAM_COUNTS_100("Variations.RequestCount", request_count_);
  ++request_count_;
  last_request_started_time_ = now;
  disable_deltas_for_next_request_ = false;
}

bool VariationsService::StoreSeed(const std::string& seed_data,
                                  const std::string& seed_signature,
                                  const std::string& country_code,
                                  const base::Time& date_fetched,
                                  bool is_delta_compressed,
                                  bool is_gzip_compressed) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  std::unique_ptr<VariationsSeed> seed(new VariationsSeed);
  if (!field_trial_creator_.seed_store().StoreSeedData(
          seed_data, seed_signature, country_code, date_fetched,
          is_delta_compressed, is_gzip_compressed, seed.get())) {
    return false;
  }

  RecordSuccessfulFetch();

  base::PostTaskWithTraitsAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::BACKGROUND},
      client_->GetVersionForSimulationCallback(),
      base::Bind(&VariationsService::PerformSimulationWithVersion,
                 weak_ptr_factory_.GetWeakPtr(), base::Passed(&seed)));
  return true;
}

std::unique_ptr<const base::FieldTrial::EntropyProvider>
VariationsService::CreateLowEntropyProvider() {
  return state_manager_->CreateLowEntropyProvider();
}

void VariationsService::FetchVariationsSeed() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  const web_resource::ResourceRequestAllowedNotifier::State state =
      resource_request_allowed_notifier_->GetResourceRequestsAllowedState();
  RecordRequestsAllowedHistogram(ResourceRequestStateToHistogramValue(state));
  if (state != web_resource::ResourceRequestAllowedNotifier::ALLOWED) {
    DVLOG(1) << "Resource requests were not allowed. Waiting for notification.";
    return;
  }

  DoActualFetch();
}

void VariationsService::NotifyObservers(
    const VariationsSeedSimulator::Result& result) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  if (result.kill_critical_group_change_count > 0) {
    for (auto& observer : observer_list_)
      observer.OnExperimentChangesDetected(Observer::CRITICAL);
  } else if (result.kill_best_effort_group_change_count > 0) {
    for (auto& observer : observer_list_)
      observer.OnExperimentChangesDetected(Observer::BEST_EFFORT);
  }
}

void VariationsService::OnURLFetchComplete(const net::URLFetcher* source) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  DCHECK_EQ(pending_seed_request_.get(), source);

  const bool is_first_request = !initial_request_completed_;
  initial_request_completed_ = true;

  // The fetcher will be deleted when the request is handled.
  std::unique_ptr<const net::URLFetcher> request(
      pending_seed_request_.release());
  const net::URLRequestStatus& status = request->GetStatus();
  const int response_code = request->GetResponseCode();
  UMA_HISTOGRAM_SPARSE_SLOWLY(
      "Variations.SeedFetchResponseOrErrorCode",
      status.is_success() ? response_code : status.error());

  if (status.status() != net::URLRequestStatus::SUCCESS) {
    DVLOG(1) << "Variations server request failed with error: "
             << status.error() << ": " << net::ErrorToString(status.error());
    // It's common for the very first fetch attempt to fail (e.g. the network
    // may not yet be available). In such a case, try again soon, rather than
    // waiting the full time interval.
    if (is_first_request)
      request_scheduler_->ScheduleFetchShortly();
    return;
  }

  const base::TimeDelta latency =
      base::TimeTicks::Now() - last_request_started_time_;

  base::Time response_date;
  if (response_code == net::HTTP_OK ||
      response_code == net::HTTP_NOT_MODIFIED) {
    bool success = request->GetResponseHeaders()->GetDateValue(&response_date);
    DCHECK(success || response_date.is_null());

    if (!response_date.is_null()) {
      client_->GetNetworkTimeTracker()->UpdateNetworkTime(
          response_date,
          base::TimeDelta::FromMilliseconds(kServerTimeResolutionMs), latency,
          base::TimeTicks::Now());
    }
  }

  if (response_code != net::HTTP_OK) {
    DVLOG(1) << "Variations server request returned non-HTTP_OK response code: "
             << response_code;
    if (response_code == net::HTTP_NOT_MODIFIED) {
      RecordSuccessfulFetch();

      // Update the seed date value in local state (used for expiry check on
      // next start up), since 304 is a successful response.
      field_trial_creator_.seed_store().UpdateSeedDateAndLogDayChange(
          response_date);
    }
    return;
  }

  std::string seed_data;
  bool success = request->GetResponseAsString(&seed_data);
  DCHECK(success);

  net::HttpResponseHeaders* headers = request->GetResponseHeaders();
  bool is_delta_compressed;
  bool is_gzip_compressed;
  if (!GetInstanceManipulations(headers, &is_delta_compressed,
                                &is_gzip_compressed)) {
    // The header does not specify supported instance manipulations, unable to
    // process data. Details of errors were logged by GetInstanceManipulations.
    field_trial_creator_.seed_store().ReportUnsupportedSeedFormatError();
    return;
  }

  const std::string signature = GetHeaderValue(headers, "X-Seed-Signature");
  const std::string country_code = GetHeaderValue(headers, "X-Country");
  const bool store_success = StoreSeed(seed_data, signature, country_code,
                                       response_date, is_delta_compressed,
                                       is_gzip_compressed);
  if (!store_success && is_delta_compressed) {
    disable_deltas_for_next_request_ = true;
    request_scheduler_->ScheduleFetchShortly();
  }
}

void VariationsService::OnResourceRequestsAllowed() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  // Note that this only attempts to fetch the seed at most once per period
  // (kSeedFetchPeriodHours). This works because
  // |resource_request_allowed_notifier_| only calls this method if an
  // attempt was made earlier that fails (which implies that the period had
  // elapsed). After a successful attempt is made, the notifier will know not
  // to call this method again until another failed attempt occurs.
  RecordRequestsAllowedHistogram(RESOURCE_REQUESTS_ALLOWED_NOTIFIED);
  DVLOG(1) << "Retrying fetch.";
  DoActualFetch();

  // This service must have created a scheduler in order for this to be called.
  DCHECK(request_scheduler_.get());
  request_scheduler_->Reset();
}

void VariationsService::PerformSimulationWithVersion(
    std::unique_ptr<VariationsSeed> seed,
    const base::Version& version) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  if (!version.IsValid())
    return;

  const base::ElapsedTimer timer;

  std::unique_ptr<const base::FieldTrial::EntropyProvider> default_provider =
      state_manager_->CreateDefaultEntropyProvider();
  std::unique_ptr<const base::FieldTrial::EntropyProvider> low_provider =
      state_manager_->CreateLowEntropyProvider();
  VariationsSeedSimulator seed_simulator(*default_provider, *low_provider);

  std::unique_ptr<ClientFilterableState> client_state =
      field_trial_creator_.GetClientFilterableStateForVersion(version);
  const VariationsSeedSimulator::Result result =
      seed_simulator.SimulateSeedStudies(*seed, *client_state);

  UMA_HISTOGRAM_COUNTS_100("Variations.SimulateSeed.NormalChanges",
                           result.normal_group_change_count);
  UMA_HISTOGRAM_COUNTS_100("Variations.SimulateSeed.KillBestEffortChanges",
                           result.kill_best_effort_group_change_count);
  UMA_HISTOGRAM_COUNTS_100("Variations.SimulateSeed.KillCriticalChanges",
                           result.kill_critical_group_change_count);

  UMA_HISTOGRAM_TIMES("Variations.SimulateSeed.Duration", timer.Elapsed());

  NotifyObservers(result);
}

void VariationsService::RecordSuccessfulFetch() {
  field_trial_creator_.RecordLastFetchTime();

  // Note: It's important to clear the crash streak as well as the fetch
  // failures streak. Crashes that occur after a successful seed fetch do not
  // prevent updating to a new seed, and therefore do not necessitate falling
  // back to a safe seed.
  local_state_->SetInteger(prefs::kVariationsCrashStreak, 0);
  local_state_->SetInteger(prefs::kVariationsFailedToFetchSeedStreak, 0);
}

std::string VariationsService::GetInvalidVariationsSeedSignature() const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  return field_trial_creator_.seed_store().GetInvalidSignature();
}

void VariationsService::GetClientFilterableStateForVersionCalledForTesting() {
  const base::Version current_version(version_info::GetVersionNumber());
  if (!current_version.IsValid())
    return;

  field_trial_creator_.GetClientFilterableStateForVersion(current_version);
}

std::string VariationsService::GetLatestCountry() const {
  return field_trial_creator_.GetLatestCountry();
}

bool VariationsService::CreateTrialsFromSeed(base::FeatureList* feature_list) {
  return field_trial_creator_.CreateTrialsFromSeed(CreateLowEntropyProvider(),
                                                   feature_list);
}

std::string VariationsService::GetStoredPermanentCountry() {
  const base::ListValue* list_value =
      local_state_->GetList(prefs::kVariationsPermanentConsistencyCountry);
  std::string stored_country;

  if (list_value->GetSize() == 2) {
    list_value->GetString(1, &stored_country);
  }

  return stored_country;
}

bool VariationsService::OverrideStoredPermanentCountry(
    const std::string& country_override) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  if (country_override.empty())
    return false;

  const base::ListValue* list_value =
      local_state_->GetList(prefs::kVariationsPermanentConsistencyCountry);

  std::string stored_country;
  const bool got_stored_country =
      list_value->GetSize() == 2 && list_value->GetString(1, &stored_country);

  if (got_stored_country && stored_country == country_override)
    return false;

  base::Version version(version_info::GetVersionNumber());
  field_trial_creator_.StorePermanentCountry(version, country_override);
  return true;
}

}  // namespace variations