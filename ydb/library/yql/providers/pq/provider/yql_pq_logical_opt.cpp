#include "yql_pq_provider_impl.h"

#include <ydb/library/yql/core/expr_nodes/yql_expr_nodes.h>
#include <ydb/library/yql/core/yql_type_helpers.h>
#include <ydb/library/yql/providers/common/provider/yql_data_provider_impl.h>
#include <ydb/library/yql/providers/common/provider/yql_provider_names.h>
#include <ydb/library/yql/providers/common/provider/yql_provider.h>
#include <ydb/library/yql/providers/common/transform/yql_optimize.h>
#include <ydb/library/yql/providers/dq/expr_nodes/dqs_expr_nodes.h>
#include <ydb/library/yql/providers/pq/common/pq_meta_fields.h>
#include <ydb/library/yql/providers/pq/expr_nodes/yql_pq_expr_nodes.h>
#include <ydb/library/yql/utils/log/log.h>


namespace NYql {

using namespace NNodes;

namespace {

std::unordered_set<TString> GetUsedMetadataFields(const TCoExtractMembers& extract) {
    std::unordered_set<TString> usedMetadataFields;
    for (const auto extractMember : extract.Members()) {
        if (FindPqMetaFieldDescriptorBySysColumn(extractMember.StringValue())) {
            usedMetadataFields.emplace(extractMember.StringValue());
        }
    }

    return usedMetadataFields;
}

TVector<TCoNameValueTuple> DropUnusedMetadata(const TPqTopic& pqTopic, const std::unordered_set<TString>& usedMetadataFields) {
    TVector<TCoNameValueTuple> newSourceMetadata;
    for (auto metadataItem : pqTopic.Metadata()) {
        auto metadataName = metadataItem.Cast<TCoNameValueTuple>().Value().Maybe<TCoAtom>().Cast().StringValue();
        if (usedMetadataFields.contains(metadataName)) {
            newSourceMetadata.push_back(metadataItem);
        }
    }

    return newSourceMetadata;
}

TCoNameValueTupleList DropUnusedMetadataFromDqWrapSettings(
    const TDqSourceWrap& dqSourceWrap,
    const TVector<TCoNameValueTuple>& newSourceMetadata,
    TExprContext& ctx)
{
    TVector<TCoNameValueTuple> newSettings;
    for (const auto settingItem : dqSourceWrap.Settings().Maybe<TCoNameValueTupleList>().Cast()) {
        if (settingItem.Name() == "metadataColumns") {
            std::vector<TExprNode::TPtr> newMetadataColumns;
            newMetadataColumns.reserve(newSourceMetadata.size());

            for (auto metadataName : newSourceMetadata) {
                newMetadataColumns.push_back(ctx.NewAtom(
                    dqSourceWrap.Pos(),
                    metadataName.Value().Maybe<TCoAtom>().Cast().StringValue()));
            }

            if (!newMetadataColumns.empty()) {
                newSettings.push_back(Build<TCoNameValueTuple>(ctx, dqSourceWrap.Pos())
                    .Name().Build("metadataColumns")
                    .Value(ctx.NewList(dqSourceWrap.Pos(), std::move(newMetadataColumns)))
                    .Done());
            }

            continue;
        }

        newSettings.push_back(settingItem);
    }

    return Build<TCoNameValueTupleList>(ctx, dqSourceWrap.Pos())
        .Add(std::move(newSettings))
        .Done();
}

TExprNode::TPtr DropUnusedMetadataFieldsFromRowType(
    TPositionHandle position,
    const TStructExprType* oldRowType,
    const std::unordered_set<TString>& usedMetadataFields,
    TExprContext& ctx)
{
    TVector<const TItemExprType*> newFields;
    newFields.reserve(oldRowType->GetSize());

    for (auto itemExprType : oldRowType->GetItems()) {
        const auto columnName = TString(itemExprType->GetName());
        if (FindPqMetaFieldDescriptorBySysColumn(columnName) && !usedMetadataFields.contains(columnName)) {
            continue;
        }

        newFields.push_back(itemExprType);
    }

    return ExpandType(position, *ctx.MakeType<TStructExprType>(newFields), ctx);
}

TExprNode::TPtr DropUnusedMetadataFieldsFromColumns(
    TExprBase oldColumns,
    const std::unordered_set<TString>& usedMetadataFields,
    TExprContext& ctx)
{
    TExprNode::TListType res;
    for (const auto& column : oldColumns.Cast<TCoAtomList>()) {
        if (FindPqMetaFieldDescriptorBySysColumn(column.StringValue()) && !usedMetadataFields.contains(column.StringValue())) {
            continue;
        }

        res.push_back(column.Ptr());
    }

    return ctx.NewList(oldColumns.Pos(), std::move(res));
}

class TPqLogicalOptProposalTransformer : public TOptimizeTransformerBase {
public:
    TPqLogicalOptProposalTransformer(TPqState::TPtr state)
        : TOptimizeTransformerBase(state->Types, NLog::EComponent::ProviderPq, {})
        , State_(state)
    {
#define HNDL(name) "LogicalOptimizer-"#name, Hndl(&TPqLogicalOptProposalTransformer::name)
        AddHandler(0, &TCoLeft::Match, HNDL(TrimReadWorld));
      //  AddHandler(0, &TCoExtractMembers::Match, HNDL(ExtractMembers));
        AddHandler(0, &TCoExtractMembers::Match, HNDL(ExtractMembersOverDqWrap));
        #undef HNDL
    }

    TMaybeNode<TExprBase> TrimReadWorld(TExprBase node, TExprContext& ctx) const {
        const auto& maybeRead = node.Cast<TCoLeft>().Input().Maybe<TPqReadTopic>();
        if (!maybeRead) {
            return node;
        }

        return TExprBase(ctx.NewWorld(node.Pos()));
    }

    /*
    TMaybeNode<TExprBase> ExtractMembers(TExprBase node, TExprContext& ctx) const {
        const auto& extract = node.Cast<TCoExtractMembers>();
        const auto& input = extract.Input();
        const auto& read = input.Maybe<TCoRight>().Input().Maybe<TPqReadTopic>();
        if (!read) {
            return node;
        }

        const auto& cast = read.Cast();
        return Build<TCoRight>(ctx, extract.Pos())
            .Input<TPqReadTopic>()
            .World(cast.World())
            .DataSource(cast.DataSource())
            .Topic(cast.Topic())
            .Columns(extract.Members())
            .Build()
            .Done();
    }*/

    TMaybeNode<TExprBase> ExtractMembersOverDqWrap(TExprBase node, TExprContext& ctx) const {
        const auto& extract = node.Cast<TCoExtractMembers>();
        const auto& input = extract.Input();
        const auto dqSourceWrap = input.Maybe<TDqSourceWrap>();
        const auto dqPqTopicSource = dqSourceWrap.Input().Maybe<TDqPqTopicSource>();
        const auto pqTopic = dqPqTopicSource.Topic().Maybe<TPqTopic>();
        if (!pqTopic) {
            return node;
        }

        const auto usedMetadataFields = GetUsedMetadataFields(extract);
        const auto newSourceMetadata = DropUnusedMetadata(pqTopic.Cast(), usedMetadataFields);
        if (newSourceMetadata.size() == pqTopic.Metadata().Cast().Size()) {
            return node;
        }

        const auto oldRowType = pqTopic.Ref().GetTypeAnn()
            ->Cast<TListExprType>()->GetItemType()->Cast<TStructExprType>();

        auto newPqTopicSource = Build<TDqPqTopicSource>(ctx, node.Pos())
            .InitFrom(dqPqTopicSource.Cast())
            .Topic<TPqTopic>()
                .InitFrom(pqTopic.Cast())
                .Metadata().Add(newSourceMetadata).Build()
                .Build();

        if (dqPqTopicSource.Columns()) {
            auto newColumns = DropUnusedMetadataFieldsFromColumns(
                dqPqTopicSource.Columns().Cast(),
                usedMetadataFields,
                ctx);
            newPqTopicSource.Columns(newColumns);
        }

        const auto newDqSourceWrap = Build<TDqSourceWrap>(ctx, node.Pos())
            .InitFrom(dqSourceWrap.Cast())
            .Input(newPqTopicSource.Done())
            .Settings(DropUnusedMetadataFromDqWrapSettings(
                dqSourceWrap.Cast(),
                newSourceMetadata,
                ctx))
            .RowType(DropUnusedMetadataFieldsFromRowType(
                node.Pos(),
                oldRowType,
                usedMetadataFields,
                ctx))
            .Done()
            .Ptr();

        return Build<TCoExtractMembers>(ctx, node.Pos())
            .InitFrom(extract)
            .Input(ctx.ReplaceNode(input.Ptr(), dqSourceWrap.Ref(), newDqSourceWrap))
            .Done();
    }

private:
    TPqState::TPtr State_;
};

}

THolder<IGraphTransformer> CreatePqLogicalOptProposalTransformer(TPqState::TPtr state) {
    return MakeHolder<TPqLogicalOptProposalTransformer>(state);
}

} // namespace NYql
