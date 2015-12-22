//
// <copyright file="Exports.cpp" company="Microsoft">
//     Copyright (c) Microsoft Corporation.  All rights reserved.
// </copyright>
//
// Exports.cpp : Defines the exported functions for the DLL application.
//

#include "stdafx.h"
#define DATAREADER_EXPORTS
#include "DataReader.h"
#include "LibSVMBinaryReader.h"

namespace Microsoft { namespace MSR { namespace CNTK {

template<class ElemType>
void DATAREADER_API GetReader(IDataReader<ElemType>** preader)
{
    *preader = new LibSVMBinaryReader<ElemType>();
}

extern "C" DATAREADER_API void GetReaderF(IDataReader<float>** preader)
{
    GetReader(preader);
}
extern "C" DATAREADER_API void GetReaderD(IDataReader<double>** preader)
{
    GetReader(preader);
}

}}}
