#pragma once

#include "MatrixQuantizer.h"
#include "ColumnQuantizer.h"
#include "QuantizedMatrix.h"
#include "CPUMatrix.h"

#ifdef    _WIN32
#ifdef MATH_EXPORTS
#define MATH_API __declspec(dllexport)
#else
#define MATH_API __declspec(dllimport)
#endif
#else    // no DLLs on Linux
#define    MATH_API 
#endif

namespace Microsoft { namespace MSR { namespace CNTK {
    
    //see dbn::matrix quantizer
    template<class ElemType>
    class MatrixQuantizerCPU final : public MatrixQuantizer<ElemType>
    {
    public:    
        MatrixQuantizerCPU(size_t numRows, size_t numCols);
        
        // Disallow copy construction and assignment
        MatrixQuantizerCPU(const MatrixQuantizerCPU&) = delete;
        MatrixQuantizerCPU& operator=(const MatrixQuantizerCPU&) = delete;

        void QuantizeAsync(const Matrix<ElemType>& inMatrix, QuantizedMatrix<ElemType>& outQMatrix, bool zeroThresholdFor1Bit) override;
        void WaitQuantizeAsyncDone() override;

        void UnquantizeAsync(QuantizedMatrix<ElemType>& inQMatrix, Matrix<ElemType>& outMatrix, bool add = false) override;
        void WaitUnquantizeAsyncDone() override;
    };
}}}