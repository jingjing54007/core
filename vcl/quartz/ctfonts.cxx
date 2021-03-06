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

#include <sal/config.h>

#include <basegfx/polygon/b2dpolygon.hxx>
#include <basegfx/matrix/b2dhommatrix.hxx>

#include <vcl/settings.hxx>


#include "quartz/ctfonts.hxx"
#include "impfont.hxx"
#ifdef MACOSX
#include "osx/saldata.hxx"
#include "osx/salinst.h"
#endif
#include "fontinstance.hxx"
#include "fontattributes.hxx"
#include "PhysicalFontCollection.hxx"
#include "quartz/salgdi.h"
#include "quartz/utils.h"
#include "sallayout.hxx"


inline double toRadian(int nDegree)
{
    return nDegree * (M_PI / 1800.0);
}

CoreTextStyle::CoreTextStyle( const FontSelectPattern& rFSD )
    : mpFontData( static_cast<CoreTextFontFace const *>(rFSD.mpFontData) )
    , mfFontStretch( 1.0 )
    , mfFontRotation( 0.0 )
    , maFontSelData( rFSD )
    , mpStyleDict( nullptr )
    , mpHbFont( nullptr )
{
    const FontSelectPattern* const pReqFont = &rFSD;

    double fScaledFontHeight = pReqFont->mfExactHeight;

    // convert font rotation to radian
    mfFontRotation = toRadian(pReqFont->mnOrientation);

    // dummy matrix so we can use CGAffineTransformConcat() below
    CGAffineTransform aMatrix = CGAffineTransformMakeTranslation(0, 0);

    // handle font stretching if any
    if( (pReqFont->mnWidth != 0) && (pReqFont->mnWidth != pReqFont->mnHeight) )
    {
        mfFontStretch = (float)pReqFont->mnWidth / pReqFont->mnHeight;
        aMatrix = CGAffineTransformConcat(aMatrix, CGAffineTransformMakeScale(mfFontStretch, 1.0F));
    }

    // create the style object for CoreText font attributes
    static const CFIndex nMaxDictSize = 16; // TODO: does this really suffice?
    mpStyleDict = CFDictionaryCreateMutable( nullptr, nMaxDictSize,
                                             &kCFTypeDictionaryKeyCallBacks,
                                             &kCFTypeDictionaryValueCallBacks );

    CFBooleanRef pCFVertBool = pReqFont->mbVertical ? kCFBooleanTrue : kCFBooleanFalse;
    CFDictionarySetValue( mpStyleDict, kCTVerticalFormsAttributeName, pCFVertBool );

    // fake bold
    if ( (pReqFont->GetWeight() >= WEIGHT_BOLD) &&
         ((mpFontData->GetWeight() < WEIGHT_SEMIBOLD) &&
          (mpFontData->GetWeight() != WEIGHT_DONTKNOW)) )
    {
        int nStroke = -lrint((3.5F * pReqFont->GetWeight()) / mpFontData->GetWeight());
        CFNumberRef rStroke = CFNumberCreate(nullptr, kCFNumberSInt32Type, &nStroke);
        CFDictionarySetValue(mpStyleDict, kCTStrokeWidthAttributeName, rStroke);
    }

    // fake italic
    if (((pReqFont->GetItalic() == ITALIC_NORMAL) ||
         (pReqFont->GetItalic() == ITALIC_OBLIQUE)) &&
        (mpFontData->GetItalic() == ITALIC_NONE))
    {
        aMatrix = CGAffineTransformConcat(aMatrix, CGAffineTransformMake(1, 0, toRadian(120), 1, 0, 0));
    }

    CTFontDescriptorRef pFontDesc = reinterpret_cast<CTFontDescriptorRef>(mpFontData->GetFontId());
    CTFontRef pNewCTFont = CTFontCreateWithFontDescriptor( pFontDesc, fScaledFontHeight, &aMatrix );
    CFDictionarySetValue( mpStyleDict, kCTFontAttributeName, pNewCTFont );
    CFRelease( pNewCTFont);
}

CoreTextStyle::~CoreTextStyle()
{
    if( mpStyleDict )
        CFRelease( mpStyleDict );
    if( mpHbFont )
        hb_font_destroy( mpHbFont );
}

void CoreTextStyle::GetFontMetric( ImplFontMetricDataRef& rxFontMetric ) const
{
    // get the matching CoreText font handle
    // TODO: is it worth it to cache the CTFontRef in SetFont() and reuse it here?
    CTFontRef aCTFontRef = static_cast<CTFontRef>(CFDictionaryGetValue( mpStyleDict, kCTFontAttributeName ));

    int nBufSize = 0;

    nBufSize = mpFontData->GetFontTable("hhea", nullptr);
    uint8_t* pHheaBuf = new uint8_t[nBufSize];
    nBufSize = mpFontData->GetFontTable("hhea", pHheaBuf);
    std::vector<uint8_t> rHhea(pHheaBuf, pHheaBuf + nBufSize);

    nBufSize = mpFontData->GetFontTable("OS/2", nullptr);
    uint8_t* pOS2Buf = new uint8_t[nBufSize];
    nBufSize = mpFontData->GetFontTable("OS/2", pOS2Buf);
    std::vector<uint8_t> rOS2(pOS2Buf, pOS2Buf + nBufSize);

    rxFontMetric->ImplCalcLineSpacing(rHhea, rOS2, CTFontGetUnitsPerEm(aCTFontRef));

    delete[] pHheaBuf;
    delete[] pOS2Buf;

    // since ImplFontMetricData::mnWidth is only used for stretching/squeezing fonts
    // setting this width to the pixel height of the fontsize is good enough
    // it also makes the calculation of the stretch factor simple
    rxFontMetric->SetWidth( lrint( CTFontGetSize( aCTFontRef ) * mfFontStretch) );

    UniChar nKashidaCh = 0x0640;
    CGGlyph nKashidaGid = 0;
    if (CTFontGetGlyphsForCharacters(aCTFontRef, &nKashidaCh, &nKashidaGid, 1))
    {
SAL_WNODEPRECATED_DECLARATIONS_PUSH
            // 'kCTFontHorizontalOrientation' is deprecated: first deprecated in
            // macOS 10.11
        double nKashidaAdv = CTFontGetAdvancesForGlyphs(aCTFontRef,
                kCTFontHorizontalOrientation, &nKashidaGid, nullptr, 1);
SAL_WNODEPRECATED_DECLARATIONS_POP
        rxFontMetric->SetMinKashida(lrint(nKashidaAdv));
    }
}

bool CoreTextStyle::GetGlyphBoundRect(const GlyphItem& rGlyph, tools::Rectangle& rRect ) const
{
    CGGlyph nCGGlyph = rGlyph.maGlyphId;
    CTFontRef aCTFontRef = static_cast<CTFontRef>(CFDictionaryGetValue( mpStyleDict, kCTFontAttributeName ));

    SAL_WNODEPRECATED_DECLARATIONS_PUSH //TODO: 10.11 kCTFontDefaultOrientation
    const CTFontOrientation aFontOrientation = kCTFontDefaultOrientation; // TODO: horz/vert
    SAL_WNODEPRECATED_DECLARATIONS_POP
    CGRect aCGRect = CTFontGetBoundingRectsForGlyphs(aCTFontRef, aFontOrientation, &nCGGlyph, nullptr, 1);

    // Apply font rotation to non-vertical glyphs.
    if (mfFontRotation && !rGlyph.IsVertical())
        aCGRect = CGRectApplyAffineTransform(aCGRect, CGAffineTransformMakeRotation(mfFontRotation));

    long xMin = floor(aCGRect.origin.x);
    long yMin = floor(aCGRect.origin.y);
    long xMax = ceil(aCGRect.origin.x + aCGRect.size.width);
    long yMax = ceil(aCGRect.origin.y + aCGRect.size.height);
    rRect = tools::Rectangle(xMin, -yMax, xMax, -yMin);
    return true;
}

// callbacks from CTFontCreatePathForGlyph+CGPathApply for GetGlyphOutline()
struct GgoData { basegfx::B2DPolygon maPolygon; basegfx::B2DPolyPolygon* mpPolyPoly; };

static void MyCGPathApplierFunc( void* pData, const CGPathElement* pElement )
{
    basegfx::B2DPolygon& rPolygon = static_cast<GgoData*>(pData)->maPolygon;
    const int nPointCount = rPolygon.count();

    switch( pElement->type )
    {
    case kCGPathElementCloseSubpath:
    case kCGPathElementMoveToPoint:
        if( nPointCount > 0 )
        {
            static_cast<GgoData*>(pData)->mpPolyPoly->append( rPolygon );
            rPolygon.clear();
        }
        // fall through for kCGPathElementMoveToPoint:
        if( pElement->type != kCGPathElementMoveToPoint )
        {
            break;
        }
        SAL_FALLTHROUGH;
    case kCGPathElementAddLineToPoint:
        rPolygon.append( basegfx::B2DPoint( +pElement->points[0].x, -pElement->points[0].y ) );
        break;

    case kCGPathElementAddCurveToPoint:
        rPolygon.append( basegfx::B2DPoint( +pElement->points[2].x, -pElement->points[2].y ) );
        rPolygon.setNextControlPoint( nPointCount - 1,
                                      basegfx::B2DPoint( pElement->points[0].x,
                                                         -pElement->points[0].y ) );
        rPolygon.setPrevControlPoint( nPointCount + 0,
                                      basegfx::B2DPoint( pElement->points[1].x,
                                                         -pElement->points[1].y ) );
        break;

    case kCGPathElementAddQuadCurveToPoint:
        {
            const basegfx::B2DPoint aStartPt = rPolygon.getB2DPoint( nPointCount-1 );
            const basegfx::B2DPoint aCtrPt1( (aStartPt.getX() + 2 * pElement->points[0].x) / 3.0,
                                             (aStartPt.getY() - 2 * pElement->points[0].y) / 3.0 );
            const basegfx::B2DPoint aCtrPt2( (+2 * pElement->points[0].x + pElement->points[1].x) / 3.0,
                                             (-2 * pElement->points[0].y - pElement->points[1].y) / 3.0 );
            rPolygon.append( basegfx::B2DPoint( +pElement->points[1].x, -pElement->points[1].y ) );
            rPolygon.setNextControlPoint( nPointCount-1, aCtrPt1 );
            rPolygon.setPrevControlPoint( nPointCount+0, aCtrPt2 );
        }
        break;
    }
}

bool CoreTextStyle::GetGlyphOutline(const GlyphItem& rGlyph, basegfx::B2DPolyPolygon& rResult) const
{
    rResult.clear();

    CGGlyph nCGGlyph = rGlyph.maGlyphId;
    CTFontRef pCTFont = static_cast<CTFontRef>(CFDictionaryGetValue( mpStyleDict, kCTFontAttributeName ));

    SAL_WNODEPRECATED_DECLARATIONS_PUSH
    const CTFontOrientation aFontOrientation = kCTFontDefaultOrientation;
    SAL_WNODEPRECATED_DECLARATIONS_POP
    CGRect aCGRect = CTFontGetBoundingRectsForGlyphs(pCTFont, aFontOrientation, &nCGGlyph, nullptr, 1);

    if (!CGRectIsNull(aCGRect) && CGRectIsEmpty(aCGRect))
    {
        // CTFontCreatePathForGlyph returns NULL for blank glyphs, but we want
        // to return true for them.
        return true;
    }

    CGPathRef xPath = CTFontCreatePathForGlyph( pCTFont, nCGGlyph, nullptr );
    if (!xPath)
    {
        return false;
    }

    GgoData aGgoData;
    aGgoData.mpPolyPoly = &rResult;
    CGPathApply( xPath, static_cast<void*>(&aGgoData), MyCGPathApplierFunc );
#if 0 // TODO: does OSX ensure that the last polygon is always closed?
    const CGPathElement aClosingElement = { kCGPathElementCloseSubpath, NULL };
    MyCGPathApplierFunc( (void*)&aGgoData, &aClosingElement );
#endif
    CFRelease( xPath );

    return true;
}

PhysicalFontFace* CoreTextFontFace::Clone() const
{
    return new CoreTextFontFace( *this);
}

LogicalFontInstance* CoreTextFontFace::CreateFontInstance( /*const*/ FontSelectPattern& rFSD ) const
{
    return new LogicalFontInstance( rFSD);
}

int CoreTextFontFace::GetFontTable( const char pTagName[5], unsigned char* pResultBuf ) const
{
    SAL_WARN_IF( pTagName[4]!='\0', "vcl", "CoreTextFontFace::GetFontTable with invalid tagname!" );

    const CTFontTableTag nTagCode = (pTagName[0]<<24) + (pTagName[1]<<16) + (pTagName[2]<<8) + (pTagName[3]<<0);

    // get the raw table length
    CTFontDescriptorRef pFontDesc = reinterpret_cast<CTFontDescriptorRef>( GetFontId());
    CTFontRef rCTFont = CTFontCreateWithFontDescriptor( pFontDesc, 0.0, nullptr);
    const uint32_t opts( kCTFontTableOptionNoOptions );
    CFDataRef pDataRef = CTFontCopyTable( rCTFont, nTagCode, opts);
    CFRelease( rCTFont);
    if( !pDataRef)
        return 0;

    const CFIndex nByteLength = CFDataGetLength( pDataRef);

    // get the raw table data if requested
    if( pResultBuf && (nByteLength > 0))
    {
        const CFRange aFullRange = CFRangeMake( 0, nByteLength);
        CFDataGetBytes( pDataRef, aFullRange, reinterpret_cast<UInt8*>(pResultBuf));
    }

    CFRelease( pDataRef);

    return (int)nByteLength;
}

FontAttributes DevFontFromCTFontDescriptor( CTFontDescriptorRef pFD, bool* bFontEnabled )
{
    // all CoreText fonts are device fonts that can rotate just fine
    FontAttributes rDFA;
    rDFA.SetQuality( 0 );

    // reset the font attributes
    rDFA.SetFamilyType( FAMILY_DONTKNOW );
    rDFA.SetPitch( PITCH_VARIABLE );
    rDFA.SetWidthType( WIDTH_NORMAL );
    rDFA.SetWeight( WEIGHT_NORMAL );
    rDFA.SetItalic( ITALIC_NONE );
    rDFA.SetSymbolFlag( false );

    // get font name
#ifdef MACOSX
    const OUString aUILang = Application::GetSettings().GetUILanguageTag().getLanguage();
    CFStringRef pUILang = CFStringCreateWithCharacters( kCFAllocatorDefault,
                                                        reinterpret_cast<UniChar const *>(aUILang.getStr()), aUILang.getLength() );
    CFStringRef pLang = nullptr;
    CFStringRef pFamilyName = static_cast<CFStringRef>(
            CTFontDescriptorCopyLocalizedAttribute( pFD, kCTFontFamilyNameAttribute, &pLang ));

    if ( !pLang || ( CFStringCompare( pUILang, pLang, 0 ) != kCFCompareEqualTo ))
    {
        if(pFamilyName)
        {
            CFRelease( pFamilyName );
        }
        pFamilyName = static_cast<CFStringRef>(CTFontDescriptorCopyAttribute( pFD, kCTFontFamilyNameAttribute ));
    }
#else
    // No "Application" on iOS. And it is unclear whether this code
    // snippet will actually ever get invoked on iOS anyway. So just
    // use the old code that uses a non-localized font name.
    CFStringRef pFamilyName = (CFStringRef)CTFontDescriptorCopyAttribute( pFD, kCTFontFamilyNameAttribute );
#endif

    rDFA.SetFamilyName( GetOUString( pFamilyName ) );

    // get font style
    CFStringRef pStyleName = static_cast<CFStringRef>(CTFontDescriptorCopyAttribute( pFD, kCTFontStyleNameAttribute ));
    rDFA.SetStyleName( GetOUString( pStyleName ) );

    // get font-enabled status
    if( bFontEnabled )
    {
        int bEnabled = TRUE; // by default (and when we're on OS X < 10.6) it's "enabled"
        CFNumberRef pEnabled = static_cast<CFNumberRef>(CTFontDescriptorCopyAttribute( pFD, kCTFontEnabledAttribute ));
        CFNumberGetValue( pEnabled, kCFNumberIntType, &bEnabled );
        *bFontEnabled = bEnabled;
    }

    // get font attributes
    CFDictionaryRef pAttrDict = static_cast<CFDictionaryRef>(CTFontDescriptorCopyAttribute( pFD, kCTFontTraitsAttribute ));

    if (bFontEnabled && *bFontEnabled)
    {
        // Ignore font formats not supported.
        int nFormat;
        CFNumberRef pFormat = static_cast<CFNumberRef>(CTFontDescriptorCopyAttribute(pFD, kCTFontFormatAttribute));
        CFNumberGetValue(pFormat, kCFNumberIntType, &nFormat);
        if (nFormat == kCTFontFormatUnrecognized || nFormat == kCTFontFormatPostScript || nFormat == kCTFontFormatBitmap)
        {
            SAL_INFO("vcl.fonts", "Ignoring font with unsupported format: " << rDFA.GetFamilyName());
            *bFontEnabled = false;
        }
        CFRelease(pFormat);
    }

    // get symbolic trait
    // TODO: use other traits such as MonoSpace/Condensed/Expanded or Vertical too
    SInt64 nSymbolTrait = 0;
    CFNumberRef pSymbolNum = nullptr;
    if( CFDictionaryGetValueIfPresent( pAttrDict, kCTFontSymbolicTrait, reinterpret_cast<const void**>(&pSymbolNum) ) )
    {
        CFNumberGetValue( pSymbolNum, kCFNumberSInt64Type, &nSymbolTrait );
        rDFA.SetSymbolFlag( (nSymbolTrait & kCTFontClassMaskTrait) == kCTFontSymbolicClass );
    }

    // get the font weight
    double fWeight = 0;
    CFNumberRef pWeightNum = static_cast<CFNumberRef>(CFDictionaryGetValue( pAttrDict, kCTFontWeightTrait ));
    CFNumberGetValue( pWeightNum, kCFNumberDoubleType, &fWeight );
    int nInt = WEIGHT_NORMAL;
    if( fWeight > 0 )
    {
        nInt = rint(WEIGHT_NORMAL + fWeight * ((WEIGHT_BLACK - WEIGHT_NORMAL)/0.68));
        if( nInt > WEIGHT_BLACK )
        {
            nInt = WEIGHT_BLACK;
        }
    }
    else if( fWeight < 0 )
    {
        nInt = rint(WEIGHT_NORMAL + fWeight * ((WEIGHT_NORMAL - WEIGHT_THIN)/0.9));
        if( nInt < WEIGHT_THIN )
        {
            nInt = WEIGHT_THIN;
        }
    }
    rDFA.SetWeight( (FontWeight)nInt );

    // get the font slant
    double fSlant = 0;
    CFNumberRef pSlantNum = static_cast<CFNumberRef>(CFDictionaryGetValue( pAttrDict, kCTFontSlantTrait ));
    CFNumberGetValue( pSlantNum, kCFNumberDoubleType, &fSlant );
    if( fSlant >= 0.035 )
    {
        rDFA.SetItalic( ITALIC_NORMAL );
    }
    // get width trait
    double fWidth = 0;
    CFNumberRef pWidthNum = static_cast<CFNumberRef>(CFDictionaryGetValue( pAttrDict, kCTFontWidthTrait ));
    CFNumberGetValue( pWidthNum, kCFNumberDoubleType, &fWidth );
    nInt = WIDTH_NORMAL;

    if( fWidth > 0 )
    {
        nInt = rint( WIDTH_NORMAL + fWidth * ((WIDTH_ULTRA_EXPANDED - WIDTH_NORMAL)/0.4));
        if( nInt > WIDTH_ULTRA_EXPANDED )
        {
            nInt = WIDTH_ULTRA_EXPANDED;
        }
    }
    else if( fWidth < 0 )
    {
        nInt = rint( WIDTH_NORMAL + fWidth * ((WIDTH_NORMAL - WIDTH_ULTRA_CONDENSED)/0.5));
        if( nInt < WIDTH_ULTRA_CONDENSED )
        {
            nInt = WIDTH_ULTRA_CONDENSED;
        }
    }
    rDFA.SetWidthType( (FontWidth)nInt );

    // release the attribute dict that we had copied
    CFRelease( pAttrDict );

    // TODO? also use the HEAD table if available to get more attributes
//  CFDataRef CTFontCopyTable( CTFontRef, kCTFontTableHead, /*kCTFontTableOptionNoOptions*/kCTFontTableOptionExcludeSynthetic );

    return rDFA;
}

static void CTFontEnumCallBack( const void* pValue, void* pContext )
{
    CTFontDescriptorRef pFD = static_cast<CTFontDescriptorRef>(pValue);

    bool bFontEnabled;
    FontAttributes rDFA = DevFontFromCTFontDescriptor( pFD, &bFontEnabled );

    if( bFontEnabled)
    {
        const sal_IntPtr nFontId = reinterpret_cast<sal_IntPtr>(pValue);
        CoreTextFontFace* pFontData = new CoreTextFontFace( rDFA, nFontId );
        SystemFontList* pFontList = static_cast<SystemFontList*>(pContext);
        pFontList->AddFont( pFontData );
    }
}

SystemFontList::SystemFontList()
  : mpCTFontCollection( nullptr )
  , mpCTFontArray( nullptr )
{}

SystemFontList::~SystemFontList()
{
    CTFontContainer::const_iterator it = maFontContainer.begin();
    for(; it != maFontContainer.end(); ++it )
    {
        delete (*it).second;
    }
    maFontContainer.clear();

    if( mpCTFontArray )
    {
        CFRelease( mpCTFontArray );
    }
    if( mpCTFontCollection )
    {
        CFRelease( mpCTFontCollection );
    }
}

void SystemFontList::AddFont( CoreTextFontFace* pFontData )
{
    sal_IntPtr nFontId = pFontData->GetFontId();
    maFontContainer[ nFontId ] = pFontData;
}

void SystemFontList::AnnounceFonts( PhysicalFontCollection& rFontCollection ) const
{
    CTFontContainer::const_iterator it = maFontContainer.begin();
    for(; it != maFontContainer.end(); ++it )
    {
        rFontCollection.Add( (*it).second->Clone() );
    }
}

CoreTextFontFace* SystemFontList::GetFontDataFromId( sal_IntPtr nFontId ) const
{
    CTFontContainer::const_iterator it = maFontContainer.find( nFontId );
    if( it == maFontContainer.end() )
    {
        return nullptr;
    }
    return (*it).second;
}

bool SystemFontList::Init()
{
    // enumerate available system fonts
    static const int nMaxDictEntries = 8;
    CFMutableDictionaryRef pCFDict = CFDictionaryCreateMutable( nullptr,
                                                                nMaxDictEntries,
                                                                &kCFTypeDictionaryKeyCallBacks,
                                                                &kCFTypeDictionaryValueCallBacks );

    CFDictionaryAddValue( pCFDict, kCTFontCollectionRemoveDuplicatesOption, kCFBooleanTrue );
    mpCTFontCollection = CTFontCollectionCreateFromAvailableFonts( pCFDict );
    CFRelease( pCFDict );
    mpCTFontArray = CTFontCollectionCreateMatchingFontDescriptors( mpCTFontCollection );

    const int nFontCount = CFArrayGetCount( mpCTFontArray );
    const CFRange aFullRange = CFRangeMake( 0, nFontCount );
    CFArrayApplyFunction( mpCTFontArray, aFullRange, CTFontEnumCallBack, this );

    return true;
}

SystemFontList* GetCoretextFontList()
{
    SystemFontList* pList = new SystemFontList();
    if( !pList->Init() )
    {
        delete pList;
        return nullptr;
    }

    return pList;
}

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
