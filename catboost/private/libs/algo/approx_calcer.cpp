#include "approx_calcer.h"

#include "approx_calcer_helpers.h"
#include "approx_calcer_multi.h"
#include "approx_calcer_querywise.h"
#include "fold.h"
#include "index_calcer.h"
#include "learn_context.h"
#include "monotonic_constraint_utils.h"
#include "scoring.h"
#include "split.h"
#include "yetirank_helpers.h"

#include <catboost/private/libs/algo_helpers/approx_calcer_helpers.h>
#include <catboost/private/libs/algo_helpers/error_functions.h>
#include <catboost/private/libs/algo_helpers/gradient_walker.h>
#include <catboost/private/libs/algo_helpers/pairwise_leaves_calculation.h>
#include <catboost/libs/data/data_provider.h>
#include <catboost/libs/helpers/parallel_tasks.h>
#include <catboost/libs/logging/logging.h>
#include <catboost/libs/logging/profile_info.h>
#include <catboost/libs/metrics/metric.h>
#include <catboost/private/libs/options/catboost_options.h>
#include <catboost/private/libs/options/enum_helpers.h>
#include <catboost/private/libs/functools/forward_as_const.h>

#include <library/threading/local_executor/local_executor.h>

#include <util/generic/algorithm.h>
#include <util/generic/ymath.h>

template <bool StoreExpApprox, int VectorWidth>
inline void UpdateApproxKernel(const double* leafDeltas, const TIndexType* indices, double* deltasDimension) {
    Y_ASSERT(VectorWidth == 4);
    const TIndexType idx0 = indices[0];
    const TIndexType idx1 = indices[1];
    const TIndexType idx2 = indices[2];
    const TIndexType idx3 = indices[3];
    const double deltasDimension0 = deltasDimension[0];
    const double deltasDimension1 = deltasDimension[1];
    const double deltasDimension2 = deltasDimension[2];
    const double deltasDimension3 = deltasDimension[3];
    const double delta0 = leafDeltas[idx0];
    const double delta1 = leafDeltas[idx1];
    const double delta2 = leafDeltas[idx2];
    const double delta3 = leafDeltas[idx3];
    deltasDimension[0] = UpdateApprox<StoreExpApprox>(deltasDimension0, delta0);
    deltasDimension[1] = UpdateApprox<StoreExpApprox>(deltasDimension1, delta1);
    deltasDimension[2] = UpdateApprox<StoreExpApprox>(deltasDimension2, delta2);
    deltasDimension[3] = UpdateApprox<StoreExpApprox>(deltasDimension3, delta3);
}

template <bool StoreExpApprox>
inline void UpdateApproxBlock(
    const NPar::TLocalExecutor::TExecRangeParams& params,
    const double* leafDeltas,
    const TIndexType* indices,
    int blockIdx,
    double* deltasDimension) {
    const int blockStart = blockIdx * params.GetBlockSize();
    const int nextBlockStart = Min<ui64>(blockStart + params.GetBlockSize(), params.LastId);
    constexpr int VectorWidth = 4;
    int doc;
    for (doc = blockStart; doc + VectorWidth <= nextBlockStart; doc += VectorWidth) {
        UpdateApproxKernel<StoreExpApprox, VectorWidth>(leafDeltas, indices + doc, deltasDimension + doc);
    }
    for (; doc < nextBlockStart; ++doc) {
        deltasDimension[doc] = UpdateApprox<StoreExpApprox>(deltasDimension[doc], leafDeltas[indices[doc]]);
    }
}

void UpdateApproxDeltas(
    bool storeExpApprox,
    const TVector<TIndexType>& indices,
    int docCount,
    NPar::TLocalExecutor* localExecutor,
    TVector<double>* leafDeltas,
    TVector<double>* deltasDimension) {
    ExpApproxIf(storeExpApprox, *leafDeltas);

    double* deltasDimensionData = deltasDimension->data();
    const TIndexType* indicesData = indices.data();
    const double* leafDeltasData = leafDeltas->data();

    NPar::TLocalExecutor::TExecRangeParams blockParams(0, docCount);
    blockParams.SetBlockSize(1000);

    const auto getUpdateApproxBlockLambda = [&](auto boolConst) -> std::function<void(int)> {
        return [=](int blockIdx) {
            UpdateApproxBlock</*StoreExpApprox*/ boolConst.value>(
                blockParams,
                leafDeltasData,
                indicesData,
                blockIdx,
                deltasDimensionData);
        };
    };
    const auto updateApproxBlockLambda = (storeExpApprox ? getUpdateApproxBlockLambda(std::true_type()) : getUpdateApproxBlockLambda(std::false_type()));
    localExecutor->ExecRange(
        updateApproxBlockLambda,
        0,
        blockParams.GetBlockCount(),
        NPar::TLocalExecutor::WAIT_COMPLETE);
}

static void CalcApproxDers(
    const TVector<double>& approxes,
    const TVector<double>& approxesDelta,
    const TVector<float>& targets,
    const TVector<float>& weights,
    const IDerCalcer& error,
    int sampleStart,
    int sampleFinish,
    TArrayRef<TDers> approxDers,
    TLearnContext* ctx) {
    NPar::TLocalExecutor::TExecRangeParams blockParams(sampleStart, sampleFinish);
    blockParams.SetBlockSize(APPROX_BLOCK_SIZE);
    ctx->LocalExecutor->ExecRangeWithThrow(
        [&](int blockId) {
            // espetrov: OK for small datasets
            const int blockOffset = sampleStart + blockId * blockParams.GetBlockSize();
            error.CalcDersRange(
                blockOffset,
                Min(blockParams.GetBlockSize(), sampleFinish - blockOffset),
                /*calcThirdDer=*/false,
                approxes.data(),
                approxesDelta.data(),
                targets.data(),
                weights.data(),
                approxDers.data() - sampleStart);
        },
        0,
        blockParams.GetBlockCount(),
        NPar::TLocalExecutor::WAIT_COMPLETE);
}

template <bool UseWeights>
static void CalcLeafDersImpl(
    int rowStart,
    int rowCount,
    TConstArrayRef<TIndexType> leafIndices,
    TConstArrayRef<float> weights,
    TConstArrayRef<TDers> approxDers,
    TArrayRef<TDers> leafDers, // view of size rowCount
    TArrayRef<double> leafWeights) {
    for (auto rowIdx : xrange(rowStart, rowStart + rowCount)) {
        TDers& ders = leafDers[leafIndices[rowIdx]];
        ders.Der1 += approxDers[rowIdx - rowStart].Der1;
        ders.Der2 += approxDers[rowIdx - rowStart].Der2;
        const double rowWeight = UseWeights ? weights[rowIdx] : 1;
        leafWeights[leafIndices[rowIdx]] += rowWeight;
    }
}

static void CalcLeafDers(
    TConstArrayRef<TIndexType> indices,
    TConstArrayRef<float> targets,
    TConstArrayRef<float> weights,
    TConstArrayRef<double> approxes,
    TConstArrayRef<double> approxesDelta,
    const IDerCalcer& error,
    int sampleCount,
    bool recalcLeafWeights,
    ELeavesEstimation estimationMethod,
    NPar::TLocalExecutor* localExecutor,
    TArrayRef<TSum> leafDers,
    TArrayRef<TDers> weightedDers) {
    NPar::TLocalExecutor::TExecRangeParams blockParams(0, sampleCount);
    blockParams.SetBlockCount(CB_THREAD_LIMIT);

    const int leafCount = leafDers.size();
    TVector<TVector<TDers>> blockBucketDers(
        blockParams.GetBlockCount(),
        TVector<TDers>(leafCount, TDers{/*Der1*/ 0.0, /*Der2*/ 0.0, /*Der3*/ 0.0}));
    TVector<TDers>* blockBucketDersData = blockBucketDers.data();
    // TODO(espetrov): Do not calculate sumWeights for Newton.
    // TODO(espetrov): Calculate sumWeights only on first iteration for Gradient, because on next iteration it
    //  is the same.
    // Check speedup on flights dataset.
    TVector<TVector<double>> blockBucketSumWeights(blockParams.GetBlockCount(), TVector<double>(leafCount, 0));
    TVector<double>* blockBucketSumWeightsData = blockBucketSumWeights.data();
    localExecutor->ExecRangeWithThrow(
        [=, &error](int blockId) {
            constexpr int innerBlockSize = APPROX_BLOCK_SIZE;
            const auto approxDers = MakeArrayRef(
                weightedDers.data() + innerBlockSize * blockId,
                innerBlockSize);

            const int blockStart = blockId * blockParams.GetBlockSize();
            const int nextBlockStart = Min(sampleCount, blockStart + blockParams.GetBlockSize());

            const auto bucketDers = MakeArrayRef(blockBucketDersData[blockId].data(), leafCount);
            const auto bucketSumWeights = MakeArrayRef(blockBucketSumWeightsData[blockId].data(), leafCount);

            for (int innerBlockStart = blockStart;
                 innerBlockStart < nextBlockStart;
                 innerBlockStart += innerBlockSize) {
                const int innerCount = Min(nextBlockStart - innerBlockStart, innerBlockSize);
                error.CalcDersRange(
                    0,
                    innerCount,
                    /*calcThirdDer=*/false,
                    approxes.data() + innerBlockStart,
                    approxesDelta.empty() ? nullptr : approxesDelta.data() + innerBlockStart,
                    targets.data() + innerBlockStart,
                    weights.empty() ? nullptr : weights.data() + innerBlockStart,
                    approxDers.data());
                if (weights.empty()) {
                    CalcLeafDersImpl<false>(
                        innerBlockStart,
                        innerCount,
                        indices,
                        weights,
                        approxDers,
                        bucketDers,
                        bucketSumWeights);
                } else {
                    CalcLeafDersImpl<true>(
                        innerBlockStart,
                        innerCount,
                        indices,
                        weights,
                        approxDers,
                        bucketDers,
                        bucketSumWeights);
                }
            }
        },
        0,
        blockParams.GetBlockCount(),
        NPar::TLocalExecutor::WAIT_COMPLETE);

    if (estimationMethod == ELeavesEstimation::Newton) {
        for (int leafId = 0; leafId < leafCount; ++leafId) {
            for (int blockId = 0; blockId < blockParams.GetBlockCount(); ++blockId) {
                if (blockBucketSumWeights[blockId][leafId] > FLT_EPSILON) {
                    AddMethodDer<ELeavesEstimation::Newton>(
                        blockBucketDers[blockId][leafId],
                        blockBucketSumWeights[blockId][leafId],
                        /* updateWeight */ false, // value doesn't matter
                        &leafDers[leafId]);
                }
            }
        }
    } else {
        Y_ASSERT(estimationMethod == ELeavesEstimation::Gradient);
        for (int leafId = 0; leafId < leafCount; ++leafId) {
            for (int blockId = 0; blockId < blockParams.GetBlockCount(); ++blockId) {
                if (blockBucketSumWeights[blockId][leafId] > FLT_EPSILON) {
                    AddMethodDer<ELeavesEstimation::Gradient>(
                        blockBucketDers[blockId][leafId],
                        blockBucketSumWeights[blockId][leafId],
                        recalcLeafWeights,
                        &leafDers[leafId]);
                }
            }
        }
    }
}

void CalcLeafDersSimple(
    const TVector<TIndexType>& indices,
    const TFold& fold,
    const TFold::TBodyTail& bt,
    const TVector<double>& approxes,
    const TVector<double>& approxDeltas,
    const IDerCalcer& error,
    int sampleCount,
    int queryCount,
    bool recalcLeafWeights,
    ELeavesEstimation estimationMethod,
    const NCatboostOptions::TCatBoostOptions& params,
    ui64 randomSeed,
    NPar::TLocalExecutor* localExecutor,
    TVector<TSum>* leafDers,
    TArray2D<double>* pairwiseBuckets,
    TVector<TDers>* scratchDers) {
    for (auto& leafDer : *leafDers) {
        leafDer.SetZeroDers();
    }
    if (error.GetErrorType() == EErrorType::PerObjectError) {
        CalcLeafDers(
            indices,
            fold.LearnTarget,
            fold.GetLearnWeights(),
            approxes,
            approxDeltas,
            error,
            sampleCount,
            recalcLeafWeights,
            estimationMethod,
            localExecutor,
            *leafDers,
            *scratchDers);
    } else {
        Y_ASSERT(
            error.GetErrorType() == EErrorType::QuerywiseError ||
            error.GetErrorType() == EErrorType::PairwiseError);

        TVector<TQueryInfo> recalculatedQueriesInfo;
        TVector<float> recalculatedPairwiseWeights;
        const bool shouldGenerateYetiRankPairs = IsYetiRankLossFunction(
            params.LossFunctionDescription->GetLossFunction());
        if (shouldGenerateYetiRankPairs) {
            YetiRankRecalculation(
                fold,
                bt,
                params,
                randomSeed,
                localExecutor,
                &recalculatedQueriesInfo,
                &recalculatedPairwiseWeights);
        }
        const TVector<TQueryInfo>& queriesInfo = shouldGenerateYetiRankPairs ? recalculatedQueriesInfo : fold.LearnQueriesInfo;
        const TVector<float>& weights = bt.PairwiseWeights.empty() ? fold.GetLearnWeights() : shouldGenerateYetiRankPairs ? recalculatedPairwiseWeights : bt.PairwiseWeights;

        CalculateDersForQueries(
            approxes,
            approxDeltas,
            fold.LearnTarget,
            weights,
            queriesInfo,
            error,
            /*queryStartIndex=*/0,
            queryCount,
            *scratchDers,
            randomSeed,
            localExecutor);
        AddLeafDersForQueries(
            *scratchDers,
            indices,
            weights,
            queriesInfo,
            /*queryStartIndex=*/0,
            queryCount,
            estimationMethod,
            recalcLeafWeights,
            leafDers,
            localExecutor);
        if (IsPairwiseScoring(params.LossFunctionDescription->GetLossFunction())) {
            const int leafCount = leafDers->ysize();
            *pairwiseBuckets = ComputePairwiseWeightSums(
                queriesInfo,
                leafCount,
                queryCount,
                indices,
                localExecutor);
        }
    }
}

void CalcLeafDeltasSimple(
    const TVector<TSum>& leafDers,
    const TArray2D<double>& pairwiseWeightSums,
    const NCatboostOptions::TCatBoostOptions& params,
    double sumAllWeights,
    int allDocCount,
    TVector<double>* leafDeltas) {
    const int leafCount = leafDers.ysize();
    const float l2Regularizer = params.ObliviousTreeOptions->L2Reg;
    const float pairwiseNonDiagReg = params.ObliviousTreeOptions->PairwiseNonDiagReg;
    if (IsPairwiseScoring(params.LossFunctionDescription->GetLossFunction())) {
        TVector<double> derSums(leafCount);
        for (int leaf = 0; leaf < leafCount; ++leaf) {
            derSums[leaf] = leafDers[leaf].SumDer;
        }
        *leafDeltas = CalculatePairwiseLeafValues(
            pairwiseWeightSums,
            derSums,
            l2Regularizer,
            pairwiseNonDiagReg);
        return;
    }

    leafDeltas->yresize(leafCount);
    const ELeavesEstimation estimationMethod = params.ObliviousTreeOptions->LeavesEstimationMethod;
    if (estimationMethod == ELeavesEstimation::Newton) {
        for (int leaf = 0; leaf < leafCount; ++leaf) {
            (*leafDeltas)[leaf] = CalcMethodDelta<ELeavesEstimation::Newton>(
                leafDers[leaf],
                l2Regularizer,
                sumAllWeights,
                allDocCount);
        }
    } else {
        Y_ASSERT(estimationMethod == ELeavesEstimation::Gradient);
        for (int leaf = 0; leaf < leafCount; ++leaf) {
            (*leafDeltas)[leaf] = CalcMethodDelta<ELeavesEstimation::Gradient>(
                leafDers[leaf],
                l2Regularizer,
                sumAllWeights,
                allDocCount);
        }
    }
}

static void CalcMonotonicLeafDeltasSimple(
    const TVector<TSum>& leafDers,
    const ELeavesEstimation& estimationMethod,
    const double scaledL2Regularizer,
    const TVector<double>& currLeafValues,
    const TVector<TVector<ui32>>& leafMonotonicLinearOrders,
    TVector<double>* leafDeltas) {
    const int leafCount = leafDers.size();
    leafDeltas->yresize(leafCount);
    TVector<double> leafWeights(leafCount);
    if (estimationMethod == ELeavesEstimation::Gradient) {
        for (int leafIndex = 0; leafIndex < leafCount; ++leafIndex) {
            const double leafWeight = leafDers[leafIndex].SumWeights + scaledL2Regularizer;
            leafWeights[leafIndex] = leafWeight;
            (*leafDeltas)[leafIndex] = leafDers[leafIndex].SumDer / leafWeight;
        }
    } else {
        Y_ASSERT(estimationMethod == ELeavesEstimation::Newton);
        for (int leafIndex = 0; leafIndex < leafCount; ++leafIndex) {
            const double leafWeight = -leafDers[leafIndex].SumDer2 + scaledL2Regularizer;
            leafWeights[leafIndex] = leafWeight;
            (*leafDeltas)[leafIndex] = leafDers[leafIndex].SumDer / leafWeight;
        }
    }
    TVector<double> updatedLeafValues = currLeafValues;
    AddElementwise(*leafDeltas, &updatedLeafValues);
    for (const auto& linearOrder : leafMonotonicLinearOrders) {
        CalcOneDimensionalIsotonicRegression(
            updatedLeafValues,
            leafWeights,
            linearOrder,
            &updatedLeafValues);
        Y_VERIFY_DEBUG(CheckMonotonicity(linearOrder, updatedLeafValues), "Tree monotonization failed");
    }
    for (int leafIndex = 0; leafIndex < leafCount; ++leafIndex) {
        (*leafDeltas)[leafIndex] = updatedLeafValues[leafIndex] - currLeafValues[leafIndex];
    }
}

static void UpdateApproxDeltasHistoricallyImpl(
    int rowStart,
    int rowCount,
    TConstArrayRef<TIndexType> leafIndices,
    TConstArrayRef<float> weights,
    TConstArrayRef<TDers> approxDers, // view of size rowCount
    float l2Regularizer,
    double bodySumWeight,
    ELeavesEstimation estimationMethod,
    bool useExpApprox,
    TArrayRef<TSum> leafDers,
    TArrayRef<double> approxDeltas) {
    const auto impl = [=](auto EstimationMethod, auto UseExpApprox, auto UseWeights) {
        double sumWeights = bodySumWeight;
        for (auto rowIdx : xrange(rowStart, rowStart + rowCount)) {
            const double rowWeight = UseWeights ? weights[rowIdx] : 1;
            sumWeights += rowWeight;
            TSum& leafDer = leafDers[leafIndices[rowIdx]];
            AddMethodDer<EstimationMethod>(
                approxDers[rowIdx - rowStart], rowWeight, /* updateWeight */ true, &leafDer);
            double approxDelta = CalcMethodDelta<EstimationMethod>(leafDer, l2Regularizer, sumWeights, rowIdx);
            if (UseExpApprox) {
                FastExpInplace(&approxDelta, /*count*/ 1);
            }
            approxDeltas[rowIdx] = UpdateApprox<UseExpApprox>(approxDeltas[rowIdx], approxDelta);
        }
    };
    using TAllowedLeavesEstimation = TIntOption<
        ELeavesEstimation, ELeavesEstimation::Newton, ELeavesEstimation::Gradient>;
    return ForwardArgsAsIntegralConst(
        impl,
        TAllowedLeavesEstimation(estimationMethod),
        useExpApprox,
        !weights.empty());
}

static void UpdateApproxDeltasHistorically(
    const TVector<TIndexType>& indices,
    const TFold& fold,
    const TFold::TBodyTail& bt,
    const IDerCalcer& error,
    float l2Regularizer,
    const NCatboostOptions::TCatBoostOptions& params,
    ui64 randomSeed,
    NPar::TLocalExecutor* localExecutor,
    TLearnContext* ctx,
    TArrayRef<TSum> leafDers,
    TVector<double>* approxDeltas,
    TArrayRef<TDers> approxDers) {
    TVector<TQueryInfo> recalculatedQueriesInfo;
    TVector<float> recalculatedPairwiseWeights;
    const bool shouldGenerateYetiRankPairs = IsYetiRankLossFunction(
        params.LossFunctionDescription->GetLossFunction());
    if (shouldGenerateYetiRankPairs) {
        YetiRankRecalculation(
            fold,
            bt,
            params,
            randomSeed,
            localExecutor,
            &recalculatedQueriesInfo,
            &recalculatedPairwiseWeights);
    }
    const TVector<TQueryInfo>& queriesInfo = shouldGenerateYetiRankPairs ? recalculatedQueriesInfo : fold.LearnQueriesInfo;
    const TVector<float>& weights = bt.PairwiseWeights.empty() ? fold.GetLearnWeights() : shouldGenerateYetiRankPairs ? recalculatedPairwiseWeights : bt.PairwiseWeights;

    if (error.GetErrorType() == EErrorType::PerObjectError) {
        CalcApproxDers(
            bt.Approx[0],
            *approxDeltas,
            fold.LearnTarget,
            weights,
            error,
            bt.BodyFinish,
            bt.TailFinish,
            approxDers,
            ctx);
    } else {
        Y_ASSERT(
            error.GetErrorType() == EErrorType::QuerywiseError ||
            error.GetErrorType() == EErrorType::PairwiseError);
        CalculateDersForQueries(
            bt.Approx[0],
            *approxDeltas,
            fold.LearnTarget,
            weights,
            queriesInfo,
            error,
            bt.BodyQueryFinish,
            bt.TailQueryFinish,
            approxDers,
            randomSeed,
            localExecutor);
    }
    const auto estimationMethod = ctx->Params.ObliviousTreeOptions->LeavesEstimationMethod;
    const auto start = bt.BodyFinish;
    const auto count = bt.TailFinish - bt.BodyFinish;
    UpdateApproxDeltasHistoricallyImpl(
        start,
        count,
        indices,
        weights,
        approxDers,
        l2Regularizer,
        bt.BodySumWeight,
        estimationMethod,
        error.GetIsExpApprox(),
        leafDers,
        *approxDeltas);
}

static void CalcQuantileLeafDeltas(
    const size_t leafCount,
    const TVector<TIndexType>& indices,
    const double alpha,
    const double delta,
    const size_t sampleCount,
    TConstArrayRef<double> approxes,
    TConstArrayRef<float> targets,
    TConstArrayRef<float> weights,
    TVector<double>* leafDeltas) {
    TVector<TVector<float>> leafSamples(leafCount);
    TVector<TVector<float>> leafWeights(leafCount);

    for (size_t i = 0; i < sampleCount; i++) {
        Y_ASSERT(indices[i] < leafSamples.size());
        leafSamples[indices[i]].emplace_back(targets[i] - approxes[i]);
        leafWeights[indices[i]].emplace_back(weights[i]);
    }

    Y_ASSERT(leafCount == leafDeltas->size());
    for (size_t i = 0; i < leafCount; i++) {
        Y_ASSERT(i < (*leafDeltas).size());
        Y_ASSERT(i < leafSamples.size());
        Y_ASSERT(i < leafWeights.size());
        TVector<float>& weight = leafWeights[i];
        TVector<float>& sample = leafSamples[i];
        double& leafDelta = (*leafDeltas)[i];
        leafDelta = CalcSampleQuantile(MakeConstArrayRef(sample), MakeConstArrayRef(weight), alpha, delta);
    }
}

static void CalcApproxDeltaSimple(
    const TFold& fold,
    const TFold::TBodyTail& bt,
    int leafCount,
    const IDerCalcer& error,
    const TVector<TIndexType>& indices,
    ui64 randomSeed,
    const TVector<int>& treeMonotoneConstraints,
    TLearnContext* ctx,
    TVector<TVector<double>>* approxDeltas,
    TVector<TVector<double>>* sumLeafDeltas) {
    const int scratchSize = Max(
        !ctx->Params.BoostingOptions->ApproxOnFullHistory ? 0 : bt.TailFinish - bt.BodyFinish,
        error.GetErrorType() == EErrorType::PerObjectError ? APPROX_BLOCK_SIZE * CB_THREAD_LIMIT : bt.BodyFinish);
    TVector<TDers> weightedDers;
    weightedDers.yresize(scratchSize); // iteration scratch space

    const auto treeLearnerOptions = ctx->Params.ObliviousTreeOptions.Get();
    const ui32 gradientIterations = treeLearnerOptions.LeavesEstimationIterations;
    const auto estimationMethod = treeLearnerOptions.LeavesEstimationMethod;
    TVector<TSum> leafDers(leafCount, TSum()); // iteration scratch space
    TArray2D<double> pairwiseBuckets;          // iteration scratch space
    const bool treeHasMonotonicConstraints = AnyOf(
        treeMonotoneConstraints,
        [](int val) { return val != 0; });
    const auto leafMonotonicLinearOrders = (treeHasMonotonicConstraints ? BuildMonotonicLinearOrdersOnLeafs(treeMonotoneConstraints) : TVector<TVector<ui32>>());

    const auto leafUpdaterFunc = [&](
                                     bool recalcLeafWeights,
                                     const TVector<TVector<double>>& approxDeltas,
                                     TVector<TVector<double>>* leafDeltas) {
        // If loss function is Quantile update leafDeltas specifically for it
        if (estimationMethod == ELeavesEstimation::Exact) {
            auto loss = ctx->Params.LossFunctionDescription->GetLossFunction();
            double alpha;
            double delta;
            if (loss == ELossFunction::Quantile) {
                const auto& quantileError = dynamic_cast<const TQuantileError&>(error);
                alpha = quantileError.Alpha;
                delta = quantileError.Delta;
            } else {
                alpha = 0.5;
                delta = DBL_EPSILON;
            }
            CalcQuantileLeafDeltas(
                leafCount,
                indices,
                alpha,
                delta,
                bt.BodyFinish,
                bt.Approx[0],
                fold.LearnTarget,
                MakeConstArrayRef(fold.SampleWeights),
                &(*leafDeltas)[0]);
            return;
        }

        CalcLeafDersSimple(
            indices,
            fold,
            bt,
            bt.Approx[0],
            approxDeltas[0],
            error,
            bt.BodyFinish,
            bt.BodyQueryFinish,
            recalcLeafWeights,
            estimationMethod,
            ctx->Params,
            randomSeed,
            ctx->LocalExecutor,
            &leafDers,
            &pairwiseBuckets,
            &weightedDers);
        if (treeHasMonotonicConstraints) {
            const double scaledL2Regularizer = (ctx->Params.ObliviousTreeOptions->L2Reg * (fold.GetSumWeight() / fold.GetLearnSampleCount()));
            CalcMonotonicLeafDeltasSimple(
                leafDers,
                estimationMethod,
                scaledL2Regularizer,
                /*curLeafValues*/ TVector<double>(leafCount),
                leafMonotonicLinearOrders,
                &(*leafDeltas)[0]);
        } else {
            CalcLeafDeltasSimple(
                leafDers,
                pairwiseBuckets,
                ctx->Params,
                bt.BodySumWeight,
                bt.BodyFinish,
                &(*leafDeltas)[0]);
        }
    };

    const float l2Regularizer = treeLearnerOptions.L2Reg;
    const auto approxUpdaterFunc = [&](
                                       const TVector<TVector<double>>& leafDeltas,
                                       TVector<TVector<double>>* approxDeltas) {
        auto localLeafValues = leafDeltas;
        if (!ctx->Params.BoostingOptions->ApproxOnFullHistory) {
            UpdateApproxDeltas(
                error.GetIsExpApprox(),
                indices,
                bt.TailFinish,
                ctx->LocalExecutor,
                &localLeafValues[0],
                &(*approxDeltas)[0]);
        } else {
            Y_ASSERT(!IsPairwiseScoring(ctx->Params.LossFunctionDescription->GetLossFunction()));
            UpdateApproxDeltas(
                error.GetIsExpApprox(),
                indices,
                bt.BodyFinish,
                ctx->LocalExecutor,
                &localLeafValues[0],
                &(*approxDeltas)[0]);
            auto localLeafDers = leafDers;
            UpdateApproxDeltasHistorically(
                indices,
                fold,
                bt,
                error,
                l2Regularizer,
                ctx->Params,
                randomSeed,
                ctx->LocalExecutor,
                ctx,
                localLeafDers,
                &(*approxDeltas)[0],
                weightedDers);
        }
    };

    bool haveBacktrackingObjective;
    double minimizationSign;
    TVector<THolder<IMetric>> lossFunction;
    CreateBacktrackingObjective(*ctx, &haveBacktrackingObjective, &minimizationSign, &lossFunction);

    const auto lossCalcerFunc = [&](const TVector<TVector<double>>& approxDeltas) {
        TConstArrayRef<TQueryInfo> bodyTailQueryInfo(fold.LearnQueriesInfo.begin(), bt.BodyQueryFinish);
        TConstArrayRef<float> bodyTailTarget(fold.LearnTarget.begin(), bt.BodyFinish);
        const auto& additiveStats = EvalErrors(
            bt.Approx,
            approxDeltas,
            error.GetIsExpApprox(),
            bodyTailTarget,
            fold.GetLearnWeights(),
            bodyTailQueryInfo,
            *lossFunction[0],
            ctx->LocalExecutor);
        return minimizationSign * lossFunction[0]->GetFinalError(additiveStats);
    };

    const auto approxCopyFunc = [ctx](const TVector<TVector<double>>& src, TVector<TVector<double>>* dst) {
        CopyApprox(src, dst, ctx->LocalExecutor);
    };

    GradientWalker(
        /*isTrivialWalker*/ !haveBacktrackingObjective,
        gradientIterations,
        leafCount,
        ctx->LearnProgress->ApproxDimension,
        leafUpdaterFunc,
        approxUpdaterFunc,
        lossCalcerFunc,
        approxCopyFunc,
        approxDeltas,
        sumLeafDeltas);
}

static void CalcLeafValuesSimple(
    int leafCount,
    const IDerCalcer& error,
    const TFold& fold,
    const TVector<TIndexType>& indices,
    const TVector<int>& treeMonotoneConstraints,
    TLearnContext* ctx,
    TVector<TVector<double>>* sumLeafDeltas) {
    const int scratchSize = error.GetErrorType() == EErrorType::PerObjectError
                                ? APPROX_BLOCK_SIZE * CB_THREAD_LIMIT
                                : fold.GetLearnSampleCount();
    TVector<TDers> weightedDers;
    weightedDers.yresize(scratchSize);
    sumLeafDeltas->assign(1, TVector<double>(leafCount));

    const int queryCount = fold.LearnQueriesInfo.ysize();
    const auto& learnerOptions = ctx->Params.ObliviousTreeOptions.Get();
    const int gradientIterations = learnerOptions.LeavesEstimationIterations;
    const auto estimationMethod = learnerOptions.LeavesEstimationMethod;
    auto& localExecutor = *ctx->LocalExecutor;
    const TFold::TBodyTail& bt = fold.BodyTailArr[0];

    const bool treeHasMonotonicConstraints = AnyOf(
        treeMonotoneConstraints,
        [](int val) { return val != 0; });
    const auto leafMonotonicLinearOrders = (treeHasMonotonicConstraints ? BuildMonotonicLinearOrdersOnLeafs(treeMonotoneConstraints) : TVector<TVector<ui32>>());

    TVector<TVector<double>> approxes;
    CopyApprox(bt.Approx, &approxes, ctx->LocalExecutor);
    TVector<TSum> leafDers(leafCount, TSum()); // iteration scratch space
    TArray2D<double> pairwiseBuckets;          // iteration scratch space
    const auto leafUpdaterFunc = [&](
                                     bool recalcLeafWeights,
                                     const TVector<TVector<double>>& approxes,
                                     TVector<TVector<double>>* leafDeltas) {
        // If loss function is Quantile update leafDeltas specifically for it
        if (estimationMethod == ELeavesEstimation::Exact) {
            auto loss = ctx->Params.LossFunctionDescription->GetLossFunction();
            double alpha;
            double delta;
            if (loss == ELossFunction::Quantile) {
                const auto& quantileError = dynamic_cast<const TQuantileError&>(error);
                alpha = quantileError.Alpha;
                delta = quantileError.Delta;
            } else {
                alpha = 0.5;
                delta = DBL_EPSILON;
            }
            CalcQuantileLeafDeltas(
                leafCount,
                indices,
                alpha,
                delta,
                bt.BodyFinish,
                bt.Approx[0],
                fold.LearnTarget,
                MakeConstArrayRef(fold.SampleWeights),
                &(*leafDeltas)[0]);
            return;
        }

        CalcLeafDersSimple(
            indices,
            fold,
            bt,
            approxes[0],
            /*approxDeltas*/ {},
            error,
            fold.GetLearnSampleCount(),
            queryCount,
            recalcLeafWeights,
            estimationMethod,
            ctx->Params,
            ctx->LearnProgress->Rand.GenRand(),
            &localExecutor,
            &leafDers,
            &pairwiseBuckets,
            &weightedDers);

        if (treeHasMonotonicConstraints) {
            const double scaledL2Regularizer = (ctx->Params.ObliviousTreeOptions->L2Reg * (fold.GetSumWeight() / fold.GetLearnSampleCount()));
            CalcMonotonicLeafDeltasSimple(
                leafDers,
                estimationMethod,
                scaledL2Regularizer,
                (*sumLeafDeltas)[0],
                leafMonotonicLinearOrders,
                &(*leafDeltas)[0]);
        } else {
            CalcLeafDeltasSimple(
                leafDers,
                pairwiseBuckets,
                ctx->Params,
                fold.GetSumWeight(),
                fold.GetLearnSampleCount(),
                &(*leafDeltas)[0]);
        }
    };

    const auto approxUpdaterFunc = [&](
                                       const TVector<TVector<double>>& leafDeltas,
                                       TVector<TVector<double>>* approxes) {
        auto localLeafValues = leafDeltas;
        UpdateApproxDeltas(
            error.GetIsExpApprox(),
            indices,
            fold.GetLearnSampleCount(),
            &localExecutor,
            &localLeafValues[0],
            &(*approxes)[0]);
    };

    bool haveBacktrackingObjective;
    double minimizationSign;
    TVector<THolder<IMetric>> lossFunction;
    CreateBacktrackingObjective(*ctx, &haveBacktrackingObjective, &minimizationSign, &lossFunction);

    const auto lossCalcerFunc = [&](const TVector<TVector<double>>& approx) {
        const auto& additiveStats = EvalErrors(
            approx,
            /*approxDelta*/ {},
            error.GetIsExpApprox(),
            fold.LearnTarget,
            fold.GetLearnWeights(),
            fold.LearnQueriesInfo,
            *lossFunction[0],
            &localExecutor);
        return minimizationSign * lossFunction[0]->GetFinalError(additiveStats);
    };

    const auto approxCopyFunc = [ctx](const TVector<TVector<double>>& src, TVector<TVector<double>>* dst) {
        CopyApprox(src, dst, ctx->LocalExecutor);
    };

    GradientWalker(
        /*isTrivialWalker*/ !haveBacktrackingObjective,
        gradientIterations,
        leafCount,
        ctx->LearnProgress->ApproxDimension,
        leafUpdaterFunc,
        approxUpdaterFunc,
        lossCalcerFunc,
        approxCopyFunc,
        &approxes,
        sumLeafDeltas);
}

void CalcLeafValues(
    const NCB::TTrainingForCPUDataProviders& data,
    const IDerCalcer& error,
    const TFold& fold,
    const TSplitTree& tree,
    TLearnContext* ctx,
    TVector<TVector<double>>* leafDeltas,
    TVector<TIndexType>* indices) {
    *indices = BuildIndices(fold, tree, data.Learn, data.Test, ctx->LocalExecutor);
    const int approxDimension = ctx->LearnProgress->AveragingFold.GetApproxDimension();
    Y_VERIFY(fold.GetLearnSampleCount() == data.Learn->GetObjectCount());
    const int leafCount = tree.GetLeafCount();

    const auto treeMonotoneConstraints = GetTreeMonotoneConstraints(
        tree,
        ctx->Params.ObliviousTreeOptions->MonotoneConstraints.Get());

    if (approxDimension == 1) {
        CalcLeafValuesSimple(leafCount, error, fold, *indices, treeMonotoneConstraints, ctx, leafDeltas);
    } else {
        CalcLeafValuesMulti(leafCount, error, fold, *indices, ctx, leafDeltas);
    }
}

// output is permuted (learnSampleCount samples are permuted by LearnPermutation, test is indexed directly)
void CalcApproxForLeafStruct(
    const NCB::TTrainingForCPUDataProviders& data,
    const IDerCalcer& error,
    const TFold& fold,
    const TSplitTree& tree,
    ui64 randomSeed,
    TLearnContext* ctx,
    TVector<TVector<TVector<double>>>* approxesDelta // [bodyTailId][approxDim][docIdxInPermuted]
) {
    const TVector<TIndexType> indices = BuildIndices(fold, tree, data.Learn, data.Test, ctx->LocalExecutor);
    const int approxDimension = ctx->LearnProgress->ApproxDimension;
    const int leafCount = tree.GetLeafCount();
    const auto treeMonotoneConstraints = GetTreeMonotoneConstraints(
        tree,
        ctx->Params.ObliviousTreeOptions->MonotoneConstraints.Get());

    TVector<ui64> randomSeeds;
    if (approxDimension == 1) {
        randomSeeds = GenRandUI64Vector(fold.BodyTailArr.ysize(), randomSeed);
    }
    approxesDelta->resize(fold.BodyTailArr.ysize());
    ctx->LocalExecutor->ExecRangeWithThrow(
        [&](int bodyTailId) {
            const TFold::TBodyTail& bt = fold.BodyTailArr[bodyTailId];
            TVector<TVector<double>>& approxDeltas = (*approxesDelta)[bodyTailId];
            const double initValue = GetNeutralApprox(error.GetIsExpApprox());
            NCB::FillRank2(initValue, approxDimension, bt.TailFinish, &approxDeltas, ctx->LocalExecutor);
            if (approxDimension == 1) {
                CalcApproxDeltaSimple(
                    fold,
                    bt,
                    leafCount,
                    error,
                    indices,
                    randomSeeds[bodyTailId],
                    treeMonotoneConstraints,
                    ctx,
                    &approxDeltas,
                    /*sumLeafDeltas*/ nullptr);
            } else {
                CalcApproxDeltaMulti(
                    fold,
                    bt,
                    leafCount,
                    error,
                    indices,
                    ctx,
                    &approxDeltas,
                    /*sumLeafDeltas*/ nullptr);
            }
        },
        0,
        fold.BodyTailArr.ysize(),
        NPar::TLocalExecutor::WAIT_COMPLETE);
}
