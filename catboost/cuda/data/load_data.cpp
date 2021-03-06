#include "load_data.h"

#include <catboost/cuda/data/binarized_features_meta_info.h>
#include <catboost/idl/pool/flat/quantized_chunk_t.fbs.h>
#include <catboost/libs/column_description/column.h>
#include <catboost/libs/data/doc_pool_data_provider.h>
#include <catboost/libs/data/load_data.h>
#include <catboost/libs/data/pool.h>
#include <catboost/libs/data_util/path_with_scheme.h>
#include <catboost/libs/helpers/exception.h>
#include <catboost/libs/helpers/math_utils.h>
#include <catboost/libs/helpers/permutation.h>
#include <catboost/libs/quantization/grid_creator.h>
#include <catboost/libs/quantization/utils.h>
#include <catboost/libs/quantization_schema/detail.h>
#include <catboost/libs/quantization_schema/schema.h>
#include <catboost/libs/quantization_schema/serialization.h>
#include <catboost/libs/quantized_pool/pool.h>
#include <catboost/libs/quantized_pool/quantized.h>
#include <catboost/libs/quantized_pool/serialization.h>

#include <util/generic/algorithm.h>
#include <util/generic/array_ref.h>
#include <util/generic/is_in.h>
#include <util/generic/scope.h>
#include <util/system/types.h>
#include <util/system/unaligned_mem.h>

#include <limits>

using NCB::NQuantizationSchemaDetail::NanModeFromProto;

namespace NCatboostCuda {

    static inline void ValidateWeights(const TVector<float>& weights) {
        bool hasNonZero = false;
        for (const auto& w : weights) {
            CB_ENSURE(w >= 0, "Weights can't be negative");
            hasNonZero |= Abs(w) != 0;
        }
        CB_ENSURE(hasNonZero, "Error: all weights are zero");
    }

    template <class T>
    static inline bool IsConstant(const TVector<T>& vec) {
        for (const auto& elem : vec) {
            if (!(elem == vec[0])) {
                return false;
            }
        }
        return true;
    }

    void TDataProviderBuilder::StartNextBlock(ui32 blockSize) {
        Cursor = DataProvider.Targets.size();
        const auto newDataSize = Cursor + blockSize;

        DataProvider.Targets.resize(newDataSize);
        DataProvider.Weights.resize(newDataSize, 1.0);
        DataProvider.QueryIds.resize(newDataSize);
        DataProvider.SubgroupIds.resize(newDataSize);
        DataProvider.Timestamp.resize(newDataSize);

        for (ui32 i = Cursor; i < DataProvider.QueryIds.size(); ++i) {
            DataProvider.QueryIds[i] = TGroupId(i);
            DataProvider.SubgroupIds[i] = i;
        }

        for (auto& baseline : DataProvider.Baseline) {
            baseline.resize(newDataSize);
        }

        for (ui32 featureId = 0; featureId < FeatureBlobs.size(); ++featureId) {
            if (IgnoreFeatures.count(featureId) == 0) {
                FeatureBlobs[featureId].resize(newDataSize * GetBytesPerFeature(featureId));
            }
        }

        Labels.resize(newDataSize);
    }

    static inline bool HasQueryIds(const TVector<TGroupId>& qids) {
        for (ui32 i = 0; i < qids.size(); ++i) {
            if (qids[i] != TGroupId(i)) {
                return true;
            }
        }
        return false;
    }

    template <class T>
    static inline TVector<T> MakeOrderedLine(const TVector<ui8>& source,
                                             const TVector<ui64>& order) {
        CB_ENSURE(source.size() == sizeof(T) * order.size(), "Error: size should be consistent " << source.size() << "  " << order.size() << " " << sizeof(T));
        TVector<T> line(order.size());

        for (size_t i = 0; i < order.size(); ++i) {
            const T* rawSourcePtr = reinterpret_cast<const T*>(source.data());
            line[i] = rawSourcePtr[order[i]];
        }
        return line;
    }

    void TDataProviderBuilder::Finish() {
        auto startTimeBuilder = Now();


        CB_ENSURE(!IsDone, "Error: can't finish more than once");
        DataProvider.Features.reserve(FeatureBlobs.size());

        DataProvider.Order.resize(DataProvider.Targets.size());
        std::iota(DataProvider.Order.begin(),
                  DataProvider.Order.end(), 0);

        if (!AreEqualTo<ui64>(DataProvider.Timestamp, 0)) {
            ShuffleFlag = false;
            DataProvider.Order = CreateOrderByKey(DataProvider.Timestamp);
        }

        bool hasQueryIds = HasQueryIds(DataProvider.QueryIds);
        if (!hasQueryIds) {
            DataProvider.QueryIds.resize(0);
        }

        if (TargetHelper) {
            DataProvider.ClassificationTargetHelper = TargetHelper;
            TargetHelper->MakeTargetAndWeights(!IsTest, &DataProvider.Targets, &DataProvider.Weights);
        }

        //TODO(noxoomo): it's not safe here, if we change order with shuffle everything'll go wrong
        if (Pairs.size()) {
            //they are local, so we don't need shuffle
            CB_ENSURE(hasQueryIds, "Error: for GPU pairwise learning you should provide query id column. Query ids will be used to split data between devices and for dynamic boosting learning scheme.");
            DataProvider.FillQueryPairs(Pairs);
        }

        if (ShuffleFlag) {
            if (hasQueryIds) {
                //should not change order inside query for pairs consistency
                QueryConsistentShuffle(Seed, 1, DataProvider.QueryIds, &DataProvider.Order);
            } else {
                Shuffle(Seed, 1, DataProvider.Targets.size(), &DataProvider.Order);
            }
            DataProvider.SetShuffleSeed(Seed);
        }

        if (ShuffleFlag || !DataProvider.Timestamp.empty()) {
            DataProvider.ApplyOrderToMetaColumns();
        }

        TVector<TString> featureNames;
        featureNames.resize(FeatureBlobs.size());

        TAdaptiveLock lock;

        NPar::TLocalExecutor executor;
        executor.RunAdditionalThreads(BuildThreads - 1);

        TVector<TFeatureColumnPtr> featureColumns(FeatureBlobs.size());

        if (!IsTest) {
            RegisterFeaturesInFeatureManager(featureColumns);
        }

        NPar::ParallelFor(executor, 0, static_cast<ui32>(FeatureBlobs.size()), [&](ui32 featureId) {
            auto featureName = GetFeatureName(featureId);
            featureNames[featureId] = featureName;

            if (FeatureBlobs[featureId].size() == 0) {
                return;
            }

            EFeatureValuesType featureValuesType = FeatureTypes[featureId];

            if (featureValuesType == EFeatureValuesType::Categorical) {
                CB_ENSURE(featureValuesType == EFeatureValuesType::Categorical, "Wrong type " << featureValuesType);

                auto line = MakeOrderedLine<float>(FeatureBlobs[featureId],
                                                   DataProvider.Order);

                static_assert(sizeof(float) == sizeof(ui32), "Error: float size should be equal to ui32 size");
                const bool shouldSkip = IsTest && (CatFeaturesPerfectHashHelper.GetUniqueValues(featureId) == 0);
                if (!shouldSkip) {
                    auto data = CatFeaturesPerfectHashHelper.UpdatePerfectHashAndBinarize(featureId,
                                                                                          ~line,
                                                                                          line.size());

                    const ui32 uniqueValues = CatFeaturesPerfectHashHelper.GetUniqueValues(featureId);

                    if (uniqueValues > 1) {
                        auto compressedData = CompressVector<ui64>(~data, line.size(), NCB::IntLog2(uniqueValues));
                        featureColumns[featureId] = MakeHolder<TCatFeatureValuesHolder>(featureId,
                                                                                        line.size(),
                                                                                        std::move(compressedData),
                                                                                        uniqueValues,
                                                                                        featureName);
                    }
                }
            } else if (featureValuesType == EFeatureValuesType::BinarizedFloat) {
                const TVector<float>& borders = Borders.at(featureId);
                const ENanMode nanMode = NanModes.at(featureId);
                if (borders.ysize() == 0) {
                    CATBOOST_DEBUG_LOG << "Float Feature #" << featureId << " is empty" << Endl;
                    return;
                }

                TVector<ui8> binarizedData = MakeOrderedLine<ui8>(FeatureBlobs[featureId],
                                                                  DataProvider.Order);

                const ui32 binCount = NCB::GetBinCount(borders, nanMode);
                auto compressedLine = CompressVector<ui64>(binarizedData, NCB::IntLog2(binCount));

                featureColumns[featureId] = MakeHolder<TBinarizedFloatValuesHolder>(featureId,
                                                                                    DataProvider.Order.size(),
                                                                                    nanMode,
                                                                                    borders,
                                                                                    std::move(compressedLine),
                                                                                    featureName);
                with_lock (lock) {
                    FeaturesManager.SetOrCheckNanMode(*featureColumns[featureId],
                                                      nanMode);
                }
            } else {
                CB_ENSURE(featureValuesType == EFeatureValuesType::Float, "Wrong feature values type (" << featureValuesType << ") for feature #" << featureId);
                TVector<float> line(DataProvider.Order.size());
                for (ui32 i = 0; i < DataProvider.Order.size(); ++i) {
                    const float* floatFeatureSource = reinterpret_cast<float*>(FeatureBlobs[featureId].data());
                    line[i] = floatFeatureSource[DataProvider.Order[i]];
                }
                auto floatFeature = MakeHolder<TFloatValuesHolder>(featureId,
                                                                   std::move(line),
                                                                   featureName);

                TVector<float>& borders = Borders[featureId];

                auto& nanMode = NanModes[featureId];
                {
                    TGuard<TAdaptiveLock> guard(lock);
                    nanMode = FeaturesManager.GetOrComputeNanMode(*floatFeature);
                }

                if (FeaturesManager.HasFloatFeatureBorders(*floatFeature)) {
                    borders = FeaturesManager.GetFloatFeatureBorders(*floatFeature);
                }

                if (borders.empty() && !IsTest) {
                    const auto& floatValues = floatFeature->GetValues();
                    NCatboostOptions::TBinarizationOptions config = FeaturesManager.GetFloatFeatureBinarization();
                    config.NanMode = nanMode;
                    borders = NCB::BuildBorders(floatValues, floatFeature->GetId(), config);
                }
                if (borders.ysize() == 0) {
                    CATBOOST_DEBUG_LOG << "Float Feature #" << featureId << " is empty" << Endl;
                    return;
                }

                auto binarizedData = NCB::BinarizeLine<ui8>(floatFeature->GetValues(),
                                                            nanMode,
                                                            borders);

                const ui32 binCount = NCB::GetBinCount(borders, nanMode);
                auto compressedLine = CompressVector<ui64>(binarizedData, NCB::IntLog2(binCount));

                featureColumns[featureId] = MakeHolder<TBinarizedFloatValuesHolder>(featureId,
                                                                                    floatFeature->GetValues().size(),
                                                                                    nanMode,
                                                                                    borders,
                                                                                    std::move(compressedLine),
                                                                                    featureName);
            }

            //Free memory
            {
                auto emptyVec = TVector<ui8>();
                FeatureBlobs[featureId].swap(emptyVec);
            }
        });

        for (ui32 featureId = 0; featureId < featureColumns.size(); ++featureId) {
            if (FeatureTypes[featureId] == EFeatureValuesType::Categorical) {
                if (featureColumns[featureId] == nullptr && (!IsTest)) {
                    CATBOOST_DEBUG_LOG << "Cat Feature #" << featureId << " is empty" << Endl;
                }
            } else if (featureColumns[featureId] != nullptr) {
                if (!FeaturesManager.HasFloatFeatureBordersForDataProviderFeature(featureId)) {
                    FeaturesManager.SetFloatFeatureBordersForDataProviderId(featureId,
                                                                            std::move(Borders[featureId]));
                }
            }
            if (featureColumns[featureId] != nullptr) {
                DataProvider.Features.push_back(std::move(featureColumns[featureId]));
            }
        }

        DataProvider.BuildIndicesRemap();

        if (!IsTest) {
            NCB::TOnCpuGridBuilderFactory gridBuilderFactory;
            FeaturesManager.SetTargetBorders(NCB::TBordersBuilder(gridBuilderFactory,
                                                                  DataProvider.GetTargets())(FeaturesManager.GetTargetBinarizationDescription()));
        }

        DataProvider.FeatureNames = featureNames;
        ValidateWeights(DataProvider.Weights);


        const bool isConstTarget = IsConstant(DataProvider.Targets);
        if (isConstTarget && IsConstant(Pairs)) {
            CB_ENSURE(false, "Error: input target is constant and there are no pairs. No way you could learn on such dataset");
        }
        if (isConstTarget) {
            CATBOOST_WARNING_LOG << "Labels column is constant. You could learn only pairClassification (if you provided pairs) on such dataset" << Endl;
        }

        const TString dataProviderName = IsTest ? "test" : "learn";
        CATBOOST_DEBUG_LOG << "Build " << dataProviderName << " dataProvider time " << (Now() - startTimeBuilder).SecondsFloat() << Endl;

        IsDone = true;
    }

    void TDataProviderBuilder::WriteBinarizedFeatureToBlobImpl(ui32 localIdx, ui32 featureId, ui8 feature) {
        Y_ASSERT(IgnoreFeatures.count(featureId) == 0);
        Y_ASSERT(FeatureTypes[featureId] == EFeatureValuesType::BinarizedFloat);
        ui8* featureColumn = FeatureBlobs[featureId].data();
        featureColumn[GetLineIdx(localIdx)] = feature;
    }

    void TDataProviderBuilder::WriteFloatOrCatFeatureToBlobImpl(ui32 localIdx, ui32 featureId, float feature) {
        Y_ASSERT(IgnoreFeatures.count(featureId) == 0);
        Y_ASSERT(FeatureTypes[featureId] == EFeatureValuesType::Float || FeatureTypes[featureId] == EFeatureValuesType::Categorical);

        auto* featureColumn = reinterpret_cast<float*>(FeatureBlobs[featureId].data());
        featureColumn[GetLineIdx(localIdx)] = feature;
    }

    void TDataProviderBuilder::Start(const TPoolMetaInfo& poolMetaInfo,
                                     int docCount,
                                     const TVector<int>& catFeatureIds) {
        DataProvider.Features.clear();

        DataProvider.Baseline.clear();
        DataProvider.Baseline.resize(poolMetaInfo.BaselineCount);

        Cursor = 0;
        IsDone = false;

        FeatureBlobs.clear();
        FeatureBlobs.resize(poolMetaInfo.FeatureCount);

        FeatureTypes.resize(poolMetaInfo.FeatureCount, EFeatureValuesType::Float);
        for (int catFeature : catFeatureIds) {
            FeatureTypes[catFeature] = EFeatureValuesType::Categorical;
        }
        Borders.resize(poolMetaInfo.FeatureCount);
        NanModes.resize(poolMetaInfo.FeatureCount);

        for (size_t i = 0; i < BinarizedFeaturesMetaInfo.BinarizedFeatureIds.size(); ++i) {
            const size_t binarizedFeatureId = static_cast<const size_t>(BinarizedFeaturesMetaInfo.BinarizedFeatureIds[i]);
            const TVector<float>& borders = BinarizedFeaturesMetaInfo.Borders.at(i);
            CB_ENSURE(binarizedFeatureId < poolMetaInfo.FeatureCount, "Error: binarized feature " << binarizedFeatureId << " is out of range");
            FeatureTypes[binarizedFeatureId] = EFeatureValuesType::BinarizedFloat;
            NanModes[binarizedFeatureId] = BinarizedFeaturesMetaInfo.NanModes.at(i);
            Borders[binarizedFeatureId] = borders;
        }

        for (ui32 i = 0; i < poolMetaInfo.FeatureCount; ++i) {
            if (!IgnoreFeatures.has(i)) {
                ui32 bytesPerFeature = GetBytesPerFeature(i);
                FeatureBlobs[i].reserve(docCount * bytesPerFeature);
            }
        }

        DataProvider.CatFeatureIds = TSet<int>(catFeatureIds.begin(), catFeatureIds.end());

        // TODO(nikitxskv): Temporary solution until MLTOOLS-140 is implemented.
        DataProvider.PoolMetaInfo = poolMetaInfo;
    }
}

static NCatboostCuda::TBinarizedFloatFeaturesMetaInfo GetQuantizedFeatureMetaInfo(
    const NCB::TQuantizedPool& pool) {

    const auto columnIndexToFlatIndex = GetColumnIndexToFlatIndexMap(pool);
    const auto columnIndexToNumericFeatureIndex = GetColumnIndexToNumericFeatureIndexMap(pool);
    const auto numericFeatureCount = columnIndexToNumericFeatureIndex.size();

    NCatboostCuda::TBinarizedFloatFeaturesMetaInfo metainfo;

    metainfo.BinarizedFeatureIds.resize(numericFeatureCount);
    metainfo.Borders.resize(numericFeatureCount);
    metainfo.NanModes.resize(numericFeatureCount, ENanMode::Min);

    for (const auto [columnIndex, localIndex] : pool.ColumnIndexToLocalIndex) {
        if (pool.ColumnTypes[localIndex] != EColumn::Num) {
            continue;
        }

        const auto flatIndex = columnIndexToFlatIndex.at(columnIndex);
        const auto numericFeatureIndex = columnIndexToNumericFeatureIndex.at(columnIndex);
        metainfo.BinarizedFeatureIds[numericFeatureIndex] = flatIndex;

        const auto it = pool.QuantizationSchema.GetFeatureIndexToSchema().find(flatIndex);
        if (it != pool.QuantizationSchema.GetFeatureIndexToSchema().end()) {
            metainfo.Borders[numericFeatureIndex].assign(
                it->second.GetBorders().begin(),
                it->second.GetBorders().end());
            metainfo.NanModes[numericFeatureIndex] = NanModeFromProto(it->second.GetNanMode());
        }
    }

    return metainfo;
}

void NCatboostCuda::ReadPool(
    const ::NCB::TPathWithScheme& poolPath,
    const ::NCB::TPathWithScheme& pairsFilePath, // can be uninited
    const ::NCB::TPathWithScheme& groupWeightsFilePath, // can be uninited
    const NCatboostOptions::TDsvPoolFormatParams& dsvPoolFormatParams,
    const TVector<int>& ignoredFeatures,
    const bool verbose,
    NCB::TTargetConverter* const targetConverter,
    NPar::TLocalExecutor* const localExecutor,
    TDataProviderBuilder* const poolBuilder) {

    // TODO(nikitxskv): Temporary solution until MLTOOLS-140 is implemented.
    poolBuilder->SetPoolPathAndFormat(poolPath, dsvPoolFormatParams.Format);

    if (poolPath.Scheme != "quantized" && poolPath.Scheme != "yt-quantized") {
        ::NCB::ReadPool(
            poolPath,
            pairsFilePath,
            groupWeightsFilePath,
            dsvPoolFormatParams,
            ignoredFeatures,
            verbose,
            targetConverter,
            localExecutor,
            poolBuilder);
        return;
    }

    if (poolPath.Scheme == "yt-quantized") {
        ythrow TCatboostException() << "\"yt-quantized\" schema is not supported yet";
    }

    // TODO(yazevnul): load data in multiple threads. One thread reads from disk, other adds chunk
    // to the `poolBuilder`

    // TODO(yazevnul): load using `TFile::Pread` instead of mapping entire file; at least until we
    // keep this interface where we are not using chunks directly

    NCB::TLoadQuantizedPoolParameters loadParameters;
    loadParameters.LockMemory = false;
    loadParameters.Precharge = false;

    const auto pool = NCB::LoadQuantizedPool(poolPath.Path, loadParameters);
    const auto& poolMetaInfo = GetPoolMetaInfo(pool, groupWeightsFilePath.Inited());

    const auto columnIndexToFlatIndex = GetColumnIndexToFlatIndexMap(pool);
    poolBuilder->SetBinarizedFeaturesMetaInfo(GetQuantizedFeatureMetaInfo(pool));
    poolBuilder->AddIgnoredFeatures(GetIgnoredFlatIndices(pool));
    poolBuilder->SetFeatureIds(GetFlatFeatureNames(pool));
    poolBuilder->Start(
        poolMetaInfo,
        pool.DocumentCount,
        GetCategoricalFeatureIndices(pool));
    poolBuilder->StartNextBlock(pool.DocumentCount);

    size_t baselineIndex = 0;
    for (const auto& kv : pool.ColumnIndexToLocalIndex) {
        const auto columnIndex = kv.first;
        const auto localIndex = kv.second;
        const auto columnType = pool.ColumnTypes[localIndex];

        if (pool.Chunks[localIndex].empty()) {
            continue;
        }

        const auto flatIndex = columnIndexToFlatIndex.Value(columnIndex, 0);
        pool.AddColumn(flatIndex, baselineIndex, columnType, localIndex, poolBuilder);

        baselineIndex += static_cast<size_t>(columnType == EColumn::Baseline);
    }

    NCB::SetGroupWeights(groupWeightsFilePath, poolBuilder);
    NCB::SetPairs(pairsFilePath, poolMetaInfo.HasGroupWeight, poolBuilder);

    poolBuilder->Finish();
}
