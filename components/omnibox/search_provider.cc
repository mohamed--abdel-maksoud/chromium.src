// Copyright 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/omnibox/search_provider.h"

#include <algorithm>
#include <cmath>

#include "base/base64.h"
#include "base/callback.h"
#include "base/i18n/break_iterator.h"
#include "base/i18n/case_conversion.h"
#include "base/json/json_string_value_serializer.h"
#include "base/metrics/histogram.h"
#include "base/metrics/user_metrics.h"
#include "base/rand_util.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "components/history/core/browser/in_memory_database.h"
#include "components/history/core/browser/keyword_search_term.h"
#include "components/metrics/proto/omnibox_input_type.pb.h"
#include "components/omnibox/autocomplete_provider_client.h"
#include "components/omnibox/autocomplete_provider_listener.h"
#include "components/omnibox/autocomplete_result.h"
#include "components/omnibox/keyword_provider.h"
#include "components/omnibox/omnibox_field_trial.h"
#include "components/omnibox/url_prefix.h"
#include "components/search/search.h"
#include "components/search_engines/template_url_prepopulate_data.h"
#include "components/search_engines/template_url_service.h"
#include "components/variations/variations_http_header_provider.h"
#include "grit/components_strings.h"
#include "net/base/escape.h"
#include "net/base/load_flags.h"
#include "net/base/net_util.h"
#include "net/http/http_request_headers.h"
#include "net/url_request/url_fetcher.h"
#include "net/url_request/url_request_status.h"
#include "ui/base/l10n/l10n_util.h"
#include "url/url_constants.h"
#include "url/url_util.h"

// Helpers --------------------------------------------------------------------

namespace {

// We keep track in a histogram how many suggest requests we send, how
// many suggest requests we invalidate (e.g., due to a user typing
// another character), and how many replies we receive.
// *** ADD NEW ENUMS AFTER ALL PREVIOUSLY DEFINED ONES! ***
//     (excluding the end-of-list enum value)
// We do not want values of existing enums to change or else it screws
// up the statistics.
enum SuggestRequestsHistogramValue {
  REQUEST_SENT = 1,
  REQUEST_INVALIDATED,
  REPLY_RECEIVED,
  MAX_SUGGEST_REQUEST_HISTOGRAM_VALUE
};

// The verbatim score for an input which is not an URL.
const int kNonURLVerbatimRelevance = 1300;

// Increments the appropriate value in the histogram by one.
void LogOmniboxSuggestRequest(
    SuggestRequestsHistogramValue request_value) {
  UMA_HISTOGRAM_ENUMERATION("Omnibox.SuggestRequests", request_value,
                            MAX_SUGGEST_REQUEST_HISTOGRAM_VALUE);
}

bool HasMultipleWords(const base::string16& text) {
  base::i18n::BreakIterator i(text, base::i18n::BreakIterator::BREAK_WORD);
  bool found_word = false;
  if (i.Init()) {
    while (i.Advance()) {
      if (i.IsWord()) {
        if (found_word)
          return true;
        found_word = true;
      }
    }
  }
  return false;
}

}  // namespace

// SearchProvider::Providers --------------------------------------------------

SearchProvider::Providers::Providers(TemplateURLService* template_url_service)
    : template_url_service_(template_url_service) {}

const TemplateURL* SearchProvider::Providers::GetDefaultProviderURL() const {
  return default_provider_.empty() ? NULL :
      template_url_service_->GetTemplateURLForKeyword(default_provider_);
}

const TemplateURL* SearchProvider::Providers::GetKeywordProviderURL() const {
  return keyword_provider_.empty() ? NULL :
      template_url_service_->GetTemplateURLForKeyword(keyword_provider_);
}


// SearchProvider::CompareScoredResults ---------------------------------------

class SearchProvider::CompareScoredResults {
 public:
  bool operator()(const SearchSuggestionParser::Result& a,
                  const SearchSuggestionParser::Result& b) {
    // Sort in descending relevance order.
    return a.relevance() > b.relevance();
  }
};


// SearchProvider -------------------------------------------------------------

// static
int SearchProvider::kMinimumTimeBetweenSuggestQueriesMs = 100;

SearchProvider::SearchProvider(
    AutocompleteProviderListener* listener,
    TemplateURLService* template_url_service,
    scoped_ptr<AutocompleteProviderClient> client)
    : BaseSearchProvider(template_url_service, client.Pass(),
                         AutocompleteProvider::TYPE_SEARCH),
      listener_(listener),
      suggest_results_pending_(0),
      providers_(template_url_service),
      answers_cache_(1) {
}

// static
std::string SearchProvider::GetSuggestMetadata(const AutocompleteMatch& match) {
  return match.GetAdditionalInfo(kSuggestMetadataKey);
}

void SearchProvider::ResetSession() {
  field_trial_triggered_in_session_ = false;
}

SearchProvider::~SearchProvider() {
}

// static
int SearchProvider::CalculateRelevanceForKeywordVerbatim(
    metrics::OmniboxInputType::Type type,
    bool prefer_keyword) {
  // This function is responsible for scoring verbatim query matches
  // for non-extension keywords.  KeywordProvider::CalculateRelevance()
  // scores verbatim query matches for extension keywords, as well as
  // for keyword matches (i.e., suggestions of a keyword itself, not a
  // suggestion of a query on a keyword search engine).  These two
  // functions are currently in sync, but there's no reason we
  // couldn't decide in the future to score verbatim matches
  // differently for extension and non-extension keywords.  If you
  // make such a change, however, you should update this comment to
  // describe it, so it's clear why the functions diverge.
  if (prefer_keyword)
    return 1500;
  return (type == metrics::OmniboxInputType::QUERY) ? 1450 : 1100;
}

// static
void SearchProvider::UpdateOldResults(
    bool minimal_changes,
    SearchSuggestionParser::Results* results) {
  // When called without |minimal_changes|, it likely means the user has
  // pressed a key.  Revise the cached results appropriately.
  if (!minimal_changes) {
    for (SearchSuggestionParser::SuggestResults::iterator sug_it =
             results->suggest_results.begin();
         sug_it != results->suggest_results.end(); ++sug_it) {
      sug_it->set_received_after_last_keystroke(false);
    }
    for (SearchSuggestionParser::NavigationResults::iterator nav_it =
             results->navigation_results.begin();
         nav_it != results->navigation_results.end(); ++nav_it) {
      nav_it->set_received_after_last_keystroke(false);
    }
  }
}

// static
ACMatches::iterator SearchProvider::FindTopMatch(ACMatches* matches) {
  ACMatches::iterator it = matches->begin();
  while ((it != matches->end()) && !it->allowed_to_be_default_match)
    ++it;
  return it;
}

void SearchProvider::Start(const AutocompleteInput& input,
                           bool minimal_changes) {
  // Do our best to load the model as early as possible.  This will reduce
  // odds of having the model not ready when really needed (a non-empty input).
  TemplateURLService* model = providers_.template_url_service();
  DCHECK(model);
  model->Load();

  matches_.clear();
  field_trial_triggered_ = false;

  // Can't return search/suggest results for bogus input.
  if (input.type() == metrics::OmniboxInputType::INVALID) {
    Stop(true);
    return;
  }

  keyword_input_ = input;
  const TemplateURL* keyword_provider =
      KeywordProvider::GetSubstitutingTemplateURLForInput(model,
                                                          &keyword_input_);
  if (keyword_provider == NULL)
    keyword_input_.Clear();
  else if (keyword_input_.text().empty())
    keyword_provider = NULL;

  const TemplateURL* default_provider = model->GetDefaultSearchProvider();
  if (default_provider &&
      !default_provider->SupportsReplacement(model->search_terms_data()))
    default_provider = NULL;

  if (keyword_provider == default_provider)
    default_provider = NULL;  // No use in querying the same provider twice.

  if (!default_provider && !keyword_provider) {
    // No valid providers.
    Stop(true);
    return;
  }

  // If we're still running an old query but have since changed the query text
  // or the providers, abort the query.
  base::string16 default_provider_keyword(default_provider ?
      default_provider->keyword() : base::string16());
  base::string16 keyword_provider_keyword(keyword_provider ?
      keyword_provider->keyword() : base::string16());
  if (!minimal_changes ||
      !providers_.equal(default_provider_keyword, keyword_provider_keyword)) {
    // Cancel any in-flight suggest requests.
    if (!done_)
      Stop(false);
  }

  providers_.set(default_provider_keyword, keyword_provider_keyword);

  if (input.text().empty()) {
    // User typed "?" alone.  Give them a placeholder result indicating what
    // this syntax does.
    if (default_provider) {
      AutocompleteMatch match;
      match.provider = this;
      match.contents.assign(l10n_util::GetStringUTF16(IDS_EMPTY_KEYWORD_VALUE));
      match.contents_class.push_back(
          ACMatchClassification(0, ACMatchClassification::NONE));
      match.keyword = providers_.default_provider();
      match.allowed_to_be_default_match = true;
      matches_.push_back(match);
    }
    Stop(true);
    return;
  }

  input_ = input;

  DoHistoryQuery(minimal_changes);
  DoAnswersQuery(input);
  StartOrStopSuggestQuery(minimal_changes);
  UpdateMatches();
}

void SearchProvider::Stop(bool clear_cached_results) {
  StopSuggest();
  done_ = true;

  if (clear_cached_results)
    ClearAllResults();
}

const TemplateURL* SearchProvider::GetTemplateURL(bool is_keyword) const {
  return is_keyword ? providers_.GetKeywordProviderURL()
                    : providers_.GetDefaultProviderURL();
}

const AutocompleteInput SearchProvider::GetInput(bool is_keyword) const {
  return is_keyword ? keyword_input_ : input_;
}

bool SearchProvider::ShouldAppendExtraParams(
    const SearchSuggestionParser::SuggestResult& result) const {
  return !result.from_keyword_provider() ||
      providers_.default_provider().empty();
}

void SearchProvider::RecordDeletionResult(bool success) {
  if (success) {
    base::RecordAction(
        base::UserMetricsAction("Omnibox.ServerSuggestDelete.Success"));
  } else {
    base::RecordAction(
        base::UserMetricsAction("Omnibox.ServerSuggestDelete.Failure"));
  }
}

void SearchProvider::OnURLFetchComplete(const net::URLFetcher* source) {
  DCHECK(!done_);
  --suggest_results_pending_;
  DCHECK_GE(suggest_results_pending_, 0);  // Should never go negative.

  const bool is_keyword = source == keyword_fetcher_.get();

  // Ensure the request succeeded and that the provider used is still available.
  // A verbatim match cannot be generated without this provider, causing errors.
  const bool request_succeeded =
      source->GetStatus().is_success() && (source->GetResponseCode() == 200) &&
      GetTemplateURL(is_keyword);

  LogFetchComplete(request_succeeded, is_keyword);

  bool results_updated = false;
  if (request_succeeded) {
    scoped_ptr<base::Value> data(SearchSuggestionParser::DeserializeJsonData(
        SearchSuggestionParser::ExtractJsonData(source)));
    if (data) {
      SearchSuggestionParser::Results* results =
          is_keyword ? &keyword_results_ : &default_results_;
      results_updated = ParseSuggestResults(*data, -1, is_keyword, results);
      if (results_updated)
        SortResults(is_keyword, results);
    }
  }
  UpdateMatches();
  if (done_ || results_updated)
    listener_->OnProviderUpdate(results_updated);
}

void SearchProvider::StopSuggest() {
  // Increment the appropriate field in the histogram by the number of
  // pending requests that were invalidated.
  for (int i = 0; i < suggest_results_pending_; ++i)
    LogOmniboxSuggestRequest(REQUEST_INVALIDATED);
  suggest_results_pending_ = 0;
  timer_.Stop();
  // Stop any in-progress URL fetches.
  keyword_fetcher_.reset();
  default_fetcher_.reset();
}

void SearchProvider::ClearAllResults() {
  keyword_results_.Clear();
  default_results_.Clear();
}

void SearchProvider::UpdateMatchContentsClass(
    const base::string16& input_text,
    SearchSuggestionParser::Results* results) {
  for (SearchSuggestionParser::SuggestResults::iterator sug_it =
           results->suggest_results.begin();
       sug_it != results->suggest_results.end(); ++sug_it) {
    sug_it->ClassifyMatchContents(false, input_text);
  }
  const std::string languages(client_->AcceptLanguages());
  for (SearchSuggestionParser::NavigationResults::iterator nav_it =
           results->navigation_results.begin();
       nav_it != results->navigation_results.end(); ++nav_it) {
    nav_it->CalculateAndClassifyMatchContents(false, input_text, languages);
  }
}

void SearchProvider::SortResults(bool is_keyword,
                                 SearchSuggestionParser::Results* results) {
  // Ignore suggested scores for non-keyword matches in keyword mode; if the
  // server is allowed to score these, it could interfere with the user's
  // ability to get good keyword results.
  const bool abandon_suggested_scores =
      !is_keyword && !providers_.keyword_provider().empty();
  // Apply calculated relevance scores to suggestions if valid relevances were
  // not provided or we're abandoning suggested scores entirely.
  if (!results->relevances_from_server || abandon_suggested_scores) {
    ApplyCalculatedSuggestRelevance(&results->suggest_results);
    ApplyCalculatedNavigationRelevance(&results->navigation_results);
    // If abandoning scores entirely, also abandon the verbatim score.
    if (abandon_suggested_scores)
      results->verbatim_relevance = -1;
  }

  // Keep the result lists sorted.
  const CompareScoredResults comparator = CompareScoredResults();
  std::stable_sort(results->suggest_results.begin(),
                   results->suggest_results.end(),
                   comparator);
  std::stable_sort(results->navigation_results.begin(),
                   results->navigation_results.end(),
                   comparator);
}

void SearchProvider::LogFetchComplete(bool success, bool is_keyword) {
  LogOmniboxSuggestRequest(REPLY_RECEIVED);
  // Record response time for suggest requests sent to Google.  We care
  // only about the common case: the Google default provider used in
  // non-keyword mode.
  const TemplateURL* default_url = providers_.GetDefaultProviderURL();
  if (!is_keyword && default_url &&
      (TemplateURLPrepopulateData::GetEngineType(
          *default_url,
          providers_.template_url_service()->search_terms_data()) ==
       SEARCH_ENGINE_GOOGLE)) {
    const base::TimeDelta elapsed_time =
        base::TimeTicks::Now() - time_suggest_request_sent_;
    if (success) {
      UMA_HISTOGRAM_TIMES("Omnibox.SuggestRequest.Success.GoogleResponseTime",
                          elapsed_time);
    } else {
      UMA_HISTOGRAM_TIMES("Omnibox.SuggestRequest.Failure.GoogleResponseTime",
                          elapsed_time);
    }
  }
}

void SearchProvider::UpdateMatches() {
  PersistTopSuggestions(&default_results_);
  PersistTopSuggestions(&keyword_results_);
  ConvertResultsToAutocompleteMatches();

  // Check constraints that may be violated by suggested relevances.
  if (!matches_.empty() &&
      (default_results_.HasServerProvidedScores() ||
       keyword_results_.HasServerProvidedScores())) {
    // These blocks attempt to repair undesirable behavior by suggested
    // relevances with minimal impact, preserving other suggested relevances.

    const TemplateURL* keyword_url = providers_.GetKeywordProviderURL();
    const bool is_extension_keyword = (keyword_url != NULL) &&
        (keyword_url->GetType() == TemplateURL::OMNIBOX_API_EXTENSION);
    if ((keyword_url != NULL) && !is_extension_keyword &&
        (FindTopMatch() == matches_.end())) {
      // In non-extension keyword mode, disregard the keyword verbatim suggested
      // relevance if necessary, so at least one match is allowed to be default.
      // (In extension keyword mode this is not necessary because the extension
      // will return a default match.)  Give keyword verbatim the lowest
      // non-zero score to best reflect what the server desired.
      DCHECK_EQ(0, keyword_results_.verbatim_relevance);
      keyword_results_.verbatim_relevance = 1;
      ConvertResultsToAutocompleteMatches();
    }
    if (IsTopMatchSearchWithURLInput()) {
      // Disregard the suggested search and verbatim relevances if the input
      // type is URL and the top match is a highly-ranked search suggestion.
      // For example, prevent a search for "foo.com" from outranking another
      // provider's navigation for "foo.com" or "foo.com/url_from_history".
      ApplyCalculatedSuggestRelevance(&keyword_results_.suggest_results);
      ApplyCalculatedSuggestRelevance(&default_results_.suggest_results);
      default_results_.verbatim_relevance = -1;
      keyword_results_.verbatim_relevance = -1;
      ConvertResultsToAutocompleteMatches();
    }
    if (!is_extension_keyword && (FindTopMatch() == matches_.end())) {
      // Guarantee that SearchProvider returns a legal default match (except
      // when in extension-based keyword mode).  The omnibox always needs at
      // least one legal default match, and it relies on SearchProvider in
      // combination with KeywordProvider (for extension-based keywords) to
      // always return one.  Give the verbatim suggestion the lowest non-zero
      // scores to best reflect what the server desired.
      DCHECK_EQ(0, default_results_.verbatim_relevance);
      default_results_.verbatim_relevance = 1;
      // We do not have to alter keyword_results_.verbatim_relevance here.
      // If the user is in keyword mode, we already reverted (earlier in this
      // function) the instructions to suppress keyword verbatim.
      ConvertResultsToAutocompleteMatches();
    }
    DCHECK(!IsTopMatchSearchWithURLInput());
    DCHECK(is_extension_keyword || (FindTopMatch() != matches_.end()));
  }
  UMA_HISTOGRAM_CUSTOM_COUNTS(
      "Omnibox.SearchProviderMatches", matches_.size(), 1, 6, 7);

  // Record the top suggestion (if any) for future use.
  top_query_suggestion_match_contents_ = base::string16();
  top_navigation_suggestion_ = GURL();
  ACMatches::const_iterator first_match = FindTopMatch();
  if ((first_match != matches_.end()) &&
      !first_match->inline_autocompletion.empty()) {
    // Identify if this match came from a query suggestion or a navsuggestion.
    // In either case, extracts the identifying feature of the suggestion
    // (query string or navigation url).
    if (AutocompleteMatch::IsSearchType(first_match->type))
      top_query_suggestion_match_contents_ = first_match->contents;
    else
      top_navigation_suggestion_ = first_match->destination_url;
  }

  UpdateDone();
}

void SearchProvider::Run() {
  // Start a new request with the current input.
  suggest_results_pending_ = 0;
  time_suggest_request_sent_ = base::TimeTicks::Now();

  default_fetcher_.reset(CreateSuggestFetcher(kDefaultProviderURLFetcherID,
      providers_.GetDefaultProviderURL(), input_));
  keyword_fetcher_.reset(CreateSuggestFetcher(kKeywordProviderURLFetcherID,
      providers_.GetKeywordProviderURL(), keyword_input_));

  // Both the above can fail if the providers have been modified or deleted
  // since the query began.
  if (suggest_results_pending_ == 0) {
    UpdateDone();
    // We only need to update the listener if we're actually done.
    if (done_)
      listener_->OnProviderUpdate(false);
  }
}

void SearchProvider::DoHistoryQuery(bool minimal_changes) {
  // The history query results are synchronous, so if minimal_changes is true,
  // we still have the last results and don't need to do anything.
  if (minimal_changes)
    return;

  keyword_history_results_.clear();
  default_history_results_.clear();

  if (OmniboxFieldTrial::SearchHistoryDisable(
      input_.current_page_classification()))
    return;

  history::URLDatabase* url_db = client_->InMemoryDatabase();
  if (!url_db)
    return;

  // Request history for both the keyword and default provider.  We grab many
  // more matches than we'll ultimately clamp to so that if there are several
  // recent multi-word matches who scores are lowered (see
  // AddHistoryResultsToMap()), they won't crowd out older, higher-scoring
  // matches.  Note that this doesn't fix the problem entirely, but merely
  // limits it to cases with a very large number of such multi-word matches; for
  // now, this seems OK compared with the complexity of a real fix, which would
  // require multiple searches and tracking of "single- vs. multi-word" in the
  // database.
  int num_matches = kMaxMatches * 5;
  const TemplateURL* default_url = providers_.GetDefaultProviderURL();
  if (default_url) {
    const base::TimeTicks start_time = base::TimeTicks::Now();
    url_db->GetMostRecentKeywordSearchTerms(default_url->id(), input_.text(),
        num_matches, &default_history_results_);
    UMA_HISTOGRAM_TIMES(
        "Omnibox.SearchProvider.GetMostRecentKeywordTermsDefaultProviderTime",
        base::TimeTicks::Now() - start_time);
  }
  const TemplateURL* keyword_url = providers_.GetKeywordProviderURL();
  if (keyword_url) {
    url_db->GetMostRecentKeywordSearchTerms(keyword_url->id(),
        keyword_input_.text(), num_matches, &keyword_history_results_);
  }
}

void SearchProvider::StartOrStopSuggestQuery(bool minimal_changes) {
  if (!IsQuerySuitableForSuggest()) {
    StopSuggest();
    ClearAllResults();
    return;
  }

  // For the minimal_changes case, if we finished the previous query and still
  // have its results, or are allowed to keep running it, just do that, rather
  // than starting a new query.
  if (minimal_changes &&
      (!default_results_.suggest_results.empty() ||
       !default_results_.navigation_results.empty() ||
       !keyword_results_.suggest_results.empty() ||
       !keyword_results_.navigation_results.empty() ||
       (!done_ && input_.want_asynchronous_matches())))
    return;

  // We can't keep running any previous query, so halt it.
  StopSuggest();

  UpdateAllOldResults(minimal_changes);

  // Update the content classifications of remaining results so they look good
  // against the current input.
  UpdateMatchContentsClass(input_.text(), &default_results_);
  if (!keyword_input_.text().empty())
    UpdateMatchContentsClass(keyword_input_.text(), &keyword_results_);

  // We can't start a new query if we're only allowed synchronous results.
  if (!input_.want_asynchronous_matches())
    return;

  // To avoid flooding the suggest server, don't send a query until at
  // least 100 ms since the last query.
  base::TimeTicks next_suggest_time(time_suggest_request_sent_ +
      base::TimeDelta::FromMilliseconds(kMinimumTimeBetweenSuggestQueriesMs));
  base::TimeTicks now(base::TimeTicks::Now());
  if (now >= next_suggest_time) {
    Run();
    return;
  }
  timer_.Start(FROM_HERE, next_suggest_time - now, this, &SearchProvider::Run);
}

bool SearchProvider::IsQuerySuitableForSuggest() const {
  // Don't run Suggest in incognito mode, if the engine doesn't support it, or
  // if the user has disabled it.
  const TemplateURL* default_url = providers_.GetDefaultProviderURL();
  const TemplateURL* keyword_url = providers_.GetKeywordProviderURL();
  if (client_->IsOffTheRecord() ||
      ((!default_url || default_url->suggestions_url().empty()) &&
       (!keyword_url || keyword_url->suggestions_url().empty())) ||
      !client_->SearchSuggestEnabled())
    return false;

  // If the input type might be a URL, we take extra care so that private data
  // isn't sent to the server.

  // FORCED_QUERY means the user is explicitly asking us to search for this, so
  // we assume it isn't a URL and/or there isn't private data.
  if (input_.type() == metrics::OmniboxInputType::FORCED_QUERY)
    return true;

  // Next we check the scheme.  If this is UNKNOWN/URL with a scheme that isn't
  // http/https/ftp, we shouldn't send it.  Sending things like file: and data:
  // is both a waste of time and a disclosure of potentially private, local
  // data.  Other "schemes" may actually be usernames, and we don't want to send
  // passwords.  If the scheme is OK, we still need to check other cases below.
  // If this is QUERY, then the presence of these schemes means the user
  // explicitly typed one, and thus this is probably a URL that's being entered
  // and happens to currently be invalid -- in which case we again want to run
  // our checks below.  Other QUERY cases are less likely to be URLs and thus we
  // assume we're OK.
  if (!LowerCaseEqualsASCII(input_.scheme(), url::kHttpScheme) &&
      !LowerCaseEqualsASCII(input_.scheme(), url::kHttpsScheme) &&
      !LowerCaseEqualsASCII(input_.scheme(), url::kFtpScheme))
    return (input_.type() == metrics::OmniboxInputType::QUERY);

  // Don't send URLs with usernames, queries or refs.  Some of these are
  // private, and the Suggest server is unlikely to have any useful results
  // for any of them.  Also don't send URLs with ports, as we may initially
  // think that a username + password is a host + port (and we don't want to
  // send usernames/passwords), and even if the port really is a port, the
  // server is once again unlikely to have and useful results.
  // Note that we only block based on refs if the input is URL-typed, as search
  // queries can legitimately have #s in them which the URL parser
  // overaggressively categorizes as a url with a ref.
  const url::Parsed& parts = input_.parts();
  if (parts.username.is_nonempty() || parts.port.is_nonempty() ||
      parts.query.is_nonempty() ||
      (parts.ref.is_nonempty() &&
       (input_.type() == metrics::OmniboxInputType::URL)))
    return false;

  // Don't send anything for https except the hostname.  Hostnames are OK
  // because they are visible when the TCP connection is established, but the
  // specific path may reveal private information.
  if (LowerCaseEqualsASCII(input_.scheme(), url::kHttpsScheme) &&
      parts.path.is_nonempty())
    return false;

  return true;
}

void SearchProvider::UpdateAllOldResults(bool minimal_changes) {
  if (keyword_input_.text().empty()) {
    // User is either in keyword mode with a blank input or out of
    // keyword mode entirely.
    keyword_results_.Clear();
  }
  UpdateOldResults(minimal_changes, &default_results_);
  UpdateOldResults(minimal_changes, &keyword_results_);
}

void SearchProvider::PersistTopSuggestions(
    SearchSuggestionParser::Results* results) {
  // Mark any results matching the current top results as having been received
  // prior to the last keystroke.  That prevents asynchronous updates from
  // clobbering top results, which may be used for inline autocompletion.
  // Other results don't need similar changes, because they shouldn't be
  // displayed asynchronously anyway.
  if (!top_query_suggestion_match_contents_.empty()) {
    for (SearchSuggestionParser::SuggestResults::iterator sug_it =
             results->suggest_results.begin();
         sug_it != results->suggest_results.end(); ++sug_it) {
      if (sug_it->match_contents() == top_query_suggestion_match_contents_)
        sug_it->set_received_after_last_keystroke(false);
    }
  }
  if (top_navigation_suggestion_.is_valid()) {
    for (SearchSuggestionParser::NavigationResults::iterator nav_it =
             results->navigation_results.begin();
         nav_it != results->navigation_results.end(); ++nav_it) {
      if (nav_it->url() == top_navigation_suggestion_)
        nav_it->set_received_after_last_keystroke(false);
    }
  }
}

void SearchProvider::ApplyCalculatedSuggestRelevance(
    SearchSuggestionParser::SuggestResults* list) {
  for (size_t i = 0; i < list->size(); ++i) {
    SearchSuggestionParser::SuggestResult& result = (*list)[i];
    result.set_relevance(
        result.CalculateRelevance(input_, providers_.has_keyword_provider()) +
        (list->size() - i - 1));
    result.set_relevance_from_server(false);
  }
}

void SearchProvider::ApplyCalculatedNavigationRelevance(
    SearchSuggestionParser::NavigationResults* list) {
  for (size_t i = 0; i < list->size(); ++i) {
    SearchSuggestionParser::NavigationResult& result = (*list)[i];
    result.set_relevance(
        result.CalculateRelevance(input_, providers_.has_keyword_provider()) +
        (list->size() - i - 1));
    result.set_relevance_from_server(false);
  }
}

net::URLFetcher* SearchProvider::CreateSuggestFetcher(
    int id,
    const TemplateURL* template_url,
    const AutocompleteInput& input) {
  if (!template_url || template_url->suggestions_url().empty())
    return NULL;

  // Bail if the suggestion URL is invalid with the given replacements.
  TemplateURLRef::SearchTermsArgs search_term_args(input.text());
  search_term_args.input_type = input.type();
  search_term_args.cursor_position = input.cursor_position();
  search_term_args.page_classification = input.current_page_classification();
  if (OmniboxFieldTrial::EnableAnswersInSuggest()) {
    search_term_args.session_token = GetSessionToken();
    if (!prefetch_data_.full_query_text.empty()) {
      search_term_args.prefetch_query =
          base::UTF16ToUTF8(prefetch_data_.full_query_text);
      search_term_args.prefetch_query_type =
          base::UTF16ToUTF8(prefetch_data_.query_type);
    }
  }
  GURL suggest_url(template_url->suggestions_url_ref().ReplaceSearchTerms(
      search_term_args,
      providers_.template_url_service()->search_terms_data()));
  if (!suggest_url.is_valid())
    return NULL;
  // Send the current page URL if user setting and URL requirements are met and
  // the user is in the field trial.
  if (CanSendURL(current_page_url_, suggest_url, template_url,
                 input.current_page_classification(),
                 template_url_service_->search_terms_data(), client_.get()) &&
      OmniboxFieldTrial::InZeroSuggestAfterTypingFieldTrial()) {
    search_term_args.current_page_url = current_page_url_.spec();
    // Create the suggest URL again with the current page URL.
    suggest_url = GURL(template_url->suggestions_url_ref().ReplaceSearchTerms(
        search_term_args,
        providers_.template_url_service()->search_terms_data()));
  }

  suggest_results_pending_++;
  LogOmniboxSuggestRequest(REQUEST_SENT);

  net::URLFetcher* fetcher =
      net::URLFetcher::Create(id, suggest_url, net::URLFetcher::GET, this);
  fetcher->SetRequestContext(client_->RequestContext());
  fetcher->SetLoadFlags(net::LOAD_DO_NOT_SAVE_COOKIES);
  // Add Chrome experiment state to the request headers.
  net::HttpRequestHeaders headers;
  variations::VariationsHttpHeaderProvider::GetInstance()->AppendHeaders(
      fetcher->GetOriginalURL(), client_->IsOffTheRecord(), false, &headers);
  fetcher->SetExtraRequestHeaders(headers.ToString());
  fetcher->Start();
  return fetcher;
}

void SearchProvider::ConvertResultsToAutocompleteMatches() {
  // Convert all the results to matches and add them to a map, so we can keep
  // the most relevant match for each result.
  base::TimeTicks start_time(base::TimeTicks::Now());
  MatchMap map;
  const base::Time no_time;
  int did_not_accept_keyword_suggestion =
      keyword_results_.suggest_results.empty() ?
      TemplateURLRef::NO_SUGGESTIONS_AVAILABLE :
      TemplateURLRef::NO_SUGGESTION_CHOSEN;

  bool relevance_from_server;
  int verbatim_relevance = GetVerbatimRelevance(&relevance_from_server);
  int did_not_accept_default_suggestion =
      default_results_.suggest_results.empty() ?
      TemplateURLRef::NO_SUGGESTIONS_AVAILABLE :
      TemplateURLRef::NO_SUGGESTION_CHOSEN;
  const TemplateURL* keyword_url = providers_.GetKeywordProviderURL();
  if (verbatim_relevance > 0) {
    const base::string16& trimmed_verbatim =
        base::CollapseWhitespace(input_.text(), false);

    // Verbatim results don't get suggestions and hence, answers.
    // Scan previous matches if the last answer-bearing suggestion matches
    // verbatim, and if so, copy over answer contents.
    base::string16 answer_contents;
    base::string16 answer_type;
    for (ACMatches::iterator it = matches_.begin(); it != matches_.end();
         ++it) {
      if (!it->answer_contents.empty() &&
          it->fill_into_edit == trimmed_verbatim) {
        answer_contents = it->answer_contents;
        answer_type = it->answer_type;
        break;
      }
    }

    SearchSuggestionParser::SuggestResult verbatim(
        trimmed_verbatim, AutocompleteMatchType::SEARCH_WHAT_YOU_TYPED,
        trimmed_verbatim, base::string16(), base::string16(), answer_contents,
        answer_type, std::string(), std::string(), false, verbatim_relevance,
        relevance_from_server, false, trimmed_verbatim);
    AddMatchToMap(verbatim, std::string(), did_not_accept_default_suggestion,
                  false, keyword_url != NULL, &map);
  }
  if (!keyword_input_.text().empty()) {
    // We only create the verbatim search query match for a keyword
    // if it's not an extension keyword.  Extension keywords are handled
    // in KeywordProvider::Start().  (Extensions are complicated...)
    // Note: in this provider, SEARCH_OTHER_ENGINE must correspond
    // to the keyword verbatim search query.  Do not create other matches
    // of type SEARCH_OTHER_ENGINE.
    if (keyword_url &&
        (keyword_url->GetType() != TemplateURL::OMNIBOX_API_EXTENSION)) {
      bool keyword_relevance_from_server;
      const int keyword_verbatim_relevance =
          GetKeywordVerbatimRelevance(&keyword_relevance_from_server);
      if (keyword_verbatim_relevance > 0) {
        const base::string16& trimmed_verbatim =
            base::CollapseWhitespace(keyword_input_.text(), false);
        SearchSuggestionParser::SuggestResult verbatim(
            trimmed_verbatim, AutocompleteMatchType::SEARCH_OTHER_ENGINE,
            trimmed_verbatim, base::string16(), base::string16(),
            base::string16(), base::string16(), std::string(), std::string(),
            true, keyword_verbatim_relevance, keyword_relevance_from_server,
            false, trimmed_verbatim);
        AddMatchToMap(verbatim, std::string(),
                      did_not_accept_keyword_suggestion, false, true, &map);
      }
    }
  }
  AddHistoryResultsToMap(keyword_history_results_, true,
                         did_not_accept_keyword_suggestion, &map);
  AddHistoryResultsToMap(default_history_results_, false,
                         did_not_accept_default_suggestion, &map);

  AddSuggestResultsToMap(keyword_results_.suggest_results,
                         keyword_results_.metadata, &map);
  AddSuggestResultsToMap(default_results_.suggest_results,
                         default_results_.metadata, &map);

  ACMatches matches;
  for (MatchMap::const_iterator i(map.begin()); i != map.end(); ++i)
    matches.push_back(i->second);

  AddNavigationResultsToMatches(keyword_results_.navigation_results, &matches);
  AddNavigationResultsToMatches(default_results_.navigation_results, &matches);

  // Now add the most relevant matches to |matches_|.  We take up to kMaxMatches
  // suggest/navsuggest matches, regardless of origin.  We always include in
  // that set a legal default match if possible.  If Instant Extended is enabled
  // and we have server-provided (and thus hopefully more accurate) scores for
  // some suggestions, we allow more of those, until we reach
  // AutocompleteResult::kMaxMatches total matches (that is, enough to fill the
  // whole popup).
  //
  // We will always return any verbatim matches, no matter how we obtained their
  // scores, unless we have already accepted AutocompleteResult::kMaxMatches
  // higher-scoring matches under the conditions above.
  std::sort(matches.begin(), matches.end(), &AutocompleteMatch::MoreRelevant);
  matches_.clear();
  // Guarantee that if there's a legal default match anywhere in the result
  // set that it'll get returned.  The rotate() call does this by moving the
  // default match to the front of the list.
  ACMatches::iterator default_match = FindTopMatch(&matches);
  if (default_match != matches.end())
    std::rotate(matches.begin(), default_match, default_match + 1);

  size_t num_suggestions = 0;
  for (ACMatches::const_iterator i(matches.begin());
       (i != matches.end()) &&
           (matches_.size() < AutocompleteResult::kMaxMatches);
       ++i) {
    // SEARCH_OTHER_ENGINE is only used in the SearchProvider for the keyword
    // verbatim result, so this condition basically means "if this match is a
    // suggestion of some sort".
    if ((i->type != AutocompleteMatchType::SEARCH_WHAT_YOU_TYPED) &&
        (i->type != AutocompleteMatchType::SEARCH_OTHER_ENGINE)) {
      // If we've already hit the limit on non-server-scored suggestions, and
      // this isn't a server-scored suggestion we can add, skip it.
      if ((num_suggestions >= kMaxMatches) &&
          (!chrome::IsInstantExtendedAPIEnabled() ||
           (i->GetAdditionalInfo(kRelevanceFromServerKey) != kTrue))) {
        continue;
      }

      ++num_suggestions;
    }

    matches_.push_back(*i);
  }
  UMA_HISTOGRAM_TIMES("Omnibox.SearchProvider.ConvertResultsTime",
                      base::TimeTicks::Now() - start_time);
}

ACMatches::const_iterator SearchProvider::FindTopMatch() const {
  ACMatches::const_iterator it = matches_.begin();
  while ((it != matches_.end()) && !it->allowed_to_be_default_match)
    ++it;
  return it;
}

bool SearchProvider::IsTopMatchSearchWithURLInput() const {
  ACMatches::const_iterator first_match = FindTopMatch();
  return (input_.type() == metrics::OmniboxInputType::URL) &&
      (first_match != matches_.end()) &&
      (first_match->relevance > CalculateRelevanceForVerbatim()) &&
      (first_match->type != AutocompleteMatchType::NAVSUGGEST) &&
      (first_match->type != AutocompleteMatchType::NAVSUGGEST_PERSONALIZED);
}

void SearchProvider::AddNavigationResultsToMatches(
    const SearchSuggestionParser::NavigationResults& navigation_results,
    ACMatches* matches) {
  for (SearchSuggestionParser::NavigationResults::const_iterator it =
           navigation_results.begin(); it != navigation_results.end(); ++it) {
    matches->push_back(NavigationToMatch(*it));
    // In the absence of suggested relevance scores, use only the single
    // highest-scoring result.  (The results are already sorted by relevance.)
    if (!it->relevance_from_server())
      return;
  }
}

void SearchProvider::AddHistoryResultsToMap(const HistoryResults& results,
                                            bool is_keyword,
                                            int did_not_accept_suggestion,
                                            MatchMap* map) {
  if (results.empty())
    return;

  base::TimeTicks start_time(base::TimeTicks::Now());
  bool prevent_inline_autocomplete = input_.prevent_inline_autocomplete() ||
      (input_.type() == metrics::OmniboxInputType::URL);
  const base::string16& input_text =
      is_keyword ? keyword_input_.text() : input_.text();
  bool input_multiple_words = HasMultipleWords(input_text);

  SearchSuggestionParser::SuggestResults scored_results;
  if (!prevent_inline_autocomplete && input_multiple_words) {
    // ScoreHistoryResults() allows autocompletion of multi-word, 1-visit
    // queries if the input also has multiple words.  But if we were already
    // scoring a multi-word, multi-visit query aggressively, and the current
    // input is still a prefix of it, then changing the suggestion suddenly
    // feels wrong.  To detect this case, first score as if only one word has
    // been typed, then check if the best result came from aggressive search
    // history scoring.  If it did, then just keep that score set.  This
    // 1200 the lowest possible score in CalculateRelevanceForHistory()'s
    // aggressive-scoring curve.
    scored_results = ScoreHistoryResults(results, prevent_inline_autocomplete,
                                         false, input_text, is_keyword);
    if ((scored_results.front().relevance() < 1200) ||
        !HasMultipleWords(scored_results.front().suggestion()))
      scored_results.clear();  // Didn't detect the case above, score normally.
  }
  if (scored_results.empty())
    scored_results = ScoreHistoryResults(results, prevent_inline_autocomplete,
                                         input_multiple_words, input_text,
                                         is_keyword);
  for (SearchSuggestionParser::SuggestResults::const_iterator i(
           scored_results.begin()); i != scored_results.end(); ++i) {
    AddMatchToMap(*i, std::string(), did_not_accept_suggestion, true,
                  providers_.GetKeywordProviderURL() != NULL, map);
  }
  UMA_HISTOGRAM_TIMES("Omnibox.SearchProvider.AddHistoryResultsTime",
                      base::TimeTicks::Now() - start_time);
}

SearchSuggestionParser::SuggestResults SearchProvider::ScoreHistoryResults(
    const HistoryResults& results,
    bool base_prevent_inline_autocomplete,
    bool input_multiple_words,
    const base::string16& input_text,
    bool is_keyword) {
  SearchSuggestionParser::SuggestResults scored_results;
  // True if the user has asked this exact query previously.
  bool found_what_you_typed_match = false;
  const bool prevent_search_history_inlining =
      OmniboxFieldTrial::SearchHistoryPreventInlining(
          input_.current_page_classification());
  const base::string16& trimmed_input =
      base::CollapseWhitespace(input_text, false);
  for (HistoryResults::const_iterator i(results.begin()); i != results.end();
       ++i) {
    const base::string16& trimmed_suggestion =
        base::CollapseWhitespace(i->term, false);

    // Don't autocomplete multi-word queries that have only been seen once
    // unless the user has typed more than one word.
    bool prevent_inline_autocomplete = base_prevent_inline_autocomplete ||
        (!input_multiple_words && (i->visits < 2) &&
         HasMultipleWords(trimmed_suggestion));

    int relevance = CalculateRelevanceForHistory(
        i->time, is_keyword, !prevent_inline_autocomplete,
        prevent_search_history_inlining);
    // Add the match to |scored_results| by putting the what-you-typed match
    // on the front and appending all other matches.  We want the what-you-
    // typed match to always be first.
    SearchSuggestionParser::SuggestResults::iterator insertion_position =
        scored_results.end();
    if (trimmed_suggestion == trimmed_input) {
      found_what_you_typed_match = true;
      insertion_position = scored_results.begin();
    }
    SearchSuggestionParser::SuggestResult history_suggestion(
        trimmed_suggestion, AutocompleteMatchType::SEARCH_HISTORY,
        trimmed_suggestion, base::string16(), base::string16(),
        base::string16(), base::string16(), std::string(), std::string(),
        is_keyword, relevance, false, false, trimmed_input);
    // History results are synchronous; they are received on the last keystroke.
    history_suggestion.set_received_after_last_keystroke(false);
    scored_results.insert(insertion_position, history_suggestion);
  }

  // History returns results sorted for us.  However, we may have docked some
  // results' scores, so things are no longer in order.  While keeping the
  // what-you-typed match at the front (if it exists), do a stable sort to get
  // things back in order without otherwise disturbing results with equal
  // scores, then force the scores to be unique, so that the order in which
  // they're shown is deterministic.
  std::stable_sort(scored_results.begin() +
                       (found_what_you_typed_match ? 1 : 0),
                   scored_results.end(),
                   CompareScoredResults());

  // Don't autocomplete to search terms that would normally be treated as URLs
  // when typed. For example, if the user searched for "google.com" and types
  // "goog", don't autocomplete to the search term "google.com". Otherwise,
  // the input will look like a URL but act like a search, which is confusing.
  // The 1200 relevance score threshold in the test below is the lowest
  // possible score in CalculateRelevanceForHistory()'s aggressive-scoring
  // curve.  This is an appropriate threshold to use to decide if we're overly
  // aggressively inlining because, if we decide the answer is yes, the
  // way we resolve it it to not use the aggressive-scoring curve.
  // NOTE: We don't check for autocompleting to URLs in the following cases:
  //  * When inline autocomplete is disabled, we won't be inline autocompleting
  //    this term, so we don't need to worry about confusion as much.  This
  //    also prevents calling Classify() again from inside the classifier
  //    (which will corrupt state and likely crash), since the classifier
  //    always disables inline autocomplete.
  //  * When the user has typed the whole string before as a query, then it's
  //    likely the user has no expectation that term should be interpreted as
  //    as a URL, so we need not do anything special to preserve user
  //    expectation.
  int last_relevance = 0;
  if (!base_prevent_inline_autocomplete && !found_what_you_typed_match &&
      scored_results.front().relevance() >= 1200) {
    AutocompleteMatch match;
    client_->Classify(scored_results.front().suggestion(), false, false,
                      input_.current_page_classification(), &match, NULL);
    // Demote this match that would normally be interpreted as a URL to have
    // the highest score a previously-issued search query could have when
    // scoring with the non-aggressive method.  A consequence of demoting
    // by revising |last_relevance| is that this match and all following
    // matches get demoted; the relative order of matches is preserved.
    // One could imagine demoting only those matches that might cause
    // confusion (which, by the way, might change the relative order of
    // matches.  We have decided to go with the simple demote-all approach
    // because selective demotion requires multiple Classify() calls and
    // such calls can be expensive (as expensive as running the whole
    // autocomplete system).
    if (!AutocompleteMatch::IsSearchType(match.type)) {
      last_relevance = CalculateRelevanceForHistory(
          base::Time::Now(), is_keyword, false,
          prevent_search_history_inlining);
    }
  }

  for (SearchSuggestionParser::SuggestResults::iterator i(
           scored_results.begin()); i != scored_results.end(); ++i) {
    if ((last_relevance != 0) && (i->relevance() >= last_relevance))
      i->set_relevance(last_relevance - 1);
    last_relevance = i->relevance();
  }

  return scored_results;
}

void SearchProvider::AddSuggestResultsToMap(
    const SearchSuggestionParser::SuggestResults& results,
    const std::string& metadata,
    MatchMap* map) {
  for (size_t i = 0; i < results.size(); ++i) {
    AddMatchToMap(results[i], metadata, i, false,
                  providers_.GetKeywordProviderURL() != NULL, map);
  }
}

int SearchProvider::GetVerbatimRelevance(bool* relevance_from_server) const {
  // Use the suggested verbatim relevance score if it is non-negative (valid),
  // if inline autocomplete isn't prevented (always show verbatim on backspace),
  // and if it won't suppress verbatim, leaving no default provider matches.
  // Otherwise, if the default provider returned no matches and was still able
  // to suppress verbatim, the user would have no search/nav matches and may be
  // left unable to search using their default provider from the omnibox.
  // Check for results on each verbatim calculation, as results from older
  // queries (on previous input) may be trimmed for failing to inline new input.
  bool use_server_relevance =
      (default_results_.verbatim_relevance >= 0) &&
      !input_.prevent_inline_autocomplete() &&
      ((default_results_.verbatim_relevance > 0) ||
       !default_results_.suggest_results.empty() ||
       !default_results_.navigation_results.empty());
  if (relevance_from_server)
    *relevance_from_server = use_server_relevance;
  return use_server_relevance ?
      default_results_.verbatim_relevance : CalculateRelevanceForVerbatim();
}

int SearchProvider::CalculateRelevanceForVerbatim() const {
  if (!providers_.keyword_provider().empty())
    return 250;
  return CalculateRelevanceForVerbatimIgnoringKeywordModeState();
}

int SearchProvider::
    CalculateRelevanceForVerbatimIgnoringKeywordModeState() const {
  switch (input_.type()) {
    case metrics::OmniboxInputType::UNKNOWN:
    case metrics::OmniboxInputType::QUERY:
    case metrics::OmniboxInputType::FORCED_QUERY:
      return kNonURLVerbatimRelevance;

    case metrics::OmniboxInputType::URL:
      return 850;

    default:
      NOTREACHED();
      return 0;
  }
}

int SearchProvider::GetKeywordVerbatimRelevance(
    bool* relevance_from_server) const {
  // Use the suggested verbatim relevance score if it is non-negative (valid),
  // if inline autocomplete isn't prevented (always show verbatim on backspace),
  // and if it won't suppress verbatim, leaving no keyword provider matches.
  // Otherwise, if the keyword provider returned no matches and was still able
  // to suppress verbatim, the user would have no search/nav matches and may be
  // left unable to search using their keyword provider from the omnibox.
  // Check for results on each verbatim calculation, as results from older
  // queries (on previous input) may be trimmed for failing to inline new input.
  bool use_server_relevance =
      (keyword_results_.verbatim_relevance >= 0) &&
      !input_.prevent_inline_autocomplete() &&
      ((keyword_results_.verbatim_relevance > 0) ||
       !keyword_results_.suggest_results.empty() ||
       !keyword_results_.navigation_results.empty());
  if (relevance_from_server)
    *relevance_from_server = use_server_relevance;
  return use_server_relevance ?
      keyword_results_.verbatim_relevance :
      CalculateRelevanceForKeywordVerbatim(keyword_input_.type(),
                                           keyword_input_.prefer_keyword());
}

int SearchProvider::CalculateRelevanceForHistory(
    const base::Time& time,
    bool is_keyword,
    bool use_aggressive_method,
    bool prevent_search_history_inlining) const {
  // The relevance of past searches falls off over time. There are two distinct
  // equations used. If the first equation is used (searches to the primary
  // provider that we want to score aggressively), the score is in the range
  // 1300-1599 (unless |prevent_search_history_inlining|, in which case
  // it's in the range 1200-1299). If the second equation is used the
  // relevance of a search 15 minutes ago is discounted 50 points, while the
  // relevance of a search two weeks ago is discounted 450 points.
  double elapsed_time = std::max((base::Time::Now() - time).InSecondsF(), 0.0);
  bool is_primary_provider = is_keyword || !providers_.has_keyword_provider();
  if (is_primary_provider && use_aggressive_method) {
    // Searches with the past two days get a different curve.
    const double autocomplete_time = 2 * 24 * 60 * 60;
    if (elapsed_time < autocomplete_time) {
      int max_score = is_keyword ? 1599 : 1399;
      if (prevent_search_history_inlining)
        max_score = 1299;
      return max_score - static_cast<int>(99 *
          std::pow(elapsed_time / autocomplete_time, 2.5));
    }
    elapsed_time -= autocomplete_time;
  }

  const int score_discount =
      static_cast<int>(6.5 * std::pow(elapsed_time, 0.3));

  // Don't let scores go below 0.  Negative relevance scores are meaningful in
  // a different way.
  int base_score;
  if (is_primary_provider)
    base_score = (input_.type() == metrics::OmniboxInputType::URL) ? 750 : 1050;
  else
    base_score = 200;
  return std::max(0, base_score - score_discount);
}

AutocompleteMatch SearchProvider::NavigationToMatch(
    const SearchSuggestionParser::NavigationResult& navigation) {
  base::string16 input;
  const bool trimmed_whitespace = base::TrimWhitespace(
      navigation.from_keyword_provider() ?
          keyword_input_.text() : input_.text(),
      base::TRIM_TRAILING, &input) != base::TRIM_NONE;
  AutocompleteMatch match(this, navigation.relevance(), false,
                          navigation.type());
  match.destination_url = navigation.url();
  BaseSearchProvider::SetDeletionURL(navigation.deletion_url(), &match);
  // First look for the user's input inside the formatted url as it would be
  // without trimming the scheme, so we can find matches at the beginning of the
  // scheme.
  const URLPrefix* prefix =
      URLPrefix::BestURLPrefix(navigation.formatted_url(), input);
  size_t match_start = (prefix == NULL) ?
      navigation.formatted_url().find(input) : prefix->prefix.length();
  bool trim_http = !AutocompleteInput::HasHTTPScheme(input) &&
      (!prefix || (match_start != 0));
  const net::FormatUrlTypes format_types =
      net::kFormatUrlOmitAll & ~(trim_http ? 0 : net::kFormatUrlOmitHTTP);

  const std::string languages(client_->AcceptLanguages());
  size_t inline_autocomplete_offset = (prefix == NULL) ?
      base::string16::npos : (match_start + input.length());
  match.fill_into_edit +=
      AutocompleteInput::FormattedStringWithEquivalentMeaning(
          navigation.url(),
          net::FormatUrl(navigation.url(), languages, format_types,
                         net::UnescapeRule::SPACES, NULL, NULL,
                         &inline_autocomplete_offset),
          client_->SchemeClassifier());
  // Preserve the forced query '?' prefix in |match.fill_into_edit|.
  // Otherwise, user edits to a suggestion would show non-Search results.
  if (input_.type() == metrics::OmniboxInputType::FORCED_QUERY) {
    match.fill_into_edit.insert(0, base::ASCIIToUTF16("?"));
    if (inline_autocomplete_offset != base::string16::npos)
      ++inline_autocomplete_offset;
  }
  if (inline_autocomplete_offset != base::string16::npos) {
    DCHECK(inline_autocomplete_offset <= match.fill_into_edit.length());
    match.inline_autocompletion =
        match.fill_into_edit.substr(inline_autocomplete_offset);
  }
  // An inlineable navsuggestion can only be the default match when there
  // is no keyword provider active, lest it appear first and break the user
  // out of keyword mode.  We also must have received the navsuggestion before
  // the last keystroke, to prevent asynchronous inline autocompletions changes.
  // The navsuggestion can also only be default if either the inline
  // autocompletion is empty or we're not preventing inline autocompletion.
  // Finally, if we have an inlineable navsuggestion with an inline completion
  // that we're not preventing, make sure we didn't trim any whitespace.
  // We don't want to claim http://foo.com/bar is inlineable against the
  // input "foo.com/b ".
  match.allowed_to_be_default_match =
      (prefix != NULL) &&
      (providers_.GetKeywordProviderURL() == NULL) &&
      !navigation.received_after_last_keystroke() &&
      (match.inline_autocompletion.empty() ||
      (!input_.prevent_inline_autocomplete() && !trimmed_whitespace));
  match.EnsureUWYTIsAllowedToBeDefault(
      input_.canonicalized_url(), providers_.template_url_service());

  match.contents = navigation.match_contents();
  match.contents_class = navigation.match_contents_class();
  match.description = navigation.description();
  AutocompleteMatch::ClassifyMatchInString(input, match.description,
      ACMatchClassification::NONE, &match.description_class);

  match.RecordAdditionalInfo(
      kRelevanceFromServerKey,
      navigation.relevance_from_server() ? kTrue : kFalse);
  match.RecordAdditionalInfo(kShouldPrefetchKey, kFalse);

  return match;
}

void SearchProvider::UpdateDone() {
  // We're done when the timer isn't running, there are no suggest queries
  // pending, and we're not waiting on Instant.
  done_ = !timer_.IsRunning() && (suggest_results_pending_ == 0);
}

std::string SearchProvider::GetSessionToken() {
  base::TimeTicks current_time(base::TimeTicks::Now());
  // Renew token if it expired.
  if (current_time > token_expiration_time_) {
    const size_t kTokenBytes = 12;
    std::string raw_data;
    base::RandBytes(WriteInto(&raw_data, kTokenBytes + 1), kTokenBytes);
    base::Base64Encode(raw_data, &current_token_);

    // Make the base64 encoded value URL and filename safe(see RFC 3548).
    std::replace(current_token_.begin(), current_token_.end(), '+', '-');
    std::replace(current_token_.begin(), current_token_.end(), '/', '_');
  }

  // Extend expiration time another 60 seconds.
  token_expiration_time_ = current_time + base::TimeDelta::FromSeconds(60);

  return current_token_;
}

void SearchProvider::RegisterDisplayedAnswers(
    const AutocompleteResult& result) {
  if (result.empty())
    return;

  // The answer must be in the first or second slot to be considered. It should
  // only be in the second slot if AutocompleteController ranked a local search
  // history or a verbatim item higher than the answer.
  AutocompleteResult::const_iterator match = result.begin();
  if (match->answer_contents.empty() && result.size() > 1)
    ++match;
  if (match->answer_contents.empty() || match->answer_type.empty() ||
      match->fill_into_edit.empty())
    return;

  // Valid answer encountered, cache it for further queries.
  answers_cache_.UpdateRecentAnswers(match->fill_into_edit, match->answer_type);
}

void SearchProvider::DoAnswersQuery(const AutocompleteInput& input) {
  prefetch_data_ = answers_cache_.GetTopAnswerEntry(input.text());
}
