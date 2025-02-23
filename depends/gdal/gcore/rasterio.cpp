/******************************************************************************
 *
 * Project:  GDAL Core
 * Purpose:  Contains default implementation of GDALRasterBand::IRasterIO()
 *           and supporting functions of broader utility.
 * Author:   Frank Warmerdam, warmerdam@pobox.com
 *
 ******************************************************************************
 * Copyright (c) 1998, Frank Warmerdam
 * Copyright (c) 2007-2014, Even Rouault <even dot rouault at mines-paris dot org>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 ****************************************************************************/

#include "cpl_port.h"
#include "gdal.h"
#include "gdal_priv.h"

#include <climits>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <algorithm>
#include <limits>
#include <stdexcept>

#include "cpl_conv.h"
#include "cpl_cpu_features.h"
#include "cpl_error.h"
#include "cpl_progress.h"
#include "cpl_string.h"
#include "cpl_vsi.h"
#include "gdal_priv_templates.hpp"
#include "gdal_vrt.h"
#include "gdalwarper.h"
#include "memdataset.h"
#include "vrtdataset.h"

CPL_CVSID("$Id: rasterio.cpp 888775cfc3d1ac5e9725716e92d21cdac66ffe72 2019-12-30 02:46:39 +0100 Even Rouault $")

/************************************************************************/
/*                             IRasterIO()                              */
/*                                                                      */
/*      Default internal implementation of RasterIO() ... utilizes      */
/*      the Block access methods to satisfy the request.  This would    */
/*      normally only be overridden by formats with overviews.          */
/************************************************************************/

CPLErr GDALRasterBand::IRasterIO( GDALRWFlag eRWFlag,
                                  int nXOff, int nYOff, int nXSize, int nYSize,
                                  void * pData, int nBufXSize, int nBufYSize,
                                  GDALDataType eBufType,
                                  GSpacing nPixelSpace, GSpacing nLineSpace,
                                  GDALRasterIOExtraArg* psExtraArg )

{
    if( eRWFlag == GF_Write && eFlushBlockErr != CE_None )
    {
        CPLError(eFlushBlockErr, CPLE_AppDefined,
                 "An error occurred while writing a dirty block "
                 "from GDALRasterBand::IRasterIO");
        CPLErr eErr = eFlushBlockErr;
        eFlushBlockErr = CE_None;
        return eErr;
    }
    if( nBlockXSize <= 0 || nBlockYSize <= 0 )
    {
        CPLError( CE_Failure, CPLE_AppDefined, "Invalid block size" );
        return CE_Failure;
    }

    const int nBandDataSize = GDALGetDataTypeSizeBytes( eDataType );
    const int nBufDataSize = GDALGetDataTypeSizeBytes( eBufType );
    GByte *pabySrcBlock = nullptr;
    GDALRasterBlock *poBlock = nullptr;
    int nLBlockX = -1;
    int nLBlockY = -1;
    int iBufYOff = 0;
    int iBufXOff = 0;
    int iSrcY = 0;
    const bool bUseIntegerRequestCoords =
           (!psExtraArg->bFloatingPointWindowValidity ||
            (nXOff == psExtraArg->dfXOff &&
             nYOff == psExtraArg->dfYOff &&
             nXSize == psExtraArg->dfXSize &&
             nYSize == psExtraArg->dfYSize));

/* ==================================================================== */
/*      A common case is the data requested with the destination        */
/*      is packed, and the block width is the raster width.             */
/* ==================================================================== */
    if( nPixelSpace == nBufDataSize
        && nLineSpace == nPixelSpace * nXSize
        && nBlockXSize == GetXSize()
        && nBufXSize == nXSize
        && nBufYSize == nYSize
        && bUseIntegerRequestCoords )
    {
        CPLErr eErr = CE_None;

        for( iBufYOff = 0; iBufYOff < nBufYSize; iBufYOff++ )
        {
            iSrcY = iBufYOff + nYOff;

            if( iSrcY < nLBlockY * nBlockYSize
                || iSrcY - nBlockYSize >= nLBlockY * nBlockYSize )
            {
                nLBlockY = iSrcY / nBlockYSize;
                bool bJustInitialize =
                    eRWFlag == GF_Write
                    && nXOff == 0 && nXSize == nBlockXSize
                    && nYOff <= nLBlockY * nBlockYSize
                    && nYOff + nYSize - nBlockYSize >= nLBlockY * nBlockYSize;

                // Is this a partial tile at right and/or bottom edges of
                // the raster, and that is going to be completely written?
                // If so, do not load it from storage, but zero it so that
                // the content outsize of the validity area is initialized.
                bool bMemZeroBuffer = false;
                if( eRWFlag == GF_Write && !bJustInitialize &&
                    nXOff == 0 && nXSize == nBlockXSize &&
                    nYOff <= nLBlockY * nBlockYSize &&
                    nYOff + nYSize == GetYSize() &&
                    nLBlockY * nBlockYSize > GetYSize() - nBlockYSize )
                {
                    bJustInitialize = true;
                    bMemZeroBuffer = true;
                }

                if( poBlock )
                    poBlock->DropLock();

                if( poDS && poDS->Interrupted() )
                    return CE_Interrupted;

                const GUInt32 nErrorCounter = CPLGetErrorCounter();
                poBlock = GetLockedBlockRef( 0, nLBlockY, bJustInitialize );
                if( poBlock == nullptr )
                {
                    if( strstr(CPLGetLastErrorMsg(), "IReadBlock failed") == nullptr )
                    {
                        CPLError( CE_Failure, CPLE_AppDefined,
                            "GetBlockRef failed at X block offset %d, "
                            "Y block offset %d%s",
                            0, nLBlockY,
                            (nErrorCounter != CPLGetErrorCounter()) ?
                                    CPLSPrintf(": %s", CPLGetLastErrorMsg()) : "");
                    }
                    eErr = CE_Failure;
                    break;
                }

                if( eRWFlag == GF_Write )
                    poBlock->MarkDirty();

                pabySrcBlock =  static_cast<GByte *>(poBlock->GetDataRef());
                if( bMemZeroBuffer )
                {
                    memset(pabySrcBlock, 0,
                           nBandDataSize * nBlockXSize * nBlockYSize);
                }
            }

            // To make Coverity happy. Should not happen by design.
            if( pabySrcBlock == nullptr )
            {
                CPLAssert(false);
                eErr = CE_Failure;
                break;
            }

            const int nSrcByteOffset =
                ((iSrcY - nLBlockY * nBlockYSize) * nBlockXSize + nXOff)
                * nBandDataSize;

            if( eDataType == eBufType )
            {
                if( eRWFlag == GF_Read )
                    memcpy(
                        static_cast<GByte *>(pData)
                        + static_cast<GPtrDiff_t>(iBufYOff) * nLineSpace,
                        pabySrcBlock + nSrcByteOffset,
                        static_cast<size_t>(nLineSpace) );
                else
                    memcpy(
                        pabySrcBlock + nSrcByteOffset,
                        static_cast<GByte *>(pData)
                        + static_cast<GPtrDiff_t>(iBufYOff) * nLineSpace,
                        static_cast<size_t>(nLineSpace) );
            }
            else
            {
                // Type to type conversion.

                if( eRWFlag == GF_Read )
                    GDALCopyWords(
                        pabySrcBlock + nSrcByteOffset,
                        eDataType, nBandDataSize,
                        static_cast<GByte *>(pData)
                        + static_cast<GPtrDiff_t>(iBufYOff) * nLineSpace,
                        eBufType,
                        static_cast<int>(nPixelSpace), nBufXSize );
                else
                    GDALCopyWords(
                        static_cast<GByte *>(pData)
                        + static_cast<GPtrDiff_t>(iBufYOff) * nLineSpace,
                        eBufType, static_cast<int>(nPixelSpace),
                        pabySrcBlock + nSrcByteOffset,
                        eDataType, nBandDataSize, nBufXSize );
            }

            if( psExtraArg->pfnProgress != nullptr &&
                !psExtraArg->pfnProgress(1.0 * (iBufYOff + 1) / nBufYSize, "",
                                         psExtraArg->pProgressData) )
            {
                eErr = CE_Failure;
                break;
            }
        }

        if( poBlock )
            poBlock->DropLock();

        return eErr;
    }

/* ==================================================================== */
/*      Do we have overviews that would be appropriate to satisfy       */
/*      this request?                                                   */
/* ==================================================================== */
    if( (nBufXSize < nXSize || nBufYSize < nYSize)
        && GetOverviewCount() > 0 && eRWFlag == GF_Read )
    {
        GDALRasterIOExtraArg sExtraArg;
        GDALCopyRasterIOExtraArg(&sExtraArg, psExtraArg);

        const int nOverview =
            GDALBandGetBestOverviewLevel2( this, nXOff, nYOff, nXSize, nYSize,
                                           nBufXSize, nBufYSize, &sExtraArg );
        if (nOverview >= 0)
        {
            GDALRasterBand* poOverviewBand = GetOverview(nOverview);
            if (poOverviewBand == nullptr)
                return CE_Failure;

            return poOverviewBand->RasterIO(
                eRWFlag, nXOff, nYOff, nXSize, nYSize,
                pData, nBufXSize, nBufYSize, eBufType,
                nPixelSpace, nLineSpace, &sExtraArg );
        }
    }

    if( eRWFlag == GF_Read &&
        nBufXSize < nXSize / 100 && nBufYSize < nYSize / 100 &&
        nPixelSpace == nBufDataSize &&
        nLineSpace == nPixelSpace * nBufXSize &&
        CPLTestBool(CPLGetConfigOption("GDAL_NO_COSTLY_OVERVIEW", "NO")) )
    {
        memset( pData, 0, static_cast<size_t>(nLineSpace * nBufYSize) );
        return CE_None;
    }

/* ==================================================================== */
/*      The second case when we don't need subsample data but likely    */
/*      need data type conversion.                                      */
/* ==================================================================== */
    int iSrcX = 0;

    if ( // nPixelSpace == nBufDataSize &&
         nXSize == nBufXSize
         && nYSize == nBufYSize
         && bUseIntegerRequestCoords )
    {
#if DEBUG_VERBOSE
        printf( "IRasterIO(%d,%d,%d,%d) rw=%d case 2\n",/*ok*/
                nXOff, nYOff, nXSize, nYSize,
                static_cast<int>(eRWFlag) );
#endif

/* -------------------------------------------------------------------- */
/*      Loop over buffer computing source locations.                    */
/* -------------------------------------------------------------------- */
        // Calculate starting values out of loop
        const int nLBlockXStart = nXOff / nBlockXSize;
        const int nXSpanEnd = nBufXSize + nXOff;

        int nYInc = 0;
        for( iBufYOff = 0, iSrcY = nYOff;
             iBufYOff < nBufYSize;
             iBufYOff += nYInc, iSrcY += nYInc )
        {
            GPtrDiff_t iSrcOffset = 0;
            int nXSpan = 0;

            GPtrDiff_t iBufOffset =
                static_cast<GPtrDiff_t>(iBufYOff) *
                static_cast<GPtrDiff_t>(nLineSpace);
            nLBlockY = iSrcY / nBlockYSize;
            nLBlockX = nLBlockXStart;
            iSrcX = nXOff;
            while( iSrcX < nXSpanEnd )
            {
                nXSpan = nLBlockX * nBlockXSize;
                if( nXSpan < INT_MAX - nBlockXSize )
                    nXSpan += nBlockXSize;
                else
                    nXSpan = INT_MAX;
                const int nXRight = nXSpan;
                nXSpan = ( nXSpan < nXSpanEnd ? nXSpan:nXSpanEnd ) - iSrcX;
                const size_t nXSpanSize = nXSpan * static_cast<size_t>(nPixelSpace);

                bool bJustInitialize =
                    eRWFlag == GF_Write
                    && nYOff <= nLBlockY * nBlockYSize
                    && nYOff + nYSize - nBlockYSize >= nLBlockY * nBlockYSize
                    && nXOff <= nLBlockX * nBlockXSize
                    && nXOff + nXSize >= nXRight;

                // Is this a partial tile at right and/or bottom edges of
                // the raster, and that is going to be completely written?
                // If so, do not load it from storage, but zero it so that
                // the content outsize of the validity area is initialized.
                bool bMemZeroBuffer = false;
                if( eRWFlag == GF_Write && !bJustInitialize &&
                    nXOff <= nLBlockX * nBlockXSize &&
                    nYOff <= nLBlockY * nBlockYSize &&
                    (nXOff + nXSize >= nXRight ||
                     (nXOff + nXSize == GetXSize() &&
                      nXRight > GetXSize())) &&
                    (nYOff + nYSize - nBlockYSize >= nLBlockY * nBlockYSize ||
                     (nYOff + nYSize == GetYSize() &&
                      nLBlockY * nBlockYSize > GetYSize() - nBlockYSize)) )
                {
                    bJustInitialize = true;
                    bMemZeroBuffer = true;
                }

                if( poDS && poDS->Interrupted() )
                    return CE_Interrupted;

/* -------------------------------------------------------------------- */
/*      Ensure we have the appropriate block loaded.                    */
/* -------------------------------------------------------------------- */
                const GUInt32 nErrorCounter = CPLGetErrorCounter();
                poBlock = GetLockedBlockRef( nLBlockX, nLBlockY,
                                             bJustInitialize );
                if( !poBlock )
                {
                    if( strstr(CPLGetLastErrorMsg(), "IReadBlock failed") == nullptr )
                    {
                        CPLError( CE_Failure, CPLE_AppDefined,
                                "GetBlockRef failed at X block offset %d, "
                                "Y block offset %d%s",
                                nLBlockX, nLBlockY,
                                (nErrorCounter != CPLGetErrorCounter()) ?
                                    CPLSPrintf(": %s", CPLGetLastErrorMsg()) : "");
                    }
                    return( CE_Failure );
                }

                if( eRWFlag == GF_Write )
                    poBlock->MarkDirty();

                pabySrcBlock =  static_cast<GByte *>(poBlock->GetDataRef());
                if( bMemZeroBuffer )
                {
                    memset(pabySrcBlock, 0,
                           nBandDataSize * nBlockXSize * nBlockYSize);
                }
/* -------------------------------------------------------------------- */
/*      Copy over this chunk of data.                                   */
/* -------------------------------------------------------------------- */
                iSrcOffset =
                    (static_cast<GPtrDiff_t>(iSrcX)
                     - static_cast<GPtrDiff_t>(nLBlockX * nBlockXSize)
                     + (static_cast<GPtrDiff_t>(iSrcY)
                       - static_cast<GPtrDiff_t>(nLBlockY) * nBlockYSize)
                     * nBlockXSize)
                    * nBandDataSize;
                // Fill up as many rows as possible for the loaded block.
                const int kmax = std::min(
                    nBlockYSize - (iSrcY % nBlockYSize), nBufYSize - iBufYOff );
                for(int k=0; k<kmax;k++)
                {
                    if( eDataType == eBufType
                        && nPixelSpace == nBufDataSize )
                    {
                        if( eRWFlag == GF_Read )
                            memcpy( static_cast<GByte *>(pData) + iBufOffset
                                    + static_cast<GPtrDiff_t>(k) * nLineSpace,
                                    pabySrcBlock + iSrcOffset,
                                    nXSpanSize );
                        else
                            memcpy( pabySrcBlock + iSrcOffset,
                                    static_cast<GByte *>(pData) + iBufOffset
                                    + static_cast<GPtrDiff_t>(k) * nLineSpace,
                                    nXSpanSize );
                    }
                    else
                    {
                        /* type to type conversion */
                        if( eRWFlag == GF_Read )
                            GDALCopyWords(
                                pabySrcBlock + iSrcOffset,
                                eDataType, nBandDataSize,
                                static_cast<GByte *>(pData) + iBufOffset
                                + static_cast<GPtrDiff_t>(k) * nLineSpace,
                                eBufType, static_cast<int>(nPixelSpace),
                                nXSpan );
                        else
                            GDALCopyWords(
                                static_cast<GByte *>(pData) + iBufOffset +
                                static_cast<GPtrDiff_t>(k) * nLineSpace,
                                eBufType, static_cast<int>(nPixelSpace),
                                pabySrcBlock + iSrcOffset,
                                eDataType, nBandDataSize, nXSpan );
                    }

                    iSrcOffset += nBlockXSize * nBandDataSize;
                }

                iBufOffset += nXSpanSize;
                nLBlockX++;
                iSrcX+=nXSpan;

                poBlock->DropLock();
                poBlock = nullptr;
            }

            /* Compute the increment to go on a block boundary */
            nYInc = nBlockYSize - (iSrcY % nBlockYSize);

            if( psExtraArg->pfnProgress != nullptr &&
                !psExtraArg->pfnProgress(
                    1.0 * std::min(nBufYSize, iBufYOff + nYInc) / nBufYSize, "",
                    psExtraArg->pProgressData) )
            {
                return CE_Failure;
            }
        }

        return CE_None;
    }

/* ==================================================================== */
/*      Loop reading required source blocks to satisfy output           */
/*      request.  This is the most general implementation.              */
/* ==================================================================== */

    double dfXSize = nXSize;
    double dfYSize = nYSize;
    if( psExtraArg->bFloatingPointWindowValidity )
    {
        dfXSize = psExtraArg->dfXSize;
        dfYSize = psExtraArg->dfYSize;
    }

/* -------------------------------------------------------------------- */
/*      Compute stepping increment.                                     */
/* -------------------------------------------------------------------- */
    const double dfSrcXInc = dfXSize / static_cast<double>( nBufXSize );
    const double dfSrcYInc = dfYSize / static_cast<double>( nBufYSize );
    CPLErr eErr = CE_None;

    if (eRWFlag == GF_Write)
    {
/* -------------------------------------------------------------------- */
/*    Write case                                                        */
/*    Loop over raster window computing source locations in the buffer. */
/* -------------------------------------------------------------------- */
        GByte* pabyDstBlock = nullptr;

        for( int iDstY = nYOff; iDstY < nYOff + nYSize; iDstY ++)
        {
            GPtrDiff_t iBufOffset = 0;
            GPtrDiff_t iDstOffset = 0;
            iBufYOff = static_cast<int>((iDstY - nYOff) / dfSrcYInc);

            for( int iDstX = nXOff; iDstX < nXOff + nXSize; iDstX ++)
            {
                iBufXOff = static_cast<int>((iDstX - nXOff) / dfSrcXInc);
                iBufOffset =
                    static_cast<GPtrDiff_t>(iBufYOff)
                    * static_cast<GPtrDiff_t>(nLineSpace)
                    + iBufXOff * static_cast<GPtrDiff_t>(nPixelSpace);

                // FIXME: this code likely doesn't work if the dirty block gets
                // flushed to disk before being completely written.
                // In the meantime, bJustInitialize should probably be set to
                // FALSE even if it is not ideal performance wise, and for
                // lossy compression.

    /* -------------------------------------------------------------------- */
    /*      Ensure we have the appropriate block loaded.                    */
    /* -------------------------------------------------------------------- */
                if( iDstX < nLBlockX * nBlockXSize
                    || iDstX - nBlockXSize >= nLBlockX * nBlockXSize
                    || iDstY < nLBlockY * nBlockYSize
                    || iDstY - nBlockYSize >= nLBlockY * nBlockYSize )
                {
                    nLBlockX = iDstX / nBlockXSize;
                    nLBlockY = iDstY / nBlockYSize;

                    const bool bJustInitialize =
                        nYOff <= nLBlockY * nBlockYSize
                        && nYOff + nYSize - nBlockYSize >= nLBlockY * nBlockYSize
                        && nXOff <= nLBlockX * nBlockXSize
                        && nXOff + nXSize - nBlockXSize >= nLBlockX * nBlockXSize;
                    /*bool bMemZeroBuffer = FALSE;
                    if( !bJustInitialize &&
                        nXOff <= nLBlockX * nBlockXSize &&
                        nYOff <= nLBlockY * nBlockYSize &&
                        (nXOff + nXSize >= (nLBlockX+1) * nBlockXSize ||
                         (nXOff + nXSize == GetXSize() &&
                         (nLBlockX+1) * nBlockXSize > GetXSize())) &&
                        (nYOff + nYSize >= (nLBlockY+1) * nBlockYSize ||
                         (nYOff + nYSize == GetYSize() &&
                         (nLBlockY+1) * nBlockYSize > GetYSize())) )
                    {
                        bJustInitialize = TRUE;
                        bMemZeroBuffer = TRUE;
                    }*/
                    if( poBlock != nullptr )
                        poBlock->DropLock();

                    poBlock = GetLockedBlockRef( nLBlockX, nLBlockY,
                                                 bJustInitialize );
                    if( poBlock == nullptr )
                    {
                        return( CE_Failure );
                    }

                    poBlock->MarkDirty();

                    pabyDstBlock =  static_cast<GByte *>(poBlock->GetDataRef());
                    /*if( bMemZeroBuffer )
                    {
                        memset(pabyDstBlock, 0,
                            nBandDataSize * nBlockXSize * nBlockYSize);
                    }*/
                }

                // To make Coverity happy. Should not happen by design.
                if( pabyDstBlock == nullptr )
                {
                    CPLAssert(false);
                    eErr = CE_Failure;
                    break;
                }

    /* -------------------------------------------------------------------- */
    /*      Copy over this pixel of data.                                   */
    /* -------------------------------------------------------------------- */
                iDstOffset =
                    (static_cast<GPtrDiff_t>(iDstX)
                     - static_cast<GPtrDiff_t>(nLBlockX) * nBlockXSize
                     + (static_cast<GPtrDiff_t>(iDstY)
                        - static_cast<GPtrDiff_t>(nLBlockY) * nBlockYSize)
                     * nBlockXSize )
                    * nBandDataSize;

                if( eDataType == eBufType )
                {
                    memcpy(
                        pabyDstBlock + iDstOffset,
                        static_cast<GByte *>(pData) + iBufOffset,
                        nBandDataSize );
                }
                else
                {
                    /* type to type conversion ... ouch, this is expensive way
                    of handling single words */

                    GDALCopyWords(
                        static_cast<GByte *>(pData) + iBufOffset, eBufType, 0,
                        pabyDstBlock + iDstOffset, eDataType, 0,
                        1 );
                }
            }

            if( psExtraArg->pfnProgress != nullptr &&
                !psExtraArg->pfnProgress(1.0 * (iDstY - nYOff + 1) / nYSize, "",
                                         psExtraArg->pProgressData) )
            {
                eErr = CE_Failure;
                break;
            }
        }
    }
    else
    {
        if( psExtraArg->eResampleAlg != GRIORA_NearestNeighbour )
        {
            if( (psExtraArg->eResampleAlg == GRIORA_Cubic ||
                 psExtraArg->eResampleAlg == GRIORA_CubicSpline ||
                 psExtraArg->eResampleAlg == GRIORA_Bilinear ||
                 psExtraArg->eResampleAlg == GRIORA_Lanczos) &&
                GetColorTable() != nullptr )
            {
                CPLError(CE_Warning, CPLE_NotSupported,
                         "Resampling method not supported on paletted band. "
                         "Falling back to nearest neighbour");
            }
            else if( psExtraArg->eResampleAlg == GRIORA_Gauss &&
                     GDALDataTypeIsComplex( eDataType ) )
            {
                CPLError(
                    CE_Warning, CPLE_NotSupported,
                    "Resampling method not supported on complex data type "
                    "band. Falling back to nearest neighbour");
            }
            else
            {
                return RasterIOResampled( eRWFlag,
                                          nXOff, nYOff, nXSize, nYSize,
                                          pData, nBufXSize, nBufYSize,
                                          eBufType,
                                          nPixelSpace, nLineSpace,
                                          psExtraArg );
            }
        }

        double dfSrcX = 0.0;
        double dfSrcY = 0.0;
        const bool bByteCopy = eDataType == eBufType && nBandDataSize == 1;
        const double EPS = 1e-10;

/* -------------------------------------------------------------------- */
/*      Read case                                                       */
/* -------------------------------------------------------------------- */

        int nStartBlockX, nStartBlockY, nEndBlockX, nEndBlockY;

        nStartBlockX = nXOff / nBlockXSize;
        nStartBlockY = nYOff / nBlockYSize;
        nEndBlockX = (nXOff+nXSize-1) / nBlockXSize;
        nEndBlockY = (nYOff+nYSize-1) / nBlockYSize;

        int iBufYLim, iBufXLim;
        GPtrDiff_t nDiffX = iSrcX - nStartBlockX;

/* -------------------------------------------------------------------- */
/*     Iterate over the source blocks                                   */
/* -------------------------------------------------------------------- */

        for(nLBlockY = nStartBlockY; nLBlockY <= nEndBlockY; nLBlockY++)
        {
            for(nLBlockX = nStartBlockX; nLBlockX <= nEndBlockX; nLBlockX++)
            {

                if( poBlock != nullptr )
                    poBlock->DropLock();

                if( poDS && poDS->Interrupted() )
                    return CE_Interrupted;

                poBlock = GetLockedBlockRef( nLBlockX, nLBlockY, 
                                             FALSE );
                if( poBlock == nullptr )
                {
                    eErr = CE_Failure;
                    break;
                }

                pabySrcBlock = (GByte *) poBlock->GetDataRef();
                if( pabySrcBlock == nullptr )
                {
                    poBlock->DropLock();
                    eErr = CE_Failure;
                    break;
                }

/* -------------------------------------------------------------------- */
/*      Loop over buffer region computing source locations.             */
/* -------------------------------------------------------------------- */
                iBufYOff = (int)((nLBlockY*nBlockYSize-nYOff)/dfSrcYInc);
                if(iBufYOff < 0)
                    iBufYOff = 0;
                iBufYLim = (int)ceil(((nLBlockY+1)*nBlockYSize-nYOff)/dfSrcYInc);
                if(iBufYLim > nBufYSize)
                    iBufYLim = nBufYSize;

                for( ; iBufYOff < iBufYLim; iBufYOff++ )
                {
                    int     iBufOffset, iSrcOffset;

                    // Add small epsilon to avoid some numeric precision issues.
                    dfSrcY = (iBufYOff+0.5) * dfSrcYInc + nYOff + EPS;
                    iSrcY = (int) dfSrcY;
                    if(iSrcY < nLBlockY*nBlockYSize)
                        iSrcY = nLBlockY*nBlockYSize;
                    else if(iSrcY >= (nLBlockY+1)*nBlockYSize)
                        iSrcY = (nLBlockY+1)*nBlockYSize-1;

                    iBufOffset = iBufYOff * nLineSpace;

                    iBufXOff = (int)((nLBlockX*nBlockXSize-nXOff)/dfSrcXInc);
                    if(iBufXOff < 0)
                        iBufXOff = 0;
                    iBufXLim = (int)ceil(((nLBlockX+1)*nBlockXSize-nXOff)/dfSrcXInc);
                    if(iBufXLim > nBufXSize)
                        iBufXLim = nBufXSize;
                    // offset by the buffer x-pixel for the block
                    iBufOffset += (iBufXOff*nPixelSpace);
                    GPtrDiff_t iSrcOffsetCst = (iSrcY - nLBlockY*nBlockYSize) * (GPtrDiff_t)nBlockXSize;

                    for( ; iBufXOff < iBufXLim; iBufXOff++ )
                    {
                        int iSrcX;

                        dfSrcX = (iBufXOff+0.5) * dfSrcXInc + nXOff + EPS;

                        iSrcX = (int) dfSrcX;
                        if(iSrcX < nLBlockX*nBlockXSize)
                            iSrcX = nLBlockX*nBlockXSize;
                        else if(iSrcX >= (nLBlockX+1)*nBlockXSize)
                            iSrcX = (nLBlockX+1)*nBlockXSize-1;

                        nDiffX = iSrcX - (nLBlockX*nBlockXSize);

    /* -------------------------------------------------------------------- */
    /*      Copy over this pixel of data.                                   */
    /* -------------------------------------------------------------------- */

                if( bByteCopy )
                {
                    iSrcOffset = nDiffX + iSrcOffsetCst;
                    static_cast<GByte *>(pData)[iBufOffset] =
                        pabySrcBlock[iSrcOffset];
                }
                else if( eDataType == eBufType )
                {
                    iSrcOffset =
                        (nDiffX + iSrcOffsetCst)
                        * nBandDataSize;
                    memcpy( static_cast<GByte *>(pData) + iBufOffset,
                            pabySrcBlock + iSrcOffset, nBandDataSize );
                }
                else
                {
                    // Type to type conversion ... ouch, this is expensive way
                    // of handling single words.
                    iSrcOffset =
                        (nDiffX + iSrcOffsetCst)
                        * nBandDataSize;
                    GDALCopyWords(
                        pabySrcBlock + iSrcOffset, eDataType, 0,
                        static_cast<GByte *>(pData) + iBufOffset, eBufType, 0,
                        1 );
                }

                iBufOffset += static_cast<int>(nPixelSpace);
            }
            if( eErr == CE_Failure )
                break;

            if( psExtraArg->pfnProgress != nullptr &&
                !psExtraArg->pfnProgress(1.0 * (iBufYOff + 1) / nBufYSize, "",
                                         psExtraArg->pProgressData) )
            {
                eErr = CE_Failure;
                break;
            }
        }
    }
        }
    }
    

    if( poBlock != nullptr )
        poBlock->DropLock();

    return eErr;
}

/************************************************************************/
/*                         GDALRasterIOTransformer()                    */
/************************************************************************/

struct GDALRasterIOTransformerStruct
{
    double dfXOff;
    double dfYOff;
    double dfXRatioDstToSrc;
    double dfYRatioDstToSrc;
};

static int GDALRasterIOTransformer( void *pTransformerArg,
                                    int bDstToSrc,
                                    int nPointCount,
                                    double *x, double *y, double * /* z */,
                                    int *panSuccess )
{
    GDALRasterIOTransformerStruct* psParams =
        static_cast<GDALRasterIOTransformerStruct *>( pTransformerArg );
    if( bDstToSrc )
    {
        for(int i = 0; i < nPointCount; i++)
        {
            x[i] = x[i] * psParams->dfXRatioDstToSrc + psParams->dfXOff;
            y[i] = y[i] * psParams->dfYRatioDstToSrc + psParams->dfYOff;
            panSuccess[i] = TRUE;
        }
    }
    else
    {
        for(int i = 0; i < nPointCount; i++)
        {
            x[i] = (x[i] - psParams->dfXOff) / psParams->dfXRatioDstToSrc;
            y[i] = (y[i] - psParams->dfYOff) / psParams->dfYRatioDstToSrc;
            panSuccess[i] = TRUE;
        }
    }
    return TRUE;
}

/************************************************************************/
/*                          RasterIOResampled()                         */
/************************************************************************/

//! @cond Doxygen_Suppress
CPLErr GDALRasterBand::RasterIOResampled(
    GDALRWFlag /* eRWFlag */,
    int nXOff, int nYOff, int nXSize, int nYSize,
    void * pData, int nBufXSize, int nBufYSize,
    GDALDataType eBufType,
    GSpacing nPixelSpace, GSpacing nLineSpace,
    GDALRasterIOExtraArg* psExtraArg )
{
    // Determine if we use warping resampling or overview resampling
    bool bUseWarp = false;
    if( GDALDataTypeIsComplex( eDataType ) )
        bUseWarp = true;

    double dfXOff = nXOff;
    double dfYOff = nYOff;
    double dfXSize = nXSize;
    double dfYSize = nYSize;
    if( psExtraArg->bFloatingPointWindowValidity )
    {
        dfXOff = psExtraArg->dfXOff;
        dfYOff = psExtraArg->dfYOff;
        dfXSize = psExtraArg->dfXSize;
        dfYSize = psExtraArg->dfYSize;
    }

    const double dfXRatioDstToSrc = dfXSize / nBufXSize;
    const double dfYRatioDstToSrc = dfYSize / nBufYSize;

    // Determine the coordinates in the "virtual" output raster to see
    // if there are not integers, in which case we will use them as a shift
    // so that subwindow extracts give the exact same results as entire raster
    // scaling.
    double dfDestXOff = dfXOff / dfXRatioDstToSrc;
    bool bHasXOffVirtual = false;
    int nDestXOffVirtual = 0;
    if( fabs(dfDestXOff - static_cast<int>(dfDestXOff + 0.5)) < 1e-8 )
    {
        bHasXOffVirtual = true;
        dfXOff = nXOff;
        nDestXOffVirtual = static_cast<int>(dfDestXOff + 0.5);
    }

    double dfDestYOff = dfYOff / dfYRatioDstToSrc;
    bool bHasYOffVirtual = false;
    int nDestYOffVirtual = 0;
    if( fabs(dfDestYOff - static_cast<int>(dfDestYOff + 0.5)) < 1e-8 )
    {
        bHasYOffVirtual = true;
        dfYOff = nYOff;
        nDestYOffVirtual = static_cast<int>(dfDestYOff + 0.5);
    }

    // Create a MEM dataset that wraps the output buffer.
    GDALDataset* poMEMDS;
    void* pTempBuffer = nullptr;
    GSpacing nPSMem = nPixelSpace;
    GSpacing nLSMem = nLineSpace;
    void* pDataMem = pData;
    GDALDataType eDTMem = eBufType;
    if( eBufType != eDataType )
    {
        nPSMem = GDALGetDataTypeSizeBytes(eDataType);
        nLSMem = nPSMem * nBufXSize;
        pTempBuffer = VSI_MALLOC2_VERBOSE( nBufYSize, static_cast<size_t>(nLSMem) );
        if( pTempBuffer == nullptr )
            return CE_Failure;
        pDataMem = pTempBuffer;
        eDTMem = eDataType;
    }

    poMEMDS = MEMDataset::Create( "", nDestXOffVirtual + nBufXSize,
                                  nDestYOffVirtual + nBufYSize, 0,
                                  eDTMem, nullptr );
    char szBuffer[32] = { '\0' };
    int nRet =
        CPLPrintPointer(
            szBuffer, static_cast<GByte*>(pDataMem)
            - nPSMem * nDestXOffVirtual
            - nLSMem * nDestYOffVirtual, sizeof(szBuffer));
    szBuffer[nRet] = '\0';

    char szBuffer0[64] = { '\0' };
    snprintf(szBuffer0, sizeof(szBuffer0), "DATAPOINTER=%s", szBuffer);
    char szBuffer1[64] = { '\0' };
    snprintf( szBuffer1, sizeof(szBuffer1),
              "PIXELOFFSET=" CPL_FRMT_GIB, static_cast<GIntBig>(nPSMem) );
    char szBuffer2[64] = { '\0' };
    snprintf( szBuffer2, sizeof(szBuffer2),
              "LINEOFFSET=" CPL_FRMT_GIB, static_cast<GIntBig>(nLSMem) );
    char* apszOptions[4] = { szBuffer0, szBuffer1, szBuffer2, nullptr };

    poMEMDS->AddBand(eDTMem, apszOptions);

    GDALRasterBandH hMEMBand = poMEMDS->GetRasterBand(1);

    const char* pszNBITS = GetMetadataItem("NBITS", "IMAGE_STRUCTURE");
    if( pszNBITS )
        reinterpret_cast<GDALRasterBand *>(hMEMBand)->
            SetMetadataItem("NBITS", pszNBITS, "IMAGE_STRUCTURE");

    CPLErr eErr = CE_None;

    // Do the resampling.
    if( bUseWarp )
    {
        int bHasNoData = FALSE;
        double dfNoDataValue = GetNoDataValue(&bHasNoData) ;

        VRTDatasetH hVRTDS = nullptr;
        GDALRasterBandH hVRTBand = nullptr;
        if( GetDataset() == nullptr )
        {
            /* Create VRT dataset that wraps the whole dataset */
            hVRTDS = VRTCreate(nRasterXSize, nRasterYSize);
            VRTAddBand( hVRTDS, eDataType, nullptr );
            hVRTBand = GDALGetRasterBand(hVRTDS, 1);
            VRTAddSimpleSource( hVRTBand,
                                this,
                                0, 0,
                                nRasterXSize, nRasterYSize,
                                0, 0,
                                nRasterXSize, nRasterYSize,
                                nullptr, VRT_NODATA_UNSET );

            /* Add a mask band if needed */
            if( GetMaskFlags() != GMF_ALL_VALID )
            {
              reinterpret_cast<GDALDataset *>(hVRTDS)->CreateMaskBand(0);
                VRTSourcedRasterBand* poVRTMaskBand =
                    reinterpret_cast<VRTSourcedRasterBand *>(
                        reinterpret_cast<GDALRasterBand *>(hVRTBand)->
                            GetMaskBand());
                poVRTMaskBand->
                    AddMaskBandSource( this,
                                       0, 0,
                                       nRasterXSize, nRasterYSize,
                                       0, 0,
                                       nRasterXSize, nRasterYSize );
            }
        }

        GDALWarpOptions* psWarpOptions = GDALCreateWarpOptions();
        switch(psExtraArg->eResampleAlg)
        {
            case GRIORA_NearestNeighbour:
                psWarpOptions->eResampleAlg = GRA_NearestNeighbour; break;
            case GRIORA_Bilinear:
                psWarpOptions->eResampleAlg = GRA_Bilinear; break;
            case GRIORA_Cubic:
                psWarpOptions->eResampleAlg = GRA_Cubic; break;
            case GRIORA_CubicSpline:
                psWarpOptions->eResampleAlg = GRA_CubicSpline; break;
            case GRIORA_Lanczos:
                psWarpOptions->eResampleAlg = GRA_Lanczos; break;
            case GRIORA_Average:
                psWarpOptions->eResampleAlg = GRA_Average; break;
            case GRIORA_Mode:
                psWarpOptions->eResampleAlg = GRA_Mode; break;
            default:
                CPLAssert(false);
                psWarpOptions->eResampleAlg = GRA_NearestNeighbour; break;
        }
        psWarpOptions->hSrcDS = hVRTDS ? hVRTDS : GetDataset();
        psWarpOptions->hDstDS = poMEMDS;
        psWarpOptions->nBandCount = 1;
        int nSrcBandNumber = hVRTDS ? 1 : nBand;
        int nDstBandNumber = 1;
        psWarpOptions->panSrcBands = &nSrcBandNumber;
        psWarpOptions->panDstBands = &nDstBandNumber;
        psWarpOptions->pfnProgress = psExtraArg->pfnProgress ?
                    psExtraArg->pfnProgress : GDALDummyProgress;
        psWarpOptions->pProgressArg = psExtraArg->pProgressData;
        psWarpOptions->pfnTransformer = GDALRasterIOTransformer;
        if (bHasNoData)
        {
            psWarpOptions->papszWarpOptions = CSLSetNameValue( psWarpOptions->papszWarpOptions,
                                                    "INIT_DEST", "NO_DATA");
            if (psWarpOptions->padfSrcNoDataReal == nullptr)
            {
                psWarpOptions->padfSrcNoDataReal = static_cast<double*>(CPLMalloc(sizeof(double)));
                psWarpOptions->padfSrcNoDataReal[0] = dfNoDataValue;
            }

            if (psWarpOptions->padfDstNoDataReal == nullptr)
            {
                psWarpOptions->padfDstNoDataReal = static_cast<double*>(CPLMalloc(sizeof(double)));
                psWarpOptions->padfDstNoDataReal[0] = dfNoDataValue;
            }
        }

        GDALRasterIOTransformerStruct sTransformer;
        sTransformer.dfXOff = bHasXOffVirtual ? 0 : dfXOff;
        sTransformer.dfYOff = bHasYOffVirtual ? 0 : dfYOff;
        sTransformer.dfXRatioDstToSrc = dfXRatioDstToSrc;
        sTransformer.dfYRatioDstToSrc = dfYRatioDstToSrc;
        psWarpOptions->pTransformerArg = &sTransformer;

        GDALWarpOperationH hWarpOperation =
            GDALCreateWarpOperation(psWarpOptions);
        eErr = GDALChunkAndWarpImage( hWarpOperation,
                                      nDestXOffVirtual, nDestYOffVirtual,
                                      nBufXSize, nBufYSize );
        GDALDestroyWarpOperation( hWarpOperation );

        psWarpOptions->panSrcBands = nullptr;
        psWarpOptions->panDstBands = nullptr;
        GDALDestroyWarpOptions( psWarpOptions );

        if( hVRTDS )
            GDALClose(hVRTDS);
    }
    else
    {
        const char* pszResampling =
            (psExtraArg->eResampleAlg == GRIORA_Bilinear) ? "BILINEAR" :
            (psExtraArg->eResampleAlg == GRIORA_Cubic) ? "CUBIC" :
            (psExtraArg->eResampleAlg == GRIORA_CubicSpline) ? "CUBICSPLINE" :
            (psExtraArg->eResampleAlg == GRIORA_Lanczos) ? "LANCZOS" :
            (psExtraArg->eResampleAlg == GRIORA_Average) ? "AVERAGE" :
            (psExtraArg->eResampleAlg == GRIORA_Mode) ? "MODE" :
            (psExtraArg->eResampleAlg == GRIORA_Gauss) ? "GAUSS" : "UNKNOWN";

        int nKernelRadius = 0;
        GDALResampleFunction pfnResampleFunc =
                        GDALGetResampleFunction(pszResampling, &nKernelRadius);
        CPLAssert(pfnResampleFunc);
        GDALDataType eWrkDataType =
            GDALGetOvrWorkDataType(pszResampling, eDataType);
        int bHasNoData = FALSE;
        float fNoDataValue = static_cast<float>( GetNoDataValue(&bHasNoData) );
        if( !bHasNoData )
            fNoDataValue = 0.0f;

        int nDstBlockXSize = nBufXSize;
        int nDstBlockYSize = nBufYSize;
        int nFullResXChunk = 0;
        int nFullResYChunk = 0;
        while( true )
        {
            nFullResXChunk =
                3 + static_cast<int>(nDstBlockXSize * dfXRatioDstToSrc);
            nFullResYChunk =
                3 + static_cast<int>(nDstBlockYSize * dfYRatioDstToSrc);
            if( nFullResXChunk > nRasterXSize )
                nFullResXChunk = nRasterXSize;
            if( nFullResYChunk > nRasterYSize )
                nFullResYChunk = nRasterYSize;
            if( (nDstBlockXSize == 1 && nDstBlockYSize == 1) ||
                (static_cast<GIntBig>(nFullResXChunk) * nFullResYChunk <= 1024 * 1024) )
                break;
            // When operating on the full width of a raster whose block width is
            // the raster width, prefer doing chunks in height.
            if( nFullResXChunk >= nXSize && nXSize == nBlockXSize &&
                nDstBlockYSize > 1 )
                nDstBlockYSize /= 2;
            /* Otherwise cut the maximal dimension */
            else if( nDstBlockXSize > 1 &&
                     (nFullResXChunk > nFullResYChunk || nDstBlockYSize == 1) )
                nDstBlockXSize /= 2;
            else
                nDstBlockYSize /= 2;
        }

        int nOvrXFactor = static_cast<int>(0.5 + dfXRatioDstToSrc);
        int nOvrYFactor = static_cast<int>(0.5 + dfYRatioDstToSrc);
        if( nOvrXFactor == 0 ) nOvrXFactor = 1;
        if( nOvrYFactor == 0 ) nOvrYFactor = 1;
        int nFullResXSizeQueried =
            nFullResXChunk + 2 * nKernelRadius * nOvrXFactor;
        int nFullResYSizeQueried =
            nFullResYChunk + 2 * nKernelRadius * nOvrYFactor;

        if( nFullResXSizeQueried > nRasterXSize )
            nFullResXSizeQueried = nRasterXSize;
        if( nFullResYSizeQueried > nRasterYSize )
            nFullResYSizeQueried = nRasterYSize;

        void * pChunk =
            VSI_MALLOC3_VERBOSE( GDALGetDataTypeSizeBytes(eWrkDataType),
                                 nFullResXSizeQueried, nFullResYSizeQueried );
        GByte * pabyChunkNoDataMask = nullptr;

        GDALRasterBand* poMaskBand = GetMaskBand();
        int l_nMaskFlags = GetMaskFlags();

        bool bUseNoDataMask = ((l_nMaskFlags & GMF_ALL_VALID) == 0);
        if (bUseNoDataMask)
        {
            pabyChunkNoDataMask = static_cast<GByte *>(
                VSI_MALLOC2_VERBOSE( nFullResXSizeQueried,
                                     nFullResYSizeQueried ) );
        }
        if( pChunk == nullptr || (bUseNoDataMask && pabyChunkNoDataMask == nullptr) )
        {
            GDALClose(poMEMDS);
            CPLFree(pChunk);
            CPLFree(pabyChunkNoDataMask);
            VSIFree(pTempBuffer);
            return CE_Failure;
        }

        int nTotalBlocks = ((nBufXSize + nDstBlockXSize - 1) / nDstBlockXSize) *
                           ((nBufYSize + nDstBlockYSize - 1) / nDstBlockYSize);
        int nBlocksDone = 0;

        int nDstYOff;
        for( nDstYOff = 0; nDstYOff < nBufYSize && eErr == CE_None;
            nDstYOff += nDstBlockYSize )
        {
            int nDstYCount;
            if  (nDstYOff + nDstBlockYSize <= nBufYSize)
                nDstYCount = nDstBlockYSize;
            else
                nDstYCount = nBufYSize - nDstYOff;

            int nChunkYOff =
                nYOff + static_cast<int>(nDstYOff * dfYRatioDstToSrc);
            int nChunkYOff2 =
                nYOff + 1 +
                static_cast<int>(
                    ceil((nDstYOff + nDstYCount) * dfYRatioDstToSrc ) );
            if( nChunkYOff2 > nRasterYSize )
                nChunkYOff2 = nRasterYSize;
            int nYCount = nChunkYOff2 - nChunkYOff;
            CPLAssert(nYCount <= nFullResYChunk);

            int nChunkYOffQueried = nChunkYOff - nKernelRadius * nOvrYFactor;
            int nChunkYSizeQueried = nYCount + 2 * nKernelRadius * nOvrYFactor;
            if( nChunkYOffQueried < 0 )
            {
                nChunkYSizeQueried += nChunkYOffQueried;
                nChunkYOffQueried = 0;
            }
            if( nChunkYSizeQueried + nChunkYOffQueried > nRasterYSize )
                nChunkYSizeQueried = nRasterYSize - nChunkYOffQueried;
            CPLAssert(nChunkYSizeQueried <= nFullResYSizeQueried);

            int nDstXOff = 0;
            for( nDstXOff = 0; nDstXOff < nBufXSize && eErr == CE_None;
                nDstXOff += nDstBlockXSize )
            {
                int nDstXCount = 0;
                if  (nDstXOff + nDstBlockXSize <= nBufXSize)
                    nDstXCount = nDstBlockXSize;
                else
                    nDstXCount = nBufXSize - nDstXOff;

                int nChunkXOff =
                    nXOff + static_cast<int>(nDstXOff * dfXRatioDstToSrc);
                int nChunkXOff2 =
                    nXOff + 1 +
                    static_cast<int>(
                        ceil((nDstXOff + nDstXCount) * dfXRatioDstToSrc));
                if( nChunkXOff2 > nRasterXSize )
                    nChunkXOff2 = nRasterXSize;
                int nXCount = nChunkXOff2 - nChunkXOff;
                CPLAssert(nXCount <= nFullResXChunk);

                int nChunkXOffQueried = nChunkXOff - nKernelRadius * nOvrXFactor;
                int nChunkXSizeQueried =
                    nXCount + 2 * nKernelRadius * nOvrXFactor;
                if( nChunkXOffQueried < 0 )
                {
                    nChunkXSizeQueried += nChunkXOffQueried;
                    nChunkXOffQueried = 0;
                }
                if( nChunkXSizeQueried + nChunkXOffQueried > nRasterXSize )
                    nChunkXSizeQueried = nRasterXSize - nChunkXOffQueried;
                CPLAssert(nChunkXSizeQueried <= nFullResXSizeQueried);

                // Read the source buffers.
                eErr = RasterIO( GF_Read,
                                nChunkXOffQueried, nChunkYOffQueried,
                                nChunkXSizeQueried, nChunkYSizeQueried,
                                pChunk,
                                nChunkXSizeQueried, nChunkYSizeQueried,
                                eWrkDataType, 0, 0, nullptr );

                bool bSkipResample = false;
                bool bNoDataMaskFullyOpaque = false;
                if (eErr == CE_None && bUseNoDataMask)
                {
                    eErr = poMaskBand->RasterIO( GF_Read,
                                                 nChunkXOffQueried,
                                                 nChunkYOffQueried,
                                                 nChunkXSizeQueried,
                                                 nChunkYSizeQueried,
                                                 pabyChunkNoDataMask,
                                                 nChunkXSizeQueried,
                                                 nChunkYSizeQueried,
                                                 GDT_Byte, 0, 0, nullptr );

                    /* Optimizations if mask if fully opaque or transparent */
                    int nPixels = nChunkXSizeQueried * nChunkYSizeQueried;
                    GByte bVal = pabyChunkNoDataMask[0];
                    int i = 1;
                    for( ; i < nPixels; i++ )
                    {
                        if( pabyChunkNoDataMask[i] != bVal )
                            break;
                    }
                    if( i == nPixels )
                    {
                        if( bVal == 0 )
                        {
                            for(int j=0;j<nDstYCount;j++)
                            {
                                GDALCopyWords(
                                    &fNoDataValue, GDT_Float32, 0,
                                    static_cast<GByte*>(pDataMem) +
                                    nLSMem * (j + nDstYOff) +
                                    nDstXOff * nPSMem,
                                    eDTMem, static_cast<int>(nPSMem),
                                    nDstXCount);
                            }
                            bSkipResample = true;
                        }
                        else
                        {
                            bNoDataMaskFullyOpaque = true;
                        }
                    }
                }

                if( !bSkipResample && eErr == CE_None )
                {
                    const bool bPropagateNoData = false;
                    eErr = pfnResampleFunc(
                        dfXRatioDstToSrc,
                        dfYRatioDstToSrc,
                        dfXOff - nXOff, /* == 0 if bHasXOffVirtual */
                        dfYOff - nYOff, /* == 0 if bHasYOffVirtual */
                        eWrkDataType,
                        pChunk,
                        bNoDataMaskFullyOpaque ? nullptr : pabyChunkNoDataMask,
                        nChunkXOffQueried - (bHasXOffVirtual ? 0 : nXOff),
                        nChunkXSizeQueried,
                        nChunkYOffQueried - (bHasYOffVirtual ? 0 : nYOff),
                        nChunkYSizeQueried,
                        nDstXOff + nDestXOffVirtual,
                        nDstXOff + nDestXOffVirtual + nDstXCount,
                        nDstYOff + nDestYOffVirtual,
                        nDstYOff + nDestYOffVirtual + nDstYCount,
                        GDALRasterBand::FromHandle(hMEMBand),
                        pszResampling,
                        bHasNoData, fNoDataValue,
                        GetColorTable(),
                        eDataType,
                        bPropagateNoData);
                }

                nBlocksDone ++;
                if( eErr == CE_None && psExtraArg->pfnProgress != nullptr &&
                    !psExtraArg->pfnProgress(
                        1.0 * nBlocksDone / nTotalBlocks, "",
                        psExtraArg->pProgressData) )
                {
                    eErr = CE_Failure;
                }
            }
        }

        CPLFree(pChunk);
        CPLFree(pabyChunkNoDataMask);
    }

    if( eBufType != eDataType )
    {
        CPL_IGNORE_RET_VAL(poMEMDS->GetRasterBand(1)->RasterIO(GF_Read,
                          nDestXOffVirtual, nDestYOffVirtual,
                          nBufXSize, nBufYSize,
                          pData,
                          nBufXSize, nBufYSize,
                          eBufType,
                          nPixelSpace, nLineSpace,
                          nullptr));
    }
    GDALClose(poMEMDS);
    VSIFree(pTempBuffer);

    return eErr;
}

/************************************************************************/
/*                          RasterIOResampled()                         */
/************************************************************************/

CPLErr GDALDataset::RasterIOResampled(
    GDALRWFlag /* eRWFlag */,
    int nXOff, int nYOff, int nXSize, int nYSize,
    void *pData, int nBufXSize, int nBufYSize,
    GDALDataType eBufType,
    int nBandCount, int *panBandMap,
    GSpacing nPixelSpace, GSpacing nLineSpace,
    GSpacing nBandSpace,
    GDALRasterIOExtraArg* psExtraArg )

{
#if 0
    // Determine if we use warping resampling or overview resampling
    bool bUseWarp = false;
    if( GDALDataTypeIsComplex( eDataType ) )
        bUseWarp = true;
#endif

    double dfXOff = nXOff;
    double dfYOff = nYOff;
    double dfXSize = nXSize;
    double dfYSize = nYSize;
    if( psExtraArg->bFloatingPointWindowValidity )
    {
        dfXOff = psExtraArg->dfXOff;
        dfYOff = psExtraArg->dfYOff;
        dfXSize = psExtraArg->dfXSize;
        dfYSize = psExtraArg->dfYSize;
    }

    const double dfXRatioDstToSrc = dfXSize / nBufXSize;
    const double dfYRatioDstToSrc = dfYSize / nBufYSize;

    // Determine the coordinates in the "virtual" output raster to see
    // if there are not integers, in which case we will use them as a shift
    // so that subwindow extracts give the exact same results as entire raster
    // scaling.
    double dfDestXOff = dfXOff / dfXRatioDstToSrc;
    bool bHasXOffVirtual = false;
    int nDestXOffVirtual = 0;
    if( fabs(dfDestXOff - static_cast<int>(dfDestXOff + 0.5)) < 1e-8 )
    {
        bHasXOffVirtual = true;
        dfXOff = nXOff;
        nDestXOffVirtual = static_cast<int>(dfDestXOff + 0.5);
    }

    double dfDestYOff = dfYOff / dfYRatioDstToSrc;
    bool bHasYOffVirtual = false;
    int nDestYOffVirtual = 0;
    if( fabs(dfDestYOff - static_cast<int>(dfDestYOff + 0.5)) < 1e-8 )
    {
        bHasYOffVirtual = true;
        dfYOff = nYOff;
        nDestYOffVirtual = static_cast<int>(dfDestYOff + 0.5);
    }

    // Create a MEM dataset that wraps the output buffer.
    GDALDataset* poMEMDS = MEMDataset::Create( "", nDestXOffVirtual + nBufXSize,
                                               nDestYOffVirtual + nBufYSize, 0,
                                               eBufType, nullptr );
    GDALRasterBand** papoDstBands =
        static_cast<GDALRasterBand **>(
            CPLMalloc( nBandCount * sizeof(GDALRasterBand*)) );
    for(int i=0;i<nBandCount;i++)
    {
        char szBuffer[32] = { '\0' };
        int nRet = CPLPrintPointer(
            szBuffer,
            static_cast<GByte*>(pData) - nPixelSpace * nDestXOffVirtual
            - nLineSpace * nDestYOffVirtual + nBandSpace * i, sizeof(szBuffer));
        szBuffer[nRet] = 0;

        char szBuffer0[64] = { '\0' };
        snprintf( szBuffer0, sizeof(szBuffer0),
                  "DATAPOINTER=%s", szBuffer );

        char szBuffer1[64] = { '\0' };
        snprintf( szBuffer1, sizeof(szBuffer1),
                  "PIXELOFFSET=" CPL_FRMT_GIB,
                  static_cast<GIntBig>(nPixelSpace) );

        char szBuffer2[64] = { '\0' };
        snprintf( szBuffer2, sizeof(szBuffer2),
                  "LINEOFFSET=" CPL_FRMT_GIB,
                  static_cast<GIntBig>(nLineSpace) );

        char* apszOptions[4] = { szBuffer0, szBuffer1, szBuffer2, nullptr };

        poMEMDS->AddBand(eBufType, apszOptions);

        GDALRasterBand* poSrcBand = GetRasterBand(panBandMap[i]);
        papoDstBands[i] = poMEMDS->GetRasterBand(i+1);
        const char* pszNBITS = poSrcBand->GetMetadataItem( "NBITS",
                                                           "IMAGE_STRUCTURE" );
        if( pszNBITS )
            poMEMDS->GetRasterBand(i+1)->SetMetadataItem( "NBITS", pszNBITS,
                                                          "IMAGE_STRUCTURE" );
    }

    CPLErr eErr = CE_None;

    // TODO(schwehr): Why disabled?  Why not just delete?
    // Looks like this code was initially added as disable by copying
    // from RasterIO here:
    // https://trac.osgeo.org/gdal/changeset/29572
#if 0
    // Do the resampling.
    if( bUseWarp )
    {
        VRTDatasetH hVRTDS = nullptr;
        GDALRasterBandH hVRTBand = nullptr;
        if( GetDataset() == nullptr )
        {
            /* Create VRT dataset that wraps the whole dataset */
            hVRTDS = VRTCreate(nRasterXSize, nRasterYSize);
            VRTAddBand( hVRTDS, eDataType, nullptr );
            hVRTBand = GDALGetRasterBand(hVRTDS, 1);
            VRTAddSimpleSource( (VRTSourcedRasterBandH)hVRTBand,
                                (GDALRasterBandH)this,
                                0, 0,
                                nRasterXSize, nRasterYSize,
                                0, 0,
                                nRasterXSize, nRasterYSize,
                                nullptr, VRT_NODATA_UNSET );

            /* Add a mask band if needed */
            if( GetMaskFlags() != GMF_ALL_VALID )
            {
                ((GDALDataset*)hVRTDS)->CreateMaskBand(0);
                VRTSourcedRasterBand* poVRTMaskBand =
                    (VRTSourcedRasterBand*)(((GDALRasterBand*)hVRTBand)->GetMaskBand());
                poVRTMaskBand->
                    AddMaskBandSource( this,
                                    0, 0,
                                    nRasterXSize, nRasterYSize,
                                    0, 0,
                                    nRasterXSize, nRasterYSize);
            }
        }

        GDALWarpOptions* psWarpOptions = GDALCreateWarpOptions();
        psWarpOptions->eResampleAlg = (GDALResampleAlg)psExtraArg->eResampleAlg;
        psWarpOptions->hSrcDS = (GDALDatasetH) (hVRTDS ? hVRTDS : GetDataset());
        psWarpOptions->hDstDS = (GDALDatasetH) poMEMDS;
        psWarpOptions->nBandCount = 1;
        int nSrcBandNumber = (hVRTDS ? 1 : nBand);
        int nDstBandNumber = 1;
        psWarpOptions->panSrcBands = &nSrcBandNumber;
        psWarpOptions->panDstBands = &nDstBandNumber;
        psWarpOptions->pfnProgress = psExtraArg->pfnProgress ?
                    psExtraArg->pfnProgress : GDALDummyProgress;
        psWarpOptions->pProgressArg = psExtraArg->pProgressData;
        psWarpOptions->pfnTransformer = GDALRasterIOTransformer;
        GDALRasterIOTransformerStruct sTransformer;
        sTransformer.dfXOff = bHasXOffVirtual ? 0 : dfXOff;
        sTransformer.dfYOff = bHasYOffVirtual ? 0 : dfYOff;
        sTransformer.dfXRatioDstToSrc = dfXRatioDstToSrc;
        sTransformer.dfYRatioDstToSrc = dfYRatioDstToSrc;
        psWarpOptions->pTransformerArg = &sTransformer;

        GDALWarpOperationH hWarpOperation = GDALCreateWarpOperation(psWarpOptions);
        eErr = GDALChunkAndWarpImage( hWarpOperation,
                                      nDestXOffVirtual, nDestYOffVirtual,
                                      nBufXSize, nBufYSize );
        GDALDestroyWarpOperation( hWarpOperation );

        psWarpOptions->panSrcBands = nullptr;
        psWarpOptions->panDstBands = nullptr;
        GDALDestroyWarpOptions( psWarpOptions );

        if( hVRTDS )
            GDALClose(hVRTDS);
    }
    else
#endif
    {
        const char* pszResampling =
            (psExtraArg->eResampleAlg == GRIORA_Bilinear) ? "BILINEAR" :
            (psExtraArg->eResampleAlg == GRIORA_Cubic) ? "CUBIC" :
            (psExtraArg->eResampleAlg == GRIORA_CubicSpline) ? "CUBICSPLINE" :
            (psExtraArg->eResampleAlg == GRIORA_Lanczos) ? "LANCZOS" :
            (psExtraArg->eResampleAlg == GRIORA_Average) ? "AVERAGE" :
            (psExtraArg->eResampleAlg == GRIORA_Mode) ? "MODE" :
            (psExtraArg->eResampleAlg == GRIORA_Gauss) ? "GAUSS" : "UNKNOWN";

        GDALRasterBand* poFirstSrcBand = GetRasterBand(panBandMap[0]);
        GDALDataType eDataType = poFirstSrcBand->GetRasterDataType();
        int nBlockXSize, nBlockYSize;
        poFirstSrcBand->GetBlockSize(&nBlockXSize, &nBlockYSize);

        int nKernelRadius;
        GDALResampleFunction pfnResampleFunc =
                        GDALGetResampleFunction(pszResampling, &nKernelRadius);
        CPLAssert(pfnResampleFunc);
#ifdef GDAL_ENABLE_RESAMPLING_MULTIBAND
        GDALResampleFunctionMultiBands pfnResampleFuncMultiBands =
                        GDALGetResampleFunctionMultiBands(pszResampling, &nKernelRadius);
#endif
        GDALDataType eWrkDataType =
            GDALGetOvrWorkDataType(pszResampling, eDataType);

        int nDstBlockXSize = nBufXSize;
        int nDstBlockYSize = nBufYSize;
        int nFullResXChunk, nFullResYChunk;
        while( true )
        {
            nFullResXChunk =
                3 + static_cast<int>(nDstBlockXSize * dfXRatioDstToSrc);
            nFullResYChunk =
                3 + static_cast<int>(nDstBlockYSize * dfYRatioDstToSrc);
            if( nFullResXChunk > nRasterXSize )
                nFullResXChunk = nRasterXSize;
            if( nFullResYChunk > nRasterYSize )
                nFullResYChunk = nRasterYSize;
            if( (nDstBlockXSize == 1 && nDstBlockYSize == 1) ||
                (static_cast<GIntBig>(nFullResXChunk) * nFullResYChunk <= 1024 * 1024) )
                break;
            // When operating on the full width of a raster whose block width is
            // the raster width, prefer doing chunks in height.
            if( nFullResXChunk >= nXSize && nXSize == nBlockXSize &&
                nDstBlockYSize > 1 )
                nDstBlockYSize /= 2;
            /* Otherwise cut the maximal dimension */
            else if( nDstBlockXSize > 1 &&
                     (nFullResXChunk > nFullResYChunk || nDstBlockYSize == 1) )
                nDstBlockXSize /= 2;
            else
                nDstBlockYSize /= 2;
        }

        int nOvrFactor = std::max( static_cast<int>(0.5 + dfXRatioDstToSrc),
                                   static_cast<int>(0.5 + dfYRatioDstToSrc) );
        if( nOvrFactor == 0 ) nOvrFactor = 1;
        int nFullResXSizeQueried = nFullResXChunk + 2 * nKernelRadius * nOvrFactor;
        int nFullResYSizeQueried = nFullResYChunk + 2 * nKernelRadius * nOvrFactor;

        if( nFullResXSizeQueried > nRasterXSize )
            nFullResXSizeQueried = nRasterXSize;
        if( nFullResYSizeQueried > nRasterYSize )
            nFullResYSizeQueried = nRasterYSize;

        void * pChunk =
            VSI_MALLOC3_VERBOSE(
                GDALGetDataTypeSizeBytes(eWrkDataType) * nBandCount,
                nFullResXSizeQueried, nFullResYSizeQueried );
        GByte * pabyChunkNoDataMask = nullptr;

        GDALRasterBand* poMaskBand = poFirstSrcBand->GetMaskBand();
        int nMaskFlags = poFirstSrcBand->GetMaskFlags();

        bool bUseNoDataMask = ((nMaskFlags & GMF_ALL_VALID) == 0);
        if (bUseNoDataMask)
        {
            pabyChunkNoDataMask = static_cast<GByte *>(
                VSI_MALLOC2_VERBOSE( nFullResXSizeQueried, nFullResYSizeQueried ));
        }
        if( pChunk == nullptr || (bUseNoDataMask && pabyChunkNoDataMask == nullptr) )
        {
            GDALClose(poMEMDS);
            CPLFree(pChunk);
            CPLFree(pabyChunkNoDataMask);
            CPLFree(papoDstBands);
            return CE_Failure;
        }

        int nTotalBlocks = ((nBufXSize + nDstBlockXSize - 1) / nDstBlockXSize) *
                           ((nBufYSize + nDstBlockYSize - 1) / nDstBlockYSize);
        int nBlocksDone = 0;

        int nDstYOff;
        for( nDstYOff = 0; nDstYOff < nBufYSize && eErr == CE_None;
            nDstYOff += nDstBlockYSize )
        {
            int nDstYCount;
            if  (nDstYOff + nDstBlockYSize <= nBufYSize)
                nDstYCount = nDstBlockYSize;
            else
                nDstYCount = nBufYSize - nDstYOff;

            int nChunkYOff =
                nYOff + static_cast<int>(nDstYOff * dfYRatioDstToSrc);
            int nChunkYOff2 =
                nYOff + 1 +
                static_cast<int>(
                    ceil((nDstYOff + nDstYCount) * dfYRatioDstToSrc) );
            if( nChunkYOff2 > nRasterYSize )
                nChunkYOff2 = nRasterYSize;
            int nYCount = nChunkYOff2 - nChunkYOff;
            CPLAssert(nYCount <= nFullResYChunk);

            int nChunkYOffQueried = nChunkYOff - nKernelRadius * nOvrFactor;
            int nChunkYSizeQueried = nYCount + 2 * nKernelRadius * nOvrFactor;
            if( nChunkYOffQueried < 0 )
            {
                nChunkYSizeQueried += nChunkYOffQueried;
                nChunkYOffQueried = 0;
            }
            if( nChunkYSizeQueried + nChunkYOffQueried > nRasterYSize )
                nChunkYSizeQueried = nRasterYSize - nChunkYOffQueried;
            CPLAssert(nChunkYSizeQueried <= nFullResYSizeQueried);

            int nDstXOff;
            for( nDstXOff = 0; nDstXOff < nBufXSize && eErr == CE_None;
                nDstXOff += nDstBlockXSize )
            {
                int nDstXCount;
                if  (nDstXOff + nDstBlockXSize <= nBufXSize)
                    nDstXCount = nDstBlockXSize;
                else
                    nDstXCount = nBufXSize - nDstXOff;

                int nChunkXOff =
                    nXOff + static_cast<int>(nDstXOff * dfXRatioDstToSrc);
                int nChunkXOff2 =
                    nXOff + 1 + static_cast<int>(
                        ceil((nDstXOff + nDstXCount) * dfXRatioDstToSrc) );
                if( nChunkXOff2 > nRasterXSize )
                    nChunkXOff2 = nRasterXSize;
                int nXCount = nChunkXOff2 - nChunkXOff;
                CPLAssert(nXCount <= nFullResXChunk);

                int nChunkXOffQueried = nChunkXOff - nKernelRadius * nOvrFactor;
                int nChunkXSizeQueried = nXCount + 2 * nKernelRadius * nOvrFactor;
                if( nChunkXOffQueried < 0 )
                {
                    nChunkXSizeQueried += nChunkXOffQueried;
                    nChunkXOffQueried = 0;
                }
                if( nChunkXSizeQueried + nChunkXOffQueried > nRasterXSize )
                    nChunkXSizeQueried = nRasterXSize - nChunkXOffQueried;
                CPLAssert(nChunkXSizeQueried <= nFullResXSizeQueried);

                bool bSkipResample = false;
                bool bNoDataMaskFullyOpaque = false;
                if (eErr == CE_None && bUseNoDataMask)
                {
                    eErr = poMaskBand->RasterIO( GF_Read,
                                                 nChunkXOffQueried,
                                                 nChunkYOffQueried,
                                                 nChunkXSizeQueried,
                                                 nChunkYSizeQueried,
                                                 pabyChunkNoDataMask,
                                                 nChunkXSizeQueried,
                                                 nChunkYSizeQueried,
                                                 GDT_Byte, 0, 0, nullptr );

                    /* Optimizations if mask if fully opaque or transparent */
                    const int nPixels = nChunkXSizeQueried * nChunkYSizeQueried;
                    const GByte bVal = pabyChunkNoDataMask[0];
                    int i = 1;  // Used after for.
                    for( ; i < nPixels; i++ )
                    {
                        if( pabyChunkNoDataMask[i] != bVal )
                            break;
                    }
                    if( i == nPixels )
                    {
                        if( bVal == 0 )
                        {
                            float fNoDataValue = 0.0f;
                            for( int iBand = 0; iBand < nBandCount; iBand++ )
                            {
                                for( int j = 0; j < nDstYCount; j++ )
                                {
                                    GDALCopyWords(
                                        &fNoDataValue, GDT_Float32, 0,
                                        static_cast<GByte *>(pData) +
                                        iBand * nBandSpace +
                                        nLineSpace * (j + nDstYOff) +
                                        nDstXOff * nPixelSpace,
                                        eBufType,
                                        static_cast<int>(nPixelSpace),
                                        nDstXCount);
                                }
                            }
                            bSkipResample = true;
                        }
                        else
                        {
                            bNoDataMaskFullyOpaque = true;
                        }
                    }
                }

                if( !bSkipResample && eErr == CE_None )
                {
                    /* Read the source buffers */
                    eErr = RasterIO( GF_Read,
                                     nChunkXOffQueried, nChunkYOffQueried,
                                     nChunkXSizeQueried, nChunkYSizeQueried,
                                     pChunk,
                                     nChunkXSizeQueried, nChunkYSizeQueried,
                                     eWrkDataType,
                                     nBandCount, panBandMap,
                                     0, 0, 0, nullptr );
                }

#ifdef GDAL_ENABLE_RESAMPLING_MULTIBAND
                if( pfnResampleFuncMultiBands &&
                    !bSkipResample &&
                    eErr == CE_None )
                {
                    eErr = pfnResampleFuncMultiBands(
                        dfXRatioDstToSrc,
                        dfYRatioDstToSrc,
                        dfXOff - nXOff, /* == 0 if bHasXOffVirtual */
                        dfYOff - nYOff, /* == 0 if bHasYOffVirtual */
                        eWrkDataType,
                        (GByte*)pChunk,
                        nBandCount,
                        bNoDataMaskFullyOpaque ? nullptr : pabyChunkNoDataMask,
                        nChunkXOffQueried - (bHasXOffVirtual ? 0 : nXOff),
                        nChunkXSizeQueried,
                        nChunkYOffQueried - (bHasYOffVirtual ? 0 : nYOff),
                        nChunkYSizeQueried,
                        nDstXOff + nDestXOffVirtual,
                        nDstXOff + nDestXOffVirtual + nDstXCount,
                        nDstYOff + nDestYOffVirtual,
                        nDstYOff + nDestYOffVirtual + nDstYCount,
                        papoDstBands,
                        pszResampling,
                        FALSE /*bHasNoData*/,
                        0.f /* fNoDataValue */,
                        nullptr /* color table*/,
                        eDataType );
                }
                else
#endif
                {
                    size_t nChunkBandOffset =
                        static_cast<size_t>(nChunkXSizeQueried) *
                        nChunkYSizeQueried *
                        GDALGetDataTypeSizeBytes(eWrkDataType);
                    for( int i = 0;
                         i < nBandCount && !bSkipResample && eErr == CE_None;
                         i++ )
                    {
                        const bool bPropagateNoData = false;
                        eErr = pfnResampleFunc(
                            dfXRatioDstToSrc,
                            dfYRatioDstToSrc,
                            dfXOff - nXOff, /* == 0 if bHasXOffVirtual */
                            dfYOff - nYOff, /* == 0 if bHasYOffVirtual */
                            eWrkDataType,
                            reinterpret_cast<GByte*>(pChunk) + i * nChunkBandOffset,
                            bNoDataMaskFullyOpaque ? nullptr : pabyChunkNoDataMask,
                            nChunkXOffQueried - (bHasXOffVirtual ? 0 : nXOff),
                            nChunkXSizeQueried,
                            nChunkYOffQueried - (bHasYOffVirtual ? 0 : nYOff),
                            nChunkYSizeQueried,
                            nDstXOff + nDestXOffVirtual,
                            nDstXOff + nDestXOffVirtual + nDstXCount,
                            nDstYOff + nDestYOffVirtual,
                            nDstYOff + nDestYOffVirtual + nDstYCount,
                            poMEMDS->GetRasterBand(i+1),
                            pszResampling,
                            FALSE /*bHasNoData*/,
                            0.f /* fNoDataValue */,
                            nullptr /* color table*/,
                            eDataType,
                            bPropagateNoData );
                    }
                }

                nBlocksDone ++;
                if( eErr == CE_None && psExtraArg->pfnProgress != nullptr &&
                    !psExtraArg->pfnProgress(
                        1.0 * nBlocksDone / nTotalBlocks, "",
                        psExtraArg->pProgressData) )
                {
                    eErr = CE_Failure;
                }
            }
        }

        CPLFree(pChunk);
        CPLFree(pabyChunkNoDataMask);
    }

    CPLFree(papoDstBands);
    GDALClose(poMEMDS);

    return eErr;
}
//! @endcond

/************************************************************************/
/*                           GDALSwapWords()                            */
/************************************************************************/

/**
 * Byte swap words in-place.
 *
 * This function will byte swap a set of 2, 4 or 8 byte words "in place" in
 * a memory array.  No assumption is made that the words being swapped are
 * word aligned in memory.  Use the CPL_LSB and CPL_MSB macros from cpl_port.h
 * to determine if the current platform is big endian or little endian.  Use
 * The macros like CPL_SWAP32() to byte swap single values without the overhead
 * of a function call.
 *
 * @param pData pointer to start of data buffer.
 * @param nWordSize size of words being swapped in bytes. Normally 2, 4 or 8.
 * @param nWordCount the number of words to be swapped in this call.
 * @param nWordSkip the byte offset from the start of one word to the start of
 * the next. For packed buffers this is the same as nWordSize.
 */

void CPL_STDCALL GDALSwapWords( void *pData, int nWordSize, int nWordCount,
                                int nWordSkip )

{
    if (nWordCount > 0)
        VALIDATE_POINTER0( pData , "GDALSwapWords" );

    GByte *pabyData = static_cast<GByte *>( pData );

    switch( nWordSize )
    {
      case 1:
        break;

      case 2:
        CPLAssert( nWordSkip >= 2 || nWordCount == 1 );
        for( int i = 0; i < nWordCount; i++ )
        {
            CPL_SWAP16PTR(pabyData);
            pabyData += nWordSkip;
        }
        break;

      case 4:
        CPLAssert( nWordSkip >= 4 || nWordCount == 1 );
        if( CPL_IS_ALIGNED(pabyData, 4) && (nWordSkip % 4) == 0 )
        {
            for( int i = 0; i < nWordCount; i++ )
            {
                *reinterpret_cast<GUInt32*>(pabyData) = CPL_SWAP32(*reinterpret_cast<const GUInt32*>(pabyData));
                pabyData += nWordSkip;
            }
        }
        else
        {
            for( int i = 0; i < nWordCount; i++ )
            {
                CPL_SWAP32PTR(pabyData);
                pabyData += nWordSkip;
            }
        }
        break;

      case 8:
        CPLAssert( nWordSkip >= 8 || nWordCount == 1 );
#ifdef CPL_HAS_GINT64
        if( CPL_IS_ALIGNED(pabyData, 8) && (nWordSkip % 8) == 0 )
        {
            for( int i = 0; i < nWordCount; i++ )
            {
                *reinterpret_cast<GUInt64*>(pabyData) = CPL_SWAP64(*reinterpret_cast<const GUInt64*>(pabyData));
                pabyData += nWordSkip;
            }
        }
        else
#endif
        {
            for( int i = 0; i < nWordCount; i++ )
            {
                CPL_SWAP64PTR(pabyData);
                pabyData += nWordSkip;
            }
        }
        break;

      default:
        CPLAssert( false );
    }
}

/************************************************************************/
/*                           GDALSwapWordsEx()                          */
/************************************************************************/

/**
 * Byte swap words in-place.
 *
 * This function will byte swap a set of 2, 4 or 8 byte words "in place" in
 * a memory array.  No assumption is made that the words being swapped are
 * word aligned in memory.  Use the CPL_LSB and CPL_MSB macros from cpl_port.h
 * to determine if the current platform is big endian or little endian.  Use
 * The macros like CPL_SWAP32() to byte swap single values without the overhead
 * of a function call.
 *
 * @param pData pointer to start of data buffer.
 * @param nWordSize size of words being swapped in bytes. Normally 2, 4 or 8.
 * @param nWordCount the number of words to be swapped in this call.
 * @param nWordSkip the byte offset from the start of one word to the start of
 * the next. For packed buffers this is the same as nWordSize.
 * @since GDAL 2.1
 */
void CPL_STDCALL GDALSwapWordsEx( void *pData, int nWordSize, size_t nWordCount,
                                  int nWordSkip )
{
    GByte* pabyData = static_cast<GByte*>(pData);
    while( nWordCount )
    {
        // Pick-up a multiple of 8 as max chunk size.
        const int nWordCountSmall =
            (nWordCount > (1 << 30)) ? (1 << 30) : static_cast<int>(nWordCount);
        GDALSwapWords(pabyData, nWordSize, nWordCountSmall, nWordSkip);
        pabyData += static_cast<size_t>(nWordSkip) * nWordCountSmall;
        nWordCount -= nWordCountSmall;
    }
}

// Place the new GDALCopyWords helpers in an anonymous namespace
namespace {

/************************************************************************/
/*                           GDALCopyWordsT()                           */
/************************************************************************/
/**
 * Template function, used to copy data from pSrcData into buffer
 * pDstData, with stride nSrcPixelStride in the source data and
 * stride nDstPixelStride in the destination data. This template can
 * deal with the case where the input data type is real or complex and
 * the output is real.
 *
 * @param pSrcData the source data buffer
 * @param nSrcPixelStride the stride, in the buffer pSrcData for pixels
 *                      of interest.
 * @param pDstData the destination buffer.
 * @param nDstPixelStride the stride in the buffer pDstData for pixels of
 *                      interest.
 * @param nWordCount the total number of pixel words to copy
 *
 * @code
 * // Assume an input buffer of type GUInt16 named pBufferIn
 * GByte *pBufferOut = new GByte[numBytesOut];
 * GDALCopyWordsT<GUInt16, GByte>(pSrcData, 2, pDstData, 1, numBytesOut);
 * @endcode
 * @note
 * This is a private function, and should not be exposed outside of rasterio.cpp.
 * External users should call the GDALCopyWords driver function.
 */

template <class Tin, class Tout>
static void inline GDALCopyWordsGenericT( const Tin* const CPL_RESTRICT pSrcData,
                            int nSrcPixelStride,
                            Tout* const CPL_RESTRICT pDstData,
                            int nDstPixelStride,
                            int nWordCount)
{
    std::ptrdiff_t nDstOffset = 0;

    const char* const pSrcDataPtr = reinterpret_cast<const char*>(pSrcData);
    char* const pDstDataPtr = reinterpret_cast<char*>(pDstData);
    for (std::ptrdiff_t n = 0; n < nWordCount; n++  )
    {
        const Tin tValue =
            *reinterpret_cast<const Tin*>(pSrcDataPtr + (n * nSrcPixelStride));
        Tout* const pOutPixel =
            reinterpret_cast<Tout*>(pDstDataPtr + nDstOffset);

        GDALCopyWord(tValue, *pOutPixel);

        nDstOffset += nDstPixelStride;
    }
}

template <class Tin, class Tout>
static void inline GDALCopyWordsT( const Tin* const CPL_RESTRICT pSrcData,
                            int nSrcPixelStride,
                            Tout* const CPL_RESTRICT pDstData,
                            int nDstPixelStride,
                            int nWordCount)
{
    GDALCopyWordsGenericT(pSrcData, nSrcPixelStride,
                          pDstData, nDstPixelStride,
                          nWordCount);
}

template <class Tin, class Tout>
static void inline GDALCopyWordsT_8atatime( const Tin* const CPL_RESTRICT pSrcData,
                                     int nSrcPixelStride,
                                     Tout* const CPL_RESTRICT pDstData,
                                     int nDstPixelStride,
                                     int nWordCount )
{
    std::ptrdiff_t nDstOffset = 0;

    const char* const pSrcDataPtr = reinterpret_cast<const char*>(pSrcData);
    char* const pDstDataPtr = reinterpret_cast<char*>(pDstData);
    std::ptrdiff_t n = 0;
    if( nSrcPixelStride == static_cast<int>(sizeof(Tin)) &&
        nDstPixelStride == static_cast<int>(sizeof(Tout)) )
    {
        for (; n < nWordCount-7; n+=8)
        {
            const Tin* pInValues =
                reinterpret_cast<const Tin*>(pSrcDataPtr + (n * nSrcPixelStride));
            Tout* const pOutPixels =
                reinterpret_cast<Tout*>(pDstDataPtr + nDstOffset);

            GDALCopy8Words(pInValues, pOutPixels);

            nDstOffset += 8 * nDstPixelStride;
        }
    }
    for( ; n < nWordCount; n++  )
    {
        const Tin tValue =
            *reinterpret_cast<const Tin*>(pSrcDataPtr + (n * nSrcPixelStride));
        Tout* const pOutPixel =
            reinterpret_cast<Tout*>(pDstDataPtr + nDstOffset);

        GDALCopyWord(tValue, *pOutPixel);

        nDstOffset += nDstPixelStride;
    }
}

#if defined(__x86_64) || defined(_M_X64)

#include <emmintrin.h>

template<class Tout> void GDALCopyWordsByteTo16Bit(
                                const GByte* const CPL_RESTRICT pSrcData,
                                int nSrcPixelStride,
                                Tout* const CPL_RESTRICT pDstData,
                                int nDstPixelStride,
                                int nWordCount )
{
    if( nSrcPixelStride == static_cast<int>(sizeof(*pSrcData)) &&
        nDstPixelStride == static_cast<int>(sizeof(*pDstData)) )
    {
        int n = 0;
        const __m128i xmm_zero = _mm_setzero_si128 ();
        GByte* CPL_RESTRICT pabyDstDataPtr = reinterpret_cast<GByte*>(pDstData);
        for (; n < nWordCount-15; n+=16)
        {
            __m128i xmm = _mm_loadu_si128(
                reinterpret_cast<const __m128i*> (pSrcData + n) );
            __m128i xmm0 = _mm_unpacklo_epi8(xmm, xmm_zero);
            __m128i xmm1 = _mm_unpackhi_epi8(xmm, xmm_zero);
            _mm_storeu_si128( reinterpret_cast<__m128i*>(pabyDstDataPtr + n * 2),
                              xmm0 );
            _mm_storeu_si128( reinterpret_cast<__m128i*>(pabyDstDataPtr + n * 2 + 16),
                              xmm1 );
        }
        for( ; n < nWordCount; n++  )
        {
            pDstData[n] = pSrcData[n];
        }
    }
    else
    {
        GDALCopyWordsGenericT(pSrcData, nSrcPixelStride,
                              pDstData, nDstPixelStride,
                              nWordCount);
    }
}

template<> void GDALCopyWordsT( const GByte* const CPL_RESTRICT pSrcData,
                                int nSrcPixelStride,
                                GUInt16* const CPL_RESTRICT pDstData,
                                int nDstPixelStride,
                                int nWordCount )
{
    GDALCopyWordsByteTo16Bit(pSrcData, nSrcPixelStride, pDstData,
                             nDstPixelStride, nWordCount);
}

template<> void GDALCopyWordsT( const GByte* const CPL_RESTRICT pSrcData,
                                int nSrcPixelStride,
                                GInt16* const CPL_RESTRICT pDstData,
                                int nDstPixelStride,
                                int nWordCount )
{
    GDALCopyWordsByteTo16Bit(pSrcData, nSrcPixelStride, pDstData,
                             nDstPixelStride, nWordCount);
}

template<class Tout> void GDALCopyWordsByteTo32Bit(
                                const GByte* const CPL_RESTRICT pSrcData,
                                int nSrcPixelStride,
                                Tout* const CPL_RESTRICT pDstData,
                                int nDstPixelStride,
                                int nWordCount )
{
    if( nSrcPixelStride == static_cast<int>(sizeof(*pSrcData)) &&
        nDstPixelStride == static_cast<int>(sizeof(*pDstData)) )
    {
        int n = 0;
        const __m128i xmm_zero = _mm_setzero_si128 ();
        GByte* CPL_RESTRICT pabyDstDataPtr = reinterpret_cast<GByte*>(pDstData);
        for (; n < nWordCount-15; n+=16)
        {
            __m128i xmm = _mm_loadu_si128(
                reinterpret_cast<const __m128i*> (pSrcData + n) );
            __m128i xmm_low = _mm_unpacklo_epi8(xmm, xmm_zero);
            __m128i xmm_high= _mm_unpackhi_epi8(xmm, xmm_zero);
            __m128i xmm0 = _mm_unpacklo_epi16(xmm_low, xmm_zero);
            __m128i xmm1 = _mm_unpackhi_epi16(xmm_low, xmm_zero);
            __m128i xmm2 = _mm_unpacklo_epi16(xmm_high, xmm_zero);
            __m128i xmm3 = _mm_unpackhi_epi16(xmm_high, xmm_zero);
            _mm_storeu_si128( reinterpret_cast<__m128i*>(pabyDstDataPtr + n * 4),
                              xmm0 );
            _mm_storeu_si128( reinterpret_cast<__m128i*>(pabyDstDataPtr + n * 4 + 16),
                              xmm1 );
            _mm_storeu_si128( reinterpret_cast<__m128i*>(pabyDstDataPtr + n * 4 + 32),
                              xmm2 );
            _mm_storeu_si128( reinterpret_cast<__m128i*>(pabyDstDataPtr + n * 4 + 48),
                              xmm3 );
        }
        for( ; n < nWordCount; n++  )
        {
            pDstData[n] = pSrcData[n];
        }
    }
    else
    {
        GDALCopyWordsGenericT(pSrcData, nSrcPixelStride,
                              pDstData, nDstPixelStride,
                              nWordCount);
    }
}

template<> void GDALCopyWordsT( const GByte* const CPL_RESTRICT pSrcData,
                                int nSrcPixelStride,
                                GUInt32* const CPL_RESTRICT pDstData,
                                int nDstPixelStride,
                                int nWordCount )
{
    GDALCopyWordsByteTo32Bit(pSrcData, nSrcPixelStride, pDstData,
                             nDstPixelStride, nWordCount);
}

template<> void GDALCopyWordsT( const GByte* const CPL_RESTRICT pSrcData,
                                int nSrcPixelStride,
                                GInt32* const CPL_RESTRICT pDstData,
                                int nDstPixelStride,
                                int nWordCount )
{
    GDALCopyWordsByteTo32Bit(pSrcData, nSrcPixelStride, pDstData,
                             nDstPixelStride, nWordCount);
}

template<> void GDALCopyWordsT( const GByte* const CPL_RESTRICT pSrcData,
                                int nSrcPixelStride,
                                float* const CPL_RESTRICT pDstData,
                                int nDstPixelStride,
                                int nWordCount )
{
    if( nSrcPixelStride == static_cast<int>(sizeof(*pSrcData)) &&
        nDstPixelStride == static_cast<int>(sizeof(*pDstData)) )
    {
        int n = 0;
        const __m128i xmm_zero = _mm_setzero_si128 ();
        GByte* CPL_RESTRICT pabyDstDataPtr = reinterpret_cast<GByte*>(pDstData);
        for (; n < nWordCount-15; n+=16)
        {
            __m128i xmm = _mm_loadu_si128(
                reinterpret_cast<const __m128i*> (pSrcData + n) );
            __m128i xmm_low = _mm_unpacklo_epi8(xmm, xmm_zero);
            __m128i xmm_high= _mm_unpackhi_epi8(xmm, xmm_zero);
            __m128i xmm0 = _mm_unpacklo_epi16(xmm_low, xmm_zero);
            __m128i xmm1 = _mm_unpackhi_epi16(xmm_low, xmm_zero);
            __m128i xmm2 = _mm_unpacklo_epi16(xmm_high, xmm_zero);
            __m128i xmm3 = _mm_unpackhi_epi16(xmm_high, xmm_zero);
            __m128 xmm0_f = _mm_cvtepi32_ps(xmm0);
            __m128 xmm1_f = _mm_cvtepi32_ps(xmm1);
            __m128 xmm2_f = _mm_cvtepi32_ps(xmm2);
            __m128 xmm3_f = _mm_cvtepi32_ps(xmm3);
            _mm_storeu_ps( reinterpret_cast<float*>(pabyDstDataPtr + n * 4),
                           xmm0_f );
            _mm_storeu_ps( reinterpret_cast<float*>(pabyDstDataPtr + n * 4 + 16),
                           xmm1_f );
            _mm_storeu_ps( reinterpret_cast<float*>(pabyDstDataPtr + n * 4 + 32),
                           xmm2_f );
            _mm_storeu_ps( reinterpret_cast<float*>(pabyDstDataPtr + n * 4 + 48),
                           xmm3_f );
        }
        for( ; n < nWordCount; n++  )
        {
            pDstData[n] = pSrcData[n];
        }
    }
    else
    {
        GDALCopyWordsGenericT(pSrcData, nSrcPixelStride,
                              pDstData, nDstPixelStride,
                              nWordCount);
    }
}

template<> void GDALCopyWordsT( const GByte* const CPL_RESTRICT pSrcData,
                                int nSrcPixelStride,
                                double* const CPL_RESTRICT pDstData,
                                int nDstPixelStride,
                                int nWordCount )
{
    if( nSrcPixelStride == static_cast<int>(sizeof(*pSrcData)) &&
        nDstPixelStride == static_cast<int>(sizeof(*pDstData)) )
    {
        int n = 0;
        const __m128i xmm_zero = _mm_setzero_si128 ();
        GByte* CPL_RESTRICT pabyDstDataPtr = reinterpret_cast<GByte*>(pDstData);
        for (; n < nWordCount-15; n+=16)
        {
            __m128i xmm = _mm_loadu_si128(
                reinterpret_cast<const __m128i*> (pSrcData + n) );
            __m128i xmm_low = _mm_unpacklo_epi8(xmm, xmm_zero);
            __m128i xmm_high= _mm_unpackhi_epi8(xmm, xmm_zero);
            __m128i xmm0 = _mm_unpacklo_epi16(xmm_low, xmm_zero);
            __m128i xmm1 = _mm_unpackhi_epi16(xmm_low, xmm_zero);
            __m128i xmm2 = _mm_unpacklo_epi16(xmm_high, xmm_zero);
            __m128i xmm3 = _mm_unpackhi_epi16(xmm_high, xmm_zero);

            __m128d xmm0_low_d = _mm_cvtepi32_pd(xmm0);
            __m128d xmm1_low_d = _mm_cvtepi32_pd(xmm1);
            __m128d xmm2_low_d = _mm_cvtepi32_pd(xmm2);
            __m128d xmm3_low_d = _mm_cvtepi32_pd(xmm3);
            xmm0 = _mm_srli_si128(xmm0, 8);
            xmm1 = _mm_srli_si128(xmm1, 8);
            xmm2 = _mm_srli_si128(xmm2, 8);
            xmm3 = _mm_srli_si128(xmm3, 8);
            __m128d xmm0_high_d = _mm_cvtepi32_pd(xmm0);
            __m128d xmm1_high_d = _mm_cvtepi32_pd(xmm1);
            __m128d xmm2_high_d = _mm_cvtepi32_pd(xmm2);
            __m128d xmm3_high_d = _mm_cvtepi32_pd(xmm3);

            _mm_storeu_pd( reinterpret_cast<double*>(pabyDstDataPtr + n * 8),
                           xmm0_low_d );
            _mm_storeu_pd( reinterpret_cast<double*>(pabyDstDataPtr + n * 8 + 16),
                           xmm0_high_d );
            _mm_storeu_pd( reinterpret_cast<double*>(pabyDstDataPtr + n * 8 + 32),
                           xmm1_low_d );
            _mm_storeu_pd( reinterpret_cast<double*>(pabyDstDataPtr + n * 8 + 48),
                           xmm1_high_d );
            _mm_storeu_pd( reinterpret_cast<double*>(pabyDstDataPtr + n * 8 + 64),
                           xmm2_low_d );
            _mm_storeu_pd( reinterpret_cast<double*>(pabyDstDataPtr + n * 8 + 80),
                           xmm2_high_d );
            _mm_storeu_pd( reinterpret_cast<double*>(pabyDstDataPtr + n * 8 + 96),
                           xmm3_low_d );
            _mm_storeu_pd( reinterpret_cast<double*>(pabyDstDataPtr + n * 8 + 112),
                           xmm3_high_d );
        }
        for( ; n < nWordCount; n++  )
        {
            pDstData[n] = pSrcData[n];
        }
    }
    else
    {
        GDALCopyWordsGenericT(pSrcData, nSrcPixelStride,
                              pDstData, nDstPixelStride,
                              nWordCount);
    }
}

template<> void GDALCopyWordsT( const GUInt16* const CPL_RESTRICT pSrcData,
                                int nSrcPixelStride,
                                GByte* const CPL_RESTRICT pDstData,
                                int nDstPixelStride,
                                int nWordCount )
{
    if( nSrcPixelStride == static_cast<int>(sizeof(*pSrcData)) &&
        nDstPixelStride == static_cast<int>(sizeof(*pDstData)) )
    {
        int n = 0;
        // In SSE2, min_epu16 does not exist, so shift from
        // UInt16 to SInt16 to be able to use min_epi16
        const __m128i xmm_UINT16_to_INT16 = _mm_set1_epi16 (-32768);
        const __m128i xmm_m255_shifted = _mm_set1_epi16 (255-32768);
        for (; n < nWordCount-7; n+=8)
        {
            __m128i xmm = _mm_loadu_si128(
                reinterpret_cast<const __m128i*> (pSrcData + n) );
            xmm = _mm_add_epi16(xmm, xmm_UINT16_to_INT16);
            xmm = _mm_min_epi16(xmm, xmm_m255_shifted);
            xmm = _mm_sub_epi16(xmm, xmm_UINT16_to_INT16);
            xmm = _mm_packus_epi16(xmm, xmm);
            GDALCopyXMMToInt64(xmm, reinterpret_cast<GInt64*>(pDstData + n));
        }
        for( ; n < nWordCount; n++  )
        {
            pDstData[n] =
                pSrcData[n] >= 255 ? 255 : static_cast<GByte>(pSrcData[n]);
        }
    }
    else
    {
        GDALCopyWordsGenericT(pSrcData, nSrcPixelStride,
                              pDstData, nDstPixelStride,
                              nWordCount);
    }
}

template<> void GDALCopyWordsT( const GUInt16* const CPL_RESTRICT pSrcData,
                                int nSrcPixelStride,
                                GInt16* const CPL_RESTRICT pDstData,
                                int nDstPixelStride,
                                int nWordCount )
{
    if( nSrcPixelStride == static_cast<int>(sizeof(*pSrcData)) &&
        nDstPixelStride == static_cast<int>(sizeof(*pDstData)) )
    {
        int n = 0;
        // In SSE2, min_epu16 does not exist, so shift from
        // UInt16 to SInt16 to be able to use min_epi16
        const __m128i xmm_UINT16_to_INT16 = _mm_set1_epi16 (-32768);
        const __m128i xmm_32767_shifted = _mm_set1_epi16 (32767-32768);
        for (; n < nWordCount-7; n+=8)
        {
            __m128i xmm = _mm_loadu_si128(
                reinterpret_cast<const __m128i*> (pSrcData + n) );
            xmm = _mm_add_epi16(xmm, xmm_UINT16_to_INT16);
            xmm = _mm_min_epi16(xmm, xmm_32767_shifted);
            xmm = _mm_sub_epi16(xmm, xmm_UINT16_to_INT16);
            _mm_storeu_si128( reinterpret_cast<__m128i*>(pDstData + n),
                              xmm );
        }
        for( ; n < nWordCount; n++  )
        {
            pDstData[n] =
                pSrcData[n] >= 32767 ? 32767 : static_cast<GInt16>(pSrcData[n]);
        }
    }
    else
    {
        GDALCopyWordsGenericT(pSrcData, nSrcPixelStride,
                              pDstData, nDstPixelStride,
                              nWordCount);
    }
}

template<> void GDALCopyWordsT( const GUInt16* const CPL_RESTRICT pSrcData,
                                int nSrcPixelStride,
                                float* const CPL_RESTRICT pDstData,
                                int nDstPixelStride,
                                int nWordCount )
{
    if( nSrcPixelStride == static_cast<int>(sizeof(*pSrcData)) &&
        nDstPixelStride == static_cast<int>(sizeof(*pDstData)) )
    {
        int n = 0;
        const __m128i xmm_zero = _mm_setzero_si128 ();
        GByte* CPL_RESTRICT pabyDstDataPtr = reinterpret_cast<GByte*>(pDstData);
        for (; n < nWordCount-7; n+=8)
        {
            __m128i xmm = _mm_loadu_si128(
                reinterpret_cast<const __m128i*> (pSrcData + n) );
            __m128i xmm0 = _mm_unpacklo_epi16(xmm, xmm_zero);
            __m128i xmm1 = _mm_unpackhi_epi16(xmm, xmm_zero);
            __m128 xmm0_f = _mm_cvtepi32_ps(xmm0);
            __m128 xmm1_f = _mm_cvtepi32_ps(xmm1);
            _mm_storeu_ps( reinterpret_cast<float*>(pabyDstDataPtr + n * 4),
                           xmm0_f );
            _mm_storeu_ps( reinterpret_cast<float*>(pabyDstDataPtr + n * 4 + 16),
                           xmm1_f );
        }
        for( ; n < nWordCount; n++  )
        {
            pDstData[n] = pSrcData[n];
        }
    }
    else
    {
        GDALCopyWordsGenericT(pSrcData, nSrcPixelStride,
                              pDstData, nDstPixelStride,
                              nWordCount);
    }
}

template<> void GDALCopyWordsT( const GUInt16* const CPL_RESTRICT pSrcData,
                                int nSrcPixelStride,
                                double* const CPL_RESTRICT pDstData,
                                int nDstPixelStride,
                                int nWordCount )
{
    if( nSrcPixelStride == static_cast<int>(sizeof(*pSrcData)) &&
        nDstPixelStride == static_cast<int>(sizeof(*pDstData)) )
    {
        int n = 0;
        const __m128i xmm_zero = _mm_setzero_si128 ();
        GByte* CPL_RESTRICT pabyDstDataPtr = reinterpret_cast<GByte*>(pDstData);
        for (; n < nWordCount-7; n+=8)
        {
            __m128i xmm = _mm_loadu_si128(
                reinterpret_cast<const __m128i*> (pSrcData + n) );
            __m128i xmm0 = _mm_unpacklo_epi16(xmm, xmm_zero);
            __m128i xmm1 = _mm_unpackhi_epi16(xmm, xmm_zero);

            __m128d xmm0_low_d = _mm_cvtepi32_pd(xmm0);
            __m128d xmm1_low_d = _mm_cvtepi32_pd(xmm1);
            xmm0 = _mm_srli_si128(xmm0, 8);
            xmm1 = _mm_srli_si128(xmm1, 8);
            __m128d xmm0_high_d = _mm_cvtepi32_pd(xmm0);
            __m128d xmm1_high_d = _mm_cvtepi32_pd(xmm1);

            _mm_storeu_pd( reinterpret_cast<double*>(pabyDstDataPtr + n * 8),
                           xmm0_low_d );
            _mm_storeu_pd( reinterpret_cast<double*>(pabyDstDataPtr + n * 8 + 16),
                           xmm0_high_d );
            _mm_storeu_pd( reinterpret_cast<double*>(pabyDstDataPtr + n * 8 + 32),
                           xmm1_low_d );
            _mm_storeu_pd( reinterpret_cast<double*>(pabyDstDataPtr + n * 8 + 48),
                           xmm1_high_d );
        }
        for( ; n < nWordCount; n++  )
        {
            pDstData[n] = pSrcData[n];
        }
    }
    else
    {
        GDALCopyWordsGenericT(pSrcData, nSrcPixelStride,
                              pDstData, nDstPixelStride,
                              nWordCount);
    }
}

template<> void GDALCopyWordsT( const double* const CPL_RESTRICT pSrcData,
                                int nSrcPixelStride,
                                GUInt16* const CPL_RESTRICT pDstData,
                                int nDstPixelStride,
                                int nWordCount )
{
    GDALCopyWordsT_8atatime( pSrcData, nSrcPixelStride,
                             pDstData, nDstPixelStride, nWordCount );
}

#endif // defined(__x86_64) || defined(_M_X64)

template<> void GDALCopyWordsT( const float* const CPL_RESTRICT pSrcData,
                                int nSrcPixelStride,
                                GByte* const CPL_RESTRICT pDstData,
                                int nDstPixelStride,
                                int nWordCount )
{
    GDALCopyWordsT_8atatime( pSrcData, nSrcPixelStride,
                             pDstData, nDstPixelStride, nWordCount );
}

template<> void GDALCopyWordsT( const float* const CPL_RESTRICT pSrcData,
                                int nSrcPixelStride,
                                GInt16* const CPL_RESTRICT pDstData,
                                int nDstPixelStride,
                                int nWordCount )
{
    GDALCopyWordsT_8atatime( pSrcData, nSrcPixelStride,
                             pDstData, nDstPixelStride, nWordCount );
}

template<> void GDALCopyWordsT( const float* const CPL_RESTRICT pSrcData,
                                int nSrcPixelStride,
                                GUInt16* const CPL_RESTRICT pDstData,
                                int nDstPixelStride,
                                int nWordCount )
{
    GDALCopyWordsT_8atatime( pSrcData, nSrcPixelStride,
                             pDstData, nDstPixelStride, nWordCount );
}

/************************************************************************/
/*                   GDALCopyWordsComplexT()                            */
/************************************************************************/
/**
 * Template function, used to copy data from pSrcData into buffer
 * pDstData, with stride nSrcPixelStride in the source data and
 * stride nDstPixelStride in the destination data. Deals with the
 * complex case, where input is complex and output is complex.
 *
 * @param pSrcData the source data buffer
 * @param nSrcPixelStride the stride, in the buffer pSrcData for pixels
 *                      of interest.
 * @param pDstData the destination buffer.
 * @param nDstPixelStride the stride in the buffer pDstData for pixels of
 *                      interest.
 * @param nWordCount the total number of pixel words to copy
 *
 */
template <class Tin, class Tout>
inline void GDALCopyWordsComplexT( const Tin* const CPL_RESTRICT pSrcData,
                                   int nSrcPixelStride,
                                   Tout* const CPL_RESTRICT pDstData,
                                   int nDstPixelStride,
                                   int nWordCount )
{
    std::ptrdiff_t nDstOffset = 0;
    const char* const pSrcDataPtr = reinterpret_cast<const char*>(pSrcData);
    char* const pDstDataPtr = reinterpret_cast<char*>(pDstData);

    for (std::ptrdiff_t n = 0; n < nWordCount; n++)
    {
        const Tin* const pPixelIn =
            reinterpret_cast<const Tin*>(pSrcDataPtr + n * nSrcPixelStride);
        Tout* const pPixelOut =
            reinterpret_cast<Tout*>(pDstDataPtr + nDstOffset);

        GDALCopyWord(pPixelIn[0], pPixelOut[0]);
        GDALCopyWord(pPixelIn[1], pPixelOut[1]);

        nDstOffset += nDstPixelStride;
    }
}

/************************************************************************/
/*                   GDALCopyWordsComplexOutT()                         */
/************************************************************************/
/**
 * Template function, used to copy data from pSrcData into buffer
 * pDstData, with stride nSrcPixelStride in the source data and
 * stride nDstPixelStride in the destination data. Deals with the
 * case where the value is real coming in, but complex going out.
 *
 * @param pSrcData the source data buffer
 * @param nSrcPixelStride the stride, in the buffer pSrcData for pixels
 *                      of interest, in bytes.
 * @param pDstData the destination buffer.
 * @param nDstPixelStride the stride in the buffer pDstData for pixels of
 *                      interest, in bytes.
 * @param nWordCount the total number of pixel words to copy
 *
 */
template <class Tin, class Tout>
inline void GDALCopyWordsComplexOutT( const Tin* const CPL_RESTRICT pSrcData,
                                      int nSrcPixelStride,
                                      Tout* const CPL_RESTRICT pDstData,
                                      int nDstPixelStride,
                                      int nWordCount )
{
    std::ptrdiff_t nDstOffset = 0;

    const Tout tOutZero = static_cast<Tout>(0);

    const char* const pSrcDataPtr = reinterpret_cast<const char*>(pSrcData);
    char* const pDstDataPtr = reinterpret_cast<char*>(pDstData);

    for (std::ptrdiff_t n = 0; n < nWordCount; n++)
    {
        const Tin tValue =
            *reinterpret_cast<const Tin*>(pSrcDataPtr + n * nSrcPixelStride);
        Tout* const pPixelOut =
            reinterpret_cast<Tout*>(pDstDataPtr + nDstOffset);
        GDALCopyWord(tValue, *pPixelOut);

        pPixelOut[1] = tOutZero;

        nDstOffset += nDstPixelStride;
    }
}

/************************************************************************/
/*                           GDALCopyWordsFromT()                       */
/************************************************************************/
/**
 * Template driver function. Given the input type T, call the appropriate
 * GDALCopyWordsT function template for the desired output type. You should
 * never call this function directly (call GDALCopyWords instead).
 *
 * @param pSrcData source data buffer
 * @param nSrcPixelStride pixel stride in input buffer, in pixel words
 * @param bInComplex input is complex
 * @param pDstData destination data buffer
 * @param eDstType destination data type
 * @param nDstPixelStride pixel stride in output buffer, in pixel words
 * @param nWordCount number of pixel words to be copied
 */
template <class T>
inline void GDALCopyWordsFromT( const T* const CPL_RESTRICT pSrcData,
                                int nSrcPixelStride, bool bInComplex,
                                void * CPL_RESTRICT pDstData,
                                GDALDataType eDstType, int nDstPixelStride,
                                int nWordCount )
{
    switch (eDstType)
    {
    case GDT_Byte:
        GDALCopyWordsT( pSrcData, nSrcPixelStride,
                        static_cast<unsigned char*>(pDstData), nDstPixelStride,
                        nWordCount );
        break;
    case GDT_UInt16:
        GDALCopyWordsT( pSrcData, nSrcPixelStride,
                        static_cast<unsigned short*>(pDstData), nDstPixelStride,
                        nWordCount );
        break;
    case GDT_Int16:
        GDALCopyWordsT( pSrcData, nSrcPixelStride,
                        static_cast<short*>(pDstData), nDstPixelStride,
                        nWordCount );
        break;
    case GDT_UInt32:
        GDALCopyWordsT( pSrcData, nSrcPixelStride,
                        static_cast<unsigned int*>(pDstData), nDstPixelStride,
                        nWordCount );
        break;
    case GDT_Int32:
        GDALCopyWordsT( pSrcData, nSrcPixelStride,
                        static_cast<int*>(pDstData), nDstPixelStride,
                        nWordCount );
        break;
    case GDT_Float32:
        GDALCopyWordsT( pSrcData, nSrcPixelStride,
                        static_cast<float*>(pDstData), nDstPixelStride,
                        nWordCount );
        break;
    case GDT_Float64:
        GDALCopyWordsT( pSrcData, nSrcPixelStride,
                        static_cast<double*>(pDstData), nDstPixelStride,
                        nWordCount );
        break;
    case GDT_CInt16:
        if (bInComplex)
        {
            GDALCopyWordsComplexT(
                pSrcData, nSrcPixelStride,
                static_cast<short *>(pDstData), nDstPixelStride,
                nWordCount );
        }
        else // input is not complex, so we need to promote to a complex buffer
        {
            GDALCopyWordsComplexOutT(
                pSrcData, nSrcPixelStride,
                static_cast<short *>(pDstData), nDstPixelStride,
                nWordCount );
        }
        break;
    case GDT_CInt32:
        if (bInComplex)
        {
            GDALCopyWordsComplexT(
                pSrcData, nSrcPixelStride,
                static_cast<int *>(pDstData), nDstPixelStride,
                nWordCount );
        }
        else // input is not complex, so we need to promote to a complex buffer
        {
            GDALCopyWordsComplexOutT(
                pSrcData, nSrcPixelStride,
                static_cast<int *>(pDstData), nDstPixelStride,
                nWordCount );
        }
        break;
    case GDT_CFloat32:
        if (bInComplex)
        {
            GDALCopyWordsComplexT(
                pSrcData, nSrcPixelStride,
                static_cast<float *>(pDstData), nDstPixelStride,
                nWordCount );
        }
        else // input is not complex, so we need to promote to a complex buffer
        {
            GDALCopyWordsComplexOutT(
                pSrcData, nSrcPixelStride,
                static_cast<float *>(pDstData), nDstPixelStride,
                nWordCount );
        }
        break;
    case GDT_CFloat64:
        if (bInComplex)
        {
            GDALCopyWordsComplexT(
                pSrcData, nSrcPixelStride,
                static_cast<double *>(pDstData), nDstPixelStride,
                nWordCount );
        }
        else // input is not complex, so we need to promote to a complex buffer
        {
            GDALCopyWordsComplexOutT(
                pSrcData, nSrcPixelStride,
                static_cast<double *>(pDstData), nDstPixelStride,
                nWordCount );
        }
        break;
    case GDT_Unknown:
    default:
        CPLAssert(false);
    }
}

} // end anonymous namespace

/************************************************************************/
/*                          GDALReplicateWord()                         */
/************************************************************************/

template <class T>
inline void GDALReplicateWordT( void * pDstData,
                                int nDstPixelStride,
                                unsigned int nWordCount )
{
    const T valSet = *static_cast<const T*>(pDstData);
    if( nDstPixelStride == static_cast<int>(sizeof(T)) )
    {
        T* pDstPtr = static_cast<T*>(pDstData) + 1;
        while( nWordCount >= 4 )
        {
            nWordCount -= 4;
            pDstPtr[0] = valSet;
            pDstPtr[1] = valSet;
            pDstPtr[2] = valSet;
            pDstPtr[3] = valSet;
            pDstPtr += 4;
        }
        while( nWordCount > 0 )
        {
            --nWordCount;
            *pDstPtr = valSet;
            pDstPtr++;
        }
    }
    else
    {
        GByte *pabyDstPtr = static_cast<GByte *>(pDstData) + nDstPixelStride;
        while( nWordCount > 0 )
        {
            --nWordCount;
            *reinterpret_cast<T*>(pabyDstPtr) = valSet;
            pabyDstPtr += nDstPixelStride;
        }
    }
}

static
void GDALReplicateWord( const void * CPL_RESTRICT pSrcData,
                        GDALDataType eSrcType,
                        void * CPL_RESTRICT pDstData,
                        GDALDataType eDstType,
                        int nDstPixelStride,
                        unsigned int nWordCount)
{
/* ----------------------------------------------------------------------- */
/* Special case when the source data is always the same value              */
/* (for VRTSourcedRasterBand::IRasterIO and VRTDerivedRasterBand::IRasterIO*/
/*  for example)                                                           */
/* ----------------------------------------------------------------------- */
    // Let the general translation case do the necessary conversions
    // on the first destination element.
    GDALCopyWords(pSrcData, eSrcType, 0,
                  pDstData, eDstType, 0,
                  1 );

    // Now copy the first element to the nWordCount - 1 following destination
    // elements.
    nWordCount--;
    GByte *pabyDstWord = reinterpret_cast<GByte *>(pDstData) + nDstPixelStride;

    switch (eDstType)
    {
        case GDT_Byte:
        {
            if (nDstPixelStride == 1)
            {
                if (nWordCount > 0)
                    memset(pabyDstWord, *reinterpret_cast<const GByte*>(pDstData), nWordCount);
            }
            else
            {
                GByte valSet = *reinterpret_cast<const GByte*>(pDstData);
                while( nWordCount > 0 )
                {
                    --nWordCount;
                    *pabyDstWord = valSet;
                    pabyDstWord += nDstPixelStride;
                }
            }
            break;
        }

#define CASE_DUPLICATE_SIMPLE(enum_type, c_type) \
        case enum_type:\
        { \
            GDALReplicateWordT<c_type>(pDstData, nDstPixelStride, nWordCount); \
            break; \
        }

        CASE_DUPLICATE_SIMPLE(GDT_UInt16, GUInt16)
        CASE_DUPLICATE_SIMPLE(GDT_Int16,  GInt16)
        CASE_DUPLICATE_SIMPLE(GDT_UInt32, GUInt32)
        CASE_DUPLICATE_SIMPLE(GDT_Int32,  GInt32)
        CASE_DUPLICATE_SIMPLE(GDT_Float32, float)
        CASE_DUPLICATE_SIMPLE(GDT_Float64, double)

#define CASE_DUPLICATE_COMPLEX(enum_type, c_type) \
        case enum_type:\
        { \
            c_type valSet1 = reinterpret_cast<const c_type*>(pDstData)[0]; \
            c_type valSet2 = reinterpret_cast<const c_type*>(pDstData)[1]; \
            while( nWordCount > 0 ) \
            { \
                --nWordCount; \
                reinterpret_cast<c_type*>(pabyDstWord)[0] = valSet1; \
                reinterpret_cast<c_type*>(pabyDstWord)[1] = valSet2; \
                pabyDstWord += nDstPixelStride; \
            } \
            break; \
        }

        CASE_DUPLICATE_COMPLEX(GDT_CInt16, GInt16)
        CASE_DUPLICATE_COMPLEX(GDT_CInt32, GInt32)
        CASE_DUPLICATE_COMPLEX(GDT_CFloat32, float)
        CASE_DUPLICATE_COMPLEX(GDT_CFloat64, double)

        default:
            CPLAssert( false );
    }
}

/************************************************************************/
/*                        GDALUnrolledCopy()                            */
/************************************************************************/

template<class T, int srcStride, int dstStride>
static inline void GDALUnrolledCopyGeneric( T* CPL_RESTRICT pDest,
                                     const T* CPL_RESTRICT pSrc,
                                     int nIters )
{
    if (nIters >= 16)
    {
        for ( int i = nIters / 16; i != 0; i -- )
        {
            pDest[0*dstStride] = pSrc[0*srcStride];
            pDest[1*dstStride] = pSrc[1*srcStride];
            pDest[2*dstStride] = pSrc[2*srcStride];
            pDest[3*dstStride] = pSrc[3*srcStride];
            pDest[4*dstStride] = pSrc[4*srcStride];
            pDest[5*dstStride] = pSrc[5*srcStride];
            pDest[6*dstStride] = pSrc[6*srcStride];
            pDest[7*dstStride] = pSrc[7*srcStride];
            pDest[8*dstStride] = pSrc[8*srcStride];
            pDest[9*dstStride] = pSrc[9*srcStride];
            pDest[10*dstStride] = pSrc[10*srcStride];
            pDest[11*dstStride] = pSrc[11*srcStride];
            pDest[12*dstStride] = pSrc[12*srcStride];
            pDest[13*dstStride] = pSrc[13*srcStride];
            pDest[14*dstStride] = pSrc[14*srcStride];
            pDest[15*dstStride] = pSrc[15*srcStride];
            pDest += 16*dstStride;
            pSrc += 16*srcStride;
        }
        nIters = nIters % 16;
    }
    for( int i = 0; i < nIters; i++ )
    {
        pDest[i*dstStride] = *pSrc;
        pSrc += srcStride;
    }
}

template<class T, int srcStride, int dstStride>
static inline void GDALUnrolledCopy( T* CPL_RESTRICT pDest,
                                     const T* CPL_RESTRICT pSrc,
                                     int nIters )
{
    GDALUnrolledCopyGeneric<T,srcStride,dstStride>(pDest, pSrc, nIters);
}

#if (defined(__x86_64) || defined(_M_X64)) &&  !(defined(__GNUC__) && __GNUC__ < 4)

#ifdef HAVE_SSSE3_AT_COMPILE_TIME

void GDALUnrolledCopy_GByte_2_1_SSSE3( GByte* CPL_RESTRICT pDest,
                                             const GByte* CPL_RESTRICT pSrc,
                                             int nIters );

void GDALUnrolledCopy_GByte_3_1_SSSE3( GByte* CPL_RESTRICT pDest,
                                             const GByte* CPL_RESTRICT pSrc,
                                             int nIters );

void GDALUnrolledCopy_GByte_4_1_SSSE3( GByte* CPL_RESTRICT pDest,
                                             const GByte* CPL_RESTRICT pSrc,
                                             int nIters );
#endif


template<> void GDALUnrolledCopy<GByte,2,1>( GByte* CPL_RESTRICT pDest,
                                             const GByte* CPL_RESTRICT pSrc,
                                             int nIters )
{
    int i = 0;
    if( nIters > 16 )
    {
#ifdef HAVE_SSSE3_AT_COMPILE_TIME
        if( CPLHaveRuntimeSSSE3() )
        {
            GDALUnrolledCopy_GByte_2_1_SSSE3(pDest, pSrc, nIters);
            return;
        }
#endif

        const __m128i xmm_zero = _mm_setzero_si128();
        const __m128i xmm_mask = _mm_set1_epi16(0xff);
        // If we were sure that there would always be 1 trailing byte, we could
        // check against nIters - 15
        for ( ; i < nIters - 16; i += 16 )
        {
            __m128i xmm0 = _mm_loadu_si128( reinterpret_cast<__m128i const*>(pSrc + 0) );
            __m128i xmm1 = _mm_loadu_si128( reinterpret_cast<__m128i const*>(pSrc + 16) );
            // Set higher 8bit of each int16 packed word to 0
            xmm0 = _mm_and_si128(xmm0, xmm_mask);
            xmm1 = _mm_and_si128(xmm1, xmm_mask);
            // Pack int16 to uint8
            xmm0 = _mm_packus_epi16(xmm0, xmm_zero);
            xmm1 = _mm_packus_epi16(xmm1, xmm_zero);

            // Move 64 lower bits of xmm1 to 64 upper bits of xmm0
            xmm1 = _mm_slli_si128(xmm1, 8);
            xmm0 = _mm_or_si128(xmm0, xmm1);

            // Store result
            _mm_storeu_si128( reinterpret_cast<__m128i*>(pDest + i), xmm0);

            pSrc += 2 * 16;
        }
    }
    for( ; i < nIters; i++ )
    {
        pDest[i] = *pSrc;
        pSrc += 2;
    }
}


#ifdef HAVE_SSSE3_AT_COMPILE_TIME

template<> void GDALUnrolledCopy<GByte,3,1>( GByte* CPL_RESTRICT pDest,
                                             const GByte* CPL_RESTRICT pSrc,
                                             int nIters )
{
    if( nIters > 16 && CPLHaveRuntimeSSSE3() )
    {
        GDALUnrolledCopy_GByte_3_1_SSSE3(pDest, pSrc, nIters);
    }
    else
    {
        GDALUnrolledCopyGeneric<GByte,3,1>(pDest, pSrc, nIters);
    }
}

#endif

template<> void GDALUnrolledCopy<GByte,4,1>( GByte* CPL_RESTRICT pDest,
                                             const GByte* CPL_RESTRICT pSrc,
                                             int nIters )
{
    int i = 0;
    if( nIters > 16 )
    {
#ifdef HAVE_SSSE3_AT_COMPILE_TIME
        if( CPLHaveRuntimeSSSE3() )
        {
            GDALUnrolledCopy_GByte_4_1_SSSE3(pDest, pSrc, nIters);
            return;
        }
#endif

        const __m128i xmm_mask = _mm_set1_epi32(0xff);
        // If we were sure that there would always be 3 trailing bytes, we could
        // check against nIters - 15
        for ( ; i < nIters - 16; i += 16 )
        {
            __m128i xmm0 = _mm_loadu_si128( reinterpret_cast<__m128i const*> (pSrc + 0) );
            __m128i xmm1 = _mm_loadu_si128( reinterpret_cast<__m128i const*> (pSrc + 16) );
            __m128i xmm2 = _mm_loadu_si128( reinterpret_cast<__m128i const*> (pSrc + 32) );
            __m128i xmm3 = _mm_loadu_si128( reinterpret_cast<__m128i const*> (pSrc + 48) );
            // Set higher 24bit of each int32 packed word to 0
            xmm0 = _mm_and_si128(xmm0, xmm_mask);
            xmm1 = _mm_and_si128(xmm1, xmm_mask);
            xmm2 = _mm_and_si128(xmm2, xmm_mask);
            xmm3 = _mm_and_si128(xmm3, xmm_mask);
            // Pack int32 to int16
            xmm0 = _mm_packs_epi32(xmm0, xmm0);
            xmm1 = _mm_packs_epi32(xmm1, xmm1);
            xmm2 = _mm_packs_epi32(xmm2, xmm2);
            xmm3 = _mm_packs_epi32(xmm3, xmm3);
            // Pack int16 to uint8
            xmm0 = _mm_packus_epi16(xmm0, xmm0);
            xmm1 = _mm_packus_epi16(xmm1, xmm1);
            xmm2 = _mm_packus_epi16(xmm2, xmm2);
            xmm3 = _mm_packus_epi16(xmm3, xmm3);

            // Store lower 32 bit word
            GDALCopyXMMToInt32(xmm0, pDest + i + 0);
            GDALCopyXMMToInt32(xmm1, pDest + i + 4);
            GDALCopyXMMToInt32(xmm2, pDest + i + 8);
            GDALCopyXMMToInt32(xmm3, pDest + i + 12);

            pSrc += 4 * 16;
        }
    }
    for( ; i < nIters; i++ )
    {
        pDest[i] = *pSrc;
        pSrc += 4;
    }
}
#endif // defined(__x86_64) || defined(_M_X64)

/************************************************************************/
/*                         GDALFastCopy()                               */
/************************************************************************/

template<class T>
static inline void GDALFastCopy( T* CPL_RESTRICT pDest,
                                 int nDestStride,
                                 const T* CPL_RESTRICT pSrc,
                                 int nSrcStride,
                                 int nIters )
{
    if( nIters == 1 )
    {
        *pDest = *pSrc;
    }
    else if( nDestStride == static_cast<int>(sizeof(T)) )
    {
        if( nSrcStride == static_cast<int>(sizeof(T)) )
        {
            memcpy(pDest, pSrc, nIters * sizeof(T));
        }
        else if( nSrcStride == 2 * static_cast<int>(sizeof(T)) )
        {
            GDALUnrolledCopy<T, 2,1>(pDest, pSrc, nIters);
        }
        else if( nSrcStride == 3 * static_cast<int>(sizeof(T)) )
        {
            GDALUnrolledCopy<T, 3,1>(pDest, pSrc, nIters);
        }
        else if( nSrcStride == 4 * static_cast<int>(sizeof(T)) )
        {
            GDALUnrolledCopy<T, 4,1>(pDest, pSrc, nIters);
        }
        else
        {
            while( nIters-- > 0 )
            {
                *pDest = *pSrc;
                pSrc += nSrcStride / static_cast<int>(sizeof(T));
                pDest ++;
            }
        }
    }
    else if( nSrcStride == static_cast<int>(sizeof(T))  )
    {
        if( nDestStride == 2 * static_cast<int>(sizeof(T)) )
        {
            GDALUnrolledCopy<T, 1,2>(pDest, pSrc, nIters);
        }
        else if( nDestStride == 3 * static_cast<int>(sizeof(T)) )
        {
            GDALUnrolledCopy<T, 1,3>(pDest, pSrc, nIters);
        }
        else if( nDestStride == 4 * static_cast<int>(sizeof(T))  )
        {
            GDALUnrolledCopy<T, 1,4>(pDest, pSrc, nIters);
        }
        else
        {
            while( nIters-- > 0 )
            {
                *pDest = *pSrc;
                pSrc ++;
                pDest += nDestStride / static_cast<int>(sizeof(T));
            }
        }
    }
    else
    {
        while( nIters-- > 0 )
        {
            *pDest = *pSrc;
            pSrc += nSrcStride / static_cast<int>(sizeof(T));
            pDest += nDestStride / static_cast<int>(sizeof(T));
        }
    }
}

/************************************************************************/
/*                           GDALCopyWords()                            */
/************************************************************************/

/**
 * Copy pixel words from buffer to buffer.
 *
 * This function is used to copy pixel word values from one memory buffer
 * to another, with support for conversion between data types, and differing
 * step factors.  The data type conversion is done using the normal GDAL
 * rules.  Values assigned to a lower range integer type are clipped.  For
 * instance assigning GDT_Int16 values to a GDT_Byte buffer will cause values
 * less the 0 to be set to 0, and values larger than 255 to be set to 255.
 * Assignment from floating point to integer uses default C type casting
 * semantics.   Assignment from non-complex to complex will result in the
 * imaginary part being set to zero on output.  Assignment from complex to
 * non-complex will result in the complex portion being lost and the real
 * component being preserved (<i>not magnitude!</i>).
 *
 * No assumptions are made about the source or destination words occurring
 * on word boundaries.  It is assumed that all values are in native machine
 * byte order.
 *
 * @param pSrcData Pointer to source data to be converted.
 * @param eSrcType the source data type (see GDALDataType enum)
 * @param nSrcPixelStride Source pixel stride (i.e. distance between 2 words),
 * in bytes
 * @param pDstData Pointer to buffer where destination data should go
 * @param eDstType the destination data type (see GDALDataType enum)
 * @param nDstPixelStride Destination pixel stride (i.e. distance between 2
 * words), in bytes
 * @param nWordCount number of words to be copied
 *
 * @note
 * When adding a new data type to GDAL, you must do the following to
 * support it properly within the GDALCopyWords function:
 * 1. Add the data type to the switch on eSrcType in GDALCopyWords.
 *    This should invoke the appropriate GDALCopyWordsFromT wrapper.
 * 2. Add the data type to the switch on eDstType in GDALCopyWordsFromT.
 *    This should call the appropriate GDALCopyWordsT template.
 * 3. If appropriate, overload the appropriate CopyWord template in the
 *    above namespace. This will ensure that any conversion issues are
 *    handled (cases like the float -> int32 case, where the min/max)
 *    values are subject to roundoff error.
 */

void CPL_STDCALL
GDALCopyWords( const void * CPL_RESTRICT pSrcData,
               GDALDataType eSrcType,
               int nSrcPixelStride,
               void * CPL_RESTRICT pDstData,
               GDALDataType eDstType,
               int nDstPixelStride,
               int nWordCount )

{
    // On platforms where alignment matters, be careful
    const int nSrcDataTypeSize = GDALGetDataTypeSizeBytes(eSrcType);
#ifdef CPL_CPU_REQUIRES_ALIGNED_ACCESS
    const int nDstDataTypeSize = GDALGetDataTypeSizeBytes(eDstType);
    if( !(eSrcType == eDstType && nSrcPixelStride == nDstPixelStride) &&
        ( (reinterpret_cast<GPtrDiff_t>(pSrcData) % nSrcDataTypeSize) != 0 ||
          (reinterpret_cast<GPtrDiff_t>(pDstData) % nDstDataTypeSize) != 0 ||
          ( nSrcPixelStride % nSrcDataTypeSize) != 0 ||
          ( nDstPixelStride % nDstDataTypeSize) != 0 ) )
    {
        if( eSrcType == eDstType )
        {
            for( int i = 0; i < nWordCount; i++ )
            {
                memcpy( static_cast<GByte*>(pDstData) + nDstPixelStride * i,
                        static_cast<const GByte*>(pSrcData) + nSrcPixelStride * i,
                        nDstDataTypeSize );
            }
        }
        else
        {
#define ALIGN_PTR(ptr, align) ((ptr) + ((align) - (reinterpret_cast<size_t>(ptr) % (align))) % (align))
            // The largest we need is for CFloat64 (16 bytes), so 32 bytes to
            // be sure to get correctly aligned pointer.
            GByte abySrcBuffer[32];
            GByte abyDstBuffer[32];
            GByte* pabySrcBuffer = ALIGN_PTR(abySrcBuffer, nSrcDataTypeSize);
            GByte* pabyDstBuffer = ALIGN_PTR(abyDstBuffer, nDstDataTypeSize);
            for( int i = 0; i < nWordCount; i++ )
            {
                memcpy( pabySrcBuffer,
                        static_cast<const GByte*>(pSrcData) + nSrcPixelStride * i,
                        nSrcDataTypeSize );
                GDALCopyWords( pabySrcBuffer,
                               eSrcType,
                               0,
                               pabyDstBuffer,
                               eDstType,
                               0,
                               1 );
                memcpy( static_cast<GByte*>(pDstData) + nDstPixelStride * i,
                        pabyDstBuffer,
                        nDstDataTypeSize );
            }
        }
        return;
    }
#endif

    // Deal with the case where we're replicating a single word into the
    // provided buffer
    if (nSrcPixelStride == 0 && nWordCount > 1)
    {
        GDALReplicateWord( pSrcData, eSrcType, pDstData,
                           eDstType, nDstPixelStride, nWordCount );
        return;
    }

    if (eSrcType == eDstType)
    {
        if( eSrcType == GDT_Byte )
        {
            GDALFastCopy(
                static_cast<GByte*>(pDstData), nDstPixelStride,
                static_cast<const GByte*>(pSrcData), nSrcPixelStride, nWordCount );
            return;
        }

        if( nSrcDataTypeSize == 2 && (nSrcPixelStride%2) == 0 &&
            (nDstPixelStride%2) == 0 )
        {
            GDALFastCopy(
                static_cast<short*>(pDstData), nDstPixelStride,
                static_cast<const short*>(pSrcData), nSrcPixelStride, nWordCount );
            return;
        }

        if( nWordCount == 1 )
        {
            if( nSrcDataTypeSize == 2 )
                memcpy(pDstData, pSrcData, 2);
            else if( nSrcDataTypeSize == 4 )
                memcpy(pDstData, pSrcData, 4);
            else if( nSrcDataTypeSize == 8 )
                memcpy(pDstData, pSrcData, 8 );
            else /* if( eSrcType == GDT_CFloat64 ) */
                memcpy(pDstData, pSrcData, 16);
            return;
        }

        // Let memcpy() handle the case where we're copying a packed buffer
        // of pixels.
        if( nSrcPixelStride == nDstPixelStride )
        {
            if( nSrcPixelStride == nSrcDataTypeSize)
            {
                memcpy(pDstData, pSrcData, nWordCount * nSrcDataTypeSize);
                return;
            }
        }
    }

    // Handle the more general case -- deals with conversion of data types
    // directly.
    switch (eSrcType)
    {
    case GDT_Byte:
        GDALCopyWordsFromT<unsigned char>(
            static_cast<const unsigned char *>(pSrcData),
            nSrcPixelStride, false,
            pDstData, eDstType, nDstPixelStride,
            nWordCount );
        break;
    case GDT_UInt16:
        GDALCopyWordsFromT<unsigned short>(
            static_cast<const unsigned short *>(pSrcData),
            nSrcPixelStride, false,
            pDstData, eDstType, nDstPixelStride,
            nWordCount );
        break;
    case GDT_Int16:
        GDALCopyWordsFromT<short>(
            static_cast<const short *>(pSrcData), nSrcPixelStride, false,
            pDstData, eDstType, nDstPixelStride,
            nWordCount );
        break;
    case GDT_UInt32:
        GDALCopyWordsFromT<unsigned int>(
            static_cast<const unsigned int *>(pSrcData), nSrcPixelStride, false,
            pDstData, eDstType, nDstPixelStride,
            nWordCount );
        break;
    case GDT_Int32:
        GDALCopyWordsFromT<int>(
            static_cast<const int *>(pSrcData), nSrcPixelStride, false,
            pDstData, eDstType, nDstPixelStride,
            nWordCount );
        break;
    case GDT_Float32:
        GDALCopyWordsFromT<float>(
            static_cast<const float *>(pSrcData), nSrcPixelStride, false,
            pDstData, eDstType, nDstPixelStride,
            nWordCount );
        break;
    case GDT_Float64:
        GDALCopyWordsFromT<double>(
            static_cast<const double *>(pSrcData), nSrcPixelStride, false,
            pDstData, eDstType, nDstPixelStride,
            nWordCount );
        break;
    case GDT_CInt16:
        GDALCopyWordsFromT<short>(
            static_cast<const short *>(pSrcData), nSrcPixelStride, true,
            pDstData, eDstType, nDstPixelStride,
            nWordCount );
        break;
    case GDT_CInt32:
        GDALCopyWordsFromT<int>(
            static_cast<const int *>(pSrcData), nSrcPixelStride, true,
            pDstData, eDstType, nDstPixelStride,
            nWordCount );
        break;
    case GDT_CFloat32:
        GDALCopyWordsFromT<float>(
            static_cast<const float *>(pSrcData), nSrcPixelStride, true,
            pDstData, eDstType, nDstPixelStride,
            nWordCount );
        break;
    case GDT_CFloat64:
        GDALCopyWordsFromT<double>(
            static_cast<const double *>(pSrcData), nSrcPixelStride, true,
            pDstData, eDstType, nDstPixelStride,
            nWordCount );
        break;
    case GDT_Unknown:
    default:
        CPLAssert(false);
    }
}

/************************************************************************/
/*                            GDALCopyBits()                            */
/************************************************************************/

/**
 * Bitwise word copying.
 *
 * A function for moving sets of partial bytes around.  Loosely
 * speaking this is a bitwise analog to GDALCopyWords().
 *
 * It copies nStepCount "words" where each word is nBitCount bits long.
 * The nSrcStep and nDstStep are the number of bits from the start of one
 * word to the next (same as nBitCount if they are packed).  The nSrcOffset
 * and nDstOffset are the offset into the source and destination buffers
 * to start at, also measured in bits.
 *
 * All bit offsets are assumed to start from the high order bit in a byte
 * (i.e. most significant bit first).  Currently this function is not very
 * optimized, but it may be improved for some common cases in the future
 * as needed.
 *
 * @param pabySrcData the source data buffer.
 * @param nSrcOffset the offset (in bits) in pabySrcData to the start of the
 * first word to copy.
 * @param nSrcStep the offset in bits from the start one source word to the
 * start of the next.
 * @param pabyDstData the destination data buffer.
 * @param nDstOffset the offset (in bits) in pabyDstData to the start of the
 * first word to copy over.
 * @param nDstStep the offset in bits from the start one word to the
 * start of the next.
 * @param nBitCount the number of bits in a word to be copied.
 * @param nStepCount the number of words to copy.
 */

void GDALCopyBits( const GByte *pabySrcData, int nSrcOffset, int nSrcStep,
                   GByte *pabyDstData, int nDstOffset, int nDstStep,
                   int nBitCount, int nStepCount )

{
    VALIDATE_POINTER0( pabySrcData, "GDALCopyBits" );

    for( int iStep = 0; iStep < nStepCount; iStep++ )
    {
        for( int iBit = 0; iBit < nBitCount; iBit++ )
        {
            if( pabySrcData[nSrcOffset>>3]
                & (0x80 >>(nSrcOffset & 7)) )
                pabyDstData[nDstOffset>>3] |= (0x80 >> (nDstOffset & 7));
            else
                pabyDstData[nDstOffset>>3] &= ~(0x80 >> (nDstOffset & 7));

            nSrcOffset++;
            nDstOffset++;
        }

        nSrcOffset += (nSrcStep - nBitCount);
        nDstOffset += (nDstStep - nBitCount);
    }
}

/************************************************************************/
/*                    GDALGetBestOverviewLevel()                        */
/*                                                                      */
/* Returns the best overview level to satisfy the query or -1 if none   */
/* Also updates nXOff, nYOff, nXSize, nYSize and psExtraArg when        */
/* returning a valid overview level                                     */
/************************************************************************/

int GDALBandGetBestOverviewLevel( GDALRasterBand* poBand,
                                  int &nXOff, int &nYOff,
                                  int &nXSize, int &nYSize,
                                  int nBufXSize, int nBufYSize )
{
    return GDALBandGetBestOverviewLevel2( poBand, nXOff, nYOff, nXSize, nYSize,
                                          nBufXSize, nBufYSize, nullptr );
}

int GDALBandGetBestOverviewLevel2( GDALRasterBand* poBand,
                                   int &nXOff, int &nYOff,
                                   int &nXSize, int &nYSize,
                                   int nBufXSize, int nBufYSize,
                                   GDALRasterIOExtraArg* psExtraArg )
{
    double dfDesiredResolution = 0.0;
/* -------------------------------------------------------------------- */
/*      Compute the desired resolution.  The resolution is              */
/*      based on the least reduced axis, and represents the number      */
/*      of source pixels to one destination pixel.                      */
/* -------------------------------------------------------------------- */
    if( (nXSize / static_cast<double>(nBufXSize)) <
        (nYSize / static_cast<double>(nBufYSize))
        || nBufYSize == 1 )
        dfDesiredResolution = nXSize / static_cast<double>( nBufXSize );
    else
        dfDesiredResolution = nYSize / static_cast<double>( nBufYSize );

/* -------------------------------------------------------------------- */
/*      Find the overview level that largest resolution value (most     */
/*      downsampled) that is still less than (or only a little more)    */
/*      downsampled than the request.                                   */
/* -------------------------------------------------------------------- */
    int nOverviewCount = poBand->GetOverviewCount();
    GDALRasterBand* poBestOverview = nullptr;
    double dfBestResolution = 0;
    int nBestOverviewLevel = -1;

    for( int iOverview = 0; iOverview < nOverviewCount; iOverview++ )
    {
        GDALRasterBand *poOverview = poBand->GetOverview( iOverview );
        if (poOverview == nullptr)
            continue;

        double dfResolution = 0.0;

        // What resolution is this?
        if( (poBand->GetXSize() / static_cast<double>(poOverview->GetXSize()))
            < (poBand->GetYSize() /
               static_cast<double>(poOverview->GetYSize())) )
            dfResolution =
                poBand->GetXSize() /
                static_cast<double>( poOverview->GetXSize() );
        else
            dfResolution =
                poBand->GetYSize() /
                static_cast<double>( poOverview->GetYSize() );

        // Is it nearly the requested resolution and better (lower) than
        // the current best resolution?
        if( dfResolution >= dfDesiredResolution * 1.2
            || dfResolution <= dfBestResolution )
            continue;

        // Ignore AVERAGE_BIT2GRAYSCALE overviews for RasterIO purposes.
        const char *pszResampling =
            poOverview->GetMetadataItem( "RESAMPLING" );

        if( pszResampling != nullptr &&
            STARTS_WITH_CI(pszResampling, "AVERAGE_BIT2") )
            continue;

        // OK, this is our new best overview.
        poBestOverview = poOverview;
        nBestOverviewLevel = iOverview;
        dfBestResolution = dfResolution;
    }

/* -------------------------------------------------------------------- */
/*      If we didn't find an overview that helps us, just return        */
/*      indicating failure and the full resolution image will be used.  */
/* -------------------------------------------------------------------- */
    if( nBestOverviewLevel < 0 )
        return -1;

/* -------------------------------------------------------------------- */
/*      Recompute the source window in terms of the selected            */
/*      overview.                                                       */
/* -------------------------------------------------------------------- */
    const double dfXRes =
        poBand->GetXSize() / static_cast<double>( poBestOverview->GetXSize() );
    const double dfYRes =
        poBand->GetYSize() / static_cast<double>( poBestOverview->GetYSize() );

    const int nOXOff = std::min( poBestOverview->GetXSize()-1,
                                 static_cast<int>(nXOff / dfXRes + 0.5));
    const int nOYOff = std::min( poBestOverview->GetYSize()-1,
                                 static_cast<int>(nYOff / dfYRes + 0.5));
    int nOXSize = std::max(1, static_cast<int>(nXSize / dfXRes + 0.5));
    int nOYSize = std::max(1, static_cast<int>(nYSize / dfYRes + 0.5));
    if( nOXOff + nOXSize > poBestOverview->GetXSize() )
        nOXSize = poBestOverview->GetXSize() - nOXOff;
    if( nOYOff + nOYSize > poBestOverview->GetYSize() )
        nOYSize = poBestOverview->GetYSize() - nOYOff;

    nXOff = nOXOff;
    nYOff = nOYOff;
    nXSize = nOXSize;
    nYSize = nOYSize;

    if( psExtraArg && psExtraArg->bFloatingPointWindowValidity )
    {
        psExtraArg->dfXOff /= dfXRes;
        psExtraArg->dfXSize /= dfXRes;
        psExtraArg->dfYOff /= dfYRes;
        psExtraArg->dfYSize /= dfYRes;
    }

    return nBestOverviewLevel;
}

/************************************************************************/
/*                          OverviewRasterIO()                          */
/*                                                                      */
/*      Special work function to utilize available overviews to         */
/*      more efficiently satisfy downsampled requests.  It will         */
/*      return CE_Failure if there are no appropriate overviews         */
/*      available but it doesn't emit any error messages.               */
/************************************************************************/

//! @cond Doxygen_Suppress
CPLErr GDALRasterBand::OverviewRasterIO( GDALRWFlag eRWFlag,
                                int nXOff, int nYOff, int nXSize, int nYSize,
                                void * pData, int nBufXSize, int nBufYSize,
                                GDALDataType eBufType,
                                GSpacing nPixelSpace, GSpacing nLineSpace,
                                GDALRasterIOExtraArg* psExtraArg )

{
    GDALRasterIOExtraArg sExtraArg;
    GDALCopyRasterIOExtraArg(&sExtraArg, psExtraArg);

    const int nOverview =
        GDALBandGetBestOverviewLevel2( this, nXOff, nYOff, nXSize, nYSize,
                                       nBufXSize, nBufYSize, &sExtraArg );
    if (nOverview < 0)
        return CE_Failure;

/* -------------------------------------------------------------------- */
/*      Recast the call in terms of the new raster layer.               */
/* -------------------------------------------------------------------- */
    GDALRasterBand* poOverviewBand = GetOverview(nOverview);
    if (poOverviewBand == nullptr)
        return CE_Failure;

    return poOverviewBand->RasterIO( eRWFlag, nXOff, nYOff, nXSize, nYSize,
                                     pData, nBufXSize, nBufYSize, eBufType,
                                     nPixelSpace, nLineSpace, &sExtraArg );
}

/************************************************************************/
/*                      TryOverviewRasterIO()                           */
/************************************************************************/

CPLErr GDALRasterBand::TryOverviewRasterIO( GDALRWFlag eRWFlag,
                                            int nXOff, int nYOff, int nXSize, int nYSize,
                                            void * pData, int nBufXSize, int nBufYSize,
                                            GDALDataType eBufType,
                                            GSpacing nPixelSpace, GSpacing nLineSpace,
                                            GDALRasterIOExtraArg* psExtraArg,
                                            int* pbTried )
{
    int nXOffMod = nXOff;
    int nYOffMod = nYOff;
    int nXSizeMod = nXSize;
    int nYSizeMod = nYSize;
    GDALRasterIOExtraArg sExtraArg;

    GDALCopyRasterIOExtraArg(&sExtraArg, psExtraArg);

    int iOvrLevel = GDALBandGetBestOverviewLevel2( this,
                                                   nXOffMod, nYOffMod,
                                                   nXSizeMod, nYSizeMod,
                                                   nBufXSize, nBufYSize,
                                                   &sExtraArg );

    if( iOvrLevel >= 0 )
    {
        GDALRasterBand* poOverviewBand = GetOverview(iOvrLevel);
        if( poOverviewBand  )
        {
            *pbTried = TRUE;
            return poOverviewBand->RasterIO(
                eRWFlag, nXOffMod, nYOffMod, nXSizeMod, nYSizeMod,
                pData, nBufXSize, nBufYSize, eBufType,
                nPixelSpace, nLineSpace, &sExtraArg);
        }
    }

    *pbTried = FALSE;
    return CE_None;
}

/************************************************************************/
/*                      TryOverviewRasterIO()                           */
/************************************************************************/

CPLErr GDALDataset::TryOverviewRasterIO( GDALRWFlag eRWFlag,
                                         int nXOff, int nYOff, int nXSize, int nYSize,
                                         void * pData, int nBufXSize, int nBufYSize,
                                         GDALDataType eBufType,
                                         int nBandCount, int *panBandMap,
                                         GSpacing nPixelSpace, GSpacing nLineSpace,
                                         GSpacing nBandSpace,
                                         GDALRasterIOExtraArg* psExtraArg,
                                         int* pbTried )
{
    int nXOffMod = nXOff;
    int nYOffMod = nYOff;
    int nXSizeMod = nXSize;
    int nYSizeMod = nYSize;
    GDALRasterIOExtraArg sExtraArg;
    GDALCopyRasterIOExtraArg(&sExtraArg, psExtraArg);

    int iOvrLevel = GDALBandGetBestOverviewLevel2( papoBands[0],
                                                   nXOffMod, nYOffMod,
                                                   nXSizeMod, nYSizeMod,
                                                   nBufXSize, nBufYSize,
                                                   &sExtraArg );

    if( iOvrLevel >= 0 && papoBands[0]->GetOverview(iOvrLevel) != nullptr &&
        papoBands[0]->GetOverview(iOvrLevel)->GetDataset() != nullptr )
    {
        *pbTried = TRUE;
        return papoBands[0]->GetOverview(iOvrLevel)->GetDataset()->RasterIO(
            eRWFlag, nXOffMod, nYOffMod, nXSizeMod, nYSizeMod,
            pData, nBufXSize, nBufYSize, eBufType,
            nBandCount, panBandMap, nPixelSpace, nLineSpace, nBandSpace,
            &sExtraArg );
    }
    else
    {
        *pbTried = FALSE;
        return CE_None;
    }
}

/************************************************************************/
/*                        GetBestOverviewLevel()                        */
/*                                                                      */
/* Returns the best overview level to satisfy the query or -1 if none   */
/* Also updates nXOff, nYOff, nXSize, nYSize when returning a valid     */
/* overview level                                                       */
/************************************************************************/

static
int GDALDatasetGetBestOverviewLevel( GDALDataset* poDS,
                                     int &nXOff, int &nYOff,
                                     int &nXSize, int &nYSize,
                                     int nBufXSize, int nBufYSize,
                                     int nBandCount, int *panBandMap )
{
    int nOverviewCount = 0;
    GDALRasterBand *poFirstBand = nullptr;

/* -------------------------------------------------------------------- */
/* Check that all bands have the same number of overviews and           */
/* that they have all the same size and block dimensions                */
/* -------------------------------------------------------------------- */
    for( int iBand = 0; iBand < nBandCount; iBand++ )
    {
        GDALRasterBand *poBand = poDS->GetRasterBand( panBandMap[iBand] );
        if ( poBand == nullptr )
            return -1;
        if (iBand == 0)
        {
            poFirstBand = poBand;
            nOverviewCount = poBand->GetOverviewCount();
        }
        else if (nOverviewCount != poBand->GetOverviewCount())
        {
            CPLDebug( "GDAL",
                      "GDALDataset::GetBestOverviewLevel() ... "
                      "mismatched overview count, use std method." );
            return -1;
        }
        else
        {
            for( int iOverview = 0; iOverview < nOverviewCount; iOverview++ )
            {
                GDALRasterBand* poOvrBand =
                    poBand->GetOverview(iOverview);
                GDALRasterBand* poOvrFirstBand =
                    poFirstBand->GetOverview(iOverview);
                if ( poOvrBand == nullptr || poOvrFirstBand == nullptr)
                    continue;

                if ( poOvrFirstBand->GetXSize() != poOvrBand->GetXSize() ||
                     poOvrFirstBand->GetYSize() != poOvrBand->GetYSize() )
                {
                    CPLDebug( "GDAL",
                              "GDALDataset::GetBestOverviewLevel() ... "
                              "mismatched overview sizes, use std method." );
                    return -1;
                }
                int nBlockXSizeFirst = 0;
                int nBlockYSizeFirst = 0;
                poOvrFirstBand->GetBlockSize( &nBlockXSizeFirst,
                                              &nBlockYSizeFirst );

                int nBlockXSizeCurrent = 0;
                int nBlockYSizeCurrent = 0;
                poOvrBand->GetBlockSize( &nBlockXSizeCurrent,
                                         &nBlockYSizeCurrent );

                if (nBlockXSizeFirst != nBlockXSizeCurrent ||
                    nBlockYSizeFirst != nBlockYSizeCurrent)
                {
                    CPLDebug(
                        "GDAL",
                        "GDALDataset::GetBestOverviewLevel() ... "
                        "mismatched block sizes, use std method." );
                    return -1;
                }
            }
        }
    }
    if( poFirstBand == nullptr )
        return -1;

    return GDALBandGetBestOverviewLevel2( poFirstBand,
                                          nXOff, nYOff, nXSize, nYSize,
                                          nBufXSize, nBufYSize, nullptr );
}

/************************************************************************/
/*                         BlockBasedRasterIO()                         */
/*                                                                      */
/*      This convenience function implements a dataset level            */
/*      RasterIO() interface based on calling down to fetch blocks,     */
/*      much like the GDALRasterBand::IRasterIO(), but it handles       */
/*      all bands at once, so that a format driver that handles a       */
/*      request for different bands of the same block efficiently       */
/*      (i.e. without re-reading interleaved data) will efficiently.    */
/*                                                                      */
/*      This method is intended to be called by an overridden           */
/*      IRasterIO() method in the driver specific GDALDataset           */
/*      derived class.                                                  */
/*                                                                      */
/*      Default internal implementation of RasterIO() ... utilizes      */
/*      the Block access methods to satisfy the request.  This would    */
/*      normally only be overridden by formats with overviews.          */
/*                                                                      */
/*      To keep things relatively simple, this method does not          */
/*      currently take advantage of some special cases addressed in     */
/*      GDALRasterBand::IRasterIO(), so it is likely best to only       */
/*      call it when you know it will help.  That is in cases where     */
/*      data is at 1:1 to the buffer, and you know the driver is        */
/*      implementing interleaved IO efficiently on a block by block     */
/*      basis. Overviews will be used when possible.                    */
/************************************************************************/

CPLErr
GDALDataset::BlockBasedRasterIO( GDALRWFlag eRWFlag,
                                 int nXOff, int nYOff, int nXSize, int nYSize,
                                 void * pData, int nBufXSize, int nBufYSize,
                                 GDALDataType eBufType,
                                 int nBandCount, int *panBandMap,
                                 GSpacing nPixelSpace, GSpacing nLineSpace,
                                 GSpacing nBandSpace,
                                 GDALRasterIOExtraArg* psExtraArg )

{
    CPLAssert( nullptr != pData );

    GByte **papabySrcBlock = nullptr;
    GDALRasterBlock *poBlock = nullptr;
    GDALRasterBlock **papoBlocks = nullptr;
    int nLBlockX = -1;
    int nLBlockY = -1;
    int iBufYOff;
    int iBufXOff;
    int iSrcY;
    int nBlockXSize = 1;
    int nBlockYSize = 1;
    CPLErr eErr = CE_None;
    GDALDataType eDataType = GDT_Byte;

/* -------------------------------------------------------------------- */
/*      Ensure that all bands share a common block size and data type.  */
/* -------------------------------------------------------------------- */
    for( int iBand = 0; iBand < nBandCount; iBand++ )
    {
        GDALRasterBand *poBand = GetRasterBand( panBandMap[iBand] );

        if( iBand == 0 )
        {
            poBand->GetBlockSize( &nBlockXSize, &nBlockYSize );
            eDataType = poBand->GetRasterDataType();
        }
        else
        {
          int nThisBlockXSize = 0;
          int nThisBlockYSize = 0;
            poBand->GetBlockSize( &nThisBlockXSize, &nThisBlockYSize );
            if( nThisBlockXSize != nBlockXSize
                || nThisBlockYSize != nBlockYSize )
            {
                CPLDebug( "GDAL",
                          "GDALDataset::BlockBasedRasterIO() ... "
                          "mismatched block sizes, use std method." );
                return BandBasedRasterIO( eRWFlag,
                                          nXOff, nYOff, nXSize, nYSize,
                                          pData, nBufXSize, nBufYSize,
                                          eBufType,
                                          nBandCount, panBandMap,
                                          nPixelSpace, nLineSpace,
                                          nBandSpace, psExtraArg );
            }

            if( eDataType != poBand->GetRasterDataType()
                && (nXSize != nBufXSize || nYSize != nBufYSize) )
            {
                CPLDebug( "GDAL",
                          "GDALDataset::BlockBasedRasterIO() ... "
                          "mismatched band data types, use std method." );
                return BandBasedRasterIO( eRWFlag,
                                          nXOff, nYOff, nXSize, nYSize,
                                          pData, nBufXSize, nBufYSize,
                                          eBufType,
                                          nBandCount, panBandMap,
                                          nPixelSpace, nLineSpace,
                                          nBandSpace, psExtraArg );
            }
        }
    }

/* ==================================================================== */
/*      In this special case at full resolution we step through in      */
/*      blocks, turning the request over to the per-band                */
/*      IRasterIO(), but ensuring that all bands of one block are       */
/*      called before proceeding to the next.                           */
/* ==================================================================== */

    if( nXSize == nBufXSize && nYSize == nBufYSize )
    {
        GDALRasterIOExtraArg sDummyExtraArg;
        INIT_RASTERIO_EXTRA_ARG(sDummyExtraArg);

        int nChunkYSize = 0;
        int nChunkXSize = 0;

        for( iBufYOff = 0; iBufYOff < nBufYSize; iBufYOff += nChunkYSize )
        {
            const int nChunkYOff = iBufYOff + nYOff;
            nChunkYSize = nBlockYSize - (nChunkYOff % nBlockYSize);
            if( nChunkYOff + nChunkYSize > nYOff + nYSize )
                nChunkYSize = (nYOff + nYSize) - nChunkYOff;

            for( iBufXOff = 0; iBufXOff < nBufXSize; iBufXOff += nChunkXSize )
            {
                const int nChunkXOff = iBufXOff + nXOff;
                nChunkXSize = nBlockXSize - (nChunkXOff % nBlockXSize);
                if( nChunkXOff + nChunkXSize > nXOff + nXSize )
                    nChunkXSize = (nXOff + nXSize) - nChunkXOff;

                GByte *pabyChunkData =
                    static_cast<GByte *>(pData)
                    + iBufXOff * nPixelSpace
                    + static_cast<GPtrDiff_t>(iBufYOff) * nLineSpace;

                for( int iBand = 0; iBand < nBandCount; iBand++ )
                {
                    GDALRasterBand *poBand = GetRasterBand(panBandMap[iBand]);

                    eErr =
                        poBand->GDALRasterBand::IRasterIO(
                            eRWFlag, nChunkXOff, nChunkYOff,
                            nChunkXSize, nChunkYSize,
                            pabyChunkData +
                            static_cast<GPtrDiff_t>(iBand) * nBandSpace,
                            nChunkXSize, nChunkYSize, eBufType,
                            nPixelSpace, nLineSpace, &sDummyExtraArg );
                    if( eErr != CE_None )
                        return eErr;
                }
            }

            if( psExtraArg->pfnProgress != nullptr &&
                !psExtraArg->pfnProgress(
                    1.0 * std::max(nBufYSize,
                                   iBufYOff + nChunkYSize) /
                    nBufYSize, "", psExtraArg->pProgressData) )
            {
                return CE_Failure;
            }
        }

        return CE_None;
    }

    /* Below code is not compatible with that case. It would need a complete */
    /* separate code like done in GDALRasterBand::IRasterIO. */
    if (eRWFlag == GF_Write && (nBufXSize < nXSize || nBufYSize < nYSize))
    {
        return BandBasedRasterIO( eRWFlag,
                                       nXOff, nYOff, nXSize, nYSize,
                                       pData, nBufXSize, nBufYSize,
                                       eBufType,
                                       nBandCount, panBandMap,
                                       nPixelSpace, nLineSpace,
                                       nBandSpace, psExtraArg );
    }

    /* We could have a smarter implementation, but that will do for now */
    if( psExtraArg->eResampleAlg != GRIORA_NearestNeighbour &&
        (nBufXSize != nXSize || nBufYSize != nYSize) )
    {
        return BandBasedRasterIO( eRWFlag,
                                       nXOff, nYOff, nXSize, nYSize,
                                       pData, nBufXSize, nBufYSize,
                                       eBufType,
                                       nBandCount, panBandMap,
                                       nPixelSpace, nLineSpace,
                                       nBandSpace, psExtraArg );
    }

/* ==================================================================== */
/*      Loop reading required source blocks to satisfy output           */
/*      request.  This is the most general implementation.              */
/* ==================================================================== */

    const int nBandDataSize = GDALGetDataTypeSizeBytes( eDataType );

    papabySrcBlock = static_cast<GByte **>(CPLCalloc(sizeof(GByte*),nBandCount));
    papoBlocks = static_cast<GDALRasterBlock **>(
        CPLCalloc(sizeof(void*), nBandCount) );

/* -------------------------------------------------------------------- */
/*      Select an overview level if appropriate.                        */
/* -------------------------------------------------------------------- */
    const int nOverviewLevel =
        GDALDatasetGetBestOverviewLevel( this,
                                         nXOff, nYOff, nXSize, nYSize,
                                         nBufXSize, nBufYSize,
                                         nBandCount, panBandMap);
    if (nOverviewLevel >= 0)
    {
        GetRasterBand(panBandMap[0])->GetOverview(nOverviewLevel)->
                                GetBlockSize( &nBlockXSize, &nBlockYSize );
    }

/* -------------------------------------------------------------------- */
/*      Compute stepping increment.                                     */
/* -------------------------------------------------------------------- */
    const double dfSrcXInc = nXSize / static_cast<double>( nBufXSize );
    const double dfSrcYInc = nYSize / static_cast<double>( nBufYSize );

    double dfSrcX = 0.0;
    double dfSrcY = 0.0;

    int nStartBlockX, nStartBlockY, nEndBlockX, nEndBlockY;

    nStartBlockX = nXOff / nBlockXSize;
    nStartBlockY = nYOff / nBlockYSize;
    nEndBlockX = (nXOff+nXSize-1) / nBlockXSize;
    nEndBlockY = (nYOff+nYSize-1) / nBlockYSize;

    int iBufYLim, iBufXLim;

            // FIXME: this code likely doesn't work if the dirty block gets flushed
            // to disk before being completely written.
            // In the meantime, bJustInitialize should probably be set to FALSE
            // even if it is not ideal performance wise, and for lossy compression

/* -------------------------------------------------------------------- */
/*     Iterate over the source blocks                                   */
/* -------------------------------------------------------------------- */

    for(nLBlockY = nStartBlockY; nLBlockY <= nEndBlockY; nLBlockY++)
    {
        for(nLBlockX = nStartBlockX; nLBlockX <= nEndBlockX; nLBlockX++)
        {
            
            const bool bJustInitialize = 
                eRWFlag == GF_Write
                && nYOff <= nLBlockY * nBlockYSize
                && nYOff + nYSize - nBlockYSize >= nLBlockY * nBlockYSize
                && nXOff <= nLBlockX * nBlockXSize
                && nXOff + nXSize - nBlockXSize >= nLBlockX * nBlockXSize;
 
                /*bool bMemZeroBuffer = FALSE;
                if( eRWFlag == GF_Write && !bJustInitialize &&
                    nXOff <= nLBlockX * nBlockXSize &&
                    nYOff <= nLBlockY * nBlockYSize &&
                    (nXOff + nXSize >= (nLBlockX+1) * nBlockXSize ||
                     (nXOff + nXSize == GetRasterXSize() &&
                     (nLBlockX+1) * nBlockXSize > GetRasterXSize())) &&
                    (nYOff + nYSize >= (nLBlockY+1) * nBlockYSize ||
                     (nYOff + nYSize == GetRasterYSize() &&
                     (nLBlockY+1) * nBlockYSize > GetRasterYSize())) )
                {
                    bJustInitialize = TRUE;
                    bMemZeroBuffer = TRUE;
                }*/
            for( int iBand = 0; iBand < nBandCount; iBand++ )
            {
                if( Interrupted() )
                {
                    eErr = CE_Interrupted;
                    goto CleanupAndReturn;
                }

                GDALRasterBand *poBand = GetRasterBand( panBandMap[iBand]);
                if (nOverviewLevel >= 0)
                    poBand = poBand->GetOverview(nOverviewLevel);
                poBlock = poBand->GetLockedBlockRef( nLBlockX, nLBlockY, 
                                                     bJustInitialize );
                if( poBlock == nullptr )
                {
                    eErr = CE_Failure;
                    goto CleanupAndReturn;
                }

                if( eRWFlag == GF_Write )
                    poBlock->MarkDirty();

                if( papoBlocks[iBand] != nullptr )
                    papoBlocks[iBand]->DropLock();

                papoBlocks[iBand] = poBlock;

                papabySrcBlock[iBand] = (GByte *) poBlock->GetDataRef();
                if( papabySrcBlock[iBand] == nullptr )
                {
                    eErr = CE_Failure; 
                    goto CleanupAndReturn;
                }
                    /*if( bMemZeroBuffer )
                    {
                        memset(papabySrcBlock[iBand], 0,
                            nBandDataSize * nBlockXSize * nBlockYSize);
                    }*/
            }

/* -------------------------------------------------------------------- */
/*      Loop over buffer region computing source locations.             */
/* -------------------------------------------------------------------- */
            iBufYOff = (int)((nLBlockY*nBlockYSize-nYOff)/dfSrcYInc);
            if(iBufYOff < 0)
                iBufYOff = 0;
            iBufYLim = (int)ceil(((nLBlockY+1)*nBlockYSize-nYOff)/dfSrcYInc);
            if(iBufYLim > nBufYSize)
                iBufYLim = nBufYSize;

            for( ; iBufYOff < iBufYLim; iBufYOff++ )
            {
                int     iBufOffset, iSrcOffset;

                dfSrcY = (iBufYOff+0.5) * dfSrcYInc + nYOff;
                iSrcY = (int) dfSrcY;
                if(iSrcY < nLBlockY*nBlockYSize)
                    iSrcY = nLBlockY*nBlockYSize;
                else if(iSrcY >= (nLBlockY+1)*nBlockYSize)
                    iSrcY = (nLBlockY+1)*nBlockYSize-1;

                iBufOffset = iBufYOff * nLineSpace;

                iBufXOff = (int)((nLBlockX*nBlockXSize-nXOff)/dfSrcXInc);
                if(iBufXOff < 0)
                    iBufXOff = 0;
                iBufXLim = (int)ceil(((nLBlockX+1)*nBlockXSize-nXOff)/dfSrcXInc);
                if(iBufXLim > nBufXSize)
                    iBufXLim = nBufXSize;
                // offset by the buffer x-pixel for the block
                iBufOffset += (iBufXOff*nPixelSpace);
                for( ; iBufXOff < iBufXLim; iBufXOff++ )
                {
                    int iSrcX;

                    dfSrcX = (iBufXOff+0.5) * dfSrcXInc + nXOff;

                    iSrcX = (int) dfSrcX;
                    if(iSrcX < nLBlockX*nBlockXSize)
                        iSrcX = nLBlockX*nBlockXSize;
                    else if(iSrcX >= (nLBlockX+1)*nBlockXSize)
                        iSrcX = (nLBlockX+1)*nBlockXSize-1;

/* -------------------------------------------------------------------- */
/*      Copy over this pixel of data.                                   */
/* -------------------------------------------------------------------- */
                    iSrcOffset = ((GPtrDiff_t)iSrcX - (GPtrDiff_t)nLBlockX*nBlockXSize
                        + ((GPtrDiff_t)iSrcY - (GPtrDiff_t)nLBlockY*nBlockYSize) * nBlockXSize)*nBandDataSize;

                    for( int iBand = 0; iBand < nBandCount; iBand++ )
                    {
                        GByte *pabySrcBlock = papabySrcBlock[iBand];
                        GPtrDiff_t iBandBufOffset = iBufOffset + (GPtrDiff_t)iBand * (GPtrDiff_t)nBandSpace;

                        if( eDataType == eBufType )
                        {
                            if( eRWFlag == GF_Read )
                                memcpy( ((GByte *) pData) + iBandBufOffset,
                                        pabySrcBlock + iSrcOffset, nBandDataSize );
                        else
                            memcpy( pabySrcBlock + iSrcOffset, 
                                    ((GByte *)pData) + iBandBufOffset, nBandDataSize );
                        }
                        else
                        {
                            /* type to type conversion ... ouch, this is expensive way
                               of handling single words */

                            if( eRWFlag == GF_Read )
                                GDALCopyWords( pabySrcBlock + iSrcOffset, eDataType, 0,
                                               ((GByte *) pData) + iBandBufOffset, 
                                               eBufType, 0, 1 );
                            else
                                GDALCopyWords( ((GByte *) pData) + iBandBufOffset, 
                                               eBufType, 0,
                                               pabySrcBlock + iSrcOffset, eDataType, 0,
                                               1 );
                        }
                    }

                    iBufOffset += static_cast<int>(nPixelSpace);
                }
            }           
        }
    }

/* -------------------------------------------------------------------- */
/*      CleanupAndReturn.                                               */
/* -------------------------------------------------------------------- */
  CleanupAndReturn:
    CPLFree( papabySrcBlock );
    if( papoBlocks != nullptr )
    {
        for( int iBand = 0; iBand < nBandCount; iBand++ )
        {
            if( papoBlocks[iBand] != nullptr )
                papoBlocks[iBand]->DropLock();
        }
        CPLFree( papoBlocks );
    }

    return eErr;
}
//! @endcond

/************************************************************************/
/*                  GDALCopyWholeRasterGetSwathSize()                   */
/************************************************************************/

static void GDALCopyWholeRasterGetSwathSize(
    GDALRasterBand *poSrcPrototypeBand,
    GDALRasterBand *poDstPrototypeBand,
    int nBandCount,
    int bDstIsCompressed, int bInterleave,
    int* pnSwathCols, int *pnSwathLines )
{
    GDALDataType eDT = poDstPrototypeBand->GetRasterDataType();
    int nSrcBlockXSize = 0;
    int nSrcBlockYSize = 0;
    int nBlockXSize = 0;
    int nBlockYSize = 0;

    int nXSize = poSrcPrototypeBand->GetXSize();
    int nYSize = poSrcPrototypeBand->GetYSize();

    poSrcPrototypeBand->GetBlockSize( &nSrcBlockXSize, &nSrcBlockYSize );
    poDstPrototypeBand->GetBlockSize( &nBlockXSize, &nBlockYSize );

    const int nMaxBlockXSize = std::max(nBlockXSize, nSrcBlockXSize);
    const int nMaxBlockYSize = std::max(nBlockYSize, nSrcBlockYSize);

    int nPixelSize = GDALGetDataTypeSizeBytes(eDT);
    if( bInterleave)
        nPixelSize *= nBandCount;

    // aim for one row of blocks.  Do not settle for less.
    int nSwathCols  = nXSize;
    int nSwathLines = nBlockYSize;

    const char* pszSrcCompression =
        poSrcPrototypeBand->GetMetadataItem("COMPRESSION", "IMAGE_STRUCTURE");

/* -------------------------------------------------------------------- */
/*      What will our swath size be?                                    */
/* -------------------------------------------------------------------- */
    // When writing interleaved data in a compressed format, we want to be sure
    // that each block will only be written once, so the swath size must not be
    // greater than the block cache.
    const char* pszSwathSize = CPLGetConfigOption("GDAL_SWATH_SIZE", nullptr);
    int nTargetSwathSize;
    if( pszSwathSize != nullptr )
        nTargetSwathSize = static_cast<int>(
            std::min(GIntBig(INT_MAX), CPLAtoGIntBig(pszSwathSize)));
    else
    {
      // As a default, take one 1/4 of the cache size.
        nTargetSwathSize = static_cast<int>(
            std::min(GIntBig(INT_MAX), GDALGetCacheMax64() / 4));

        // but if the minimum idal swath buf size is less, then go for it to
        // avoid unnecessarily abusing RAM usage.
        // but try to use 10 MB at least.
        GIntBig nIdealSwathBufSize =
            static_cast<GIntBig>(nSwathCols) * nSwathLines * nPixelSize;
        if( nIdealSwathBufSize < nTargetSwathSize &&
            nIdealSwathBufSize < 10 * 1000 * 1000 )
        {
            nIdealSwathBufSize = 10 * 1000 * 1000;
        }
        if( pszSrcCompression != nullptr && EQUAL(pszSrcCompression, "JPEG2000") &&
            (!bDstIsCompressed || ((nSrcBlockXSize % nBlockXSize) == 0 &&
                                   (nSrcBlockYSize % nBlockYSize) == 0)) )
        {
            nIdealSwathBufSize =
                std::max( nIdealSwathBufSize,
                          static_cast<GIntBig>(nSwathCols) *
                          nSrcBlockYSize * nPixelSize) ;
        }
        if( nTargetSwathSize > nIdealSwathBufSize )
            nTargetSwathSize = static_cast<int>(
                std::min(GIntBig(INT_MAX), nIdealSwathBufSize));
    }

    if (nTargetSwathSize < 1000000)
        nTargetSwathSize = 1000000;

    /* But let's check that  */
    if( bDstIsCompressed && bInterleave &&
        nTargetSwathSize > GDALGetCacheMax64() )
    {
        CPLError(
            CE_Warning, CPLE_AppDefined,
            "When translating into a compressed interleave format, "
            "the block cache size (" CPL_FRMT_GIB ") "
            "should be at least the size of the swath (%d) "
            "(GDAL_SWATH_SIZE config. option)",
            GDALGetCacheMax64(), nTargetSwathSize);
    }

#define IS_DIVIDER_OF(x,y) ((y)%(x) == 0)
#define ROUND_TO(x,y) (((x)/(y))*(y))

    // if both input and output datasets are tiled, that the tile dimensions
    // are "compatible", try to stick  to a swath dimension that is a multiple
    // of input and output block dimensions.
    if (nBlockXSize != nXSize && nSrcBlockXSize != nXSize &&
        IS_DIVIDER_OF(nBlockXSize, nMaxBlockXSize) &&
        IS_DIVIDER_OF(nSrcBlockXSize, nMaxBlockXSize) &&
        IS_DIVIDER_OF(nBlockYSize, nMaxBlockYSize) &&
        IS_DIVIDER_OF(nSrcBlockYSize, nMaxBlockYSize))
    {
        if( static_cast<GIntBig>(nMaxBlockXSize) *
            nMaxBlockYSize * nPixelSize <=
            static_cast<GIntBig>(nTargetSwathSize) )
        {
            nSwathCols = nTargetSwathSize / (nMaxBlockYSize * nPixelSize);
            nSwathCols = ROUND_TO(nSwathCols, nMaxBlockXSize);
            if (nSwathCols == 0)
                nSwathCols = nMaxBlockXSize;
            if (nSwathCols > nXSize)
                nSwathCols = nXSize;
            nSwathLines = nMaxBlockYSize;

            if (static_cast<GIntBig>(nSwathCols) * nSwathLines * nPixelSize >
                static_cast<GIntBig>(nTargetSwathSize))
            {
                nSwathCols  = nXSize;
                nSwathLines = nBlockYSize;
            }
        }
    }

    const GIntBig nMemoryPerCol = static_cast<GIntBig>(nSwathCols) * nPixelSize;
    const GIntBig nSwathBufSize = nMemoryPerCol * nSwathLines;
    if( nSwathBufSize > static_cast<GIntBig>(nTargetSwathSize) )
    {
        nSwathLines = static_cast<int>(nTargetSwathSize / nMemoryPerCol);
        if (nSwathLines == 0)
            nSwathLines = 1;

        CPLDebug(
            "GDAL",
            "GDALCopyWholeRasterGetSwathSize(): adjusting to %d line swath "
            "since requirement (" CPL_FRMT_GIB " bytes) exceed target swath "
            "size (%d bytes) (GDAL_SWATH_SIZE config. option)",
            nSwathLines,
            nBlockYSize * nMemoryPerCol,
            nTargetSwathSize);
    }
    // If we are processing single scans, try to handle several at once.
    // If we are handling swaths already, only grow the swath if a row
    // of blocks is substantially less than our target buffer size.
    else if( nSwathLines == 1
        || nMemoryPerCol * nSwathLines <
                        static_cast<GIntBig>(nTargetSwathSize) / 10 )
    {
        nSwathLines =
            std::min(nYSize, std::max(1,
                     static_cast<int>(nTargetSwathSize / nMemoryPerCol)));

        /* If possible try to align to source and target block height */
        if ((nSwathLines % nMaxBlockYSize) != 0 &&
            nSwathLines > nMaxBlockYSize &&
            IS_DIVIDER_OF(nBlockYSize, nMaxBlockYSize) &&
            IS_DIVIDER_OF(nSrcBlockYSize, nMaxBlockYSize))
            nSwathLines = ROUND_TO(nSwathLines, nMaxBlockYSize);
    }

    if( pszSrcCompression != nullptr && EQUAL(pszSrcCompression, "JPEG2000") &&
        (!bDstIsCompressed ||
            (IS_DIVIDER_OF(nBlockXSize, nSrcBlockXSize) &&
             IS_DIVIDER_OF(nBlockYSize, nSrcBlockYSize))) )
    {
        // Typical use case: converting from Pleaiades that is 2048x2048 tiled.
        if( nSwathLines < nSrcBlockYSize )
        {
            nSwathLines = nSrcBlockYSize;

            // Number of pixels that can be read/write simultaneously.
            nSwathCols = nTargetSwathSize / (nSrcBlockXSize * nPixelSize);
            nSwathCols = ROUND_TO(nSwathCols, nSrcBlockXSize);
            if (nSwathCols == 0)
                nSwathCols = nSrcBlockXSize;
            if (nSwathCols > nXSize)
                nSwathCols = nXSize;

            CPLDebug(
                "GDAL",
                "GDALCopyWholeRasterGetSwathSize(): because of compression and "
                "too high block, "
                "use partial width at one time" );
        }
        else if ((nSwathLines % nSrcBlockYSize) != 0)
        {
            /* Round on a multiple of nSrcBlockYSize */
            nSwathLines = ROUND_TO(nSwathLines, nSrcBlockYSize);
            CPLDebug(
                "GDAL",
                "GDALCopyWholeRasterGetSwathSize(): because of compression, "
                "round nSwathLines to block height : %d", nSwathLines);
        }
    }
    else if (bDstIsCompressed)
    {
        if (nSwathLines < nBlockYSize)
        {
            nSwathLines = nBlockYSize;

            // Number of pixels that can be read/write simultaneously.
            nSwathCols = nTargetSwathSize / (nSwathLines * nPixelSize);
            nSwathCols = ROUND_TO(nSwathCols, nBlockXSize);
            if (nSwathCols == 0)
                nSwathCols = nBlockXSize;
            if (nSwathCols > nXSize)
                nSwathCols = nXSize;

            CPLDebug(
                "GDAL",
                "GDALCopyWholeRasterGetSwathSize(): because of compression and "
                "too high block, "
                "use partial width at one time" );
        }
        else if ((nSwathLines % nBlockYSize) != 0)
        {
            // Round on a multiple of nBlockYSize.
            nSwathLines = ROUND_TO(nSwathLines, nBlockYSize);
            CPLDebug(
                "GDAL",
                "GDALCopyWholeRasterGetSwathSize(): because of compression, "
                "round nSwathLines to block height : %d", nSwathLines);
        }
    }

    *pnSwathCols = nSwathCols;
    *pnSwathLines = nSwathLines;
}

/************************************************************************/
/*                     GDALDatasetCopyWholeRaster()                     */
/************************************************************************/

/**
 * \brief Copy all dataset raster data.
 *
 * This function copies the complete raster contents of one dataset to
 * another similarly configured dataset.  The source and destination
 * dataset must have the same number of bands, and the same width
 * and height.  The bands do not have to have the same data type.
 *
 * This function is primarily intended to support implementation of
 * driver specific CreateCopy() functions.  It implements efficient copying,
 * in particular "chunking" the copy in substantial blocks and, if appropriate,
 * performing the transfer in a pixel interleaved fashion.
 *
 * Currently the only papszOptions value supported are :
 * <ul>
 * <li>"INTERLEAVE=PIXEL" to force pixel interleaved operation</li>
 * <li>"COMPRESSED=YES" to force alignment on target dataset block sizes to
 * achieve best compression.</li>
 * <li>"SKIP_HOLES=YES" to skip chunks for which GDALGetDataCoverageStatus()
 * returns GDAL_DATA_COVERAGE_STATUS_EMPTY (GDAL &gt;= 2.2)</li>
 * </ul>
 * More options may be supported in the future.
 *
 * @param hSrcDS the source dataset
 * @param hDstDS the destination dataset
 * @param papszOptions transfer hints in "StringList" Name=Value format.
 * @param pfnProgress progress reporting function.
 * @param pProgressData callback data for progress function.
 *
 * @return CE_None on success, or CE_Failure on failure.
 */

CPLErr CPL_STDCALL GDALDatasetCopyWholeRaster(
    GDALDatasetH hSrcDS, GDALDatasetH hDstDS, CSLConstList papszOptions,
    GDALProgressFunc pfnProgress, void *pProgressData )

{
    VALIDATE_POINTER1( hSrcDS, "GDALDatasetCopyWholeRaster", CE_Failure );
    VALIDATE_POINTER1( hDstDS, "GDALDatasetCopyWholeRaster", CE_Failure );

    GDALDataset *poSrcDS = GDALDataset::FromHandle(hSrcDS);
    GDALDataset *poDstDS = GDALDataset::FromHandle(hDstDS);

    if( pfnProgress == nullptr )
        pfnProgress = GDALDummyProgress;

/* -------------------------------------------------------------------- */
/*      Confirm the datasets match in size and band counts.             */
/* -------------------------------------------------------------------- */
    const int nXSize = poDstDS->GetRasterXSize();
    const int nYSize = poDstDS->GetRasterYSize();
    const int nBandCount = poDstDS->GetRasterCount();

    if( poSrcDS->GetRasterXSize() != nXSize
        || poSrcDS->GetRasterYSize() != nYSize
        || poSrcDS->GetRasterCount() != nBandCount )
    {
        CPLError( CE_Failure, CPLE_AppDefined,
                  "Input and output dataset sizes or band counts do not\n"
                  "match in GDALDatasetCopyWholeRaster()" );
        return CE_Failure;
    }

/* -------------------------------------------------------------------- */
/*      Report preliminary (0) progress.                                */
/* -------------------------------------------------------------------- */
    if( !pfnProgress( 0.0, nullptr, pProgressData ) )
    {
        CPLError( CE_Failure, CPLE_UserInterrupt,
                  "User terminated CreateCopy()" );
        return CE_Failure;
    }

/* -------------------------------------------------------------------- */
/*      Get our prototype band, and assume the others are similarly     */
/*      configured.                                                     */
/* -------------------------------------------------------------------- */
    if( nBandCount == 0 )
        return CE_None;

    GDALRasterBand *poSrcPrototypeBand = poSrcDS->GetRasterBand(1);
    GDALRasterBand *poDstPrototypeBand = poDstDS->GetRasterBand(1);
    GDALDataType eDT = poDstPrototypeBand->GetRasterDataType();

/* -------------------------------------------------------------------- */
/*      Do we want to try and do the operation in a pixel               */
/*      interleaved fashion?                                            */
/* -------------------------------------------------------------------- */
    bool bInterleave = false;
    const char *pszInterleave = poSrcDS->GetMetadataItem(
                                            "INTERLEAVE", "IMAGE_STRUCTURE");
    if( pszInterleave != nullptr
        && (EQUAL(pszInterleave,"PIXEL") || EQUAL(pszInterleave,"LINE")) )
        bInterleave = true;

    pszInterleave = poDstDS->GetMetadataItem( "INTERLEAVE", "IMAGE_STRUCTURE");
    if( pszInterleave != nullptr
        && (EQUAL(pszInterleave,"PIXEL") || EQUAL(pszInterleave,"LINE")) )
        bInterleave = true;

    pszInterleave = CSLFetchNameValue( papszOptions, "INTERLEAVE" );
    if( pszInterleave != nullptr
        && (EQUAL(pszInterleave, "PIXEL") || EQUAL(pszInterleave, "LINE")) )
        bInterleave = true;
    else if( pszInterleave != nullptr && EQUAL(pszInterleave, "BAND") )
        bInterleave = false;

    // If the destination is compressed, we must try to write blocks just once,
    // to save disk space (GTiff case for example), and to avoid data loss
    // (JPEG compression for example).
    bool bDstIsCompressed = false;
    const char* pszDstCompressed =
        CSLFetchNameValue( papszOptions, "COMPRESSED" );
    if (pszDstCompressed != nullptr && CPLTestBool(pszDstCompressed))
        bDstIsCompressed = true;

/* -------------------------------------------------------------------- */
/*      What will our swath size be?                                    */
/* -------------------------------------------------------------------- */

    int nSwathCols = 0;
    int nSwathLines = 0;
    GDALCopyWholeRasterGetSwathSize( poSrcPrototypeBand,
                                     poDstPrototypeBand,
                                     nBandCount,
                                     bDstIsCompressed, bInterleave,
                                     &nSwathCols, &nSwathLines );

    int nPixelSize = GDALGetDataTypeSizeBytes(eDT);
    if( bInterleave)
        nPixelSize *= nBandCount;

    void *pSwathBuf = VSI_MALLOC3_VERBOSE(nSwathCols, nSwathLines, nPixelSize );
    if( pSwathBuf == nullptr )
    {
        return CE_Failure;
    }

    CPLDebug( "GDAL",
              "GDALDatasetCopyWholeRaster(): %d*%d swaths, bInterleave=%d",
              nSwathCols, nSwathLines, static_cast<int>(bInterleave) );

    // Advise the source raster that we are going to read it completely
    // Note: this might already have been done by GDALCreateCopy() in the
    // likely case this function is indirectly called by it
    poSrcDS->AdviseRead( 0, 0, nXSize, nYSize, nXSize, nYSize, eDT,
                         nBandCount, nullptr, nullptr );

/* ==================================================================== */
/*      Band oriented (uninterleaved) case.                             */
/* ==================================================================== */
    CPLErr eErr = CE_None;
    const bool bCheckHoles = CPLTestBool( CSLFetchNameValueDef(
                                        papszOptions, "SKIP_HOLES", "NO" ) );

    if( !bInterleave )
    {
        GDALRasterIOExtraArg sExtraArg;
        INIT_RASTERIO_EXTRA_ARG(sExtraArg);

        const GIntBig nTotalBlocks =
            static_cast<GIntBig>(nBandCount) *
            DIV_ROUND_UP(nYSize, nSwathLines) *
            DIV_ROUND_UP(nXSize, nSwathCols);
        GIntBig nBlocksDone = 0;

        for( int iBand = 0; iBand < nBandCount && eErr == CE_None; iBand++ )
        {
            int nBand = iBand + 1;

            for( int iY = 0; iY < nYSize && eErr == CE_None; iY += nSwathLines )
            {
                int nThisLines = nSwathLines;

                if( iY + nThisLines > nYSize )
                    nThisLines = nYSize - iY;

                for( int iX = 0;
                     iX < nXSize && eErr == CE_None;
                     iX += nSwathCols )
                {
                    int nThisCols = nSwathCols;

                    if( iX + nThisCols > nXSize )
                        nThisCols = nXSize - iX;

                    int nStatus = GDAL_DATA_COVERAGE_STATUS_DATA;
                    if( bCheckHoles )
                    {
                        nStatus = poSrcDS->GetRasterBand(nBand)->GetDataCoverageStatus(
                              iX, iY, nThisCols, nThisLines,
                              GDAL_DATA_COVERAGE_STATUS_DATA);
                    }
                    if( nStatus & GDAL_DATA_COVERAGE_STATUS_DATA )
                    {
                        sExtraArg.pfnProgress = GDALScaledProgress;
                        sExtraArg.pProgressData =
                            GDALCreateScaledProgress(
                                nBlocksDone / static_cast<double>(nTotalBlocks),
                                (nBlocksDone + 0.5) /
                                static_cast<double>(nTotalBlocks),
                                pfnProgress,
                                pProgressData );
                        if( sExtraArg.pProgressData == nullptr )
                            sExtraArg.pfnProgress = nullptr;

                        eErr = poSrcDS->RasterIO( GF_Read,
                                                  iX, iY, nThisCols, nThisLines,
                                                  pSwathBuf, nThisCols, nThisLines,
                                                  eDT, 1, &nBand,
                                                  0, 0, 0, &sExtraArg );

                        GDALDestroyScaledProgress( sExtraArg.pProgressData );

                        if( eErr == CE_None )
                            eErr = poDstDS->RasterIO( GF_Write,
                                                      iX, iY, nThisCols, nThisLines,
                                                      pSwathBuf, nThisCols,
                                                      nThisLines,
                                                      eDT, 1, &nBand,
                                                      0, 0, 0, nullptr );
                    }

                    nBlocksDone++;
                    if( eErr == CE_None
                        && !pfnProgress(
                            nBlocksDone / static_cast<double>(nTotalBlocks),
                            nullptr, pProgressData ) )
                    {
                        eErr = CE_Failure;
                        CPLError( CE_Failure, CPLE_UserInterrupt,
                                  "User terminated CreateCopy()" );
                    }
                }
            }
        }
    }

/* ==================================================================== */
/*      Pixel interleaved case.                                         */
/* ==================================================================== */
    else /* if( bInterleave ) */
    {
        GDALRasterIOExtraArg sExtraArg;
        INIT_RASTERIO_EXTRA_ARG(sExtraArg);

        const GIntBig nTotalBlocks =
            static_cast<GIntBig>(DIV_ROUND_UP(nYSize, nSwathLines)) *
            DIV_ROUND_UP(nXSize, nSwathCols);
        GIntBig nBlocksDone = 0;

        for( int iY = 0; iY < nYSize && eErr == CE_None; iY += nSwathLines )
        {
            int nThisLines = nSwathLines;

            if( iY + nThisLines > nYSize )
                nThisLines = nYSize - iY;

            for( int iX = 0; iX < nXSize && eErr == CE_None; iX += nSwathCols )
            {
                int nThisCols = nSwathCols;

                if( iX + nThisCols > nXSize )
                    nThisCols = nXSize - iX;

                int nStatus = GDAL_DATA_COVERAGE_STATUS_DATA;
                if( bCheckHoles )
                {
                    for( int iBand = 0; iBand < nBandCount; iBand++ )
                    {
                        nStatus |= poSrcDS->GetRasterBand(iBand+1)->GetDataCoverageStatus(
                          iX, iY, nThisCols, nThisLines,
                          GDAL_DATA_COVERAGE_STATUS_DATA);
                        if( nStatus & GDAL_DATA_COVERAGE_STATUS_DATA )
                            break;
                    }
                }
                if( nStatus & GDAL_DATA_COVERAGE_STATUS_DATA )
                {
                    sExtraArg.pfnProgress = GDALScaledProgress;
                    sExtraArg.pProgressData =
                        GDALCreateScaledProgress(
                            nBlocksDone / static_cast<double>(nTotalBlocks),
                            (nBlocksDone + 0.5) / static_cast<double>(nTotalBlocks),
                            pfnProgress,
                            pProgressData );
                    if( sExtraArg.pProgressData == nullptr )
                        sExtraArg.pfnProgress = nullptr;

                    eErr = poSrcDS->RasterIO( GF_Read,
                                              iX, iY, nThisCols, nThisLines,
                                              pSwathBuf, nThisCols, nThisLines,
                                              eDT, nBandCount, nullptr,
                                              0, 0, 0, &sExtraArg );

                    GDALDestroyScaledProgress( sExtraArg.pProgressData );

                    if( eErr == CE_None )
                        eErr = poDstDS->RasterIO( GF_Write,
                                                  iX, iY, nThisCols, nThisLines,
                                                  pSwathBuf, nThisCols, nThisLines,
                                                  eDT, nBandCount, nullptr,
                                                  0, 0, 0, nullptr );
                }

                nBlocksDone++;
                if( eErr == CE_None &&
                    !pfnProgress(
                        nBlocksDone / static_cast<double>( nTotalBlocks ),
                        nullptr, pProgressData ) )
                {
                    eErr = CE_Failure;
                    CPLError( CE_Failure, CPLE_UserInterrupt,
                            "User terminated CreateCopy()" );
                }
            }
        }
    }

/* -------------------------------------------------------------------- */
/*      Cleanup                                                         */
/* -------------------------------------------------------------------- */
    CPLFree( pSwathBuf );

    return eErr;
}

/************************************************************************/
/*                     GDALRasterBandCopyWholeRaster()                  */
/************************************************************************/

/**
 * \brief Copy a whole raster band
 *
 * This function copies the complete raster contents of one band to
 * another similarly configured band.  The source and destination
 * bands must have the same width and height.  The bands do not have
 * to have the same data type.
 *
 * It implements efficient copying, in particular "chunking" the copy in
 * substantial blocks.
 *
 * Currently the only papszOptions value supported are :
 * <ul>
 * <li>"COMPRESSED=YES" to force alignment on target dataset block sizes to
 * achieve best compression.</li>
 * <li>"SKIP_HOLES=YES" to skip chunks for which GDALGetDataCoverageStatus()
 * returns GDAL_DATA_COVERAGE_STATUS_EMPTY (GDAL &gt;= 2.2)</li>
 * </ul>
 *
 * @param hSrcBand the source band
 * @param hDstBand the destination band
 * @param papszOptions transfer hints in "StringList" Name=Value format.
 * @param pfnProgress progress reporting function.
 * @param pProgressData callback data for progress function.
 *
 * @return CE_None on success, or CE_Failure on failure.
 */

CPLErr CPL_STDCALL GDALRasterBandCopyWholeRaster(
    GDALRasterBandH hSrcBand, GDALRasterBandH hDstBand,
    const char * const * const papszOptions,
    GDALProgressFunc pfnProgress, void *pProgressData )

{
    VALIDATE_POINTER1( hSrcBand, "GDALRasterBandCopyWholeRaster", CE_Failure );
    VALIDATE_POINTER1( hDstBand, "GDALRasterBandCopyWholeRaster", CE_Failure );

    GDALRasterBand *poSrcBand = GDALRasterBand::FromHandle( hSrcBand );
    GDALRasterBand *poDstBand = GDALRasterBand::FromHandle( hDstBand );
    CPLErr eErr = CE_None;

    if( pfnProgress == nullptr )
        pfnProgress = GDALDummyProgress;

/* -------------------------------------------------------------------- */
/*      Confirm the datasets match in size and band counts.             */
/* -------------------------------------------------------------------- */
    int nXSize = poSrcBand->GetXSize();
    int nYSize = poSrcBand->GetYSize();

    if( poDstBand->GetXSize() != nXSize
        || poDstBand->GetYSize() != nYSize )
    {
        CPLError( CE_Failure, CPLE_AppDefined,
                  "Input and output band sizes do not\n"
                  "match in GDALRasterBandCopyWholeRaster()" );
        return CE_Failure;
    }

/* -------------------------------------------------------------------- */
/*      Report preliminary (0) progress.                                */
/* -------------------------------------------------------------------- */
    if( !pfnProgress( 0.0, nullptr, pProgressData ) )
    {
        CPLError( CE_Failure, CPLE_UserInterrupt,
                  "User terminated CreateCopy()" );
        return CE_Failure;
    }

    GDALDataType eDT = poDstBand->GetRasterDataType();

    // If the destination is compressed, we must try to write blocks just once,
    // to save disk space (GTiff case for example), and to avoid data loss
    // (JPEG compression for example).
    bool bDstIsCompressed = false;
    const char* pszDstCompressed =
        CSLFetchNameValue( const_cast<char **>(papszOptions), "COMPRESSED" );
    if (pszDstCompressed != nullptr && CPLTestBool(pszDstCompressed))
        bDstIsCompressed = true;

/* -------------------------------------------------------------------- */
/*      What will our swath size be?                                    */
/* -------------------------------------------------------------------- */

    int nSwathCols = 0;
    int nSwathLines = 0;
    GDALCopyWholeRasterGetSwathSize( poSrcBand,
                                     poDstBand,
                                     1,
                                     bDstIsCompressed, FALSE,
                                     &nSwathCols, &nSwathLines);

    const int nPixelSize = GDALGetDataTypeSizeBytes(eDT);

    void *pSwathBuf = VSI_MALLOC3_VERBOSE(nSwathCols, nSwathLines, nPixelSize );
    if( pSwathBuf == nullptr )
    {
        return CE_Failure;
    }

    CPLDebug( "GDAL",
              "GDALRasterBandCopyWholeRaster(): %d*%d swaths",
              nSwathCols, nSwathLines );

    const bool bCheckHoles = CPLTestBool( CSLFetchNameValueDef(
                    papszOptions, "SKIP_HOLES", "NO" ) );

    // Advise the source raster that we are going to read it completely
    poSrcBand->AdviseRead( 0, 0, nXSize, nYSize, nXSize, nYSize, eDT, nullptr );

/* ==================================================================== */
/*      Band oriented (uninterleaved) case.                             */
/* ==================================================================== */

    for( int iY = 0; iY < nYSize && eErr == CE_None; iY += nSwathLines )
    {
        int nThisLines = nSwathLines;

        if( iY + nThisLines > nYSize )
            nThisLines = nYSize - iY;

        for( int iX = 0; iX < nXSize && eErr == CE_None; iX += nSwathCols )
        {
            int nThisCols = nSwathCols;

            if( iX + nThisCols > nXSize )
                nThisCols = nXSize - iX;

            int nStatus = GDAL_DATA_COVERAGE_STATUS_DATA;
            if( bCheckHoles )
            {
                nStatus = poSrcBand->GetDataCoverageStatus(
                        iX, iY, nThisCols, nThisLines,
                        GDAL_DATA_COVERAGE_STATUS_DATA);
            }
            if( nStatus & GDAL_DATA_COVERAGE_STATUS_DATA )
            {
                eErr = poSrcBand->RasterIO( GF_Read,
                                            iX, iY, nThisCols, nThisLines,
                                            pSwathBuf, nThisCols, nThisLines,
                                            eDT, 0, 0, nullptr );

                if( eErr == CE_None )
                    eErr = poDstBand->RasterIO( GF_Write,
                                                iX, iY, nThisCols, nThisLines,
                                                pSwathBuf, nThisCols, nThisLines,
                                                eDT, 0, 0, nullptr );
            }

            if( eErr == CE_None
                && !pfnProgress(
                    (iY + nThisLines) / static_cast<float>(nYSize),
                    nullptr, pProgressData ) )
            {
                eErr = CE_Failure;
                CPLError( CE_Failure, CPLE_UserInterrupt,
                          "User terminated CreateCopy()" );
            }
        }
    }

/* -------------------------------------------------------------------- */
/*      Cleanup                                                         */
/* -------------------------------------------------------------------- */
    CPLFree( pSwathBuf );

    return eErr;
}

/************************************************************************/
/*                      GDALCopyRasterIOExtraArg ()                     */
/************************************************************************/

void GDALCopyRasterIOExtraArg( GDALRasterIOExtraArg* psDestArg,
                               GDALRasterIOExtraArg* psSrcArg )
{
    INIT_RASTERIO_EXTRA_ARG(*psDestArg);
    if( psSrcArg )
    {
        psDestArg->eResampleAlg = psSrcArg->eResampleAlg;
        psDestArg->pfnProgress = psSrcArg->pfnProgress;
        psDestArg->pProgressData = psSrcArg->pProgressData;
        psDestArg->bFloatingPointWindowValidity =
            psSrcArg->bFloatingPointWindowValidity;
        if( psSrcArg->bFloatingPointWindowValidity )
        {
            psDestArg->dfXOff = psSrcArg->dfXOff;
            psDestArg->dfYOff = psSrcArg->dfYOff;
            psDestArg->dfXSize = psSrcArg->dfXSize;
            psDestArg->dfYSize = psSrcArg->dfYSize;
        }
    }
}
