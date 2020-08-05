/* Copyright 2020 Google LLC

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    https://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/
#include "ml_metadata/tools/mlmd_bench/util.h"

#include <vector>

#include "absl/time/clock.h"
#include "absl/types/variant.h"
#include "ml_metadata/metadata_store/metadata_store.h"
#include "ml_metadata/proto/metadata_store.pb.h"
#include "ml_metadata/proto/metadata_store_service.pb.h"
#include "ml_metadata/tools/mlmd_bench/proto/mlmd_bench.pb.h"
#include "tensorflow/core/lib/core/errors.h"
#include "tensorflow/core/lib/core/status.h"

namespace ml_metadata {
namespace {

// Enumeration type that indicates which node type to be fetched using
// GetExistingTypesImpl().
enum FetchType { FetchArtifactType, FetchExecutionType, FetchContextType };

// Prepares node's properties for inserting inside InsertNodesInDb().
template <typename T, typename NT>
void PrepareNode(const std::string& node_name, const T& node_type, NT& node) {
  node.set_type_id(node_type.id());
  node.set_name(node_name);
  (*node.mutable_properties())["property"].set_string_value("foo");
  (*node.mutable_custom_properties())["custom-property"].set_string_value(
      "bar");
}

// Detailed implementation of multiple GetExistingTypes() overload function.
// Returns detailed error if query executions failed.
tensorflow::Status GetExistingTypesImpl(const FetchType& fetch_type,
                                        MetadataStore& store,
                                        std::vector<Type>& existing_types) {
  switch (fetch_type) {
    case FetchArtifactType: {
      GetArtifactTypesResponse get_response;
      TF_RETURN_IF_ERROR(store.GetArtifactTypes(
          /*request=*/{}, &get_response));
      for (auto& artifact_type : get_response.artifact_types()) {
        existing_types.push_back(artifact_type);
      }
      break;
    }
    case FetchExecutionType: {
      GetExecutionTypesResponse get_response;
      TF_RETURN_IF_ERROR(store.GetExecutionTypes(
          /*request=*/{}, &get_response));
      for (auto& execution_type : get_response.execution_types()) {
        existing_types.push_back(execution_type);
      }
      break;
    }
    case FetchContextType: {
      GetContextTypesResponse get_response;
      TF_RETURN_IF_ERROR(store.GetContextTypes(
          /*request=*/{}, &get_response));
      for (auto& context_type : get_response.context_types()) {
        existing_types.push_back(context_type);
      }
      break;
    }
    default:
      LOG(FATAL) << "Wrong specification for getting types in db!";
  }
  return tensorflow::Status::OK();
}

}  // namespace

tensorflow::Status GetExistingTypes(const FillTypesConfig& fill_types_config,
                                    MetadataStore& store,
                                    std::vector<Type>& existing_types) {
  switch (fill_types_config.specification()) {
    case FillTypesConfig::ARTIFACT_TYPE: {
      return GetExistingTypesImpl(FetchArtifactType, store, existing_types);
    }
    case FillTypesConfig::EXECUTION_TYPE: {
      return GetExistingTypesImpl(FetchExecutionType, store, existing_types);
    }
    case FillTypesConfig::CONTEXT_TYPE: {
      return GetExistingTypesImpl(FetchContextType, store, existing_types);
    }
    default:
      return tensorflow::errors::Unimplemented(
          "Unknown FillTypesConfig specification.");
  }
}

tensorflow::Status GetExistingTypes(const FillNodesConfig& fill_nodes_config,
                                    MetadataStore& store,
                                    std::vector<Type>& existing_types) {
  switch (fill_nodes_config.specification()) {
    case FillNodesConfig::ARTIFACT: {
      return GetExistingTypesImpl(FetchArtifactType, store, existing_types);
    }
    case FillNodesConfig::EXECUTION: {
      return GetExistingTypesImpl(FetchExecutionType, store, existing_types);
    }
    case FillNodesConfig::CONTEXT: {
      return GetExistingTypesImpl(FetchContextType, store, existing_types);
    }
    default:
      return tensorflow::errors::Unimplemented(
          "Unknown FillNodesConfig specification.");
  }
}

tensorflow::Status GetExistingNodes(const FillNodesConfig& fill_nodes_config,
                                    MetadataStore& store,
                                    std::vector<Node>& existing_nodes) {
  switch (fill_nodes_config.specification()) {
    case FillNodesConfig::ARTIFACT: {
      GetArtifactsResponse get_response;
      TF_RETURN_IF_ERROR(store.GetArtifacts(
          /*request=*/{}, &get_response));
      for (auto& artifact : get_response.artifacts()) {
        existing_nodes.push_back(artifact);
      }
      break;
    }
    case FillNodesConfig::EXECUTION: {
      GetExecutionsResponse get_response;
      TF_RETURN_IF_ERROR(store.GetExecutions(
          /*request=*/{}, &get_response));
      for (auto& execution : get_response.executions()) {
        existing_nodes.push_back(execution);
      }
      break;
    }
    case FillNodesConfig::CONTEXT: {
      GetContextsResponse get_response;
      TF_RETURN_IF_ERROR(store.GetContexts(
          /*request=*/{}, &get_response));
      for (auto& context : get_response.contexts()) {
        existing_nodes.push_back(context);
      }
      break;
    }
    default:
      LOG(FATAL) << "Wrong specification for getting nodes in db!";
  }
  return tensorflow::Status::OK();
}

tensorflow::Status InsertTypesInDb(const int64 num_artifact_types,
                                   const int64 num_execution_types,
                                   const int64 num_context_types,
                                   MetadataStore& store) {
  PutTypesRequest put_request;
  PutTypesResponse put_response;

  const std::string curr_time = absl::FormatTime(absl::Now());
  for (int64 i = 0; i < num_artifact_types; i++) {
    ArtifactType* curr_type = put_request.add_artifact_types();
    curr_type->set_name(
        absl::StrCat("pre_insert_artifact_type-", curr_time, "-", i));
    (*curr_type->mutable_properties())["property"] = STRING;
  }

  for (int64 i = 0; i < num_execution_types; i++) {
    ExecutionType* curr_type = put_request.add_execution_types();
    curr_type->set_name(
        absl::StrCat("pre_insert_execution_type-", curr_time, "-", i));
    (*curr_type->mutable_properties())["property"] = STRING;
  }

  for (int64 i = 0; i < num_context_types; i++) {
    ContextType* curr_type = put_request.add_context_types();
    curr_type->set_name(
        absl::StrCat("pre_insert_context_type-", curr_time, "-", i));
    (*curr_type->mutable_properties())["property"] = STRING;
  }

  return store.PutTypes(put_request, &put_response);
}

tensorflow::Status InsertNodesInDb(const int64 num_artifact_nodes,
                                   const int64 num_execution_nodes,
                                   const int64 num_context_nodes,
                                   MetadataStore& store) {
  FillTypesConfig fill_types_config;
  fill_types_config.set_specification(FillTypesConfig::ARTIFACT_TYPE);
  std::vector<Type> existing_artifact_types;
  TF_RETURN_IF_ERROR(
      GetExistingTypes(fill_types_config, store, existing_artifact_types));
  fill_types_config.set_specification(FillTypesConfig::EXECUTION_TYPE);
  std::vector<Type> existing_execution_types;
  TF_RETURN_IF_ERROR(
      GetExistingTypes(fill_types_config, store, existing_execution_types));
  fill_types_config.set_specification(FillTypesConfig::CONTEXT_TYPE);
  std::vector<Type> existing_context_types;
  TF_RETURN_IF_ERROR(
      GetExistingTypes(fill_types_config, store, existing_context_types));

  PutArtifactsRequest put_artifacts_request;
  PutArtifactsResponse put_artifacts_response;
  const std::string curr_time = absl::FormatTime(absl::Now());
  for (int64 i = 0; i < num_artifact_nodes; i++) {
    const int64 type_index = i % existing_artifact_types.size();
    const string& node_name =
        absl::StrCat("pre_insert_artifact-", curr_time, "-", i);
    Artifact* curr_node = put_artifacts_request.add_artifacts();
    PrepareNode<ArtifactType, Artifact>(
        node_name, absl::get<ArtifactType>(existing_artifact_types[type_index]),
        *curr_node);
    curr_node->set_uri(absl::StrCat(node_name, "_uri"));
    curr_node->set_state(Artifact::UNKNOWN);
  }
  TF_RETURN_IF_ERROR(
      store.PutArtifacts(put_artifacts_request, &put_artifacts_response));

  PutExecutionsRequest put_executions_request;
  PutExecutionsResponse put_executions_response;
  for (int64 i = 0; i < num_execution_nodes; i++) {
    const int64 type_index = i % existing_execution_types.size();
    const string& node_name =
        absl::StrCat("pre_insert_execution-", curr_time, "-", i);
    Execution* curr_node = put_executions_request.add_executions();
    PrepareNode<ExecutionType, Execution>(
        node_name,
        absl::get<ExecutionType>(existing_execution_types[type_index]),
        *curr_node);
    curr_node->set_last_known_state(Execution::UNKNOWN);
  }
  TF_RETURN_IF_ERROR(
      store.PutExecutions(put_executions_request, &put_executions_response));

  PutContextsRequest put_contexts_request;
  PutContextsResponse put_contexts_response;
  for (int64 i = 0; i < num_context_nodes; i++) {
    const int64 type_index = i % existing_context_types.size();
    const string& node_name =
        absl::StrCat("pre_insert_context-", curr_time, "-", i);
    PrepareNode<ContextType, Context>(
        node_name, absl::get<ContextType>(existing_context_types[type_index]),
        *put_contexts_request.add_contexts());
  }
  TF_RETURN_IF_ERROR(
      store.PutContexts(put_contexts_request, &put_contexts_response));

  return tensorflow::Status::OK();
}

}  // namespace ml_metadata