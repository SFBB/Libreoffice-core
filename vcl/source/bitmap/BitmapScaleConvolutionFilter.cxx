/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * This file is part of the LibreOffice project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * This file incorporates work covered by the following license notice:
 *
 *   Licensed to the Apache Software Foundation (ASF) under one or more
 *   contributor license agreements. See the NOTICE file distributed
 *   with this work for additional information regarding copyright
 *   ownership. The ASF licenses this file to you under the Apache
 *   License, Version 2.0 (the "License"); you may not use this file
 *   except in compliance with the License. You may obtain a copy of
 *   the License at http://www.apache.org/licenses/LICENSE-2.0 .
 */

#include <osl/diagnose.h>
#include <tools/helpers.hxx>

#include <vcl/BitmapWriteAccess.hxx>
#include <bitmap/BitmapScaleConvolutionFilter.hxx>

#include <algorithm>
#include <memory>

namespace vcl
{

namespace
{

void ImplCalculateContributions(
    const sal_Int32 aSourceSize,
    const sal_Int32 aDestinationSize,
    sal_Int32& aNumberOfContributions,
    std::vector<sal_Int16>& rWeights,
    std::vector<sal_Int32>& rPixels,
    std::vector<sal_Int32>& rCounts,
    const Kernel& aKernel)
{
    const double fSamplingRadius(aKernel.GetWidth());
    const double fScale(aDestinationSize / static_cast< double >(aSourceSize));
    const double fScaledRadius((fScale < 1.0) ? fSamplingRadius / fScale : fSamplingRadius);
    const double fFilterFactor(std::min(fScale, 1.0));

    aNumberOfContributions = (sal_Int32(fabs(ceil(fScaledRadius))) * 2) + 1;
    const sal_Int32 nAllocSize(aDestinationSize * aNumberOfContributions);
    rWeights.resize(nAllocSize);
    rPixels.resize(nAllocSize);
    rCounts.resize(aDestinationSize);

    for(sal_Int32 i(0); i < aDestinationSize; i++)
    {
        const sal_Int32 aIndex(i * aNumberOfContributions);
        const double aCenter(i / fScale);
        const sal_Int32 aLeft(static_cast< sal_Int32 >(floor(aCenter - fScaledRadius)));
        const sal_Int32 aRight(static_cast< sal_Int32 >(ceil(aCenter + fScaledRadius)));
        sal_Int32 aCurrentCount(0);

        for(sal_Int32 j(aLeft); j <= aRight; j++)
        {
            const double aWeight(aKernel.Calculate(fFilterFactor * (aCenter - static_cast< double>(j))));

            // Reduce calculations with ignoring weights of 0.0
            if(fabs(aWeight) < 0.0001)
            {
                continue;
            }

            // Handling on edges
            const sal_Int32 aPixelIndex(std::clamp(j, sal_Int32(0), aSourceSize - 1));
            const sal_Int32 nIndex(aIndex + aCurrentCount);

            // scale the weight by 255 since we're converting from float to int
            rWeights[nIndex] = aWeight * 255;
            rPixels[nIndex] = aPixelIndex;

            aCurrentCount++;
        }

        rCounts[i] = aCurrentCount;
    }
}

bool ImplScaleConvolutionHor(Bitmap& rSource, Bitmap& rTarget, const double& rScaleX, const Kernel& aKernel)
{
    // Do horizontal filtering
    OSL_ENSURE(rScaleX > 0.0, "Error in scaling: Mirror given in non-mirror-capable method (!)");
    const sal_Int32 nWidth(rSource.GetSizePixel().Width());
    const sal_Int32 nNewWidth(FRound(nWidth * rScaleX));

    if(nWidth == nNewWidth)
    {
        return true;
    }

    BitmapScopedReadAccess pReadAcc(rSource);

    if(pReadAcc)
    {
        std::vector<sal_Int16> aWeights;
        std::vector<sal_Int32> aPixels;
        std::vector<sal_Int32> aCounts;
        sal_Int32 aNumberOfContributions(0);

        const sal_Int32 nHeight(rSource.GetSizePixel().Height());
        ImplCalculateContributions(nWidth, nNewWidth, aNumberOfContributions, aWeights, aPixels, aCounts, aKernel);
        rTarget = Bitmap(Size(nNewWidth, nHeight), vcl::PixelFormat::N24_BPP);
        BitmapScopedWriteAccess pWriteAcc(rTarget);
        bool bResult(pWriteAcc);

        if(bResult)
        {
            for(sal_Int32 y(0); y < nHeight; y++)
            {
                Scanline pScanline = pWriteAcc->GetScanline( y );
                Scanline pScanlineRead = pReadAcc->GetScanline( y );
                for(sal_Int32 x(0); x < nNewWidth; x++)
                {
                    const sal_Int32 aBaseIndex(x * aNumberOfContributions);
                    sal_Int32 aSum(0);
                    sal_Int32 aValueRed(0);
                    sal_Int32 aValueGreen(0);
                    sal_Int32 aValueBlue(0);

                    for(sal_Int32 j(0); j < aCounts[x]; j++)
                    {
                        const sal_Int32 aIndex(aBaseIndex + j);
                        const sal_Int16 aWeight(aWeights[aIndex]);
                        BitmapColor aColor;

                        aSum += aWeight;

                        if(pReadAcc->HasPalette())
                        {
                            aColor = pReadAcc->GetPaletteColor(pReadAcc->GetIndexFromData(pScanlineRead, aPixels[aIndex]));
                        }
                        else
                        {
                            aColor = pReadAcc->GetPixelFromData(pScanlineRead, aPixels[aIndex]);
                        }

                        aValueRed += aWeight * aColor.GetRed();
                        aValueGreen += aWeight * aColor.GetGreen();
                        aValueBlue += aWeight * aColor.GetBlue();
                    }

                    assert(aSum != 0);

                    const BitmapColor aResultColor(
                        static_cast< sal_uInt8 >(std::clamp< sal_Int32 >(aValueRed / aSum, 0, 255)),
                        static_cast< sal_uInt8 >(std::clamp< sal_Int32 >(aValueGreen / aSum, 0, 255)),
                        static_cast< sal_uInt8 >(std::clamp< sal_Int32 >(aValueBlue / aSum, 0, 255)));

                    pWriteAcc->SetPixelOnData(pScanline, x, aResultColor);
                }
            }

            pWriteAcc.reset();
        }

        aWeights.clear();
        aCounts.clear();
        aPixels.clear();

        if(bResult)
        {
            return true;
        }
    }

    return false;
}

bool ImplScaleConvolutionVer(Bitmap& rSource, Bitmap& rTarget, const double& rScaleY, const Kernel& aKernel)
{
    // Do vertical filtering
    OSL_ENSURE(rScaleY > 0.0, "Error in scaling: Mirror given in non-mirror-capable method (!)");
    const sal_Int32 nHeight(rSource.GetSizePixel().Height());
    const sal_Int32 nNewHeight(FRound(nHeight * rScaleY));

    if(nHeight == nNewHeight)
    {
        return true;
    }

    BitmapScopedReadAccess pReadAcc(rSource);
    if(!pReadAcc)
        return false;

    std::vector<sal_Int16> aWeights;
    std::vector<sal_Int32> aPixels;
    std::vector<sal_Int32> aCounts;
    sal_Int32 aNumberOfContributions(0);

    const sal_Int32 nWidth(rSource.GetSizePixel().Width());
    ImplCalculateContributions(nHeight, nNewHeight, aNumberOfContributions, aWeights, aPixels, aCounts, aKernel);
    rTarget = Bitmap(Size(nWidth, nNewHeight), vcl::PixelFormat::N24_BPP);
    BitmapScopedWriteAccess pWriteAcc(rTarget);
    if(!pWriteAcc)
        return false;

    std::vector<BitmapColor> aScanline(nHeight);
    for(sal_Int32 x(0); x < nWidth; x++)
    {
        for(sal_Int32 y(0); y < nHeight; y++)
                if(pReadAcc->HasPalette())
                    aScanline[y] = pReadAcc->GetPaletteColor(pReadAcc->GetPixelIndex(y, x));
                else
                    aScanline[y] = pReadAcc->GetPixel(y, x);
        for(sal_Int32 y(0); y < nNewHeight; y++)
        {
            const sal_Int32 aBaseIndex(y * aNumberOfContributions);
            sal_Int32 aSum(0);
            sal_Int32 aValueRed(0);
            sal_Int32 aValueGreen(0);
            sal_Int32 aValueBlue(0);

            for(sal_Int32 j(0); j < aCounts[y]; j++)
            {
                const sal_Int32 aIndex(aBaseIndex + j);
                const sal_Int16 aWeight(aWeights[aIndex]);
                aSum += aWeight;
                const BitmapColor & aColor = aScanline[aPixels[aIndex]];
                aValueRed += aWeight * aColor.GetRed();
                aValueGreen += aWeight * aColor.GetGreen();
                aValueBlue += aWeight * aColor.GetBlue();
            }

            assert(aSum != 0);

            const BitmapColor aResultColor(
                static_cast< sal_uInt8 >(std::clamp< sal_Int32 >(aValueRed / aSum, 0, 255)),
                static_cast< sal_uInt8 >(std::clamp< sal_Int32 >(aValueGreen / aSum, 0, 255)),
                static_cast< sal_uInt8 >(std::clamp< sal_Int32 >(aValueBlue / aSum, 0, 255)));

            if(pWriteAcc->HasPalette())
            {
                pWriteAcc->SetPixelIndex(y, x, static_cast< sal_uInt8 >(pWriteAcc->GetBestPaletteIndex(aResultColor)));
            }
            else
            {
                pWriteAcc->SetPixel(y, x, aResultColor);
            }
        }
    }

    aWeights.clear();
    aCounts.clear();
    aPixels.clear();

    return true;
}

bool ImplScaleConvolution(Bitmap& rBitmap, const double& rScaleX, const double& rScaleY, const Kernel& aKernel)
{
    const bool bMirrorHor(rScaleX < 0.0);
    const bool bMirrorVer(rScaleY < 0.0);
    const double fScaleX(bMirrorHor ? -rScaleX : rScaleX);
    const double fScaleY(bMirrorVer ? -rScaleY : rScaleY);
    const sal_Int32 nWidth(rBitmap.GetSizePixel().Width());
    const sal_Int32 nHeight(rBitmap.GetSizePixel().Height());
    const sal_Int32 nNewWidth(FRound(nWidth * fScaleX));
    const sal_Int32 nNewHeight(FRound(nHeight * fScaleY));
    const bool bScaleHor(nWidth != nNewWidth);
    const bool bScaleVer(nHeight != nNewHeight);
    const bool bMirror(bMirrorHor || bMirrorVer);

    if (!bMirror && !bScaleHor && !bScaleVer)
    {
        return true;
    }

    bool bResult(true);
    BmpMirrorFlags nMirrorFlags(BmpMirrorFlags::NONE);
    bool bMirrorAfter(false);

    if (bMirror)
    {
        if(bMirrorHor)
        {
            nMirrorFlags |= BmpMirrorFlags::Horizontal;
        }

        if(bMirrorVer)
        {
            nMirrorFlags |= BmpMirrorFlags::Vertical;
        }

        const sal_Int32 nStartSize(nWidth * nHeight);
        const sal_Int32 nEndSize(nNewWidth * nNewHeight);

        bMirrorAfter = nStartSize > nEndSize;

        if(!bMirrorAfter)
        {
            bResult = rBitmap.Mirror(nMirrorFlags);
        }
    }

    Bitmap aResult;

    if (bResult)
    {
        const sal_Int32 nInBetweenSizeHorFirst(nHeight * nNewWidth);
        const sal_Int32 nInBetweenSizeVerFirst(nNewHeight * nWidth);
        Bitmap aSource(rBitmap);

        if(nInBetweenSizeHorFirst < nInBetweenSizeVerFirst)
        {
            if(bScaleHor)
            {
                bResult = ImplScaleConvolutionHor(aSource, aResult, fScaleX, aKernel);
            }

            if(bResult && bScaleVer)
            {
                if(bScaleHor)
                {
                    // copy partial result, independent of color depth
                    aSource = aResult;
                }

                bResult = ImplScaleConvolutionVer(aSource, aResult, fScaleY, aKernel);
            }
        }
        else
        {
            if(bScaleVer)
            {
                bResult = ImplScaleConvolutionVer(aSource, aResult, fScaleY, aKernel);
            }

            if(bResult && bScaleHor)
            {
                if(bScaleVer)
                {
                    // copy partial result, independent of color depth
                    aSource = aResult;
                }

                bResult = ImplScaleConvolutionHor(aSource, aResult, fScaleX, aKernel);
            }
        }
    }

    if(bResult && bMirrorAfter)
    {
        bResult = aResult.Mirror(nMirrorFlags);
    }

    if(bResult)
    {
        rBitmap.AdaptBitCount(aResult);
        rBitmap = aResult;
    }

    return bResult;
}

} // end anonymous namespace

BitmapEx BitmapScaleConvolutionFilter::execute(BitmapEx const& rBitmapEx) const
{
    bool bRetval = false;
    Bitmap aBitmap(rBitmapEx.GetBitmap());

    bRetval = ImplScaleConvolution(aBitmap, mrScaleX, mrScaleY, *mxKernel);

    if (bRetval)
        return BitmapEx(aBitmap);

    return BitmapEx();
}

}

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
