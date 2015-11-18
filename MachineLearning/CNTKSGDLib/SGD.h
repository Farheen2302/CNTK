//
// <copyright file="SGD.h" company="Microsoft">
//     Copyright (c) Microsoft Corporation.  All rights reserved.
// </copyright>
//
#pragma once

#include "Basics.h"
#include "ComputationNetwork.h"
#include "NonlinearityNodes.h"          // for DropoutNode
#include "CompositeComputationNodes.h"  // for PrecomputeNode
#include "SimpleEvaluator.h"
#include "DataReader.h"
#include "IComputationNetBuilder.h"
#include <vector>
#include <string>
#include <stdexcept>
#include "fileutil.h"
#include "commandArgUtil.h"
#include <chrono> 
#include <random>
#include "TimerUtility.h"
#include "Profiler.h"

using namespace std;

namespace Microsoft { namespace MSR { namespace CNTK {

enum class LearningRateSearchAlgorithm : int
{
    None,
    AdjustAfterEpoch,
    SearchBeforeEpoch
};

enum class AdaptationRegType : int
{
    None,
    KL
};

enum class GradientsUpdateType : int
{
    None,
    AdaGrad,
    RmsProp,
    FSAdaGrad
};

// TODO: While currently combining these methods is not supported,
// these are not mutually exclusive and we can/should support combinations of these
// in the future
enum class ParallelizationMethod : int
{
    None = 0,
    DataParallelSGD = 1,
    ModelAveragingSGD = (1 << 1),
    ModelParallelSGD = (1 << 2), // Currently unsupported
};

// configuration parameters associated with RMSProp learning algorithm
struct RMSPropInfo
{
    double gamma;
    double inc;
    double dec;
    double max;
    double min;

    RMSPropInfo()
    {
        gamma = 0.99;
        inc = 1.2;
        dec = 0.75;
        max = 10.0;
        min = 0.1;
    }
};

struct GradientUpdateInfo
{
    GradientsUpdateType mType;
    float mGaussianNoiseInjectStd;

    GradientUpdateInfo()
    {
        mType = GradientsUpdateType::AdaGrad;
        mGaussianNoiseInjectStd = 0.0075f;
    }
};

struct SGDParams
{
    template<class ElemType>    // (needed for default value of m_gradientBits)
    SGDParams(const ConfigParameters& configSGD, size_t fullEpochsOffset, size_t fullTotalMaxEpochs, ElemType exampleElemType/*for type deduction*/);

    //SGDParams(SGDParams&&) = default; // (does not compile in VS 2013; not critical)

    //autoLearnRateSearchType is applied only if the learning rate for the epoch is not specified in learningRatesPerMB and learningRatesPerSample
    SGDParams(const floatargvector& learningRatesPerMB,
              const floatargvector& learningRatesPerSample,
              const intargvector& mbSize,
              bool truncated,
              const size_t fullEpochsOffset,
              const size_t fullTotalMaxEpochs,
              const size_t epochSize,
              const size_t maxEpochs,
              const wstring& modelPath,
              const floatargvector& momentumPerMB,
              const floatargvector& momentumPerSample,
              const bool gradientClippingWithTruncation,
              const double clippingThresholdPerSample,
              const LearningRateSearchAlgorithm autoLearnRateSearchType,
              const double increaseLearnRateIfImproveMoreThan,
              const double learnRateIncreaseFactor,
              const double reduceLearnRateIfImproveLessThan,
              const bool continueReduce,
              const double learnRateDecreaseFactor,
              floatargvector dropoutRates,
              const bool loadBestModel,
              const intargvector& numMiniBatch4LRSearch,
              const size_t numPrevLearnRates,
              const size_t numBestSearchEpoch,
              const int traceLevel,
              const bool progressTracing,
              const size_t numMBsToShowResult,
              const size_t numMBsToCUDAProfile,
              const size_t maxTempMemSizeInSamplesForCNN,
              const GradientUpdateInfo gradUpdateType,
              const bool keepCheckPointFiles,
              const AdaptationRegType adaptationRegType,
              const double adaptationRegWeight,
              const wstring trainCriterionNodeName,
              const wstring evalCriterionNodeName,
              const bool doGradientCheck,
              const double gradientCheckSigDigit,
              const bool validateAfterModelReloading,
              RMSPropInfo rpi,
              size_t learnRateAdjustInterval,
              const bool UsingAllDataForPreComputed,
              const bool needAveMultiplier,
              const double L2RegWeight,
              const double L1RegWeight,
              const bool autoAdjustMinibatch,
              const size_t minibatchSizeTuningFrequency,
              const size_t minibatchSizeTuningMax,
              const bool useCVSetControlLRIfCVExists,
              const bool useEvalCriterionControlLR,
              const size_t minibatchSearchCriterionErrorMargin,
              const double hsmoothingWeight,
              const double frameDropThresh,
              const bool doreferencealign);

protected:
    // learning rate per sample provided outside
    floatargvector m_learningRatesParam;
    intargvector m_learningRatesSpecifiedForMBSize;       // 1 for per sample, m_mbSize[] for per MB
    floatargvector m_momentumParam;
    intargvector m_momentumSpecifiedForMBSize;

    // Determine the MB size used for mapping a given learning-rate or momentum parameter to a per-sample value.
    // MB size is the number of samples across all time steps and parallel sequences.
    // This function exists to post-fix a design bug in SGD:
    // In the case of BPTT, the 'minibatchSize' parameter given to the SGD module really means the truncation size,
    // while the MB size to be used is (truncation size * number of parallel sequences).
    // SGD also does not know #parallel sequences upfront.
    size_t FixUpEffectiveMBSize(size_t specifiedMBSize, size_t numParallelSequences) const
    {
        // remedy the bug that truncation size is incorrectly passed as MB size
        if (m_truncated && specifiedMBSize > 1)         // currently only happens in this mode
            specifiedMBSize *= numParallelSequences;    // assume 'specifiedMBSize' refers to truncation size
        // end bug post-fix
        // TODO: This ^^ should go away once SGD gets fixed to take the truncation size as a parameter.

        return specifiedMBSize;
    }

    // helpers to convert learning rates to per-sample values used in the actual algorithms
    // 'numParallelSequences' must be specified because of the definitional MB-size bug in SGD mentioned above, and should go away once that is sorted out.
    double GetLearningRatePerSample(size_t epoch/*BUGBUG workaround:*/, size_t numParallelSequences) const
    {
        return m_learningRatesParam[epoch] / FixUpEffectiveMBSize(m_learningRatesSpecifiedForMBSize[epoch], numParallelSequences);
    }
    double GetMomentumPerSample(size_t epoch/*BUGBUG workaround:*/, size_t numParallelSequences) const
    {
        return pow(m_momentumParam[epoch], 1.0 / FixUpEffectiveMBSize(m_momentumSpecifiedForMBSize[epoch], numParallelSequences));
    }

    // only true when the user specify LearningRatePerMB and the number of parallel utterances in Reader > 1
    //bool m_needToNormalizeLRByParallUtterance;          // TODO: should go away
    //bool m_needToNormalizeMomentumByParallUtterance;

    intargvector m_mbSize;
    bool m_truncated;           // do BPTT
    // BUGBUG: The 'Truncated' option is duplicated in the reader and must be set to the same there (e.g. by defining in the config on an outer enclosing level, like current samples).
    //         We really should only read it in SGD and pass it ourselves on to the Reader, instead of it being a Reader parameter.
    // BUGBUG: If m_truncated, then m_mbSize is interpreted as truncation length; the actual MB size is a combination of that and the #parallel sequences specified in the reader.
    // TODO: do not specify 'Truncated' but 'TruncatedLength', set m_truncated so given, and let m_mbSize control how many #parallel sequences the reader is allowed to pack into an MB.

    // This includes the total epochs across all training commands
    size_t m_fullTotalMaxEpochs;
    size_t m_fullEpochsOffset;

    // the number of samples in each epoch (0 means, use all the samples in each epoch).
    size_t m_epochSize;
    size_t m_maxComputedEpochSize;

    // the total number of epochs to run.
    size_t m_maxEpochs;

    bool m_gradientClippingWithTruncation;
    double m_clippingThresholdPerSample;
    double m_lastFinishedEpochEvalErr;

    wstring m_modelPath;
    wstring m_trainCriterionNodeName;
    wstring m_evalCriterionNodeName;

    intargvector m_numMiniBatch4LRSearch;
    size_t m_numBestSearchEpoch;

    LearningRateSearchAlgorithm m_autoLearnRateSearchType;

    AdaptationRegType m_adaptationRegType;
    double m_adaptationRegWeight;
    bool m_needAdaptRegularization;
    bool m_progressTracing;
    Timer m_progressTracingTimer;

    bool m_loadBestModel;
    double m_reduceLearnRateIfImproveLessThan;
    bool m_continueReduce;

    // determine after how many epochs the learning rate should be auto adjusted.
    size_t m_learnRateAdjustInterval;

    bool m_useCVSetControlLRIfCVExists;
    bool m_useEvalCriterionControlLR;

    double m_increaseLearnRateIfImproveMoreThan;
    double m_learnRateIncreaseFactor;
    double m_learnRateDecreaseFactor;
    size_t m_prevChosenMinibatchSize;
    bool m_autoAdjustMinibatch;
    size_t m_minibatchSearchCriterionErrorMargin;
    size_t m_minibatchSizeTuningFrequency;
    size_t m_minibatchSizeTuningMax;

    floatargvector m_dropoutRates;
    size_t m_maxTempMemSizeInSamplesForCNN;

    int m_traceLevel;

    size_t m_numPrevLearnRates;

    double m_minLearnRate;

    GradientUpdateInfo m_gradType;
    RMSPropInfo m_rpi;

    bool m_keepCheckPointFiles;

    int m_numMBsToShowResult;
    int m_numMBsToCUDAProfile;

    bool m_doGradientCheck;
    double m_gradientCheckSigDigit;

    bool m_doUnitTest;

    bool m_validateAfterModelReloading;

    bool m_useAllDataForPreComputedNode;

    // Parallel training
    ParallelizationMethod m_parallelizationMethod;
    int m_numGradientBits;
    bool m_zeroThresholdFor1Bit;
    bool m_enableDistributedMBReading;
    int m_parallelizationStartEpochNum;

    // Parallel training related with MA 
    // decide how much information we want to show MA performance stats (seconds spend on sync, seconds since last sync etc.) ?  
    // 0: means no perfomance stats show
    // 1: means show stats every sync 
    // n>1: means show stats after every n sync
    int    m_iMASyncStatsTrace;
    size_t m_nFramesBetweenMASync;

    bool m_needAveMultiplier;
    double m_L2RegWeight;
    double m_L1RegWeight;

    //sequence trainning
    double m_hsmoothingWeight;
    double m_frameDropThresh;
    bool m_doreferencealign;
};

template<class ElemType> class IDistGradAggregator;

// TODO: make this independent of ElemType. Then these repeated dynamic_pointer_casts will go away
// TODO: why is this a class, and not just a procedure? Then we wouldn't have to include the massive header
template<class ElemType>
class SGD : public SGDParams
{
protected:
    typedef shared_ptr<ComputationNode<ElemType>> ComputationNodePtr;
    typedef ClassBasedCrossEntropyWithSoftmaxNode<ElemType>* ClassBasedCrossEntropyWithSoftmaxNodePtr;

public:
    SGD(const ConfigParameters& configSGD, size_t fullEpochsOffset, size_t fullTotalMaxEpochs);
    SGD(SGDParams&& sgdParams);

    void Adapt(wstring origModelFileName, wstring refNodeName,
               IDataReader<ElemType>* trainSetDataReader,
               IDataReader<ElemType>* validationSetDataReader,
               const DEVICEID_TYPE deviceID, const bool makeMode = true);
    void SequenceTrain(IComputationNetBuilder<ElemType>* netBuilder, wstring origModelFileName,
                       IDataReader<ElemType>* trainSetDataReader, IDataReader<ElemType>* validationSetDataReader,
                       const DEVICEID_TYPE deviceID, const bool makeMode = true);
    void Train(IComputationNetBuilder<ElemType>* netBuilder,
               IDataReader<ElemType>* trainSetDataReader,
               IDataReader<ElemType>* validationSetDataReader,
               const bool makeMode = true);

protected:
    std::vector<ComputationNodeBasePtr> & GetTrainCriterionNodes(ComputationNetwork& net);
    std::vector<ComputationNodeBasePtr> & GetEvalCriterionNodes(ComputationNetwork& net);

    void TrainOrAdaptModel(int startEpoch, ComputationNetwork& net,
                           ComputationNetwork& refNet,
                           ComputationNodeBasePtr refNode,
                           IDataReader<ElemType>* trainSetDataReader,
                           IDataReader<ElemType>* validationSetDataReader);

protected:
    // return true if precomputation is executed.
    bool PreCompute(ComputationNetwork& net,
                    IDataReader<ElemType>* trainSetDataReader,
                    std::vector<ComputationNodeBasePtr> & featureNodes,
                    std::vector<ComputationNodeBasePtr> & labelNodes,
                    std::map<std::wstring, Matrix<ElemType>*>* inputMatrices);

    // return a reasonable initial learning rate based on the initial mbsize
    double SearchForBestLearnRate(ComputationNetwork& net,
                                  ComputationNetwork& refNet,
                                  const ComputationNodeBasePtr& refNode, const int epochNumber,
                                  const double curLearnRate,
                                  IDataReader<ElemType>* trainSetDataReader,
                                  const std::vector<ComputationNodeBasePtr> & featureNodes,
                                  const std::vector<ComputationNodeBasePtr> & labelNodes,
                                  const std::vector<ComputationNodeBasePtr> & criterionNodes,
                                  const std::vector<ComputationNodeBasePtr> & evaluationNodes,
                                  std::map<std::wstring, Matrix<ElemType>*>* inputMatrices,
                                  const std::list<ComputationNodeBasePtr> & learnableNodes,
                                  std::list<Matrix<ElemType>>& smoothedGradients,
                                  const bool learnRateInitialized,
                                  const double largestPrevLearnRatePerSample);

    void TrainOneMiniEpochAndReloadModel(ComputationNetwork& net,
                                         ComputationNetwork& refNet,
                                         const ComputationNodeBasePtr& refNode, const int epochNumber,
                                         const size_t epochSize, IDataReader<ElemType>* trainSetDataReader,
                                         const double learnRatePerSample,
                                         const size_t minibatchSize,
                                         const std::vector<ComputationNodeBasePtr> & featureNodes,
                                         const std::vector<ComputationNodeBasePtr> & labelNodes,
                                         const std::vector<ComputationNodeBasePtr> & criterionNodes,
                                         const std::vector<ComputationNodeBasePtr> & evaluationNodes,
                                         std::map<std::wstring, Matrix<ElemType>*>* inputMatrices,
                                         const std::list<ComputationNodeBasePtr> & learnableNodes,
                                         std::list<Matrix<ElemType>>& smoothedGradients,
                                         /*out*/ double& epochCriterion,
                                         /*out*/ std::vector<double>& epochEvalErrors,
                                         /*out*/ size_t& totalSamplesSeen,
                                         std::string prefixMsg = "");

    size_t AdaptiveMinibatchSizing(ComputationNetwork& net,
                                   ComputationNetwork& refNet,
                                   const ComputationNodeBasePtr& refNode,
                                   const int epochNumber,
                                   const size_t numFramesToUseInSearch,
                                   IDataReader<ElemType>* trainSetDataReader,
                                   const double learnRatePerSample,
                                   const size_t initialMinibatchSize,
                                   const std::vector<ComputationNodeBasePtr> & featureNodes,
                                   const std::vector<ComputationNodeBasePtr> & labelNodes,
                                   const std::vector<ComputationNodeBasePtr> & criterionNodes,
                                   const std::vector<ComputationNodeBasePtr> & evaluationNodes,
                                   std::map<std::wstring, Matrix<ElemType>*>* inputMatrices,
                                   const std::list<ComputationNodeBasePtr> & learnableNodes,
                                   std::list<Matrix<ElemType>>& smoothedGradients,
                                   const double learningRateAdjustmentFactor);

    // uses a small percentage of training data of minibatch to
    // speculatively train with various MB sizes; then picks the best
    size_t SearchForBestMinibatchSize(ComputationNetwork& net,
                                      ComputationNetwork& refNet,
                                      const ComputationNodeBasePtr& refNode,
                                      const int epochNumber,
                                      const size_t numFramesToUseInSearch,
                                      IDataReader<ElemType>* trainSetDataReader,
                                      const double learnRatePerSample,
                                      const std::vector<ComputationNodeBasePtr> & featureNodes,
                                      const std::vector<ComputationNodeBasePtr> & labelNodes,
                                      const std::vector<ComputationNodeBasePtr> & criterionNodes,
                                      const std::vector<ComputationNodeBasePtr> & evaluationNodes,
                                      std::map<std::wstring, Matrix<ElemType>*>* inputMatrices,
                                      const std::list<ComputationNodeBasePtr> & learnableNodes,
                                      std::list<Matrix<ElemType>>& smoothedGradients,
                                      const size_t minMinibatchSize, const size_t maxMinibatchSize);

    //// Attemps to compute the error signal for the whole utterance, which will
    //// be fed to the neural network as features. Currently it is a workaround
    //// for the two-forward-pass sequence and ctc training, which allows
    //// processing more utterances at the same time. Only used in Kaldi2Reader.
    //// TODO: move the two-forward-pass support out of the reader.
    //void AttemptUtteranceDerivativeFeatures(ComputationNetwork& net,
    //                                        IDataReader<ElemType>* trainSetDataReader,
    //                                        const std::vector<ComputationNodeBasePtr> & featureNodes,
    //                                        std::map<std::wstring, Matrix<ElemType>*>* inputMatrices);

    size_t TrainOneEpoch(ComputationNetwork& net,
                         ComputationNetwork& refNet,
                         const ComputationNodeBasePtr& refNode,
                         const int epochNumber,
                         const size_t epochSize,
                         IDataReader<ElemType>* trainSetDataReader,
                         const double learnRatePerSample,
                         size_t tunedMBSize,
                         const std::vector<ComputationNodeBasePtr> & featureNodes,
                         const std::vector<ComputationNodeBasePtr> & labelNodes,
                         const std::vector<ComputationNodeBasePtr> & criterionNodes,
                         const std::vector<ComputationNodeBasePtr> & evaluationNodes,
                         std::map<std::wstring, Matrix<ElemType>*>* inputMatrices,
                         const std::list<ComputationNodeBasePtr> & learnableNodes,
                         std::list<Matrix<ElemType>>& smoothedGradients,
                         /*out*/ double& epochCriterion,
                         /*out*/ std::vector<double>& epochEvalErrors,
                         /*out*/ size_t& totalSamplesSeen,
                         std::string prefixMsg = "");

    void InitDistGradAgg(int numEvalNodes, int traceLevel);

    bool ModelAveragingProcessing(size_t nSamplesSinceLastSync, const std::list<ComputationNodeBasePtr>& learnableNodes, size_t& nProcessedFrames, 
                                  float& SecondsSinceLastSyncFinished, float& SecondsSpentOnSync);

    size_t ModelAveragingSync(int nSamplesSinceLastSync, const std::list<ComputationNodeBasePtr>& learnableNodes);

public:
    // UpdateWeightsS - static version of UpdateWeights()
    static void UpdateWeightsS(const SGD* sgd, Matrix<ElemType>& functionValues,
                               Matrix<ElemType>& gradientValues,
                               Matrix<ElemType>& smoothedGradient,
                               const double learnRatePerSample,
                               const double momentumPerSample,
                               size_t actualMBSize,
                               const double L2RegWeight,
                               const double L1RegWeight,
                               const bool needAveMultiplier);

protected:
    // UpdateWeights - update the weights in
    void UpdateWeights(const ComputationNodeBasePtr& node,
                       Matrix<ElemType>& smoothedGradient,
                       const double learnRatePerSample,
                       const double momentumPerSample,
                       const size_t actualMBSize,
                       const double L2RegWeight, const double L1RegWeight,
                       const bool needAveMultiplier) const;

    void ClipGradient(Matrix<ElemType>& gradient, const size_t actualMBSize) const;

    void SaveCheckPointInfo(const size_t epoch, const size_t totalSamplesSeen,
                            const double learnRatePerSample,
                            const std::list<Matrix<ElemType>>& smoothedGradients,
                            const double prevCriterion,
                            const size_t minibatchSize);

    bool LoadCheckPointInfo(const size_t epochNumber,
                            /*out*/ size_t& totalSamplesSeen,
                            /*out*/ double& learnRatePerSample,
                            std::list<Matrix<ElemType>>& smoothedGradients,
                            /*out*/ double& prevCriterion,
                            /*out*/ size_t& minibatchSize);

    wstring GetCheckPointFileNameForEpoch(const int epoch);
    wstring GetModelNameForEpoch(const int epoch, bool bLastModel = false);

    // return -1 if nothing exists
    int DetermineStartEpoch(const bool makeMode);

    GradientsUpdateType GradUpdateType() const { return m_gradType.mType; }
    double GradientUpdateNoiseStd() const { return m_gradType.mGaussianNoiseInjectStd; }

public:

#define EPSILON 1e-5

    bool GradientCheck(ComputationNetwork& net,
                       const std::vector<ComputationNodeBasePtr> & criterionNodes,
                       const std::list<ComputationNodeBasePtr> & learnableNodes,
                       int npos);

protected:

    IDistGradAggregator<ElemType>* m_distGradAgg;
    struct DistGradHeader* m_gradHeader;

private:
    int SGDTrace(FILE *__restrict __stream, const char *__restrict __format, ...);
};

}}}
