//
// <copyright file="TrainingCriterionNodes.h" company="Microsoft">
//     Copyright (c) Microsoft Corporation.  All rights reserved.
// </copyright>
//
#pragma once

#include <map>
#include <string>
#include <vector>
#include <stdexcept>
#include <list>
#include <memory>
#include "ComputationNode.h"
#include "InputAndParamNodes.h"
#include "gammacalculation.h"

namespace Microsoft { namespace MSR { namespace CNTK {

    // -----------------------------------------------------------------------
    /// SquareErrorNode (left, right)
    // -----------------------------------------------------------------------

    //note: to save computation the gradient may be scaled by an constant. 

    template<class ElemType>
    class SquareErrorNode : public ComputationNodeNonLooping/*ComputationNode*/<ElemType>, public NumInputs<2>
    {
        typedef ComputationNodeNonLooping<ElemType> Base; UsingComputationNodeMembersBoilerplate;
        static const std::wstring TypeName() { return L"SquareError"; }
    public:
        DeclareConstructorFromConfigWithNumInputs(SquareErrorNode);
        SquareErrorNode(DEVICEID_TYPE deviceId, const wstring & name) :
            Base(deviceId, name)
        { }

        virtual void BackpropToNonLooping(size_t inputIndex) override
        {
            FrameRange fr(Input(0)->GetMBLayout());
            auto gradient = Input(inputIndex)->GradientFor(fr);
            Matrix<ElemType>::Multiply1x1AndWeightedAdd(inputIndex == 0 ? 1.0f : -1.0f, Gradient()/*1x1*/, *m_leftMinusRight, 1.0f, gradient);
        }

        virtual void UpdateFunctionMBSize() override
        {
            m_leftMinusRight->Resize(Input(0)->GetNumRows(), Input(0)->GetNumCols());
        }

        virtual void /*ComputationNodeNonLooping::*/ForwardPropNonLooping() override
        {
            FrameRange fr(Input(0)->GetMBLayout());
            m_leftMinusRight->AssignDifferenceOf(Input(0)->ValueFor(fr), Input(1)->ValueFor(fr));
            MaskMissingColumnsToZero(*m_leftMinusRight, Input(0)->GetMBLayout(), fr);    // we are fine since it will only be called with full minibatch.
            ElemType v = m_leftMinusRight->FrobeniusNorm();
            VerifyDims(1,1);
            Value().SetValue(v*v / 2);
#if NANCHECK
            Value().HasNan("SquareError");
#endif
        }

        virtual void /*ComputationNodeBase::*/Validate(bool isFinalValidationPass) override
        {
            ValidateBinaryReduce(isFinalValidationPass);
        }

        virtual void CopyTo(ComputationNodeBasePtr nodeP, const std::wstring& newName, const CopyNodeFlags flags) const override
        {
            Base::CopyTo(nodeP, newName, flags);
            if (flags & CopyNodeFlags::copyNodeValue)
            {
                auto node = dynamic_pointer_cast<SquareErrorNode<ElemType>>(nodeP);
                *node->m_leftMinusRight = *m_leftMinusRight;
            }
        }

        //request matrices needed to do node function value evaluation
        virtual void RequestMatricesBeforeForwardProp(MatrixPool& matrixPool)
        {
            Base::RequestMatricesBeforeForwardProp(matrixPool);
            RequestMatrixFromPool(m_leftMinusRight, matrixPool);
        }

        //release gradient and temp matrices that no longer needed after all the children's gradients are computed.
        virtual void ReleaseMatricesAfterBackprop(MatrixPool& matrixPool)
        {
            Base::ReleaseMatricesAfterBackprop(matrixPool);
            ReleaseMatrixToPool(m_leftMinusRight, matrixPool);
        }

    private:
        shared_ptr<Matrix<ElemType>> m_leftMinusRight;
    };

    template class SquareErrorNode<float>; 
    template class SquareErrorNode<double>;

    // -----------------------------------------------------------------------
    // CrossEntropyWithSoftmaxNode (labels, prediction)
    // calculates: -sum(left_i * log(softmax_i(right)))
    // -----------------------------------------------------------------------

    template<class ElemType>
    class CrossEntropyWithSoftmaxNode : public ComputationNodeNonLooping/*ComputationNode*/<ElemType>, public NumInputs<2>
    {
        typedef ComputationNodeNonLooping<ElemType> Base; UsingComputationNodeMembersBoilerplate;
        static const std::wstring TypeName() { return L"CrossEntropyWithSoftmax"; }
    public:
        DeclareConstructorFromConfigWithNumInputs(CrossEntropyWithSoftmaxNode);
        CrossEntropyWithSoftmaxNode(DEVICEID_TYPE deviceId, const wstring & name) :
            Base(deviceId, name)
        { }

        virtual void BackpropToNonLooping(size_t inputIndex) override
        {
            FrameRange fr(Input(0)->GetMBLayout());
            // left input is scalar
            if (inputIndex == 0)  //left derivative
            {
#if DUMPOUTPUT
                *m_logSoftmaxOfRight.Print("CrossEntropyWithSoftmax Partial-logSoftmaxOfRight");
                Gradient().Print("CrossEntropyWithSoftmax Partial-gradientValues");
                Input(0)->GradientFor(fr).Print("CrossEntropyWithSoftmaxNode Partial-Left-in");
#endif

                auto gradient = Input(0)->GradientFor(fr);
                Matrix<ElemType>::Multiply1x1AndWeightedAdd(-1.0f, Gradient()/*1x1*/, *m_logSoftmaxOfRight, 1.0f, gradient);
#if DUMPOUTPUT
                Input(0)->GradientFor(fr).Print("CrossEntropyWithSoftmaxNode Partial-Left-out");
#endif

        }

            else if (inputIndex == 1)  // right derivative
        {
#if DUMPOUTPUT
                *m_softmaxOfRight.Print("CrossEntropyWithSoftmax Partial-softmaxOfRight");
                Input(0)->ValueFor(fr).Print("CrossEntropyWithSoftmax Partial-inputFunctionValues");
                Gradient().Print("CrossEntropyWithSoftmax Partial-gradientValues");
                Input(1)->GradientFor(fr).Print("CrossEntropyWithSoftmaxNode Partial-Right-in");
#endif

                auto gradient = Input(1)->GradientFor(fr);
                Matrix<ElemType>::AddScaledDifference(Gradient(), *m_softmaxOfRight, Input(0)->ValueFor(fr), gradient);
#if DUMPOUTPUT
                Input(1)->GradientFor(fr).Print("CrossEntropyWithSoftmaxNode Partial-Right");
#endif
#ifdef _DEBUG
                Input(1)->InvalidateMissingGradientColumns(fr);  // TODO: This should not be necessary.
#endif
            }
        }

        virtual void UpdateFunctionMBSize() override
        {
            m_logSoftmaxOfRight->Resize(Input(1)->GetNumRows(), Input(1)->GetNumCols());
            m_softmaxOfRight->Resize(m_logSoftmaxOfRight->GetNumRows(), m_logSoftmaxOfRight->GetNumCols());
        }

        virtual void /*ComputationNodeNonLooping::*/ForwardPropNonLooping() override   //-sum(left_i * log(softmax_i(right)))
        {
            FrameRange fr(Input(0)->GetMBLayout());
            // first compute the softmax (column-wise)
            // Note that we need both log and non-log for gradient computation.
            m_logSoftmaxOfRight->AssignLogSoftmaxOf(Input(1)->ValueFor(fr), true);
            m_softmaxOfRight->SetValue(*m_logSoftmaxOfRight);
            m_softmaxOfRight->InplaceExp();
            // flatten all gaps to zero, such that gaps will contribute zero to the sum
            MaskMissingColumnsToZero(*m_logSoftmaxOfRight, Input(1)->GetMBLayout(), fr);
            // reduce over all frames
            Value().AssignInnerProductOfMatrices(Input(0)->MaskedValueFor(fr), *m_logSoftmaxOfRight);
            Value() *= -1;
#if NANCHECK
            Value().HasNan("CrossEntropyWithSoftmax");
#endif
#if DUMPOUTPUT
            Value().Print("CrossEntropyWithSoftmaxNode");
#endif
        }

        virtual void /*ComputationNodeBase::*/Validate(bool isFinalValidationPass) override
        {
            ValidateBinaryReduce(isFinalValidationPass);
        }

        virtual void CopyTo(ComputationNodeBasePtr nodeP, const std::wstring& newName, const CopyNodeFlags flags) const override
        {
            Base::CopyTo(nodeP, newName, flags);
            if (flags & CopyNodeFlags::copyNodeValue)
            {
                auto node = dynamic_pointer_cast<CrossEntropyWithSoftmaxNode<ElemType>>(nodeP);
                *node->m_logSoftmaxOfRight = *m_logSoftmaxOfRight;
                *node->m_softmaxOfRight = *m_softmaxOfRight;
            }
        }

        //request matrices needed to do node function value evaluation
        virtual void RequestMatricesBeforeForwardProp(MatrixPool& matrixPool)
        {
            Base::RequestMatricesBeforeForwardProp(matrixPool);
            RequestMatrixFromPool(m_logSoftmaxOfRight, matrixPool);
            RequestMatrixFromPool(m_softmaxOfRight, matrixPool);
        }

    protected:
        shared_ptr<Matrix<ElemType>> m_logSoftmaxOfRight;
        shared_ptr<Matrix<ElemType>> m_softmaxOfRight;
    };

    template class CrossEntropyWithSoftmaxNode<float>; 
    template class CrossEntropyWithSoftmaxNode<double>;

    // -----------------------------------------------------------------------
    /// CrossEntropyNode (labels, prediction)
    // -----------------------------------------------------------------------

    // calculates: -sum(left_i * log(right_i))
    // assume softmax is already done
    // You probably want to use CrossEntropyWithSoftMaxNode instead, it is more efficient in most cases.
    template<class ElemType>
    class CrossEntropyNode : public ComputationNodeNonLooping/*ComputationNode*/<ElemType>, public NumInputs<2>
    {
        typedef ComputationNodeNonLooping<ElemType> Base; UsingComputationNodeMembersBoilerplate;
        static const std::wstring TypeName() { return L"CrossEntropy"; }
    public:
        DeclareConstructorFromConfigWithNumInputs(CrossEntropyNode);
        CrossEntropyNode(DEVICEID_TYPE deviceId, const wstring & name) :
            Base(deviceId, name)
        { }

        virtual void BackpropToNonLooping(size_t inputIndex) override
        {
            FrameRange fr(Input(0)->GetMBLayout());
            //left Node must be a scalar
            if (inputIndex == 0)  //left derivative
            {
                BackpropToLeft(*m_logOfRight, Input(0)->GradientFor(fr), Gradient());
            }
            else
            {
                BackpropToRight(*m_leftDivRight, Input(0)->ValueFor(fr), Input(1)->ValueFor(fr), Input(1)->GradientFor(fr), Gradient());
            }
        }

        /*TODO: merge with call site*/void BackpropToLeft(const Matrix<ElemType>& logOfRight, Matrix<ElemType> inputGradientValues, 
            const Matrix<ElemType>& gradientValues)  
        {
            Matrix<ElemType>::Multiply1x1AndWeightedAdd(-1.0f, gradientValues/*1x1*/, logOfRight, 1.0f, inputGradientValues);
        }

        /*TODO: merge with call site*/void BackpropToRight(Matrix<ElemType>& leftDivRight, 
            const Matrix<ElemType> inputFunctionValues0, const Matrix<ElemType> inputFunctionValues1,
            Matrix<ElemType> inputGradientValues, const Matrix<ElemType>& gradientValues)
        {
            FrameRange fr(Input(0)->GetMBLayout());
            leftDivRight.AssignElementDivisionOf(inputFunctionValues0, inputFunctionValues1);
            MaskMissingColumnsToZero(leftDivRight, Input(0)->GetMBLayout(), fr);
            Matrix<ElemType>::Multiply1x1AndWeightedAdd(-1.0f, gradientValues/*1x1*/, leftDivRight, 1.0f, inputGradientValues);
        }

        virtual void UpdateFunctionMBSize() override
        {
            m_logOfRight->Resize(Input(1)->GetNumRows(), Input(1)->GetNumCols());
            m_leftDivRight->Resize(Input(1)->GetNumRows(), Input(1)->GetNumCols());
        }

        //-sum(left_i * log(right_i))
        virtual void /*ComputationNodeNonLooping::*/ForwardPropNonLooping() override
        {
            FrameRange fr(Input(0)->GetMBLayout());
            m_logOfRight->SetValue(Input(1)->ValueFor(fr));
            m_logOfRight->InplaceLog();
            MaskMissingColumnsToZero(*m_logOfRight, Input(1)->GetMBLayout(), fr);
            Value().AssignInnerProductOfMatrices(Input(0)->MaskedValueFor(fr), *m_logOfRight);
            Value() *= -1;
#if NANCHECK
            functionValues.HasNan("CrossEntropy");
#endif
        }

        virtual void /*ComputationNodeBase::*/Validate(bool isFinalValidationPass) override
        {
            ValidateBinaryReduce(isFinalValidationPass);
            if (Input(0)->OperationName() != L"InputValue")    // TODO: but labels could be post-processed, e.g. sub-sampled. This test should not be here.
                LogicError("CrossEntropyNode criterion requires the first input to be the label.");
        }

        virtual void CopyTo(ComputationNodeBasePtr nodeP, const std::wstring& newName, const CopyNodeFlags flags) const override
        {
            Base::CopyTo(nodeP, newName, flags);
            if (flags & CopyNodeFlags::copyNodeValue)
            {
                auto node = dynamic_pointer_cast<CrossEntropyNode<ElemType>>(nodeP);
                *node->m_logOfRight = *m_logOfRight;
                *node->m_leftDivRight = *m_leftDivRight;
            }
        }

        //request matrices needed to do node function value evaluation
        virtual void RequestMatricesBeforeForwardProp(MatrixPool& matrixPool)
        {
            Base::RequestMatricesBeforeForwardProp(matrixPool);
            RequestMatrixFromPool(m_logOfRight, matrixPool);
        }

        //request matrices that are needed for gradient computation
        virtual void RequestMatricesBeforeBackprop(MatrixPool& matrixPool)
        {
            Base::RequestMatricesBeforeBackprop(matrixPool);
            RequestMatrixFromPool(m_leftDivRight, matrixPool);
        }

        //release gradient and temp matrices that no longer needed after all the children's gradients are computed.
        virtual void ReleaseMatricesAfterBackprop(MatrixPool& matrixPool)
        {
            Base::ReleaseMatricesAfterBackprop(matrixPool);
            ReleaseMatrixToPool(m_logOfRight, matrixPool);
            ReleaseMatrixToPool(m_leftDivRight, matrixPool);
        }

    private:
        // matrix value passed from evaluate to computePartial
        shared_ptr<Matrix<ElemType>> m_logOfRight;
        // temporary
        shared_ptr<Matrix<ElemType>> m_leftDivRight;
    };

    template class CrossEntropyNode<float>; 
    template class CrossEntropyNode<double>;

    // -----------------------------------------------------------------------
    // MatrixL1RegNode (input)
    // TODO: share most code with MatrixL2RegNode
    // -----------------------------------------------------------------------

    template<class ElemType>
    class MatrixL1RegNode : public ComputationNodeNonLooping/*ComputationNode*/<ElemType>, public NumInputs<1>
    {
        typedef ComputationNodeNonLooping<ElemType> Base; UsingComputationNodeMembersBoilerplate;
        static const std::wstring TypeName() { return L"MatrixL1Reg"; }
    public:
        DeclareConstructorFromConfigWithNumInputs(MatrixL1RegNode);
        MatrixL1RegNode(DEVICEID_TYPE deviceId, const wstring & name) :
            Base(deviceId, name)
        { }

        virtual void BackpropToNonLooping(size_t inputIndex) override // scale by number of cols (or samples)
        {
            FrameRange fr(Input(0)->GetMBLayout());
            assert(inputIndex == 0); inputIndex;
            BackpropToS(*m_gradientOfL1Norm, Input(0)->GradientFor(fr), Gradient(), Input(0)->ValueFor(fr));
        }

        /*TODO: merge with call site*/void BackpropToS(Matrix<ElemType>& gradientOfL1Norm, 
            Matrix<ElemType> inputGradientValues, const Matrix<ElemType>& gradientValues, const Matrix<ElemType>& inputFunctionValues)  
        {
            gradientOfL1Norm.AssignSignOf(inputFunctionValues);
            Matrix<ElemType>::Multiply1x1AndWeightedAdd(+1.0f, gradientValues/*1x1*/, gradientOfL1Norm, 1.0f, inputGradientValues);
        }

        virtual void UpdateFunctionMBSize() override
        {
            m_gradientOfL1Norm->Resize(Input(0)->GetNumRows(), Input(0)->GetNumCols());
        }

        virtual void /*ComputationNodeNonLooping::*/ForwardPropNonLooping() override  
        {
            FrameRange fr(Input(0)->GetMBLayout());
            VerifyDims(1, 1);
            Value().SetValue(Input(0)->MaskedValueFor(fr).MatrixNorm1());
#if NANCHECK
            Value().HasNan("MatrixL1Reg");
#endif
        }

        virtual void /*ComputationNodeBase::*/Validate(bool isFinalValidationPass) override
        {
            ValidateUnaryReduce(isFinalValidationPass);
        }

        virtual void CopyTo(ComputationNodeBasePtr nodeP, const std::wstring& newName, const CopyNodeFlags flags) const override
        {
            Base::CopyTo(nodeP, newName, flags);
            if (flags & CopyNodeFlags::copyNodeValue)
            {
                auto node = dynamic_pointer_cast<MatrixL1RegNode<ElemType>>(nodeP);
                *node->m_gradientOfL1Norm = *m_gradientOfL1Norm;
            }
        }

        //request matrices that are needed for gradient computation
        virtual void RequestMatricesBeforeBackprop(MatrixPool& matrixPool)
        {
            Base::RequestMatricesBeforeBackprop(matrixPool);
            RequestMatrixFromPool(m_gradientOfL1Norm, matrixPool);
        }

        //release gradient and temp matrices that no longer needed after all the children's gradients are computed.
        virtual void ReleaseMatricesAfterBackprop(MatrixPool& matrixPool)
        {
            Base::ReleaseMatricesAfterBackprop(matrixPool);
            ReleaseMatrixToPool(m_gradientOfL1Norm, matrixPool);
        }

    private:
        shared_ptr<Matrix<ElemType>> m_gradientOfL1Norm;    // temporary
    };

    template class MatrixL1RegNode<float>; 
    template class MatrixL1RegNode<double>;

    // -----------------------------------------------------------------------
    // MatrixL2RegNode (input)
    // TODO: share most code with MatrixL1RegNode
    // -----------------------------------------------------------------------

    template<class ElemType>
    class MatrixL2RegNode : public ComputationNodeNonLooping/*ComputationNode*/<ElemType>, public NumInputs<1>
    {
        typedef ComputationNodeNonLooping<ElemType> Base; UsingComputationNodeMembersBoilerplate;
        static const std::wstring TypeName() { return L"MatrixL2Reg"; }
    public:
        DeclareConstructorFromConfigWithNumInputs(MatrixL2RegNode);
        MatrixL2RegNode(DEVICEID_TYPE deviceId, const wstring & name) :
            Base(deviceId, name)
        { }

        virtual void BackpropToNonLooping(size_t inputIndex) override // scale by number of cols (or samples)
        {
            FrameRange fr(Input(0)->GetMBLayout());
            assert(inputIndex == 0); inputIndex;
            BackpropToS(Input(0)->GradientFor(fr), Gradient(), Input(0)->ValueFor(fr), Value());
        }

        /*TODO: merge with call site*/void BackpropToS(Matrix<ElemType> inputGradientValues, const Matrix<ElemType>& gradientValues, const Matrix<ElemType>& inputFunctionValues, const Matrix<ElemType>& functionValues)  
        {
            ElemType v = gradientValues.Get00Element() / (functionValues.Get00Element() + EPS_IN_INVERSE);  // TODO: GPU inefficiency
            inputGradientValues.AddWithScaleOf(v, inputFunctionValues);
        }

        virtual void /*ComputationNodeNonLooping::*/ForwardPropNonLooping() override  
        {
            FrameRange fr(Input(0)->GetMBLayout());
            VerifyDims(1,1);
            Value().SetValue(Input(0)->MaskedValueFor(fr).FrobeniusNorm());
#if NANCHECK
            Value().HasNan("MatrixL2Reg");
#endif
        }

        virtual void /*ComputationNodeBase::*/Validate(bool isFinalValidationPass) override
        {
            ValidateUnaryReduce(isFinalValidationPass);
        }
    };

    template class MatrixL2RegNode<float>; 
    template class MatrixL2RegNode<double>;

    // -----------------------------------------------------------------------
    /// NoiseContrastiveEstimationNode (labels, input, inputWeights, biasWeights)
    // BUGBUG: This node has not been converted to memshare conventions.
    // -----------------------------------------------------------------------

    enum NCEEvalMode
    {
        Softmax = 0,
        Unnormalized = 1,
        None = 2
    };
    template<class ElemType>
    class NoiseContrastiveEstimationNode : public ComputationNodeNonLooping/*ComputationNode*/<ElemType>, public NumInputs<4>
    {
        typedef ComputationNodeNonLooping<ElemType> Base; UsingComputationNodeMembersBoilerplate;
        static const std::wstring TypeName() { return L"NCEBasedCrossEntropyWithSoftmax"; }
    public:
        DeclareConstructorFromConfigWithNumInputs(NoiseContrastiveEstimationNode);
        NoiseContrastiveEstimationNode(DEVICEID_TYPE deviceId, const wstring & name) :
            Base(deviceId, name),
            m_logSoftmax(deviceId),
            m_softMax(deviceId), m_grdToSoftMaxInput(deviceId), m_ncePrediction(deviceId),
            m_evalMode(NCEEvalMode::None)
        { }
        NoiseContrastiveEstimationNode(DEVICEID_TYPE deviceId, const wstring & name, NCEEvalMode xm_evalMode) :
            Base(deviceId, name),
            m_logSoftmax(deviceId),
            m_softMax(deviceId), m_grdToSoftMaxInput(deviceId), m_ncePrediction(deviceId),
            m_evalMode(xm_evalMode)
        { }
        // ^^ TODO: we can merge these two

        virtual void Save(File& fstream) const override
        {
            Base::Save(fstream);
            fstream << m_evalMode;
        }

        virtual void Load(File& fstream, size_t modelVersion) override
        {
            Base::Load(fstream, modelVersion);
            fstream >> m_evalMode;
            if (m_evalMode > NCEEvalMode::None)
            {
                m_evalMode = NCEEvalMode::None;
                fstream.SetPosition(fstream.GetPosition() - sizeof(m_evalMode));
            }
        }

        void SetEvalMode(NCEEvalMode& xevMode) { m_evalMode = xevMode; }
        NCEEvalMode & EvalMode() { return m_evalMode; } // TODO: really? Return a reference to a local? TODO: change to const? and call it GetEvalMode()

        /**
        compute gradients to input observations, the weights to the observations, and the class log posterior probabilities
        */
        virtual void BackpropToNonLooping(size_t inputIndex) override
        {
            FrameRange fr(Input(0)->GetMBLayout());
            m_needRecomputeGradientToSoftmaxInput = false;
            //gradient computation@yinggongzhao
            //inputIndex should be 2 this time
            if (m_evalMode != NCEEvalMode::None)
                LogicError("BackpropTo should only be called in training mode");
            if (inputIndex == 0)
                InvalidArgument("ComputeInput partial should not be called for label");
            //                                                                              samples+probs                   hidden                  embedding
            //Input(inputIndex)->GradientFor(fr).AssignNCEDerivative(m_ncePrediction, Input(0)->ValueFor(fr), Input(1)->ValueFor(fr), Input(2)->Value(), inputIndex);
			if (inputIndex >= 2)
				Input(inputIndex)->Gradient().AssignNCEDerivative(m_ncePrediction, Input(0)->ValueFor(fr), Input(1)->ValueFor(fr), Input(2)->Value(), inputIndex);
			else
			    Input(inputIndex)->GradientFor(fr).AssignNCEDerivative(m_ncePrediction, Input(0)->ValueFor(fr), Input(1)->ValueFor(fr), Input(2)->Value(), inputIndex);
        }

#if 0   // TODO: delete this. Seems copy-paste leftover?
        /*TODO: merge with call site*/void BackpropToRight(const Matrix<ElemType>& inputFunctionValues, Matrix<ElemType>& inputGradientValues, const Matrix<ElemType>& gradientValues)
        {
            Matrix<ElemType>::MultiplyAndAdd(inputFunctionValues, false, gradientValues, true, inputGradientValues);
        }

        /*TODO: merge with call site*/void BackpropToLeft(const Matrix<ElemType>& obs, Matrix<ElemType>& inputGradientValues, const Matrix<ElemType>& gradientValues)
        {
            Matrix<ElemType>::MultiplyAndAdd(obs, false, gradientValues, false, inputGradientValues);
        }

        static void WINAPI ComputeCEPartialToSoftmaxInputs(Matrix<ElemType>& inputGradientValues, Matrix<ElemType>& gradientValues, size_t y_t)
        {
            Matrix<ElemType>::MinusOneAt(inputGradientValues, y_t);
            Matrix<ElemType>::Scale(gradientValues, inputGradientValues);
        }
#endif

        virtual void UpdateFunctionMBSize() override
        {
            // TODO (this does not really break it since for full matrices, class Matrix will resize by itself)
        }

        virtual void /*ComputationNodeNonLooping::*/ForwardPropNonLooping() override   //-sum(left_i * log(softmax_i(right)))
        {
            FrameRange fr(Input(0)->GetMBLayout());
            if (Input(0)->HasMBLayout() && Input(0)->GetMBLayout()->HasGaps())
                LogicError("%ls %ls operation does not handle multiple parallel sequences with gaps correctly. Contact fseide@microsoft.com if you have a need and a test case.", NodeName().c_str(), OperationName().c_str());
            //Input(0)->MaskMissingValueColumnsToZero(fr);
            int positive = 0, negative = 0;
            if (Input(0)->GetNumRows() == 1)
            {
                for (int i = 0; i < Input(0)->GetNumCols(); i++)   // BUGBUG: Loops must be over frames, not columns. Columns may contain gaps.
                {
                    if (Input(0)->Value()(0, i) > 0)
                        positive++;
                    else if (Input(0)->Value()(0, i) < 0)
                        negative++;
                }
                assert(positive * negative == 0);
            }
            if (m_evalMode == NCEEvalMode::Softmax || (Input(0)->GetNumRows() == 1 && positive > 0))
            {
                // evaluation uses softmax
                m_logSoftmax.AssignProductOf(Input(1)->Value(), true, Input(2)->Value(), false);
                m_logSoftmax += Input(3)->Value();
                m_logSoftmax.InplaceLogSoftmax(false);
                MaskMissingColumnsToZero(m_logSoftmax, Input(1)->GetMBLayout(), fr);  // TODO: is this the right way to neutralize gaps?
                Value().AssignSoftmaxSum(Input(0)->Value(), m_logSoftmax);
            }
            else if (m_evalMode == NCEEvalMode::Unnormalized || (Input(0)->GetNumRows() == 1 && negative > 0))
            {
                // TODO: are we treating gaps correctly here?
                Value().AssignNceUnnormalizedEval(Input(0)->Value(), Input(1)->Value(), Input(2)->Value(), Input(3)->Value());
            }
            else
            {
                // TODO: are we treating gaps correctly here?
                // training criterion uses NCE
                //likelihood                                         samples+probs                        hidden                       embedding            bias
                Value().AssignNoiseContrastiveEstimation(Input(0)->Value(), Input(1)->Value(), Input(2)->Value(), Input(3)->Value(), m_ncePrediction);
            }
            m_needRecomputeGradientToSoftmaxInput = true;
        }

        /**
        Inputs: [0] label in dense matrix in [4 x T]
        the first row is the word index, the second row is the class index, the third row is the first word index of the class
        the last row is the first word index of the next class
        [1] hidden layer activity to the node in [hdsize x T]. for a simple rnn, this is the hidden layer activty
        [2] weight matrix in [hdsize x vocab_size], for speed-up, as per word matrix can be simply obtained as column slice
        [3] clsprob in dense matrix in [nbr_cls x T]. this is the output from logsoftmax node for the log-posterior probabilty of class given observations
        */
        virtual void /*ComputationNodeBase::*/Validate(bool isFinalValidationPass) override
        {
            Base::Validate(isFinalValidationPass);
            m_pMBLayout = nullptr;    // this node does not hold mini-batch data

            if (Input(0)->OperationName() != OperationNameOf(InputValue))
                LogicError("NoiseContrastiveEstimationNode criterion requires the first input to be the label.");
            if (isFinalValidationPass)
            {
                if (!(Input(1)->GetNumRows() == Input(2)->GetNumRows())) // input and matrix can be timed
                    LogicError("The Matrix dimension for observation and weight in the NoiseContrastiveEstimationNode operation does not match.");
                if (!(Input(0)->GetNumCols() == Input(1)->GetNumCols())) // label and input same obs numbers
                    LogicError("The Matrix dimension for label and observation in the NoiseContrastiveEstimationNode operation does not match.");
            }

            //cerr << Input(3)->GetNumCols() << "\t" << Input(0)->GetNumCols() << endl;
            SetDims(TensorShape(1), 1);
        }

    protected:
        Matrix<ElemType> m_logSoftmax;
        Matrix<ElemType> m_softMax;
        Matrix<ElemType> m_ncePrediction;

        // gradient of cross entropy with respect to the input of softmax
        // a 1 row by \sum_t m_nbrWordsInEachTime[t] vector
        // one slice of size m_nbrWordsInEachTime[t] saves the input to softmax for word y_t
        Matrix<ElemType> m_grdToSoftMaxInput;
        bool m_needRecomputeGradientToSoftmaxInput;

        size_t m_nbrNoise;
        size_t           m_totalNbrWords;
    private:
        NCEEvalMode m_evalMode;
    };
    template class NoiseContrastiveEstimationNode<float>;
    template class NoiseContrastiveEstimationNode<double>;

    // -----------------------------------------------------------------------
    /// ClassBasedCrossEntropyWithSoftmaxNode (labels(.,t), input(.,t), inputweights, clsProbBeforeSoftmax(.,t))
    // Inputs:
    // Input(0) [4 x T] label in dense matrix in
    //           (0,t) the first row is the word index
    //           (1,t) the second row is the class index
    //           (2,t) the third row is the first word index of the class
    //           (3,t) the last row is the first word index of the next class
    // Input(1) [hdsize x T] hidden layer activation to the node in. for a simple rnn, this is the hidden layer activty
    // Input(2) [hdsize x vocab_size] weight matrix in, for speed-up, as per word matrix can be simply obtained as column slice
    // Input(3) [nbr_cls x T] clsprob in dense matrix in. this input, if applied softmax on, is the posterior probabilty of class given observations
    // -----------------------------------------------------------------------

    // calculates: -sum(left_i * log(softmax_i(right))) for class given history and for word given history
    // need to provide class probabilty from external node
    template<class ElemType>
    class ClassBasedCrossEntropyWithSoftmaxNode : public ComputationNodeNonLooping/*ComputationNode*/<ElemType>, public NumInputs<4>
    {
        typedef ComputationNodeNonLooping<ElemType> Base; UsingComputationNodeMembersBoilerplate;
        static const std::wstring TypeName() { return L"ClassBasedCrossEntropyWithSoftmax"; }
    public:
        DeclareConstructorFromConfigWithNumInputs(ClassBasedCrossEntropyWithSoftmaxNode);
        ClassBasedCrossEntropyWithSoftmaxNode(DEVICEID_TYPE deviceId, const wstring & name) :
            Base(deviceId, name),
            m_logSoftmax(deviceId), m_softMax(deviceId), m_grdToSoftMaxInput(deviceId), m_clsLogSoftmax(deviceId), m_clsSoftmax(deviceId)
        { }

        /**
        compute gradients to input observations, the weights to the observations, and the class log posterior probabilites
        */
        virtual void BackpropToNonLooping(size_t inputIndex) override
        {
            // this should never be called for input[0], which is controlled through the needGradient flag
            if (inputIndex != 1 && inputIndex != 2 && inputIndex != 3)
                InvalidArgument("ClassCrossEntropyWithSoftmaxNode criterion only takes with respect to input, weight to the input and class log posterior probability.");

            ComputeSoftMaxPartial();

            Matrix<ElemType> grd_t;
            Matrix<ElemType> grd_to_wgt_t;

            const size_t nT = Input(0)->GetNumTimeSteps();
            const size_t nS = Input(0)->GetNumParallelSequences();
            size_t sz = 0;     // iterate over the packed concatenated class-conditioned prob vectors
            for (size_t s = 0; s < nS; s++) for (size_t t = 0; t < nT; t++)
            {
                FrameRange fr = FrameRange(Input(0)->GetMBLayout(), t).Sequence(s);

                //if (Input(0)->GetMBLayout()->IsGap(s, t))  // skip gaps        --TODO: use FrameRange version of Is()
                if (Input(0)->GetMBLayout()->IsGap(fr))     // skip gaps
                    continue;

                Matrix<ElemType> lbl_t = Input(0)->ValueFor(fr);
                size_t c_t = (size_t)lbl_t(1, 0);
                size_t lft_bnd = (size_t)lbl_t(2, 0); // index of first word belonging to current word token's class
                size_t rgt_bnd = (size_t)lbl_t(3, 0); // and end of that range
                size_t nbr_wrd = (rgt_bnd - lft_bnd); // number of words in the class

                // compute prb - 1 and prb
                Matrix<ElemType> weightForClass = Input(2)->Value().ColumnSlice(lft_bnd, nbr_wrd);
                Matrix<ElemType> obs = Input(1)->ValueFor(fr);   // hidden activation vector for current word token
                Matrix<ElemType> grd_to_soft_max_input = m_grdToSoftMaxInput.ColumnSlice(sz, nbr_wrd);
                Matrix<ElemType> grd_to_cls_prob = DataWithMBLayoutFor(m_clsLogSoftmax, fr, Input(3)->GetMBLayout());

                switch (inputIndex)
                {
                case 1:
                    // gradient to input
                    grd_t = Input(1)->GradientFor(fr);
                    Matrix<ElemType>::MultiplyAndAdd(weightForClass, false, grd_to_soft_max_input, true, grd_t);
                    break;
                case 2:
                    // gradient to input weight
                    grd_to_wgt_t = Input(2)->Gradient().ColumnSlice(lft_bnd, nbr_wrd);
                    Matrix<ElemType>::MultiplyAndAdd(obs, false, grd_to_soft_max_input, false, grd_to_wgt_t);
                    break;
                case 3:
                    grd_t = Input(3)->GradientFor(fr);
                    grd_t.SetValue(DataWithMBLayoutFor(m_clsSoftmax, fr, Input(3)->GetMBLayout()));
                    ComputeCEPartialToSoftmaxInputs(grd_t, Gradient(), c_t);
                    break;
                }

                sz += nbr_wrd;
            }
        }
    private:
        void ComputeCEPartialToSoftmaxInputs(Matrix<ElemType>& inputGradientValues, Matrix<ElemType>& gradientValues, size_t y_t)
        {
            Matrix<ElemType>::MinusOneAt(inputGradientValues, y_t);
            Matrix<ElemType>::Scale(gradientValues, inputGradientValues);
        }

        // gradient of cross entropy w.r.t. to input to softmax
        void ComputeSoftMaxPartial()
        {
            if (m_needRecomputeGradientToSoftmaxInput)
            {
                m_grdToSoftMaxInput.Resize(1, m_totalNbrWords); // buffer that contains a concatenation of class-conditional values

                const size_t nT = Input(0)->GetNumTimeSteps();
                const size_t nS = Input(0)->GetNumParallelSequences();
                size_t sz = 0;     // iterate over the packed concatenated class-conditioned prob vectors
                for (size_t s = 0; s < nS; s++) for (size_t t = 0; t < nT; t++)
                {
                    FrameRange fr = FrameRange(Input(0)->GetMBLayout(), t).Sequence(s);

                    //if (Input(0)->GetMBLayout()->IsGap(s, t))  // skip gaps
                    if (Input(0)->GetMBLayout()->IsGap(fr))  // skip gaps
                        continue;

                    Matrix<ElemType> lbl_t = Input(0)->ValueFor(fr);
                    size_t y_t = (size_t)lbl_t(0, 0);       // word index
                    size_t lft_bnd = (size_t)lbl_t(2, 0);   // index of first word belonging to current word token's class
                    size_t rgt_bnd = (size_t)lbl_t(3, 0);   // and end of that range
                    size_t nbr_wrd = (rgt_bnd - lft_bnd);   // number of words in the class

                    Matrix<ElemType> softMax = m_softMax.ColumnSlice(sz, nbr_wrd);

                    size_t idx_in_class = y_t - lft_bnd;
                    ComputeCEPartialToSoftmaxInputs(softMax, Gradient(), idx_in_class);

                    m_grdToSoftMaxInput.ColumnSlice(sz, nbr_wrd).SetValue(softMax);

                    sz += nbr_wrd;
                }

                m_needRecomputeGradientToSoftmaxInput = false;
            }
        }
    public:

        virtual void UpdateFunctionMBSize() override
        {
            // TODO (this does not really break it since for full matrices, class Matrix will resize by itself)
        }

        // -sum(left_i * log(softmax_i(right)))
        virtual void /*ComputationNodeNonLooping::*/ForwardPropNonLooping() override
        {
            if (Input(0)->Value().GetDeviceId() != CPUDEVICE)
                LogicError("ClassBasedCrossEntropyWithSoftmax (ForwardPropNonLooping()): The label matrix is not using CPU device. This will make computation slow, even though the label data is probably saved on GPU. Because of the external loop over time with explicit class id retrieved from the label matrix, the computation will be very slow if the label matrix is saved on GPU. However, this is only a constraint for label matrix and other matrices such as data are suggested to reside on GPU. ");

            // (the below is left-over from refactoring)
            Matrix<ElemType>& functionValues = Value();
            
            const size_t hdSize = Input(1)->GetNumRows();    // hdSize
            assert(m_nbrCls == Input(3)->GetNumRows());

            // compute the class posteriors
            m_clsLogSoftmax = Input(3)->Value();
            m_clsLogSoftmax.InplaceLogSoftmax(true);        // log
            m_clsSoftmax.AssignExpOf(m_clsLogSoftmax);      // non-log

            // create a large workspace to contain all class-conditioned probs concatenated
            // 'sz' is the offset into that vector. We will iterate over these vectors at a few places. Always use this same boilerplate code.
            // TODO: should we pull this iteration into an iterator, to reduce the code dup?
            const size_t nT = Input(0)->GetNumTimeSteps();
            const size_t nS = Input(0)->GetNumParallelSequences();
            size_t sz = 0;
            for (size_t s = 0; s < nS; s++) for (size_t t = 0; t < nT; t++)
            {
                FrameRange fr = FrameRange(Input(0)->GetMBLayout(), t).Sequence(s);
                //if (Input(0)->GetMBLayout()->IsGap(s, t))  // skip gaps
                if (Input(0)->GetMBLayout()->IsGap(fr))  // skip gaps
                    continue;

                const Matrix<ElemType> & lbl_t = Input(0)->ValueFor(fr);
                size_t lft_bnd = (size_t)lbl_t(2, 0);
                size_t rgt_bnd = (size_t)lbl_t(3, 0);
                size_t nbr_wrd = (rgt_bnd - lft_bnd);   // number of words in the class
                if (nbr_wrd == 0)
                    LogicError("ClassBasedCrossEntropyWithSoftmax (ForwardPropNonLooping()): Encountered a class of size 0. This sample seems to lack an NoInput flag.");

                sz += nbr_wrd;
            }
            m_totalNbrWords = sz;   // total size of concatenated vector

            // buffer to hold the concatenated class-conditioned prob vectors
            m_softMax.Resize(1, sz);
            m_logSoftmax.Resize(1, sz);

            // accumulate objective
            functionValues.SetValue(0);
            sz = 0;     // iterate over the packed concatenated class-conditioned prob vectors
            for (size_t s = 0; s < nS; s++) for (size_t t = 0; t < nT; t++)
            {
                FrameRange fr = FrameRange(Input(0)->GetMBLayout(), t).Sequence(s);
                //if (Input(0)->GetMBLayout()->IsGap(s, t))  // skip gaps
                if (Input(0)->GetMBLayout()->IsGap(fr))  // skip gaps
                    continue;

                const Matrix<ElemType> & lbl_t = Input(0)->ValueFor(fr);
                size_t y_t = (size_t)lbl_t(0, 0);     // current word token index
                size_t c_t = (size_t)lbl_t(1, 0);     // current word token's class index
                size_t lft_bnd = (size_t)lbl_t(2, 0); // index of first word belonging to current word token's class
                size_t rgt_bnd = (size_t)lbl_t(3, 0); // and end of that range
                size_t nbr_wrd = (rgt_bnd - lft_bnd);   // number of words in the class

                // now get views of various arrays that correspond to the index range of words belonging to this class

                // get hidden vectors for the words in this class
                Matrix<ElemType> weightForClass = Input(2)->Value().ColumnSlice(lft_bnd, nbr_wrd);    // [hdSize x nbr_wrd]

                // buffer to hold the class-conditional distribution
                Matrix<ElemType> softMax_t    =    m_softMax.ColumnSlice(sz, nbr_wrd);
                Matrix<ElemType> logSoftMax_t = m_logSoftmax.ColumnSlice(sz, nbr_wrd);

                Matrix<ElemType> obs = Input(1)->ValueFor(fr);   // hidden activation vector for current word token

                // multiply hidden activation with weight matrix (the slice of the weight matrix for the range of class members)
                // TODO: can we use 'true' here instead? Above transposition hack won't work with row slices. 'obs' not used elsewhere
                obs.Reshape(1, hdSize);  // transpose it (make it a column vector)
                logSoftMax_t.AssignProductOf(obs/*(1 x hdSize)*/, false, weightForClass/*hdSize x nbr_wrd*/, false);  // -> 1 x nbr_word

                // log softmax(W x_t)
                logSoftMax_t.InplaceLogSoftmax(false);

                // and non-log version
                softMax_t.SetValue(logSoftMax_t);
                softMax_t.InplaceExp();
                // we now have a column vector of class-conditional probabilities over the class members

                // add  the word's class-conditional log posterior
                if (y_t < lft_bnd || y_t >= rgt_bnd)
                    LogicError("ClassBasedCrossEntropyWithSoftmax (ForwardPropNonLooping()): Word index out of bounds of class-member index range (word not a class member).");
                size_t idx_in_class = y_t - lft_bnd;
                Matrix<ElemType>::AddElementToElement(logSoftMax_t, 0, idx_in_class, functionValues, 0, 0);   // (1x1)

                // add the class log posterior probability
                Matrix<ElemType>::AddElementToElement(m_clsLogSoftmax, c_t, t, functionValues, 0, 0);     // (1x1)

                sz += nbr_wrd;
            }

            functionValues *= (-1);

#if NANCHECK
            functionValues.HasNan("ClassBasedCrossEntropyWithSoftmax");
#endif
            m_needRecomputeGradientToSoftmaxInput = true;
        }


        virtual void /*ComputationNodeBase::*/Validate(bool isFinalValidationPass) override
        {
            Base::Validate(isFinalValidationPass);
            m_pMBLayout = nullptr;    // this node does not hold mini-batch data

            if (Input(0)->OperationName() != OperationNameOf(InputValue))  // TODO: but why could that label not be post-processed through another node?
                LogicError("ClassBasedCrossEntropyWithSoftmaxNode criterion requires the first input to be the label.");
            if (isFinalValidationPass)
            {
                if (Input(0)->GetNumRows() != 4) // label needs to be 4 rows
                    LogicError("The label in the ClassBasedCrossEntropyWithSoftmaxNode operation needs to be 4 rows.");
                if (Input(1)->GetNumRows() != Input(2)->GetNumRows()) // input and matrix can be timed
                    LogicError("The Matrix<ElemType>  dimension for observation and weight in the ClassBasedCrossEntropyWithSoftmaxNode operation does not match.");
                if (Input(0)->GetMBLayout() != Input(1)->GetMBLayout() || Input(0)->GetMBLayout() != Input(3)->GetMBLayout())
                    InvalidArgument("%ls %ls operation requires that the layouts of inputs 0 (label), 1 (hidden activation), and 3 (log softmax) match.", NodeName().c_str(), OperationName().c_str());
            }

            SetDims(TensorShape(1), 1);

            m_nbrCls = Input(3)->GetNumRows();
        }

    protected:
        Matrix<ElemType> m_logSoftmax;
        Matrix<ElemType> m_softMax;

        Matrix<ElemType> m_clsLogSoftmax;
        Matrix<ElemType> m_clsSoftmax;

        /// gradient of cross entropy with respect to the input of softmax
        /// a 1 row by \sum_t m_nbrWordsInEachTime[t] vector
        /// one slice of size m_nbrWordsInEachTime[t] saves the input to softmax for word y_t
        Matrix<ElemType> m_grdToSoftMaxInput;
        bool m_needRecomputeGradientToSoftmaxInput;

        size_t           m_nbrCls;
        size_t           m_totalNbrWords;
    };

    template class ClassBasedCrossEntropyWithSoftmaxNode<float>;
    template class ClassBasedCrossEntropyWithSoftmaxNode<double>;

    // -----------------------------------------------------------------------
    // CRFNode (labels, position_dependent_scores, transition_scores)
    //  - labels : output label vector of [0:T-1]
    //  - position_dependent_scores [?] : score from position dependent node,
    //    in the R-CRF case, it is the RNN output score before softmax
    //  - transition scores [?] : score from the transition node, 
    //    in the R-CRF case, it is the transition probability between labels
    // BUGBUG: This node cannot operate with truncated BPTT, but does not detect it. It also does not handle gaps or test boundary flags.
    // -----------------------------------------------------------------------

    /**
        CRF training criterion 
        It uses forward-backward algorithm within a minibatch to compute statistics for sequence level optimization 
        This node can serve a base class for other sequence level optimization

        Developed by Kaisheng Yao
        This node is for replicating results of the following work
        K. Yao, B. Peng, G. Zweig, D. Yu, X. Li and F. Gao, "Recurrent Conditional Random Fields", NIPS Deep Learning Workshop 2014
        K. Yao, B. Peng, G. Zweig, D. Yu, X. Li and F. Gao, "Recurrent Conditional Random Fields for Language Understanding", ICASSP 2014 
        http://research.microsoft.com/pubs/210167/rcrf_v9.pdf

        The forward-backward algorithm follows the derivation in 
        http://jmlr.org/papers/volume12/collobert11a/collobert11a.pdf

    */
    template<class ElemType>
    class CRFNode : public ComputationNodeNonLooping/*ComputationNode*/<ElemType>, public NumInputs<3>
    {
        typedef ComputationNodeNonLooping<ElemType> Base; UsingComputationNodeMembersBoilerplate;
        static const std::wstring TypeName() { return L"CRF"; }
    public:
        DeclareConstructorFromConfigWithNumInputs(CRFNode);
        CRFNode(DEVICEID_TYPE deviceId, const wstring & name) :
            Base(deviceId, name),
            mAlpha(deviceId), mBeta(deviceId), mPostProb(deviceId)
        { }

        /// compute posterior probability of label y at position t
        virtual void /*ComputationNodeNonLooping::*/ForwardPropNonLooping() override
        {
            FrameRange fr(Input(0)->GetMBLayout());
            size_t nrow = Input(0)->GetNumRows();
            size_t ncol = Input(0)->GetNumCols();

            mAlpha.Resize(nrow, ncol);
            mBeta.Resize(nrow, ncol);
            mPostProb.Resize(nrow, ncol);

            Value().SetValue(0.0);
            Matrix<ElemType> funcVal = Value();    // TODO: This just creates a 1x1 matrix set to 0.

            size_t nS = Input(0)->GetNumParallelSequences();
            if (nS != 1)
                LogicError("CRFNode: >1 parallel sequences are curently not implemented correctly.");
            for (size_t i = 0; i < nS; i++)     // process parallel sequences one by one  --BUGBUG: We should loop over individual sequences.
            {
                FrameRange sequenceRange = fr.Sequence(i);    // FrameRange to select one sequence
                // BUGBUG: This ^^ is neither supported nor correct, since this code does not handle gaps or start/end flags
                ForwardPropS(
                    DataWithMBLayoutFor(mPostProb, sequenceRange, Input(0)->GetMBLayout()),
                    DataWithMBLayoutFor(mAlpha,    sequenceRange, Input(0)->GetMBLayout()),
                    DataWithMBLayoutFor(mBeta,     sequenceRange, Input(0)->GetMBLayout()),
                    funcVal,
                    Input(0)->ValueFor(sequenceRange),
                    Input(1)->ValueFor(sequenceRange),
                    Input(2)->Value(), mStartLbl,
                    mEndLbl);

                Value() += funcVal;    // aggregate over sequences
            }
        }

        virtual void BackpropToNonLooping(size_t inputIndex) override  //scaled by 2*number of colmns (samples) in the Matrix<ElemType>
        {
            FrameRange fr(Input(0)->GetMBLayout());
            // inputIndex 0 should not get us here, it should be prevented by the needGradient flag of input[0]
            if (inputIndex != 1 && inputIndex != 2)
                InvalidArgument("CRFNode only takes with respect to input and weight.");

            if (inputIndex == 1)
            {
                auto gradient = Input(1)->GradientFor(fr);
                Matrix<ElemType>::AddScaledDifference(Gradient(), mPostProb, Input(0)->ValueFor(fr), gradient);
            }
            else if (inputIndex == 2)
            {
                assert(Input(inputIndex)->GradientFor(fr).GetNumElements() > 0);
                size_t nS = Input(0)->GetNumParallelSequences();
                for (size_t i = 0; i < nS; i++)         // process all sequences one by one
                {
                    FrameRange sequenceRange = fr.Sequence(i);    // FrameRange to select one sequence
                    auto gradient = Input(2)->GradientFor(fr);
                    TransGrdCompute(Input(0)->ValueFor(sequenceRange),
                                    DataWithMBLayoutFor(mAlpha, sequenceRange, Input(0)->GetMBLayout()),
                                    DataWithMBLayoutFor(mBeta,  sequenceRange, Input(0)->GetMBLayout()),
                                    Input(2)->ValueFor(fr),
                                    gradient,
                        mStartLbl, 1);
                }
            }
            else
                return;
        }

        // compute forward backward algorithm
        /*TODO: merge with call site*/void ForwardPropS(Matrix<ElemType> postprob, Matrix<ElemType> alpha, Matrix<ElemType> beta, Matrix<ElemType> & functionValues, const Matrix<ElemType> & lbls, const Matrix<ElemType> & pos_scores, const Matrix<ElemType> & pair_scores, int& firstLbl, int& lastLbl, const int iStep = 1)
        {
            /// to-do, each slice is for one sentence
            /// to-do, number of slices correspond to number of frames 
            /// this implementation only supports one sentence per minibatch

            int nObs = lbls.GetNumCols();

            /// change to other values so can support multiple sentences in each minibatch
            assert(iStep == 1);
            ForwardCompute(alpha, lbls, pos_scores, pair_scores);
            BackwardCompute(alpha, beta, functionValues, lbls, pos_scores, pair_scores, iStep);
            PostProbCompute(postprob, alpha, beta);

            firstLbl = -1;
            for (int ik = 0; ik < lbls.GetNumRows(); ik++)
            if (lbls(ik, 0) != 0)
            {
                firstLbl = ik; break;
            }

            lastLbl = -1;
            for (int ik = 0; ik < lbls.GetNumRows(); ik++)
            if (lbls(ik, nObs - 1) != 0)
            {
                lastLbl = ik; break;
            }

            functionValues.AssignInnerProductOfMatrices(lbls, pos_scores);

            Matrix<ElemType> a = alpha.ColumnSlice(nObs - 1, 1);
            ElemType fAlpha;
            fAlpha = a.LogAddSumOfElements();

            /// transition score
            ElemType tscore = 0;
            for (int t = 0; t < nObs - 1; t++){
                int i = -1;
                for (int ik = 0; ik < lbls.GetNumRows(); ik++)
                if (lbls(ik, t) != 0){
                    i = ik; break;
                }
                int j = -1;
                for (int ik = 0; ik < lbls.GetNumRows(); ik++)
                if (lbls(ik, t + 1) != 0){
                    j = ik; break;
                }
                tscore += pair_scores(j, i);
            }
            tscore += functionValues.Get00Element();  /// correct path score
            tscore -= fAlpha;  /// reduced by the scores from all paths
            functionValues.SetValue(tscore);

            functionValues *= (-1);
        }

        /// compute forward backward algorithm
        static void ForwardCompute(Matrix<ElemType>& alpha,
            const Matrix<ElemType>& lbls,
            const Matrix<ElemType>& pos_scores, const Matrix<ElemType>& pair_scores)
        {
            /// to-do, shift more than 1 to support muliple sentences per minibatch
            int iNumPos = lbls.GetNumCols();
            int iNumLab = lbls.GetNumRows();

            int firstLbl = -1;
            for (int ik = 0; ik < lbls.GetNumRows(); ik++)
            if (lbls(ik, 0) != 0){
                firstLbl = ik; break;
            }

            /// need to have 
            alpha.Resize(iNumLab, iNumPos);

            for (int t = 0; t < iNumPos; t++)
            {
                for (int k = 0; k < iNumLab; k++)
                {
                    ElemType fTmp = (ElemType)LZERO;
                    for (int j = 0; j < iNumLab; j++)
                    {
                        ElemType fAlpha = (j == firstLbl) ? (ElemType) 0.0 : (ElemType)LZERO;
                        if (t > 0)
                            fAlpha = alpha(j, t - 1);
                        fTmp = alpha.LogAdd(fTmp, fAlpha + pair_scores(k, j));
                    }
                    fTmp += pos_scores(k, t);  /// include position dependent score
                    alpha(k, t) = fTmp;
                }
            }
        }

        /// compute backward algorithm
        static void BackwardCompute( const Matrix<ElemType>& alpha, Matrix<ElemType>& beta,
            Matrix<ElemType>& functionValues, const Matrix<ElemType>& lbls,
            const Matrix<ElemType>& pos_scores, const Matrix<ElemType>& pair_scores, const int shift = 1)
        {
            assert(shift == 1);

            alpha.RCRFBackwardCompute(alpha, beta, functionValues, lbls, pos_scores, pair_scores, shift);
        }

        static void TransGrdCompute(const Matrix<ElemType>& lbls,
            const Matrix<ElemType>&   alpha,
            const Matrix<ElemType>& beta,
            const Matrix<ElemType>& pair_scores,
            Matrix<ElemType>& grd,
            const int startLbl,
            const int shift = 1)
        {
            assert(shift == 1);

            alpha.RCRFTransGrdCompute(lbls,
                alpha,
                beta,
                pair_scores,
                grd,
                startLbl, shift);
        }

        /// compute forward backward algorithm
        static void PostProbCompute(Matrix<ElemType>& postprob, const Matrix<ElemType>& alpha, const Matrix<ElemType>& beta)
        {
            int iNumPos = alpha.GetNumCols();
            int iNumLab = alpha.GetNumRows();

            postprob.Resize(iNumLab, iNumPos);
            postprob.SetValue(beta);
            postprob.InplaceExp();
        }

        virtual void /*ComputationNodeBase::*/Validate(bool isFinalValidationPass) override
        {
            Base::Validate(isFinalValidationPass);
            m_pMBLayout = nullptr;    // this node does not hold mini-batch data

            if (isFinalValidationPass)
                if (!(Input(1)->GetNumRows() == Input(2)->GetNumRows() &&  // position dependent and pair scores have same number of labels
                    Input(0)->GetNumRows() == Input(1)->GetNumRows() &&
                    Input(0)->GetNumCols() == Input(1)->GetNumCols() && // position dependent and pair scores have the same observation numbers
                    Input(2)->GetNumCols() == Input(2)->GetNumRows()))
            {
                LogicError("The Matrix dimension in the CRFNode operation does not match.");
            }

            SetDims(TensorShape(1), 1);
        }

        virtual void CopyTo(ComputationNodeBasePtr nodeP, const std::wstring& newName, const CopyNodeFlags flags) const override
        {
            Base::CopyTo(nodeP, newName, flags);
            if (flags & CopyNodeFlags::copyNodeValue)
            {
                auto node = dynamic_pointer_cast<CRFNode<ElemType>>(nodeP);
                node->mAlpha = mAlpha;
                node->mBeta= mBeta;
                node->mPostProb = mPostProb;

                node->mStartLbl = mStartLbl;
                node->mEndLbl = mEndLbl;
            }
        }

    private:
        Matrix<ElemType> mAlpha;    // TODO: m_Alpha etc.
        Matrix<ElemType> mBeta;
        Matrix<ElemType> mPostProb;
        int mStartLbl;
        int mEndLbl;
    };

    // -----------------------------------------------------------------------
    /// SequenceWithSoftmaxNode (label, prediction, loglikelihood)
    // word-lattice based sequence training criterion
    // BUGBUG: Likely not very useful since it uses an MS-proprietary lattice-archive format
    //         that requires Frank's DBN.exe tool to create. The inner C++ code for conversion
    //         is in this repo (latticearchive.h), but not the outer main program.
    // -----------------------------------------------------------------------

    template<class ElemType>
    class SequenceWithSoftmaxNode : public ComputationNodeNonLooping<ElemType>, public NumInputs<3>
    {
        typedef ComputationNodeNonLooping<ElemType> Base; UsingComputationNodeMembersBoilerplate;
        static const std::wstring TypeName() { return L"SequenceWithSoftmax"; }
    public:
        DeclareConstructorFromConfigWithNumInputs(SequenceWithSoftmaxNode);
        SequenceWithSoftmaxNode(DEVICEID_TYPE deviceId, const wstring & name) :
            Base(deviceId, name), m_gammaCalcInitialized(false)
        {
        }
        
        //compute gradients to input observations, the weights to the observations, and the class log posterior probabilites
        virtual void BackpropToNonLooping(size_t inputIndex) override
        {
            //auto t_start_time = Timer::MilliSecondElapsed();
            //left Node must be a scalar
            if (inputIndex == 0)  //left derivative
            {
                BackpropToLeft(*m_logSoftmaxOfRight, Input(inputIndex)->Gradient(), Gradient());
            }
            else if (inputIndex == 1)
            {
				FrameRange fr(Input(0)->GetMBLayout());
				BackpropToRight(*m_softmaxOfRight, Input(0)->Value(), Input(inputIndex)->Gradient(),
					Gradient(), *m_gammaFromLattice, m_fsSmoothingWeight, m_frameDropThreshold);
				MaskMissingColumnsToZero(Input(inputIndex)->Gradient(), Input(0)->GetMBLayout(), fr);
                
#ifdef _DEBUG
                Input(inputIndex)->InvalidateMissingGradientColumns(FrameRange(Input(inputIndex)->GetMBLayout()));
#endif
            }
            else if (inputIndex == 2)
            {
#if 1           // no gradient flows to log LLs (but otherwise we leave it to user if, e.g., another node propagates a gradient into there)
                ;   // gradient does not flow here
#else
                Input(inputIndex)->SetParameterUpdateRequired(false);
                Input(inputIndex)->Gradient().SetValue(0.0);
#endif
            }
            else
                RuntimeError("SequenceWithSoftmaxNode criterion only takes with respect to label, DNN output and log likelihood.");
        }

        static void WINAPI BackpropToLeft(const Matrix<ElemType>& logSoftmaxOfRight, Matrix<ElemType>& inputGradientValues, const Matrix<ElemType>& gradientValues)
        {
#if DUMPOUTPUT
            logSoftmaxOfRight.Print("SequenceWithSoftmaxNode Partial-logSoftmaxOfRight");
            gradientValues.Print("SequenceWithSoftmaxNode Partial-gradientValues");
            inputGradientValues.Print("SequenceWithSoftmaxNode Partial-Left-in");
#endif

            Matrix<ElemType>::Multiply1x1AndWeightedAdd(-1.0f, gradientValues/*1x1*/, logSoftmaxOfRight, 1.0f, inputGradientValues);
#if DUMPOUTPUT
            inputGradientValues.Print("SequenceWithSoftmaxNode Partial-Left-out");
#endif
        }

        static void WINAPI BackpropToRight(const Matrix<ElemType>& softmaxOfRight, const Matrix<ElemType>& inputFunctionValues,
                                                    Matrix<ElemType>& inputGradientValues, const Matrix<ElemType>& gradientValues,
                                                    const Matrix<ElemType> & gammaFromLattice, double hsmoothingWeight, double frameDropThresh)
        {
#if DUMPOUTPUT
            softmaxOfRight.Print("SequenceWithSoftmaxNode Partial-softmaxOfRight");
            inputFunctionValues.Print("SequenceWithSoftmaxNode Partial-inputFunctionValues");
            gradientValues.Print("SequenceWithSoftmaxNode Partial-gradientValues");
            inputGradientValues.Print("SequenceWithSoftmaxNode Partial-Right-in");
#endif  
            
            inputGradientValues.AssignSequenceError((ElemType)hsmoothingWeight, inputFunctionValues, softmaxOfRight, gammaFromLattice, gradientValues.Get00Element());            
            inputGradientValues.DropFrame(inputFunctionValues, gammaFromLattice, (ElemType)frameDropThresh);
#if DUMPOUTPUT
            inputGradientValues.Print("SequenceWithSoftmaxNode Partial-Right");
#endif
        }

        // -sum(left_i * log(softmax_i(right)))
        virtual void ForwardPropNonLooping()
        {
            // Initialize m_gammaCalculator
            // TODO: Would this lend itself to a unique_ptr instead of the init flag?
            if (!m_gammaCalcInitialized)
            {
                if (m_hmm.hmms.size() == 0)
                {
                    LogicError("SequenceWithSoftmaxNode criterion evaluation requires HMM states to be set.");
                }
                m_gammaCalculator.init(m_hmm, m_deviceId);
                m_gammaCalcInitialized = true;
            }
            //softmax
            m_logSoftmaxOfRight->AssignLogSoftmaxOf(Input(1)->Value()/*prediction*/, true);
            m_softmaxOfRight->SetValue(*m_logSoftmaxOfRight);
            m_softmaxOfRight->InplaceExp();

            m_gammaFromLattice->SwitchToMatrixType(m_softmaxOfRight->GetMatrixType(), m_softmaxOfRight->GetFormat(), false);
            m_gammaFromLattice->Resize(m_softmaxOfRight->GetNumRows(), m_softmaxOfRight->GetNumCols());
            m_gammaCalculator.calgammaformb(Value(), m_lattices, Input(2)->Value()/*log LLs*/,
                                            Input(0)->Value()/*labels*/, *m_gammaFromLattice,
                                            m_uids, m_boundaries, Input(1)->GetNumParallelSequences(),
                                            Input(0)->GetMBLayout(), m_extraUttMap, m_doReferenceAlignment);
            
#if NANCHECK
            Value().HasNan("SequenceWithSoftmaxNode");
#endif
#if DUMPOUTPUT
            Value().Print("SequenceWithSoftmaxNode");
#endif
        }

        virtual void /*ComputationNodeBase::*/Validate(bool isFinalValidationPass) override
        {
            Base::Validate(isFinalValidationPass);
            m_pMBLayout = nullptr;  // no layout

            if (Input(0)->OperationName() != L"InputValue" && Input(0)->OperationName() != L"SparseInputValue")
                LogicError("SequenceWithSoftmaxNode criterion requires the first input to be the label.");

            if (isFinalValidationPass)
                if (!(Input(0)->GetNumRows() == Input(1)->GetNumRows() &&  //match size
                    Input(1)->GetNumRows() == Input(2)->GetNumRows() &&
                    Input(0)->GetNumCols() == Input(1)->GetNumCols() &&
                    Input(1)->GetNumCols() == Input(2)->GetNumCols()))
            {
                    LogicError("The Matrix dimension in the SequenceWithSoftmaxNode operation does not match.");
            }

            SetDims(TensorShape(1), 1);

            m_gammatime = 0;
            m_partialtime = 0;
        }

        virtual void CopyTo(ComputationNodeBasePtr nodeP, const std::wstring& newName, const CopyNodeFlags flags) const override
        {
            Base::CopyTo(nodeP, newName, flags);
            
            if (flags & CopyNodeFlags::copyNodeValue)
            {
                auto node = dynamic_pointer_cast<SequenceWithSoftmaxNode<ElemType>>(nodeP);

                *node->m_logSoftmaxOfRight = *m_logSoftmaxOfRight;
                *node->m_softmaxOfRight = *m_softmaxOfRight;
                *node->m_gammaFromLattice = *m_gammaFromLattice;
                node->m_fsSmoothingWeight = m_fsSmoothingWeight;
                node->m_frameDropThreshold = m_frameDropThreshold;
                node->m_doReferenceAlignment = m_doReferenceAlignment;
            }
        }

        //request matrices needed to do node function value evaluation
        virtual void RequestMatricesBeforeForwardProp(MatrixPool& matrixPool)
        {
            Base::RequestMatricesBeforeForwardProp(matrixPool);
            RequestMatrixFromPool(m_logSoftmaxOfRight, matrixPool);
            RequestMatrixFromPool(m_softmaxOfRight, matrixPool);
            RequestMatrixFromPool(m_gammaFromLattice, matrixPool);
        }

		//request matrices needed to do node function value evaluation
		virtual void ReleaseMatricesAfterBackprop(MatrixPool& matrixPool)
		{
			Base::ReleaseMatricesAfterForwardProp(matrixPool);
			ReleaseMatrixToPool(m_logSoftmaxOfRight, matrixPool);
			ReleaseMatrixToPool(m_softmaxOfRight, matrixPool);
			ReleaseMatrixToPool(m_gammaFromLattice, matrixPool);
		}

        // TODO: method names should be CamelCase
        std::vector<shared_ptr<const msra::dbn::latticepair>> * getLatticePtr()
        {
            return &m_lattices;
        }

        std::vector<size_t> * getuidprt()
        {
            return &m_uids;
        }

        std::vector<size_t> * getboundaryprt()
        {
            return &m_boundaries;
        }
        std::vector<size_t> * getextrauttmap()
        {
            return &m_extraUttMap;
        }
        msra::asr::simplesenonehmm *gethmm()
        {
            return &m_hmm;
        }

        void SetSmoothWeight(double fsSmoothingWeight)
        {
            m_fsSmoothingWeight = fsSmoothingWeight;
        }
        void SetFrameDropThresh(double frameDropThresh)
        {
            m_frameDropThreshold = frameDropThresh;
        }

        void SetReferenceAlign(const bool doreferencealign)
        {
            m_doReferenceAlignment = doreferencealign;
        }

        void gettime(unsigned long long &gammatime, unsigned long long &partialtime)
        {
            gammatime = m_gammatime;
            partialtime = m_partialtime;
        }

    protected:
        shared_ptr<Matrix<ElemType>> m_logSoftmaxOfRight;
        shared_ptr<Matrix<ElemType>> m_softmaxOfRight;
        shared_ptr<Matrix<ElemType>> m_gammaFromLattice;
        double m_frameDropThreshold;
        double m_fsSmoothingWeight;         // frame-sequence criterion interpolation weight    --TODO: can this be done outside?
        bool m_doReferenceAlignment;
        std::vector<shared_ptr<const msra::dbn::latticepair>> m_lattices;
        msra::asr::simplesenonehmm m_hmm;
        msra::lattices::GammaCalculation<ElemType> m_gammaCalculator;
        bool m_gammaCalcInitialized;
        std::vector<size_t> m_uids;
        std::vector<size_t> m_boundaries;
        std::vector<size_t> m_extraUttMap;

        unsigned long long m_gammatime;     // TODO: what are these? Not even the context can be guessed from these names.
        unsigned long long m_partialtime;
    };

    template class SequenceWithSoftmaxNode<float>;
    template class SequenceWithSoftmaxNode<double>;

    // -----------------------------------------------------------------------
    // LogisticNode (labels, prediction, weight)
    // calculates: -sum(left * log(right) + (1-left)*log(1-right)) (optionally * weight)
    // -----------------------------------------------------------------------

    template<class ElemType>
    class LogisticNode : public ComputationNodeNonLooping/*ComputationNode*/<ElemType>
    {
        typedef ComputationNodeNonLooping<ElemType> Base; UsingComputationNodeMembersBoilerplate;
        static const std::wstring TypeName() { return L"Logistic"; }
    public:
        DeclareConstructorFromConfig(LogisticNode);
        LogisticNode(DEVICEID_TYPE deviceId, const wstring & name) :
            Base(deviceId, name)
        { }

        virtual void BackpropToNonLooping(size_t inputIndex) override
        {
            FrameRange fr(Input(0)->GetMBLayout());
            if (inputIndex != 1)
                InvalidArgument("%ls %ls operation cannot compute the gradient for its first inpute.", NodeName().c_str(), OperationName().c_str());

            //BackpropToRight(m_temp, Input(0)->Value(), Input(2)->Value(), Input(inputIndex)->Gradient(), Gradient(), m_classZeroLabels, m_result);
            // Create vector with 1 for class 1, and -1 for class 0
            m_temp->AssignDifferenceOf(Input(0)->ValueFor(fr), *m_classZeroLabels);  // TODO: need a slice for m_classZeroLabels?

            // Multiply the vector by the Input(2)->Value()
            if (m_inputs.size() == 3) // without weight
                m_temp->AssignElementProductOf(*m_temp, Input(2)->ValueFor(fr));     // TODO: is Input(2) minibatch data? Confirm

            // divide class by p (class 1) or (1-p) (class 0)
            m_temp->AssignElementDivisionOf(*m_temp, *m_result);            // TODO: this is in-place--does this function allow that?

            auto gradient = Input(inputIndex)->GradientFor(fr);
            Matrix<ElemType>::Multiply1x1AndWeightedAdd(-1.0f, Gradient()/*1x1*/, *m_temp, 1.0f, gradient);
        }

        virtual void UpdateFunctionMBSize() override
        {
            m_classZeroLabels->Resize(Input(0)->GetNumRows(), Input(0)->GetNumCols());
            m_result->Resize(Input(0)->GetNumRows(), Input(0)->GetNumCols());
            m_temp->Resize(Input(0)->GetNumRows(), Input(0)->GetNumCols());
        }

        //-sum(left * log(right) + (1-left)*log(1-right)) (optionally * weight)
        virtual void /*ComputationNodeNonLooping::*/ForwardPropNonLooping() override
        {
            FrameRange fr(Input(0)->GetMBLayout());
            
            const Matrix<ElemType>& classOneLabels        = Input(0)->ValueFor(fr);
            const Matrix<ElemType>& classOneProbabilities = Input(1)->ValueFor(fr);
            Matrix<ElemType>&       classZeroLabels       = *m_classZeroLabels;

            Matrix<ElemType> ones = ConstOnes(classOneLabels.GetNumRows(), classOneLabels.GetNumCols(), classOneLabels.GetDeviceId());

            // compute the indices for the class 0 indices
            classZeroLabels.AssignDifferenceOf(ones, classOneLabels);

            /* We're computing result = weight*(y*p + (1-y)*(1-p) = 2*y*p + (1-y) - p) */

            /* First compute result = y*p */
            m_result->AssignElementProductOf(classOneLabels, classOneProbabilities);

            // TODO: verify that all these operations on m_result really can do in-place (or use different methods instead)
            /* Now compute result = 2*y*p */
            m_result->AssignProductOf((ElemType)2.0, *m_result);

            /* Now compute result = 2*y*p + (1-y) */
            m_result->AssignSumOf(*m_result, classZeroLabels);

            /* Finally compute result = 2*y*p + (1-y) - p */
            m_result->AssignDifferenceOf(*m_result, classOneProbabilities);

            // compute the log, resulting in y*log(p) + (1-y)*log(1-p)
            m_temp->AssignLogOf(*m_result);

            // The error is the negative of the sum of the result
            if (m_inputs.size() == 2)
                Value().AssignSumOfElements(*m_temp);
            else
                Value().AssignInnerProductOf(Input(2)->ValueFor(fr), *m_temp, false);
            Value() *= (-1);
        }

        virtual void /*ComputationNodeBase::*/Validate(bool isFinalValidationPass) override
        {
            if (m_inputs.size() != 2 && m_inputs.size() != 3)
                InvalidArgument("%ls %ls operation requires two or three inputs.", NodeName().c_str(), OperationName().c_str());

            ValidateBinaryReduce(isFinalValidationPass);

            /* Note that this is the same as ValidateInferBinaryInputDims, but done for the 3rd child if it exists */
            if (m_inputs.size() == 3)
            {
                auto in = Input(2);
                auto other = Input(1);
                // borrow any unset dimension on one input from the other input
                size_t rows =                        in->GetNumRows() == 0 ? other->GetNumRows()/*borrow from peer*/ : in->GetNumRows()/*keep as is*/;
                size_t cols = (!in->HasMBLayout() && in->GetNumCols() == 0) ? other->GetNumCols()/*borrow from peer*/ : in->GetNumCols()/*keep as is*/;

                ValidateInferInputDims(2, rows, cols);

                if (isFinalValidationPass &&
                    !(Input(0)->GetNumRows() == Input(2)->GetNumRows() &&
                    (Input(0)->HasMBLayout() || (Input(0)->GetNumCols() == Input(2)->GetNumCols()))))
                {
                    LogicError("The Matrix dimensions of the second argument in the %ls %ls operation do not match.", NodeName().c_str(), OperationName().c_str());
                }
            }
        }

        //request matrices needed to do node function value evaluation
        virtual void RequestMatricesBeforeForwardProp(MatrixPool& matrixPool)
        {
            Base::RequestMatricesBeforeForwardProp(matrixPool);
            RequestMatrixFromPool(m_classZeroLabels, matrixPool);
            RequestMatrixFromPool(m_result, matrixPool);
            RequestMatrixFromPool(m_temp, matrixPool);
        }

        //release gradient and temp matrices that no longer needed after all the children's gradients are computed.
        virtual void ReleaseMatricesAfterBackprop(MatrixPool& matrixPool)
        {
            Base::ReleaseMatricesAfterBackprop(matrixPool);
            ReleaseMatrixToPool(m_classZeroLabels, matrixPool);
            ReleaseMatrixToPool(m_result, matrixPool);
            ReleaseMatrixToPool(m_temp, matrixPool);
        }

        virtual void CopyTo(const ComputationNodePtr nodeP, const std::wstring& newName, const CopyNodeFlags flags) const
        {
            Base::CopyTo(nodeP, newName, flags);
            if (flags & CopyNodeFlags::copyNodeValue)
            {
                auto node = dynamic_pointer_cast<LogisticNode<ElemType>>(nodeP);
                *node->m_classZeroLabels = *m_classZeroLabels;
                *node->m_result = *m_result;
                *node->m_temp = *m_temp;
            }
        }

    private:
        shared_ptr<Matrix<ElemType>> m_classZeroLabels;
        shared_ptr<Matrix<ElemType>> m_result;
        shared_ptr<Matrix<ElemType>> m_temp;
    };

    template class LogisticNode<float>;
    template class LogisticNode<double>;
}}}
