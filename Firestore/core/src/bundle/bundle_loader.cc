/*
 * Copyright 2021 Google LLC
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "Firestore/core/src/bundle/bundle_loader.h"

#include "Firestore/core/include/firebase/firestore/firestore_errors.h"
#include "Firestore/core/src/api/bundle_types.h"
#include "Firestore/core/src/bundle/bundle_document.h"
#include "Firestore/core/src/model/document_key.h"
#include "Firestore/core/src/model/document_key_set.h"
#include "Firestore/core/src/model/model_fwd.h"
#include "Firestore/core/src/model/no_document.h"

namespace firebase {
namespace firestore {
namespace bundle {

using firestore::Error;
using firestore::LoadBundleTaskProgress;
using model::DocumentKeySet;
using model::MaybeDocumentMap;
using model::NoDocument;
using util::Status;
using util::StatusOr;

StatusOr<absl::optional<LoadBundleTaskProgress>> BundleLoader::AddElement(
    const BundleElement& element, uint64_t byte_size) {
  HARD_ASSERT(element.ElementType() != BundleElementType::Metadata,
              "Unexpected bundle metadata element.");

  auto before_count = documents_.size();

  if (element.ElementType() == BundleElementType::NamedQuery) {
    queries_.push_back(dynamic_cast<const NamedQuery&>(element));
  } else if (element.ElementType() == BundleElementType::DocumentMetadata) {
    const auto& document_metadata =
        dynamic_cast<const BundledDocumentMetadata&>(element);
    current_document_ = document_metadata.key();
    documents_metadata_.insert({document_metadata.key(), document_metadata});

    if (!document_metadata.exists()) {
      documents_ = documents_.insert(
          document_metadata.key(),
          NoDocument(document_metadata.key(), document_metadata.read_time(),
                     /*has_committed_mutations=*/false));
      current_document_ = absl::nullopt;
    }
  } else if (element.ElementType() == BundleElementType::Document) {
    const auto& document = dynamic_cast<const BundleDocument&>(element);
    if (!current_document_.has_value() ||
        document.key() != current_document_.value()) {
      return {util::Status::FromErrno(
          Error::kErrorInvalidArgument,
          "The document being added does not match the stored metadata.")};
    }

    documents_ = documents_.insert(document.key(), document.document());
    current_document_ = absl::nullopt;
  }

  bytes_loaded_ += byte_size;

  if (before_count == documents_.size()) {
    return {absl::nullopt};
  }

  return {absl::make_optional(LoadBundleTaskProgress(
      documents_.size(), metadata_.total_documents(), bytes_loaded_,
      metadata_.total_bytes(), TaskState::Running))};
}

StatusOr<MaybeDocumentMap> BundleLoader::ApplyChanges() {
  if (current_document_ != absl::nullopt) {
    return StatusOr<MaybeDocumentMap>(
        Status::FromErrno(Error::kErrorInvalidArgument,
                          "Bundled documents end with a document metadata "
                          "element instead of a document."));
  }
  if (metadata_.total_documents() != documents_.size()) {
    return StatusOr<MaybeDocumentMap>(Status::FromErrno(
        Error::kErrorInvalidArgument,
        "Loaded documents count is not the same as in metadata."));
  }

  auto changes =
      callback_->ApplyBundledDocuments(documents_, metadata_.bundle_id());
  auto query_document_map = GetQueryDocumentMapping();
  for (const auto& named_query : queries_) {
    auto matching_keys = query_document_map[named_query.query_name()];
    callback_->SaveNamedQuery(named_query, matching_keys);
  }

  callback_->SaveBundle(metadata_);

  return changes;
}

std::unordered_map<std::string, DocumentKeySet>
BundleLoader::GetQueryDocumentMapping() {
  std::unordered_map<std::string, DocumentKeySet> result;
  for (const auto& named_query : queries_) {
    result.insert({named_query.query_name(), DocumentKeySet{}});
  }
  for (const auto& doc_metadata : documents_metadata_) {
    const auto& metadata = doc_metadata.second;
    for (const auto& query : doc_metadata.second.queries()) {
      auto inserted = result[query].insert(metadata.key());
      result[query] = std::move(inserted);
    }
  }

  return result;
}

}  // namespace bundle
}  // namespace firestore
}  // namespace firebase
