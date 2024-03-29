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
#include "ml_metadata/query/filter_query_builder.h"

#include <glog/logging.h>
#include "zetasql/public/strings.h"
#include "zetasql/resolved_ast/sql_builder.h"
#include "absl/status/status.h"
#include "absl/strings/substitute.h"
#include "ml_metadata/metadata_store/constants.h"
#include "ml_metadata/proto/metadata_store.pb.h"

namespace ml_metadata {
namespace {

// The prefix for table alias in SQL clauses
constexpr char kTableAliasPrefix[] = "table_";

// Default static references keys used in the JoinTableAlias
constexpr char kBaseTableRef[] = "";
constexpr char kTypeTableRef[] = "type";

// A list of template queries of joins to compose FROM clause.
// $0 is the base node table, $1 is the type related neighborhood table.
// $2 is the type_kind enum value.
constexpr absl::string_view kTypeJoinTable = R"sql(
JOIN (
  SELECT Type.id as type_id, Type.name as type
  FROM Type
  WHERE Type.type_kind = $2
) AS $1 ON $0.type_id = $1.type_id )sql";

// $0 is the base node table, $1 is the context related neighborhood table.
constexpr absl::string_view kContextJoinTableViaAttribution = R"sql(
JOIN (
  SELECT Context.id, Context.name,
         Type.name as type,
         Attribution.artifact_id,
         Context.create_time_since_epoch,
         Context.last_update_time_since_epoch
  FROM Context
       JOIN Type ON Context.type_id = Type.id
       JOIN Attribution ON Context.id = Attribution.context_id
) AS $1 ON $0.id = $1.artifact_id )sql";

// $0 is the base node table, $1 is the context related neighborhood table.
constexpr absl::string_view kContextJoinTableViaAssociation = R"sql(
JOIN (
  SELECT Context.id, Context.name,
         Type.name as type,
         Association.execution_id,
         Context.create_time_since_epoch,
         Context.last_update_time_since_epoch

  FROM Context
       JOIN Type ON Context.type_id = Type.id
       JOIN Association ON Context.id = Association.context_id
) AS $1 ON $0.id = $1.execution_id )sql";

// $0 is the base context table, $1 is the context related through ParentContext
// table.
constexpr absl::string_view kParentContextJoinTableViaParentContext = R"sql(
JOIN (
  SELECT Context.name,
         Type.name as type,
         ParentContext.context_id as child_context_id
  FROM Context
       JOIN Type ON Context.type_id = Type.id
       JOIN ParentContext ON Context.id = ParentContext.parent_context_id
) AS $1 ON $0.id = $1.child_context_id )sql";

// $0 is the base context table, $1 is the context related through ParentContext
// table.
constexpr absl::string_view kChildContextJoinTableViaParentContext = R"sql(
JOIN (
  SELECT Context.name,
         Type.name as type,
         ParentContext.parent_context_id as parent_context_id
  FROM Context
       JOIN Type ON Context.type_id = Type.id
       JOIN ParentContext ON Context.id = ParentContext.context_id
) AS $1 ON $0.id = $1.parent_context_id )sql";

// $0 is the base node table. $1 is the property related neighborhood table.
// $2 is property name, $3 is a boolean for is_custom_property.
constexpr absl::string_view kArtifactPropertyJoinTable = R"sql(
JOIN (
  SELECT artifact_id, int_value, double_value, string_value
  FROM ArtifactProperty WHERE name = "$2" AND is_custom_property = $3
) AS $1 ON $0.id = $1.artifact_id )sql";

constexpr absl::string_view kExecutionPropertyJoinTable = R"sql(
JOIN (
  SELECT execution_id, int_value, double_value, string_value
  FROM ExecutionProperty WHERE name = "$2" AND is_custom_property = $3
) AS $1 ON $0.id = $1.execution_id )sql";

constexpr absl::string_view kContextPropertyJoinTable = R"sql(
JOIN (
  SELECT context_id, int_value, double_value, string_value
  FROM ContextProperty WHERE name = "$2" AND is_custom_property = $3
) AS $1 ON $0.id = $1.context_id )sql";

constexpr absl::string_view kArtifactEventJoinTable = R"sql(
JOIN Event AS $1 ON $0.id = $1.artifact_id )sql";

constexpr absl::string_view kExecutionEventJoinTable = R"sql(
JOIN Event AS $1 ON $0.id = $1.execution_id )sql";

// Returns the persisted type kind value given a node template.
template <typename T>
int GetTypeKindValue() {
  if constexpr (std::is_same<T, Artifact>::value) {
    return static_cast<int>(TypeKind::ARTIFACT_TYPE);
  } else if constexpr (std::is_same<T, Execution>::value) {
    return static_cast<int>(TypeKind::EXECUTION_TYPE);
  } else if constexpr (std::is_same<T, Context>::value) {
    return static_cast<int>(TypeKind::CONTEXT_TYPE);
  }
}

// Returns the context join clause depending on the node types.
template <typename T>
absl::string_view GetContextJoinTemplate() {
  if constexpr (std::is_same<T, Artifact>::value) {
    return kContextJoinTableViaAttribution;
  } else if constexpr (std::is_same<T, Execution>::value) {
    return kContextJoinTableViaAssociation;
  } else if constexpr (std::is_same<T, Context>::value) {
    LOG(ERROR) << "Context Join does not apply to T = Context.";
    return "";
  }
}

template <typename T>
absl::string_view GetEventJoinTemplate() {
  if constexpr (std::is_same<T, Artifact>::value) {
    return kArtifactEventJoinTable;
  } else if constexpr (std::is_same<T, Execution>::value) {
    return kExecutionEventJoinTable;
  } else if constexpr (std::is_same<T, Context>::value) {
    LOG(ERROR) << "Event Join does not apply to T = Context.";
    return "";
  }
}

// Returns the property join clause depending on the node types.
template <typename T>
std::string GetPropertyJoinTableImpl(absl::string_view base_alias,
                                     absl::string_view property_alias,
                                     absl::string_view property_name,
                                     bool is_custom_property) {
  if constexpr (std::is_same<T, Artifact>::value) {
    return absl::Substitute(kArtifactPropertyJoinTable, base_alias,
                            property_alias, property_name, is_custom_property);
  } else if constexpr (std::is_same<T, Execution>::value) {
    return absl::Substitute(kExecutionPropertyJoinTable, base_alias,
                            property_alias, property_name, is_custom_property);
  } else if constexpr (std::is_same<T, Context>::value) {
    return absl::Substitute(kContextPropertyJoinTable, base_alias,
                            property_alias, property_name, is_custom_property);
  }
}
}  // namespace

template <typename T>
std::string FilterQueryBuilder<T>::GetBaseNodeTable(
    absl::string_view base_alias) {
  if constexpr (std::is_same<T, Artifact>::value) {
    return absl::StrCat("Artifact AS ", base_alias, " ");
  } else if constexpr (std::is_same<T, Execution>::value) {
    return absl::StrCat("Execution AS ", base_alias, " ");
  } else if constexpr (std::is_same<T, Context>::value) {
    return absl::StrCat("Context AS ", base_alias, " ");
  }
}

template <typename T>
std::string FilterQueryBuilder<T>::GetTypeJoinTable(
    absl::string_view base_alias, absl::string_view type_alias) {
  return absl::Substitute(kTypeJoinTable, base_alias, type_alias,
                          GetTypeKindValue<T>());
}

template <typename T>
std::string FilterQueryBuilder<T>::GetContextJoinTable(
    absl::string_view base_alias, absl::string_view context_alias) {
  return absl::Substitute(GetContextJoinTemplate<T>(), base_alias,
                          context_alias);
}

template <typename T>
std::string FilterQueryBuilder<T>::GetParentContextJoinTable(
    absl::string_view base_alias, absl::string_view parent_context_alias) {
  return absl::Substitute(kParentContextJoinTableViaParentContext, base_alias,
                          parent_context_alias);
}

template <typename T>
std::string FilterQueryBuilder<T>::GetChildContextJoinTable(
    absl::string_view base_alias, absl::string_view child_context_alias) {
  return absl::Substitute(kChildContextJoinTableViaParentContext, base_alias,
                          child_context_alias);
}

template <typename T>
std::string FilterQueryBuilder<T>::GetPropertyJoinTable(
    absl::string_view base_alias, absl::string_view property_alias,
    absl::string_view property_name) {
  return GetPropertyJoinTableImpl<T>(base_alias, property_alias, property_name,
                                     /*is_custom_property=*/false);
}

template <typename T>
std::string FilterQueryBuilder<T>::GetCustomPropertyJoinTable(
    absl::string_view base_alias, absl::string_view property_alias,
    absl::string_view property_name) {
  return GetPropertyJoinTableImpl<T>(base_alias, property_alias, property_name,
                                     /*is_custom_property=*/true);
}

template <typename T>
std::string FilterQueryBuilder<T>::GetEventJoinTable(
    absl::string_view base_alias, absl::string_view event_alias) {
  return absl::Substitute(GetEventJoinTemplate<T>(), base_alias, event_alias);
}

template <typename T>
FilterQueryBuilder<T>::FilterQueryBuilder() {
  mentioned_alias_[AtomType::ATTRIBUTE].insert(
      {kBaseTableRef, std::string(kBaseTableAlias)});
}

template <typename T>
std::string FilterQueryBuilder<T>::GetWhereClause() {
  return sql();
}

template <typename T>
std::string FilterQueryBuilder<T>::GetFromClause() {
  const std::string& base_alias =
      mentioned_alias_[AtomType::ATTRIBUTE][kBaseTableRef];
  std::string result = GetBaseNodeTable(base_alias);
  if (mentioned_alias_[AtomType::ATTRIBUTE].contains(kTypeTableRef)) {
    const std::string& type_alias =
        mentioned_alias_[AtomType::ATTRIBUTE][kTypeTableRef];
    absl::StrAppend(&result, GetTypeJoinTable(base_alias, type_alias));
  }
  for (const auto& mentioned_context : mentioned_alias_[AtomType::CONTEXT]) {
    const std::string& context_alias = mentioned_context.second;
    absl::StrAppend(&result, GetContextJoinTable(base_alias, context_alias));
  }
  for (const auto& mentioned_property : mentioned_alias_[AtomType::PROPERTY]) {
    const std::string& property_alias = mentioned_property.second;
    // property's name starts after prefix 'properties_'
    static constexpr absl::string_view kPropertyPrefix = "properties_";
    const std::string property_name =
        mentioned_property.first.substr(kPropertyPrefix.length());
    absl::StrAppend(&result, GetPropertyJoinTable(base_alias, property_alias,
                                                  property_name));
  }
  for (const auto& mentioned_property :
       mentioned_alias_[AtomType::CUSTOM_PROPERTY]) {
    const std::string& property_alias = mentioned_property.second;
    // property's name starts after prefix 'custom_properties_'
    static constexpr absl::string_view kPropertyPrefix = "custom_properties_";
    const std::string property_name =
        mentioned_property.first.substr(kPropertyPrefix.length());
    absl::StrAppend(&result, GetCustomPropertyJoinTable(
                                 base_alias, property_alias, property_name));
  }
  for (const auto& parent_contexts :
       mentioned_alias_[AtomType::PARENT_CONTEXT]) {
    const std::string& parent_context_alias = parent_contexts.second;
    absl::StrAppend(
        &result, GetParentContextJoinTable(base_alias, parent_context_alias));
  }
  for (const auto& child_contexts : mentioned_alias_[AtomType::CHILD_CONTEXT]) {
    const std::string& child_context_alias = child_contexts.second;
    absl::StrAppend(&result,
                    GetChildContextJoinTable(base_alias, child_context_alias));
  }
  for (const auto& event : mentioned_alias_[AtomType::EVENT]) {
    const std::string& event_alias = event.second;
    absl::StrAppend(&result, GetEventJoinTable(base_alias, event_alias));
  }
  return result;
}

template <typename T>
std::string FilterQueryBuilder<T>::GetTableAlias(
    AtomType atom_type, absl::string_view concept_name) {
  if (!mentioned_alias_[atom_type].contains(concept_name)) {
    mentioned_alias_[atom_type][concept_name] =
        absl::StrCat(kTableAliasPrefix, ++alias_index_);
  }
  return zetasql::ToIdentifierLiteral(
      mentioned_alias_[atom_type][concept_name]);
}

template <typename T>
absl::Status FilterQueryBuilder<T>::VisitResolvedExpressionColumn(
    const zetasql::ResolvedExpressionColumn* node) {
  // If it is a struct type, then it is constructed by the AST resolver, when
  // mentioning concepts around the node. For each of the struct mentioning,
  // we need to generate the alias to join with other tables, and rewrite the
  // the mentioning with those FROM clause alias.
  if (node->type()->IsStruct()) {
    const std::string& neighbor_name = node->name();
    if (absl::StartsWith(neighbor_name, "contexts_")) {
      // example output: table_i.name, table_i.type
      PushQueryFragment(node, GetTableAlias(AtomType::CONTEXT, neighbor_name));
    } else if (absl::StartsWith(neighbor_name, "properties_")) {
      PushQueryFragment(node, GetTableAlias(AtomType::PROPERTY, neighbor_name));
    } else if (absl::StartsWith(neighbor_name, "custom_properties_")) {
      PushQueryFragment(
          node, GetTableAlias(AtomType::CUSTOM_PROPERTY, neighbor_name));
    } else if (absl::StartsWith(neighbor_name, "parent_contexts_")) {
      PushQueryFragment(node,
                        GetTableAlias(AtomType::PARENT_CONTEXT, neighbor_name));
    } else if (absl::StartsWith(neighbor_name, "child_contexts_")) {
      PushQueryFragment(node,
                        GetTableAlias(AtomType::CHILD_CONTEXT, neighbor_name));
    } else if (absl::StartsWith(neighbor_name, "events_")) {
      PushQueryFragment(node, GetTableAlias(AtomType::EVENT, neighbor_name));
    } else {
      // TODO(b/145945460) Context neighbor artifacts/executions.
      return absl::UnimplementedError(
          "context-executions and context-artifacts are not supported yet in"
          " filtering predicate.");
    }
  } else {
    // For attributes, except the `type` which requires join, in other cases
    // we simply prefix it with base table.
    if (node->name() != "type") {
      // example output: table_0.id, table_0.uri
      PushQueryFragment(
          node,
          absl::StrCat(GetTableAlias(AtomType::ATTRIBUTE, kBaseTableRef), ".",
                       zetasql::ToIdentifierLiteral(node->name())));
    } else {
      // example output: table_j.type
      PushQueryFragment(
          node,
          absl::StrCat(GetTableAlias(AtomType::ATTRIBUTE, kTypeTableRef), ".",
                       zetasql::ToIdentifierLiteral(kTypeTableRef)));
    }
  }
  return absl::OkStatus();
}

// Explicit template instantiation for supported node types.
template class FilterQueryBuilder<Artifact>;
template class FilterQueryBuilder<Execution>;
template class FilterQueryBuilder<Context>;

}  // namespace ml_metadata
