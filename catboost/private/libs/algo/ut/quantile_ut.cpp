#include <library/unittest/registar.h>

#include <catboost/private/libs/algo/approx_calcer.cpp>

#include <catboost/libs/data/data_provider_builders.h>
#include <catboost/libs/eval_result/eval_result.h>
#include <catboost/libs/train_lib/train_model.h>

using namespace NCB;

Y_UNIT_TEST_SUITE(TCalcQuantile) {

    const TVector<float> weightsNoWeights = {1, 1, 1, 1, 1, 1, 1, 1};
    const TVector<float> weightsHasWeights = {0.2, 0.4, 0.13, 0.43, 0.74, 0.3, 0.44, 0.37};
    const TVector<float> sampleOrderedNoWeights = {0, 2, 10, 37, 40, 500, 501, 600};
    TVector<float> sampleUnorderedNoWeights = {600, 40, 2, 500, 0, 37, 10, 501};
    const double eps = 1e-6;

    Y_UNIT_TEST(TCalcQuantileSampleOrderedNoWeights1) {
        UNIT_ASSERT_DOUBLES_EQUAL(CalcSampleQuantile(sampleOrderedNoWeights, weightsNoWeights, 0, eps), -eps, 1e-6);
    }

    Y_UNIT_TEST(TCalcQuantileSampleOrderedNoWeights2) {
        UNIT_ASSERT_DOUBLES_EQUAL(CalcSampleQuantile(sampleOrderedNoWeights, weightsNoWeights, 0.125 - 5 * DBL_EPSILON, eps), eps, 1e-6);
    }

    Y_UNIT_TEST(TCalcQuantileSampleOrderedNoWeights3) {
        UNIT_ASSERT_DOUBLES_EQUAL(CalcSampleQuantile(sampleOrderedNoWeights, weightsNoWeights, 0.25 - 5 * DBL_EPSILON, eps), 2 + eps, 1e-6);
    }

    Y_UNIT_TEST(TCalcQuantileSampleOrderedNoWeights4) {
        UNIT_ASSERT_DOUBLES_EQUAL(CalcSampleQuantile(sampleOrderedNoWeights, weightsNoWeights, 0.375 - 5 * DBL_EPSILON, eps), 10 + eps, 1e-6);
    }

    Y_UNIT_TEST(TCalcQuantileSampleOrderedNoWeights5) {
        UNIT_ASSERT_DOUBLES_EQUAL(CalcSampleQuantile(sampleOrderedNoWeights, weightsNoWeights, 0.5 - 5 * DBL_EPSILON, eps), 37 + eps, 1e-6);
    }

    Y_UNIT_TEST(TCalcQuantileSampleOrderedNoWeights6) {
        UNIT_ASSERT_DOUBLES_EQUAL(CalcSampleQuantile(sampleOrderedNoWeights, weightsNoWeights, 0.625 - 5 * DBL_EPSILON, eps), 40 + eps, 1e-6);
    }

    Y_UNIT_TEST(TCalcQuantileSampleOrderedNoWeights7) {
        UNIT_ASSERT_DOUBLES_EQUAL(CalcSampleQuantile(sampleOrderedNoWeights, weightsNoWeights, 0.75 - 5 * DBL_EPSILON, eps), 500 + eps, 1e-6);
    }

    Y_UNIT_TEST(TCalcQuantileSampleOrderedNoWeights8) {
        UNIT_ASSERT_DOUBLES_EQUAL(CalcSampleQuantile(sampleOrderedNoWeights, weightsNoWeights, 0.875 - 5 * DBL_EPSILON, eps), 501 + eps, 1e-6);
    }

    Y_UNIT_TEST(TCalcQuantileSampleOrderedNoWeights9) {
        UNIT_ASSERT_DOUBLES_EQUAL(CalcSampleQuantile(sampleOrderedNoWeights, weightsNoWeights, 1 - 5 * DBL_EPSILON, eps), 600 - eps, 1e-6);
    }

    Y_UNIT_TEST(TCalcQuantileSampleOrderedNoWeights10) {
        UNIT_ASSERT_DOUBLES_EQUAL(CalcSampleQuantile(sampleOrderedNoWeights, weightsNoWeights, 5 * DBL_EPSILON, eps), eps, 1e-6);
    }

    Y_UNIT_TEST(TCalcQuantileSampleOrderedNoWeights11) {
        UNIT_ASSERT_DOUBLES_EQUAL(CalcSampleQuantile(sampleOrderedNoWeights, weightsNoWeights, 0.125 + 5 * DBL_EPSILON, eps), 2 - eps, 1e-6);
    }

    Y_UNIT_TEST(TCalcQuantileSampleOrderedNoWeights12) {
        UNIT_ASSERT_DOUBLES_EQUAL(CalcSampleQuantile(sampleOrderedNoWeights, weightsNoWeights, 0.25 + 5 * DBL_EPSILON, eps), 10 - eps, 1e-6);
    }

    Y_UNIT_TEST(TCalcQuantileSampleOrderedNoWeights13) {
        UNIT_ASSERT_DOUBLES_EQUAL(CalcSampleQuantile(sampleOrderedNoWeights, weightsNoWeights, 0.375 + 5 * DBL_EPSILON, eps), 37 - eps, 1e-6);
    }

    Y_UNIT_TEST(TCalcQuantileSampleOrderedNoWeights14) {
        UNIT_ASSERT_DOUBLES_EQUAL(CalcSampleQuantile(sampleOrderedNoWeights, weightsNoWeights, 0.5 + 5 * DBL_EPSILON, eps), 40 - eps, 1e-6);
    }

    Y_UNIT_TEST(TCalcQuantileSampleOrderedNoWeights15) {
        UNIT_ASSERT_DOUBLES_EQUAL(CalcSampleQuantile(sampleOrderedNoWeights, weightsNoWeights, 0.625 + 5 * DBL_EPSILON, eps), 500 - eps, 1e-6);
    }

    Y_UNIT_TEST(TCalcQuantileSampleOrderedNoWeights16) {
        UNIT_ASSERT_DOUBLES_EQUAL(CalcSampleQuantile(sampleOrderedNoWeights, weightsNoWeights, 0.75 + 5 * DBL_EPSILON, eps), 501 - eps, 1e-6);
    }

    Y_UNIT_TEST(TCalcQuantileSampleOrderedNoWeights17) {
        UNIT_ASSERT_DOUBLES_EQUAL(CalcSampleQuantile(sampleOrderedNoWeights, weightsNoWeights, 0.875 + 5 * DBL_EPSILON, eps), 600 - eps, 1e-6);
    }

    Y_UNIT_TEST(TCalcQuantileSampleUnrderedNoWeights1) {
        UNIT_ASSERT_DOUBLES_EQUAL(CalcSampleQuantile(sampleUnorderedNoWeights, weightsNoWeights, 0, eps), -eps, 1e-6);
    }

    Y_UNIT_TEST(TCalcQuantileSampleUnrderedNoWeights2) {
        UNIT_ASSERT_DOUBLES_EQUAL(CalcSampleQuantile(sampleUnorderedNoWeights, weightsNoWeights, 0.125 - 5 * DBL_EPSILON, eps), eps, 1e-6);
    }

    Y_UNIT_TEST(TCalcQuantileSampleUnrderedNoWeights3) {
        UNIT_ASSERT_DOUBLES_EQUAL(CalcSampleQuantile(sampleUnorderedNoWeights, weightsNoWeights, 0.25 - 5 * DBL_EPSILON, eps), 2 + eps, 1e-6);
    }

    Y_UNIT_TEST(TCalcQuantileSampleUnrderedNoWeights4) {
        UNIT_ASSERT_DOUBLES_EQUAL(CalcSampleQuantile(sampleUnorderedNoWeights, weightsNoWeights, 0.375 - 5 * DBL_EPSILON, eps), 10 + eps, 1e-6);
    }

    Y_UNIT_TEST(TCalcQuantileSampleUnrderedNoWeights5) {
        UNIT_ASSERT_DOUBLES_EQUAL(CalcSampleQuantile(sampleUnorderedNoWeights, weightsNoWeights, 0.5 - 5 * DBL_EPSILON, eps), 37 + eps, 1e-6);
    }

    Y_UNIT_TEST(TCalcQuantileSampleUnrderedNoWeights6) {
        UNIT_ASSERT_DOUBLES_EQUAL(CalcSampleQuantile(sampleUnorderedNoWeights, weightsNoWeights, 0.625 - 5 * DBL_EPSILON, eps), 40 + eps, 1e-6);
    }

    Y_UNIT_TEST(TCalcQuantileSampleUnrderedNoWeights7) {
        UNIT_ASSERT_DOUBLES_EQUAL(CalcSampleQuantile(sampleUnorderedNoWeights, weightsNoWeights, 0.75 - 5 * DBL_EPSILON, eps), 500 + eps, 1e-6);
    }

    Y_UNIT_TEST(TCalcQuantileSampleUnrderedNoWeights8) {
        UNIT_ASSERT_DOUBLES_EQUAL(CalcSampleQuantile(sampleUnorderedNoWeights, weightsNoWeights, 0.875 - 5 * DBL_EPSILON, eps), 501 + eps, 1e-6);
    }

    Y_UNIT_TEST(TCalcQuantileSampleUnrderedNoWeights9) {
        UNIT_ASSERT_DOUBLES_EQUAL(CalcSampleQuantile(sampleUnorderedNoWeights, weightsNoWeights, 1 - 5 * DBL_EPSILON, eps), 600 - eps, 1e-6);
    }

    Y_UNIT_TEST(TCalcQuantileSampleUnrderedNoWeights10) {
        UNIT_ASSERT_DOUBLES_EQUAL(CalcSampleQuantile(sampleUnorderedNoWeights, weightsNoWeights, 5 * DBL_EPSILON, eps), eps, 1e-6);
    }

    Y_UNIT_TEST(TCalcQuantileSampleUnrderedNoWeights11) {
        UNIT_ASSERT_DOUBLES_EQUAL(CalcSampleQuantile(sampleUnorderedNoWeights, weightsNoWeights, 0.125 + 5 * DBL_EPSILON, eps), 2 - eps, 1e-6);
    }

    Y_UNIT_TEST(TCalcQuantileSampleUnrderedNoWeights12) {
        UNIT_ASSERT_DOUBLES_EQUAL(CalcSampleQuantile(sampleUnorderedNoWeights, weightsNoWeights, 0.25 + 5 * DBL_EPSILON, eps), 10 - eps, 1e-6);
    }

    Y_UNIT_TEST(TCalcQuantileSampleUnrderedNoWeights13) {
        UNIT_ASSERT_DOUBLES_EQUAL(CalcSampleQuantile(sampleUnorderedNoWeights, weightsNoWeights, 0.375 + 5 * DBL_EPSILON, eps), 37 - eps, 1e-6);
    }

    Y_UNIT_TEST(TCalcQuantileSampleUnrderedNoWeights14) {
        UNIT_ASSERT_DOUBLES_EQUAL(CalcSampleQuantile(sampleUnorderedNoWeights, weightsNoWeights, 0.5 + 5 * DBL_EPSILON, eps), 40 - eps, 1e-6);
    }

    Y_UNIT_TEST(TCalcQuantileSampleUnrderedNoWeights15) {
        UNIT_ASSERT_DOUBLES_EQUAL(CalcSampleQuantile(sampleUnorderedNoWeights, weightsNoWeights, 0.625 + 5 * DBL_EPSILON, eps), 500 - eps, 1e-6);
    }

    Y_UNIT_TEST(TCalcQuantileSampleUnrderedNoWeights16) {
        UNIT_ASSERT_DOUBLES_EQUAL(CalcSampleQuantile(sampleUnorderedNoWeights, weightsNoWeights, 0.75 + 5 * DBL_EPSILON, eps), 501 - eps, 1e-6);
    }

    Y_UNIT_TEST(TCalcQuantileSampleUnrderedNoWeights17) {
        UNIT_ASSERT_DOUBLES_EQUAL(CalcSampleQuantile(sampleUnorderedNoWeights, weightsNoWeights, 0.875 + 5 * DBL_EPSILON, eps), 600 - eps, 1e-6);
    }

    Y_UNIT_TEST(TCalcQuantileSampleOrderedNoWeightsRepeating1) {
        TVector<float> sample = {0, 1, 1, 1, 2, 2, 2, 2};
        UNIT_ASSERT_DOUBLES_EQUAL(CalcSampleQuantile(sample, weightsNoWeights, 0, eps), -eps, 1e-6);
    }

    Y_UNIT_TEST(TCalcQuantileSampleOrderedNoWeightsRepeating2) {
        TVector<float> sample = {0, 1, 1, 1, 2, 2, 2, 2};
        UNIT_ASSERT_DOUBLES_EQUAL(CalcSampleQuantile(sample, weightsNoWeights, 0.125 - 5 * DBL_EPSILON, eps), eps, 1e-6);
    }

    Y_UNIT_TEST(TCalcQuantileSampleOrderedNoWeightsRepeating3) {
        TVector<float> sample = {0, 1, 1, 1, 2, 2, 2, 2};
        UNIT_ASSERT_DOUBLES_EQUAL(CalcSampleQuantile(sample, weightsNoWeights, 0.125 + 5 * DBL_EPSILON, eps), 1 - eps, 1e-6);
    }

    Y_UNIT_TEST(TCalcQuantileSampleOrderedNoWeightsRepeating4) {
        TVector<float> sample = {0, 1, 1, 1, 2, 2, 2, 2};
        UNIT_ASSERT_DOUBLES_EQUAL(CalcSampleQuantile(sample, weightsNoWeights, 0.2 - 5 * DBL_EPSILON, eps), 1 - eps, 1e-6);
    }

    Y_UNIT_TEST(TCalcQuantileSampleOrderedNoWeightsRepeating5) {
        TVector<float> sample = {0, 1, 1, 1, 2, 2, 2, 2};
        UNIT_ASSERT_DOUBLES_EQUAL(CalcSampleQuantile(sample, weightsNoWeights, 0.2 + 5 * DBL_EPSILON, eps), 1 + eps, 1e-6);
    }

    Y_UNIT_TEST(TCalcQuantileSampleOrderedNoWeightsRepeating6) {
        TVector<float> sample = {0, 1, 1, 1, 2, 2, 2, 2};
        UNIT_ASSERT_DOUBLES_EQUAL(CalcSampleQuantile(sample, weightsNoWeights, 0.5 - 5 * DBL_EPSILON, eps), 1 + eps , 1e-6);
    }

    Y_UNIT_TEST(TCalcQuantileSampleOrderedNoWeightsRepeating7) {
        TVector<float> sample = {0, 1, 1, 1, 2, 2, 2, 2};
        UNIT_ASSERT_DOUBLES_EQUAL(CalcSampleQuantile(sample, weightsNoWeights, 0.5 + 5 * DBL_EPSILON, eps), 2 - eps, 1e-6);
    }

    Y_UNIT_TEST(TCalcQuantileSampleOrderedWeights1) {
        TVector<float> sample =    {0,     1,      2,      3,      4,      5,      6,      7};
        UNIT_ASSERT_DOUBLES_EQUAL(CalcSampleQuantile(sample, weightsHasWeights, 0.38, eps), 3 + eps, 1e-6);
    }

    Y_UNIT_TEST(TCalcQuantileSampleOrderedWeights2) {
        TVector<float> sample =    {0,     1,      2,      3,      4,      5,      6,      7};
        UNIT_ASSERT_DOUBLES_EQUAL(CalcSampleQuantile(sample, weightsHasWeights, 0.5, eps), 4 - eps, 1e-6);
    }

    Y_UNIT_TEST(TCalcQuantileSampleOrderedWeights3) {
        TVector<float> sample =    {0,     1,      2,      3,      4,      5,      6,      7};
        UNIT_ASSERT_DOUBLES_EQUAL(CalcSampleQuantile(sample, weightsHasWeights, 0.52, eps), 4 + eps, 1e-6);
    }

    Y_UNIT_TEST(TCalcQuantileLeafDeltas) {
        TVector<TIndexType> indices = {0, 1, 1, 0, 1, 1, 0, 0, 0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 0, 1};
        TVector<float> targets = {5, 21, 24, 7, 30, 28, 4, 1, 9, 23, 22, 3, 27, 8, 10, 25, 2, 29, 6, 26};
        size_t sampleCount = indices.size();
        TVector<double> approxes(sampleCount);
        TVector<float> weights1(sampleCount, 1);
        TVector<double> leafDeltas(2);
        double alpha = 0.5;
        double delta = 1e-6;
        CalcQuantileLeafDeltas(2, indices, alpha, delta, sampleCount, MakeConstArrayRef(approxes), MakeConstArrayRef(targets), MakeConstArrayRef(weights1), &leafDeltas);
        UNIT_ASSERT_DOUBLES_EQUAL(leafDeltas[0], 5 + eps, 1e-6);
        UNIT_ASSERT_DOUBLES_EQUAL(leafDeltas[1], 25 + eps, 1e-6);
    }
}
