#include <catboost/private/libs/feature_estimator/text_feature_estimators.h>
#include <catboost/private/libs/feature_estimator/base_text_feature_estimator.h>

#include <catboost/private/libs/options/text_processing_options.h>
#include <catboost/private/libs/text_features/bow.h>
#include <catboost/private/libs/text_features/bm25.h>
#include <catboost/private/libs/text_features/naive_bayesian.h>

#include <library/unittest/registar.h>
#include <util/random/fast.h>


using namespace NCB;
using namespace NCatboostOptions;

Y_UNIT_TEST_SUITE(TestFeatureEstimators) {
    Y_UNIT_TEST(TestTargetLeakage) {
        class TIdentityVisitor;

        class TIdentityCalcer : public TTextFeatureCalcer {
        public:
            TIdentityCalcer()
                : TTextFeatureCalcer(1, CreateGuid())
            {}

            EFeatureCalcerType Type() const override {
                return EFeatureCalcerType::BM25; // only for test purposes
            }

            void Compute(const TText& text, TOutputFloatIterator outputFeaturesIterator) const override {
                Y_UNUSED(text);
                *outputFeaturesIterator = Storage;
                outputFeaturesIterator++;
            }

        private:
            ui32 Storage = 0;

            friend class TIdentityVisitor;
        };

        class TIdentityVisitor final : public ITextCalcerVisitor {
        public:
            void Update(ui32 classIdx, const TText& text, TTextFeatureCalcer* calcer) {
                auto identityCalcer = dynamic_cast<TIdentityCalcer*>(calcer);
                Y_ASSERT(identityCalcer);

                Y_UNUSED(text);
                identityCalcer->Storage = classIdx;
            }
        };

        class TSampleCountEstimator : public TBaseEstimator<TIdentityCalcer, TIdentityVisitor> {
        public:
            TSampleCountEstimator(
                TTextClassificationTargetPtr target,
                TTextDataSetPtr learnTexts,
                TArrayRef<TTextDataSetPtr> testText)
                : TBaseEstimator(std::move(target), std::move(learnTexts), testText)
                , Identity()
            {
            }

            TEstimatedFeaturesMeta FeaturesMeta() const override {
                TEstimatedFeaturesMeta meta;
                meta.FeaturesCount = 1;
                meta.Type.resize(meta.FeaturesCount, Identity.Type());
                return meta;
            }

            TIdentityCalcer CreateFeatureCalcer() const override {
                return TIdentityCalcer();
            }

            TIdentityVisitor CreateCalcerVisitor() const override {
                return TIdentityVisitor();
            }

        private:
            TIdentityCalcer Identity;
        };

        const ui32 numSamples = 100;

        const ui32 numClasses = numSamples + 1;
        TVector<ui32> classes(numSamples);
        for (ui32 i: xrange(numSamples)) {
            classes[i] = i + 1;
        }
        TTextClassificationTargetPtr target = MakeIntrusive<TTextClassificationTarget>(
            std::move(classes),
            numClasses
        );

        TVector<TText> texts(numSamples);
        {
            TText text;
            text.insert({/*tokenId*/ 0, /*count*/1});
            Fill(texts.begin(), texts.end(), text);
        }

        TTextColumnDictionaryOptions columnDictionaryOptions;
        TDictionaryPtr dictionary = new TDictionaryProxy(
            NTextProcessing::NDictionary::TDictionaryBuilder(
                columnDictionaryOptions.DictionaryBuilderOptions,
                columnDictionaryOptions.DictionaryOptions
            ).FinishBuilding()
        );

        TTextColumn textColumn = TTextColumn::CreateOwning(std::move(texts));
        TTextDataSetPtr learnTexts = MakeIntrusive<TTextDataSet>(textColumn, dictionary);
        TVector<TTextDataSetPtr> testText;

        TSampleCountEstimator targetIdentityEstimator(target, learnTexts, testText);

        TVector<ui32> learnPermutation(learnTexts->SamplesCount());
        Iota(learnPermutation.begin(), learnPermutation.end(), 0);

        TVector<float> learn(learnTexts->SamplesCount());
        TCalculatedFeatureVisitor learnVisitor = [&](ui32 featureId, TConstArrayRef<float> features) {
            Y_UNUSED(featureId);
            for (ui32 sampleId : xrange(features.size())) {
                learn[sampleId] = features[sampleId];
            }
        };
        TVector<TCalculatedFeatureVisitor> testVisitors;

        NPar::TLocalExecutor localExecutor;
        targetIdentityEstimator.ComputeOnlineFeatures(
            learnPermutation,
            learnVisitor,
            testVisitors,
            &localExecutor
        );

        for (ui32 i : xrange(learnTexts->SamplesCount())) {
            UNIT_ASSERT_EQUAL(i, learn[i]);
        }
    }

    Y_UNIT_TEST(TestIdenticalOutput) {
        const ui32 numSamples = 100;
        const ui32 numClasses = 10;
        const ui32 dictionarySize = 30;

        TVector<ui32> classes(numSamples);
        for (ui32 i: xrange(numSamples)) {
            classes[i] = i % numClasses;
        }
        TTextClassificationTargetPtr target = MakeIntrusive<TTextClassificationTarget>(
            std::move(classes),
            numClasses
        );

        TFastRng<ui64> rng(42);
        TVector<TText> texts;
        texts.yresize(numSamples);
        for (ui32 sampleId: xrange(numSamples)) {
            Y_UNUSED(sampleId);
            TText text;
            for (ui32 tokenId: xrange(dictionarySize)) {
                double real1 = rng.GenRandReal1();
                if (real1 > 0.5) {
                    text.insert({tokenId, static_cast<ui32>(real1 * 10)});
                }
            }
            texts[sampleId] = text;
        }

        TTextColumnDictionaryOptions columnDictionaryOptions;
        TDictionaryPtr dictionary = new TDictionaryProxy(
            NTextProcessing::NDictionary::TDictionaryBuilder(
                columnDictionaryOptions.DictionaryBuilderOptions,
                columnDictionaryOptions.DictionaryOptions
            ).FinishBuilding()
        );

        TTextColumn textColumn = TTextColumn::CreateOwning(std::move(texts));
        TTextDataSetPtr learnTexts = MakeIntrusive<TTextDataSet>(textColumn, dictionary);
        TVector<TTextDataSetPtr> testText;

        TVector<ui32> learnPermutation(learnTexts->SamplesCount());
        Iota(learnPermutation.begin(), learnPermutation.end(), 0);

        NPar::TLocalExecutor localExecutor;

        const TVector<EFeatureCalcerType> calcerTypes = {
            EFeatureCalcerType::BM25,
            EFeatureCalcerType::NaiveBayes
        };

        const TMap<EFeatureCalcerType, TTextFeatureCalcerPtr> calcers = {
            {EFeatureCalcerType::BM25, MakeIntrusive<TBM25>(CreateGuid(), numClasses)},
            {EFeatureCalcerType::NaiveBayes, MakeIntrusive<TMultinomialNaiveBayes>(CreateGuid(), numClasses)}
        };

        const TMap<EFeatureCalcerType, TTextCalcerVisitorPtr> visitors = {
            {EFeatureCalcerType::BM25, MakeIntrusive<TBM25Visitor>()},
            {EFeatureCalcerType::NaiveBayes, MakeIntrusive<TNaiveBayesVisitor>()}
        };

        TEmbeddingPtr embeddingPtr;
        for (auto calcerType : calcerTypes) {
            TVector<TOnlineFeatureEstimatorPtr> estimators = CreateEstimators(
                {NCatboostOptions::TFeatureCalcerDescription(calcerType)},
                embeddingPtr,
                target,
                learnTexts,
                testText
            );
            UNIT_ASSERT(estimators.size() == 1);

            auto& calcer = calcers.at(calcerType);
            auto& visitor = visitors.at(calcerType);

            TVector<float> learn(learnTexts->SamplesCount() * calcer->FeatureCount());

            TCalculatedFeatureVisitor learnVisitor = [&](ui32 featureId, TConstArrayRef<float> features) {
                for (ui32 sampleId : xrange(features.size())) {
                    learn[featureId * learnTexts->SamplesCount() + sampleId] = features[sampleId];
                }
            };
            TVector<TCalculatedFeatureVisitor> testVisitors;

            estimators[0]->ComputeOnlineFeatures(
                learnPermutation,
                learnVisitor,
                testVisitors,
                &localExecutor
            );

            for (ui32 line : learnPermutation) {
                TVector<float> features = calcer->Compute(learnTexts->GetText(line));
                visitor->Update(target->Classes[line], learnTexts->GetText(line), calcer.Get());

                for (ui32 featureId: xrange(calcer->FeatureCount())) {
                    UNIT_ASSERT_EQUAL(features[featureId], learn[featureId * numSamples + line]);
                }
            }
        }
    }
}
